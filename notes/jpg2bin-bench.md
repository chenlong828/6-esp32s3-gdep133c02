# 设备端 JPG → EPD6BIN 性能测试

> 目标：把 `images/genshin01.jpg` 原图放到 SD 卡，让 ESP32-S3 自己完成
> 解码 → 缩放到 1200×1600 → 6 色蛇形误差扩散 → 4bpp 打包，测端到端耗时。
> 固件：`firmware/jpg2bin_bench/`；采集脚本：`host/capture_bench.py`。

测试日期：2026-11-06。板子：佳显 ESP32-133C02-IL（ESP32-S3R8，240 MHz，8 MB Octal PSRAM @80 MHz，16 MB Flash）。
ESP-IDF v5.4.4，`-O2`，JPEG 解码用 **ROM 里的 TJpgDec**（RGB888、支持 1/2 降采样；ROM 版没有源码档的 Huffman 表优化）。

---

## 1. 结论（先看这张表）

| 输入 | 转换（SD → 960 KB packed） | 再加“写成 .bin 到 SD” | 备注 |
|---|---:|---:|---|
| **原图** `genshin01.jpg`，2211×2979，3.06 MB，4:4:4 | **18.2 s** | **22.8 s** | 受 8 MB PSRAM 限制只能 1/2 解码；解码占 86% |
| **面板尺寸** 1200×1600，337 KB，4:2:0 | **4.7 s** | **12.4–13.3 s** | 1:1 解码，**推荐路径**，快 3.9 倍 |
| 主机 Python 脚本（同算法，对照） | 3.6 s | — | `host/e6_convert.py`，含 Pillow 解码/缩放 |

一句话：**把 6.6 MP 原图直接丢给 ESP32，转换要 ~18 秒；先把图缩到面板尺寸再传，只要 ~4.7 秒。**
瓶颈不是抖动（2.5 s），而是 JPEG 解码（15.7 s，其中 SD 读 5.7 s、CPU 约 10.0 s）。

---

## 2. 测试对象与处理链路

`images/genshin01.jpg`：

| 项 | 值 |
|---|---|
| 像素 | 2211 × 2979（6.59 MP） |
| 色度采样 | **4:4:4**（三个分量都是 1×1，无子采样） |
| 文件 | 3 063 621 B |
| EXIF 方向 | 1（正常） |

设备端链路（与 `host/e6_convert.py` 对齐）：

```
/sdcard/genshin01.jpg
  → TJpgDec 1/2 解码，RGB888 写入 PSRAM（1105×1489，4.94 MB）
  → 逐行 cover 裁剪 + 双线性缩放到 1200×1600
  → 蛇形 Floyd–Steinberg（REAL_PALETTE：白/黑/红/黄/蓝/绿）
  → 4bpp 打包（高 nibble = 左像素）→ 960 000 B
  → 可选：写 /sdcard/jpg2bin_bench.bin（EPD6BIN 头 18 B）
```

与主机脚本的唯一差异：主机用 Pillow LANCZOS 缩放，设备用双线性；抖动权重、调色板、误差扩散顺序、4bpp 打包规则完全一致。

**为什么原图只能 1/2 解码**：2211×2979×3 = **19.8 MB** RGB888，放不进 8 MB PSRAM。
1/2 后是 1105×1489×3 = **4.94 MB**，再加 960 KB 输出 = 5.9 MB，刚好放得下。
面板本身只有 1200×1600（1.92 MP），解码 6.6 MP 纯属浪费。

---

## 3. 实测结果

### 3.1 原图（1/2 解码）

日志：`out/bench-final.log`（本地产物，`out/` 不入库）。

