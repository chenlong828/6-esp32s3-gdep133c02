/*
 * GDEP133C02 (13.3", 1200x1600, E Ink Spectra 6) 面板驱动
 *
 * 移植自仓库 firmware/e6_epd 的 7.09" 驱动（GDEB0709E01.c）：
 * 传输骨架、双 CS 时序、错误处理沿用 7.09" 版本；
 * 初始化参数整组换成 13.3" 参考设计（vendor/gdep133c02）的值。
 *
 * 与 7.09" 的关键差异（见 notes/analysis.md §5）：
 *   AN_TM  C0 1E 1E CE CE CE 15 15 55   （7.09 是 00 0C 0C D9 DD DD ...）
 *   PSR    DF 69                        （7.09 是 DF 6B）
 *   CDI    F7                           （7.09 是 37）
 *   BTST_P/N  E8 28                     （7.09 是 E0 20）
 *   DRF    01                           （7.09 是 00）
 *   POF    00                           （7.09 是 01）
 *   另外 13.3" 参考设计没有 PLL / POFS / DCDC / CMDA4 四条命令。
 *
 * SPI 时钟取 10 MHz，与 13.3" 参考设计一致（7.09" 用的是 20 MHz）。
 * 整帧 960 000 B 在 10 MHz 下约 0.77 s，相对 12 s 以上的波形刷新可忽略。
 */

#include "GDEP133C02.h"

#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EPD_SCK_PIN     9
#define EPD_MOSI_PIN    41
#define EPD_MISO_PIN    40
#define EPD_CS_M_PIN    18
#define EPD_CS_S_PIN    17
#define EPD_RST_PIN     6
#define EPD_DC_PIN      2
#define EPD_BUSY_PIN    7
#define EPD_PWR_PIN     45

#define EPD_SPI_CLOCK_HZ    (10 * 1000 * 1000)

/* BUSY 轮询超时：整屏刷新 12~19 s，留足余量 */
#define EPD_BUSY_TIMEOUT_MS 90000

/* ---------------- 13.3" 初始化参数（照抄 vendor/gdep133c02） --------------- */

static const uint8_t PSR_V[2] = {0xDF, 0x69};
static const uint8_t PWR_V[6] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t POF_V[1] = {0x00};
static const uint8_t DRF_V[1] = {0x01};
static const uint8_t CDI_V[1] = {0xF7};
static const uint8_t TCON_V[2] = {0x03, 0x03};
static const uint8_t TRES_V[4] = {0x04, 0xB0, 0x03, 0x20};
static const uint8_t CMD66_V[6] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t EN_BUF_V[1] = {0x07};
static const uint8_t CCSET_V[1] = {0x01};
static const uint8_t PWS_V[1] = {0x22};
static const uint8_t AN_TM_V[9] = {0xC0, 0x1E, 0x1E, 0xCE, 0xCE, 0xCE, 0x15, 0x15, 0x55};
static const uint8_t AGID_V[1] = {0x10};
static const uint8_t BTST_P_V[2] = {0xE8, 0x28};
static const uint8_t BOOST_VDDP_EN_V[1] = {0x01};
static const uint8_t BTST_N_V[2] = {0xE8, 0x28};
static const uint8_t BUCK_BOOST_VDDN_V[1] = {0x01};
static const uint8_t TFT_VCOM_POWER_V[1] = {0x02};

enum {
    CMD_PSR = 0x00, CMD_PWR = 0x01, CMD_POF = 0x02,
    CMD_PON = 0x04, CMD_BTST_N = 0x05, CMD_BTST_P = 0x06,
    CMD_DTM = 0x10, CMD_DRF = 0x12,
    CMD_CDI = 0x50, CMD_TCON = 0x60, CMD_TRES = 0x61,
    CMD_AN_TM = 0x74, CMD_AGID = 0x86,
    CMD_BUCK_BOOST_VDDN = 0xB0,
    CMD_TFT_VCOM_POWER = 0xB1, CMD_EN_BUF = 0xB6,
    CMD_BOOST_VDDP_EN = 0xB7, CMD_CCSET = 0xE0, CMD_PWS = 0xE3,
    CMD_CMD66 = 0xF0
};

static const char *TAG = "GDEP133C02";
static spi_device_handle_t s_spi = NULL;
static uint8_t *s_dma_buf = NULL;

/* ---------------- 底层收发 ---------------- */

