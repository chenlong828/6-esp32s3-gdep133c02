/*
 * GDEP133C02 (13.3") 固件：开机直接从固件内嵌的 EPD6BIN 数据刷屏。
 *
 * 数据来源：host/e6_convert.py 生成的 out/genshin01.bin，
 * 构建时由顶层 CMakeLists.txt 同步到 main/assets/，再用 EMBED_FILES 打进固件。
 * 不读 SD 卡，不依赖文件系统。
 *
 * 内嵌格式（与 host 管线一致）：
 *   偏移 0   "EPD6BIN\0"        8 B
 *   偏移 8   uint16 width       2 B LE = 1200
 *   偏移 10  uint16 height      2 B LE = 1600
 *   偏移 12  uint16 frames      2 B
 *   偏移 14  uint32 interval_ms 4 B
 *   偏移 18  帧数据 × frames     每帧 960 000 B
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "GDEP133C02.h"

#define EPD_BIN_MAGIC       "EPD6BIN"
#define EPD_BIN_MAGIC_LEN   8
#define EPD_BIN_HEADER_SIZE 18

/* EMBED_FILES 生成的符号（名字取自文件名 genshin01.bin） */
extern const uint8_t genshin01_bin_start[] asm("_binary_genshin01_bin_start");
extern const uint8_t genshin01_bin_end[]   asm("_binary_genshin01_bin_end");

static const char *TAG = "demo";

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void app_main(void)
{
    const uint8_t *blob = genshin01_bin_start;
    const size_t blob_len = (size_t)(genshin01_bin_end - genshin01_bin_start);

    ESP_LOGI(TAG, "=== GDEP133C02 13.3\" 内嵌图演示 ===");
    ESP_LOGI(TAG, "内嵌数据：%u 字节", (unsigned)blob_len);

    if (blob_len < EPD_BIN_HEADER_SIZE + GDEP133C02_IMAGE_SIZE ||
        memcmp(blob, EPD_BIN_MAGIC, EPD_BIN_MAGIC_LEN) != 0) {
        ESP_LOGE(TAG, "内嵌数据不是合法的 EPD6BIN（长度 %u）", (unsigned)blob_len);
        return;
    }

    const uint16_t width = rd_u16(blob + 8);
    const uint16_t height = rd_u16(blob + 10);
    const uint16_t frames = rd_u16(blob + 12);
    const uint32_t interval_ms = rd_u32(blob + 14);

    ESP_LOGI(TAG, "面板 %ux%u，帧数 %u，间隔 %u ms",
             width, height, frames, (unsigned)interval_ms);

    if (width != GDEP133C02_WIDTH || height != GDEP133C02_HEIGHT) {
        ESP_LOGE(TAG, "尺寸不匹配：数据 %ux%u，面板 %ux%u",
                 width, height, GDEP133C02_WIDTH, GDEP133C02_HEIGHT);
        return;
    }

    const size_t need = EPD_BIN_HEADER_SIZE + (size_t)frames * GDEP133C02_IMAGE_SIZE;
    if (frames == 0 || blob_len < need) {
        ESP_LOGE(TAG, "帧数/长度不一致：需要 %u 字节，实际 %u",
                 (unsigned)need, (unsigned)blob_len);
        return;
    }

    ESP_ERROR_CHECK(gdep133c02_init());

    for (uint16_t i = 0; i < frames; i++) {
        const uint8_t *frame = blob + EPD_BIN_HEADER_SIZE + (size_t)i * GDEP133C02_IMAGE_SIZE;

        /* 与厂家示例一致：每帧刷新前重跑一次面板初始化 */
        if (i > 0) {
            ESP_ERROR_CHECK(gdep133c02_reinit());
        }

        ESP_LOGI(TAG, "刷新第 %u/%u 帧...", i + 1, frames);
        ESP_ERROR_CHECK(gdep133c02_display(frame));

        if (i + 1 < frames && interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }

    ESP_ERROR_CHECK(gdep133c02_sleep());
    ESP_LOGI(TAG, "演示结束，画面保持。");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
