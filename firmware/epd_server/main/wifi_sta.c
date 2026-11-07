/*
 * WiFi STA 连接 / 重连。
 *
 * 逻辑沿用 firmware/wifi_check（已实测）：STA 模式、事件组记录“已拿到 IP”，
 * 断线按 1s→2s→…→30s 退避重连，重连成功后计数归零。
 * 与 wifi_check 的差别：不再打印 DNS / ping，只保留网络层必要信息。
 */

#include "wifi_sta.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "epd_config.h"
#include "wifi_secrets.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static int s_retry_count;

static void retry_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "重连中 ...");
    esp_wifi_connect();
}

static uint32_t backoff_ms(void)
{
    int shift = s_retry_count > 5 ? 5 : s_retry_count;
    uint32_t ms = 1000U << shift;
    return ms > EPD_WIFI_RETRY_MAX_MS ? EPD_WIFI_RETRY_MAX_MS : ms;
}

static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:            return "找不到 AP";
    case WIFI_REASON_AUTH_FAIL:              return "认证失败";
    case WIFI_REASON_ASSOC_FAIL:             return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "四次握手超时";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时";
    case WIFI_REASON_CONNECTION_FAIL:        return "连接失败";
    case WIFI_REASON_BEACON_TIMEOUT:         return "信标超时";
    case WIFI_REASON_AUTH_EXPIRE:            return "认证过期";
    default:                                 return "其他原因";
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 启动，连接 \"%s\" ...", WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *ev = data;
            ESP_LOGI(TAG, "已关联 AP：信道 %u，BSSID %02x:%02x:%02x:%02x:%02x:%02x",
                     ev->channel,
                     ev->bssid[0], ev->bssid[1], ev->bssid[2],
                     ev->bssid[3], ev->bssid[4], ev->bssid[5]);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
            s_retry_count++;
            uint32_t delay = backoff_ms();
            ESP_LOGW(TAG, "连接断开：reason=%d（%s），%u ms 后重连（第 %d 次）",
                     ev->reason, reason_str(ev->reason), (unsigned)delay, s_retry_count);
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000ULL);
            break;
        }

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        char ip[16];
        esp_ip4addr_ntoa(&ev->ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "DHCP 成功：IP=%s（http://%s:%d/）", ip, ip, EPD_HTTP_PORT);
        s_retry_count = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_start(const char *hostname)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_netif, hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    s_events = xEventGroupCreate();
    configASSERT(s_events != NULL);

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    /* 传大文件时关省电，减少丢包与停留 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool wifi_sta_wait_connected(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                          pdFALSE, pdTRUE, ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_sta_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_netif_t *wifi_sta_netif(void)
{
    return s_netif;
}

void wifi_sta_ip_string(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    buf[0] = '\0';

    esp_netif_ip_info_t info = { 0 };
    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &info) == ESP_OK &&
        info.ip.addr != 0) {
        esp_ip4addr_ntoa(&info.ip, buf, len);
    }
    if (buf[0] == '\0') {
        strlcpy(buf, "0.0.0.0", len);
    }
}
