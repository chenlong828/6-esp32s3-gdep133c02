cmake_minimum_required(VERSION 3.16)

# 出图脚本（host/e6_convert.py）的产物是素材来源：仓库根 out/genshin01.bin。
# 存在就同步一份到 main/assets/ 供 EMBED_FILES 打包；不存在则沿用已有快照。
set(ASSET_SRC "${CMAKE_CURRENT_LIST_DIR}/../../out/genshin01.bin")
set(ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}/main/assets")
if(EXISTS "${ASSET_SRC}")
    file(MAKE_DIRECTORY "${ASSET_DIR}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${ASSET_SRC}" "${ASSET_DIR}/genshin01.bin")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(e6_epd)

# 素材一变就重新 configure，避免"改了 out/genshin01.bin 但固件还是旧图"
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ASSET_SRC}")
