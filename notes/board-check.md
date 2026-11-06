# 开发板连接检查记录（2026-11-06）

> 2026-11-06 18:42 起板上已换成自编 13.3" 固件（`firmware/e6_epd`），下面第 4/5 节是**出厂状态快照**；
> 出厂 16 MB 镜像见 [backup.md](backup.md)，移植过程见 [port-13.3.md](port-13.3.md)。

## 1. 连接状态

| 项 | 值 |
|---|---|
| 串口 | `/dev/cu.usbmodem1101`（`/dev/tty.usbmodem1101` 同源） |
| USB 设备 | Espressif **USB JTAG/serial debug unit**，VID `0x303A` PID `0x1001` |
| USB 序号 | `44:B1:76:CB:2A:10`（= MAC） |
| 驱动 | **免驱**（S3 原生 USB-Serial-JTAG，Mac 直接出 `/dev/cu.usbmodem*`） |
| esptool 连通性 | ✅ stub flasher 正常，可读写 Flash |

## 2. 芯片

```
Chip type          : ESP32-S3 (QFN56) (revision v0.2)
Features           : Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency  : 40MHz
USB mode           : USB-Serial/JTAG
MAC                : 44:b1:76:cb:2a:10
Flash              : 16MB, JEDEC 0x46/0x4018, eFuse: quad 4 lines, 3.3V
```

即 **ESP32-S3R8**（片内 8 MB Octal PSRAM）+ 16 MB Flash，对应板级手册里的 "ESP32-N16R8" 模块。

## 3. 分区表（读自 0x8000）

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 20 KB |
| otadata | data/ota | 0x00E000 | 8 KB |
| app0 | app/ota_0 | 0x010000 | 3072 KB |
| spiffs | data/spiffs | 0x310000 | 896 KB |
| coredump | data/coredump | 0x3F0000 | 64 KB |

app0 里实际固件约 **416 KB**（0x10000–0x77E70），其余 0xFF。

## 4. 板上现有固件

```
project_name : arduino-lib-builder
version      : afa5cdd
idf_ver      : v5.5-1-gb66b5448e0
built        : 2025-07-22 12:15:52
```

FQBN（编译参数，内嵌在固件里）：

```
esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,
CPUFreq=240,FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,
DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1
```

> ⚠️ `FlashSize=4M`，但芯片实际 16 MB —— 有 12 MB 完全没用上，是 Arduino 默认配置没改。

从字符串可看出这是**官方 SD 卡播放固件**：

```
Starting SPI initialization...      SPI initialization completed
Starting SD card initialization...  SD card initialization successful/failed
SD card root directory file list:   File: / Size:
PSRAM initialization failed!        Memory allocation failed!
/images.bin   open images.bin failed   invalid images.bin header
Frames:   IntervalMs:   Failed to read frame:   Display Done!!
/spiffs
```

含义：开机 → 初始化 SPI → 初始化 SD → 列出根目录 → 打开 `/images.bin` → 校验头 → 打印 Frames/IntervalMs → 逐帧读 → 刷屏 → `Display Done!!`。

## 5. 关键发现

1. **`images.bin` 头确实被解析**（有 `invalid images.bin header` 和 `Frames:/IntervalMs:` 打印）——与 `host-pipeline.md` §3.5 推导的 `EPD6BIN` 头一致；但固件里没有 `EPD6BIN` 字面量，magic 可能是数值比较，需实测确认。
2. **日志走 UART0，不走 USB**：`CDCOnBoot=default` 关闭了 USB CDC 控制台，所以插 Type-C 看不到 `Serial.print` 输出（ROM 日志例外，ROM 固定往 USB-JTAG 打）。要看得接 UART0（GPIO43/44）或重编固化配置。
3. **spiffs 区全空**（0xFF），没存过图。
4. Flash 配置只用了 4 MB 方案，做自己的固件要改 16 MB + 自定义分区。

## 6. 复现命令

```bash
# 需要在可访问 /dev/cu.* 的权限下运行
.venv/bin/python -m esptool -p /dev/cu.usbmodem1101 flash-id
.venv/bin/python -m esptool -p /dev/cu.usbmodem1101 -b 921600 read-flash 0 0x20000 /tmp/flash_head.bin
.venv/bin/python -m esptool -p /dev/cu.usbmodem1101 -b 921600 read-flash 0x10000 0x300000 /tmp/app.bin
```

（esptool 5.6.0 已装进根目录共享 `.venv`）
