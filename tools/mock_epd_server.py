#!/usr/bin/env python3
"""本地假设备：按 firmware/epd_server 的 JSON 契约提供 HTTP 接口，用于上位机联调。

没有板子的时候，用它验证 Swift 界面 / Python 客户端的请求格式与解析：

    ./.venv/bin/python tools/mock_epd_server.py --port 8080
    ./.venv/bin/python tools/mock_epd_server.py --port 8080 --discovery   # 顺带应答 UDP 发现

然后把上位机指向 http://127.0.0.1:8080：
    host/EDPHost/.build/release/EDPHost --api-selftest http://127.0.0.1:8080
    ./.venv/bin/python host/epd_client.py --ip 127.0.0.1 status   # 需把端口改成 8080

它不刷屏、不写盘，只在内存里记录状态，返回的 JSON 字段与固件一致。
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import struct
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAGIC = b"EPD6BIN\0"
FRAME_BYTES = 960_000
TOTAL_BYTES = 18 + FRAME_BYTES
NAME_RE = re.compile(r"^[A-Za-z0-9._\-]{1,63}\.bin$")


class State:
    def __init__(self, port: int):
        self.lock = threading.Lock()
        self.port = port
        self.started = time.time()
        self.history: dict[str, bytes] = {}
        self.current_name = ""
        self.current_crc = 0
        self.current_saved_at = 0
        self.current_bytes = 0
        self.refresh_count = 0
        self.uploads_ok = 0
        self.uploads_fail = 0
        self.last_upload_ms = 0
        self.last_upload_bytes = 0

    def pick_name(self, requested: str) -> str:
        base = requested if NAME_RE.match(requested or "") else time.strftime("%Y%m%d-%H%M%S.bin")
        if base not in self.history:
            return base
        stem = base[:-4]
        for i in range(1, 1000):
            candidate = f"{stem}-{i}.bin"
            if candidate not in self.history:
                return candidate
        return f"img-{int(time.time())}.bin"

    def status(self) -> dict:
        return {
            "ok": True,
            "device": "GDEP133C02",
            "firmware": "1.0.0-mock",
            "hostname": "epd-mock",
            "ip": "127.0.0.1",
            "mac": "02:00:00:00:00:01",
            "uptime_s": int(time.time() - self.started),
            "http_port": self.port,
            "discovery_port": 3232,
            "panel": {"width": 1200, "height": 1600, "colors": 6,
                      "frame_bytes": FRAME_BYTES, "ready": True},
            "sd": {"mounted": True,
                   "total_bytes": 8_000_000_000,
                   "free_bytes": 7_000_000_000},
            "display": {"busy": False,
                        "refresh_count": self.refresh_count,
                        "last_refresh_ms": 15000,
                        "last_refresh_uptime_s": int(time.time() - self.started),
                        "last_error": "ESP_OK",
                        "current_name": self.current_name,
                        "current_crc": self.current_crc,
                        "current_saved_at": self.current_saved_at,
                        "current_bytes": self.current_bytes},
            "upload": {"active": False, "received": 0, "total": 0,
                       "ok_count": self.uploads_ok, "fail_count": self.uploads_fail,
                       "last_bytes": self.last_upload_bytes,
                       "last_ms": self.last_upload_ms},
        }


class Handler(BaseHTTPRequestHandler):
    server_version = "MockEPD/1.0"
    state: State = None  # 由 main() 注入

    # ---------------- 工具 ----------------

    def _send(self, status: int, payload: dict, extra: dict | None = None) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status: int, code: str, message: str) -> None:
        self._send(status, {"ok": False, "error": code, "message": message})

    def _query(self) -> dict:
        return {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}

    def log_message(self, fmt: str, *args) -> None:  # 安静一点
        print(f"[mock] {self.address_string()} {fmt % args}")

    # ---------------- GET ----------------

    def do_GET(self) -> None:
        path = urlparse(self.path).path

        if path == "/api/v1/status":
            self._send(200, self.state.status())
            return

        if path == "/api/v1/history":
            with self.state.lock:
                files = [{"name": name, "bytes": len(blob)}
                         for name, blob in sorted(self.state.history.items(), reverse=True)]
                current = self.state.current_name
            self._send(200, {"ok": True, "files": files, "count": len(files), "current": current})
            return

        if path == "/api/v1/image":
            name = self._query().get("file", "")
            if not NAME_RE.match(name):
                self._error(400, "bad_file", "缺少或非法的 file 参数")
                return
            blob = self.state.history.get(name)
            if blob is None:
                self._error(404, "not_found", "文件不存在")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(blob)))
            self.end_headers()
            self.wfile.write(blob)
            return

        if path == "/":
            body = (b"<!doctype html><meta charset=utf-8><h1>Mock EPD</h1>"
                    b"<p>mock server for host integration tests</p>")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self._error(404, "not_found", "未知路径")

    # ---------------- POST ----------------

    def do_POST(self) -> None:
        path = urlparse(self.path).path

        if path == "/api/v1/image":
            self._handle_upload()
            return

        if path == "/api/v1/display":
            name = self._query().get("file", "")
            blob = self.state.history.get(name)
            if blob is None:
                self._error(404, "not_found", "文件不存在或不合法")
                return
            with self.state.lock:
                self.state.current_name = name
                self.state.current_bytes = len(blob)
                self.state.refresh_count += 1
            self._send(202, {"ok": True, "file": name, "queued": True})
            return

        if path == "/api/v1/clear":
            color = self._query().get("color", "white")
            if color not in {"white", "black", "red", "yellow", "blue", "green"}:
                self._error(400, "bad_color", "颜色取值不合法")
                return
            with self.state.lock:
                self.state.current_name = f"solid-{color}"
                self.state.current_bytes = FRAME_BYTES
                self.state.refresh_count += 1
            self._send(202, {"ok": True, "color": color, "queued": True})
            return

        self._error(404, "not_found", "未知路径")

    def _handle_upload(self) -> None:
        started = time.time()
        total = int(self.headers.get("Content-Length", "0"))

        if total != TOTAL_BYTES:
            self._error(400, "bad_length",
                        f"长度应为 {TOTAL_BYTES} 字节（EPD6BIN 单帧），收到 {total}")
            return

        blob = self.rfile.read(total)
        if len(blob) != total:
            self._error(400, "recv_failed", "连接中断")
            return

        if blob[:8] != MAGIC:
            self._error(400, "bad_format", "不是合法的单帧 EPD6BIN（检查尺寸/帧数）")
            return

        width, height, frames, _interval = struct.unpack("<HHHI", blob[8:18])
        if (width, height, frames) != (1200, 1600, 1):
            self._error(400, "bad_format",
                        f"尺寸/帧数不符：{width}x{height} x{frames}")
            return

        crc = zlib.crc32(blob) & 0xFFFFFFFF
        header_crc = self.headers.get("X-Crc32", "")
        if header_crc:
            try:
                if int(header_crc, 16) != crc:
                    self._error(400, "crc_mismatch", "CRC 校验失败，传输可能丢字节")
                    return
            except ValueError:
                self._error(400, "crc_mismatch", "CRC 头格式不对")
                return

        requested = self.headers.get("X-Image-Name", "")
        saved_at = 0
        try:
            saved_at = int(self.headers.get("X-Timestamp", "0"))
        except ValueError:
            saved_at = 0
        if saved_at < 1_600_000_000:
            saved_at = int(time.time())

        with self.state.lock:
            name = self.state.pick_name(requested)
            self.state.history[name] = blob
            self.state.current_name = name
            self.state.current_crc = crc
            self.state.current_saved_at = saved_at
            self.state.current_bytes = len(blob)
            self.state.refresh_count += 1
            self.state.uploads_ok += 1
            self.state.last_upload_ms = int((time.time() - started) * 1000)
            self.state.last_upload_bytes = len(blob)

        self._send(200, {"ok": True, "file": name, "bytes": len(blob),
                         "crc32": crc, "saved_at": saved_at,
                         "received_ms": int((time.time() - started) * 1000),
                         "queued": True})


def discovery_responder(port: int, stop: threading.Event) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", 3232))
    sock.settimeout(0.5)

    while not stop.is_set():
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            break

        if len(data) < 12 or data[:8] != b"EPD6DISC" or data[8] != 1 or data[9] != 1:
            continue

        host = b"epd-mock"
        reply = (b"EPD6DISC" + bytes([1, 2]) + data[10:12]
                 + socket.inet_aton("127.0.0.1")
                 + bytes.fromhex("020000000001")
                 + struct.pack(">H", port)
                 + bytes([len(host)]) + host)
        sock.sendto(reply, addr)

    sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="本地假设备（上位机联调用）")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--discovery", action="store_true",
                        help="同时在 UDP 3232 应答发现（应答里的 IP 是 127.0.0.1）")
    args = parser.parse_args()

    state = State(args.port)
    Handler.state = state

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"mock EPD 已启动：http://127.0.0.1:{args.port}/api/v1/status")

    stop = threading.Event()
    if args.discovery:
        threading.Thread(target=discovery_responder, args=(args.port, stop),
                         daemon=True).start()
        print("UDP 发现应答已开启：3232")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
