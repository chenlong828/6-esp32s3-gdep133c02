/*
 * 全局运行状态：唯一一处“可以被 HTTP 状态接口读、被各任务写”的地方。
 *
 * 所有字段都在 s_lock 保护下读写；对外只给快照，避免调用方拿着指针跨越任务。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#include "epd_config.h"

typedef struct {
    /* SD */
    bool sd_mounted;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;

    /* 面板 / 任务 3 */
    bool panel_ready;
    bool display_busy;
    uint32_t refresh_count;
    uint32_t last_refresh_ms;
    uint32_t last_refresh_uptime_s;
    int last_display_err;      /* esp_err_t */
    char current_name[EPD_NAME_MAX];
    uint32_t current_crc;
    uint32_t current_saved_at;
    uint32_t current_bytes;

    /* 上传 / 任务 2 */
    bool upload_active;
    uint32_t upload_received;
    uint32_t upload_total;
    uint32_t upload_ok_count;
    uint32_t upload_fail_count;
    uint32_t last_upload_bytes;
    uint32_t last_upload_ms;
} epd_state_t;

void epd_state_init(void);

void epd_state_snapshot(epd_state_t *out);

void epd_state_set_sd(bool mounted, uint64_t total, uint64_t free);
void epd_state_set_panel(bool ready);

void epd_state_begin_display(void);
void epd_state_end_display(esp_err_t err, uint32_t duration_ms,
                           const char *name, uint32_t crc, uint32_t saved_at,
                           uint32_t bytes);

void epd_state_upload_begin(uint32_t total);
void epd_state_upload_progress(uint32_t received);
void epd_state_upload_end(bool ok, uint32_t bytes, uint32_t ms);

/* 把快照 + 设备静态信息填进 cJSON 对象（状态接口的唯一实现） */
void epd_state_fill_json(cJSON *root);
