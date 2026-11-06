# sd_bridge —— SD 卡读写 / USB 传输固件

把 ESP32-133C02-IL 板载 microSD 卡挂到 FATFS，通过 **USB-Serial/JTAG** 暴露一套二进制文件协议；配套上位机 `host/sd_bridge.py` 可以浏览、校验、上传、下载、递归拉取整张卡。

> 目标：不依赖读卡器，插着板子就能把 SD 卡内容原样取到主机；同时支持安全写入（临时文件 + CRC 校验 + 原子改名）。

## 1. 硬件与资源

| 项 | 值 |
|---|---|
| 开发板 | 佳显 ESP32-133C02-IL（ESP32-S3R8，16 MB Flash / 8 MB PSRAM） |
| SD 接口 | SPI3_HOST，**CS=IO15，MOSI=IO3，MISO=IO5，SCK=IO8** |
| 卡检测 / 写保护 | 无（板上未接） |
| 当前 SPI 时钟 | **10 MHz**（`SD_MAX_FREQ_KHZ`；早期读测试用过 20 MHz） |
| 文件系统 | FAT32，扇区 512 B，簇 32 KiB（本卡） |
| 控制台 | UART0 115200；USB-Serial-JTAG 只跑协议，不打印日志 |
| 分区 | `nvs 0x9000/0x6000`、`phy_init 0xF000/0x1000`、`factory 0x10000/0x400000` |

SPI 引脚与 EPD 不冲突：EPD 用 SPI2，SD 用 SPI3。固件**不会格式化卡**，`format_if_mount_failed` 始终为 `false`。

## 2. 构建与烧录

```bash
cd /Users/chenlong/github/s6-esp32s3-gdep133c02
. tools/env.sh

cd firmware/sd_bridge
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

串口被上位机占用时先关闭 `idf.py monitor`。烧录后固件自动挂载 SD；即使卡没插或挂载失败也会保持运行，`INFO` 会显示 `mounted=0` 和错误原因，可用 `MOUNT` 重试。

## 3. 上位机

依赖仓库内 `.venv` 的 pyserial：

```bash
./.venv/bin/python host/sd_bridge.py --help
```

端口默认自动探测 `/dev/cu.usbmodem*`、`/dev/tty.usbmodem*`、`/dev/ttyACM*`、`/dev/ttyUSB*`；多板时用 `--port` 指定。

### 3.1 查看

```bash
./.venv/bin/python host/sd_bridge.py info          # 卡信息、容量、挂载点、协议版本
./.venv/bin/python host/sd_bridge.py df            # 总空间 / 已用 / 剩余
./.venv/bin/python host/sd_bridge.py ls -l /       # 根目录
./.venv/bin/python host/sd_bridge.py stat /images.bin
./.venv/bin/python host/sd_bridge.py crc /images.bin   # 设备端算 CRC32，避免整文件回传
```

### 3.2 下载

```bash
# 单文件
./.venv/bin/python host/sd_bridge.py get /images.bin backups/images.bin --overwrite

