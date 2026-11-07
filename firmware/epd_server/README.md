# epd_server —— 13.3" 电子纸局域网刷屏固件

把 **GDEP133C02（13.3" E Ink Spectra 6，1200×1600）** 做成一台局域网显示终端：
上位机广播找到它 → HTTP 传一张已经抖动打包好的图 → 固件写 SD 卡 → 刷到屏幕上。

本工程把仓库里已经分别验证过的四块能力合到一个固件里：

| 能力 | 来源工程 | 本工程文件 |
|---|---|---|
| 面板驱动（双 IC、4bpp、DTM/DRF） | `firmware/e6_epd` | `main/GDEP133C02.c`、`main/epd_panel.c` |
| SD 卡挂载（SPI3 + FATFS，绝不格式化） | `firmware/sd_bridge` | `main/sd_card.c` |
| WiFi STA + UDP 发现（端口 3232） | `firmware/wifi_check` | `main/wifi_sta.c`、`main/discovery.c` |
| EPD6BIN 出图格式 | `host/e6_convert.py` | `main/image_store.c`（校验） |

---

## 1. 三个任务

需求里的三段链路在固件里就是三个独立任务，互相只通过队列和一两个状态变量打交道：

```
上位机 ──UDP 3232 广播──▶ ┌──────────────────────────────┐
                          │ 任务 1  discovery.c          │
上位机 ◀─回包(IP/MAC/端口)─│ recvfrom → 校验魔数 → 单播回包 │
                          └──────────────────────────────┘

上位机 ──HTTP POST 960018B─▶ ┌─────────────────────────────────────┐
                             │ 任务 2  upload_server.c (httpd 任务) │
                             │ 边收边算 CRC → incoming.part         │
                             │ → 校验头 → rename 到 history/        │
                             │ → 原子更新 current → 投递刷屏队列     │
                             └───────────────┬─────────────────────┘
                                             │ xQueueSend(display_job)
                                             ▼
                             ┌─────────────────────────────────────┐
                             │ 任务 3  display_task.c               │
                             │ 取最新一张 → 读满 960018 B 到 PSRAM   │
                             │ → CRC/头校验 → init/reinit → DTM/DRF │
                             └─────────────────────────────────────┘
```

| 任务 | 触发 | 阻塞点 | 优先级 |
|---|---|---|---|
| 1 发现 | 收到 UDP 广播查询 | `recvfrom` | 4 |
| 2 接收 | 收到 HTTP 请求 | `httpd_req_recv` / SD 写 | 5（httpd 默认） |
| 3 刷屏 | 队列里有任务 | 面板 BUSY 轮询（**实测 ≈30 s**） | 6 |

**只刷最新一张**：队列深度 4，刷屏任务每次取完当前任务后会把队列里排队的旧任务都丢掉。
连续上传三张时，用户要的是最后一张，而不是每张等 30 s。

---

## 2. 硬件与资源

| 项 | 值 |
|---|---|
| 板子 | 佳显 ESP32-133C02-IL（ESP32-S3R8，16 MB Flash / 8 MB PSRAM） |
| 面板 | GDEP133C02，1200×1600，6 色，4bpp，整帧 **960 000 B**，实测刷新 **29~30 s** |
| 面板 SPI | SPI2，SCK=9 MOSI=41 MISO=40 CS_M=18 CS_S=17 RST=6 DC=2 BUSY=7 PWR=45，**10 MHz** |
| SD 卡 | SPI3，CS=15 MOSI=3 MISO=5 SCK=8，**10 MHz**，FAT32，**固件绝不格式化** |
| 单帧容器 | EPD6BIN：18 B 头 + 960 000 B，即 **960 018 B** |
| 固件大小 | 约 843 KB（分区 4 MB，剩余 80%） |

内存分布：

| 用途 | 大小 | 位置 |
|---|---|---|
| 任务 3 整帧缓冲 | 960 018 B | PSRAM（`MALLOC_CAP_SPIRAM`） |
| 任务 2 接收缓冲 | 8 KB | PSRAM，失败回退内部 RAM |
| 面板 DMA 中转 | 300 B | 内部 DMA 内存（驱动要求） |
| httpd 任务栈 | 12 KB | 内部 RAM |

---

## 3. 目录结构

