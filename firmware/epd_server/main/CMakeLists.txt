# 刷屏固件主组件
#
# 凭据注入（构建期）：从仓库根 .env 读取 WIFI_SSID / WIFI_PASSWORD，
# 在构建目录生成 wifi_secrets.h。凭据不进源码树、不进 git。
# 与 firmware/wifi_check 完全同一套做法（见 tools/wifi-check.sh）。

set(WIFI_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../.env"
    CACHE FILEPATH "WiFi 凭据文件（.env）")

# 让 .env 变更触发重新配置。IDF 的依赖预扫描阶段（CMAKE_BUILD_EARLY_EXPANSION=1）
# 没有真实目录作用域，必须跳过，否则 set_property(DIRECTORY ...) 会报错。
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_property(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WIFI_ENV_FILE}")
endif()

set(_wifi_ssid "")
set(_wifi_pass "")
if(EXISTS "${WIFI_ENV_FILE}")
    file(STRINGS "${WIFI_ENV_FILE}" _wifi_env_lines)
    foreach(_line IN LISTS _wifi_env_lines)
        if(_line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.*)$")
            set(_key "${CMAKE_MATCH_1}")
            set(_val "${CMAKE_MATCH_2}")
            string(REGEX REPLACE "[ \t]+$" "" _val "${_val}")
            if(_val MATCHES "^\"(.*)\"$")
                set(_val "${CMAKE_MATCH_1}")
            elseif(_val MATCHES "^'(.*)'$")
                set(_val "${CMAKE_MATCH_1}")
            endif()
            if(_key STREQUAL "WIFI_SSID")
                set(_wifi_ssid "${_val}")
            elseif(_key STREQUAL "WIFI_PASSWORD")
                set(_wifi_pass "${_val}")
            endif()
        endif()
    endforeach()
endif()

if(NOT _wifi_ssid)
    message(FATAL_ERROR
        "未从 ${WIFI_ENV_FILE} 读到 WIFI_SSID。\n"
        "请复制仓库根 .env.example 为 .env，并填写 WIFI_SSID / WIFI_PASSWORD。")
endif()
if(NOT _wifi_pass)
    message(FATAL_ERROR
        "未从 ${WIFI_ENV_FILE} 读到 WIFI_PASSWORD。\n"
        "请复制仓库根 .env.example 为 .env，并填写 WIFI_SSID / WIFI_PASSWORD。")
endif()

string(REPLACE "\\" "\\\\" _wifi_ssid "${_wifi_ssid}")
string(REPLACE "\"" "\\\"" _wifi_ssid "${_wifi_ssid}")
string(REPLACE "\\" "\\\\" _wifi_pass "${_wifi_pass}")
string(REPLACE "\"" "\\\"" _wifi_pass "${_wifi_pass}")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/wifi_secrets.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/wifi_secrets.h"
    @ONLY
)

idf_component_register(
    SRCS
        "main.c"
        "wifi_sta.c"
        "discovery.c"
        "sd_card.c"
        "GDEP133C02.c"
        "epd_panel.c"
        "image_store.c"
        "epd_state.c"
        "display_task.c"
        "upload_server.c"
        "crc32.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        esp_wifi
        esp_netif
        nvs_flash
        lwip
        esp_timer
        esp_http_server
        esp_driver_spi
        esp_driver_gpio
        esp_driver_sdspi
        sdmmc
        fatfs
        json
        esp_psram
)
