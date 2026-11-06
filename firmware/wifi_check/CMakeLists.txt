# WiFi 连通性测试工程（独立，不碰 EPD 屏和 SD 卡）。
#
# 目标：连接指定 AP，确认 DHCP 下发的 IP / 掩码 / 网关 / DNS 是否正确，
# 并对网关做 ICMP ping 验证三层连通性。
#
# 凭据不进源码：main/CMakeLists.txt 从仓库根 .env 读取 WIFI_SSID / WIFI_PASSWORD，
# 在构建目录生成 wifi_secrets.h 供 main.c 使用。.env 已被 .gitignore 排除。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_check)