```
firmware/epd_server/
├── CMakeLists.txt
├── sdkconfig.defaults        16 MB Flash / 8 MB PSRAM / USB-Serial-JTAG 控制台
├── partitions.csv            nvs / phy_init / factory 4 MB
└── main/
    ├── CMakeLists.txt        从仓库根 .env 注入 WiFi 凭据
    ├── wifi_secrets.h.in
    ├── epd_config.h          端口、路径、开关集中在这里
    ├── main.c                启动编排
    ├── wifi_sta.c/.h         STA 连接 + 指数退避重连
    ├── discovery.c/.h        任务 1
    ├── sd_card.c/.h          FATFS 挂载
    ├── GDEP133C02.c/.h       面板驱动（从 e6_epd 移植）
    ├── epd_panel.c/.h        首次 init / 之后 reinit 的封装
    ├── image_store.c/.h      SD 布局、EPD6BIN 校验、原子归档
    ├── display_task.c/.h     任务 3
    ├── upload_server.c/.h    任务 2 + HTTP API
    ├── epd_state.c/.h        全局状态快照 + 状态 JSON
    └── crc32.c/.h            zlib 同款 CRC32（与上位机对得上）
```

---

## 4. 构建与烧录

```bash
cp .env.example .env          # 填 WIFI_SSID / WIFI_PASSWORD（首次）
./tools/epd-server.sh build   # 只编译
./tools/epd-server.sh all     # 编译 + 烧录 + 监视（Ctrl+] 退出）
```

手动：

```bash
. tools/env.sh
cd firmware/epd_server
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

> 本固件会覆盖板上原有的 `e6_epd` / `sd_bridge` 固件。要刷回去，分别进对应目录
> `idf.py flash` 即可（分区表不同，`idf.py flash` 会连分区表一起写）。

---

## 5. 启动流程

```
app_main
 ├─ epd_state_init                    状态模块
 ├─ nvs_flash_init                    WiFi 驱动需要
 ├─ wifi_sta_start                    STA 连接（非阻塞，断线自动重连）
 ├─ sd_card_mount                     失败不致命，管理接口会报告
 │   └─ image_store_init              建 /sdcard/epd、清理残留临时文件
 ├─ display_task_start                任务 3 就位（整帧缓冲在这里分配）
 │   └─ boot_restore_last_image       有 current 就立刻排队重刷
 ├─ upload_server_start               任务 2（HTTP，绑定 0.0.0.0）
 └─ discovery_start                   任务 1（UDP）
```

**网络先起、刷屏后动**：面板没接好或 SD 没插，都不影响上位机发现设备；
状态接口会如实报告 `panel.ready` / `sd.mounted`，上传会返回 503 而不是假装成功。

启动日志示例：

```
I (312) main:  epd_server v1.0.0 —— GDEP133C02 局域网刷屏固件
I (322) main:  发现 UDP 3232 ｜ HTTP 80 ｜ 主机名 epd-133c02
I (702) sd_card: SD 卡已挂载：/sdcard
I (712) store: 图片仓库就绪：/sdcard/epd/history（已归档 3 张）
I (732) display: 整帧缓冲 960018 字节已就绪
I (842) http: HTTP 服务已启动：端口 80（7 个接口）
I (852) discovery: UDP 发现服务已启动：端口 3232，回包 HTTP 端口 80
I (2862) wifi: DHCP 成功：IP=192.168.1.50（http://192.168.1.50:80/）
I (2902) main: 设备已就绪：在浏览器打开 http://192.168.1.50/ 或用 Swift 上位机自动发现
I (3102) display: 开机恢复：准备重刷 20261107-153012.bin（960018 字节）
```

---

## 6. HTTP API

端口 `EPD_HTTP_PORT`（默认 80）。所有响应都是 JSON；错误响应统一为
`{"ok":false,"error":"<code>","message":"<中文说明>"}`。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 纯文本 / HTML 状态页，浏览器直接看 |
| GET | `/api/v1/status` | 完整 JSON 状态 |
| POST | `/api/v1/image` | **上传一帧**（body = 960 018 B 的 EPD6BIN） |
| GET | `/api/v1/image?file=xx.bin` | 下载归档文件 |
| GET | `/api/v1/history` | 归档列表（文件名倒序） |
| POST | `/api/v1/display?file=xx.bin` | 重新显示某张归档（202），并把 `current` 指向它 |
| POST | `/api/v1/clear?color=white` | 整屏单色（202），同时清掉 `current` |

### 6.1 上传

```bash
curl -X POST http://192.168.1.50/api/v1/image \
     -H "Content-Type: application/octet-stream" \
     -H "X-Image-Name: 20261107-153012-genshin01.bin" \
     -H "X-Timestamp: 1794123012" \
     -H "X-Crc32: e7ae39ec" \
     --data-binary @out/genshin01.bin
