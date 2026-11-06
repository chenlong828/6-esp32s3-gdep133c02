#!/usr/bin/env python3
"""
SD card bridge host tool for the ESP32-133C02-IL board.

Talks to firmware/sd_bridge over the board's USB-Serial/JTAG port using a
simple framed protocol (magic "SDBG", CRC32, request/response/data frames).

Examples
--------
    # 查看卡信息
    python host/sd_bridge.py info

    # 列根目录
    python host/sd_bridge.py ls -l /

    # 把整张卡递归拉到本地（默认不覆盖已有文件）
    python host/sd_bridge.py pull / backups/sdcard-20261106

    # 上传一个文件 / 目录
    python host/sd_bridge.py put photo.bin /photo.bin --overwrite
    python host/sd_bridge.py push ./pics /pics --overwrite

    # 单文件校验/读取
    python host/sd_bridge.py crc /images.bin
    python host/sd_bridge.py get /images.bin ./images.bin

Remote paths are relative to the SD card root; "/" is the card root.
Filenames are UTF-8.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator, Optional

try:
    import serial
except ImportError as exc:  # pragma: no cover
    raise SystemExit("pyserial is required: ./.venv/bin/python host/sd_bridge.py ...") from exc


# ---------------------------------------------------------------- protocol

MAGIC = b"SDBG"
HEADER = struct.Struct("<4sBBHII")  # magic, type, flags, seq, len, crc
FRAME_REQ = 1
FRAME_RSP = 2
FRAME_DATA = 3
MAX_PAYLOAD = 32768

DEFAULT_TIMEOUT = 10.0
DEFAULT_CHUNK = 16384


class BridgeError(RuntimeError):
    pass


class BridgeTimeout(BridgeError):
    pass


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes


@dataclass
class Entry:
    name: str
    is_dir: bool
    size: int
    mtime: int


def encode_frame(frame_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = HEADER.pack(MAGIC, frame_type, 0, seq, len(payload), 0)
    crc = zlib.crc32(header[4:12] + payload) & 0xFFFFFFFF
    header = HEADER.pack(MAGIC, frame_type, 0, seq, len(payload), crc)
    return header + payload


def decode_response(payload: bytes) -> tuple[str, object, list[str]]:
    """Return (kind, header, extra_lines)."""
    text = payload.decode("utf-8", "replace")
    lines = text.splitlines()
    if not lines:
        raise BridgeError("empty response")
    first = lines[0].strip()

    if first == "OK" or first.startswith("OK "):
        return "OK", first[3:].strip(), lines[1:]
    if first.startswith("ERR"):
        parts = first.split(" ", 2)
        try:
            code = int(parts[1], 0)
        except (IndexError, ValueError):
            code = -1
        message = parts[2] if len(parts) > 2 else ""
        return "ERR", (code, message), lines[1:]
    if first.startswith("ACK "):
        return "ACK", int(first.split()[1], 10), lines[1:]
    if first.startswith("DONE "):
        parts = first.split()
        return "DONE", (int(parts[1], 10), int(parts[2], 0)), lines[1:]
    raise BridgeError(f"unexpected response: {first!r}")


def parse_kv(lines: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in lines:
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def remote_join(parent: str, name: str) -> str:
    if parent in ("", "/"):
        return "/" + name
    return parent.rstrip("/") + "/" + name


def quote_arg(value: str) -> str:
    """Quote one protocol argument. The firmware understands double quotes."""
    if value == "":
        return '""'
    if all(ch not in ' \t\r\n"\\' for ch in value):
        return value
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return '"' + escaped + '"'


def unescape_name(value: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(value):
        ch = value[i]
        if ch == "\\" and i + 1 < len(value):
            nxt = value[i + 1]
            out.append({"n": "\n", "r": "\r", "t": "\t", "\\": "\\"}.get(nxt, nxt))
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def human_bytes(n: int) -> str:
    value = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{n} B"


def file_crc32(path: Path, chunk: int = 1 << 20) -> int:
    crc = 0
    with open(path, "rb") as handle:
        while True:
            block = handle.read(chunk)
            if not block:
                break
            crc = zlib.crc32(block, crc)
    return crc & 0xFFFFFFFF


def safe_join(root: Path, name: str) -> Path:
    if name in ("", ".", "..") or "/" in name or "\\" in name or "\x00" in name:
        raise BridgeError(f"unsafe file name on card: {name!r}")
    candidate = root / name
    root_abs = root.resolve()
    try:
        candidate.resolve().relative_to(root_abs)
    except ValueError as exc:
        raise BridgeError(f"file name escapes target directory: {name!r}") from exc
    return candidate


def find_port() -> str:
    for pattern in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        ports = sorted(p for p in glob.glob(pattern) if os.path.exists(p))
        if len(ports) == 1:
            return ports[0]
        if len(ports) > 1:
            raise BridgeError("multiple serial ports found, use --port: " + ", ".join(ports))
    raise BridgeError("no USB serial port found, use --port /dev/cu.usbmodemXXXX")


class Progress:
    def __init__(self, label: str, enabled: bool = True) -> None:
        self.label = label
        self.enabled = enabled and sys.stderr.isatty()
        self.last_time = 0.0
        self.last_done = 0

    def __call__(self, done: int, total: int) -> None:
        if not self.enabled:
            return
        now = time.monotonic()
        if done != total and now - self.last_time < 0.5 and done - self.last_done < (1 << 20):
            return
        self.last_time = now
        self.last_done = done
        pct = (done * 100.0 / total) if total else 100.0
        line = f"\r  {self.label}: {pct:5.1f}%  {human_bytes(done)} / {human_bytes(total)}"
        print(line, end="", file=sys.stderr, flush=True)
        if done == total:
            print(file=sys.stderr, flush=True)


class Bridge:
    def __init__(self, port: Optional[str] = None, timeout: float = DEFAULT_TIMEOUT,
                 debug: bool = False) -> None:
        self.port = port or find_port()
        self.timeout = timeout
        self.debug = debug
        self.buf = bytearray()
        self.seq = 0
        self.ser: Optional[serial.Serial] = None
        self._open()

    # ---------------------------------------------------------- lifecycle

    def _open(self) -> None:
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=0.1, write_timeout=10.0)
        except serial.SerialException as exc:
            raise BridgeError(f"cannot open {self.port}: {exc}") from exc

        try:
            # Avoid asserting DTR/RTS: some USB-Serial/JTAG setups reset on it.
            self.ser.dtr = False
            self.ser.rts = False
        except (OSError, serial.SerialException):
            pass

        self.ser.reset_input_buffer()
        self._handshake()

    def _handshake(self) -> None:
        deadline = time.monotonic() + max(3.0, self.timeout)
        last_error: Optional[Exception] = None
        while time.monotonic() < deadline:
            try:
                payload = self.request("PING", timeout=1.0)
                kind, _, _ = decode_response(payload)
                if kind == "OK":
                    return
            except (BridgeError, serial.SerialException) as exc:
                last_error = exc
            time.sleep(0.1)
        raise BridgeError(
            f"no bridge firmware answered on {self.port}: {last_error}. "
            "Flash firmware/sd_bridge and make sure no monitor is holding the port."
        )

    def close(self) -> None:
        if self.ser is not None:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.ser = None

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # ---------------------------------------------------------- transport

    def _next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFFFF
        if self.seq == 0:
            self.seq = 1
        return self.seq

    def _read_more(self, deadline: float) -> bool:
        assert self.ser is not None
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        try:
            self.ser.timeout = min(0.2, remaining)
            data = self.ser.read(4096)
        except serial.SerialException as exc:
            raise BridgeError(f"serial read failed: {exc}") from exc
        if not data:
            return False
        self.buf.extend(data)
        if self.debug:
            print(f"< {data!r}", file=sys.stderr)
        return True

    def _wait_for_more(self, deadline: float, message: str) -> None:
        """Wait for more bytes; only fail when the overall deadline expires."""
        if self._read_more(deadline):
            return
        if time.monotonic() >= deadline:
            raise BridgeTimeout(message)

    def recv_frame(self, deadline: float) -> Frame:
        while True:
            index = self.buf.find(MAGIC)
            if index < 0:
                if len(self.buf) > len(MAGIC) - 1:
                    del self.buf[: -(len(MAGIC) - 1)]
                self._wait_for_more(deadline, "timeout waiting for frame")
                continue

            if index:
                del self.buf[:index]

            if len(self.buf) < HEADER.size:
                self._wait_for_more(deadline, "timeout reading frame header")
                continue

            magic, frame_type, flags, seq, length, crc = HEADER.unpack_from(self.buf)
            if (magic != MAGIC or flags != 0 or length > MAX_PAYLOAD
                    or frame_type not in (FRAME_RSP, FRAME_DATA)):
                del self.buf[0]
                continue

            if len(self.buf) < HEADER.size + length:
                self._wait_for_more(deadline, "timeout reading frame payload")
                continue

            payload = bytes(self.buf[HEADER.size:HEADER.size + length])
            calc = zlib.crc32(bytes(self.buf[4:12]) + payload) & 0xFFFFFFFF
            if calc != crc:
                if self.debug:
                    print(f"! bad crc (got {crc:08x}, want {calc:08x})", file=sys.stderr)
                del self.buf[0]
                continue

            del self.buf[:HEADER.size + length]
            return Frame(frame_type, seq, payload)

    def send_frame(self, frame_type: int, seq: int, payload: bytes = b"") -> None:
        if self.ser is None:
            raise BridgeError("serial port is closed")
        data = encode_frame(frame_type, seq, payload)
        if self.debug:
            print(f"> {frame_type} seq={seq} len={len(payload)} {payload[:80]!r}",
                  file=sys.stderr)
        try:
            self.ser.write(data)
            self.ser.flush()
        except serial.SerialException as exc:
            raise BridgeError(f"serial write failed: {exc}") from exc

    def request(self, command: str, timeout: Optional[float] = None) -> bytes:
        seq = self._next_seq()
        self.send_frame(FRAME_REQ, seq, command.encode("utf-8"))
        return self.wait_rsp(seq, timeout if timeout is not None else self.timeout)

    def wait_rsp(self, seq: int, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            frame = self.recv_frame(deadline)
            if frame.type == FRAME_RSP and frame.seq == seq:
                return frame.payload

    # ------------------------------------------------------------ commands

    def info(self) -> dict[str, str]:
        kind, _, lines = decode_response(self.request("INFO"))
        if kind != "OK":
            raise BridgeError(f"INFO failed: {kind}")
        return parse_kv(lines)

    def ls(self, path: str = "/") -> Iterator[Entry]:
        start = 0
        while True:
            payload = self.request(f"LS {quote_arg(path)} {start} 128")
            kind, header, lines = decode_response(payload)
            if kind != "OK":
                code, message = header  # type: ignore[misc]
                raise BridgeError(f"LS {path}: {code} {message}")

            parts = str(header).split()
            if len(parts) != 2:
                raise BridgeError(f"bad LS header: {header!r}")
            total, returned = int(parts[0]), int(parts[1])

            for line in lines:
                fields = line.split("\t")
                if len(fields) != 4:
                    raise BridgeError(f"bad LS line: {line!r}")
                entry_type, name, size, mtime = fields
                yield Entry(unescape_name(name), entry_type == "d", int(size), int(mtime))

            start += returned
            if returned == 0 or start >= total:
                break

    def stat(self, path: str) -> dict[str, str]:
        kind, _, lines = decode_response(self.request(f"STAT {quote_arg(path)}"))
        if kind != "OK":
            raise BridgeError(f"STAT {path} failed: {kind}")
        return parse_kv(lines)

    def crc(self, path: str) -> tuple[int, int]:
        kind, _, lines = decode_response(self.request(f"CRC {quote_arg(path)}", timeout=120.0))
        if kind != "OK":
            raise BridgeError(f"CRC {path} failed: {kind}")
        values = parse_kv(lines)
        return int(values["size"]), int(values["crc"], 0)

    def mkdir(self, path: str) -> None:
        kind, header, _ = decode_response(self.request(f"MKDIR {quote_arg(path)}"))
        if kind != "OK":
            raise BridgeError(f"MKDIR {path}: {header}")

    def rm(self, path: str) -> None:
        kind, header, _ = decode_response(self.request(f"RM {quote_arg(path)}"))
        if kind != "OK":
            raise BridgeError(f"RM {path}: {header}")

    def rmdir(self, path: str) -> None:
        kind, header, _ = decode_response(self.request(f"RMDIR {quote_arg(path)}"))
        if kind != "OK":
            raise BridgeError(f"RMDIR {path}: {header}")

    def mv(self, src: str, dst: str, overwrite: bool = False) -> None:
        command = f"MV {quote_arg(src)} {quote_arg(dst)}"
        if overwrite:
            command += " 1"
        kind, header, _ = decode_response(self.request(command))
        if kind != "OK":
            raise BridgeError(f"MV {src} {dst}: {header}")

    def mount(self) -> dict[str, str]:
        kind, _, lines = decode_response(self.request("MOUNT", timeout=30.0))
        if kind != "OK":
            raise BridgeError(f"MOUNT failed: {kind}")
        return parse_kv(lines)

    def unmount(self) -> dict[str, str]:
        kind, _, lines = decode_response(self.request("UNMOUNT", timeout=30.0))
        if kind != "OK":
            raise BridgeError(f"UNMOUNT failed: {kind}")
        return parse_kv(lines)

    def sync(self) -> None:
        kind, header, _ = decode_response(self.request("SYNC"))
        if kind != "OK":
            raise BridgeError(f"SYNC failed: {header}")

    # ---------------------------------------------------------- file get/put

    def get(self, remote: str, local: Path, overwrite: bool = False,
            progress: Optional[Callable[[int, int], None]] = None) -> tuple[int, int, int]:
        """Download one file.  Returns (size, crc32, mtime)."""
        local = Path(local)
        if local.exists() and not overwrite:
            raise BridgeError(f"local file exists: {local} (use --overwrite)")

        seq = self._next_seq()
        self.send_frame(FRAME_REQ, seq, f"GET {quote_arg(remote)}".encode("utf-8"))
        payload = self.wait_rsp(seq, self.timeout)
        kind, _, lines = decode_response(payload)
        if kind != "OK":
            code, message = _error_from(kind, payload)  # type: ignore[misc]
            raise BridgeError(f"GET {remote}: {code} {message}")

        values = parse_kv(lines)
        size = int(values["size"])
        offset = int(values.get("offset", "0"))
        if offset != 0:
            raise BridgeError(f"GET {remote}: unexpected offset {offset}")

        local.parent.mkdir(parents=True, exist_ok=True)
        temp = local.with_name(local.name + ".part")
        received = 0
        crc = 0
        try:
            with open(temp, "wb") as handle:
                while True:
                    frame = self.recv_frame(time.monotonic() + self.timeout)
                    if frame.seq != seq:
                        continue
                    if frame.type == FRAME_DATA:
                        received += len(frame.payload)
                        if received > size:
                            raise BridgeError(f"GET {remote}: device sent more than {size} bytes")
                        handle.write(frame.payload)
                        crc = zlib.crc32(frame.payload, crc)
                        if progress:
                            progress(received, size)
                        continue

                    kind2, header2, _ = decode_response(frame.payload)
                    if kind2 == "DONE":
                        done, done_crc = header2  # type: ignore[misc]
                        if done != received or done_crc != (crc & 0xFFFFFFFF):
                            raise BridgeError(
                                f"GET {remote}: done={done} crc={done_crc:08x}, "
                                f"received={received} crc={crc & 0xFFFFFFFF:08x}"
                            )
                        break
                    if kind2 == "ERR":
                        code, message = header2  # type: ignore[misc]
                        raise BridgeError(f"GET {remote}: device error {code}: {message}")
                    raise BridgeError(f"GET {remote}: unexpected frame {kind2}")

            if received != size:
                raise BridgeError(f"GET {remote}: expected {size} bytes, got {received}")
            if local.exists() and not overwrite:
                raise BridgeError(f"local file appeared during transfer: {local} (use --overwrite)")
            os.replace(temp, local)

            stat_result = None
            try:
                stat_result = self.stat(remote)
                mtime = int(stat_result.get("mtime", "0"))
                if mtime > 0:
                    os.utime(local, (mtime, mtime))
            except (BridgeError, ValueError):
                mtime = 0
            return size, crc & 0xFFFFFFFF, mtime
        except Exception:
            try:
                temp.unlink()
            except OSError:
                pass
            raise

    def put(self, local: Path, remote: str, overwrite: bool = False,
            progress: Optional[Callable[[int, int], None]] = None) -> tuple[int, int]:
        """Upload one file.  Returns (size, crc32)."""
        local = Path(local)
        if not local.is_file():
            raise BridgeError(f"local file not found: {local}")

        size = local.stat().st_size
        if size > 0x7FFFFFFF:
            raise BridgeError(f"file too large for this protocol: {size} bytes")
        crc = file_crc32(local)

        command = f"PUT {quote_arg(remote)} {size} {crc}"
        if overwrite:
            command += " 1"

        seq = self._next_seq()
        self.send_frame(FRAME_REQ, seq, command.encode("utf-8"))
        payload = self.wait_rsp(seq, max(self.timeout, 30.0))
        kind, _, lines = decode_response(payload)
        if kind != "OK":
            code, message = _error_from(kind, payload)  # type: ignore[misc]
            raise BridgeError(f"PUT {remote}: {code} {message}")

        values = parse_kv(lines)
        chunk = int(values.get("chunk", str(DEFAULT_CHUNK)))

        received = 0
        last_kind = "OK"
        last_header: object = None
        try:
            if size == 0:
                frame = self.wait_rsp(seq, max(self.timeout, 30.0))
                kind2, header2, _ = decode_response(frame)
                if kind2 == "ERR":
                    code, message = header2  # type: ignore[misc]
                    raise BridgeError(f"PUT {remote}: device error {code}: {message}")
                if kind2 != "DONE":
                    raise BridgeError(f"PUT {remote}: expected DONE, got {kind2}")
                done, done_crc = header2  # type: ignore[misc]
                if done != 0 or done_crc != 0:
                    raise BridgeError(f"PUT {remote}: bad empty-file DONE")
                return 0, 0

            with open(local, "rb") as handle:
                while True:
                    block = handle.read(chunk)
                    if not block:
                        break
                    data_seq = self._next_seq()
                    self.send_frame(FRAME_DATA, data_seq, block)
                    frame = self.wait_rsp(data_seq, max(self.timeout, 30.0))
                    kind2, header2, _ = decode_response(frame)
                    last_kind, last_header = kind2, header2

                    if kind2 == "ACK":
                        ack = int(header2)
                        if ack != received + len(block):
                            raise BridgeError(
                                f"PUT {remote}: ack {ack}, expected {received + len(block)}"
                            )
                    elif kind2 == "DONE":
                        done, done_crc = header2  # type: ignore[misc]
                        received += len(block)
                        if done != size or done_crc != crc:
                            raise BridgeError(
                                f"PUT {remote}: done={done} crc={done_crc:08x}, "
                                f"expected {size} crc={crc:08x}"
                            )
                        if progress:
                            progress(size, size)
                        return size, crc
                    elif kind2 == "ERR":
                        code, message = header2  # type: ignore[misc]
                        raise BridgeError(f"PUT {remote}: device error {code}: {message}")
                    else:
                        raise BridgeError(f"PUT {remote}: unexpected frame {kind2}")

                    received += len(block)
                    if progress:
                        progress(received, size)
        except Exception:
            try:
                self.request("ABORT", timeout=2.0)
            except (BridgeError, serial.SerialException):
                pass
            raise

        raise BridgeError(
            f"PUT {remote}: transfer ended unexpectedly ({last_kind} {last_header!r})"
        )


def _error_from(kind: str, payload: bytes) -> tuple[object, str]:
    _, header, _ = decode_response(payload)
    if kind == "ERR" and isinstance(header, tuple):
        return header
    return "?", "unexpected response"


# ------------------------------------------------------------------- pull/push


def pull_tree(bridge: Bridge, remote: str, local: Path, args: argparse.Namespace,
              manifest: dict) -> None:
    try:
        entries = list(bridge.ls(remote))
    except BridgeError as exc:
        if not args.skip_errors:
            raise
        print(f"  warn {remote}: {exc}", file=sys.stderr)
        manifest.setdefault("errors", []).append({"remote": remote, "error": str(exc)})
        return

    if not args.dry_run:
        local.mkdir(parents=True, exist_ok=True)

    for entry in entries:
        remote_path = remote_join(remote, entry.name)
        local_path = safe_join(local, entry.name)

        if entry.is_dir:
            print(f"  dir  {remote_path}/")
            if not args.dry_run:
                pull_tree(bridge, remote_path, local_path, args, manifest)
                if entry.mtime > 0:
                    try:
                        os.utime(local_path, (entry.mtime, entry.mtime))
                    except OSError:
                        pass
            continue

        if local_path.exists():
            if args.skip_existing:
                print(f"  skip {remote_path} (exists)")
                manifest["files"].append({
                    "remote": remote_path, "local": str(local_path),
                    "size": entry.size, "mtime": entry.mtime, "status": "skipped",
                })
                continue
            if not args.overwrite:
                if local_path.is_file() and local_path.stat().st_size == entry.size:
                    print(f"  skip {remote_path} (same size)")
                    manifest["files"].append({
                        "remote": remote_path, "local": str(local_path),
                        "size": entry.size, "mtime": entry.mtime, "status": "skipped",
                    })
                    continue
                raise BridgeError(
                    f"local file differs: {local_path}; use --overwrite or --skip-existing"
                )

        if args.dry_run:
            print(f"  file {remote_path}  {human_bytes(entry.size)} (dry-run)")
            manifest["files"].append({
                "remote": remote_path, "local": str(local_path),
                "size": entry.size, "mtime": entry.mtime, "status": "would-download",
            })
            continue

        print(f"  file {remote_path}  {human_bytes(entry.size)}")
        progress = Progress(entry.name, enabled=not args.quiet)
        try:
            size, crc, mtime = bridge.get(remote_path, local_path, overwrite=args.overwrite,
                                          progress=progress)
        except BridgeError as exc:
            if not args.skip_errors:
                raise
            print(f"  warn {remote_path}: {exc}", file=sys.stderr)
            manifest.setdefault("errors", []).append({"remote": remote_path, "error": str(exc)})
            continue
        manifest["files"].append({
            "remote": remote_path, "local": str(local_path),
            "size": size, "mtime": mtime, "crc32": crc, "status": "downloaded",
        })


def push_tree(bridge: Bridge, local: Path, remote: str, args: argparse.Namespace,
              manifest: dict) -> None:
    if local.is_dir():
        print(f"  dir  {remote}/")
        if not args.dry_run:
            bridge.mkdir(remote)
        for child in sorted(local.iterdir(), key=lambda p: p.name):
            if args.exclude and any(child.match(pattern) for pattern in args.exclude):
                print(f"  skip {child} (excluded)")
                continue
            push_tree(bridge, child, remote_join(remote, child.name), args, manifest)
        return

    if not local.is_file():
        raise BridgeError(f"not a regular file: {local}")

    if args.dry_run:
        print(f"  file {local} -> {remote}  {human_bytes(local.stat().st_size)} (dry-run)")
        manifest["files"].append({
            "local": str(local), "remote": remote,
            "size": local.stat().st_size, "status": "would-upload",
        })
        return

    print(f"  file {local} -> {remote}  {human_bytes(local.stat().st_size)}")
    progress = Progress(local.name, enabled=not args.quiet)
    size, crc = bridge.put(local, remote, overwrite=args.overwrite, progress=progress)
    manifest["files"].append({
        "local": str(local), "remote": remote,
        "size": size, "crc32": crc, "status": "uploaded",
    })


# --------------------------------------------------------------------- CLI


def cmd_info(bridge: Bridge, args: argparse.Namespace) -> int:
    values = bridge.info()
    order = [
        "mounted", "card_type", "card_name", "capacity_bytes", "sector_size",
        "max_freq_khz", "real_freq_khz", "fs_total_bytes", "fs_free_bytes",
        "proto", "max_frame", "data_chunk", "mount", "error",
    ]
    for key in order:
        if key not in values:
            continue
        value = values[key]
        if key.endswith("_bytes") and value.isdigit():
            print(f"{key:16} {value}  ({human_bytes(int(value))})")
        else:
            print(f"{key:16} {value}")
    return 0


def cmd_df(bridge: Bridge, args: argparse.Namespace) -> int:
    values = bridge.info()
    try:
        total = int(values.get("fs_total_bytes", "0"))
        free = int(values.get("fs_free_bytes", "0"))
    except ValueError:
        print("filesystem size unavailable; is the card mounted?")
        return 1
    used = total - free
    print(f"total {human_bytes(total)}")
    print(f"used  {human_bytes(used)}")
    print(f"free  {human_bytes(free)}")
    return 0


def cmd_ls(bridge: Bridge, args: argparse.Namespace) -> int:
    count = 0
    for entry in bridge.ls(args.path):
        suffix = "/" if entry.is_dir else ""
        if args.long:
            stamp = "-"
            if entry.mtime > 0:
                stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(entry.mtime))
            print(f"{'d' if entry.is_dir else '-'} {entry.size:>12} {stamp}  {entry.name}{suffix}")
        else:
            print(f"{entry.name}{suffix}")
        count += 1
    print(f"-- {count} entries", file=sys.stderr)
    return 0


def cmd_stat(bridge: Bridge, args: argparse.Namespace) -> int:
    values = bridge.stat(args.path)
    for key in ("type", "size", "mtime"):
        print(f"{key:8} {values.get(key, '-')}")
    return 0


def cmd_crc(bridge: Bridge, args: argparse.Namespace) -> int:
    size, crc = bridge.crc(args.path)
    print(f"size {size}")
    print(f"crc32 {crc:08x}")
    return 0


def cmd_get(bridge: Bridge, args: argparse.Namespace) -> int:
    progress = Progress(Path(args.remote).name, enabled=not args.quiet)
    size, crc, _ = bridge.get(args.remote, Path(args.local), overwrite=args.overwrite,
                              progress=progress)
    print(f"downloaded {args.remote} -> {args.local}: {size} bytes, crc32 {crc:08x}")
    return 0


def cmd_put(bridge: Bridge, args: argparse.Namespace) -> int:
    progress = Progress(Path(args.local).name, enabled=not args.quiet)
    size, crc = bridge.put(Path(args.local), args.remote, overwrite=args.overwrite,
                           progress=progress)
    print(f"uploaded {args.local} -> {args.remote}: {size} bytes, crc32 {crc:08x}")
    return 0


def cmd_pull(bridge: Bridge, args: argparse.Namespace) -> int:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    local = Path(args.local) if args.local else Path(f"sdcard_dump-{stamp}")

    manifest: dict = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "remote_root": args.remote,
        "local_root": str(local),
        "dry_run": bool(args.dry_run),
        "files": [],
        "errors": [],
    }

    print(f"pull {args.remote} -> {local}")
    if not args.dry_run:
        local.mkdir(parents=True, exist_ok=True)
    pull_tree(bridge, args.remote, local, args, manifest)

    manifest_path = Path(args.manifest) if args.manifest else \
        local.parent / (local.name + "-manifest.json")
    if args.manifest == "-":
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
    else:
        try:
            manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
            print(f"manifest: {manifest_path}")
        except OSError as exc:
            print(f"warning: could not write manifest {manifest_path}: {exc}", file=sys.stderr)

    total = sum(item.get("size", 0) for item in manifest["files"]
                if item.get("status") in ("downloaded", "would-download"))
    print(f"done: {len(manifest['files'])} entries, {human_bytes(total)}"
          + (" (dry-run)" if args.dry_run else ""))
    return 0


def cmd_push(bridge: Bridge, args: argparse.Namespace) -> int:
    local = Path(args.local)
    if not local.exists():
        raise BridgeError(f"local path not found: {local}")

    manifest: dict = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "remote_root": args.remote,
        "local_root": str(local),
        "dry_run": bool(args.dry_run),
        "files": [],
    }

    print(f"push {local} -> {args.remote}")
    push_tree(bridge, local, args.remote, args, manifest)

    manifest_path = Path(args.manifest) if args.manifest else \
        local.parent / (local.name + "-push-manifest.json")
    if args.manifest == "-":
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
    else:
        try:
            manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
            print(f"manifest: {manifest_path}")
        except OSError as exc:
            print(f"warning: could not write manifest {manifest_path}: {exc}", file=sys.stderr)

    total = sum(item.get("size", 0) for item in manifest["files"]
                if item.get("status") in ("uploaded", "would-upload"))
    print(f"done: {len(manifest['files'])} entries, {human_bytes(total)}"
          + (" (dry-run)" if args.dry_run else ""))
    return 0


def cmd_mkdir(bridge: Bridge, args: argparse.Namespace) -> int:
    for path in args.paths:
        bridge.mkdir(path)
        print(f"mkdir {path}")
    return 0


def cmd_rm(bridge: Bridge, args: argparse.Namespace) -> int:
    for path in args.paths:
        bridge.rm(path)
        print(f"rm {path}")
    return 0


def cmd_rmdir(bridge: Bridge, args: argparse.Namespace) -> int:
    for path in args.paths:
        bridge.rmdir(path)
        print(f"rmdir {path}")
    return 0


def cmd_mv(bridge: Bridge, args: argparse.Namespace) -> int:
    bridge.mv(args.src, args.dst, overwrite=args.overwrite)
    print(f"mv {args.src} -> {args.dst}")
    return 0



def cmd_mount(bridge: Bridge, args: argparse.Namespace) -> int:
    values = bridge.mount()
    print(f"mounted={values.get('mounted', '?')}")
    return 0


def cmd_unmount(bridge: Bridge, args: argparse.Namespace) -> int:
    values = bridge.unmount()
    print(f"mounted={values.get('mounted', '?')}")
    return 0


def cmd_sync(bridge: Bridge, args: argparse.Namespace) -> int:
    bridge.sync()
    print("synced")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="SD card bridge host tool (ESP32-S3 USB-Serial/JTAG)",
    )
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", help="serial port (default: auto-detect)")
    common.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"per-operation timeout in seconds (default {DEFAULT_TIMEOUT})")
    common.add_argument("--debug", action="store_true", help="dump protocol bytes on stderr")

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", parents=[common], help="card/filesystem info").set_defaults(func=cmd_info)
    sub.add_parser("df", parents=[common], help="filesystem free space").set_defaults(func=cmd_df)

    p = sub.add_parser("ls", parents=[common], help="list a directory")
    p.add_argument("path", nargs="?", default="/")
    p.add_argument("-l", "--long", action="store_true")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("stat", parents=[common], help="stat one path")
    p.add_argument("path")
    p.set_defaults(func=cmd_stat)

    p = sub.add_parser("crc", parents=[common], help="compute CRC32 of a remote file")
    p.add_argument("path")
    p.set_defaults(func=cmd_crc)

    p = sub.add_parser("get", parents=[common], help="download one file")
    p.add_argument("remote")
    p.add_argument("local")
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("put", parents=[common], help="upload one file")
    p.add_argument("local")
    p.add_argument("remote")
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(func=cmd_put)

    p = sub.add_parser("pull", parents=[common], help="recursively download a directory")
    p.add_argument("remote", nargs="?", default="/")
    p.add_argument("local", nargs="?")
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("--skip-existing", action="store_true")
    p.add_argument("--skip-errors", action="store_true",
                   help="skip unreadable files/directories and keep going")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--manifest", help="manifest path, or '-' for stdout")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(func=cmd_pull)

    p = sub.add_parser("push", parents=[common], help="recursively upload a file/directory")
    p.add_argument("local")
    p.add_argument("remote")
    p.add_argument("--overwrite", action="store_true")
    p.add_argument("--exclude", action="append", default=[], help="glob to skip (repeatable)")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--manifest", help="manifest path, or '-' for stdout")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(func=cmd_push)

    p = sub.add_parser("mkdir", parents=[common], help="create directories")
    p.add_argument("paths", nargs="+")
    p.set_defaults(func=cmd_mkdir)

    p = sub.add_parser("rm", parents=[common], help="remove files")
    p.add_argument("paths", nargs="+")
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser("rmdir", parents=[common], help="remove empty directories")
    p.add_argument("paths", nargs="+")
    p.set_defaults(func=cmd_rmdir)

    p = sub.add_parser("mv", parents=[common], help="rename/move")
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--overwrite", action="store_true")
    p.set_defaults(func=cmd_mv)

    sub.add_parser("mount", parents=[common], help="retry mounting the card").set_defaults(func=cmd_mount)
    sub.add_parser("unmount", parents=[common], help="unmount the card").set_defaults(func=cmd_unmount)
    sub.add_parser("sync", parents=[common], help="flush filesystem buffers").set_defaults(func=cmd_sync)

    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        with Bridge(args.port, args.timeout, args.debug) as bridge:
            return int(args.func(bridge, args))
    except BridgeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
