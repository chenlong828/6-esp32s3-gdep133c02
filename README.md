# s6-esp32s3-gdep133c02

ESP32-S3 驱动佳显（Good Display）**13.3 英寸 E Ink Spectra 6（S6）** 彩色电子纸 —— GDEP133C02。
仓库内容：固件工程、上位机出图工具、面板资料、案例笔记。

**Topics**: `eink` `epaper` `spectra6` `esp32s3` `gdep133c02` `e6` `spi` `dithering`

| 项 | 值 |
|---|---|
| 面板 | GDEP133C02，13.3"，1200×1600，6 色（黑/白/红/黄/蓝/绿），4bpp，150 PPI，刷新 ~12 s |
| 驱动 IC | 双 IC（Master/Slave），每颗管 600 列，每行共 600 字节 |
| 开发板 | 佳显 ESP32-133C02-IL：ESP32-S3R8（16 MB Flash / 8 MB PSRAM），Type-C 原生 USB ｜ 照片见 [`docs/ESP32-133C02-IL_board.jpg`](docs/ESP32-133C02-IL_board.jpg) |
| 串口 | `/dev/cu.usbmodem1101`（USB-Serial-JTAG，免驱） |
| 单帧数据量 | 960 000 字节（= 1200×1600/2，每字节 2 像素，高 nibble 在左） |

## 目录

```
docs/           面板资料（规格书/设计须知/板级手册/原理图/烧录指南）+ 厂家示例 zip + 文本提取
docs/preview/     机械图、原理图、image.h 解码图
docs/reference/   官网在线转换器原页面与逆向 JS
host/           上位机出图工具 e6_convert.py + samples/（示例 bin 与预览图）
host/sd_bridge.py SD 卡上位机：浏览/校验/上传/下载/递归拉取
host/discover.py  UDP 广播发现：自动找设备 IP / MAC / 主机名
host/EDPHost/     Swift 上位机（SwiftUI：发现 / 裁切 / 转换 / 上传，可打包 .app）
host/epd_client.py 参考客户端（命令行：discover/status/send/upload/history/...）
firmware/e6_epd/  固件工程（13.3" GDEP133C02，内嵌图开机直刷）
firmware/sd_bridge/ SD 卡读写/传输固件（FATFS + USB 二进制协议）
firmware/wifi_check/ WiFi 连通性测试（STA 连接 + DHCP 信息 + ping 网关）
firmware/jpg2bin_bench/ 设备端 JPG→EPD6BIN 性能测试（SD + TJpgDec + 6 色抖动）
firmware/epd_server/ 局域网刷屏固件（UDP 发现 + HTTP 传图 + SD 归档 + 面板刷新）
.env.example     WiFi 凭据模板（复制为 .env 填写；.env 不入库）
vendor/         厂家原始示例源码（13.3" 参考设计 / 7.09" 双环境版）
notes/          调研与分析笔记（产品参数、代码分析、出图链路、macOS 环境、板子体检）
tools/          构建/烧录/备份脚本 + mock_epd_server.py（无硬件联调）
tools/verify_swift_dither.py  Swift 与 Python 出图逐字节对比
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

## 局域网刷屏（完整版固件 + Swift 上位机）

`firmware/epd_server` 把三件事合成一台可用的显示终端：

1. **任务 1**：UDP 3232 广播发现 —— 回自己的 IP / MAC / HTTP 端口；
2. **任务 2**：HTTP `POST /api/v1/image` 收 960 018 B 的 EPD6BIN，边收边算 CRC，写入 SD 卡按时间戳归档；
3. **任务 3**：从 SD 读整帧刷到面板（实测 29~30 s），连续上传时只刷最新一张。

```bash
./tools/epd-server.sh all                        # 编译 + 烧录 + 监视
./.venv/bin/python host/discover.py              # 找设备
./.venv/bin/python host/epd_client.py send 你的照片.jpg            # 转换 + 上传（自备图片）
cd host/EDPHost && ./build_app.sh release run    # Swift 图形上位机
```

Swift 上位机启动后自动广播找设备、每 3 s 刷新设备状态；本地选图后在可拖拽的 3:4 / 4:3
裁切框里调构图（支持横竖图、旋转、等比缩放），点「一键生成并上传」即完成
转换 → 编译 → HTTP 上传，带六色占比预览，还能回看设备端归档。

- 固件细节、HTTP API、SD 布局、上板测试清单：[`firmware/epd_server/README.md`](firmware/epd_server/README.md)
- 上位机用法、命令行模式、逐字节验证：[`host/EDPHost/README.md`](host/EDPHost/README.md)
- 没板子也能联调：[`tools/mock_epd_server.py`](tools/mock_epd_server.py) 起一个假设备，
  `./.venv/bin/python tools/mock_epd_server.py --port 8080`

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

## WiFi 连通性测试

`firmware/wifi_check` 是独立小工程：板子以 STA 连上指定 AP，打印 DHCP 下发的 IP / 掩码 / 网关 / DNS、STA MAC、BSSID、信道、RSSI，并对网关发 4 个 ICMP 包；断线自动退避重连，在线每 30 s 打一次 RSSI 心跳。它同时跑一个 UDP 发现服务（端口 3232），配合 `host/discover.py` 可在网段内自动找到设备。

**凭据不进源码**：从仓库根 `.env` 读取，构建时由 CMake 生成 `build/esp-idf/main/wifi_secrets.h` 注入（`.env` 已在 `.gitignore` 中，改完直接重新编译会自动 reconfigure）。

```bash
cp .env.example .env          # 填 WIFI_SSID / WIFI_PASSWORD
./tools/wifi-check.sh all     # build + flash + monitor（Ctrl+] 退出）

