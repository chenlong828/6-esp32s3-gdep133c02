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
host/sd_bridge.py SD 卡上位机：浏览/校验/上传/下载/递归拉取
firmware/e6_epd/  固件工程（13.3" GDEP133C02，内嵌图开机直刷）
firmware/sd_bridge/ SD 卡读写/传输固件（FATFS + USB 二进制协议）
vendor/         厂家原始示例源码（13.3" 参考设计 / 7.09" 双环境版）
notes/          调研与分析笔记（产品参数、代码分析、出图链路、macOS 环境、板子体检）
tools/          构建/烧录/备份脚本
```

## 快速开始

```bash
# 0) 首次碰板先整片备份（写进 backups/，不入库；详见 notes/backup.md）
./tools/backup-factory.sh

# 1) ESP-IDF（已装在 ~/esp/esp-idf-v5.4.4，工具在 ~/.espressif）
. tools/env.sh   # 内含 export.sh + IDF_SKIP_CHECK_SUBMODULES

# 2) 编译固件
cd firmware/e6_epd && idf.py set-target esp32s3 && idf.py build

# 3) 烧录（日志走 USB-Serial-JTAG 二级控制台，可 idf.py monitor 直接看）
idf.py -p /dev/cu.usbmodem1101 flash

# 4) 上位机出图
./.venv/bin/python host/e6_convert.py photo.jpg --fit cover --dither fs-ser \
    -o out/photo.bin --header out/photo_image.h --preview out/photo.png
```

## SD 卡读写 / 导出

板载 microSD 走 SPI3（CS=15 / MOSI=3 / MISO=5 / SCK=8），`firmware/sd_bridge` 把它挂成 FATFS，通过 USB-Serial/JTAG 暴露文件协议；不拆卡、不格式化即可整卡导出或写入。

```bash
# 烧 SD 桥固件（会覆盖当前显示固件；e6_epd 工程未改动，可随时烧回）
cd firmware/sd_bridge && idf.py build && idf.py -p /dev/cu.usbmodem1101 flash

# 看卡、算 CRC、整卡拉取
./.venv/bin/python host/sd_bridge.py info
./.venv/bin/python host/sd_bridge.py crc /images.bin
DEST="backups/sdcard-$(date +%Y%m%d-%H%M%S)"
./.venv/bin/python host/sd_bridge.py pull / "$DEST" --skip-errors --manifest "$DEST-manifest.json"
```

写入使用临时文件 + CRC + 原子改名，目标已存在需 `--overwrite`；固件**绝不格式化**卡。细节与协议见 [`firmware/sd_bridge/README.md`](firmware/sd_bridge/README.md)，调试记录见 [`notes/sd-bridge.md`](notes/sd-bridge.md)。

## 关键结论（详见 notes/）

- **固件只认 960 000 字节裸数据**，旋转/缩放/抖动全部在上位机完成。
- 抖动要对着**面板实测色**算误差（红 `160,32,32`、黄 `240,224,80`、蓝 `80,128,184`、绿 `96,128,80`），不是纯 RGB。
- `images.bin` 格式：`"EPD6BIN\0" + u16 w + u16 h + u16 frames + u32 interval_ms + 帧数据`。
- 出厂固件是厂家 SD 播放版（Arduino 构建，IDF 5.5，2025-07-22），读 `/images.bin`；已整片备份并覆盖为自编 13.3" 固件（见 `notes/backup.md`）。
- 当前 `firmware/e6_epd` 把 `out/genshin01.bin` 内嵌进固件，开机直接刷屏、不读 SD；换图导出同格式 bin 后重新编译即可。
- 6 色无灰阶，中间色靠空间抖动；可辨色约几百种，纯色块观感接近印刷，照片会显颗粒。

## 大文件策略

`docs/*.pdf`、`docs/*.zip`（约 100 MB）不入库，本地保留；`.gitignore` 已排除。

## 环境

- ESP-IDF **v5.4.4**：`~/esp/esp-idf-v5.4.4`
- Python 3.12（uv 管理）：仓库内 `.venv`（numpy / pillow / pyserial / esptool）
- 13.3" 厂家参考设计需 **IDF 4.4.4**（旧式 `component.mk`），需要原样编译时另装
