#!/usr/bin/env python3
"""
E Ink Spectra 6 (E6) 上位机图像处理工具 —— 面向佳显 GDEP133C02 / GDEB0709E01

复刻官网在线工具 https://www.e-paper-display.com/bmp.html 的处理链路：
    读图 → 拉伸到面板分辨率 → 6 色误差扩散抖动 → 4bpp 打包 → .bin / .h

用法
    # 1) 出图（默认 1200x1600，Floyd-Steinberg 蛇形，输出 .bin + .h + 预览）
    python e6_convert.py photo.jpg -o out/photo.bin --header out/image.h

    # 2) 多张图打包成一个 SD 卡 bin（带帧间隔，固件按间隔轮播）
    python e6_convert.py a.jpg b.jpg c.jpg -o out/images.bin --interval 150

    # 3) 生成方向自检图（第一次点亮必做）
    python e6_convert.py --test-pattern -o out/test.bin --header out/test_image.h

    # 4) 反向验证：把 .bin 解回 PNG，确认字节与预期一致
    python e6_convert.py --decode out/photo.bin -o out/photo_back.png

依赖：pillow + numpy
"""

from __future__ import annotations

import argparse
import json
from array import array
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------- 调色板

# 官网在线工具里的两块调色板（REAL = 面板实测色，DRIVER = 发给驱动的纯色）
REAL_PALETTE = np.array([
    [255, 255, 255],   # 0 white
    [0, 0, 0],         # 1 black
    [160, 32, 32],     # 2 red
    [240, 224, 80],    # 3 yellow
    [80, 128, 184],    # 4 blue
    [96, 128, 80],     # 5 green
], dtype=np.float32)

# 面板 4bit 像素码：黑0 白1 黄2 红3 蓝5 绿6
PALETTE_CODE = np.array([0x1, 0x0, 0x3, 0x2, 0x5, 0x6], dtype=np.uint8)

# 预览用的“驱动纯色”（在屏幕上看起来更接近真实观感时改用 REAL_PALETTE）
DRIVER_PALETTE = np.array([
    [255, 255, 255],
    [0, 0, 0],
    [255, 0, 0],
    [255, 255, 0],
    [0, 0, 255],
    [0, 255, 0],
], dtype=np.uint8)

MAGIC = b"EPD6BIN\0"


# ---------------------------------------------------------------- 抖动算法
# 权重表与官网 JS 完全一致；误差扩散后按 Uint8ClampedArray 语义钳位到 0..255

KERNELS = {
    "fs": [(1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)],
    "atkinson": [(1, 0, 1 / 8), (2, 0, 1 / 8), (-1, 1, 1 / 8),
                 (0, 1, 1 / 8), (1, 1, 1 / 8), (0, 2, 1 / 8)],
    "stucki": [(1, 0, 8 / 42), (2, 0, 4 / 42),
               (-2, 1, 2 / 42), (-1, 1, 4 / 42), (0, 1, 8 / 42), (1, 1, 4 / 42), (2, 1, 2 / 42),
               (-2, 2, 1 / 42), (-1, 2, 2 / 42), (0, 2, 4 / 42), (1, 2, 2 / 42), (2, 2, 1 / 42)],
    "jjn": [(1, 0, 7 / 48), (2, 0, 5 / 48),
            (-2, 1, 3 / 48), (-1, 1, 5 / 48), (0, 1, 7 / 48), (1, 1, 5 / 48), (2, 1, 3 / 48),
            (-2, 2, 1 / 48), (-1, 2, 3 / 48), (0, 2, 5 / 48), (1, 2, 3 / 48), (2, 2, 1 / 48)],
}


def _closest_index(px: np.ndarray) -> np.ndarray:
    """按欧氏距离取最近的调色板索引（与 JS closestColor 等价，向量化）"""
    d = ((px[:, None, :] - REAL_PALETTE[None, :, :]) ** 2).sum(axis=2)
    return d.argmin(axis=1)


_PAL = REAL_PALETTE.reshape(-1).tolist()          # 6x3 展平，供标量循环内联访问


