/*
 * SD 卡上的图片仓库。
 *
 * 布局：
 *   /sdcard/epd/current        当前应显示的图（文本元数据，原子替换）
 *   /sdcard/epd/incoming.part  上传中的临时文件
 *   /sdcard/epd/history/      每次上传都留档，永不覆盖（文件名带时间戳）
 *
 * 所有“写”都遵循同一套安全顺序：临时文件 → fflush/fsync → 校验 → rename。
 * FATFS 的 rename 是同目录原子操作，所以外部永远看不到半截图。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#include "epd_config.h"

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint32_t interval_ms;
    uint32_t total_bytes;
} epd_bin_info_t;

/* 建目录；卡未挂载时返回 ESP_ERR_INVALID_STATE */
esp_err_t image_store_init(void);

/* 解析内存中的 EPD6BIN（单帧），只校验头部与长度 */
esp_err_t image_store_parse(const uint8_t *data, size_t len, epd_bin_info_t *info);

/* 解析磁盘上的 EPD6BIN */
esp_err_t image_store_validate(const char *path, epd_bin_info_t *info);

/* 把上传完成的临时文件归档并更新 current；成功时 out_name 返回最终文件名 */
esp_err_t image_store_commit(const char *tmp_path, const char *requested_name,
                             uint32_t crc, uint32_t saved_at, uint32_t total_bytes,
                             char *out_name, size_t out_len);

/* 直接把某张归档设为 current（重新显示时用），不影响 history/ 里的文件 */
esp_err_t image_store_set_current(const char *name, uint32_t crc,
                                  uint32_t saved_at, uint32_t bytes);

/* 读 current 元数据；没有有效记录返回 ESP_ERR_NOT_FOUND */
esp_err_t image_store_read_current(char *name, size_t name_len,
                                   uint32_t *crc, uint32_t *saved_at,
                                   uint32_t *bytes);

/* 文件名 → 绝对路径（会先做名字合法性检查） */
esp_err_t image_store_abs_path(const char *name, char *out, size_t out_len);

/* 归档目录里的 .bin 列表，按文件名倒序（文件名带时间戳，即最新在前） */
esp_err_t image_store_list(cJSON *out_array);

uint32_t image_store_count(void);

bool image_store_name_valid(const char *name);
