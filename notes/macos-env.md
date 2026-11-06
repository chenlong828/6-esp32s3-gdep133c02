# macOS 上搭 ESP32（ESP32-S3）开发环境

> 结论：**有，而且是一等公民**。乐鑫官方独立维护 macOS 安装文档（含 Apple Silicon 原生工具链，不需要 Rosetta），IDE 侧 VSCode / Arduino / PlatformIO 全都支持。
> 本机体检时间：2026-11-04，macOS 26.6.2 / arm64。

## 0. 这台机器现状

| 项 | 状态 |
|---|---|
| Xcode Command Line Tools | ✅ `/Applications/Xcode.app/Contents/Developer` |
| Homebrew | ✅ 6.0.22 |
| git | ✅ 2.50.1 |
| cmake / ninja / dfu-util / ccache | ❌ 都没装 |
| python3 | ⚠️ 只有系统 3.9.6（IDF 6.x 要求 ≥3.10） |
| ESP-IDF / `~/.espressif` | ❌ 未安装 |
| VSCode / Arduino IDE | ❌ 未安装 |
| 已连接开发板 | 当前无（`/dev/cu.*` 只有蓝牙口） |

## 1. 三条路线怎么选

| 路线 | 适合 | 代价 |
|---|---|---|
| **A. 命令行 ESP-IDF**（EIM 或 git clone） | 长期做产品、要 CI、要多版本共存 | 需要记环境变量（`export.sh`） |
| **B. VSCode + ESP-IDF 扩展** | 图形化烧录/监视、断点调试 | 首次配置向导较慢，占空间大 |
| **C. Arduino IDE 2 + esp32 core** | 只想点屏、跑 7.09" 那份 Arduino 示例 | 大工程不好管，库版本冲突常见 |
| D. PlatformIO（可选） | 喜欢 `platformio.ini` 的团队 | 与 IDF 版本对齐不如 A/B 直接 |

## 2. 路线 A：命令行（推荐，最可控）

### 2.1 装依赖

```bash
brew install cmake ninja dfu-util ccache python@3.11
```

> IDF 4.4 / 5.4 用 Python 3.11（3.12 移除 `distutils`，4.4 的 `install.sh` 会挂）；IDF 6.x 用 3.12+。

### 2.2 装 ESP-IDF —— 两种方式

**新方式（IDF v6.0+，官方推荐）**

```bash
brew tap espressif/eim
brew install eim            # 或 brew install --cask eim-gui（图形界面）
eim install                 # 装最新稳定版
eim install -i v5.4.4       # 装指定版本（本项目要用的）
eim --help                  # 看所有参数
```

**老方式（v5.x 及以前，仍受支持）**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh               # 每开一个新终端都要 source 一次
```

多个版本共存：装到不同目录，用各自的 `export.sh` 切换即可（`eim` 装的多版本也能直接切）。

### 2.3 编译烧录

```bash
idf.py set-target esp32s3
idf.py menuconfig            # 需要时
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

退出 monitor：`Ctrl+]`。

## 3. 路线 B：VSCode 扩展

1. 装 VSCode（Apple Silicon 版）；
2. 扩展市场搜 **ESP-IDF**（Espressif 官方）；
3. 首次打开会引导 "Configure ESP-IDF Extension" → 选 Express 安装，可指定 IDF 版本与安装路径；
4. 之后底部状态栏直接点 🔨 编译、⚡ 烧录、📺 监视。

扩展里也能直接选 `/dev/cu.*` 串口，比命令行少踩环境变量的坑。

## 4. 路线 C：Arduino IDE 2

1. 装 Arduino IDE 2（arm64）；
2. Preferences → Additional Boards Manager URLs 填
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Boards Manager 搜 `esp32`（Espressif Systems）装 3.x；
4. 开发板选 **ESP32S3 Dev Module**，注意设：
   - Flash Size 16 MB（13.3" 板）/ 8 MB（若按 7.09 配置）
   - PSRAM: OPI PSRAM（13.3" 板是 Octal）
   - USB CDC On Boot: Enabled（S3 原生 USB 串口）

命令行版：`brew install arduino-cli`。

## 5. 串口与驱动

| 板子上的 USB | macOS 设备名 | 驱动 |
|---|---|---|
| ESP32-S3 原生 USB（USB-Serial-JTAG） | `/dev/cu.usbmodem*` | **免驱** |
| CH343 / CH340（外置 USB-UART） | `/dev/cu.wchusbserial*` | 装 WCH 官方 macOS 驱动，装完需在"隐私与安全性"里允许 |
| CP2102 | `/dev/cu.usbserial-*` | 装 SiLabs 驱动 |

插上板子先 `ls /dev/cu.*` 看名字；Mac 上别用 `/dev/tty.*`（会被 DCD 信号卡住）。

## 6. 和这两个示例工程的版本对应

| 工程 | IDF 版本 | 说明 |
|---|---|---|
| 13.3" `GDEP133C02` | **4.4.4**（旧式 `component.mk`） | 用 5.x 打开会报组件风格错误；想省事就按 4.4.4 装一套，或用 5.4 手工移植（`.c` 里 API 基本没变） |
| 7.09" `gdeb0709e01_demo` | **5.4.4**（标准 CMake） | 直接 `idf.py build` 就能过；也带 Arduino 版本 |

装两套 IDF 是最省心的：`~/esp/esp-idf-4.4.4` 和 `~/esp/esp-idf-5.4.4`，各 source 各的。

## 7. 常见坑

1. **Python 3.12 + IDF 4.4** → `ModuleNotFoundError: distutils`，用 3.11。
2. **`idf.py: command not found`** → 忘了 `. ./export.sh`（注意点号后要有空格）。
3. **烧录报 "Failed to connect"** → 按住 BOOT 再点 RST 进下载模式；或换根数据线（很多线只有电没有数据）。
4. **权限** → 原生 USB 免驱；CH343 需装驱动并在系统设置里放行。
5. **Xcode 路径异常** → `sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer`。
6. **构建慢** → `brew install ccache`，IDF 会自动用。
7. **Apple Silicon 上跑 xtensa 工具链** → 官方 arm64 包早就有了，若报 "Bad CPU type" 才是缺 Rosetta（`softwareupdate --install-rosetta`）。

## 8. 最短路径（复制粘贴版）

```bash
brew install cmake ninja dfu-util ccache python@3.11
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh
cd /path/to/gdeb0709e01_demo && idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```
