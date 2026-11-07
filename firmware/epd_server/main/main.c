/*
 * GDEP133C02（13.3" E Ink Spectra 6）局域网刷屏固件
 *
 * 三个任务，对应需求里的三段链路：
 *
 *   任务 1  discovery.c      UDP 3232 广播应答，告诉上位机 IP / MAC / HTTP 端口
 *   任务 2  upload_server.c  HTTP POST /api/v1/image，边收边落 SD，归档后投递刷屏任务
 *   任务 3  display_task.c   从 SD 读整帧 → 面板时序 → 刷新（12~19 s）
 *
 * 开机顺序刻意做成“网络先起、刷屏后动”：
 *   WiFi 连接 / HTTP / 发现服务都不依赖面板，面板初始化失败也不影响上位机
 *   照常找到设备并上传图片（状态接口会如实报告 panel.ready=false）。
 *
 * 凭据来自仓库根 .env（构建期注入 wifi_secrets.h），源码与 git 中不含密码。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "discovery.h"
#include "display_task.h"
#include "epd_config.h"
#include "epd_state.h"
#include "image_store.h"
#include "sd_card.h"
#include "upload_server.h"
#include "wifi_sta.h"

static const char *TAG = "main";

static void boot_restore_last_image(void)
{
#if EPD_BOOT_RESTORE
    char name[EPD_NAME_MAX] = "";
    uint32_t crc = 0, saved_at = 0, bytes = 0;

    if (image_store_read_current(name, sizeof(name), &crc, &saved_at, &bytes) != ESP_OK) {
        ESP_LOGI(TAG, "SD 上没有上次的图片记录，等待首次上传");
        return;
    }

    char path[EPD_PATH_MAX];
    if (image_store_abs_path(name, path, sizeof(path)) != ESP_OK) {
        ESP_LOGW(TAG, "上次的图片名不合法：%s", name);
        return;
    }

    epd_bin_info_t info;
    if (image_store_validate(path, &info) != ESP_OK) {
        ESP_LOGW(TAG, "上次的图片文件不可用：%s", name);
        return;
    }

    display_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = DISPLAY_JOB_FILE;
    strlcpy(job.name, name, sizeof(job.name));
    strlcpy(job.path, path, sizeof(job.path));
    job.crc32 = crc;
    job.saved_at = saved_at;
    job.bytes = info.total_bytes;

    if (display_task_submit(&job, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGI(TAG, "开机恢复：准备重刷 %s（%u 字节）", name, (unsigned)info.total_bytes);
    } else {
        ESP_LOGW(TAG, "开机恢复任务投递失败");
    }
#else
    ESP_LOGI(TAG, "EPD_BOOT_RESTORE=0，开机不动屏");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " %s v%s —— %s 局域网刷屏固件", EPD_FW_NAME, EPD_FW_VERSION, EPD_DEVICE_MODEL);
    ESP_LOGI(TAG, " 发现 UDP %d ｜ HTTP %d ｜ 主机名 %s",
             EPD_DISCOVERY_PORT, EPD_HTTP_PORT, EPD_HOSTNAME);
    ESP_LOGI(TAG, "==============================================");

    epd_state_init();

    /* NVS：WiFi 驱动需要 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 先起网络：即使面板 / SD 有问题，上位机也能发现设备并看到状态 */
    ESP_ERROR_CHECK(wifi_sta_start(EPD_HOSTNAME));

    /* SD 卡：失败不致命，管理接口会报告 sd.mounted=false */
    if (sd_card_mount() == ESP_OK) {
        sd_card_info_t info;
        if (sd_card_get_info(&info) == ESP_OK) {
            epd_state_set_sd(true, info.fs_total_bytes, info.fs_free_bytes);
            ESP_LOGI(TAG, "SD：%s \"%s\"，FAT 总 %llu MB / 剩 %llu MB，SPI %u kHz",
                     info.card_type, info.card_name,
                     (unsigned long long)(info.fs_total_bytes / (1024 * 1024)),
                     (unsigned long long)(info.fs_free_bytes / (1024 * 1024)),
                     (unsigned)info.real_freq_khz);
        }

        if (image_store_init() != ESP_OK) {
            ESP_LOGE(TAG, "图片目录初始化失败：%s", sd_card_last_error());
        }
    } else {
        epd_state_set_sd(false, 0, 0);
        ESP_LOGE(TAG, "SD 卡挂载失败：%s（上传功能不可用，网络功能正常）",
                 sd_card_last_error());
    }

    /* 任务 3 先就位，开机恢复才能立刻投递 */
    ESP_ERROR_CHECK(display_task_start());
    boot_restore_last_image();

    /* 任务 2：HTTP 服务 */
    ESP_ERROR_CHECK(upload_server_start());

    /* 任务 1：UDP 发现 */
    ESP_ERROR_CHECK(discovery_start(wifi_sta_netif()));

    ESP_LOGI(TAG, "内部 RAM 空闲 %u B，PSRAM 空闲 %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    bool announced = false;
    int64_t last_heartbeat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!announced && wifi_sta_is_connected()) {
            char ip[16];
            wifi_sta_ip_string(ip, sizeof(ip));
            ESP_LOGI(TAG, "设备已就绪：在浏览器打开 http://%s/ 或用 Swift 上位机自动发现",
                     ip);
            announced = true;
        } else if (announced && !wifi_sta_is_connected()) {
            ESP_LOGW(TAG, "WiFi 断开，等待自动重连 ...");
            announced = false;
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_heartbeat > 60LL * 1000000) {
            last_heartbeat = now;
            epd_state_t st;
            epd_state_snapshot(&st);
            ESP_LOGI(TAG, "心跳：刷新 %u 次，上传成功 %u 次，当前图 %s，堆 %u B",
                     (unsigned)st.refresh_count, (unsigned)st.upload_ok_count,
                     st.current_name[0] ? st.current_name : "（无）",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }
}
