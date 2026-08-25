"""MCP4725 Boot模式的离线电压与Fast Write编码合同。"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "BSP/MCP4725/mcp4725.c").read_text(encoding="utf-8")
EXPECTED_MV = [0, 300, 600, 900, 1200, 1500, 1800]
EXPECTED_CODE = [0, 372, 745, 1117, 1489, 1861, 2234]


def main() -> None:
    match = re.search(r"s_boot_target_mv\[BOOT_MODE_COUNT\]\s*=\s*\{([^}]*)\}", SOURCE)
    assert match, "未找到Boot目标电压表"
    actual_mv = [int(value) for value in re.findall(r"(\d+)U", match.group(1))]
    assert actual_mv == EXPECTED_MV, f"Boot电压表不符: {actual_mv}"

    actual_code = [(mv * 4095 + 1650) // 3300 for mv in actual_mv]
    assert actual_code == EXPECTED_CODE, f"3.3V DAC码不符: {actual_code}"

    packets = [((code >> 8) & 0x0F, code & 0xFF) for code in actual_code]
    assert packets[-1] == (0x08, 0xBA), "1.8V Fast Write应为08 BA"
    assert max(actual_mv) == 1800, "Boot模式不得高于1.8V"
    print("MCP4725 Boot模式离线验证: 7/7 PASS")


if __name__ == "__main__":
    main()
