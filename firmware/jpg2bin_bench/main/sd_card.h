#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    char card_type[16];
    char card_name[16];
    uint64_t capacity_bytes;
    uint32_t sector_size;
    uint32_t max_freq_khz;
    int real_freq_khz;
    uint64_t fs_total_bytes;
    uint64_t fs_free_bytes;
} sd_card_info_t;

/* Mount the card at the requested SPI clock.  Never formats. */
esp_err_t sd_card_mount(int freq_khz);

/* Unmount the card and release the SPI bus. */
esp_err_t sd_card_unmount(void);

bool sd_card_is_mounted(void);

/* Human readable description of the last mount/unmount failure. */
const char *sd_card_last_error(void);

esp_err_t sd_card_get_info(sd_card_info_t *out);
