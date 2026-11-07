# 浏览器端设备发现调研（discover.py 的 Web 化方案）

> **状态：已归档（2026-11-06）** —— 该方案暂不采用，仅作调研记录保留。

调研日期：2026-11-06　对应固件：`firmware/wifi_check`（UDP 发现，端口 3232）

## 结论先行

1. **纯浏览器做不到和 `discover.py` 等价的“广播一问、设备即答”**：浏览器没有 UDP、
   没有原始套接字、没有广播，也没有 mDNS/SSDP 浏览 API，还拿不到本机 IP/网段。
2. 但“打开静态 HTML，自动定位到设备”在**单设备、已知主机名**场景下可行：
   设备端加 **mDNS 主机名 + HTTP/CORS**，页面直接 `fetch('http://epd-xxxx.local:8080/api/info')`，
   零配置、亚秒级。本机已实测 Chrome 157 能解析 `.local` 并跨源取到 JSON。
3. 如果必须“任意设备自动列出列表”，纯浏览器无解，只能加一个**本地小助手**
   （复用现有 `host/discover.py`，加 `--serve` 暴露 `http://127.0.0.1:PORT/api/devices`）。
   页面仍是静态 HTML，跨浏览器最稳，代价是用户要起一个桌面进程。
4. 纯浏览器兜底方案是**子网 IP 扫描**（逐个 `fetch`/图片探针），但需要用户提供网段、
   慢（254 个地址、失败约 3 s/个）、噪声大，不推荐作为主路径。

## 一、浏览器的硬限制

| 能力 | 浏览器支持 | 对本需求的影响 |
|---|---|---|
| UDP / 广播 / 组播 | ❌ 只有 TCP（fetch/XHR/WebSocket）和 WebTransport（QUIC，单播、需证书） | 无法复刻广播发现协议 |
| mDNS 查询 / DNS-SD 浏览 | ❌ 无 API；WebRTC 里的 mDNS 只是把 ICE 候选混淆成 `xxx.local` | 不能枚举网内设备 |
| 读取本机 IP / 网段 | ❌ `location` 只有页面自身地址；WebRTC 候选被 mDNS 混淆 | 无法自动推导要扫的 /24 |
| 访问私有 IP（LNA/PNA） | Chrome/Edge 142+：public→local 需 `local-network` 权限（secure context）；`.local`、私有 IP 都算 local | 公网页面会弹权限框；本机页面（file:// / localhost）实测不拦 |
| 混合内容 | https 页面访问 `http://LAN` 默认被拦，LNA 授权后放宽；Safari/Firefox 无此豁免 | 页面不要托管在公网 https 上 |
| CORS | 跨源读响应必须由设备返回 `Access-Control-Allow-Origin` | 固件 HTTP 必须加 CORS |
| 端口/路径 | 只能访问明确的 URL | 设备 HTTP 端口、发现路径要固定 |

