# 局域网刷屏固件（epd_server + EDPHost）

## 目标

把仓库里分别验证过的四块能力（面板驱动、SD/FATFS、WiFi+UDP 发现、EPD6BIN 出图）
合成一台**完整可用**的展示终端，并配一个 macOS Swift 上位机：

1. 上位机广播找设备，设备回 IP；
2. 上位机把照片转成 1200×1600 六色 4bpp 并上传；
3. 固件落 SD 卡归档，再刷到屏幕。

关键前提：**抖动和缩放都在上位机做**。设备端不解析 JPG —— `notes/jpg2bin-bench.md`
实测 6.6 MP 原图设备端要 18.2 s，而先把图缩到 1200×1600 再传只要 4.7 s；
官网三条工具链也全是“抖一次，谁都不再动”。

## 结构

```
firmware/epd_server/   三个任务：discovery / upload_server / display_task
host/EDPHost/          SwiftUI 上位机（SwiftPM 包，可打 .app）
host/epd_client.py     命令行参考客户端（也是 HTTP 接口的可执行文档）
tools/mock_epd_server.py  假设备，无硬件联调
```

任务边界很清楚：任务 2 只负责“收整包 → 落盘 → 投递”，任务 3 只负责“读盘 → 刷屏”。
两者之间只有一个 `display_job_t` 队列和 `epd_state` 里的状态，没有共享缓冲。

## 协议

- 发现：完全沿用 `wifi_check` 的 12 B 查询 / 25+N B 应答，只是把应答里的
  `http_port` 从 0 填成 80。`host/discover.py` 不用改就能用。
- 上传：HTTP `POST /api/v1/image`，body 就是 `host/e6_convert.py` 产出的 `.bin` 原样
  （960 018 B）。`X-Image-Name` 带上位机本地时间戳 → 设备端直接拿来做归档文件名。
- 状态：`GET /api/v1/status` 返回 `display.busy` / `current_name` / `refresh_count`，
  上位机靠它判断“刷完没有”。

为什么用 HTTP 而不是自定义 TCP：发现回包里本来就预留了 http 端口字段；
`curl`、浏览器、`URLSession` 全都能直接用，错误码也够表达“卡没插 / CRC 错 / 忙”。

## SD 卡写入顺序

`incoming.part`（边收边算 CRC）→ 校验 → `rename` 到 `history/<唯一名>.bin`
→ 写 `current.tmp` → `fsync` → `rename` 成 `current`。

FATFS 的同目录 `rename` 是原子的，所以外部永远看不到半截图；断电最坏情况是
`history/` 多一个孤儿文件，而 `current` 还是上一张完整的图。

## 验证记录

本轮做过的（**没有连实物板**）：

| 项 | 结果 |
|---|---|
| `idf.py build` | 通过，无警告；`epd_server.bin` 0xCDE00 ≈ 843 KB，4 MB 分区剩 80% |
| 固件 CRC32（`main/crc32.c`）与本机 zlib | 随机 250 KB：`aa53d65d` == `aa53d65d` |
| Swift 抖动/打包 vs `host/e6_convert.py` | fs-ser / fs / atkinson / none + strength 0.5 全部**逐字节一致** |
| Swift CRC32 vs zlib | 1 000 003 B：`467d498d` == `467d498d` |
| Swift HTTP 客户端 | 对 `tools/mock_epd_server.py` 跑 status→upload→history→display→clear 全通过 |
| Python 客户端 | 对同一 mock：status / upload / history 通过 |
| `.app` | `./build_app.sh` 组装 + ad-hoc 签名；`open` 后进程以 Foreground 身份存活 |

### 真机（2026-11-07）

板子接上后跑了一遍完整链路，结果：

| 项 | 实测 |
|---|---|
| 启动 | WiFi 3 s 内拿到 IP `192.168.1.50`，SD 7.5 GB 挂载正常 |
| 发现 | 2 s 内回包，主机名/MAC/端口都对 |
| 上传 | 960 018 B 用 5.7~7.7 s（约 1.0~1.3 Mbps），SD 写入与收包重叠 |
| 刷屏 | **29.4~30.0 s**（不是规格书的 12 s） |
| 开机恢复 | 重启自动重刷最后一张 |
| 归档回读 | 951 KB 文件下载回来与原文件逐字节一致 |
| 重刷 / 队列 | `/display` 正常；刷屏中到达的图会排队，多张只刷最新 |
| 一键流程 | `--oneclick-selftest` 直打真机：0.13 s 转换 + 6.5 s 上传 + 上屏 |

