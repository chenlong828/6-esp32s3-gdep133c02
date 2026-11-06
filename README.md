# s6-esp32s3-gdep133c02

ESP32-S3 驱动佳显（Good Display）**13.3 英寸 E Ink Spectra 6（S6）** 彩色电子纸 —— GDEP133C02。
仓库内容：固件工程、上位机出图工具、面板资料、案例笔记。

**Topics**: `eink` `epaper` `spectra6` `esp32s3` `gdep133c02` `e6` `spi` `dithering`

| 项 | 值 |
|---|---|
| 面板 | GDEP133C02，13.3"，1200×1600，6 色（黑/白/红/黄/蓝/绿），4bpp，150 PPI，刷新 ~12 s |
| 驱动 IC | 双 IC（Master/Slave），每颗管 600 列，每行共 600 字节 |
| 开发板 | 佳显 ESP32-133C02-IL：ESP32-S3R8（16 MB Flash / 8 MB PSRAM），Type-C 原生 USB |
| 串口 | `/dev/cu.usbmodem1101`（USB-Serial-JTAG，免驱） |
| 单帧数据量 | 960 000 字节（= 1200×1600/2，每字节 2 像素，高 nibble 在左） |

## 目录

```
docs/           面板资料（规格书/设计须知/板级手册/原理图/烧录指南）+ 厂家示例 zip + 文本提取
docs/preview/     机械图、原理图、image.h 解码图
docs/reference/   官网在线转换器原页面与逆向 JS
host/           上位机出图工具 e6_convert.py + samples/（示例 bin 与预览图）
firmware/e6_epd/  固件工程（当前以厂家 7.09" 示例为基线，待适配 13.3"）
vendor/         厂家原始示例源码（13.3" 参考设计 / 7.09" 双环境版）
notes/          调研与分析笔记（产品参数、代码分析、出图链路、macOS 环境、板子体检）
tools/          构建/烧录/备份脚本
```

## 快速开始

```bash
# 1) ESP-IDF（已装在 ~/esp/esp-idf-v5.4.4，工具在 ~/.espressif）
. tools/env.sh   # 内含 export.sh + IDF_SKIP_CHECK_SUBMODULES

# 2) 编译固件
cd firmware/e6_epd && idf.py set-target esp32s3 && idf.py build

# 3) 烧录（日志走 UART0，本板未引出；USB 上只有 ROM 日志）
idf.py -p /dev/cu.usbmodem1101 flash

# 4) 上位机出图
./.venv/bin/python host/e6_convert.py photo.jpg --fit cover --dither fs-ser \
    -o out/photo.bin --header out/photo_image.h --preview out/photo.png
```

## 关键结论（详见 notes/）

- **固件只认 960 000 字节裸数据**，旋转/缩放/抖动全部在上位机完成。
- 抖动要对着**面板实测色**算误差（红 `160,32,32`、黄 `240,224,80`、蓝 `80,128,184`、绿 `96,128,80`），不是纯 RGB。
- `images.bin` 格式：`"EPD6BIN\0" + u16 w + u16 h + u16 frames + u32 interval_ms + 帧数据`。
- 板子现有固件是厂家 SD 播放版（Arduino 构建，IDF 5.5，2025-07-22），读 `/images.bin` 并解析头。
- 6 色无灰阶，中间色靠空间抖动；可辨色约几百种，纯色块观感接近印刷，照片会显颗粒。

## 大文件策略

`docs/*.pdf`、`docs/*.zip`（约 100 MB）不入库，本地保留；`.gitignore` 已排除。

## 环境

- ESP-IDF **v5.4.4**：`~/esp/esp-idf-v5.4.4`
- Python 3.12（uv 管理）：仓库内 `.venv`（numpy / pillow / pyserial / esptool）
- 13.3" 厂家参考设计需 **IDF 4.4.4**（旧式 `component.mk`），需要原样编译时另装