def dither(rgb: np.ndarray, method: str = "fs-ser", strength: float = 1.0) -> np.ndarray:
    """
    输入 HxWx3 float32 (0..255)，输出 HxW uint8 调色板索引。

    method: fs | fs-ser | atkinson | stucki | jjn | none
    strength: 误差扩散强度（1.0 = 官网默认；0 = 不扩散，只剩最近色量化）

    说明：误差扩散必须逐像素串行，这里用 array('f') 平铺缓冲 + 内联最近色判断，
    1200x1600 约 10 s（纯 numpy 逐元素访问反而更慢）。
    """
    h, w = rgb.shape[:2]

    if method == "none":
        flat = rgb.reshape(-1, 3)
        return _closest_index(flat).reshape(h, w).astype(np.uint8)

    ser = method == "fs-ser"
    kernel = KERNELS["fs" if ser else method]

    buf = array("f", np.clip(rgb, 0, 255).astype(np.float32).reshape(-1).tolist())
    out = np.empty(h * w, dtype=np.uint8)
    pr, pg, pb = _PAL[0::3], _PAL[1::3], _PAL[2::3]
    thr = strength

    for y in range(h):
        base = y * w * 3
        rev = ser and (y & 1)
        for k in range(w):
            x = (w - 1 - k) if rev else k
            i = base + x * 3
            r = buf[i]; g = buf[i + 1]; b = buf[i + 2]

            best, bd = 0, 1e18
            for c in range(6):
                dr = r - pr[c]; dg = g - pg[c]; db = b - pb[c]
                d = dr * dr + dg * dg + db * db
                if d < bd:
                    bd = d; best = c
            out[y * w + x] = best

            er = (r - pr[best]) * thr
            eg = (g - pg[best]) * thr
            eb = (b - pb[best]) * thr
            buf[i] = pr[best]; buf[i + 1] = pg[best]; buf[i + 2] = pb[best]

            for dx, dy, wt in kernel:
                sx = x - dx if rev else x + dx
                sy = y + dy
                if 0 <= sx < w and sy < h:
                    j = (sy * w + sx) * 3
                    v = buf[j] + er * wt
                    buf[j] = 0.0 if v < 0 else (255.0 if v > 255 else v)
                    v = buf[j + 1] + eg * wt
                    buf[j + 1] = 0.0 if v < 0 else (255.0 if v > 255 else v)
                    v = buf[j + 2] + eb * wt
                    buf[j + 2] = 0.0 if v < 0 else (255.0 if v > 255 else v)

    return out.reshape(h, w)


# ---------------------------------------------------------------- 图像前处理