./.venv/bin/python host/discover.py   # 广播找设备，打印 IP / MAC / 主机名
```

实测输出（示例 AP）：

```
I (1302) wifi:connected with your-ssid, aid = 2, channel 6, BW20, bssid = 00:11:22:33:44:55
I (2862) wifi-check: DHCP 成功：IP=192.168.1.50 掩码=255.255.255.0 网关=192.168.1.1
I (2972) wifi-check: STA MAC  : aa:bb:cc:dd:ee:ff
I (2992) wifi-check: DNS(主): 192.168.1.1
I (5002) wifi-check: ping 结果：4/4 回包，min/avg/max = 8/10/14 ms —— 网关可达
```

注意：WPA2 口令必须是 8–63 位（4 字符的口令 ESP32 无法完成握手）；密码会随代码编译进固件镜像，**不要外发 `build/` 或固件 bin**。细节见 [`firmware/wifi_check/README.md`](firmware/wifi_check/README.md)。

## 关键结论（详见 notes/）

- **固件只认 960 000 字节裸数据**，旋转/缩放/抖动全部在上位机完成。
- 抖动要对着**面板实测色**算误差（红 `160,32,32`、黄 `240,224,80`、蓝 `80,128,184`、绿 `96,128,80`），不是纯 RGB。
- `images.bin` 格式：`"EPD6BIN\0" + u16 w + u16 h + u16 frames + u32 interval_ms + 帧数据`。
- 出厂固件是厂家 SD 播放版（Arduino 构建，IDF 5.5，2025-07-22），读 `/images.bin`；已整片备份并覆盖为自编 13.3" 固件（见 `notes/backup.md`）。
- 当前 `firmware/e6_epd` 把 `out/genshin01.bin` 内嵌进固件，开机直接刷屏、不读 SD；换图导出同格式 bin 后重新编译即可。
- 设备端直接转 JPG：6.6 MP 原图约 **18.2 s**（解码 15.7 s + 抖动 2.5 s）；先把图缩到 1200×1600 再传只要 **4.7 s**。SD 20 MHz 可用，40 MHz 挂载失败。详见 [`notes/jpg2bin-bench.md`](notes/jpg2bin-bench.md)。
- 6 色无灰阶，中间色靠空间抖动；可辨色约几百种，纯色块观感接近印刷，照片会显颗粒。
- 局域网刷屏固件只收成品 EPD6BIN（18 B 头 + 960 000 B 帧），先落 SD 再刷屏；
  `GET /api/v1/status` 里的 `display.busy` / `display.current_name` 可直接判断刷完没有。

## 大文件策略

`docs/*.pdf`、`docs/*.zip`（约 100 MB）不入库，本地保留；`.gitignore` 已排除。

## 环境

- ESP-IDF **v5.4.4**：`~/esp/esp-idf-v5.4.4`
- Python 3.12（uv 管理）：仓库内 `.venv`（numpy / pillow / pyserial / esptool）
- WiFi 凭据：仓库根 `.env`（模板 `.env.example`），构建时注入，不入库
- 13.3" 厂家参考设计需 **IDF 4.4.4**（旧式 `component.mk`），需要原样编译时另装