第一次真机测试抓到一个真 bug：**第二次上传必失败**，日志
`store: current 改名失败：errno=17`。FATFS 的 `f_rename` 不覆盖已存在的目标文件
（POSIX `rename` 是覆盖语义），而 `current` 从第二次起就存在。修复是先 `remove`
再 `rename`，代价是几毫秒的空窗（断电最坏情况是“没有 current”，重启不自动刷图，
图片本身在 `history/` 里完好）。

## 踩过的坑

1. **驱动里的 `ESP_ERROR_CHECK` 会让服务器整体挂掉**
   `GDEP133C02.c` 原来在 `gpio_config` / `spi_bus_initialize` 上用 `ESP_ERROR_CHECK`，
   面板没接好就直接 abort。服务器固件不能这样，改成返回错误码；`init` 也做成可重入：
   总线已建时只重跑 `panel_setup()`，DMA 缓冲单独补分配（否则上次 malloc 失败后
   第二次进 `panel_setup()` 会空指针）。

2. **两帧之间不要调 `gdep133c02_sleep()`**
   它会把 `EPD_PWR_PIN` 拉低，而 `panel_setup()` 不会重新拉高。厂家 13.3" 多帧示例
   是 `initEPD() → 刷 → POF → 下一帧再 initEPD()`，本固件照这个来，只在 `turn_on_display`
   里发 POF。

3. **SwiftPM 在受限环境里嵌套 sandbox 失败**
   `swift build` 会调 `sandbox-exec` 编译 manifest，外层沙箱一挡就报
   `sandbox_apply: Operation not permitted`。构建脚本统一带 `--disable-sandbox`。

4. **CGContext 的旋转方向**
   `ctx.rotate(by: +90°)` 在最终位图里是**逆时针**（因为 bitmap 内存行序与 CG 用户空间
   y 轴相反）。UI 上标“顺时针 90°”必须传负角。验证时还被调色板差异误导过一次：
   `e6_convert.py --decode` 用 DRIVER_PALETTE（纯 RGB），`--preview` 用 REAL_PALETTE
   （面板实测色），同一份数据渲染出来颜色差很多，一开始误判成旋转错。

5. **注释里出现 `/*` 会编译失败**
   `image_store.h` 里写 `/sdcard/epd/history/*.bin` 触发了 `-Werror=comment`。

6. **`-Wformat-truncation` 对 255 B 文件名报警**
   `d_name` 最长 255，路径缓冲只有 128，`snprintf` 被判定可能截断。把缓冲加大到
   `EPD_PATH_MAX + 256`。

7. **Swift 静态属性初始化里的 `.map` 解析失败**
   `static let table: [UInt32] = (0..<256).map { ... }()` 在 CRC32 枚举里报了
   `cannot call value of non-function type '[UInt32]'`，改成显式 `for` 循环就好了。

8. **macOS 本地网络权限**
   macOS 15+ 上 UDP 广播会被系统拦，必须在 `Info.plist` 写
   `NSLocalNetworkUsageDescription`，否则“扫描永远 0 台设备”。

9. **FATFS 的 rename 不覆盖目标**
   见上面的真机记录。这类问题只有第二次操作才会暴露，单次冒烟测试发现不了。

10. **面板实际刷新 30 s，不是规格书的 12 s**
   13.3" Spectra 6 整屏刷新比小尺寸慢得多，文档和 UI 文案都按实测改成了约 30 s。

## 可继续做的

- mDNS/Bonjour 广播，省掉 UDP 发现；
- 上传分块 + 断点续传（当前是整包 POST）；
- 设备端保存多帧轮播（驱动和 bin 格式都支持，`frames>1` 被主动拒了）；
- 上位机批量队列（一次选多张，按顺序上传）；
- 面板不接时自动跳过开机恢复，避免 90 s BUSY 超时占用刷屏任务。
