/*
 * SD card mount/unmount for the ESP32-133C02-IL board.
 *
 * Board wiring (see docs/text/ESP32-133C02_board_manual.txt, section 3.2):
 *   CS=IO15, MOSI=IO3, MISO=IO5, SCK=IO8, no card-detect, no write-protect.
 * The EPD uses SPI2 on this board, so the card gets SPI3.
 */

#include "sd_card.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define TAG "sd_card"

#define PIN_SD_SCLK 8
#define PIN_SD_MOSI 3
#define PIN_SD_MISO 5
#define PIN_SD_CS   15

#define SD_SPI_HOST     SPI3_HOST
#define SD_MAX_FREQ_KHZ 10000

static sdmmc_card_t *s_card;
static bool s_mounted;
static char s_last_error[160] = "not mounted";

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

const char *sd_card_last_error(void)
{
    return s_last_error;
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        set_error("spi_bus_initialize: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_MAX_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = PIN_SD_CS;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = SDSPI_SLOT_NO_WP;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, /* never touch the user's card */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
        .rootdir_entries = 0,
    };

    err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        set_error("mount: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        spi_bus_free(SD_SPI_HOST);
        s_card = NULL;
        return err;
    }

    s_mounted = true;
    set_error("ok");
    ESP_LOGI(TAG, "SD card mounted at %s", SD_CARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        set_error("unmount: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    s_mounted = false;
    s_card = NULL;
    spi_bus_free(SD_SPI_HOST);
    set_error("not mounted");
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

esp_err_t sd_card_get_info(sd_card_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const sdmmc_csd_t *csd = &s_card->csd;
    out->sector_size = (uint32_t)csd->sector_size;
    out->capacity_bytes = (uint64_t)(uint32_t)csd->capacity * out->sector_size;
    out->max_freq_khz = s_card->max_freq_khz;
    out->real_freq_khz = s_card->real_freq_khz;

    if (s_card->is_mmc) {
        snprintf(out->card_type, sizeof(out->card_type), "MMC");
    } else if (s_card->ocr & (1u << 30)) {
        snprintf(out->card_type, sizeof(out->card_type), "SDHC/SDXC");
    } else {
        snprintf(out->card_type, sizeof(out->card_type), "SDSC");
    }

    /* CID name is exactly 8 bytes and is not NUL terminated. */
    for (size_t i = 0; i < sizeof(out->card_name) - 1 && i < 8; i++) {
        unsigned char c = (unsigned char)s_card->cid.name[i];
        out->card_name[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    for (int i = (int)strlen(out->card_name) - 1; i >= 0 && out->card_name[i] == ' '; i--) {
        out->card_name[i] = '\0';
    }

    esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &out->fs_total_bytes, &out->fs_free_bytes);

    return ESP_OK;
}
