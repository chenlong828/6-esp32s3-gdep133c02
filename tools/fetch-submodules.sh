#!/usr/bin/env bash
# 用 GitHub codeload tarball 补齐 ESP-IDF 子模块（代理下 git pack 容易断，tarball 更稳）
# 兼容 macOS 自带 bash 3.2（不能用 mapfile）
set -uo pipefail
IDF=${1:-$HOME/esp/esp-idf-v5.4.4}
PROXY=${PROXY:-socks5h://127.0.0.1:2080}
cd "$IDF" || exit 1
for round in 1 2 3; do
  missing=$(git submodule status | awk '$1 ~ /^-/ {print substr($1,2)" "$2}')
  if [ -z "$missing" ]; then echo "全部子模块就绪"; exit 0; fi
  echo "第 $round 轮：待补 $(echo "$missing" | wc -l | tr -d ' ') 个"
  echo "$missing" | while read -r sha path; do
    [ -z "${sha:-}" ] && continue
    url=$(git config -f .gitmodules "submodule.$path.url")
    repo=$(echo "$url" | sed -E 's#https?://[^/]+/##; s#\.git$##')
    echo "  -> $path @ ${sha:0:8} ($repo)"
    tmp="/tmp/sub-$(echo "$path" | tr '/' '_').tgz"
    for attempt in 1 2 3; do
      curl -x "$PROXY" -L --fail --connect-timeout 20 --max-time 900 \
           --retry 2 --retry-delay 2 -C - -o "$tmp" \
           "https://codeload.github.com/$repo/tar.gz/$sha" && break
      echo "     curl 失败，重试 $attempt"; sleep 3
    done
    if [ ! -s "$tmp" ]; then echo "     !! 下载失败：$path"; continue; fi
    mkdir -p "$path"
    rm -rf "$path"/* 2>/dev/null
    if tar -xzf "$tmp" -C "$path" --strip-components=1; then echo "     解包完成"; else echo "     !! 解包失败"; fi
    rm -f "$tmp"
  done
done
echo "仍缺失：$(git submodule status | awk '$1 ~ /^-/' | wc -l | tr -d ' ')"
