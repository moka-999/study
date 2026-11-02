"""在工程build目录安装打包依赖并生成独立窗口程序，不修改全局Python环境。"""
import os
from pathlib import Path
import shutil
import subprocess
import sys


def finish_package(source, build):
    """附使用说明和未改动的运行库许可，压缩包可整体交付给同事。"""
    destination = build / "b1-debug-tool"
    shutil.copyfile(source / "README.md", destination / "README.md")
    notices = []
    libraries = [("Python", Path(sys.base_prefix) / "LICENSE.txt"),
                 ("Tcl/Tk", Path(sys.base_prefix) / "tcl" / "tk8.6" / "license.terms"),
                 ("PyInstaller", build / "b1-debug-builder" / "pyinstaller-6.16.0.dist-info" / "licenses" / "COPYING.txt")]
    for name, path in libraries:
        if path.is_file():
            notices.append(name + "\n\n" + path.read_text(encoding="utf-8"))
    (destination / "THIRD_PARTY_LICENSES.txt").write_text("\n\n".join(notices), encoding="utf-8")
    shutil.make_archive(str(build / "JMCT_B1_Debug"), "zip", destination)


def main():
    source = Path(__file__).resolve().parent
    build = source.parents[1] / "build"
    dependencies = build / "b1-debug-builder"
    if not (dependencies / "PyInstaller" / "__init__.py").exists():
        subprocess.run([sys.executable, "-m", "pip", "install", "--target", str(dependencies),
                        "PyInstaller==6.16.0"], check=True)
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(dependencies)
    # 某些便携Python路径不能被Tcl直接读取；复制资源到工程build，打包时一起收集。
    tcl_source = Path(sys.base_prefix) / "tcl"
    if tcl_source.is_dir():
        tcl_copy = build / "b1-debug-tcl"
        shutil.copytree(tcl_source, tcl_copy, dirs_exist_ok=True)
        environment["TCL_LIBRARY"] = str(tcl_copy / "tcl8.6")
        environment["TK_LIBRARY"] = str(tcl_copy / "tk8.6")
    destination = build / "b1-debug-tool"
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
                    "--onefile", "--windowed", "--name", "JMCT_B1_Debug",
                    "--workpath", str(build / "b1-debug-package"),
                    "--specpath", str(build / "b1-debug-package"),
                    "--distpath", str(destination), str(source / "app.py")],
                   env=environment, check=True, cwd=source)
    finish_package(source, build)
    print(f"可直接双击：{destination / 'JMCT_B1_Debug.exe'}")


if __name__ == "__main__":
    main()
