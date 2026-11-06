# 出厂固件全片备份（2026-11-06）

## 文件

| 项 | 值 |
|---|---|
| 路径 | `backups/factory-16mb-20261106-180436.bin` |
| 大小 | 16,777,216 B（16 MiB，整片 0x0–0x1000000） |
| SHA-256 | `8f65452bac8dbcbac2f304573ced4a90441fa20e6c04be952169995269da57a0` |
| MD5 | `7e0f0d050d3b2bd9fbaa622a08fb024a` |

`.gitignore` 已排除 `backups/`，文件不入库。**建议再拷一份到仓库外**（移动硬盘/网盘），本机磁盘坏了它也会一起没。

## 读的是什么

| 项 | 值 |
|---|---|
| 串口 | `/dev/cu.usbmodem1101`（USB-Serial/JTAG，免驱） |
| 芯片 | ESP32-S3 (QFN56) rev v0.2，MAC `44:b1:76:cb:2a:10` |
| Flash | 16 MB（JEDEC `0x46/0x4018`），eFuse quad 4 线 / 3.3 V |
| 读速 | 55 s，约 2.4 Mbit/s |
| 校验 | 连续两次整片读取 + `cmp` 逐字节一致（两次 SHA-256 相同） |

板上即厂家 Arduino 固件（`arduino-lib-builder` / `afa5cdd` / IDF v5.5，2025-07-22），细节见 [board-check.md](board-check.md)。

## 镜像内容（非 0xFF 区域）

| 区域 | 内容 |
|---|---|
| `0x000000`–`0x005000` | 2nd bootloader |
| `0x008000`–`0x009000` | 分区表（MD5 项全 0xFF，即未写校验） |
| `0x00E000`–`0x078000` | otadata + app0（app 约 424 KB） |
| 其余 | 全 0xFF，spiffs 未使用 |

分区表与 [board-check.md](board-check.md) 记录一致：

```
nvs        off=0x009000 size=0x005000 (20 KB)
otadata    off=0x00E000 size=0x002000 (8 KB)
app0       off=0x010000 size=0x300000 (3072 KB)
spiffs     off=0x310000 size=0x0E0000 (896 KB)
coredump   off=0x3F0000 size=0x010000 (64 KB)
```

## eFuse 安全状态（只读检查）

- `SECURE_BOOT_EN = 0`
- `SPI_BOOT_CRYPT_CNT = 0`（Flash 未加密）
- `DIS_DOWNLOAD_MODE = 0`、JTAG 未软禁

即没有 secure boot / flash encryption 挡路，备份是明文，可直接整片写回，也可后续自由烧录自编固件。

## 恢复

```bash
cd /Users/chenlong/github/s6-esp32s3-gdep133c02
./.venv/bin/python -m esptool --port /dev/cu.usbmodem1101 --baud 921600 \
    write-flash 0 backups/factory-16mb-20261106-180436.bin
```

写整片约 1–2 分钟（含 esptool 默认回读校验）。**注意**：这一步会覆盖板上一切内容，执行前确认板子是要恢复的那块。

## 复现备份

```bash
# 需在可读写串口设备的权限下运行（工作区文件沙箱只给只读打开，会报 EPERM）
./.venv/bin/python -m esptool --port /dev/cu.usbmodem1101 --baud 921600 \
    read-flash 0 0x1000000 backups/factory-16mb-$(date +%Y%m%d-%H%M%S).bin
```

或直接用 [tools/backup-factory.sh](../tools/backup-factory.sh)。
