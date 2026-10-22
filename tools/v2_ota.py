#!/usr/bin/env python3
"""V2固定板型入口；拒绝B1包，不改变固件内的电源模拟或MATS联调开关。"""
from pathlib import Path
from ota_client import main

if __name__ == "__main__":
    raise SystemExit(main(target="apollo-v2-h743", tool_dir=Path(__file__).resolve().parent))