static void cs_all(int level)
{
    gpio_set_level(EPD_CS_M_PIN, level);
    gpio_set_level(EPD_CS_S_PIN, level);
}

/* 直接发送一块 DMA 缓冲（调用方保证 data 位于 DMA 可访问内存） */
static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    if (len > GDEP133C02_SIDE_BYTES) return ESP_ERR_INVALID_SIZE;

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t spi_tx(const uint8_t *data, size_t len)
{
    if (len > GDEP133C02_SIDE_BYTES) return ESP_ERR_INVALID_SIZE;
    memcpy(s_dma_buf, data, len);
    return spi_tx_raw(s_dma_buf, len);
}

static esp_err_t send_command(uint8_t cmd)
{
    gpio_set_level(EPD_DC_PIN, 0);
    s_dma_buf[0] = cmd;
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = s_dma_buf;
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t send_data(const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_DC_PIN, 1);
    return spi_tx(data, len);
}

static esp_err_t send_register(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send_command(cmd);
    if (err != ESP_OK) return err;
    return send_data(data, len);
}

static esp_err_t write_master(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_CS_M_PIN, 0);
    esp_err_t err = send_register(cmd, data, len);
    gpio_set_level(EPD_CS_M_PIN, 1);
    return err;
}

/* ---------------- 复位 / BUSY ---------------- */

static void reset_panel(void)
{
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

/*
 * BUSY 低电平 = 忙（与两份厂家示例一致）。
 * 带超时，避免面板没接好时卡死。
 */
static esp_err_t wait_busy_release(void)
{
    const int step_ms = 10;
    int waited = 0;

    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        if (waited >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY 超时（%d ms）", EPD_BUSY_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */

static esp_err_t panel_setup(void);

esp_err_t gdep133c02_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << EPD_RST_PIN) |
                        (1ULL << EPD_DC_PIN) |
                        (1ULL << EPD_CS_M_PIN) |
                        (1ULL << EPD_CS_S_PIN) |
                        (1ULL << EPD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_config_t busy = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&busy));

    gpio_set_level(EPD_CS_M_PIN, 1);
    gpio_set_level(EPD_CS_S_PIN, 1);
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_PWR_PIN, 1);
    gpio_set_level(EPD_RST_PIN, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_MOSI_PIN,
        .miso_io_num = EPD_MISO_PIN,
        .sclk_io_num = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GDEP133C02_SIDE_BYTES
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = 0
    };

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    s_dma_buf = heap_caps_malloc(GDEP133C02_SIDE_BYTES, MALLOC_CAP_DMA);
    if (!s_dma_buf) return ESP_ERR_NO_MEM;

    return panel_setup();
}