def load_and_fit(path: Path, width: int, height: int, fit: str = "stretch",
                 rotate: int = 0, flip: str = "") -> Image.Image:
    im = Image.open(path).convert("RGB")
    if rotate:
        im = im.rotate(-rotate, expand=True)          # 顺时针
    if "h" in flip:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if "v" in flip:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)

    if fit == "stretch":                              # 官网行为：直接拉伸，不变形保护
        return im.resize((width, height), Image.LANCZOS)
    if fit == "cover":                                # 等比铺满后居中裁剪
        scale = max(width / im.width, height / im.height)
        im2 = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))),
                        Image.LANCZOS)
        left = (im2.width - width) // 2
        top = (im2.height - height) // 2
        return im2.crop((left, top, left + width, top + height))
    if fit == "contain":                              # 等比缩进白边
        scale = min(width / im.width, height / im.height)
        im2 = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))),
                        Image.LANCZOS)
        canvas = Image.new("RGB", (width, height), (255, 255, 255))
        canvas.paste(im2, ((width - im2.width) // 2, (height - im2.height) // 2))
        return canvas
    raise ValueError(f"unknown fit: {fit}")


# ---------------------------------------------------------------- 打包


def pack(indices: np.ndarray) -> bytes:
    """HxW 索引 → 4bpp 行优先字节流（高 nibble = 左像素）"""
    h, w = indices.shape
    if w % 2:
        raise ValueError("width must be even")
    codes = PALETTE_CODE[indices]
    packed = (codes[:, 0::2] << 4) | codes[:, 1::2]
    return packed.tobytes()


def unpack(data: bytes, width: int, height: int) -> np.ndarray:
    """字节流 → HxW 索引（pack 的逆运算）"""
    need = width * height // 2
    if len(data) < need:
        raise ValueError(f"need {need} bytes, got {len(data)}")
    raw = np.frombuffer(data[:need], dtype=np.uint8)
    hi = raw >> 4
    lo = raw & 0x0F
    px = np.empty(raw.size * 2, dtype=np.uint8)
    px[0::2] = hi
    px[1::2] = lo
    code_to_idx = {int(c): i for i, c in enumerate(PALETTE_CODE)}
    lut = np.array([code_to_idx.get(v, 0) for v in range(16)], dtype=np.uint8)
    return lut[px].reshape(height, width)


# ---------------------------------------------------------------- 输出


def write_bin(path, frames, width: int, height: int, interval_ms: int) -> None:
    """SD 卡固件使用的容器：magic[8] + u16 w + u16 h + u16 frames + u32 interval + 数据"""
    path = Path(path)
    header = MAGIC + struct.pack("<HHHI", width, height, len(frames), interval_ms)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        for fr in frames:
            f.write(fr)


def read_bin(path) -> tuple[int, int, int, list[bytes]]:
    blob = Path(path).read_bytes()
    if blob[:8] == MAGIC:
        w, h, n, interval = struct.unpack("<HHHI", blob[8:18])
        body = blob[18:]
    else:                                   # 裸 960000 字节
        w = h = 0
        n, interval, body = 1, 0, blob
    size = (w * h) // 2 if w and h else len(body)
    frames = [body[i * size:(i + 1) * size] for i in range(n)]
    return w, h, interval, frames


def write_header(path, data: bytes, name: str = "gImage", per_line: int = 16) -> None:
    path = Path(path)
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "// generated by e6_convert.py — 4bpp, 2 pixels/byte, high nibble = left pixel\n"
        "#pragma once\n"
        f"const unsigned char {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n",
        encoding="utf-8",
    )


def render(indices: np.ndarray, palette: np.ndarray) -> Image.Image:
    return Image.fromarray(palette[indices].astype(np.uint8), "RGB")


# ---------------------------------------------------------------- 自检图


def _font(size: int):
    for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if Path(p).exists():
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def test_pattern(width: int = 1200, height: int = 1600) -> Image.Image:
    """
    方向 / 色彩自检图：
      - 左上角红色实心方块，右下角绿色实心方块（上下颠倒或左右镜像一眼可见）
      - 顶部 "TOP / LEFT" 文字，底部倒置文字（若上屏后底部文字可正读，说明方向反了）
      - 6 色色块 + 灰阶渐变（检查抖动与色偏）
    """
    im = Image.new("RGB", (width, height), "white")
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, width // 6, height // 10], fill=(160, 32, 32))          # 左上红
    d.rectangle([width - width // 6, height - height // 10, width, height],
                fill=(96, 128, 80))                                           # 右下绿
    d.text((width // 6 + 30, 40), "TOP-LEFT = RED", font=_font(72), fill="black")
    d.text((width // 6 + 30, 140), "orientation check 1200x1600", font=_font(48), fill="black")

    labels = ["BLACK", "WHITE", "YELLOW", "RED", "BLUE", "GREEN"]
    cols = [(0, 0, 0), (255, 255, 255), (240, 224, 80), (160, 32, 32), (80, 128, 184), (96, 128, 80)]
    bw = width // 6
    y0 = height // 3
    for i, (lab, col) in enumerate(zip(labels, cols)):
        d.rectangle([i * bw, y0, (i + 1) * bw - 1, y0 + 260], fill=col, outline="black")
        txt = "black" if sum(col) > 380 else "white"
        d.text((i * bw + 16, y0 + 200), lab, font=_font(40), fill=txt)

    for x in range(width):                                    # 水平灰阶
        v = int(255 * x / (width - 1))
        d.line([(x, y0 + 320), (x, y0 + 420)], fill=(v, v, v))
    for y in range(400):                                      # 垂直 RGB 渐变
        r = int(255 * y / 399)
        d.line([(0, y0 + 460 + y), (width // 3, y0 + 460 + y)], fill=(r, 0, 0))
        d.line([(width // 3, y0 + 460 + y), (2 * width // 3, y0 + 460 + y)], fill=(0, r, 0))
        d.line([(2 * width // 3, y0 + 460 + y), (width, y0 + 460 + y)], fill=(0, 0, r))

    d.text((60, height - 140), "BOTTOM-RIGHT = GREEN", font=_font(48), fill="black")
    return im


# ---------------------------------------------------------------- CLI


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="E Ink Spectra 6 (E6) 上位机图像转换")
    ap.add_argument("inputs", nargs="*", type=Path, help="输入图片（可多张，多张=多帧）")
    ap.add_argument("-o", "--out", type=Path, help="输出 .bin")
    ap.add_argument("--size", default="1200x1600", help="面板分辨率，默认 1200x1600")
    ap.add_argument("--dither", default="fs-ser",
                    choices=["fs", "fs-ser", "atkinson", "stucki", "jjn", "none"])
    ap.add_argument("--strength", type=float, default=1.0, help="抖动强度 0..1")
    ap.add_argument("--fit", default="stretch", choices=["stretch", "cover", "contain"])
    ap.add_argument("--rotate", type=int, default=0, help="顺时针旋转角度 90/180/270")
    ap.add_argument("--flip", default="", help="h=水平、v=垂直，可组合 h/v")
    ap.add_argument("--interval", type=int, default=150, help="帧间隔 ms，写进 bin 头")
    ap.add_argument("--header", type=Path, help="同时导出 C 数组 image.h")
    ap.add_argument("--array-name", default="gImage")
    ap.add_argument("--preview", type=Path, help="导出抖动后预览 PNG")
    ap.add_argument("--decode", type=Path, help="解码 .bin 为 PNG")
    ap.add_argument("--test-pattern", action="store_true", help="生成方向/色彩自检图")
    ap.add_argument("--report", type=Path, help="输出 JSON 摘要")
    args = ap.parse_args(argv)

    w, h = (int(v) for v in args.size.lower().split("x"))

    # 解码模式
    if args.decode:
        bw, bh, interval, frames = read_bin(args.decode)
        bw, bh = (bw or w), (bh or h)
        outs = []
        for i, fr in enumerate(frames):
            idx = unpack(fr, bw, bh)
            p = args.decode.with_name(f"{args.decode.stem}_frame{i}.png")
            render(idx, DRIVER_PALETTE.astype(np.float32)).save(p)
            outs.append(str(p))
        print(json.dumps({"frames": len(frames), "interval_ms": interval, "outputs": outs},
                         ensure_ascii=False, indent=2))
        return 0

    # 输入图
    if args.test_pattern:
        jobs = [("test-pattern", test_pattern(w, h))]
    elif args.inputs:
        jobs = [(p.name, load_and_fit(p, w, h, args.fit, args.rotate, args.flip))
                for p in args.inputs]
    else:
        ap.error("需要输入图片，或用 --test-pattern / --decode")

    frames, report = [], []
    for name, im in jobs:
        rgb = np.asarray(im, dtype=np.float32)
        idx = dither(rgb, args.dither, args.strength)
        data = pack(idx)
        assert len(data) == w * h // 2, (len(data), w * h // 2)
        frames.append(data)
        uniq, cnt = np.unique(idx, return_counts=True)
        share = {["白", "黑", "红", "黄", "蓝", "绿"][i]: round(float(c) / idx.size, 4)
                 for i, c in zip(uniq.tolist(), cnt.tolist())}
        report.append({"input": name, "bytes": len(data), "color_share": share})
        if args.preview:
            pp = args.preview if len(jobs) == 1 else \
                args.preview.with_name(f"{args.preview.stem}_{name}{args.preview.suffix}")
            pp.parent.mkdir(parents=True, exist_ok=True)
            render(idx, REAL_PALETTE).save(pp)

    if args.out:
        write_bin(args.out, frames, w, h, args.interval)
    if args.header:
        if len(frames) == 1:
            write_header(args.header, frames[0], args.array_name)
        else:                                   # 多帧时导出二维数组
            body = "\n".join(
                f"const unsigned char {args.array_name}_{i}[{len(f)}] __attribute__((unused));"
                for i, f in enumerate(frames))
            args.header.parent.mkdir(parents=True, exist_ok=True)
            args.header.write_text(
                "// multiple frames: see .bin for the packed data\n" + body + "\n", encoding="utf-8")

    summary = {"size": [w, h], "dither": args.dither, "strength": args.strength,
               "fit": args.fit, "frames": len(frames),
               "bytes_per_frame": w * h // 2,
               "bin_total": (len(frames) * w * h // 2 + 18) if args.out else None,
               "detail": report}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
