# ESP 开发库功能盘点（ESP-IDF v5.4.4 / ESP32-S3）

> 盘点对象：本仓库的 ESP 侧开发环境 + 厂家示例库
> 环境：ESP-IDF **v5.4.4** @ `~/esp/esp-idf-v5.4.4`（107 个组件，FreeRTOS **V10.5.1**）
> 芯片：ESP32-S3R8（双核，最高 240 MHz，本工程跑 160 MHz；16 MB Flash、8 MB Octal PSRAM；Wi-Fi 2.4 GHz + BLE 5）
> 盘点日期：2026-11-06

---

## 0. 先分清"哪个库"

| 层 | 位置 | 实际提供什么 | 本项目用了什么 |
|---|---|---|---|
| **厂家示例库** | `vendor/gdep133c02/`、`vendor/gdeb0709e01/` | 只有点屏：SPI/GPIO 封装 + EPD 私有命令 + 测试图 | 13.3" 初始化参数、双 CS 传输骨架 |
| **平台 SDK** | ESP-IDF v5.4.4 | Wi-Fi、BLE、SD/FATFS、HTTP/MQTT/OTA、FreeRTOS…… | 只用了 SPI / GPIO / USB-Serial-JTAG 控制台 |
| **出厂固件**（二进制） | `backups/factory-16mb-*.bin` | Arduino 写的 SD 播放器，读 `/images.bin` | 已被自编固件覆盖 |

关键事实：**厂家没有提供 Wi-Fi / SD / USB 传图的实现**（只有一份没被调用的 `usbcdc.c`）。板级手册第 20 页原文：

> "友情提醒，官网只提供最基础的刷图程序。SD卡功能，wifi功能，USB通信功能等，需自行开发，或与我们的销售取得联系。"

所以"能不能连 Wi-Fi / 读 SD 卡"分两件事：**硬件全支持，软件要自己用 ESP-IDF 写**。

---

## 1. 结论

- **ESP-IDF 5.4.4 能力齐全**：Wi-Fi STA/AP、BLE、SD 卡 + FATFS、SPIFFS/LittleFS/NVS、HTTP/HTTPS/MQTT/WebSocket、OTA、TLS、FreeRTOS 多任务，全部已随 SDK 安装，可直接调用。
- **当前固件是"单任务点屏器"**：`firmware/e6_epd` 只调 SPI/GPIO，把内嵌图刷上屏后休眠；Wi-Fi、SD 一行代码都没有。
- **多任务有，而且是内核级**：FreeRTOS 双核抢占式调度，任务/队列/信号量/事件组/软件定时器/看门狗一应俱全（见 §4）。

---

## 2. ESP-IDF 能力矩阵（ESP32-S3）

状态图例：**已用** = 当前固件在跑；**可加** = SDK 已具备、加代码即可；**未用** = 组件/分区存在但没启用。

### 2.1 无线与网络

| 能力 | 组件 / 头文件 | 关键 API | 本项目 |
|---|---|---|---|
| Wi-Fi STA 连路由器 | `esp_wifi` / `esp_wifi.h` | `esp_netif_init` → `esp_wifi_init` → `esp_wifi_set_mode(WIFI_MODE_STA)` → `esp_wifi_set_config` → `esp_wifi_start` → `esp_wifi_connect` | 可加 |
| Wi-Fi SoftAP 自建热点 | `esp_wifi` | `esp_netif_create_default_wifi_ap` + `WIFI_MODE_AP` | 可加 |
| STA + AP 并存 | `esp_wifi` | `WIFI_MODE_APSTA` | 可加 |
| 扫描 / 信号强度 | `esp_wifi` | `esp_wifi_scan_start`、`esp_wifi_sta_get_ap_info` | 可加 |
| WPA2 / WPA3-SAE / OWE / 企业级 EAP | `esp_wifi` + `wpa_supplicant` | 配置项已开（`CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y`、`..._ENTERPRISE_SUPPORT=y`） | 可加 |
| SmartConfig / WPS / Easy Connect | `esp_wifi` | `esp_smartconfig_start`、`esp_wps_*`、`esp_easy_connect` | 可加 |
| ESP-NOW 无连接组网 | `esp_wifi` / `esp_now.h` | `esp_now_init`、`esp_now_send`、`esp_now_register_recv_cb` | 可加 |
| Wi-Fi Mesh | `esp_wifi` / `esp_mesh.h` | `esp_mesh_init`、`esp_mesh_start` | 可加 |
| TCP/IP 协议栈 | `lwip` / `esp_netif` | BSD socket、DHCP、DNS、`getaddrinfo` | 可加 |
| SNTP 对时 | `lwip` / `esp_sntp.h` | `esp_sntp_init`、`sntp_set_time_sync_notification_cb` | 可加 |
| Wi-Fi 配网（SoftAP / BLE） | `wifi_provisioning` | `wifi_prov_mgr_init/start` | 可加 |
| BLE（NimBLE 或 Bluedroid） | `bt` | `esp_bt_controller_init`、`esp_bluedroid_init` / `nimble_port_init` | 可加（默认未编） |
| BLE Mesh / HID | `bt` / `esp_hid` | `esp_ble_mesh_*`、`esp_hid_device_*` | 可加 |
| mDNS 局域网发现 | 托管组件 `espressif/mdns` | `mdns_init`、`mdns_service_add` | 可加 |
| 射频低功耗 | `esp_pm` + `esp_wifi` | modem sleep、light sleep、DFS | 可加 |