| 阶段 | 耗时 | 说明 |
|---|---:|---|
| SD 顺序读（20 MHz，单独测） | 5.659 s | 3.06 MB → 528.7 KiB/s |
| `jd_prepare` | 0.290 s | 解析头 |
| `jd_decomp`（流式，含 SD 读） | 15.367 s | 0.42 MPix/s（含 I/O） |
| **解码合计** | **15.656 s** | 其中 CPU ≈ **10.0 s**（15.656 − 5.659） |
| 缩放 + 抖动 + 打包 | **2.530 s** | 1317.8 ns/px（1.92 M 像素） |
| **转换合计** | **18.19 s** | SD 里的 JPG → 内存里的 960 KB |
| 写 .bin 到 SD | 4.570 s | 960 KB → 约 210 KiB/s |
| **含写卡合计** | **22.76 s** | |

颜色占比（设备 vs 主机脚本）：

| | 白 | 黑 | 红 | 黄 | 蓝 | 绿 |
|---|---:|---:|---:|---:|---:|---:|
| 设备 | 0.4868 | 0.1050 | 0.1086 | 0.0568 | 0.1822 | 0.0606 |
| 主机 | 0.5025 | 0.1011 | 0.1108 | 0.0538 | 0.1755 | 0.0563 |

解码帧平均 RGB：设备 `182.1, 172.0, 172.6`；主机（同一图的 cover 参考）`182.8, 172.1, 173.2`。
差异来自双线性 vs LANCZOS，以及误差扩散里的整数截断；观感一致（见第 6 节）。

### 3.2 面板尺寸 JPEG（1:1 解码，推荐路径）

把原图按 cover 裁成 1200×1600、存成 4:2:0 JPEG：337 249 B（`images/genshin01_1200x1600.jpg`，生成脚本 `host/make_panel_jpeg.py`）。

日志：`out/bench-panel-jpeg-2.log`。

| 阶段 | 耗时（两次运行） | 说明 |
|---|---:|---|
| SD 读（20 MHz） | 0.596 s | 552.4 KiB/s |
| 解码（1:1） | 2.136 s | 3.08 MPix/s（含 I/O） |
| 缩放 + 抖动 + 打包 | 2.540 s | 仍是 1322.8 ns/px |
| **转换合计** | **4.676 / 4.676 s** | 稳定 |
| 写 .bin 到 SD | 7.70 / 8.00 / 8.65 s | 960 KB → 110–125 KiB/s（比读慢很多，且波动） |
| **含写卡合计** | **12.38 / 12.67 / 13.33 s** | |

颜色占比：设备 `白 0.4866 / 黑 0.1079 / 红 0.1059 / 黄 0.0597 / 蓝 0.1773 / 绿 0.0625`，主机 `0.5021 / 0.1013 / 0.1104 / 0.0541 / 0.1755 / 0.0567`，仍在 1.6 个百分点内。

### 3.3 与原图同分辨率的 1/4 解码（对照，不推荐）

把解码档位改成 1/4（552×744）后：解码 **15.17 s**，和 1/2 的 15.66 s 几乎一样。
TJpgDec 的耗时主要在熵解码/反量化（每块都要做），降低输出分辨率省不了多少；而 1/4 再放大到 1200×1600 会明显变软。**所以 1/4 不是提速手段。**

---

## 4. SD 卡 SPI 频率

同一张卡、同一个 3.06 MB 文件，固件在启动时依次以 10/20/40 MHz 重新挂载并整文件读取，校验 CRC 一致：

| SPI 时钟 | 实测吞吐 | 读 3.06 MB | CRC |
|---:|---:|---:|---|
| 10 MHz | 427.6 KiB/s | 6.996 s | 一致 |
| **20 MHz** | **528.7 KiB/s** | **5.659 s** | 一致 |
| 40 MHz | 挂载失败 | — | `ESP_ERR_INVALID_RESPONSE`，`sdmmc_enable_hs_mode_and_check: send_csd returned 0x108` |

结论：

- **20 MHz 可以用**，比 10 MHz 快约 24%，是本次实测的可用上限；40 MHz 初始化就过不去（CSD 读失败），不建议。
- 提速收益不大是因为**固定开销主导**：20 MHz 下单是 SPI 传输 3 MB 只要 1.22 s，实测 5.66 s，多出来的 ~4.4 s 摊到约 6000 个 512 B 扇区，约 **0.74 ms/扇区**。也就是说瓶颈更像“每次扇区读的延迟”，不是时钟。
- 写更慢：960 KB 写回要 4.6–8.7 s（约 110–210 KiB/s），且同一固件多次运行能差近一倍（卡内部状态/簇分配都有影响）。如果只是要上屏，**没必要写回 SD**，直接把 960 KB 流给面板即可。

