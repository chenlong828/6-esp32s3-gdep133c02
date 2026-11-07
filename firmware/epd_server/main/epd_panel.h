/*
 * 面板高层封装：把“首次初始化 / 后续重新初始化”的顺序收在一处。
 *
 * 厂家 13.3" 示例与 e6_epd 的多帧轮播都是：第一帧前走一次完整 init，
 * 之后每帧前再跑一次 initEPD()（复位 + 整套命令），然后 DTM → DRF。
 * 本模块只记录“是否已经初始化过”，不改变驱动时序。
 *
 * 线程约束：只允许刷屏任务（任务 3）调用，内部不做加锁。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 第一次调用做完整初始化，之后每次调用做面板重新初始化 */
esp_err_t epd_panel_prepare(void);

/* 把 960 000 字节的一帧推给面板并触发刷新（阻塞到 POF 完成，约 12~19 s） */
esp_err_t epd_panel_display(const uint8_t *frame);

/* 整屏刷成单色，用于硬件自检（color 取 GDEP133C02_* 宏） */
esp_err_t epd_panel_fill(uint8_t color);

bool epd_panel_is_ready(void);