参考：[MDN Local network access](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Local_network_access)、
[MDN Request.targetAddressSpace](https://developer.mozilla.org/en-US/docs/Web/API/Request/targetAddressSpace)、
[caniuse local-network 权限](https://caniuse.com/mdn-api_permissions_permission_local-network-access)（Chrome/Edge 142+、Opera 126+、
Samsung 30+ 支持；Firefox、Safari 截至 2026-10 不支持）。

## 二、候选方案对比

| 方案 | 纯浏览器 | 设备端改动 | 上位机改动 | 速度 | 可靠性 | 多设备 | 兼容性 |
|---|---|---|---|---|---|---|---|
| **A. mDNS 名 + HTTP**（页面直接 fetch `epd-xxx.local`） | ✅ | 加 mDNS + HTTP/CORS | 写静态页 | 成功亚秒级；失败约 3 s | 中（依赖系统 mDNS 与命名） | ❌ 只能试候选名 | macOS/Windows 桌面好；Android 可能不解析 `.local` |
| **B. 子网 IP 扫描**（页面并发探测 1~254） | ✅ | 加 HTTP/CORS（固定端口/路径） | 写静态页 | 数秒~几十秒 | 低（超时、AP 隔离、端口占用） | 只能找“有 HTTP 的”设备 | 取决于 LNA 与浏览器限流 |
| **C. 本地助手 + localhost**（discover.py --serve） | ❌（要起进程） | 无（已有 UDP） | 复用现有脚本 + 静态页 | 2 s 内 | 高 | ✅ 可列全部 | 所有浏览器（页面同源） |
| D. WebSerial/WebUSB 读 IP | ❌（Chrome only、要手势） | 无 | 写页面 | 快 | 高但要有线 | ✅ | 仅 Chromium，需插线 |
| E. 浏览器扩展 + native messaging | ❌ | 无 | 写扩展 | 快 | 高 | ✅ | 安装成本高 |

## 三、选定路线（2026-11-06：单设备 / 纯浏览器 / file://）

### 目标形态

双击 `web/index.html`（`file://` 打开）→ 页面自动请求
`http://epd-wifi-check.local:8080/api/info` → 拿到设备 IP/MAC 后进入上传界面；
失败约 3 s 后回落到手动输入 IP（localStorage 记忆上次成功地址）。

### 设备端

- `esp_http_server`，端口 **8080**：
  - `GET /api/info` → JSON `{"ip","mac","hostname","fw","http_port"}`；
  - 所有响应带 `Access-Control-Allow-Origin: *`（file:// 页面 `Origin: null`，
    实测 `*` 可用），并响应 `OPTIONS` 预检；
  - 后续 `POST /api/upload` 接收 EPD6BIN。
- `mdns` 组件：主机名 `epd-wifi-check`，发布 `_http._tcp` 服务（8080）；
- 与现有 UDP 发现（3232）并存，互不影响。

### 页面端

- 单设备场景，候选地址只有两个：`epd-wifi-check.local:8080`、localStorage 里上次的 IP；
- 每个请求 2~3 s 超时；不实现候选名列表和多设备枚举（本阶段不需要）；
- 页面用原生 JS，单文件、无构建、无外部依赖，可直接 `file://` 打开。

### 验收标准

1. 板子连上 your-ssid 后，macOS 上 `dscacheutil -q host -a name epd-wifi-check.local`
   能解析到设备 IP；
2. Chrome 打开 `file://.../web/index.html`，3 s 内显示设备 IP/MAC；
3. 手动输入 IP 的兜底路径可用；
4. Safari/Firefox 行为记录在文档（若 file:// 跨源被拦，改用 localhost 托管页面，
   页面代码不变，只是入口方式变化）。

### 不在本阶段范围

- 多设备列表、本地助手（阶段 2）、子网扫描（阶段 3）；
- 图片上传（后续 `/api/upload` 与本阶段共用同一个 HTTP 服务）。

## 四、设备端改动清单（阶段 1）

1. `esp_http_server` 初始化（端口 8080），注册 `/api/info`、`/api/upload`；
2. 所有响应加 `Access-Control-Allow-Origin: *`，并响应 `OPTIONS`；
3. `mdns` 组件：`mdns_init()`、`mdns_hostname_set("epd-wifi-check")`、
   `mdns_service_add(NULL, "_http", "_tcp", 8080, NULL, 0)`；
4. `/api/info` 返回 IP/MAC/主机名/固件版本（与 UDP 发现回包字段保持一致）；
5. 命名冲突：mDNS 冲突时自动加 `-2` 后缀，页面候选名里体现；
6. 安全：局域网无鉴权接口有 CSRF 风险，上传接口建议带一次性 token 或校验 `Origin`/自定义头。

## 五、兼容性与验证清单

| 浏览器 | `.local` 解析 | 私有 IP + CORS | 备注 |
|---|---|---|---|
| Chrome/Edge 157（macOS，本次实测） | ✅ 系统 mDNS | ✅ file:// 与 localhost 页面均 200 | headless 验证，有头权限弹窗待人工确认 |
| Safari | 系统 mDNS，预期 ✅ | 待测（file:// 跨源策略较严） | 无 LNA；https 页面会被混合内容拦 |
| Firefox | 系统 mDNS，预期 ✅ | 待测（file:// 的 null origin） | 无 LNA |
| Chrome Android | ⚠️ 可能不解析 `.local` | 待测 | 移动端建议走助手或手动 IP |
| iOS Safari | 预期 ✅（mDNSResponder） | 待测 | 需真机 |

待人工验证项：

- [ ] 有头 Chrome 从公网 https 页面访问 `http://192.168.x.x` 是否弹 LNA 权限、授权后是否可用；
- [ ] Safari / Firefox 打开 `file://` 页面跨源 fetch 私有 IP 的行为；
- [ ] 设备 mDNS 名在 Windows 10/11 的解析（Bonjour 服务是否默认存在）；
- [ ] 多设备同名冲突与候选名列表耗时；
- [ ] 上传大文件（960 KB 帧）时的 CORS 预检与超时。

## 六、本次实测记录（2026-11-06，macOS + Chrome 157.0.8103.39）

1. 用 `dns-sd -P` 在 macOS 上发布 `epdtest.local → 192.168.124.19`；
   `dscacheutil -q host -a name epdtest.local` 解析成功，`curl http://epdtest.local:8765/ping` 返回 200。
2. 静态页（`file://` 打开）与 `http://localhost` 页面都能 `fetch`：
   - `http://epdtest.local:8765/ping` → 200；
   - `http://192.168.124.19:8765/ping` → 200；
   - `window.isSecureContext === true`。
3. 不存在的 `.local` 名解析失败约 3 s（`curl --max-time 3` 超时），说明**候选名列表不能串行试太多**。
4. 局限：headless Chrome 不代表有头浏览器的权限弹窗行为；Safari/Firefox 未测。

## 七、工作量估算

| 工作 | 估时 |
|---|---|
| 固件 HTTP 服务 + CORS + `/api/info` | 0.5 人日 |
| mDNS 主机名/服务 | 0.5 人日 |
| 静态页（发现 + 手动 IP + 上传 UI 雏形） | 1 人日 |
| `discover.py --serve`（可选阶段 2） | 0.5 人日 |
| 浏览器矩阵测试与兼容处理 | 0.5~1 人日 |

## 八、决策记录（2026-11-06）

| 问题 | 决定 |
|---|---|
| 设备数量 | 单台 |
| 路线 | 阶段 1：纯浏览器，设备加 mDNS + HTTP |
| 页面托管 | `file://` 双击打开 |
| 多设备 / 本地助手 / 子网扫描 | 暂不做 |

## 九、实现清单（下一步）

| 文件 | 内容 |
|---|---|
| `firmware/wifi_check/main/http_server.c/.h` | `esp_http_server` 启动、`/api/info`、CORS、`OPTIONS` |
| `firmware/wifi_check/main/mdns_service.c/.h` | `mdns_init` + 主机名 `epd-wifi-check` + `_http._tcp` 服务 |
| `firmware/wifi_check/main/main.c` | 联网后启动 HTTP/mDNS（发现服务已存在） |
| `firmware/wifi_check/main/CMakeLists.txt` | 增加 `esp_http_server`、`mdns` 依赖 |
| `web/index.html` | 单文件页面：fetch `.local` → 手动 IP 兜底 → 显示设备信息 |
| 文档 | 更新 `firmware/wifi_check/README.md`、根 README |

验收按第三节的 4 条执行；Safari/Firefox 的 `file://` 跨源行为若被拦，页面代码不变，
改为 `python3 -m http.server` 打开即可（实测 Chrome 两种方式都可用）。
