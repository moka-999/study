#!/usr/bin/env bash
# 仅在本机 Linux/WSL 编译完整 H1 EDK2；不打包旧 FIT、不联系服务器、不烧录。
set -e
cd "${1:-/mnt/d/pro/edk2}"
export WORKSPACE="$PWD"
export PACKAGES_PATH="$WORKSPACE/edk2:$WORKSPACE/edk2-platforms:$WORKSPACE/edk2-non-osi"
export PYTHON_COMMAND=python3
# 本机固定 GCC 11，避免默认版本升级改变旧 EDK 的告警和语言语义；原交叉基线为 10.3。
command -v gcc-11
command -v g++-11
if [ -z "${GCC5_AARCH64_PREFIX:-}" ]; then
  handoff_tool_dir="$WORKSPACE/Build/H1Toolchain"
  mkdir -p "$handoff_tool_dir"
  for tool in gcc gcc-ar; do
    handoff_tool_path=$(command -v "aarch64-linux-gnu-$tool-11")
    ln -sf "$handoff_tool_path" "$handoff_tool_dir/aarch64-linux-gnu-$tool"
  done
  ln -sf "$(command -v aarch64-linux-gnu-objcopy)" "$handoff_tool_dir/aarch64-linux-gnu-objcopy"
  export GCC5_AARCH64_PREFIX="$handoff_tool_dir/aarch64-linux-gnu-"
fi
command -v "${GCC5_AARCH64_PREFIX}gcc"
gcc-11 --version
"${GCC5_AARCH64_PREFIX}gcc" --version
unset EDK_TOOLS_PATH
source edk2/edksetup.sh BaseTools
# 来源快照含旧 GCC 11 的 .o/.d；先清主机工具产物，避免引用另一台机器的头文件。
make -C edk2/BaseTools/Source/C clean
make -C edk2/BaseTools -j4 'CC=gcc-11 -std=gnu11' 'BUILD_CC=gcc-11 -std=gnu11' 'BUILD_CXX=g++-11'
# 切换交叉编译器时必须清除旧 LTO 对象；常规同工具链增量构建无需设置该变量。
if [ "${H1_CLEAN:-0}" = 1 ]; then
  build cleanall -a AARCH64 -b DEBUG -t GCC5 -p edk2-platforms/Platform/Rockchip/RK3588/RK3588.dsc
fi
build -a AARCH64 -b DEBUG -t GCC5 -n 4 -p edk2-platforms/Platform/Rockchip/RK3588/RK3588.dsc
