/* CRC32（IEEE 802.3 / zlib 同款），半字节表实现，约 64 字节表 + 少量循环。
 *
 * 用途：上传时边收边算，写入 current 元数据；刷屏前再算一遍做端到端校验。
 * 与 Python zlib.crc32 / Swift 上位机实现逐位一致（多项式 0xEDB88320，初值
 * 0xFFFFFFFF，末尾取反）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define EPD_CRC32_INIT 0xFFFFFFFFu

uint32_t epd_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t epd_crc32_final(uint32_t crc);
uint32_t epd_crc32(const void *data, size_t len);
