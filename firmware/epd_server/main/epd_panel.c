#include "epd_panel.h"

#include "esp_log.h"

#include "GDEP133C02.h"

static const char *TAG = "panel";

static bool s_ready;

bool epd_panel_is_ready(void)
{
    return s_ready;
}

esp_err_t epd_panel_prepare(void)
{
    esp_err_t err;

    if (!s_ready) {
        ESP_LOGI(TAG, "首次初始化面板（建立 SPI2 + 完整上电时序）...");
        err = gdep133c02_init();
        if (err == ESP_OK) {
            s_ready = true;
        }
        return err;
    }

    ESP_LOGI(TAG, "重新初始化面板 ...");
    return gdep133c02_reinit();
}

esp_err_t epd_panel_display(const uint8_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return gdep133c02_display(frame);
}

esp_err_t epd_panel_fill(uint8_t color)
{
    return gdep133c02_clear(color);
}
