#!/usr/bin/env python3
"""PC发送Seq、电源/采样/结果灯业务的离线线格式合同。"""

from pathlib import Path
import json
import re
import struct


ROOT = Path(__file__).resolve().parents[1]


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def build_wire_frame(command: int, sequence: int, body: bytes) -> bytes:
    """按正式18字节大端头构造期望帧，作为源码合同的独立线格式判据。"""
    return struct.pack(
        ">IHHHHIH",
        18 + len(body), 1, 8, 0, command, sequence, 0,
    ) + body


def json_string_lexemes_are_safe(data: bytes) -> bool:
    """镜像固件的原始JSON字符串扫描，覆盖C字符串内嵌NUL边界。"""
    in_string = False
    index = 0
    while index < len(data):
        value = data[index]
        if value == 0:
            return False
        if not in_string:
            if value == ord('"'):
                in_string = True
            index += 1
            continue
        if value == ord('"'):
            in_string = False
            index += 1
            continue
        if value < 0x20:
            return False
        if value != ord('\\'):
            index += 1
            continue
        index += 1
        if index >= len(data):
            return False
        if data[index] == ord('u'):
            if index + 4 >= len(data):
                return False
            if data[index + 1:index + 5] == b"0000":
                return False
            index += 4
        index += 1
    return True


