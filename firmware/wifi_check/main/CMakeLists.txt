# 凭据注入（构建期）：
# 从仓库根 .env 读取 WIFI_SSID / WIFI_PASSWORD，在构建目录生成 wifi_secrets.h。
# 凭据不进入源码树、不进 git；.env 已被 .gitignore 排除。
# 修改 .env 后重新编译即可（CMAKE_CONFIGURE_DEPENDS 会触发重新配置）。
set(WIFI_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../.env"
    CACHE FILEPATH "WiFi 凭据文件（.env）")

# 让 .env 变更触发重新配置。注意：IDF 的“依赖收集”预扫描阶段
# （CMAKE_BUILD_EARLY_EXPANSION=1）没有真实目录作用域，必须跳过。
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_property(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WIFI_ENV_FILE}")
endif()

set(_wifi_ssid "")
set(_wifi_pass "")
if(EXISTS "${WIFI_ENV_FILE}")
    file(STRINGS "${WIFI_ENV_FILE}" _wifi_env_lines)
    foreach(_line IN LISTS _wifi_env_lines)
        # 跳过注释 / 空行，匹配 KEY = VALUE（允许首尾空白）
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

# 转义为 C 字符串字面量（支持带引号/反斜杠的值）
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
    SRCS "main.c" "discovery.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_REQUIRES esp_wifi esp_netif nvs_flash lwip esp_timer
)
