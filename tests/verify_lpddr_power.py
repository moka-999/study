#!/usr/bin/env python3
"""离线验证 LPDDR4/4X 三轨、拉偏、IDD 采样和上下电软件合同。"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NOMINAL_MV = {
    "LPDDR4": (1800, 1100, 1100),
    "LPDDR4X": (1800, 1100, 600),
}
BIAS_PERCENT = (100, 105, 95, 90)


def build_profile(memory: str, percent: int) -> tuple[int, int, int]:
    """按固件固定档位生成三轨目标；所有目标必须能被 5mV 步进精确表示。"""

    target = tuple(mv * percent // 100 for mv in NOMINAL_MV[memory])
    assert all(mv * percent % 100 == 0 for mv in NOMINAL_MV[memory])
    assert all(mv % 5 == 0 for mv in target)
    vdd1, vdd2, vddq = target
    assert vdd1 <= 2100 and vdd2 <= 1400 and vddq <= 1400
    assert vdd1 >= vdd2 and vdd2 + 200 >= vddq
    return target


def tps_code(mv: int, factor: int) -> int:
    """复核 TPS62868/69 VOUT1: 实际电压 = factor * (400mV + code * 5mV)。"""

    assert mv % factor == 0
    base_mv = mv // factor
    assert 400 <= base_mv <= 1675 and (base_mv - 400) % 5 == 0
    return (base_mv - 400) // 5


def verify_profiles_and_tps_encoding() -> None:
    for memory in NOMINAL_MV:
        for percent in BIAS_PERCENT:
            build_profile(memory, percent)

    assert build_profile("LPDDR4X", 105) == (1890, 1155, 630)
    assert build_profile("LPDDR4", 90) == (1620, 990, 990)
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
    mock = (ROOT / "App/tasks/driver_mock_test.c").read_text(encoding="utf-8")

    assert "_bias_profile = DDR_BIAS_NOMINAL" in power, "每次上电必须清除上次拉偏"
    assert "memcpy(_mv, nominal" in power, "每次上电必须恢复器件标称三轨"
    assert power.index("candidate[DDR_RAIL_VDD1] >") < power.index("candidate[DDR_RAIL_VDD2] !=")
    assert power.index("candidate[DDR_RAIL_VDDQ] <") < power.index("candidate[DDR_RAIL_VDD2] !=")
    assert power.index("candidate[DDR_RAIL_VDD2] !=") < power.index("candidate[DDR_RAIL_VDDQ] >")
    assert power.index("candidate[DDR_RAIL_VDD2] !=") < power.index("candidate[DDR_RAIL_VDD1] <")
    assert "s_state != PWR_CTRL_STATE_OFF" in state, "器件类型只能在 OFF 状态选择"
    assert "s_state != PWR_CTRL_STATE_ON" in state, "拉偏只能在稳定 ON 状态执行"
    assert "enter_fault();" in state, "拉偏失败必须收敛到全关故障态"
    assert "pac1934_measure_all" in pac, "PAC1934 缺少三轨工程量采样接口"
    assert "test_lpddr_power_idd_bias_flow" in mock, "ARM Mock 镜像未覆盖完整流程"
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "if(ENABLE_DRIVER_MOCK_TEST)" in cmake, "正式固件不应编译Mock任务体"
    assert mock.index("GPIO_FUNC_PWR_4V0_EN") < mock.index("GPIO_FUNC_PWR_3V3_EN")
    assert "RK完成CKE低准备后先关闭DDR三轨" in mock


def main() -> None:
    verify_profiles_and_tps_encoding()
    verify_pac_idd_conversion()
    verify_source_contract()
    print("LPDDR4/4X: 8 voltage profiles, TPS encoding, PAC IDD and power-flow contracts PASS")
    print("Boundary: RK creates JEDEC IDD/CKE conditions; MCU sequences rails and samples them")


if __name__ == "__main__":
    main()
