#!/usr/bin/env python3
"""局域网发现工具：广播查询 ESP32 设备，列出各自的 IP / MAC / 主机名。

配合 firmware/wifi_check 的 UDP 发现服务（默认端口 3232）：

    QUERY 12 B : b"EPD6DISC" + ver(1) + type(0x01) + nonce(u16 BE)
    REPLY      : b"EPD6DISC" + ver(1) + type(0x02) + nonce(u16 BE)
                 + ip(u32 BE) + mac(6) + http_port(u16 BE) + hostlen(1) + hostname

示例：
    ./.venv/bin/python host/discover.py                 # 广播扫描整个网段
    ./.venv/bin/python host/discover.py --json          # JSON 输出，给其他程序用
    ./.venv/bin/python host/discover.py --target 192.168.1.50   # 单播自测
    ./.venv/bin/python host/discover.py --broadcast 192.168.1.255

退出码：0 = 发现设备，1 = 未发现，2 = 参数/网络错误。
"""

from __future__ import annotations

import argparse
import json
import re
import secrets
import socket
import struct
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field

MAGIC = b"EPD6DISC"
VERSION = 1
TYPE_QUERY = 0x01
TYPE_REPLY = 0x02
DEFAULT_PORT = 3232

# 8s magic, B version, B type, H nonce
PROBE = struct.Struct(">8sBBH")
# 8s magic, B version, B type, H nonce, I ip, 6s mac, H http_port, B hostlen
REPLY_HEAD = struct.Struct(">8sBBH I 6s H B")


@dataclass
class Device:
    ip: str
    mac: str
    hostname: str
    http_port: int
    source: str = field(default="", compare=False)


def parse_reply(data: bytes, addr: tuple[str, int], nonce: int) -> Device | None:
    """校验并解析一个回包；格式/魔数/nonce 不符返回 None。"""
    if len(data) < REPLY_HEAD.size:
        return None

    magic, version, type_, reply_nonce, ip_raw, mac_raw, http_port, host_len = (
        REPLY_HEAD.unpack_from(data)
    )
    if magic != MAGIC or version != VERSION or type_ != TYPE_REPLY:
        return None
    if reply_nonce != nonce:
        return None
    if len(data) < REPLY_HEAD.size + host_len:
        return None

    hostname = data[REPLY_HEAD.size:REPLY_HEAD.size + host_len].decode("utf-8", "replace")
    ip = socket.inet_ntoa(struct.pack(">I", ip_raw))
    mac = ":".join(f"{b:02x}" for b in mac_raw)
    return Device(ip=ip, mac=mac, hostname=hostname, http_port=http_port,
                  source=f"{addr[0]}:{addr[1]}")


def interface_broadcasts() -> list[str]:
    """收集子网定向广播地址：优先 macOS/Linux 的 ifconfig，其次 Linux 的 ip。

    两样都失败（如 Windows）时返回空列表，调用方仍会发 255.255.255.255。
    """
    addrs: list[str] = []
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True,
                             timeout=3).stdout
        addrs += re.findall(r"\bbroadcast\s+(\d{1,3}(?:\.\d{1,3}){3})", out)
    except (OSError, subprocess.SubprocessError):
        pass

    if not addrs:
        try:
            out = subprocess.run(["ip", "-4", "addr", "show"], capture_output=True,
                                 text=True, timeout=3).stdout
            addrs += re.findall(r"\bbrd\s+(\d{1,3}(?:\.\d{1,3}){3})", out)
        except (OSError, subprocess.SubprocessError):
            pass

    return addrs


def default_targets() -> list[str]:
    targets = ["255.255.255.255", *interface_broadcasts()]
    # 去重并保持顺序
    return list(dict.fromkeys(targets))


def scan(targets: list[str], port: int, timeout: float, retries: int,
         interval: float) -> dict[str, Device]:
    nonce = secrets.randbits(16)
    probe = PROBE.pack(MAGIC, VERSION, TYPE_QUERY, nonce)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", 0))
    sock.settimeout(0.2)

    found: dict[str, Device] = {}
    deadline = time.monotonic() + timeout
    next_send = 0.0
    sent = 0

    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if sent < retries and now >= next_send:
                for target in targets:
                    try:
                        sock.sendto(probe, (target, port))
                    except OSError:
                        pass
                sent += 1
                next_send = now + interval

            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue

            dev = parse_reply(data, addr, nonce)
            if dev is None:
                continue
            key = dev.mac or dev.ip
            found.setdefault(key, dev)
    finally:
        sock.close()

    return found


def print_table(devices: list[Device]) -> None:
    rows = [
        (d.ip or "-", d.mac or "-", d.hostname or "-",
         str(d.http_port) if d.http_port else "-")
        for d in devices
    ]
    headers = ("IP", "MAC", "主机名", "HTTP")
    widths = [max(len(headers[i]), *(len(r[i]) for r in rows)) for i in range(4)]

    def fmt(row: tuple[str, ...]) -> str:
        return "  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip()

    print(fmt(headers))
    print(fmt(tuple("-" * w for w in widths)))
    for row in rows:
        print(fmt(row))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="广播查询 ESP32 设备（UDP 发现协议），打印 IP / MAC / 主机名。")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"UDP 端口（默认 {DEFAULT_PORT}）")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="等待回包的秒数（默认 2.0）")
    parser.add_argument("--retries", type=int, default=3,
                        help="探测包发送次数（默认 3）")
    parser.add_argument("--interval", type=float, default=0.4,
                        help="两次探测之间的秒数（默认 0.4）")
    parser.add_argument("--target", action="append", default=None,
                        metavar="IP",
                        help="只对这些地址发单播查询（可重复），不给则广播")
    parser.add_argument("--broadcast", action="append", default=None,
                        metavar="IP",
                        help="指定广播地址（可重复），默认自动收集网段广播")
    parser.add_argument("--json", action="store_true",
                        help="以 JSON 输出（仅 stdout 输出 JSON）")
    args = parser.parse_args()

    if args.target:
        targets = list(dict.fromkeys(args.target))
    elif args.broadcast:
        targets = list(dict.fromkeys(args.broadcast))
    else:
        targets = default_targets()

    if not args.json:
        print(f"扫描目标 {', '.join(targets)}，端口 {args.port}，"
              f"超时 {args.timeout:.1f} s ...")

    try:
        found = scan(targets, args.port, args.timeout, args.retries, args.interval)
    except OSError as exc:
        print(f"网络错误：{exc}", file=sys.stderr)
        return 2

    devices = list(found.values())

    if args.json:
        print(json.dumps([asdict(d) for d in devices], ensure_ascii=False, indent=2))
    elif devices:
        print(f"\n发现 {len(devices)} 台设备：\n")
        print_table(devices)
    else:
        print("未发现设备。排查：设备与上位机是否同一网段、端口 "
              f"{args.port} 是否被防火墙拦截、设备是否已连上 WiFi。",
              file=sys.stderr)

    return 0 if devices else 1


if __name__ == "__main__":
    sys.exit(main())
