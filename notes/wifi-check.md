# WiFi 连通性测试（wifi_check）

## 目标

在不动 EPD 屏、不动 SD 卡的前提下，确认 ESP32-S3 能连上目标 AP、拿到 DHCP
租约，并且网关可达。凭据从 `.env` 注入，源码与 git 中不含密码。

## 工程结构

```
firmware/wifi_check/
├── CMakeLists.txt              idf 工程入口
├── sdkconfig.defaults          16MB Flash、single app large
└── main/
    ├── CMakeLists.txt          读 ../../../.env → 生成 wifi_secrets.h
    ├── wifi_secrets.h.in       模板（占位）
    └── main.c                  STA 连接 + 信息打印 + ping + 自动重连
```

生成的头文件落在 `build/esp-idf/main/wifi_secrets.h`，不属于源码树；`build/`
已被 `.gitignore` 排除。

## 实测结果（示例 AP，2026-11-06）

| 项 | 值 |
|---|---|
| 关联 | 信道 6，BSSID `00:11:22:33:44:55`，WPA2-PSK，RSSI −58 dBm |
| STA MAC | `aa:bb:cc:dd:ee:ff`（`esp_read_mac` 读到的 eFuse 地址） |
| 主机名 | `epd-wifi-check` |
| DHCP | IP `192.168.1.50` / 掩码 `255.255.255.0` / 网关 `192.168.1.1` |
| DNS | 主 `192.168.1.1`，备/兜底未分配 |
| ping 网关 | 4/4 回包，8–14 ms（另一次 3/4，偶发丢包） |

结论：WiFi 链路、DHCP、DNS、ICMP 全部正常，可以在此之上加传图服务。

## 局域网发现（UDP 3232）

- **设备侧**：`main/discovery.c` 起一个 UDP 任务，`bind(INADDR_ANY:3232)`；
  收到 12 字节查询（`EPD6DISC` + ver + type + nonce）后向来源单播回包。
  回包里的 IP / 主机名在发送时实时读取，重连换 IP 不需要重启服务。
- **上位机**：`host/discover.py` 同时发 `255.255.255.255` 和从 `ifconfig`
  解析出的子网定向广播，发送 3 次（间隔 0.4 s），用 nonce 校验归属、按 MAC 去重。
- **实测**：广播 2 s 内回包，应答 39 字节 = 25（头）+ 14（hostname
  `epd-wifi-check`）；错误魔数 / 版本 / 类型 / 短包均无应答；`--json` 可直接
  给其他程序消费；未发现设备时退出码为 1，方便脚本判断。

为什么用固定二进制头而不是 JSON：包小、无解析歧义，后续可直接按偏移扩展；
`HTTP 端口` 字段（当前 0）预留给传图服务，发现应答里就能告诉上位机该连哪个端口。

## 踩过的坑

1. **CMake 预扫描阶段没有目录作用域**
   IDF 会在 `component_get_requirements` 阶段以 `CMAKE_BUILD_EARLY_EXPANSION=1`
   再解析一遍组件 CMakeLists。此时 `set_property(DIRECTORY ...)` 直接报
   “requested directory was not found”。用该变量做守卫，只在正常配置阶段设置
   `CMAKE_CONFIGURE_DEPENDS`。
2. **事件组的位被误消费**
   报告任务用 `xEventGroupWaitBits(..., xClearOnExit=pdTRUE, ...)` 等待
   “已连接”，导致位被清掉，打印完网络信息就误判成断线、打日志
   “连接已断开”。改为 `pdFALSE`，由断线事件负责清位。
3. **凭据注入的验证**
   临时把 `.env` 的 SSID 改成 `your-ssid-RECONF-TEST` 再 `idf.py build`，确认
   生成头文件跟着变、且构建自动 reconfigure；随后恢复。说明改 `.env` 后
   直接编译即可，无需手动 `reconfigure`。
4. **口令长度**
   最初提供的口令只有 4 个字符，WPA2/WPA3 要求 8–63 位，ESP32 会在四次握手
   阶段失败（`reason=204`/`15`）。现在代码里对长度做了告警。

## 后续

若要“WiFi 传图”，建议在本工程基础上升级：

- `esp_http_server` 提供 `POST /upload`（EPD6BIN 分块接收）和 `GET /` 状态页；
- 接收时写入 PSRAM 缓冲或临时文件，校验 magic/宽高/帧数后调用
  `gdep133c02_display()`；
- 注意 13.3" 单帧 960 000 B，8 MB PSRAM 可以放 8 帧，缓冲策略要按帧数设计。
