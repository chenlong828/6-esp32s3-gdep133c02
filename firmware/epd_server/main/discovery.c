/*
 * 任务 1：UDP 局域网发现服务
 *
 * 上位机在网段内广播一个固定格式的小包，设备单播回自己的 IP / MAC / 主机名 /
 * HTTP 端口，上位机据此“找到设备并知道该连哪个端口”。
 *
 * 协议（与 firmware/wifi_check、host/discover.py 完全一致，未改字段）：
 *
 * 查询（上位机 → 广播/单播 UDP:3232，12 字节）
 *   0  8  magic "EPD6DISC"
 *   8  1  version = 1
 *   9  1  type    = 0x01 (QUERY)
 *   10 2  nonce（u16 BE，原样回显）
 *
 * 应答（设备 → 来源地址:端口，单播，25 + N 字节）
 *   0  8  magic
 *   8  1  version = 1
 *   9  1  type    = 0x02 (REPLY)
 *   10 2  nonce（回显）
 *   12 4  IP（u32 BE，网络字节序）
 *   16 6  MAC（STA）
 *   22 2  HTTP 端口（u16 BE）—— 本固件填 EPD_HTTP_PORT
 *   24 1  hostname 长度 N
 *   25 N  hostname（UTF-8，无结尾 0）
 *
 * 设计要点：
 *   - socket 绑定 INADDR_ANY，255.255.255.255 与子网定向广播都能收到；
 *   - IP / 主机名在回包时实时读取，DHCP 换 IP 后无需重启服务；
 *   - 未联网也回包，但 IP 为 0.0.0.0，上位机据此区分“在线但未就绪”。
 */

#include "discovery.h"

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include "epd_config.h"

#define DISC_MAGIC        "EPD6DISC"
#define DISC_MAGIC_LEN    8
#define DISC_VERSION      1
#define DISC_TYPE_QUERY   0x01
#define DISC_TYPE_REPLY   0x02

#define DISC_PROBE_MIN_LEN  12
#define DISC_REPLY_HEAD_LEN 25
#define DISC_HOSTNAME_MAX   32

#define DISC_TASK_STACK 4096
#define DISC_TASK_PRIO  4
#define DISC_LOG_EVERY  10

static const char *TAG = "discovery";

static esp_netif_t *s_netif;
static uint32_t s_query_count;

static void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static bool is_query(const uint8_t *buf, int len)
{
    return len >= DISC_PROBE_MIN_LEN &&
           memcmp(buf, DISC_MAGIC, DISC_MAGIC_LEN) == 0 &&
           buf[8] == DISC_VERSION &&
           buf[9] == DISC_TYPE_QUERY;
}

static size_t build_reply(const uint8_t *probe, uint8_t *out, size_t cap)
{
    esp_netif_ip_info_t ip_info = { 0 };
    if (s_netif != NULL) {
        esp_netif_get_ip_info(s_netif, &ip_info);
    }

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char hostname[DISC_HOSTNAME_MAX] = "";
    const char *name = NULL;
    if (s_netif != NULL &&
        esp_netif_get_hostname(s_netif, &name) == ESP_OK && name != NULL) {
        strlcpy(hostname, name, sizeof(hostname));
    }
    const size_t host_len = strlen(hostname);

    if (cap < DISC_REPLY_HEAD_LEN + host_len) {
        return 0;
    }

    memcpy(out, DISC_MAGIC, DISC_MAGIC_LEN);
    out[8] = DISC_VERSION;
    out[9] = DISC_TYPE_REPLY;
    out[10] = probe[10];
    out[11] = probe[11];
    memcpy(out + 12, &ip_info.ip.addr, 4); /* esp_ip4_addr_t.addr 已是网络字节序 */
    memcpy(out + 16, mac, 6);
    put_u16be(out + 22, EPD_HTTP_PORT);
    out[24] = (uint8_t)host_len;
    memcpy(out + 25, hostname, host_len);

    return DISC_REPLY_HEAD_LEN + host_len;
}

static void discovery_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败：errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(EPD_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "绑定 UDP %u 失败：errno=%d",
                 (unsigned)EPD_DISCOVERY_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP 发现服务已启动：端口 %u，回包 HTTP 端口 %u",
             (unsigned)EPD_DISCOVERY_PORT, (unsigned)EPD_HTTP_PORT);

    uint8_t probe[128];
    uint8_t reply[DISC_REPLY_HEAD_LEN + 255];

    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int len = recvfrom(sock, probe, sizeof(probe), 0,
                           (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "recvfrom 失败：errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!is_query(probe, len)) {
            continue; /* 端口上可能有杂包，静默丢弃 */
        }

        size_t reply_len = build_reply(probe, reply, sizeof(reply));
        if (reply_len == 0) {
            continue;
        }

        const int sent = sendto(sock, reply, reply_len, 0,
                                (struct sockaddr *)&from, from_len);
        if (sent < 0) {
            /* DHCP 之前接口没有 IP 时 sendto 会 ENOTCONN，属正常，降级到 DEBUG */
            if (errno == ENOTCONN) {
                ESP_LOGD(TAG, "尚未联网，跳过回复");
            } else {
                ESP_LOGW(TAG, "回复失败：errno=%d", errno);
            }
            continue;
        }

        s_query_count++;
        char src[16] = "?";
        inet_ntoa_r(from.sin_addr, src, sizeof(src));
        if (s_query_count == 1 || (s_query_count % DISC_LOG_EVERY) == 0) {
            ESP_LOGI(TAG, "已回复第 %u 次查询：%s:%u → %u 字节",
                     (unsigned)s_query_count, src,
                     (unsigned)ntohs(from.sin_port), (unsigned)reply_len);
        } else {
            ESP_LOGD(TAG, "已回复查询：%s:%u", src, (unsigned)ntohs(from.sin_port));
        }
    }
}

esp_err_t discovery_start(esp_netif_t *netif)
{
    s_netif = netif;

    BaseType_t ok = xTaskCreate(discovery_task, "discovery", DISC_TASK_STACK,
                                NULL, DISC_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
