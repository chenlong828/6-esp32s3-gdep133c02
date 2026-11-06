# 13.3" GDEP133C02 移植记录（2026-11-06）

## 做了什么

把 `firmware/e6_epd` 从 7.09"（GDEB0709E01）基线改成 13.3"（GDEP133C02）：

| 项 | 7.09" 基线 | 13.3" 现在 |
|---|---|---|
| 驱动文件 | `main/GDEB0709E01.c` | `main/GDEP133C02.c`（旧文件保留、不编译） |
| 初始化参数 | 7.09" 一套 | 照抄 `vendor/gdep133c02` 的 13.3" 一套（PSR `DF 69`、CDI `F7`、AN_TM `C0 1E 1E CE CE CE 15 15 55`、BTST `E8 28`、DRF `01`、POF `00`，且不带 PLL/POFS/DCDC/CMDA4） |
| SPI 速率 | 20 MHz | 10 MHz（与 13.3" 参考设计一致） |
| 图像来源 | `image.h` 的 `gImage[]` | `out/genshin01.bin`，用 `EMBED_FILES` 打进固件 |
| BUSY 等待 | 无超时 | 90 s 超时，面板没接好会报错而不是死等 |
| 收尾 | `POF + 0xA5` + PWR 断电 | 同（沿用 7.09" 已验证做法） |

传输骨架（双 CS、DTM 每颗 IC 只发一次、CS 全程拉低、每行前 300 B 给 Master）与 7.09" 相同，
因为两屏都是 1200×1600 双 IC、4bpp、960 000 B 布局。

## 图像打包

- `firmware/e6_epd/CMakeLists.txt` 在 configure 阶段把 `out/genshin01.bin` 同步到 `main/assets/`；
- `main/CMakeLists.txt` 用 `EMBED_FILES "assets/genshin01.bin"` 打包，符号 `_binary_genshin01_bin_start/_end`；
- `main.c` 运行时校验 `EPD6BIN` 头（magic / 1200×1600 / frames / interval），逐帧调用 `gdep133c02_display()`，不做任何旋转或抖动；
- 换图只需替换 `out/genshin01.bin` 后重新 `idf.py build`。

固件体积：app 1,172,224 B（4 MB 分区余 72%），其中内嵌图 960,018 B 在 flash 的 `.rodata` 里。

多帧 bin 也支持：每帧刷新前会重跑一次面板初始化（`gdep133c02_reinit()`，与厂家示例一致）。

## 实测（2026-11-06 18:49，板子 `/dev/cu.usbmodem1101`）

```
I (873) demo: === GDEP133C02 13.3" 内嵌图演示 ===
I (883) demo: 内嵌数据：960018 字节
I (883) demo: 面板 1200x1600，帧数 1，间隔 150 ms
I (1113) GDEP133C02: 初始化完成（1200x1600，双 IC）
I (1113) demo: 刷新第 1/1 帧...
I (28429) GDEP133C02: 刷新完成
I (28469) GDEP133C02: 面板已休眠
I (28469) demo: 演示结束，画面保持。
```

- 初始化到刷完约 **27.3 s**（全波形），比规格书的 12 s 长，属 E6 13.3" 正常量级；
- 日志走 **USB-Serial-JTAG 二级控制台**（`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` 原本就开着），UART0 不用引出也能看；
- 刷完 POF + 拉低 PWR，画面保持。

## 待确认

1. **画面方向/颜色是否正常**（需要人眼看面板）：固件只照抄字节，官方现行约定是"左上角起、行优先、高 nibble 在左"；若出现倒置/镜像，应在上位机加 `--rotate 180` / `--flip`，不要改固件。
2. 刷新时间是否还能压（13.3" 参考设计没有 PLL 命令，7.09" 用 `PLL 0x08` 调帧率，能否套用待试）。
3. 深睡：规格书写 `0x07 + 0xA5`，当前用的是两份例程的 `0x02 + 0xA5`，待机电流未实测。