---

## 5. 时间都花在哪 / 怎么再快

原图 18.19 s 的构成：

```
SD 读 5.66 s █████████
解码 CPU 10.00 s ██████████████████
抖动+打包 2.53 s ████
```

面板尺寸 4.68 s 的构成：

```
SD 读 0.60 s █
解码 CPU 1.54 s ███
抖动+打包 2.54 s █████
```

可选的优化，按性价比排序：

1. **先缩到 1200×1600（最好 4:2:0）再传**：18.2 s → 4.7 s，3.9 倍。面板分辨率就这么多，原图的 6.6 MP 对最终画面没有贡献。
2. **别写回 SD**：转换完直接按 600 B/行推给两块驱动 IC，省 4.6–8.7 s（原图路径省 20%，面板路径省 60% 以上）。
3. **双核流水**：SD 读和解码现在是串行的（读 512 B → 解码 → 再读）。用一个 core 上的生产者任务把 SD 数据预读进 256 KB 环形缓冲，另一个 core 跑 TJpgDec，可把原图路径的 5.7 s 读时间基本藏掉，预计 18.2 s → **12.5 s 左右**（未实现，属于下一步）。
4. **抖动用颜色 LUT**：现在是每像素对 6 个调色板色算欧氏距离（1318 ns/px）。用 32×32×32 的 RGB→调色板索引查找表（32 KB 内部 RAM）可以省掉这段，预计抖动 2.5 s → 1.5–1.8 s；面板尺寸路径总时间可到 ~3.9 s。
5. **1:1 快路径**：面板尺寸 JPEG 时缩放是恒等映射，可以跳过双线性，省 0.3–0.5 s。
6. **换解码器**：`espressif/esp_jpeg` 底层还是 TJpgDec，ROM 版没有源码档的 Huffman 表优化；源码档开表驱动大约快 10%，不是数量级。真正的大头是 4:4:4 的 6.6 MP 熵解码，除非改图，否则很难再快。

参考：面板本身刷新约 12 s。面板尺寸路径“JPG → 可见画面”约 4.7 + 12 ≈ 17 s；原图路径约 18.2 + 12 ≈ 30 s。

---

## 6. 正确性验证

- 设备解码帧平均 RGB 与主机一致（误差 <1/255 量级，见 §3.1）。
- 抖动后颜色占比与主机脚本相差 ≤1.6 个百分点（见 §3.1/§3.2）。
- 生成的 bin 头正确：`EPD6BIN\0`，1200×1600，1 帧，interval 150 ms；文件体 960 000 B。
- 设备端打印的 body CRC32 与拉回主机重算一致：面板路径 body `781232d6`、文件级 `e85ec3b5`；原图路径也验证过 body `4c605b71`（边界修正前的一次运行）。
- 把 bin 解回 PNG 与主机脚本产物对比：面板尺寸路径 **61% 像素完全相同**，其余是误差扩散里整数截断导致的蝴蝶效应（局部换色，整体色调不变）。目视对比见：
  - 设备原图路径：[out/jpg2bin_bench_frame0.png](../out/jpg2bin_bench_frame0.png)（边界修正前的一次运行，只影响最右/最下 1 列/1 行，观感不变）
  - 主机参考：[out/genshin01_frame0.png](../out/genshin01_frame0.png)
  - 设备面板路径：[out/jpg2bin_bench_panel_frame0.png](../out/jpg2bin_bench_panel_frame0.png)
- 设备产物肤色周围比主机略偏绿一点，来源是双线性缩放 + 整数误差；如果在意，可以在设备端改用浮点误差缓冲（会慢一点），或者干脆在主机端预抖动。

---

## 7. 复现

### 7.1 准备 SD 卡

