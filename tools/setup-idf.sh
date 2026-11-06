#!/usr/bin/env bash
# 安装 ESP-IDF（一次性）；默认 v5.4.4
set -euo pipefail
VERSION=${1:-v5.4.4}
DIR="$HOME/esp/esp-idf-$VERSION"
export GIT_TERMINAL_PROMPT=0
if [ ! -d "$DIR/.git" ]; then
  mkdir -p "$HOME/esp"
  git clone -b "$VERSION" --recursive --depth 1 https://github.com/espressif/esp-idf.git "$DIR"
fi
cd "$DIR"
IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets ./install.sh esp32s3
echo "完成：. $DIR/export.sh"
