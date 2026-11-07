/* 任务 3：把 SD 卡里的图片包刷新到屏幕。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

#include "epd_config.h"

typedef enum {
    DISPLAY_JOB_FILE = 0, /* 读 SD 上的 EPD6BIN 并刷屏 */
    DISPLAY_JOB_SOLID,    /* 整屏单色（自检） */
} display_job_kind_t;

typedef struct {
    display_job_kind_t kind;
    uint8_t solid_color;              /* DISPLAY_JOB_SOLID 用 */
    char path[EPD_PATH_MAX];          /* DISPLAY_JOB_FILE 用：VFS 绝对路径 */
    char name[EPD_NAME_MAX];          /* 对外展示的文件名 */
    uint32_t crc32;                   /* 0 = 不校验 */
    uint32_t saved_at;                /* Unix 秒，0 = 未知 */
    uint32_t bytes;
} display_job_t;

esp_err_t display_task_start(void);

/* 投递一次刷新；队列满返回 ESP_ERR_TIMEOUT */
esp_err_t display_task_submit(const display_job_t *job, TickType_t wait);

/* 队列里还有没有待刷的（不含正在刷的那张） */
bool display_task_pending(void);
