# 开发环境（macOS，2026-11-06 实装记录）

## 位置

| 组件 | 路径 | 体积 |
|---|---|---|
| ESP-IDF v5.4.4 | `~/esp/esp-idf-v5.4.4` | ~1.0 GB（含子模块） |
| 工具链 / 工具 / python env | `~/.espressif` | 3.8 GB |
| 上位机 Python | 仓库内 `.venv`（uv，3.12） | 63 MB |

## 日常使用

```bash
. ~/esp/esp-idf-v5.4.4/export.sh
export IDF_SKIP_CHECK_SUBMODULES=1     # 见下方"坑 2"，必须设
cd firmware/e6_epd
idf.py build
```

## 四个坑（都踩过）

### 1. 子模块 git clone 在代理下必断

`git clone --recursive` 走 SOCKS5 代理拉 `esp32-wifi-lib` 这类大包时反复
`SSL_ERROR_SYSCALL / early EOF`，重试也过不去（体积卡在 638 MB 不动）。

**解法**：用 GitHub codeload tarball 逐个补，脚本已提交：

```bash
PROXY=socks5h://127.0.0.1:2080 ./tools/fetch-submodules.sh
```

单模块手动补：

```bash
sha=$(git ls-tree HEAD components/unity/unity | awk '{print $3}')
curl -x $PROXY -L --fail -o /tmp/u.tgz "https://codeload.github.com/ThrowTheSwitch/Unity/tar.gz/$sha"
rm -rf components/unity/unity && mkdir -p components/unity/unity
tar -xzf /tmp/u.tgz -C components/unity/unity --strip-components=1
```

### 2. `git submodule status` 永远显示 `-`

tarball 解包没有 git 元数据，IDF 的 CMake 会判定子模块缺失并尝试 `git submodule update`（然后失败）。

**解法**：`export IDF_SKIP_CHECK_SUBMODULES=1`（IDF 内部 CI 变量，`tools/cmake/git_submodules.cmake` 里有判断）。

### 3. `install.sh` 不装 cmake / ninja

macOS 上 `install.sh esp32s3` 只装了交叉编译器、openocd、python env，`idf.py` 报
`"cmake" must be available on the PATH`。

**解法**（走国内镜像，约 60 MB）：

```bash
IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets \
  python3 ~/esp/esp-idf-v5.4.4/tools/idf_tools.py install cmake ninja
```

注意用 `python3` 调用——`idf_tools.py` 的 shebang 是 `python`，本机没有这个别名。

### 4. 厂家工程的 flash 配置与本板不符

7.09" 示例默认 2 MB flash、无 PSRAM，分区表要 4 MB → 直接报
`Partitions tables occupies 4.1MB ... does not fit in configured flash size 2MB`。

**已改** `firmware/e6_epd/sdkconfig.defaults`：

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
```

## 验证结果

```
idf.py set-target esp32s3   → OK
idf.py build                → OK
build/e6_epd.bin            → 1.1 MB（app 分区 4 MB，够）
```

`ESP-IDF v5.4.4-dirty`：源码树带 dirty 标记属正常（子模块是解包而非 clone），不影响编译。

## 状态

- 已备份出厂 16 MB 全片：`backups/factory-16mb-20261106-180436.bin`，两次读取逐字节一致，详见 [backup.md](backup.md)。
- 已把 13.3" GDEP133C02 驱动移植进 `firmware/e6_epd`（参数照抄厂家 13.3" 参考设计），并烧录验证：刷新约 27 s，日志正常，详见 [port-13.3.md](port-13.3.md)。
- 当前固件把 `out/genshin01.bin` 内嵌进固件开机直刷，不读 SD 卡。