```

| 请求头 | 必填 | 说明 |
|---|---|---|
| `Content-Length` | 是 | 必须正好 960 018 |
| `X-Image-Name` | 否 | 归档文件名，**建议带本地时间**；非法名字会被替换成 `img-<uptime>.bin`，重名自动加 `-1`、`-2` |
| `X-Timestamp` | 否 | Unix 秒，写进 `current` 元数据，供状态接口显示；缺失则为 0 |
| `X-Crc32` | 否 | 整文件 CRC32（十六进制）；给了就校验，不一致直接 400 |

成功（200）：

```json
{
  "ok": true,
  "file": "20261107-153012-genshin01.bin",
  "bytes": 960018,
  "crc32": 3722463389,
  "saved_at": 1794123012,
  "received_ms": 2210,
  "queued": true
}
```

`queued=false` 表示已安全落盘，但刷屏队列当时满了（没有自动上屏，可再用
`/api/v1/display` 触发）。

失败：

| HTTP | error | 含义 |
|---|---|---|
| 400 | `bad_length` / `bad_format` | 字节数不对 / 不是单帧 EPD6BIN / 尺寸不是 1200×1600 |
| 400 | `crc_mismatch` | 传输过程中丢字节 |
| 409 | `upload_busy` | 已有一次上传在进行（串行化） |
| 408 | `recv_timeout` | 收包超时（20 s 无数据，重试 3 次后放弃） |
| 503 | `sd_unavailable` / `sd_write_failed` / `commit_failed` | 卡没挂载、写失败、归档失败 |

### 6.2 状态

```json
{
  "ok": true,
  "device": "GDEP133C02",
  "firmware": "1.0.0",
  "hostname": "epd-133c02",
  "ip": "192.168.1.50",
  "mac": "aa:bb:cc:dd:ee:ff",
  "uptime_s": 42,
  "http_port": 80,
  "discovery_port": 3232,
  "panel": { "width": 1200, "height": 1600, "colors": 6,
             "frame_bytes": 960000, "ready": true },
  "sd": { "mounted": true, "total_bytes": 7861305344, "free_bytes": 7000000000 },
  "display": { "busy": false, "refresh_count": 3,
               "last_refresh_ms": 16520, "last_refresh_uptime_s": 30,
               "last_error": "ESP_OK",
               "current_name": "20261107-153012-genshin01.bin",
               "current_crc": 3722463389,
               "current_saved_at": 1794123012,
               "current_bytes": 960018 },
  "upload": { "active": false, "received": 0, "total": 0,
              "ok_count": 3, "fail_count": 0,
              "last_bytes": 960018, "last_ms": 2210 }
}
```

上位机就是靠 `display.busy` + `display.current_name` 判断“刷完了没有”。

---

## 7. 发现协议（任务 1）

和 `firmware/wifi_check` / `host/discover.py` **完全一致**（未改字段），只有
`http_port` 从 0 变成了本固件的 HTTP 端口：

```
查询 12 B（上位机 → 255.255.255.255 或子网广播 :3232）
  0  8  "EPD6DISC"
  8  1  version = 1
  9  1  type    = 0x01
  10 2  nonce（u16 BE）

应答 25+N B（设备 → 来源地址单播）
  0  8  "EPD6DISC"
  8  1  version = 1
  9  1  type    = 0x02
  10 2  nonce 回显
  12 4  IP（u32 BE）
  16 6  STA MAC
  22 2  HTTP 端口（u16 BE）   ← 本固件填 80
  24 1  hostname 长度 N
  25 N  hostname（UTF-8）
```

```bash
./.venv/bin/python host/discover.py           # 已验证过的发现工具
```

---

## 8. SD 卡布局

```
/sdcard/epd/
├── current                 当前应显示的图（文本元数据，原子替换）
├── incoming.part           上传中的临时文件（启动时清理）
└── history/
    ├── 20261107-153012-genshin01.bin
    ├── 20261107-160044-DSCF1172.bin
    └── ...