/* 复位 + 整套初始化命令；多帧轮播时每帧前重跑一次（与厂家 13.3" 示例一致） */
static esp_err_t panel_setup(void)
{
    reset_panel();
    esp_err_t err = wait_busy_release();
    if (err != ESP_OK) return err;

    cs_all(1);

    /* 上电时序参数：只发给 Master */
    err = write_master(CMD_AN_TM, AN_TM_V, sizeof(AN_TM_V));
    if (err != ESP_OK) return err;

    /* 公用配置：两颗 IC 同时选中 */
    cs_all(0);
    err = send_register(CMD_CMD66, CMD66_V, sizeof(CMD66_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_PSR, PSR_V, sizeof(PSR_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_CDI, CDI_V, sizeof(CDI_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_TCON, TCON_V, sizeof(TCON_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_AGID, AGID_V, sizeof(AGID_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_PWS, PWS_V, sizeof(PWS_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_CCSET, CCSET_V, sizeof(CCSET_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_TRES, TRES_V, sizeof(TRES_V));
    cs_all(1);
    if (err != ESP_OK) return err;

    /* 电源/升压：只发给 Master（与 13.3" 参考设计一致） */
    err = write_master(CMD_PWR, PWR_V, sizeof(PWR_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_EN_BUF, EN_BUF_V, sizeof(EN_BUF_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_BTST_P, BTST_P_V, sizeof(BTST_P_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_BOOST_VDDP_EN, BOOST_VDDP_EN_V, sizeof(BOOST_VDDP_EN_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_BTST_N, BTST_N_V, sizeof(BTST_N_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V, sizeof(BUCK_BOOST_VDDN_V));
    if (err != ESP_OK) return err;
    err = write_master(CMD_TFT_VCOM_POWER, TFT_VCOM_POWER_V, sizeof(TFT_VCOM_POWER_V));
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "初始化完成（1200x1600，双 IC）");
    return ESP_OK;
}

/*
 * 多帧轮播时，每帧前重跑一次初始化（厂家 13.3" 示例就是这么做的：
 * 每张图显示完 POF 后，下一张前重新 initEPD()）。
 * 要求 gdep133c02_init() 已经跑过，SPI 总线已建立。
 */
esp_err_t gdep133c02_reinit(void)
{
    if (s_spi == NULL) return ESP_ERR_INVALID_STATE;
    return panel_setup();
}

/* ---------------- 刷屏 ---------------- */

/* PON → 等 BUSY → DRF → 等 BUSY → POF → 等 BUSY（13.3" 参考设计顺序） */
static esp_err_t turn_on_display(void)
{
    esp_err_t err;

    cs_all(0);
    err = send_command(CMD_PON);
    cs_all(1);
    if (err != ESP_OK) return err;
    err = wait_busy_release();
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(30));

    cs_all(0);
    err = send_register(CMD_DRF, DRF_V, sizeof(DRF_V));
    cs_all(1);
    if (err != ESP_OK) return err;
    err = wait_busy_release();
    if (err != ESP_OK) return err;

    cs_all(0);
    err = send_register(CMD_POF, POF_V, sizeof(POF_V));
    cs_all(1);
    if (err != ESP_OK) return err;
    err = wait_busy_release();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "刷新完成");
    return ESP_OK;
}

/*
 * 每颗 IC 收 1600 行 × 300 B：
 *   Master 取每行前 300 B（左半 600 像素）
 *   Slave  取每行后 300 B（右半 600 像素）
 * DTM(0x10) 每颗 IC 只发一次，期间 CS 必须一直保持低电平。
 */
static esp_err_t send_half(const uint8_t *image, bool master)
{
    const uint32_t row_bytes = GDEP133C02_BYTES_PER_ROW;
    const uint32_t side_bytes = GDEP133C02_SIDE_BYTES;
    const uint32_t offset = master ? 0 : side_bytes;

    gpio_set_level(master ? EPD_CS_M_PIN : EPD_CS_S_PIN, 0);

    esp_err_t err = send_command(CMD_DTM);
    if (err == ESP_OK) {
        for (uint32_t row = 0; row < GDEP133C02_HEIGHT; row++) {
            err = send_data(image + row * row_bytes + offset, side_bytes);
            if (err != ESP_OK) break;
        }
    }

    gpio_set_level(master ? EPD_CS_M_PIN : EPD_CS_S_PIN, 1);
    return err;
}

esp_err_t gdep133c02_display(const uint8_t *image)
{
    if (image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = send_half(image, true);
    if (err != ESP_OK) return err;

    err = send_half(image, false);
    if (err != ESP_OK) return err;

    return turn_on_display();
}

esp_err_t gdep133c02_clear(uint8_t color)
{
    /* 4bit 像素码打包：color << 4 | color，例如白 0x11 */
    uint8_t packed = (uint8_t)((color << 4) | color);
    memset(s_dma_buf, packed, GDEP133C02_SIDE_BYTES);

    for (int master = 1; master >= 0; master--) {
        gpio_set_level(master ? EPD_CS_M_PIN : EPD_CS_S_PIN, 0);

        esp_err_t err = send_command(CMD_DTM);
        if (err == ESP_OK) {
            gpio_set_level(EPD_DC_PIN, 1);
            for (uint32_t row = 0; row < GDEP133C02_HEIGHT; row++) {
                err = spi_tx_raw(s_dma_buf, GDEP133C02_SIDE_BYTES);
                if (err != ESP_OK) break;
            }
        }

        gpio_set_level(master ? EPD_CS_M_PIN : EPD_CS_S_PIN, 1);
        if (err != ESP_OK) return err;
    }

    return turn_on_display();
}

esp_err_t gdep133c02_sleep(void)
{
    /* POF(0x02) + 0xA5 关电源，再拉低面板供电（沿用 7.09" 已验证的收尾方式） */
    cs_all(0);
    esp_err_t err = send_command(CMD_POF);
    if (err == ESP_OK) {
        err = send_data((const uint8_t[]){0xA5}, 1);
    }
    cs_all(1);
    if (err != ESP_OK) return err;

    err = wait_busy_release();
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PWR_PIN, 0);
    ESP_LOGI(TAG, "面板已休眠");
    return ESP_OK;
}
