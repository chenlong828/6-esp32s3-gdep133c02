/* SD 卡挂载（SPI3，FATFS）。绝不格式化卡。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    uint32_t sector_size;
    uint64_t capacity_bytes;
    uint32_t real_freq_khz;
    char card_type[16];   /* SDHC/SDXC、SDSC、MMC */
    char card_name[16];   /* CID 里的产品名，可能带空格 */
    uint64_t fs_total_bytes;
    uint64_t fs_free_bytes;
} sd_card_info_t;

esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
bool sd_card_is_mounted(void);
const char *sd_card_last_error(void);
esp_err_t sd_card_get_info(sd_card_info_t *out);
