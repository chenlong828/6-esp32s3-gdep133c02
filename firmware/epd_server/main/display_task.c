/*
 * 任务 3：刷屏任务
 *
 * 流程：等待队列 → 取最新一张（中间的跳过）→ 从 SD 整帧读入 PSRAM →
 *       CRC / 头部校验 → init 或 reinit 面板 → DTM/DRF → 收敛状态。
 *
 * 为什么整帧读进 PSRAM 而不是边读 SD 边发 SPI：
 *   - 驱动 send_half() 每次要按行跨 600 B 取 300 B，整帧连续最省事；
 *   - 8 MB PSRAM 放 960 KB 绰绰有余，读满再刷也能避免 SD 与面板时序互相拖累；
 *   - 复制到 DMA 缓冲的逻辑不变（驱动只认内部 DMA 内存）。
 *
 * “跳过中间帧”的意义：连续上传 3 张时，每张要刷 12~19 s，
 * 只刷最后一张才是用户要的结果。
 */

#include "display_task.h"

#include <stdio.h>
#include <string.h>

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "crc32.h"
#include "epd_panel.h"
#include "epd_state.h"
#include "image_store.h"

static const char *TAG = "display";

#define FRAME_TOTAL_BYTES (EPD_BIN_HEADER_SIZE + 960000)

static QueueHandle_t s_queue;
static uint8_t *s_file_buf;

static esp_err_t load_file(const display_job_t *job)
{
    FILE *f = fopen(job->path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开 %s", job->path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t got = fread(s_file_buf, 1, FRAME_TOTAL_BYTES, f);
    const int extra = fgetc(f);
    fclose(f);

    if (got != FRAME_TOTAL_BYTES || extra != EOF) {
        ESP_LOGE(TAG, "文件长度异常：读到 %u 字节（应为 %u）",
                 (unsigned)got, (unsigned)FRAME_TOTAL_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    epd_bin_info_t info;
    esp_err_t err = image_store_parse(s_file_buf, got, &info);
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t crc = epd_crc32(s_file_buf, got);
    if (job->crc32 != 0 && crc != job->crc32) {
        ESP_LOGE(TAG, "CRC 不符：文件 %08x，期望 %08x",
                 (unsigned)crc, (unsigned)job->crc32);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "已读入 %s：%u 字节，crc32=%08x，面板 %ux%u",
             job->name, (unsigned)got, (unsigned)crc,
             info.width, info.height);
    return ESP_OK;
}

/* 面板这一段单独抽出来，方便失败重试一次 */
static esp_err_t push_to_panel(const display_job_t *job)
{
    esp_err_t err = epd_panel_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板初始化失败：%s", esp_err_to_name(err));
        return err;
    }
    epd_state_set_panel(true);

    if (job->kind == DISPLAY_JOB_SOLID) {
        return epd_panel_fill(job->solid_color);
    }

    return epd_panel_display(s_file_buf + EPD_BIN_HEADER_SIZE);
}

static void run_job(const display_job_t *job)
{
    const int64_t t0 = esp_timer_get_time();
    epd_state_begin_display();

    esp_err_t err = ESP_OK;

    if (job->kind == DISPLAY_JOB_FILE) {
        err = load_file(job);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "开始刷屏：%s%s", job->name,
                 job->kind == DISPLAY_JOB_SOLID ? "（单色）" : "");
        err = push_to_panel(job);
    }

#if EPD_DISPLAY_RETRY
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_INVALID_SIZE && err != ESP_ERR_INVALID_CRC) {
        ESP_LOGW(TAG, "刷屏失败（%s），重试一次", esp_err_to_name(err));
        err = push_to_panel(job);
    }
#endif

    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "刷新完成：%s，用时 %u ms", job->name, (unsigned)ms);
    } else {
        ESP_LOGE(TAG, "刷新失败：%s（%s），用时 %u ms",
                 job->name, esp_err_to_name(err), (unsigned)ms);
    }

    epd_state_end_display(err, ms, job->name, job->crc32,
                          job->saved_at, job->bytes);
}

static void display_task(void *arg)
{
    (void)arg;

    s_file_buf = heap_caps_malloc(FRAME_TOTAL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_file_buf == NULL) {
        ESP_LOGW(TAG, "PSRAM 分配失败，退回内部 RAM（大概率失败）");
        s_file_buf = malloc(FRAME_TOTAL_BYTES);
    }
    if (s_file_buf == NULL) {
        ESP_LOGE(TAG, "整帧缓冲分配失败，刷屏任务退出");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "整帧缓冲 %u 字节已就绪", (unsigned)FRAME_TOTAL_BYTES);

    for (;;) {
        display_job_t job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 只刷最新一张：把队列里已排队的旧任务丢掉 */
        display_job_t newer;
        while (xQueueReceive(s_queue, &newer, 0) == pdTRUE) {
            ESP_LOGI(TAG, "跳过过期任务：%s", job.name);
            job = newer;
        }

        run_job(&job);
    }
}

esp_err_t display_task_start(void)
{
    s_queue = xQueueCreate(EPD_DISPLAY_QUEUE_DEPTH, sizeof(display_job_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(display_task, "epd_display",
                                EPD_DISPLAY_TASK_STACK, NULL,
                                EPD_DISPLAY_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t display_task_submit(const display_job_t *job, TickType_t wait)
{
    if (job == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return (xQueueSend(s_queue, job, wait) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool display_task_pending(void)
{
    return (s_queue != NULL) && (uxQueueMessagesWaiting(s_queue) > 0);
}