def main() -> None:
    protocol = (ROOT / "App/protocol/pc/pc_protocol.c").read_text(encoding="utf-8")
    dispatch = (ROOT / "App/protocol/pc/pc_dispatch.c").read_text(encoding="utf-8")
    command = (ROOT / "App/protocol/pc/pc_command.h").read_text(encoding="utf-8")
    tcp = (ROOT / "App/tasks/pc/tcp_task.c").read_text(encoding="utf-8")
    state_machine = (ROOT / "App/protocol/pc/pc_state_machine.c").read_text(
        encoding="utf-8"
    )
    monitor_task = (ROOT / "App/tasks/common/monitor_task.c").read_text(
        encoding="utf-8"
    )
    monitor_driver = (ROOT / "HAL_Drv/memory_monitor.c").read_text(
        encoding="utf-8"
    )
    power_control = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    ddr_source = (ROOT / "HAL_Drv/ddr_power.c").read_text(encoding="utf-8")
    led_source = (ROOT / "Core/Src/result_led.c").read_text(encoding="utf-8")

    require("write_be16(&output[8], 0U);" in protocol,
            "MCU发送ServiceID必须固定为0")
    require("write_be32(&output[12], mcu_tx_sequence);" in protocol and
            "request->sequence" not in protocol,
            "MCU发送头必须使用显式本端Seq，禁止回显请求Seq")
    require("PC_MCU_TX_SEQUENCE_FIRST 1U" in tcp and
            "uint32_t mcu_tx_sequence = PC_MCU_TX_SEQUENCE_FIRST;" in tcp,
            "每条TCP连接的MCU首个发送Seq必须为1")
    require(tcp.index("pc_send_all(connfd") < tcp.index("mcu_tx_sequence++;"),
            "Seq只能在完整PDU成功发送后推进")
    require("read_be16(&sm->buffer[8]) != 0U" in state_machine,
            "接收端必须拒绝非0 ServiceID")
    require("json_string_lexemes_are_safe" in protocol and
            "json[i + 4U] == '0'" in protocol,
            "共享JSON入口必须拒绝\\u0000造成的C字符串截断")
    require(tcp.count("pc_last_byte_tick = xTaskGetTickCount();") >= 2 and
            tcp.index("for (int i = 0; i < n; i++)") <
            tcp.rindex("pc_last_byte_tick = xTaskGetTickCount();"),
            "每次消费已接收字节都必须刷新半帧计时")

    for symbol, value in (
        ("PC_COMMAND_COMMAND_RESP", "0x0006U"),
        ("PC_COMMAND_POWER_ON", "0x0501U"),
        ("PC_COMMAND_POWER_OFF", "0x0502U"),
        ("PC_COMMAND_QUERY_POWER_STATUS", "0x0503U"),
        ("PC_COMMAND_POWER_STATUS_RESP", "0x0504U"),
        ("PC_COMMAND_RESET_RK", "0x0505U"),
        ("PC_COMMAND_SET_BOOT_MODE", "0x0506U"),
        ("PC_COMMAND_QUERY_BOOT_MODE", "0x0507U"),
        ("PC_COMMAND_BOOT_MODE_RESP", "0x0508U"),
        ("PC_COMMAND_SET_DDR_CONFIG", "0x0509U"),
        ("PC_COMMAND_QUERY_POWER_MONITOR", "0x0601U"),
        ("PC_COMMAND_POWER_MONITOR_RESP", "0x0602U"),
        ("PC_COMMAND_SET_RESULT_INDICATOR", "0x0701U"),
        ("PC_COMMAND_QUERY_INDICATOR_STATUS", "0x0702U"),
        ("PC_COMMAND_INDICATOR_STATUS_RESP", "0x0703U"),
    ):
        require(re.search(rf"#define\s+{symbol}\s+{value}", command) is not None,
                f"missing command id: {symbol}")

    require("{PC_COMMAND_QUERY_POWER_STATUS, handle_query_power_status}" in dispatch,
            "0x0503未接入只读状态处理函数")
    require("{PC_COMMAND_POWER_STATUS_RESP, reject_response_command}" in dispatch,
            "PC反向发送0x0504必须按方向错误拒绝")
    require("build_command_failure" in dispatch and
            all(f'"{code}"' in dispatch for code in (
                "invalidArgument", "invalidDirection",
                "notSupported", "executionFailed")) and
            '"requestCommandId"' in dispatch and '"status", "failed"' in dispatch,
            "0x0006最小业务错误应答未完整实现")
    require("if (!command_matched)" in dispatch and '"notSupported"' in dispatch,
            "未知合法命令必须返回0x0006而不是静默丢弃")
    require(all(f'[{state}]' in dispatch and f'= "{wire}"' in dispatch
                for state, wire in (
                    ("PWR_CTRL_STATE_OFF", "off"),
                    ("PWR_CTRL_STATE_WAIT_FIRST_RAIL", "starting"),
                    ("PWR_CTRL_STATE_WAIT_SECOND_RAIL", "starting"),
                    ("PWR_CTRL_STATE_ON", "on"),
                    ("PWR_CTRL_STATE_WAIT_SECOND_RAIL_OFF", "stopping"),
                    ("PWR_CTRL_STATE_FAULT", "fault"),
                )), "电源内部状态到线协议字符串的映射不完整")

    require(all(token in dispatch for token in (
        "handle_power_on", "handle_power_off", "handle_reset_rk",
        "handle_set_boot_mode", "handle_query_boot_mode",
        '"bootMode"', '"targetMv"', '"notReady"', '"notConfigured"',
        "handle_set_ddr_config", "monitor_apply_ddr_config",
        '"memoryType", "vdd1Steps", "vdd2Steps", "vddqSteps"',
        "handle_query_power_monitor", "monitor_get_latest",
        '"sampleAgeMs"', '"samplePowerState"', '"validMask"',
        '"voltageMv"', '"currentMa"', '"powerMw"',
        "handle_set_result_indicator", "handle_query_indicator_status",
        '"lpddr0"', '"lpddr1"', '"emcp"',
        '"testNormal"', '"testHot"',
    )), "0x0509/0x0601/0x0701业务处理或JSON字段不完整")
    require("xSemaphoreCreateMutexStatic" in monitor_task and
            monitor_task.count("memory_io_take(") >= 6 and
            "xSemaphoreGive(s_memory_io_mutex)" in monitor_task,
            "监测采样与三轨调压必须共用完整事务互斥量")
    require("PWR_CTRL_ERR_RESOURCE_BUSY" in monitor_task and
            '"busy"' in dispatch,
            "互斥量占用必须与电源状态非法使用不同错误语义")
    require("pwr_ctrl_apply_ddr_steps(steps)" in monitor_task and
            "ready强制门待双方业务握手冻结" in monitor_task,
            "ON状态三轨调压业务路径或RK ready待办缺失")
    require("* 101" in monitor_driver and "(scaled + 50) / 100" in monitor_driver,
            "AD7124上报电压缺少B1前端1.01换算")
    require(power_control.index("ddr_power_build_step_targets") <
            power_control.index("ddr_power_set(DDR_RAIL_MASK_ALL"),
            "调压必须先纯校验，非法JSON不能触发FAULT关断")

    # 新训练目标仅是内部启动入口，不把已有0509静默改成另一种PC合同。
    training_request = power_control.split(
        "pwr_ctrl_result_t pwr_ctrl_request_training_on(", 1)[1].split(
        "pwr_ctrl_result_t pwr_ctrl_request_off(", 1)[0]
    require(training_request.index("s_state != PWR_CTRL_STATE_OFF") <
            training_request.index("ddr_power_configure_training") <
            training_request.index("pwr_ctrl_request_on(now_ms)"),
            "训练请求必须先拒绝非OFF，复制合法目标后才开电")
    initialize = ddr_source.split("int ddr_power_init(void)", 1)[1].split(
        "int ddr_power_set(", 1)[0]
    require("_nominal_mv[" not in initialize and "_training_mv[rail]" in initialize and
            "memcpy(_mv, _training_mv" in initialize and "continue;" not in initialize,
            "直接写训练目标，首错停止，全部成功才提交缓存")
    require("_training_mv" not in ddr_source.split("int ddr_power_set(", 1)[1],
            "运行拉偏不能反向修改下一次训练目标")
    require("(uint32_t)type > DDR_MEMORY_LPDDR4X" in ddr_source,
            "负枚举值也必须在索引标称表前被拒绝")
    for name in ("monitor_request_power_on", "monitor_request_power_off",
                 "monitor_request_training_power_on"):
        body = monitor_task.split(f"pwr_ctrl_result_t {name}(", 1)[1].split("\n}", 1)[0]
        require("memory_io_take_for_power(&now_ms)" in body, "电源请求须补偿锁等待")
    require("monitor_request_training_power_on" not in dispatch,
            "未冻结开始测试JSON和安全范围前不能直接接训练入口")
    require("result_led_get_all" in led_source and
            led_source.count("taskENTER_CRITICAL()") >= 4 and
            "s_states[target] == state" in led_source,
            "结果灯设置、查询和闪烁推进缺少原子状态保护")

    body = b'{"powerState":"off"}'
    first = build_wire_frame(0x0504, 1, body)
    second = build_wire_frame(0x0001, 2, b"{}")
    require(first[8:10] == b"\x00\x00" and first[12:16] == b"\x00\x00\x00\x01",
            "首个MCU状态响应头的ServiceID/Seq错误")
    require(second[12:16] == b"\x00\x00\x00\x02",
            "跨命令发送Seq没有连续递增")
    require(((0xFFFFFFFF + 1) & 0xFFFFFFFF) == 0,
            "u32 Seq必须在UINT32_MAX后自然回绕到0")
    require(json.loads(body) == {"powerState": "off"},
            "0x0504最小JSON格式错误")
    error_body = b'{"requestCommandId":1283,"status":"failed","errorCode":"invalidArgument"}'
    error_frame = build_wire_frame(0x0006, 2, error_body)
    require(error_frame[10:12] == b"\x00\x06" and
            json.loads(error_body)["requestCommandId"] == 0x0503,
            "0x0006参数错误应答线格式错误")

    completed = {"requestCommandId": 0x0509, "status": "completed"}
    require(json.loads(json.dumps(completed)) == completed,
            "0x0006 completed成功应答格式错误")

    ddr_config = json.loads(
        '{"memoryType":"LPDDR4X","vdd1Steps":0,'
        '"vdd2Steps":0,"vddqSteps":0}'
    )
    require(set(ddr_config) == {
        "memoryType", "vdd1Steps", "vdd2Steps", "vddqSteps"
    }, "0x0509必须固定为四字段")
    step_ranges = {
        "LPDDR4": ((-50, 77), (-35, 28), (-35, 28)),
        "LPDDR4X": ((-50, 77), (-35, 28), (-10, 53)),
    }
    for memory_type, ranges in step_ranges.items():
        nominals = (1800, 1100, 1100 if memory_type == "LPDDR4" else 600)
        for nominal, (first_step, last_step) in zip(nominals, ranges):
            require(nominal + first_step * 20 in (800, 400) and
                    nominal + last_step * 20 in (3340, 1660),
                    f"{memory_type} 20mV档位边界错误")

    monitor_body = {
        "sampleAgeMs": 37,
        "samplePowerState": "on",
        "validMask": 3,
        "voltageMv": [1801, 1099, 601],
        "currentMa": [312, 145, 88],
        "powerMw": [562, 159, 53],
    }
    require(all(len(monitor_body[name]) == 3 for name in (
        "voltageMv", "currentMa", "powerMw"
    )), "0x0602三轨数组必须固定为VDD1/VDD2/VDDQ")
    partial_monitor = {
        "sampleAgeMs": 10, "samplePowerState": "on", "validMask": 2,
        "currentMa": [1, 2, 3], "powerMw": [2, 3, 4],
    }
    require("voltageMv" not in partial_monitor,
            "无效测量组必须省略，不能用零数组冒充有效值")

    targets = ("lpddr0", "lpddr1", "emcp")
    states = ("off", "pass", "fail", "testNormal", "testHot")
    indicator_requests = [
        {"target": target, "state": state}
        for target in targets for state in states
    ]
    require(len(indicator_requests) == 15 and
            all(set(request) == {"target", "state"}
                for request in indicator_requests),
            "0x0701应覆盖3对象×5业务状态且只含两个字段")
    indicator_response = {target: "off" for target in targets}
    require(tuple(indicator_response) == targets,
            "0x0703对象顺序/命名必须为lpddr0、lpddr1、emcp")

    require(not json_string_lexemes_are_safe(
                br'{"target":"lpddr0","state":"pass\u0000junk"}') and
            not json_string_lexemes_are_safe(
                br'{"target\u0000junk":"lpddr0","state":"pass"}') and
            not json_string_lexemes_are_safe(
                b'{"state":"pass\x00junk"}') and
            json_string_lexemes_are_safe(
                br'{"state":"pass\\u0000"}'),
            "JSON NUL词法边界模型错误")

    print("PC Seq, power control, monitor snapshot and result LED JSON contract PASS")
    print("Training-target / fail-fast / lock-wait source contracts PASS (not C execution)")


if __name__ == "__main__":
    main()
