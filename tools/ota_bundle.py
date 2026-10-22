#!/usr/bin/env python3
"""离线生成固定板型的独立升级工具包；不打包源工程、编译器或任何设备操作。"""
from datetime import datetime
import json
from pathlib import Path
import shutil
import zipfile

from ota_client import read_package

ROOT = Path(__file__).resolve().parents[1]


def bundle(package_path: Path, destination: Path, target: str) -> Path:
    """先完整校验再交付，新目录/ZIP不覆盖历史产物，禁止混装另一个板型入口。"""
    package = read_package(package_path, target)
    board = "b1" if target == "b1-h743" else "v2"
    sources = ROOT.parent / "b1/tools" if board == "b1" else ROOT / "tools"
    archive = Path(str(destination) + ".zip")
    if destination.exists() or archive.exists():
        raise ValueError("交付目录或ZIP已存在，请使用新名称，避免覆盖历史包")
    destination.mkdir(parents=True, exist_ok=False)
    for name in (f"{board}_ota.py", f"start_{board}_ota.cmd", f"{board}_ota_README.md"):
        if name.endswith(".cmd"):
            # Windows命令解释器需要CRLF，避免UTF-8中文注释在LF文件中被错误截断执行。
            text = (sources / name).read_text(encoding="utf-8")
            (destination / name).write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
        else:
            shutil.copyfile(sources / name, destination / name)
    shutil.copyfile(ROOT / "tools/ota_client.py", destination / "ota_client.py")
    # 使用已校验的内存快照，避免源文件在校验后被另一轮构建替换。
    (destination / f"{board}.ota").write_bytes(package.data)
    (destination / "package_info.json").write_text(
        json.dumps(package.describe(), ensure_ascii=False, indent=2), encoding="utf-8")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as output:
        for file in sorted(destination.iterdir()):
            output.write(file, file.name)
    return archive


def publish(package_path: Path, target: str, build_root: Path) -> Path:
    """使用版本和本次时间生成历史目录，返回可转交同事的ZIP绝对路径。"""
    package = read_package(package_path, target)
    board = "b1" if target == "b1-h743" else "v2"
    version = package.describe()["version"]
    destination = build_root / (f"{board}-ota-tool-{version}-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    archive = bundle(package_path, destination, target)
    print(f"{board.upper()}交付包：{archive}\n工具目录：{destination}\n只生成文件，未连接或烧录板卡。", flush=True)
    return archive
