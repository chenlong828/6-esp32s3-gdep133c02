#!/usr/bin/env python3
"""把照片缩成面板原生的 1200x1600 JPEG，供设备端 JPG -> bin 测试使用。

推荐 4:2:0（--subsampling 2）：面板只有 6 色、150 PPI，色度子采样看不出来，
但 ESP32-S3 解码时 Cb/Cr 的块数只有 4:4:4 的 1/4，能明显缩短解码时间。

用法：
    ./host/make_panel_jpeg.py images/genshin01.jpg \
        -o images/genshin01_1200x1600.jpg --fit cover --quality 90 --subsampling 2
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image


def fit_cover(im: Image.Image, width: int, height: int) -> Image.Image:
    scale = max(width / im.width, height / im.height)
    im2 = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))),
                    Image.LANCZOS)
    left = (im2.width - width) // 2
    top = (im2.height - height) // 2
    return im2.crop((left, top, left + width, top + height))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--size", default="1200x1600")
    ap.add_argument("--fit", default="cover", choices=["cover", "stretch"])
    ap.add_argument("--quality", type=int, default=90)
    ap.add_argument("--subsampling", type=int, default=2, choices=[0, 1, 2],
                    help="0=4:4:4, 1=4:2:2, 2=4:2:0（推荐）")
    args = ap.parse_args()

    width, height = (int(v) for v in args.size.lower().split("x"))
    im = Image.open(args.input).convert("RGB")

    if args.fit == "cover":
        out = fit_cover(im, width, height)
    else:
        out = im.resize((width, height), Image.LANCZOS)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    out.save(args.out, "JPEG", quality=args.quality, subsampling=args.subsampling,
             optimize=True, progressive=False)

    print(json.dumps({
        "input": str(args.input),
        "output": str(args.out),
        "size": [width, height],
        "fit": args.fit,
        "quality": args.quality,
        "subsampling": args.subsampling,
        "bytes": args.out.stat().st_size,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
