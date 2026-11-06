#!/usr/bin/env bash
# 整片备份 16MB（刷坏可整片写回）
set -euo pipefail
PORT=${PORT:-/dev/cu.usbmodem1101}
OUT=${1:-factory-16mb-$(date +%Y%m%d-%H%M).bin}
cd "$(dirname "$0")/.."
./.venv/bin/python -m esptool -p "$PORT" -b 921600 read-flash 0 0x1000000 "$OUT"
echo "备份完成：$OUT"
echo "恢复：./.venv/bin/python -m esptool -p $PORT write-flash 0 $OUT"
