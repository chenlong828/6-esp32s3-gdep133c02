#!/usr/bin/env python3
"""逐字节验证 Swift 上位机的抖动 / 打包 / CRC 与 Python 参考实现一致。

参考实现就是仓库里的 host/e6_convert.py（官网在线工具逆向版，已在真机验证）。
验证方法：
  1. 用 e6_convert.test_pattern() 生成 1200×1600 测试图；
  2. 导出裸 RGB888，交给 Swift 的 --dither-raw 走同一条抖动 + 打包；
  3. Python 侧对同一份裸 RGB 跑 e6_convert.dither + pack；
  4. 逐字节比较两边输出（这一步刻意绕开缩放/解码，只比抖动与打包）；
  5. 另外用随机文件比对 zlib.crc32 与 Swift 的 --crc32。

用法：
    ./.venv/bin/python tools/verify_swift_dither.py
    ./.venv/bin/python tools/verify_swift_dither.py --binary host/EDPHost/.build/release/EDPHost
    ./.venv/bin/python tools/verify_swift_dither.py --quick   # 只测 fs-ser

退出码：0 = 全部一致，1 = 有不一致或执行失败。
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

import e6_convert as ref  # noqa: E402


def run_swift(binary: Path, args: list[str]) -> str:
    proc = subprocess.run([str(binary), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"Swift 命令失败：{' '.join(args)}\n{proc.stderr}")
    return proc.stdout.strip()


def first_diff(a: bytes, b: bytes) -> tuple[int, int]:
    limit = min(len(a), len(b))
    index = next((i for i in range(limit) if a[i] != b[i]), limit)
    count = sum(1 for x, y in zip(a, b) if x != y)
    return index, count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "host/EDPHost/.build/release/EDPHost")
    parser.add_argument("--quick", action="store_true", help="只验证默认的 fs-ser")
    args = parser.parse_args()

    binary = args.binary
    if not binary.exists():
        print(f"找不到 {binary}\n先执行：cd host/EDPHost && swift build -c release --disable-sandbox",
              file=sys.stderr)
        return 1

    modes = ["fs-ser"] if args.quick else ["fs-ser", "fs", "atkinson", "none"]
    cases = [(mode, 1.0) for mode in modes]
    if not args.quick:
        cases.append(("fs-ser", 0.5))

    print("生成测试图（1200×1600）...")
    image = ref.test_pattern(1200, 1600)
    rgb_u8 = np.asarray(image, dtype=np.uint8)
    rgb_f32 = rgb_u8.astype(np.float32)

    failures = 0
    with tempfile.TemporaryDirectory(prefix="edp-verify-") as tmp:
        tmpdir = Path(tmp)
        raw_path = tmpdir / "pattern.rgb"
        raw_path.write_bytes(rgb_u8.tobytes())
        swift_out = tmpdir / "swift.bin"

        print(f"{'模式':<10}{'强度':<6}{'字节数':<10}{'结果':<8}说明")
        print("-" * 78)

        for mode, strength in cases:
            py_packed = ref.pack(ref.dither(rgb_f32, mode, strength))

            info = run_swift(binary, ["--dither-raw", "1200", "1600",
                                      str(raw_path), str(swift_out),
                                      mode, str(strength)])
            swift_packed = swift_out.read_bytes()

            same = swift_packed == py_packed
            note = info
            if not same:
                index, count = first_diff(swift_packed, py_packed)
                note = (f"首个不同 @ {index}，共 {count} 字节不同 "
                        f"(py={py_packed[index]:02x} swift={swift_packed[index]:02x})")
                failures += 1

            print(f"{mode:<10}{strength:<6}{len(swift_packed):<10}"
                  f"{'一致' if same else '不一致':<8}{note}")

        # CRC32：随机数据 + 与 Python zlib 对比
        print("-" * 78)
        rng = np.random.default_rng(20261107)
        payload = rng.integers(0, 256, size=1_000_003, dtype=np.uint8).tobytes()
        blob = tmpdir / "crc.bin"
        blob.write_bytes(payload)

        py_crc = zlib.crc32(payload) & 0xFFFFFFFF
        swift_info = run_swift(binary, ["--crc32", str(blob)])
        swift_crc = int(swift_info.split("crc32=")[1].split()[0], 16)
        crc_ok = py_crc == swift_crc
        if not crc_ok:
            failures += 1
        print(f"CRC32      {'':<6}{len(payload):<10}{'一致' if crc_ok else '不一致':<8}"
              f"py={py_crc:08x} swift={swift_crc:08x}")

    print("-" * 78)
    if failures:
        print(f"失败：{failures} 项不一致")
        return 1
    print("全部一致：Swift 的抖动、打包、CRC 与 Python 参考实现逐字节相同")
    return 0


if __name__ == "__main__":
    sys.exit(main())
