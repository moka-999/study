"""核对实际构建对象：TCP任务必须与lwIP socket引用同一errno，不访问另一份newlib错误码。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()
    for name in ("tcp_task.c.obj", "sockets.c.obj"):
        objects = list(args.build_dir.rglob(name))
        if len(objects) != 1:
            raise RuntimeError(f"{name}: 需要唯一实际构建对象，找到{len(objects)}个")
        output = subprocess.check_output([args.nm, "-u", str(objects[0])], text=True)
        symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
        if "errno" not in symbols or "__errno" in symbols:
            raise RuntimeError(f"{name}: errno来源不一致，errno={'errno' in symbols}, __errno={'__errno' in symbols}")
    print(f"PASS: {args.build_dir} TCP与lwIP socket均引用同一errno")


if __name__ == "__main__":
    main()
