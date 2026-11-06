/*
 * WiFi 连通性测试（独立工程，不碰 EPD 屏和 SD 卡）
 *
 * 流程：
 *   1. 以 STA 模式连接 .env 中指定的 AP（SSID / 密码由构建期生成的头文件注入）；
 *   2. 拿到 DHCP 租约后打印：STA MAC、主机名、BSSID、信道、RSSI、认证方式、
 *      IP、子网掩码、网关、DNS（主/备/兜底）；
 *   3. 对网关发 4 个 ICMP 包，验证二层/三层连通性；
 *   4. 断线按 2s→4s…→30s 退避自动重连，重连后再次打印整套信息；在线时每 30s
 *      打印一次 RSSI 心跳。
 *
 * 凭据来源：仓库根 .env（见 .env.example），CMake 在构建时生成
 * build/.../wifi_secrets.h 注入。源码与 git 中不含密码；注意编译产物里会带，
 * 不要外发 build/ 目录或固件镜像。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "discovery.h"
#include "wifi_secrets.h"

static const char *TAG = "wifi-check";

#define WIFI_CONNECTED_BIT BIT0

#define IP4_STR_MAX 16

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_retry_timer;
static SemaphoreHandle_t s_ping_done;
static int s_retry_count;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static const char *ip4_str(const esp_ip4_addr_t *addr, char *buf)
{
    esp_ip4addr_ntoa(addr, buf, IP4_STR_MAX);
    return buf;
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:            return "找不到 AP（SSID 错误 / AP 未开 / 信号太弱）";
    case WIFI_REASON_AUTH_FAIL:              return "认证失败（密码错误？）";
    case WIFI_REASON_ASSOC_FAIL:             return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "四次握手超时（密码错误？信号差？）";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时";
    case WIFI_REASON_CONNECTION_FAIL:        return "连接失败";
    case WIFI_REASON_BEACON_TIMEOUT:         return "信标超时";
    case WIFI_REASON_AUTH_EXPIRE:            return "认证过期";
    default:                                 return "其他原因";
    }
}

static const char *authmode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "OPEN（开放）";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    default:                        return "其他/企业级";
    }
}

/* ------------------------------------------------------------------ */
/* 连接 / 重连                                                         */
/* ------------------------------------------------------------------ */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "正在重连 ...");
    esp_wifi_connect();
}

/* 1s,2s,4s,8s,16s,30s（封顶） */
static uint32_t backoff_ms(void)
{
    int shift = s_retry_count > 5 ? 5 : s_retry_count;
    uint32_t ms = 1000U << shift;
    return ms > 30000U ? 30000U : ms;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动，开始连接 \"%s\" ...", WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *ev = data;
            ESP_LOGI(TAG, "已关联 AP：SSID=%s 信道=%u BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
                     (const char *)ev->ssid, ev->channel,
                     ev->bssid[0], ev->bssid[1], ev->bssid[2],
                     ev->bssid[3], ev->bssid[4], ev->bssid[5]);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = data;
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            s_retry_count++;
            uint32_t delay = backoff_ms();
            ESP_LOGW(TAG, "连接断开：reason=%d（%s），%u ms 后重连（第 %d 次）",
                     ev->reason, disconnect_reason_str(ev->reason),
                     (unsigned)delay, s_retry_count);
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
        char ip[IP4_STR_MAX], mask[IP4_STR_MAX], gw[IP4_STR_MAX];
        ESP_LOGI(TAG, "DHCP 成功：IP=%s 掩码=%s 网关=%s",
                 ip4_str(&ev->ip_info.ip, ip),
                 ip4_str(&ev->ip_info.netmask, mask),
                 ip4_str(&ev->ip_info.gw, gw));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/* 网络信息打印                                                        */
/* ------------------------------------------------------------------ */

static void print_link_info(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "STA MAC  : %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    const char *hostname = NULL;
    if (esp_netif_get_hostname(s_sta_netif, &hostname) == ESP_OK && hostname != NULL) {
        ESP_LOGI(TAG, "主机名   : %s", hostname);
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "BSSID    : %02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        ESP_LOGI(TAG, "信道/RSSI: %u / %d dBm", ap.primary, ap.rssi);
        ESP_LOGI(TAG, "认证方式 : %s", authmode_str(ap.authmode));
    } else {
        ESP_LOGW(TAG, "读取 AP 信息失败");
    }
}

static void print_dns(void)
{
    static const struct {
        esp_netif_dns_type_t type;
        const char *label;
    } dns_list[] = {
        { ESP_NETIF_DNS_MAIN,     "主" },
        { ESP_NETIF_DNS_BACKUP,   "备" },
        { ESP_NETIF_DNS_FALLBACK, "兜底" },
    };

    char buf[IP4_STR_MAX];
    for (size_t i = 0; i < sizeof(dns_list) / sizeof(dns_list[0]); i++) {
        esp_netif_dns_info_t dns = { 0 };
        if (esp_netif_get_dns_info(s_sta_netif, dns_list[i].type, &dns) != ESP_OK) {
            continue;
        }
        if (dns.ip.u_addr.ip4.addr == 0) {
            ESP_LOGI(TAG, "DNS(%s): （未分配）", dns_list[i].label);
            continue;
        }
        ESP_LOGI(TAG, "DNS(%s): %s", dns_list[i].label,
                 ip4_str(&dns.ip.u_addr.ip4, buf));
    }
}

/* ------------------------------------------------------------------ */
/* ping 网关                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int ok;
    int fail;
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t sum_ms;
} ping_stats_t;

static ping_stats_t s_ping_stats;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    ping_stats_t *st = (ping_stats_t *)args;
    uint16_t seq = 0;
    uint32_t elapsed = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));

    st->ok++;
    st->sum_ms += elapsed;
    if (st->ok == 1 || elapsed < st->min_ms) {
        st->min_ms = elapsed;
    }
    if (elapsed > st->max_ms) {
        st->max_ms = elapsed;
    }
    ESP_LOGI(TAG, "ping 回包：seq=%u %u ms", seq, (unsigned)elapsed);
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    ping_stats_t *st = (ping_stats_t *)args;
    uint16_t seq = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    st->fail++;
    ESP_LOGW(TAG, "ping 超时：seq=%u", seq);
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    xSemaphoreGive(s_ping_done);
}

static bool ping_gateway(const esp_ip4_addr_t *gw)
{
    char gw_str[IP4_STR_MAX];
    if (gw->addr == 0) {
        ESP_LOGW(TAG, "网关地址为 0，跳过连通性测试");
        return false;
    }

    ip4_str(gw, gw_str);
    ESP_LOGI(TAG, "开始 ping 网关 %s（4 次）...", gw_str);

    memset(&s_ping_stats, 0, sizeof(s_ping_stats));
    xSemaphoreTake(s_ping_done, 0); /* 清掉可能残留的信号量 */

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 4;
    cfg.interval_ms = 500;
    cfg.timeout_ms = 1000;
    IP_ADDR4(&cfg.target_addr,
             esp_ip4_addr1_16(gw), esp_ip4_addr2_16(gw),
             esp_ip4_addr3_16(gw), esp_ip4_addr4_16(gw));

    esp_ping_callbacks_t cbs = {
        .cb_args = &s_ping_stats,
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
    };

    esp_ping_handle_t hdl = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 ping 会话失败：%s", esp_err_to_name(err));
        return false;
    }

    err = esp_ping_start(hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 ping 失败：%s", esp_err_to_name(err));
        esp_ping_delete_session(hdl);
        return false;
    }

    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "ping 未在 15 秒内结束（强制结束）");
    }
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);

    if (s_ping_stats.ok > 0) {
        ESP_LOGI(TAG, "ping 结果：%d/%d 回包，min/avg/max = %u/%u/%u ms —— 网关可达",
                 s_ping_stats.ok, (int)cfg.count,
                 (unsigned)s_ping_stats.min_ms,
                 (unsigned)(s_ping_stats.sum_ms / (uint32_t)s_ping_stats.ok),
                 (unsigned)s_ping_stats.max_ms);
        return true;
    }

    ESP_LOGW(TAG, "ping 结果：0/%d 回包 —— 网关不响应 ICMP（路由器可能禁 ping，或二层不通）",
             (int)cfg.count);
    return false;
}

