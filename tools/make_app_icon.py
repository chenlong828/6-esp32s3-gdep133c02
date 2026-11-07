#!/usr/bin/env python3
"""生成 EDPHost 的 App 图标（macOS .icns）。

只画一个东西：深色圆角底板 + 一块竖着的 13.3" 六色屏。
运行：./.venv/bin/python tools/make_app_icon.py
产物：host/EDPHost/Resources/AppIcon.icns（build_app.sh 会自动带上）
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "host/EDPHost/Resources"
SIZE = 1024

COLORS = [
    (255, 255, 255),
    (0, 0, 0),
    (160, 32, 32),
    (240, 224, 80),
    (80, 128, 184),
    (96, 128, 80),
]


def draw_icon(size: int) -> Image.Image:
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    # 底板：macOS 风格的圆角方块（保留边距，系统会再加阴影）
    margin = int(size * 0.06)
    radius = int(size * 0.22)
    d.rounded_rectangle([margin, margin, size - margin, size - margin],
                        radius=radius, fill=(24, 26, 30, 255))

    # 屏幕：竖屏 3:4，居中
    sw, sh = int(size * 0.44), int(size * 0.60)
    sx = (size - sw) // 2
    sy = (size - sh) // 2
    d.rounded_rectangle([sx - 6, sy - 6, sx + sw + 6, sy + sh + 6],
                        radius=int(size * 0.035), fill=(245, 245, 245, 255))

    band = sh // len(COLORS)
    for i, color in enumerate(COLORS):
        top = sy + i * band
        bottom = sy + sh if i == len(COLORS) - 1 else top + band
        d.rectangle([sx, top, sx + sw, bottom], fill=color + (255,))

    d.rounded_rectangle([sx, sy, sx + sw, sy + sh],
                        radius=int(size * 0.012), outline=(20, 20, 20, 120), width=3)
    return im


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    base = draw_icon(SIZE)

    iconset = OUT / "AppIcon.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)

    specs = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2),
             (256, 1), (256, 2), (512, 1), (512, 2)]
    for points, scale in specs:
        pixels = points * scale
        name = f"icon_{points}x{points}{'@2x' if scale == 2 else ''}.png"
        base.resize((pixels, pixels), Image.LANCZOS).save(iconset / name)

    target = OUT / "AppIcon.icns"
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(target)], check=True)
    shutil.rmtree(iconset)

    preview = OUT / "AppIcon-1024.png"
    base.save(preview)
    print(f"生成 {target} 和 {preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
