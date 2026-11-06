# wifi_check —— WiFi 连通性测试（独立工程）

不碰 EPD 屏、不碰 SD 卡，只验证板子的 WiFi 链路：

1. 以 STA 连接 `.env` 里配置的 AP；
2. 打印 DHCP 下发的 **IP / 子网掩码 / 网关 / DNS**，以及 STA MAC、主机名、BSSID、
   信道、RSSI、认证方式；
3. 对网关发 4 个 ICMP 包，验证二层/三层连通；
4. 断线后按 2 s → 4 s → … → 30 s 退避自动重连；在线时每 30 s 打印一次 RSSI 心跳；
5. 提供 UDP 发现服务（端口 3232）：上位机广播查询，设备回包告知自己的 IP。

## 凭据注入（不进源码）

密码不发在代码里，而是放在仓库根 `.env`（已在 `.gitignore` 中）：

```bash
cp .env.example .env
# 编辑 .env：
#   WIFI_SSID=your-ssid
#   WIFI_PASSWORD=xxxxxxxx
```

构建时 `main/CMakeLists.txt` 读取仓库根 `.env`，在
`build/esp-idf/main/wifi_secrets.h` 生成：

```c
#define WIFI_SSID     "…"
#define WIFI_PASSWORD "…"
```

- `.env` 变化会触发 CMake 重新配置（`CMAKE_CONFIGURE_DEPENDS`），改完直接
  `idf.py build` 即可生效；
- 密码只出现在 `build/` 和生成的固件 bin 里（WPA2 必须把口令交给固件），
  **不要外发 `build/` 目录或固件镜像**；
- 日志只打印密码长度，不打印明文。

## 构建 / 烧录

```bash
# 在仓库根
./tools/wifi-check.sh all     # build + flash + monitor
# 或
. tools/env.sh
cd firmware/wifi_check
idf.py set-target esp32s3     # 首次
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor
```

## 局域网发现（UDP，端口 3232）

上位机不需要预知设备 IP：向网段广播一个 12 字节查询包，设备收到后向请求来源
单播回复 IP、MAC、主机名。`255.255.255.255`、子网定向广播（如
`192.168.1.255`）和单播查询都会应答。

| 包 | 偏移 | 长度 | 字段 |
|---|---|---|---|
| 查询 | 0 | 8 | `EPD6DISC` |
|  | 8 | 1 | 版本 = 1 |
|  | 9 | 1 | 类型 = 0x01（QUERY） |
|  | 10 | 2 | nonce（u16 BE，原样回显） |
| 应答 | 0 | 8 | `EPD6DISC` |
|  | 8 | 1 | 版本 = 1 |
|  | 9 | 1 | 类型 = 0x02（REPLY） |
|  | 10 | 2 | nonce（回显） |
|  | 12 | 4 | IP（u32 BE） |
|  | 16 | 6 | MAC（STA） |
|  | 22 | 2 | HTTP 端口（u16 BE，0 = 暂未启用，预留给传图服务） |
|  | 24 | 1 | hostname 长度 N |
|  | 25 | N | hostname（UTF-8，无结尾 0） |

```bash
./.venv/bin/python host/discover.py              # 广播扫描（自动收集本机网段广播）
./.venv/bin/python host/discover.py --json       # JSON 输出，给其他程序用
./.venv/bin/python host/discover.py --target 192.168.1.50   # 单播自测
```

实测：

```
扫描目标 255.255.255.255, 192.168.1.255，端口 3232，超时 2.0 s ...

发现 1 台设备：

IP              MAC                主机名             HTTP
--------------  -----------------  --------------  ----
192.168.1.50  aa:bb:cc:dd:ee:ff  epd-wifi-check  -
```

固件日志（首次查询）：

```
I (6883) discovery: 已回复第 1 次查询：192.168.1.23:61240 → 39 字节
```

实现见 [`main/discovery.c`](main/discovery.c)；协议校验（魔数/版本/类型/nonce
不符）不通过时静默丢弃，端口上其他杂包也不会误答。

## 实测输出（示例 AP，2026-11-06）

```
I (1302) wifi:connected with your-ssid, aid = 2, channel 6, BW20, bssid = 00:11:22:33:44:55
I (1302) wifi:security: WPA2-PSK, phy: bgn, rssi: -58, cipher(pairwise:0x3, group:0x3), pmf:0
I (2862) wifi-check: DHCP 成功：IP=192.168.1.50 掩码=255.255.255.0 网关=192.168.1.1
I (2962) wifi-check: ===== 已联网，读取网络信息 =====
I (2962) wifi-check: STA MAC  : aa:bb:cc:dd:ee:ff
I (2962) wifi-check: 主机名   : epd-wifi-check
I (2962) wifi-check: BSSID    : 00:11:22:33:44:55
I (2962) wifi-check: 信道/RSSI: 6 / -58 dBm
I (2972) wifi-check: 认证方式 : WPA2-PSK
I (2972) wifi-check: IP       : 192.168.1.50
I (2982) wifi-check: 网关     : 192.168.1.1
I (2982) wifi-check: DNS(主)  : 192.168.1.1
I (5002) wifi-check: ping 结果：4/4 回包，min/avg/max = 8/10/14 ms —— 网关可达
```

说明：

- 认证方式 WPA2-PSK；网关/DNS 都是 `192.168.1.1`（路由器兼 DNS）；
- ping 会有偶发丢包（实测 4/4、3/4 都出现过），与省电/空口重传有关，
  本工程已 `esp_wifi_set_ps(WIFI_PS_NONE)`；
- **WPA2/WPA3 口令必须 8–63 位**，4 字符口令在四次握手阶段就会失败
  （日志会打 `reason=204` 或 `reason=15`）。

## 已知限制

- 目前只做“发现 + 连通性验证”，尚未提供 HTTP/Socket 传图；发现应答里预留了
  `HTTP 端口` 字段（当前 0），后续加 `esp_http_server` 后填上即可；
- 没有做配网（SmartConfig / softAP），凭据固定来自 `.env`；
- 部分 AP 会丢弃 `255.255.255.255`，`host/discover.py` 默认同时发子网定向广播，
  仍失败时可用 `--broadcast <网段广播地址>` 指定。
