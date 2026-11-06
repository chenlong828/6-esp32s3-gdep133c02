# e6_epd —— GDEP133C02 13.3" 内嵌图固件

ESP-IDF 5.4.4 工程，驱动佳显 **GDEP133C02**（13.3"，E Ink Spectra 6，1200×1600，双 IC）。

行为：上电 → 初始化面板 → 把**编进固件**的一帧图像直接刷到屏上 → 休眠，画面保持。
不读 SD 卡、不联网、不依赖文件系统。

## 数据

- 4 bit/pixel，1 字节 2 像素（高 nibble 在左），整帧 1200 × 1600 / 2 = **960 000 B**。
- 每行 600 B：前 300 B 给 Master（左 600 px），后 300 B 给 Slave（右 600 px）。
- 图像容器是上位机产物 `out/genshin01.bin`（`EPD6BIN` 头 + 帧数据，见根目录 `host/e6_convert.py`）：
  构建时顶层 `CMakeLists.txt` 会把它同步到 `main/assets/`，再由 `EMBED_FILES` 打包进固件。
- 固件运行时解析头里的 width/height/frames/interval 并逐帧显示，**不做任何旋转/抖动**；多帧时每帧前重跑一次面板初始化。

换图：把新图导出成同格式 bin，覆盖 `out/genshin01.bin`（或改上方 CMakeLists 里的路径），重新 `idf.py build` 即可。

## GPIO（ESP32-133C02-IL 板）

| 信号 | GPIO |
|---|---:|
| SCK | 9 |
| MOSI | 41 |
| MISO | 40 |
| CS Master | 18 |
| CS Slave | 17 |
| RST | 6 |
| DC | 2 |
| BUSY | 7 |
| PWR | 45 |

SPI 用 SPI2_HOST @ **10 MHz**（与 13.3" 参考设计一致），DMA 逐行发送。

## 驱动文件

| 文件 | 说明 |
|---|---|
| `main/GDEP133C02.c/.h` | 13.3" 面板驱动（初始化参数照抄 `vendor/gdep133c02`） |
| `main/GDEB0709E01.c/.h` | 7.09" 驱动，**保留作参考、不参与编译** |
| `main/image.h` | 7.09" 示例图（960 000 B），**当前未使用**，留作对照 |
| `main/assets/genshin01.bin` | 内嵌图数据，构建时从 `out/` 同步 |

与 7.09" 的差异（见 `notes/analysis.md` §5）：PSR `DF 69`、CDI `F7`、AN_TM `C0 1E 1E CE CE CE 15 15 55`、
BTST `E8 28`、DRF `01`、POF `00`，且不带 PLL / POFS / DCDC / CMDA4 四条命令。

## 编译烧录

```bash
. ../../tools/env.sh          # 载入 ESP-IDF 5.4.4
idf.py set-target esp32s3     # 首次
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

日志走 **USB-Serial-JTAG 二级控制台**（UART0 未引出），可直接：

```bash
idf.py -p /dev/cu.usbmodem1101 monitor
```

实测刷新一次约 **27 s**（1200×1600 全波形），之后 `POF(0x02)+0xA5` 并拉低 PWR 断电。

## 注意

- 面板峰值电流约 1 A，USB 供电别太弱；刷新期间不要断电。
- 出厂固件（厂家 SD 播放版）已整片备份到 `backups/`，恢复方法见 `notes/backup.md`。
- BUSY 等待带 90 s 超时，面板没接好会报 `BUSY 超时` 而不是死等。