```bash
. tools/env.sh
cd firmware/sd_bridge && idf.py -p /dev/cu.usbmodem1101 flash && cd ../..

./.venv/bin/python host/sd_bridge.py put images/genshin01.jpg /genshin01.jpg --overwrite
./.venv/bin/python host/make_panel_jpeg.py images/genshin01.jpg \
    -o images/genshin01_1200x1600.jpg --quality 90 --subsampling 2
./.venv/bin/python host/sd_bridge.py put images/genshin01_1200x1600.jpg \
    /genshin01_1200x1600.jpg --overwrite
```

> 固件会优先用 `/sdcard/genshin01_1200x1600.jpg`（存在时 1:1 解码）；删掉它、只留
> `/genshin01.jpg`，就会跑原图 1/2 解码的路径。两条路径的日志分别在
> `out/bench-final.log` 和 `out/bench-panel-jpeg-2.log`。

### 7.2 跑 benchmark

```bash
. tools/env.sh
cd firmware/jpg2bin_bench
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash

# 另开终端（或紧接着执行），从 USB-Serial-JTAG 控制台抓日志
cd ../..
./.venv/bin/python host/capture_bench.py --timeout 180 --out out/bench.log
```

固件启动后先等 3 s（给主机挂串口），然后依次：

1. 以 10/20/40 MHz 重挂 SD 并整文件读 + CRC；
2. 以 20 MHz 挂载，解码 + 抖动 + 打包；
3. 写 `/sdcard/jpg2bin_bench.bin`；
4. 打印 `BENCH ...` 汇总和 `BENCH_DONE`。

关键打印：

```
SD_SWEEP freq=20000 real=20000 size=3063621 us=5658558 speed=528.7 KiB/s crc=ec94fd39 OK
BENCH decode_us=15656138 convert_us=2530165 write_us=4569639 total_convert_us=18186303 total_with_write_us=22755942
BENCH hist white=0.4868 black=0.1050 red=0.1086 yellow=0.0568 blue=0.1822 green=0.0606
```

### 7.3 取回产物 / 恢复固件

```bash
# 拉 bin（会先覆盖 /jpg2bin_bench.bin）
./.venv/bin/python host/sd_bridge.py get /jpg2bin_bench.bin out/jpg2bin_bench.bin --overwrite
./.venv/bin/python host/e6_convert.py --decode out/jpg2bin_bench.bin -o out/jpg2bin_bench.png

# 恢复显示固件 / WiFi 测试固件
cd firmware/e6_epd && idf.py -p /dev/cu.usbmodem1101 flash
# 或 cd firmware/wifi_check && ./tools/wifi-check.sh all
```

> 当前板子上留的是 `jpg2bin_bench`，SD 卡上新增了
> `genshin01.jpg`、`genshin01_1200x1600.jpg`、`jpg2bin_bench.bin`；
> 原来的 `images.bin` 未动。

---

## 8. 相关文件

| 文件 | 作用 |
|---|---|
| [`firmware/jpg2bin_bench/`](../firmware/jpg2bin_bench/) | benchmark 固件（SD + ROM TJpgDec + 双线性 + FS-ser + 4bpp） |
| [`host/capture_bench.py`](../host/capture_bench.py) | 打开 USB-Serial-JTAG 控制台抓 `BENCH_DONE`，DTR/RTS 保持低电平避免复位 |
| [`host/make_panel_jpeg.py`](../host/make_panel_jpeg.py) | 把任意图按 cover 裁成 1200×1600、存 4:2:0 JPEG |
| `images/genshin01_1200x1600.jpg` | 面板尺寸测试图（337 KB，本地生成，不入库） |
| `out/bench-final.log` | 原图 1/2 路径日志（`out/` 不入库） |
| `out/bench-panel-jpeg-2.log` | 面板尺寸 1:1 路径日志 |
| `out/jpg2bin_bench_frame0.png` | 设备原图路径解回 PNG |
| `out/jpg2bin_bench_panel_frame0.png` | 设备面板路径解回 PNG |
