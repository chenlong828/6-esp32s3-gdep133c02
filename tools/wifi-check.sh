#!/usr/bin/env bash
# 构建 / 烧录 / 监视 WiFi 连通性测试工程。
#
# 凭据来源：仓库根 .env（构建时由 CMake 注入，源码中无密码）。
# 用法：
#   ./tools/wifi-check.sh build     # 只编译
#   ./tools/wifi-check.sh flash     # 编译并烧录
#   ./tools/wifi-check.sh monitor   # 串口监视（Ctrl+] 退出）
#   ./tools/wifi-check.sh all       # 编译 + 烧录 + 监视
# 可用 PORT=... 覆盖串口，默认 /dev/cu.usbmodem1101。
set -e

cd "$(dirname "$0")/.."

if [ ! -f .env ]; then
    echo "缺少 .env：请执行  cp .env.example .env  并填写 WIFI_SSID / WIFI_PASSWORD" >&2
    exit 1
fi

. tools/env.sh

PORT="${PORT:-/dev/cu.usbmodem1101}"
CMD="${1:-build}"

cd firmware/wifi_check
if [ ! -f sdkconfig ]; then
    idf.py set-target esp32s3
fi

case "$CMD" in
    build)   idf.py build ;;
    flash)   idf.py -p "$PORT" flash ;;
    monitor) idf.py -p "$PORT" monitor ;;
    all)     idf.py -p "$PORT" flash monitor ;;
    *)       echo "未知命令：$CMD（可用：build / flash / monitor / all）" >&2; exit 2 ;;
esac