```

`current` 内容：

```
name=20261107-153012-genshin01.bin
crc32=e7ae39ec
saved_at=1794123012
bytes=960018
```

**写入顺序**（任何一步失败都不会留下“半截图”）：

1. 收包 → `incoming.part`，边收边算 CRC，`fflush` + `fsync`；
2. 校验魔数 / 宽高 / 帧数 / 总长度（可选再对 `X-Crc32`）；
3. `rename(incoming.part → history/<唯一名>.bin)`（同目录原子）；
4. 写 `current.tmp` → `fsync` → `remove(current)` → `rename` 成 `current`（几毫秒空窗）；
5. 投递刷屏任务。

断电只会造成两种结果：要么 `current` 还是旧的（旧图继续用），要么新文件已经在
`history/` 里但 `current` 没更新（下次上传会继续用新名字，不会覆盖）。

`/api/v1/display` 会把 `current` 指向重新显示的那张；`/api/v1/clear` 会删掉
`current`——所以清屏之后重启不会又把旧图刷回来。

**归档策略**：每次上传都留一份，按文件名倒序（文件名带上位机本地时间戳），
不做轮转；一张 0.92 MB，7 GB 卡能放约 7600 张。

> 坑：FATFS 的 `f_rename` 在**目标文件已存在**时返回 `FR_EXIST`（POSIX 的 `rename`
> 是覆盖语义）。所以 `current` 的原子更新必须先 `remove` 旧文件再 `rename`；图片归档
> 用唯一文件名，不受影响。

---

## 9. 刷屏时序

任务 3 每刷一帧：

1. `fopen` 归档文件，一次性 `fread` 960 018 B 到 PSRAM；
2. 校验 EPD6BIN 头 + CRC32（与上传时记录的值比对）；
3. 第一次 → `gdep133c02_init()`（建 SPI2 + 完整上电时序）；
   之后每次 → `gdep133c02_reinit()`（复位 + 整套初始化命令，厂家多帧示例同款）；
4. Master 取每行前 300 B、Slave 取后 300 B，各发 1600 行 `DTM`；
5. `PON → DRF → POF`，全程轮询 BUSY，单帧实测 29~30 s（规格书写 ~12 s，本板 13.3" 实际更慢）；
6. 更新 `epd_state`（`busy=false`、`current_name`、耗时）。

刻意**不在两帧之间调用 `gdep133c02_sleep()`**：厂家 13.3" 多帧示例就是
`initEPD() → 刷 → POF → 下一帧再 initEPD()`；`sleep()` 会把 `PWR_PIN` 拉低，
而 `panel_setup()` 不会重新拉高，频繁刷屏时反而会卡在 BUSY。

---

## 10. 设计取舍

| 选择 | 原因 |
|---|---|
| **上位机做缩放/抖动，固件只收成品** | 设备端 JPG→6 色实测 18.2 s（见 `notes/jpg2bin-bench.md`），而且官网工具链本来就是“抖一次、谁都不再动” |
| **整帧读进 PSRAM 再刷** | 驱动按行跨 600 B 取 300 B，整帧连续最省事；960 KB 对 8 MB PSRAM 很小 |
| **HTTP 而不是裸 TCP** | 发现回包里本来就预留了 http 端口字段；`curl`、浏览器、URLSession 都能直接用，错误码也够表达“卡没插 / CRC 错 / 忙” |
| **只接受单帧** | 需求是“一次一张”；多帧轮播在这里没有产品价值，反而让队列语义变复杂 |
| **只刷最新** | 用户连传多张时，中间那些没有观看价值；每张 30 s 等不起 |
| **上传必须落盘成功才算成功** | “先显示再存”会在断电后丢图；图片先落到 `history/` 唯一文件名，`current` 只是指针 |
| **开机自动重刷最后一张** | `EPD_BOOT_RESTORE=1`；电子纸断电保画面，但重启后重刷一次能保证“上电一定有内容”。想省刷新次数改成 0 |

---

## 11. 验证状态

### 固件（2026-11-07，真机 ESP32-133C02-IL）

板子：ESP32-S3R8，WiFi `your-ssid`，DHCP 得到 `192.168.1.50`，SD 卡 7.5 GB。

| 项 | 结果 |
|---|---|
| 启动 | PSRAM 8 MB、SD 挂载、WiFi 3 s 内拿到 IP、HTTP / 发现同时就绪 |
| 发现 | `host/discover.py` 2 s 内找到 `192.168.1.50 / aa:bb:cc:dd:ee:ff / epd-133c02 / 80` |
| 上传 | 960 018 B 单张，设备端 **5.7~7.7 s**（约 1.0~1.3 Mbps） |
| 落盘 | 归档文件用 `GET /api/v1/image` 取回，与原文件**逐字节一致**（CRC `081107dd`） |
| 刷屏 | **29.4~30.0 s**，`refresh_count` 递增，`current_name` / `current_crc` 正确 |
| 开机恢复 | 断电重启后自动重刷最后一张（约 30 s） |
| 连续上传 | 两次紧接上传都成功（修复了 FATFS `rename` 的 `EEXIST` 问题） |
| 队列 | 刷屏期间到达的图按顺序刷完；有多张排队时只刷最新 |
| 重刷归档 | `POST /api/v1/display?file=` 生效，并把 `current` 指向该文件 |
| 清屏 | 未在真机验证（会让屏幕变白 30 s，需要时自行点一下） |

### 上位机

- 抖动 / 打包 / CRC 与 `host/e6_convert.py` 逐字节一致（`tools/verify_swift_dither.py`）。
- HTTP 契约用 `tools/mock_epd_server.py` 全通过（status / upload / history / display / clear）。
- `--oneclick-selftest` 直打真机：0.13 s 转换 + 6.5 s 上传，屏幕上屏成功。
- GUI 的点击/拖拽手势无法在本环境自动操作，已用离屏渲染（`--render-crop`）和
  headless 自检覆盖布局与算法路径。

## 12. 上板测试清单

```bash
# 0) 先备份整片（首次碰板必做，见 notes/backup.md）
./tools/backup-factory.sh