> 硬件限制：S3 只有 **2.4 GHz 802.11 b/g/n**，没有 5 GHz / Wi-Fi 6；蓝牙只有 **BLE 5**，没有经典蓝牙。板级手册也强调配网必须用 2.4 GHz。

### 2.2 应用层协议

| 能力 | 组件 | 关键 API | 备注 |
|---|---|---|---|
| HTTP 客户端（下载图片） | `esp_http_client` | `esp_http_client_init/perform`、`esp_http_client_read` | 支持 HTTPS（mbedTLS） |
| HTTP 服务器（网页传图） | `esp_http_server` | `httpd_start`、`httpd_register_uri_handler` | 可做 `/upload` 收 960 KB 帧 |
| HTTPS 服务端 | `esp_https_server` | `httpd_ssl_start` | 需证书 |
| WebSocket | 托管组件 `espressif/esp_websocket_client` | `esp_websocket_client_start` | — |
| MQTT / MQTT 5 | `mqtt` | `esp_mqtt_client_init/start` | 支持 TLS/WS |
| TLS / 证书 | `mbedtls`、`esp-tls` | `esp_tls_init/conn`、x509 证书束 | 见 `examples/protocols/https_x509_bundle` |
| SMTP / Modbus / ICMP | 示例 | `examples/protocols/*` | — |
| OTA 升级 | `app_update`、`esp_https_ota` | `esp_ota_begin/write/end`、`esp_https_ota` | 需要 OTA 分区 |

### 2.3 存储与文件系统

| 能力 | 组件 | 关键 API | 本项目 |
|---|---|---|---|
| SD 卡（SPI 模式） | `esp_driver_sdspi`、`sdmmc`、`fatfs` | `spi_bus_initialize` → `esp_vfs_fat_sdspi_mount("/sdcard", ...)` | 可加（板载卡槽） |
| SD 卡（SDMMC 4 线） | `esp_driver_sdmmc` | `esp_vfs_fat_sdmmc_mount` | 板子未引出 4 线 |
| FATFS 文件读写 | `fatfs` + VFS | `fopen/fread/fwrite/fclose`、`opendir/readdir` | 可加 |
| SPIFFS | `spiffs` | `esp_vfs_spiffs_register` | 分区表里没有 |
| LittleFS | 托管组件 `joltwallet/littlefs` | `esp_littlefs_mount` | 可加 |
| NVS 键值存储 | `nvs_flash` | `nvs_open`、`nvs_set_str`、`nvs_commit` | 有 24 KB 分区，未用 |
| 磨损均衡 | `wear_levelling` | `wl_mount` | 可加 |
| 分区表 API | `esp_partition` | `esp_partition_find/find_first`、`read/write` | 未用 |

> 板载 SD 卡槽引脚（手册）：**CS=IO15、MOSI=IO3、MISO=IO5、SCK=IO8**，只有 4 根线 → 只能走 **SPI 模式**（1-bit），没有卡检测（CD）脚。

### 2.4 外设与硬件接口

