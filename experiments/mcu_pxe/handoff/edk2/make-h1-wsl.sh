#!/usr/bin/env bash
# 在本机WSL编译H1并生成eMMC/SPI NOR升级包；使用同级test仓库的已核验打包逻辑。
set -euo pipefail
edk_dir="$(cd "$(dirname "$0")" && pwd)"
exec bash "$edk_dir/../test/experiments/mcu_pxe/handoff/build_pack_wsl.sh" "$edk_dir"
