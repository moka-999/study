#!/usr/bin/env python3
"""离线验证 LPDDR4/4X 三轨、20mV步进、IDD 采样和上下电软件合同。"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
NOMINAL_MV = {
    "LPDDR4": (1800, 1100, 1100),
    "LPDDR4X": (1800, 1100, 600),
}
STEP_MV = 20
WRITABLE_MV = ((800, 3350), (400, 1675), (400, 1675))


def step_range(memory: str, rail: int) -> range:
    """返回以该轨标称值为零点、又不超出TPS可写范围的全部20mV步数。"""

    nominal = NOMINAL_MV[memory][rail]
    minimum, maximum = WRITABLE_MV[rail]
    first = (minimum - nominal + STEP_MV - 1) // STEP_MV
    last = (maximum - nominal) // STEP_MV
    return range(first, last + 1)


def build_steps(memory: str, steps: tuple[int, int, int]) -> tuple[int, int, int]:
    target = tuple(mv + step * STEP_MV for mv, step in zip(NOMINAL_MV[memory], steps))
    for rail, mv in enumerate(target):
        assert WRITABLE_MV[rail][0] <= mv <= WRITABLE_MV[rail][1]
    return target


def tps_code(mv: int, factor: int) -> int:
    """复核 TPS62868/69 VOUT1: 实际电压 = factor * (400mV + code * 5mV)。"""

    assert mv % factor == 0
    base_mv = mv // factor
    assert 400 <= base_mv <= 1675 and (base_mv - 400) % 5 == 0
    return (base_mv - 400) // 5


def verify_all_20mv_steps_and_tps_encoding() -> None:
    for memory in NOMINAL_MV:
        for rail in range(3):
            for step in step_range(memory, rail):
                target = NOMINAL_MV[memory][rail] + step * STEP_MV
                factor = (2, 1, 1)[rail]
                assert 0 <= tps_code(target, factor) <= 0xFF

    assert (step_range("LPDDR4X", 0).start, step_range("LPDDR4X", 0).stop - 1) == (-50, 77)
    assert (step_range("LPDDR4X", 1).start, step_range("LPDDR4X", 1).stop - 1) == (-35, 28)
    assert (step_range("LPDDR4X", 2).start, step_range("LPDDR4X", 2).stop - 1) == (-10, 53)
    assert (step_range("LPDDR4", 2).start, step_range("LPDDR4", 2).stop - 1) == (-35, 28)
    assert build_steps("LPDDR4X", (2, -3, 5)) == (1840, 1040, 700)
    assert tps_code(800, 2) == 0x00 and tps_code(3340, 2) == 0xFE
    assert tps_code(400, 1) == 0x00 and tps_code(1660, 1) == 0xFC
    assert tuple(tps_code(mv, factor) for mv, factor in
                 zip(NOMINAL_MV["LPDDR4X"], (2, 1, 1))) == (0x64, 0x8C, 0x28)
    assert tps_code(1100, 1) == 0x8C  # LPDDR4 VDDQ


def verify_pac_idd_conversion() -> None:
    """0.01ohm 采样电阻下，复核 PAC1934 VSENSE 到 mA 的整数换算。"""

    rsense_uohm = 10_000
    current_ma = lambda raw: raw * 100_000_000 // 65_536 // rsense_uohm
    assert tuple(current_ma(raw) for raw in (0x0800, 0x1000, 0x1800)) == (312, 625, 937)


def verify_source_contract() -> None:
    power = (ROOT / "HAL_Drv/ddr_power.c").read_text(encoding="utf-8")
    state = (ROOT / "HAL_Drv/pwr_ctrl.c").read_text(encoding="utf-8")
    pac = (ROOT / "BSP/PAC1934/pac1934.c").read_text(encoding="utf-8")
    monitor = (ROOT / "HAL_Drv/memory_monitor.c").read_text(encoding="utf-8")
    monitor_task = (ROOT / "App/tasks/common/monitor_task.c").read_text(encoding="utf-8")
    mock = (ROOT / "App/tasks/test/driver_mock_test.c").read_text(encoding="utf-8")

    assert "memcpy(_mv, nominal" in power, "每次上电必须恢复器件标称三轨"
    assert "steps[rail] * DDR_VOLTAGE_STEP_MV" in power, "拉偏必须以标称值为基准20mV步进"
    assert "JESD建议值或三路相对电压拦截" in power, "极限测试不应被工作区限制"
    assert power.index("candidate[DDR_RAIL_VDD1] >") < power.index("candidate[DDR_RAIL_VDD2] !=")
    assert power.index("candidate[DDR_RAIL_VDDQ] <") < power.index("candidate[DDR_RAIL_VDD2] !=")
    assert power.index("candidate[DDR_RAIL_VDD2] !=") < power.index("candidate[DDR_RAIL_VDDQ] >")
    assert power.index("candidate[DDR_RAIL_VDD2] !=") < power.index("candidate[DDR_RAIL_VDD1] <")
    assert "s_state != PWR_CTRL_STATE_OFF" in state, "器件类型只能在 OFF 状态选择"
    assert "s_state != PWR_CTRL_STATE_ON" in state, "拉偏只能在稳定 ON 状态执行"
    assert "enter_fault();" in state, "拉偏失败必须收敛到全关故障态"
    assert "pac1934_measure_all" in pac, "PAC1934 缺少三轨工程量采样接口"
    assert "memory_monitor_sample" in monitor and "valid_mask" in monitor, \
        "TPS/PAC/AD尚未组合为可部分有效的正式快照"
    assert "snapshot.power_state == PWR_CTRL_STATE_ON" in monitor_task, \
        "监测任务不得在DDR电源未稳定时访问采样器件"
    assert "memory_monitor_invalidate" in monitor_task, \
        "下电后必须清除PAC/AD就绪态"
    assert "test_lpddr_power_idd_bias_flow" in mock, "ARM Mock 镜像未覆盖完整流程"
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "if(ENABLE_DRIVER_MOCK_TEST)" in cmake, "正式固件不应编译Mock任务体"
    assert mock.index("GPIO_FUNC_PWR_4V0_EN") < mock.index("GPIO_FUNC_PWR_3V3_EN")
    # 校验下电调用和DDR使能检查，不依赖会随注释规范化变化的日志原句。
    # 这里只证明Mock包含该检查，不代表RK安全下电握手已经实现或验证。
    assert re.search(
        r"mock_check\(pwr_ctrl_request_off\(30U\)\s*==\s*PWR_CTRL_OK\s*&&\s*"
        r"bsp_gpio_get\(GPIO_FUNC_PWR_DDR_EN\)\s*==\s*0U", mock)


def main() -> None:
    verify_all_20mv_steps_and_tps_encoding()
    verify_pac_idd_conversion()
    verify_source_contract()
    print("LPDDR4/4X: all 20mV voltage steps, TPS encoding, PAC IDD and power-flow contracts PASS")
    print("Boundary: RK creates JEDEC IDD/CKE conditions; MCU sequences rails and samples them")


if __name__ == "__main__":
    main()
