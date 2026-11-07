/* WiFi STA：连接 .env 注入的 AP，断线指数退避重连，对外提供 IP 查询。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t wifi_sta_start(const char *hostname);

/* 阻塞等待首次拿到 IP；超时返回 false（服务仍会在后续连上时可用） */
bool wifi_sta_wait_connected(uint32_t timeout_ms);

bool wifi_sta_is_connected(void);

esp_netif_t *wifi_sta_netif(void);

/* 已联网时填 IP，未联网填 "0.0.0.0" */
void wifi_sta_ip_string(char *buf, size_t len);
