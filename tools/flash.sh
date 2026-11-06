#!/usr/bin/env bash
# 编译并烧录（默认 firmware/e6_epd）
set -euo pipefail
IDF=${IDF_PATH:-$HOME/esp/esp-idf-v5.4.4}
PORT=${PORT:-/dev/cu.usbmodem1101}
. "$IDF/export.sh" >/dev/null
cd "$(dirname "$0")/.."
cd "${1:-firmware/e6_epd}"
idf.py set-target esp32s3 build
idf.py -p "$PORT" flash
