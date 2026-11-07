#!/usr/bin/env bash
#
# 把 SwiftPM 可执行文件打成一个可双击的 .app。
#
#   ./build_app.sh            # release 构建 + 打包
#   ./build_app.sh debug      # 调试构建
#   ./build_app.sh release run # 打包后直接启动
#
# 产物：host/EDPHost/build/EDPHost.app
# 说明：本地构建的 .app 不是公证版本，只在本机使用；用 ad-hoc 签名避免
#       Gatekeeper 直接拒绝启动。

set -euo pipefail

cd "$(dirname "$0")"

CONFIG="${1:-release}"
ACTION="${2:-}"

echo "==> swift build -c ${CONFIG}"
swift build -c "${CONFIG}" --disable-sandbox

BIN_DIR="$(swift build -c "${CONFIG}" --disable-sandbox --show-bin-path)"
BIN="${BIN_DIR}/EDPHost"
if [[ ! -x "${BIN}" ]]; then
    echo "找不到可执行文件：${BIN}" >&2
    exit 1
fi

APP="build/EDPHost.app"
echo "==> 组装 ${APP}"
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
cp "${BIN}" "${APP}/Contents/MacOS/EDPHost"
cp Resources/Info.plist "${APP}/Contents/Info.plist"

# 有图标就一起带上（tools/make_app_icon.py 生成）
if [[ -f Resources/AppIcon.icns ]]; then
    cp Resources/AppIcon.icns "${APP}/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" \
        "${APP}/Contents/Info.plist" >/dev/null 2>&1 || true
fi

echo "==> ad-hoc 签名"
codesign --force --sign - --timestamp=none "${APP}" >/dev/null 2>&1 \
    && echo "    签名完成" \
    || echo "    签名失败（不致命，仍可本机运行）"

echo "==> 完成：$(pwd)/${APP}"

if [[ "${ACTION}" == "run" ]]; then
    echo "==> 启动"
    open "${APP}"
fi
