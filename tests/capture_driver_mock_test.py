"""抓取阿波罗V2三类驱动Mock自检日志，并按最终FAIL数量返回退出码。"""

from __future__ import annotations

import argparse
import codecs
import re
import sys
import time

import serial


RESULT_PATTERN = re.compile(rb"RESULT:\s+(\d+) PASS,\s+(\d+) FAIL")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    received = bytearray()
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    deadline = time.monotonic() + args.timeout
    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        port.reset_input_buffer()
        print(f"capture: {args.port} @ {args.baud}, timeout={args.timeout:.0f}s")
        while time.monotonic() < deadline:
            chunk = port.read(port.in_waiting or 1)
            if not chunk:
                continue
            received.extend(chunk)
            sys.stdout.write(decoder.decode(chunk))
            sys.stdout.flush()
            match = RESULT_PATTERN.search(received)
            if match:
                return 0 if int(match.group(2)) == 0 else 1

    print("\ncapture: timeout before driver test result", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
