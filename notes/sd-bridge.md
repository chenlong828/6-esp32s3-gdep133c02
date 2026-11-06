# SD 卡桥接固件：从“目录打不开”到根因定位

> 记录 2026-11-06 实现 `firmware/sd_bridge` 时踩到的一个隐蔽 bug。结论先行：**SD 卡没有坏，FATFS 也没有坏，是路径字符串少了 NUL 终止符。**

## 1. 目标与实现

目标：ESP32-133C02-IL 不拆卡、不读卡器，通过 USB-Serial/JTAG 把板载 microSD 的内容传到主机，同时支持安全写入。

实现：

- `firmware/sd_bridge/`：FATFS + SDSPI（SPI3，CS=15 / MOSI=3 / MISO=5 / SCK=8），USB 自定义帧协议。
- `host/sd_bridge.py`：`info/df/ls/stat/crc/get/put/pull/push/mkdir/rm/rmdir/mv/mount/unmount/sync`。
- 读取路径很快跑通：`images.bin`（3,840,018 B，CRC32 `caa408b6`）成功拉到主机，整卡递归 `pull` 也完成。

## 2. 症状：写能“成功”，读不回来

第一次写测试：

```text
MKDIR /NEWDIR          -> OK
LS /NEWDIR             -> ERR 22 opendir failed
STAT /NEWDIR           -> OK, type=dir
PUT /NEW.BIN           -> ERR 22 rename failed
```

更奇怪的是：

- 根目录 `LS /` 正常；
- 新建目录在根目录列表里能看到，`STAT` 也认；
- 有的名字能打开，有的不能：`/NEWDIR`、`/T` 可以，`/TESTDIR`、`/A`、`/sdtest` 不行；
- `mkdir` 名字 15 字符可以，16 字符失败；
- `System Volume Information` 也 `opendir` 失败；
- 降低 SPI 从 20 MHz 到 10 MHz 完全没变化；
- 卡上已有 `images.bin` 始终能读，CRC 不变。

一开始怀疑过：假容量卡、SD 写信号完整性、LFN 校验和、FatFs 版本、PSRAM DMA、临时文件名以点开头、堆/栈 `FF_DIR` 差异。逐项排除后都不是。

## 3. 关键证据：路径字符串后面有垃圾

先确认 FatFs 本身没问题：

- 直接用 `f_opendir("/TESTDIR")`（栈上 `FF_DIR`）→ `FR_OK`；
- `f_stat("/TESTDIR")` → `FR_OK`；
- 同一路径经 VFS `opendir()` → `errno=22`。

说明问题在 **VFS 传给 FatFs 的路径**，不在目录内容或卡。

最终用链接器 `--wrap=f_opendir` 拦截所有 `f_opendir` 调用，记录实际 path 和返回值：

```text
0 len=15 res=6  path='0:/TESTDIR?��<'
1 len=13 res=6  path='/TESTDIR?��<'
2 len=15 res=6  path='0:/TESTDIR?��<'
3 len=15 res=12 path='1:/TESTDIR?��<'
```

对 `/TESTDIR`：

- 期望 `0:/TESTDIR`，长度 10；
- 实际长度 **15**，后面多了 5 个不可打印字节；
- `f_opendir` 返回 `FR_INVALID_NAME`（6），VFS 映射成 `EINVAL`（22）。

`/NEWDIR` 的 path 后面也有少量垃圾，但恰好没有触发非法字符，所以“有时能打开”。这解释了所有“名字相关、时好时坏”的现象。

## 4. 根因：`resolve_path()` 少了一行

`firmware/sd_bridge/main/bridge.c` 里路径解析原本是：

```c
char rel[256];
size_t rel_len = 0;
...
memcpy(rel + rel_len, s, n);
rel_len += n;
...
int n = snprintf(out, out_len, "%s/%s", SD_CARD_MOUNT_POINT, rel);
```

`rel` 是**未初始化的栈数组**，`memcpy` 只写了有效字符，**没有写结尾 `'\0'`**。`snprintf("%s", rel)` 于是一路读到栈上某个随机 NUL，把垃圾一起拼进 `full` 路径。

修复：

```c
rel[rel_len] = '\0';

int n = snprintf(out, out_len, "%s/%s", SD_CARD_MOUNT_POINT, rel);
```

边界检查原本已经保证 `rel_len + n < sizeof(rel)`，所以这个 NUL 一定放得下。

## 5. 修复后的验证

- `ls /TESTDIR`、`ls /NEWDIR` 正常；
- `f_opendir` 日志变成 `len=10 path='0:/TESTDIR'`、`res=0`；
- 整卡递归 `pull /` 成功，连之前打不开的 `System Volume Information/IndexerVolumeGuid` 也拉下来了，manifest 无错误；
- 4 KiB 文件 `PUT → CRC → GET → cmp` 一致；
- 长文件名、`MV`、`MKDIR`、`RM`、`RMDIR`、覆盖写全部通过；
- 卡上测试目录/文件清理后，`images.bin` CRC 仍是 `caa408b6`。

## 6. 教训

1. **手工拼字符串必须显式补 NUL**。`memcpy` + 长度不等于 C 字符串。
2. **不要被 “mkdir 成功但 opendir 失败” 带偏**：创建用的是路径 A，打开用的是路径 B；两边都可能被同一个字符串 bug 影响。
3. **名字长度相关、时好时坏**，优先怀疑缓冲区/终止符，而不是存储介质。
4. 读路径一直正常，是因为 `images.bin` 那次栈上恰好很快遇到 NUL；写测试换名字后垃圾变了，问题才暴露。
5. 诊断用的 `RAW/DIAG/FATOP/VFSOP/FOPLOG` 和 `--wrap=f_opendir` 已从最终固件移除，只保留生产命令。

## 7. 相关文件

- 固件说明：[`firmware/sd_bridge/README.md`](../firmware/sd_bridge/README.md)
- 上位机：[`host/sd_bridge.py`](../host/sd_bridge.py)
- 出厂备份与恢复：[`notes/backup.md`](backup.md)