# 1) 烧固件并看日志
./tools/epd-server.sh all

# 2) 找设备
./.venv/bin/python host/discover.py
./.venv/bin/python host/epd_client.py status

# 3) 传一张图（Python 版，转换 + 上传一条龙）
./.venv/bin/python host/epd_client.py send images/genshin01.jpg --fit cover

# 4) 看屏幕是否刷新、状态是否正确
curl -s http://<设备IP>/api/v1/status | python3 -m json.tool

# 5) Swift 上位机
cd host/EDPHost && ./build_app.sh release run
```

日志里应该依次看到：`SD 卡已挂载` → `图片仓库就绪` → `HTTP 服务已启动` →
`UDP 发现服务已启动` → `DHCP 成功` → 上传时 `开始接收图片` / `已归档` /
`开始刷屏` / `刷新完成`。

### 常见问题

| 现象 | 排查 |
|---|---|
| 上位机找不到设备 | 同一网段？AP 开了客户端隔离？macOS 本地网络权限给了没？UDP 3232 被防火墙拦？ |
| 找到设备但 status 连不上 | `http_port` 是不是 80；用 `curl http://<ip>/` 直接试 |
| 上传 503 `sd_unavailable` | 卡没插好 / 不是 FAT32 / 卡座接触不良；看启动日志的 `SD 卡挂载失败：...` |
| 上传 400 `bad_format` | 传的不是 EPD6BIN 单帧（用 `host/e6_convert.py` 或 Swift 上位机重新生成） |
| 上传 400 `crc_mismatch` | 传输丢字节；重试；检查 WiFi 信号 |
| 上传 503 `commit_failed`，日志 `current 改名失败：errno=17` | FATFS 的 `rename` 不覆盖已存在目标；当前固件已改为“先 `remove` 再 `rename`”，旧固件才会踩到 |
| 屏幕一直不刷 | status 里 `panel.ready`；日志有 `BUSY 超时（90000 ms）` 说明面板排线/供电有问题 |
| 刷完画面方向不对 | 上位机里调「旋转 90°/180°/270°」；面板原生 1200 宽 × 1600 高 |
| 只想省刷新次数 | 把 `main/epd_config.h` 的 `EPD_BOOT_RESTORE` 改成 0 |
