/* 任务 1：UDP 局域网发现服务。 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t discovery_start(esp_netif_t *netif);
