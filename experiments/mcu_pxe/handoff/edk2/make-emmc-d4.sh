#!/usr/bin/env bash
# 本机WSL编译H1，仅生成普通D4前级的eMMC升级整包，不烧录。
set -euo pipefail
edk_dir="$(cd "$(dirname "$0")" && pwd)"
exec bash "$edk_dir/../test/experiments/mcu_pxe/handoff/build_pack_wsl.sh" "$edk_dir" emmc-d4
