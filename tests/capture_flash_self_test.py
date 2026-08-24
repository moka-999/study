"""抓取阿波罗 V2 W25Q256 隔离自检日志，并以最终 PASS/FAIL 决定退出码。"""

from __future__ import annotations

import argparse
import codecs
import sys
import time

import serial


PASS_MARKER = b"--- W25Q256 HARDWARE TEST PASS ---"
FAIL_MARKER = b"--- W25Q256 HARDWARE TEST FAIL"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout
    received = bytearray()
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        port.reset_input_buffer()
        print(f"capture: {args.port} @ {args.baud}, timeout={args.timeout:.0f}s")
        while time.monotonic() < deadline:
            chunk = port.read(port.in_waiting or 1)
            if not chunk:
                continue
            received.extend(chunk)
            # 串口 read() 可能把一个 UTF-8 汉字拆成两段，必须使用增量解码器。
            sys.stdout.write(decoder.decode(chunk))
            sys.stdout.flush()
            if PASS_MARKER in received:
                return 0
            if FAIL_MARKER in received:
                return 1

    print("\ncapture: timeout before final PASS/FAIL marker", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
