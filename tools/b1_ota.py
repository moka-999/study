#!/usr/bin/env python3
"""B1固定板型入口；交付包用同目录公共实现，源码环境复用同级V2工具实现。"""
from pathlib import Path
import sys

DIRECTORY = Path(__file__).resolve().parent
if not (DIRECTORY / "ota_client.py").is_file():
    sys.path.insert(0, str(DIRECTORY.parents[1] / "test/tools"))

from ota_client import main

if __name__ == "__main__":
    raise SystemExit(main(target="b1-h743", tool_dir=DIRECTORY))
