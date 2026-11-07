# jpg2bin_bench —— 设备端 JPG → EPD6BIN 性能测试固件

把 SD 卡上的 JPEG 直接交给 ESP32-S3，完成 **解码 → cover 缩放 → 6 色蛇形误差扩散 → 4bpp 打包**，用 `esp_timer` 给每个阶段计时，并打印可以 `grep` 的 `BENCH_*` 行。

算法、调色板、4bpp 打包规则与 [`host/e6_convert.py`](../../host/e6_convert.py) 一致；只有缩放核不同（设备用双线性，主机用 Pillow LANCZOS）。

---

## 1. 流程

```
/sdcard/genshin01.jpg                            (原图 2211x2979，3.06 MB)
  │  TJpgDec（ESP32-S3 ROM，RGB888）
  │  按像素数自动选档：>2.2 MP → 1/2，否则 1/1
  ▼
PSRAM RGB888（原图路径 1105x1489 ≈ 4.94 MB；面板图路径 1200x1600 = 5.76 MB）
  │  逐行 cover 裁剪 + 双线性 → 1200x1600
  │  蛇形 Floyd–Steinberg（REAL_PALETTE：白/黑/红/黄/蓝/绿）
  │  4bpp 打包（高 nibble = 左像素）
  ▼
960 000 B → /sdcard/jpg2bin_bench.bin（EPD6BIN 头 18 B）
```

同时做两件“顺带”的事：

1. 启动时以 **10 / 20 / 40 MHz** 重新挂载 SD，整文件读一遍并校验 CRC，打印吞吐（回答“SPI 能开到多少”）。
2. 解码后打印 RGB 平均值、输出块数和最右/最下坐标，便于和主机脚本对拍。

---

## 2. 构建 / 烧录 / 运行

```bash
cd /Users/chenlong/github/s6-esp32s3-gdep133c02
. tools/env.sh

cd firmware/jpg2bin_bench
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash

# 控制台在 USB-Serial-JTAG（/dev/cu.usbmodem1101），不是 UART0
cd ../..
./.venv/bin/python host/capture_bench.py --timeout 180 --out out/bench.log
```

`capture_bench.py` 打开串口时把 DTR/RTS 置低，避免 pyserial 复位芯片；固件启动后也会先等 3 s 再跑，给主机留出挂串口的时间。

`app_main` 最后会卸载 SD 并返回；要重跑，复位板子即可：

```bash
.venv/bin/python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 \
    --before default_reset --after hard_reset flash_id
```

---

## 3. SD 卡准备

固件**只读** JPEG，不格式化卡。文件路径在 `main/main.c` 顶部：

```c
#define JPEG_PATH_FULL  "/sdcard/genshin01.jpg"
#define JPEG_PATH_PANEL "/sdcard/genshin01_1200x1600.jpg"
```

启动后优先用 `JPEG_PATH_PANEL`（存在时 1:1 解码）；不存在才用原图。测原图路径：

```bash
cd firmware/sd_bridge && idf.py -p /dev/cu.usbmodem1101 flash && cd ../..

./.venv/bin/python host/sd_bridge.py put images/genshin01.jpg /genshin01.jpg --overwrite
./.venv/bin/python host/make_panel_jpeg.py images/genshin01.jpg \
    -o images/genshin01_1200x1600.jpg --quality 90 --subsampling 2
./.venv/bin/python host/sd_bridge.py put images/genshin01_1200x1600.jpg \
    /genshin01_1200x1600.jpg --overwrite

# 想测原图就删掉面板版
./.venv/bin/python host/sd_bridge.py rm /genshin01_1200x1600.jpg
```

> 板子上目前留的是本固件；恢复显示/WiFi 固件见 `firmware/e6_epd/`、`firmware/wifi_check/`。
> SD 卡上新增了 `genshin01.jpg`、`genshin01_1200x1600.jpg`、`jpg2bin_bench.bin`，原 `images.bin` 未动。

---

## 4. 实测（2026-11-06，240 MHz，SD 20 MHz）

| 输入 | 解码 | 抖动+打包 | 转换合计 | 写 bin | 含写卡 |
|---|---:|---:|---:|---:|---:|
| 原图 2211×2979（4:4:4） | 15.66 s | 2.53 s | **18.19 s** | 4.57 s | **22.76 s** |
| 1200×1600（4:2:0） | 2.14 s | 2.54 s | **4.68 s** | 7.7–8.7 s | **12.4–13.3 s** |

SD 速率：10 MHz 427.6 KiB/s；20 MHz 528.7 KiB/s（+24%）；40 MHz 挂载失败（`ESP_ERR_INVALID_RESPONSE`）。
面板尺寸路径的抖动阶段是最大 CPU 项（2.54 s，约 1.32 µs/像素）。

完整分析、颜色占比对拍和优化建议见 [`notes/jpg2bin-bench.md`](../../notes/jpg2bin-bench.md)。

---

## 5. 代码结构与可调项

| 文件 | 内容 |
|---|---|
| `main/main.c` | 全部 benchmark 逻辑：SD 扫频、TJpgDec 回调、缩放/抖动/打包、计时、写 bin |
| `main/sd_card.c/.h` | 从 `firmware/sd_bridge` 复用，`sd_card_mount(freq_khz)` 可指定 SPI 时钟；`max_transfer_sz=32768` |

可调项（`main/main.c` 顶部）：

| 宏 / 逻辑 | 默认 | 说明 |
|---|---|---|
| `PANEL_W` / `PANEL_H` | 1200 / 1600 | 面板分辨率 |
| `JPEG_PATH_FULL` / `JPEG_PATH_PANEL` | 见上 | 输入文件 |
| 解码档位 | 像素数 >2.2 MP → 1/2，否则 1/1 | 1/2 时 RGB888 ≈4.94 MB；1/1 面板图 5.76 MB，均在 8 MB PSRAM 内 |
| `SD_CHUNK` | 32 KiB | 整文件读/写缓冲 |
| `TJPG_POOL` | 32 KiB | TJpgDec 工作池（内部 RAM） |
| 调色板 `PAL` / 像素码 `CODE` | REAL_PALETTE | 与 `e6_convert.py` 完全一致 |
| `strength` | 1.0 | 误差扩散写死为 1.0；要调需改 `(e*w)>>4` |

内存占用（原图路径，PSRAM）：

```
JPEG 流式读取（不整文件缓存）
解码帧 4.94 MB + packed 0.96 MB = 5.90 MB / 8.39 MB 可用
```

内部 RAM：`mapx/mapy 16.8 KB`、错误行双缓冲 14.4 KB、当前行 7.2 KB、codes 1.2 KB、TJpgDec 池 32 KB、stdio 32 KB。

> 注意：8 MB PSRAM 放不下 2211×2979 的 RGB888（19.8 MB），所以**原图不可能 1:1 解码**，
> 1/2 是这块板的上限。面板尺寸图（1.92 MP）才能 1:1。

---

## 6. 已知限制 / 下一步

- 解码是单线程串行，SD 读与解码没有重叠；用双核 + 环形缓冲可以把原图路径的 ~5.7 s 读时间藏掉。
- 抖动逐像素 6 色搜索（~1.32 µs/px）；32³ 颜色 LUT 可望降到 ~1.5 s。
- 写回 SD 很慢且波动（110–210 KiB/s）；实际产品应直接把 packed 数据流给 EPD，而不是落盘。
- 设备端产物与主机脚本有约 39% 像素不同（误差扩散的整数截断蝴蝶效应），整体颜色占比差 ≤1.6 个百分点；如需逐像素一致，把误差缓冲改成浮点或按 1/16 定点累加。
