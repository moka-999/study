#!/usr/bin/env bash
# 本机WSL一键构建和打包；只生成文件，不访问服务器或板卡。
set -euo pipefail
handoff_dir="$(cd "$(dirname "$0")" && pwd)"
edk_dir="${1:-/mnt/d/pro/edk2}"
variant="${2:-all}"
python3 "$handoff_dir/package_wsl.py" --check-tools --edk-root "$edk_dir" --variant "$variant"
bash "$handoff_dir/edk2/build_local.sh" "$edk_dir"
python3 "$handoff_dir/package_wsl.py" --edk-root "$edk_dir" --variant "$variant"
