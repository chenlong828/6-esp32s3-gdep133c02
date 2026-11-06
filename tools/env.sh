#!/usr/bin/env bash
# 进入项目环境：source tools/env.sh
export IDF_PATH="$HOME/esp/esp-idf-v5.4.4"
. "$IDF_PATH/export.sh" >/dev/null
export IDF_SKIP_CHECK_SUBMODULES=1   # 子模块由 tarball 解包，git 元数据缺失
echo "ESP-IDF: $(idf.py --version 2>/dev/null | tail -1)  |  fastfetch-submodules: 已就绪"
