#include "epd_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "epd_config.h"
#include "wifi_sta.h"

static const char *TAG = "state";

static epd_state_t s_state;
static SemaphoreHandle_t s_lock;

void epd_state_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_state, 0, sizeof(s_state));
    s_state.last_display_err = ESP_OK;
    ESP_LOGI(TAG, "状态模块就绪");
}

static inline void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void epd_state_snapshot(epd_state_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = s_state;
    unlock();
}

void epd_state_set_sd(bool mounted, uint64_t total, uint64_t free_bytes)
{
    lock();
    s_state.sd_mounted = mounted;
    s_state.sd_total_bytes = total;
    s_state.sd_free_bytes = free_bytes;
    unlock();
}

void epd_state_set_panel(bool ready)
{
    lock();
    s_state.panel_ready = ready;
    unlock();
}

void epd_state_begin_display(void)
{
    lock();
    s_state.display_busy = true;
    unlock();
}

void epd_state_end_display(esp_err_t err, uint32_t duration_ms,
                           const char *name, uint32_t crc, uint32_t saved_at,
                           uint32_t bytes)
{
    lock();
    s_state.display_busy = false;
    s_state.last_display_err = (int)err;
    s_state.last_refresh_ms = duration_ms;
    s_state.last_refresh_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (err == ESP_OK) {
        s_state.refresh_count++;
        if (name != NULL) {
            strlcpy(s_state.current_name, name, sizeof(s_state.current_name));
        }
        s_state.current_crc = crc;
        s_state.current_saved_at = saved_at;
        s_state.current_bytes = bytes;
    }
    unlock();
}

void epd_state_upload_begin(uint32_t total)
{
    lock();
    s_state.upload_active = true;
    s_state.upload_received = 0;
    s_state.upload_total = total;
    unlock();
}

void epd_state_upload_progress(uint32_t received)
{
    lock();
    s_state.upload_received = received;
    unlock();
}

void epd_state_upload_end(bool ok, uint32_t bytes, uint32_t ms)
{
    lock();
    s_state.upload_active = false;
    s_state.upload_received = 0;
    s_state.upload_total = 0;
    if (ok) {
        s_state.upload_ok_count++;
        s_state.last_upload_bytes = bytes;
        s_state.last_upload_ms = ms;
    } else {
        s_state.upload_fail_count++;
    }
    unlock();
}

static void mac_string(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void ip_string(char *buf, size_t len)
{
    /* 由调用方保证 netif 已初始化；discovery / http 都在 netif 之后启动 */
    wifi_sta_ip_string(buf, len);
}

void epd_state_fill_json(cJSON *root)
{
    if (root == NULL) {
        return;
    }

    epd_state_t st;
    epd_state_snapshot(&st);

    char mac[24] = "-";
    mac_string(mac, sizeof(mac));

    char ip[16] = "0.0.0.0";
    ip_string(ip, sizeof(ip));

    cJSON_AddStringToObject(root, "device", EPD_DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware", EPD_FW_VERSION);
    cJSON_AddStringToObject(root, "hostname", EPD_HOSTNAME);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "http_port", EPD_HTTP_PORT);
    cJSON_AddNumberToObject(root, "discovery_port", EPD_DISCOVERY_PORT);

    cJSON *panel = cJSON_AddObjectToObject(root, "panel");
    cJSON_AddNumberToObject(panel, "width", 1200);
    cJSON_AddNumberToObject(panel, "height", 1600);
    cJSON_AddNumberToObject(panel, "colors", 6);
    cJSON_AddNumberToObject(panel, "frame_bytes", 960000);
    cJSON_AddBoolToObject(panel, "ready", st.panel_ready);

    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddBoolToObject(sd, "mounted", st.sd_mounted);
    cJSON_AddNumberToObject(sd, "total_bytes", (double)st.sd_total_bytes);
    cJSON_AddNumberToObject(sd, "free_bytes", (double)st.sd_free_bytes);

    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    cJSON_AddBoolToObject(disp, "busy", st.display_busy);
    cJSON_AddNumberToObject(disp, "refresh_count", st.refresh_count);
    cJSON_AddNumberToObject(disp, "last_refresh_ms", st.last_refresh_ms);
    cJSON_AddNumberToObject(disp, "last_refresh_uptime_s", st.last_refresh_uptime_s);
    cJSON_AddStringToObject(disp, "last_error", esp_err_to_name((esp_err_t)st.last_display_err));
    cJSON_AddStringToObject(disp, "current_name", st.current_name);
    cJSON_AddNumberToObject(disp, "current_crc", (double)st.current_crc);
    cJSON_AddNumberToObject(disp, "current_saved_at", (double)st.current_saved_at);
    cJSON_AddNumberToObject(disp, "current_bytes", st.current_bytes);

    cJSON *up = cJSON_AddObjectToObject(root, "upload");
    cJSON_AddBoolToObject(up, "active", st.upload_active);
    cJSON_AddNumberToObject(up, "received", st.upload_received);
    cJSON_AddNumberToObject(up, "total", st.upload_total);
    cJSON_AddNumberToObject(up, "ok_count", st.upload_ok_count);
    cJSON_AddNumberToObject(up, "fail_count", st.upload_fail_count);
    cJSON_AddNumberToObject(up, "last_bytes", st.last_upload_bytes);
    cJSON_AddNumberToObject(up, "last_ms", st.last_upload_ms);
}