| 类别 | 组件 | 说明 |
|---|---|---|
| GPIO | `esp_driver_gpio` | 本工程已用（CS/RST/DC/BUSY/PWR） |
| SPI | `esp_driver_spi` | 本工程已用：`SPI2_HOST` @ 10 MHz 驱动 EPD；`SPI3_HOST` 空闲 |
| I2C / I2S / UART | `esp_driver_i2c/i2s/uart` | 可接传感器、音频、串口外设 |
| USB Serial/JTAG | `esp_driver_usb_serial_jtag` | 本工程已用：`/dev/cu.usbmodem1101` 控制台 |
| USB OTG（Host/Device） | 托管组件 `espressif/esp_tinyusb` | 可做 U 盘、HID、CDC |
| RMT / LEDC / MCPWM / PCNT | `esp_driver_*` | 红外、WS2812、PWM、电机、编码器 |
| ADC / 温度 / 触摸 | `esp_adc`、`esp_driver_tsens`、`touch_element` | 电池电压、模拟传感器（分压引脚需查原理图） |
| LCD / Camera | `esp_lcd`、`esp_driver_cam` | S3 的 LCD_CAM 外设（I80/RGB 屏、并口摄像头） |
| TWAI（CAN） | `esp_driver_twai` | 需外部收发器 |
| ULP 协处理器 | `ulp` | 深睡时做低功耗采样 |

### 2.5 系统、调度与调试

| 能力 | 组件 | 说明 |
|---|---|---|
| 多任务 / 队列 / 信号量 | `freertos` | 见 §4 |
| 高精度定时器 | `esp_timer` | 微秒级单次/周期回调（在定时器任务上下文） |
| 事件循环 | `esp_event` | Wi-Fi/IP 事件、自定义事件 |
| 电源管理 | `esp_pm` | DFS 调频、light sleep、自动休眠 |
| 深睡 / 唤醒 | `esp_system` | `esp_deep_sleep_start`、GPIO/定时器唤醒 |
| 看门狗 | `esp_system` | 任务看门狗 5 s、中断看门狗 |
| 堆管理 | `heap` | 内部 RAM / PSRAM 分区、`heap_caps_*` |
| 日志 / 控制台 | `log`、`console` | `ESP_LOGI`、`esp_console` 命令行 |
| 崩溃分析 | `espcoredump`、`esp_gdbstub` | Core dump、JTAG 调试 |
| 性能分析 | `perfmon`、`app_trace` | 计数、系统跟踪 |

### 2.6 OTA 与安全

| 能力 | 组件 | 说明 |
|---|---|---|
| 固件升级 | `app_update`、`esp_https_ota` | 需要 ota_0/ota_1 双分区（当前分区表没有） |
| TLS | `mbedtls`、`esp-tls` | 客户端/服务端证书 |
| 安全启动 / Flash 加密 | `bootloader`、`esp_security` | eFuse 一次性烧录 |
| NVS 加密 | `nvs_sec_provider` | 密钥存独立 `nvs_keys` 分区（配合 flash 加密） |
| 本地控制 | `esp_local_ctrl`、`protocomm` | BLE/串口配置设备 |

### 2.7 ESP32-S3 没有的、以及本板没引出的（选型避坑）

- 5 GHz Wi-Fi、Wi-Fi 6
- 经典蓝牙（只有 BLE 5）
- IEEE 802.15.4 / Thread / Zigbee（那是 C6/H2 系列）
- 内置以太网 MAC（可外挂 W5500 等 SPI 以太网）
- JPEG / PPA / ISP 硬件加速（P4 才有）
- SDMMC 4 线模式（板子只引出 SPI 四根线）

### 2.8 还能装第三方组件

ESP-IDF 的组件管理器（`main/idf_component.yml`）可以直接拉官方/第三方组件，不必手工移植：LVGL、LittleFS、mDNS、TinyUSB、WebSocket、esp-box、各种传感器驱动等，`idf.py build` 时自动下载。上面的表格里标"托管组件"的就是这一类。

---

## 3. 厂家示例库到底给了什么

`vendor/gdep133c02/GDEP133C02/main/` 只有 4 个源文件 + 一张图：

| 文件 | 内容 |
|---|---|
| `comm.c/.h` | SPI 初始化/收发（`initialSpi`、`spiTransmitData`、`spiTransmitLargeData`…）、GPIO 读写、`delayms` |
| `GDEP133C02.c/.h` | 面板驱动：`initEPD`、`epdDisplay`、`epdDisplayColor`、`epdDisplayColorBar`、`writeEpdImage`、局部刷新、`checkDriverICStatus` |
| `main.c` | 开机跑四个测试：彩条 → 图片 → 棋盘格 → 纯色，然后死循环 |
| `usbcdc.c`、`*.pem` | **没被调用**的 USB CDC 和 HTTPS 证书残留 |

