#!/usr/bin/env python3
"""B1开发侧构建入口：复用双槽校验/正式打包器，输出可交付的独立工具ZIP。

本脚本只生成文件，不建立设备连接；操作人员解压后不需要这份构建脚本或源码。
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
# 只在开发侧依赖同级工程；交付ZIP已经带上ota_client.py，不需要此构建入口。
sys.path.insert(0, str(ROOT.parent / "test/tools"))
from ota_client import parse_version
from ota_bundle import publish


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Release")
    for tool in ("cmake", "ninja", "compiler"):
        parser.add_argument("--" + tool, type=Path, required=True)
    args = parser.parse_args()
    parse_version(args.version)
    env = dict(os.environ)
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    env["SOURCE_DATE_EPOCH"] = str(int(time.time()))
    shared = ROOT.parent / "test/tests/verify_ota_relocation.py"
    pack = ROOT.parent / "bootloader/tools/pack_ota.py"
    if not shared.is_file() or not pack.is_file():
        raise ValueError("构建侧需要同级test和bootloader工程；交付ZIP本身不依赖它们")
    python = [sys.executable, "-B", "-X", "utf8"]
    print("仅构建B1槽限App并验证A1/A2修正；不连接设备。", flush=True)
    subprocess.run(python + [str(shared), "--app-root", str(ROOT), "--app-name", "b1",
        "--rk-uart", "4", "--ota-boot", "--version", args.version, "--build-type", args.configuration,
        "--cmake", str(args.cmake), "--ninja", str(args.ninja), "--compiler", str(args.compiler)],
        env=env, check=True)
    build = ROOT / "build" / f"ota-app-{args.version}-{args.configuration.lower()}-a1"
    subprocess.run(python + [str(pack), "--app-build", str(build), "--app-name", "b1",
                            "--compiler", str(args.compiler)], env=env, check=True)
    publish(build / "b1.ota", "b1-h743", ROOT / "build")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        raise SystemExit(f"构建交付失败：{error}") from error
