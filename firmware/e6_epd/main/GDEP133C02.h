#ifndef GDEP133C02_H
#define GDEP133C02_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Good Display GDEP133C02 —— 13.3 英寸 E Ink Spectra 6（六色）
 *
 *   ｜<--------------- 1200 像素 --------------->|
 *   ｜<---- 600 ---->|<--------- 600 ----------->|
 *   ｜   Master IC   |        Slave IC          |
 *
 * 每像素 4 bit，1 字节打包 2 个像素（高 nibble 在左）；
 * 整帧 1200 × 1600 / 2 = 960 000 字节。
 * 每行 600 B：前 300 B 给 Master，后 300 B 给 Slave。
 */
#define GDEP133C02_WIDTH         1200
#define GDEP133C02_HEIGHT        1600
#define GDEP133C02_BYTES_PER_ROW 600
#define GDEP133C02_SIDE_BYTES    300
#define GDEP133C02_IMAGE_SIZE    (GDEP133C02_WIDTH * GDEP133C02_HEIGHT / 2)

/* 4bit 像素码：4 和 7 不用 */
#define GDEP133C02_BLACK   0x0
#define GDEP133C02_WHITE   0x1
#define GDEP133C02_YELLOW  0x2
#define GDEP133C02_RED     0x3
#define GDEP133C02_BLUE    0x5
#define GDEP133C02_GREEN   0x6

esp_err_t gdep133c02_init(void);
esp_err_t gdep133c02_reinit(void);
esp_err_t gdep133c02_display(const uint8_t *image);
esp_err_t gdep133c02_clear(uint8_t color);
esp_err_t gdep133c02_sleep(void);

#endif