**没有的东西**：Wi-Fi、SD 卡、文件系统、HTTP、任务创建——一个都没有。整个示例只在 `app_main` 里顺序执行，没有建过任何任务（只用到 `vTaskDelay` 让出 CPU）。

7.09" 示例（`vendor/gdeb0709e01`）更干净，但同样只有点屏，属于"纯驱动基线"。

出厂固件（Arduino，SD 播放版）倒是实现了 SD 读取 + `/images.bin` 轮播，但只给了二进制，没有源码。从字符串看它按同样的"魔数 + 宽高 + 帧数 + 间隔"头结构解析（具体 magic 比较方式未实测），所以自己补 SD 读取并不难，解析逻辑可复用 `main.c`（见 §5）。

### 3.1 如果走 Arduino 路线

这块板官方也支持 Arduino IDE（出厂固件就是 Arduino 3.x，内核 IDF 5.5）。Arduino 这边对应的库是 `WiFi.h`、`WebServer.h`、`SD.h`、`SPI.h`、`FS.h`、`Wire.h`、`ESPmDNS.h`、`ArduinoOTA.h` 等，底层同样是 ESP-IDF + FreeRTOS；`loop()` 本身就跑在一个名为 `loopTask` 的 FreeRTOS 任务里（优先级 1），照样可以 `xTaskCreate` 加自己的任务。本仓库的固件走的是 ESP-IDF 原生工程，没有 Arduino 模板。

---

## 4. 多任务管理：有，FreeRTOS

### 4.1 当前工程的实际参数（从 sdkconfig 读出）

| 项 | 值 | 含义 |
|---|---|---|
| 内核版本 | FreeRTOS **V10.5.1** | ESP-IDF 自带 |
| 核心数 | 2（`CONFIG_FREERTOS_UNICORE` 未开） | 双核 |
| 调度模式 | **AMP**（`CONFIG_FREERTOS_SMP` 未开） | 任务可绑核也可不绑，同一时刻只在一个核上跑 |
| Tick | **100 Hz** | 1 tick = 10 ms，`vTaskDelay(1)` 最少睡 10 ms |
| 优先级 | 0–24（`configMAX_PRIORITIES=25`） | 0 = idle，数值越大越高 |
| `app_main` 所在任务 | `main`，CPU0，优先级 1，栈 3584+512 B | 返回后任务自动删除 |
| 任务看门狗 | 5 s，监视两个 idle 任务 | 长时间不让出 CPU 会重启 |
| 栈溢出检测 | Canary | `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` |
| 任务栈放 PSRAM | 允许 | `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`、`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` |
| 静态分配 | 已开 | 可用 `xTaskCreateWithCaps` 指定内存属性 |

### 4.2 能用的 API

| 类别 | 头文件 | 常用函数 |
|---|---|---|
| 任务 | `freertos/task.h` | `xTaskCreate`、`xTaskCreatePinnedToCore`、`vTaskDelete`、`vTaskDelay`、`vTaskDelayUntil`、`xTaskGetTickCount`、`uxTaskPriorityGet`、`vTaskPrioritySet`、`vTaskSuspend/Resume` |
| 队列 | `freertos/queue.h` | `xQueueCreate`、`xQueueSend`、`xQueueReceive`、`xQueueSendFromISR`、`xQueuePeek` |
| 信号量 / 互斥 | `freertos/semphr.h` | `xSemaphoreCreateBinary`、`xSemaphoreCreateMutex`、`xSemaphoreTake/Give`（互斥量带优先级继承） |
| 事件组 | `freertos/event_groups.h` | `xEventGroupCreate`、`xEventGroupSetBits`、`xEventGroupWaitBits` |
| 任务通知 | `freertos/task.h` | `xTaskNotifyGive`、`ulTaskNotifyTake`（比队列更轻） |
| 流/消息缓冲 | `freertos/stream_buffer.h`、`message_buffer.h` | 字节流、带长度消息 |
| 软件定时器 | `freertos/timers.h` | `xTimerCreate`、`xTimerStart`（回调在定时器任务，优先级 1） |
| 高精度定时器 | `esp_timer.h` | `esp_timer_create/start_once/start_periodic`（微秒级） |
| 看门狗 | `esp_task_wdt.h` | `esp_task_wdt_add`、`esp_task_wdt_reset` |
| 任务状态查看 | `freertos/task.h` | `vTaskList`、`uxTaskGetStackHighWaterMark`（需开 trace 选项） |

### 4.3 六个容易踩的点

