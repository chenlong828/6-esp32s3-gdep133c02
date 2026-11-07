#!/usr/bin/env python3
"""Capture the jpg2bin_bench log from the USB-Serial-JTAG console.

Opens the port with DTR/RTS deasserted (so pyserial does not reset the chip),
streams bytes to stdout, and exits after BENCH_DONE or the timeout.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem1101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--marker", default="BENCH_DONE")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port
    s.baudrate = args.baud
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()

    buf = bytearray()
    started = time.time()
    try:
        while time.time() - started < args.timeout:
            data = s.read(4096)
            if not data:
                continue
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            buf.extend(data)
            if args.marker.encode() in buf:
                break
    finally:
        s.close()

    if args.out:
        with open(args.out, "wb") as f:
            f.write(bytes(buf))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
