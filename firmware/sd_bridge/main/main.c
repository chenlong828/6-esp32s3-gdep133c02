#include "esp_err.h"
#include "esp_log.h"

#include "bridge.h"
#include "sd_card.h"

static const char *TAG = "sd_bridge";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-133C02-IL SD card bridge");

    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) {
        /* Keep running: the host can inspect the error and retry MOUNT. */
        ESP_LOGE(TAG, "SD mount failed: %s (%s)", esp_err_to_name(err),
                 sd_card_last_error());
    }

    bridge_run();
}