1. **栈大小单位是字节**。IDF 改了原生 FreeRTOS 的"字"语义：`xTaskCreate(..., 4096, ...)` 就是 4 KB。
2. **`app_main` 不是主循环**。它跑在 `main` 任务里，返回即删除该任务；要常驻就自己建任务或 `while(1)`。
3. **tick 只有 100 Hz**。`vTaskDelay(1)` = 10 ms；要更细的延时用 `esp_rom_delay_us` / `esp_timer`。
4. **看门狗 5 s**。刷屏时如果死等 BUSY 而不 `vTaskDelay`，idle 任务得不到调度 → 触发 TWDT 重启。现有驱动的 BUSY 轮询里有延时，所以 27 s 刷新不会重启。
5. **双核共享资源要加锁**。SPI 总线、全局缓冲、文件句柄都不是线程安全的；跨任务用互斥量，ISR 里只能用 `*FromISR` 版本。
6. **PSRAM 栈能放但慢**。默认栈在内部 RAM；大栈任务可用 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`，但访问 PSRAM 有 cache 延迟，热路径别这么干。

### 4.4 单任务 vs 多任务（本屏场景）

当前固件是"单任务顺序流"：初始化 → 刷一帧（约 27 s）→ 休眠 → 死循环。这在只显示一张内嵌图时最简单，也最省 RAM。

一旦要加 Wi-Fi 下载 / SD 轮播 / 按键，单任务就不合适了：刷一帧要 27 s，期间没法响应 HTTP 请求、没法读卡、没法处理按键。正确做法是把"刷屏"做成一个独立任务，网络/存储任务只管生产帧数据，通过队列交接。

---

## 5. 在本板加 Wi-Fi + SD + 多任务

### 5.1 总线与引脚分配

| 用途 | 总线 | 引脚 | 现状 |
|---|---|---|---|
| EPD | **SPI2_HOST** @ 10 MHz | SCK=9、MOSI=41、MISO=40、CS_M=18、CS_S=17、RST=6、DC=2、BUSY=7、PWR=45 | 已用 |
| SD 卡 | **SPI3_HOST** @ 20 MHz | SCK=8、MOSI=3、MISO=5、CS=15 | 空闲 |
| 按键 | GPIO | SW2=12、SW3=13、SW4=14（高电平触发） | 空闲 |
| 控制台 | USB-Serial-JTAG | Type-C | 已用 |

EPD 与 SD 分属两个 SPI 主机，**不会互相抢总线**；但两者都在 `main` 里初始化时，注意先 `spi_bus_initialize` 再挂设备。

### 5.2 推荐任务划分

```
app_main (CPU0, prio 1)
  ├─ 初始化 NVS / netif / Wi-Fi / SD
  ├─ 创建队列 s_frame_q（深度 2，元素 = 帧指针）
  ├─ net_task  (CPU0, prio 4, 栈 12 KB)  → Wi-Fi 连接、HTTP 下载、SD 读取
  └─ epd_task  (CPU1, prio 6, 栈 4 KB)   → 等队列、刷屏、休眠
```

- `epd_task` 独占 SPI2 和面板状态，27 s 刷新期间不阻塞网络；
- `net_task` 把 960 KB 帧写进 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`），把指针丢进队列；
- 队列深度 2 就够：刷屏慢，深了只会占 PSRAM；
- 如果以后有第二个任务也要碰 SPI2（比如状态灯、局部刷新），再加 `xSemaphoreCreateMutex` 保护总线。

### 5.3 代码骨架

**Wi-Fi STA（最小可用）**

```c
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();                       // 生产代码应加退避
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI("wifi", "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

void wifi_sta_start(const char *ssid, const char *pass)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}
```

**SD 卡挂载（SPI 模式）**

```c
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_MOSI 3
#define SD_MISO 5
#define SD_SCLK 8
#define SD_CS   15

sdmmc_card_t *sd_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;                        // EPD 占 SPI2，SD 走 SPI3

    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO));

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = SD_CS;

    esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mnt, &card));
    return card;
}
```

挂上后就是标准 POSIX：`fopen("/sdcard/images.bin", "rb")`、`fread`。若出厂固件的 `images.bin` 与本仓库 `out/*.bin` 同构（`EPD6BIN` 头 + 帧数据，待实测确认），解析逻辑可直接复用 `main.c` 里那段。

**任务 + 队列**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

typedef struct { uint8_t *data; } frame_msg_t;
static QueueHandle_t s_frame_q;