# 递归拉取整张卡（推荐；--skip-errors 遇到不可读项继续）
DEST="backups/sdcard-$(date +%Y%m%d-%H%M%S)"
./.venv/bin/python host/sd_bridge.py pull / "$DEST" --skip-errors --manifest "$DEST-manifest.json"
```

`pull` 会：
- 递归建本地目录；
- 先写 `.part`，下载完比对设备端 CRC32，再改名；
- 恢复文件 mtime；
- 输出 JSON manifest（`files` + `errors`）。

### 3.3 上传 / 修改

```bash
./.venv/bin/python host/sd_bridge.py put local.bin /REMOTE.BIN
./.venv/bin/python host/sd_bridge.py put local.bin /REMOTE.BIN --overwrite
./.venv/bin/python host/sd_bridge.py pull /DIR local-dir          # 先确认
./.venv/bin/python host/sd_bridge.py push local-dir /DIR          # 递归上传
./.venv/bin/python host/sd_bridge.py mkdir /A /A/B
./.venv/bin/python host/sd_bridge.py mv /OLD.BIN /NEW.BIN
./.venv/bin/python host/sd_bridge.py rm /NEW.BIN
./.venv/bin/python host/sd_bridge.py rmdir /A/B
```

写入流程：先写同目录临时文件 `/.sdbtmp-xxxxxxxx`，边收边算 CRC32；收完 `fflush` + `fclose`，校验一致后才 `rename` 到目标名。中途失败或 `ABORT` 会删临时文件，不会留下半截目标文件。目标已存在时必须显式 `--overwrite`。

`push` 默认跳过已存在且大小/内容相同的文件；`--exclude` 可重复，支持 glob。

### 3.4 挂载维护

```bash
./.venv/bin/python host/sd_bridge.py unmount   # 释放 SPI / FATFS
./.venv/bin/python host/sd_bridge.py mount     # 重新挂载（换卡后）
./.venv/bin/python host/sd_bridge.py sync      # 协议级 no-op；每次 PUT 已完成落盘
```

## 4. 协议摘要

帧格式（小端）：

```
offset 0   "SDBG"          4 B
offset 4   type            1 B   1=REQ, 2=RSP, 3=DATA
offset 5   flags           1 B
offset 6   seq             2 B
offset 8   len             4 B   payload 长度，最大 32768
offset 12  crc32           4 B   覆盖 header[4:12] + payload
offset 16  payload         len B
```

- 文件数据按 16 KiB 分片，每片一个 `DATA` 帧；`GET` 以 `DONE <size> <crc32>` 结束。
- `PUT` 每个 `DATA` 帧回 `ACK <received>`；最后回 `DONE <size> <crc32>`。
- 文本响应以 `OK` 或 `ERR <code> <msg>` 开头。
- 路径是相对 `/sdcard` 的 UTF-8 路径；拒绝 `..`、反斜杠、冒号、控制字符。
- 支持命令：`PING`、`INFO`、`LS`、`STAT`、`GET`、`PUT`、`ABORT`、`CRC`、`MKDIR`、`RM`、`RMDIR`、`MV`、`SYNC`、`MOUNT`、`UNMOUNT`。

## 5. 安全设计与限制

- **绝不格式化**：挂载失败只报错，不调用 `f_mkfs`。
- 写入使用临时文件 + CRC + 改名；目标已存在需显式覆盖。
- 不暴露裸块设备，只能访问 FAT 文件；`System Volume Information` 等系统目录也会按普通目录列出（本卡已验证可读）。
- FAT 时间戳精度 2 s，且没有 RTC 时新文件可能显示 1980 年；这是时间问题，不是数据损坏。
- 卡片必须是 FAT32（本卡 7.3 GiB，512 B 扇区，32 KiB 簇）。exFAT 不支持。
- 传输是单会话串行协议；不要同时开两个上位机进程抢同一个串口。

## 6. 已验证

在 2026-11-06 的板子上：

- 卡：`SDHC/SDXC`，CID 名 `ASTC????`，容量 7,864,320,000 B，FAT32 总 7,861,305,344 B。
- `images.bin`：3,840,018 B，CRC32 `caa408b6`；多次整卡 `pull` 与旧备份逐字节一致。
- 递归 `pull /` 成功拉取 `images.bin` 和 `System Volume Information/IndexerVolumeGuid`，manifest 无错误。
- 4 KiB 文件 `PUT → CRC → GET → cmp` 通过；长文件名、`MV`、`MKDIR`、`RM`、`RMDIR`、覆盖写均通过。
- 写测试产生的临时目录/文件已清理；卡上只保留 `images.bin` 和 `System Volume Information`。

## 7. 恢复显示固件

`firmware/e6_epd` 未被修改。要回到“开机直刷内嵌图”的显示固件：

```bash
cd firmware/e6_epd
idf.py -p /dev/cu.usbmodem1101 flash
```

分区表不同（e6_epd 使用自己的 `partitions.csv`），`idf.py flash` 会同时写 bootloader、分区表和应用；整片出厂备份见 `notes/backup.md`。
