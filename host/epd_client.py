#!/usr/bin/env python3
"""GDEP133C02 局域网刷屏固件 —— 参考客户端（命令行）。

图形界面的 Swift 版在 host/EDPHost/；这个脚本更适合脚本化、联调和故障排查，
同时也是 HTTP 接口的可执行文档。

    ./.venv/bin/python host/epd_client.py discover        # 广播找设备
    ./.venv/bin/python host/epd_client.py status          # 设备状态
    ./.venv/bin/python host/epd_client.py send 照片.jpg    # 转换 + 上传一条龙
    ./.venv/bin/python host/epd_client.py upload out/photo.bin
    ./.venv/bin/python host/epd_client.py history         # SD 归档列表
    ./.venv/bin/python host/epd_client.py show 2026xxxx.bin
    ./.venv/bin/python host/epd_client.py clear white

设备地址：默认自动广播发现；也可以用 --ip 192.168.1.50 直接指定。
退出码：0 成功，1 失败，2 参数/发现错误。
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import discover as disc  # noqa: E402
import e6_convert as conv  # noqa: E402

DEFAULT_HTTP_PORT = 80
TIMEOUT = 30


# ---------------------------------------------------------------- HTTP


class Device:
    def __init__(self, ip: str, http_port: int = DEFAULT_HTTP_PORT, mac: str = "-",
                 hostname: str = "-"):
        self.ip = ip
        self.http_port = http_port or DEFAULT_HTTP_PORT
        self.mac = mac
        self.hostname = hostname

    @property
    def base(self) -> str:
        return f"http://{self.ip}:{self.http_port}"

    def __str__(self) -> str:
        return f"{self.hostname} {self.ip}:{self.http_port} (mac {self.mac})"


def request(device: Device, path: str, method: str = "GET",
            body: bytes | None = None, headers: dict | None = None) -> tuple[int, dict | bytes]:
    url = f"{device.base}{path}"
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Accept", "application/json")
    for key, value in (headers or {}).items():
        req.add_header(key, value)

    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as response:
            raw = response.read()
            status = response.status
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        status = exc.code
    except urllib.error.URLError as exc:
        raise SystemExit(f"连不上 {url}：{exc.reason}") from exc

    try:
        return status, json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return status, raw


def print_json(payload: dict | bytes) -> None:
    if isinstance(payload, dict):
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print(payload.decode("utf-8", "replace"))


# ---------------------------------------------------------------- 发现


def find_device(ip: str | None, http_port: int = DEFAULT_HTTP_PORT,
                timeout: float = 2.0) -> Device:
    if ip:
        return Device(ip, http_port)

    targets = disc.default_targets()
    found = disc.scan(targets, disc.DEFAULT_PORT, timeout, retries=3, interval=0.4)
    devices = [d for d in found.values() if d.ip and d.ip != "0.0.0.0"]
    if not devices:
        raise SystemExit(
            "未发现设备。检查：同一网段？设备已连 WiFi？UDP 3232 没被防火墙拦？\n"
            "也可以直接用 --ip 指定地址。")

    devices.sort(key=lambda d: d.hostname)
    chosen = devices[0]
    if len(devices) > 1:
        print(f"发现 {len(devices)} 台设备，使用 {chosen.ip}；其余："
              + ", ".join(d.ip for d in devices[1:]), file=sys.stderr)

    return Device(chosen.ip, chosen.http_port, chosen.mac, chosen.hostname)


# ---------------------------------------------------------------- 命令


def cmd_discover(args: argparse.Namespace) -> int:
    found = disc.scan(disc.default_targets(), disc.DEFAULT_PORT,
                      args.timeout, retries=3, interval=0.4)
    devices = [d for d in found.values() if d.ip and d.ip != "0.0.0.0"]
    if not devices:
        print("未发现设备", file=sys.stderr)
        return 2
    for d in sorted(devices, key=lambda x: x.ip):
        print(f"{d.ip:<16} http://{d.ip}:{d.http_port or 80}  mac={d.mac}  host={d.hostname}")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    device = find_device(args.ip, args.http_port)
    status, payload = request(device, "/api/v1/status")
    print_json(payload)
    return 0 if status == 200 else 1


def cmd_history(args: argparse.Namespace) -> int:
    device = find_device(args.ip, args.http_port)
    status, payload = request(device, "/api/v1/history")
    if status != 200 or not isinstance(payload, dict):
        print_json(payload)
        return 1
    files = payload.get("files", [])
    print(f"归档 {payload.get('count', len(files))} 张，当前：{payload.get('current') or '（无）'}")
    for item in files:
        print(f"  {item['name']:<44} {item['bytes']:>9} B")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    device = find_device(args.ip, args.http_port)
    status, payload = request(device, f"/api/v1/display?file={args.file}", method="POST")
    print_json(payload)
    return 0 if status in (200, 202) else 1


def cmd_clear(args: argparse.Namespace) -> int:
    device = find_device(args.ip, args.http_port)
    status, payload = request(device, f"/api/v1/clear?color={args.color}", method="POST")
    print_json(payload)
    return 0 if status in (200, 202) else 1


def upload_bytes(device: Device, blob: bytes, name: str) -> dict:
    crc = zlib.crc32(blob) & 0xFFFFFFFF
    headers = {
        "Content-Type": "application/octet-stream",
        "X-Image-Name": name,
        "X-Timestamp": str(int(time.time())),
        "X-Crc32": f"{crc:08x}",
    }
    started = time.monotonic()
    status, payload = request(device, "/api/v1/image", method="POST", body=blob, headers=headers)
    elapsed = time.monotonic() - started

    if status != 200 or not isinstance(payload, dict) or not payload.get("ok"):
        print_json(payload)
        raise SystemExit(f"上传失败（HTTP {status}）")

    print(f"上传完成：{payload['file']}，{payload['bytes']} B，"
          f"设备端 {payload['received_ms']} ms（本地 {elapsed:.2f} s），"
          f"crc32={crc:08x} {'已排队刷屏' if payload.get('queued') else '未排队'}")
    return payload


def cmd_upload(args: argparse.Namespace) -> int:
    path = Path(args.file)
    if not path.exists():
        raise SystemExit(f"找不到 {path}")
    blob = path.read_bytes()

    if len(blob) != 960018:
        raise SystemExit(f"{path.name} 长度 {len(blob)}，应为 960018（EPD6BIN 单帧）")

    device = find_device(args.ip, args.http_port)
    upload_bytes(device, blob, args.name or f"{time.strftime('%Y%m%d-%H%M%S')}-{path.stem[:24]}.bin")
    return 0


def cmd_send(args: argparse.Namespace) -> int:
    image = Path(args.image)
    if not image.exists():
        raise SystemExit(f"找不到 {image}")

    started = time.monotonic()
    im = conv.load_and_fit(image, 1200, 1600, args.fit, args.rotate, args.flip)
    rgb = np.asarray(im, dtype=np.float32)
    indices = conv.dither(rgb, args.dither, args.strength)
    frame = conv.pack(indices)
    blob = conv.MAGIC + struct.pack("<HHHI", 1200, 1600, 1, 150) + frame
    elapsed = time.monotonic() - started

    print(f"转换完成：{len(blob)} B（帧 {len(frame)} B），用时 {elapsed:.2f} s")

    device = find_device(args.ip, args.http_port)
    default_name = f"{time.strftime('%Y%m%d-%H%M%S')}-{image.stem[:24]}.bin"
    upload_bytes(device, blob, args.name or default_name)
    return 0


# ---------------------------------------------------------------- CLI


def main() -> int:
    parser = argparse.ArgumentParser(
        description="GDEP133C02 局域网刷屏固件参考客户端",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="设备地址默认自动广播发现；也可用 --ip 指定。")
    parser.add_argument("--ip", help="设备 IP（跳过广播发现）")
    parser.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT,
                        help=f"HTTP 端口（默认 {DEFAULT_HTTP_PORT}，联调 mock 时用 8080）")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("discover", help="广播发现设备")
    p.add_argument("--timeout", type=float, default=2.0)
    p.set_defaults(func=cmd_discover)

    p = sub.add_parser("status", help="读取设备状态 JSON")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("history", help="列出 SD 卡归档")
    p.set_defaults(func=cmd_history)

    p = sub.add_parser("show", help="重新显示某个归档")
    p.add_argument("file")
    p.set_defaults(func=cmd_show)

    p = sub.add_parser("clear", help="整屏单色")
    p.add_argument("color", choices=["white", "black", "red", "yellow", "blue", "green"])
    p.set_defaults(func=cmd_clear)

    p = sub.add_parser("upload", help="上传现成的 EPD6BIN")
    p.add_argument("file")
    p.add_argument("--name", help="归档文件名（默认用当前时间）")
    p.set_defaults(func=cmd_upload)

    p = sub.add_parser("send", help="转换一张图片并上传")
    p.add_argument("image")
    p.add_argument("--name", help="归档文件名")
    p.add_argument("--fit", default="cover", choices=["cover", "contain", "stretch"])
    p.add_argument("--rotate", type=int, default=0, help="顺时针 0/90/180/270")
    p.add_argument("--flip", default="", help="h=水平 v=垂直")
    p.add_argument("--dither", default="fs-ser",
                   choices=["fs", "fs-ser", "atkinson", "stucki", "jjn", "none"])
    p.add_argument("--strength", type=float, default=1.0)
    p.set_defaults(func=cmd_send)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