static void epd_task(void *arg)
{
    frame_msg_t msg;
    ESP_ERROR_CHECK(gdep133c02_init());
    for (;;) {
        if (xQueueReceive(s_frame_q, &msg, portMAX_DELAY) != pdTRUE) continue;
        ESP_ERROR_CHECK(gdep133c02_reinit());
        ESP_ERROR_CHECK(gdep133c02_display(msg.data));   // ~27 s，内部 vTaskDelay 轮询 BUSY
        ESP_ERROR_CHECK(gdep133c02_sleep());
        free(msg.data);                                  // PSRAM，用 free 即可
    }
}

void start_pipeline(void)
{
    s_frame_q = xQueueCreate(2, sizeof(frame_msg_t));
    xTaskCreatePinnedToCore(epd_task, "epd", 4096, NULL, 6, NULL, 1);   // CPU1
    // xTaskCreatePinnedToCore(net_task, "net", 12288, NULL, 4, NULL, 0); // CPU0
}
```

`net_task` 里拿到一帧后：

```c
uint8_t *buf = heap_caps_malloc(GDEP133C02_IMAGE_SIZE, MALLOC_CAP_SPIRAM);
// ... 从 HTTP / SD 填满 buf ...
frame_msg_t msg = { .data = buf };
xQueueSend(s_frame_q, &msg, portMAX_DELAY);
```

### 5.4 工程配置要改的地方

| 项 | 现状 | 加 Wi-Fi / SD 后 |
|---|---|---|
| 分区表 | `factory` 4 MB + NVS 24 KB + PHY 4 KB | NVS 24 KB 一般够用，要存多组配置/日志可加到 64 KB；要 OTA 再加两个 2 MB app 分区 |
| NVS | 已存在，未用 | `nvs_flash_init()` 后 Wi-Fi 校准数据也会写这里 |
| PSRAM | 8 MB，已开 | 一帧 960 KB，双缓冲约 2 MB，余量充足 |
| `main/CMakeLists.txt` | 无 `REQUIRES`，默认链接全部组件 | 可显式写 `REQUIRES`（含 `esp_driver_spi/gpio`、`esp_wifi`、`nvs_flash`、`esp_netif`、`esp_event`、`fatfs`、`sdmmc`、`esp_driver_sdspi`、`esp_http_client` 等）缩短编译时间；漏写会编译失败 |
| 任务看门狗 | 5 s | 刷屏任务里有 `vTaskDelay`，不用改；若改成忙等，要 `esp_task_wdt_reset()` |
| 刷新间隔 | 单帧 | 多帧轮播时 `vTaskDelay(pdMS_TO_TICKS(interval_ms))`，与现有 `main.c` 一致 |

---

## 6. 参考入口（本地 IDF 内）

| 主题 | 路径 |
|---|---|
| Wi-Fi STA / SoftAP | `examples/wifi/getting_started/station`、`softAP` |
| Wi-Fi 配网 | `examples/provisioning/wifi_prov_mgr` |
| ESP-NOW | `examples/wifi/espnow` |
| SD 卡 SPI | `examples/storage/sd_card/sdspi` |
| SD 卡 SDMMC | `examples/storage/sd_card/sdmmc` |
| FATFS / SPIFFS / NVS | `examples/storage/fatfs`、`spiffs`、`nvs_rw_value` |
| HTTP 客户端 / 服务端 | `examples/protocols/esp_http_client`、`http_server/simple` |
| MQTT | `examples/protocols/mqtt/tcp` |
| OTA | `examples/system/ota` |
| FreeRTOS 任务 | `examples/system/freertos` |
| 任务看门狗 | `examples/system/task_watchdog` |
| 低功耗 | `examples/system/light_sleep`、`deep_sleep` |

---

## 7. 依据

- 板级手册 `docs/text/ESP32-133C02_board_manual.txt`（功能、SD/按键引脚、四种固件型号、第 20 页"只提供基础刷图程序"）
- 厂家示例 `vendor/gdep133c02/GDEP133C02/main/*`（只有 SPI/GPIO/EPD）
- 本工程 `firmware/e6_epd/sdkconfig`（双核、100 Hz、看门狗、PSRAM、WDT 等）
- ESP-IDF v5.4.4 本地源码：`components/*`、`examples/*`、`components/freertos/config/include/freertos/FreeRTOSConfig.h`
- 出厂固件字符串分析 `notes/board-check.md`（Arduino、SD 播放、`/images.bin`）