/* ------------------------------------------------------------------ */
/* 报告任务                                                            */
/* ------------------------------------------------------------------ */

static void report_once(void)
{
    print_link_info();

    esp_netif_ip_info_t ip_info = { 0 };
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "读取 IP 信息失败");
        return;
    }

    char ip[IP4_STR_MAX], mask[IP4_STR_MAX], gw[IP4_STR_MAX];
    ESP_LOGI(TAG, "IP       : %s", ip4_str(&ip_info.ip, ip));
    ESP_LOGI(TAG, "子网掩码 : %s", ip4_str(&ip_info.netmask, mask));
    ESP_LOGI(TAG, "网关     : %s", ip4_str(&ip_info.gw, gw));

    print_dns();
    ping_gateway(&ip_info.gw);

    ESP_LOGI(TAG, "===== 基础信息读取完毕，画面保持空闲 =====");
}

static void report_task(void *arg)
{
    (void)arg;

    for (;;) {
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(100)); /* 等 DHCP/DNS 收敛 */

        ESP_LOGI(TAG, "===== 已联网，读取网络信息 =====");
        report_once();

        while (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                ESP_LOGI(TAG, "心跳：RSSI=%d dBm 信道=%u", ap.rssi, ap.primary);
            }
        }
        ESP_LOGW(TAG, "连接已断开，等待自动重连 ...");
    }
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, "epd-wifi-check"));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_wifi_events = xEventGroupCreate();
    configASSERT(s_wifi_events != NULL);

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    s_ping_done = xSemaphoreCreateBinary();
    configASSERT(s_ping_done != NULL);

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; /* 不限制加密方式，交给 AP 协商 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    /* 关闭省电，避免 ping 时延抖动，也方便观察 RSSI */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    const size_t ssid_len = strlen(WIFI_SSID);
    const size_t pass_len = strlen(WIFI_PASSWORD);

    ESP_LOGI(TAG, "===== WiFi 连通性测试 =====");
    ESP_LOGI(TAG, "SSID：%s（%u 字节），密码来自 .env（%u 字节，不打印明文）",
             WIFI_SSID, (unsigned)ssid_len, (unsigned)pass_len);

    if (ssid_len == 0 || ssid_len > 32) {
        ESP_LOGW(TAG, "SSID 长度应为 1~32 字节，当前 %u 字节", (unsigned)ssid_len);
    }
    if (pass_len < 8 || pass_len > 63) {
        ESP_LOGW(TAG, "WPA2/WPA3 口令应为 8~63 字节，当前 %u 字节，认证大概率失败",
                 (unsigned)pass_len);
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_init_sta();

    esp_err_t disc_err = discovery_start(s_sta_netif);
    if (disc_err != ESP_OK) {
        ESP_LOGE(TAG, "UDP 发现服务启动失败：%s", esp_err_to_name(disc_err));
    }

    xTaskCreate(report_task, "report", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
