#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UDP 发现服务端口（上位机 host/discover.py 使用同一端口） */
#define EPD_DISCOVERY_PORT 3232

/* 为后续 WiFi 传图服务预留；0 = 尚未提供 HTTP 服务 */
#define EPD_HTTP_PORT 0

/**
 * @brief 启动 UDP 发现服务。
 *
 * 收到合法查询包（魔数 "EPD6DISC" + 版本 + QUERY）后，向请求来源单播回复：
 * 本机 IP、MAC、主机名、HTTP 端口（预留）。广播和单播查询都会应答。
 *
 * @param netif 已创建的 STA netif（用于实时读取 IP 和主机名）
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t discovery_start(esp_netif_t *netif);

#ifdef __cplusplus
}
#endif
