/*
 * 编译期常量集中在这里：网络端口、SD 路径、上传约束、启动行为。
 * 想改行为优先改这个文件，不要散落到各模块里。
 */
#pragma once

/* ---------------- 身份 ---------------- */

#define EPD_FW_NAME        "epd_server"
#define EPD_FW_VERSION     "1.0.0"
#define EPD_DEVICE_MODEL   "GDEP133C02"
#define EPD_HOSTNAME       "epd-133c02"

/* ---------------- 网络 ---------------- */

/* 任务 1：UDP 发现，与 host/discover.py、Swift 上位机共用同一协议 */
#define EPD_DISCOVERY_PORT 3232

/* 任务 2：HTTP 传图服务。发现应答里的 http_port 字段就填这个值 */
#define EPD_HTTP_PORT      80

/* 单次上传允许的最大字节数 = EPD6BIN 头 + 一帧 */
#define EPD_BIN_HEADER_SIZE 18
#define EPD_BIN_MAGIC       "EPD6BIN"
#define EPD_BIN_MAGIC_LEN   8

/* ---------------- SD 卡布局 ----------------
 *
 * /sdcard/epd/
 *   ├── current                   当前应显示的图（元数据文本，原子替换）
 *   ├── incoming.part             上传中的临时文件，成功后改名归档
 *   └── history/
 *        ├── 20261107-153012.bin  每次上传都留档，不覆盖
 *        └── ...
 */
#define EPD_SD_MOUNT_POINT "/sdcard"
#define EPD_STORE_DIR      "/sdcard/epd"
#define EPD_HISTORY_DIR    "/sdcard/epd/history"
#define EPD_CURRENT_FILE   "/sdcard/epd/current"
#define EPD_CURRENT_TMP    "/sdcard/epd/current.tmp"
#define EPD_UPLOAD_TMP     "/sdcard/epd/incoming.part"

/* ---------------- 行为开关 ---------------- */

/* 1 = 上电后自动重刷 SD 里最后一张图；0 = 上电不动屏，只等新图。
 * 电子纸断电保画面，所以 0 更省刷新次数；默认 1 是为了“上电一定有内容”。 */
#define EPD_BOOT_RESTORE   1

/* 连续上传时只刷最新一张，中间的先丢在队列里跳过（避免每张都等 12 s） */
#define EPD_DISPLAY_QUEUE_DEPTH 4

/* 面板 BUSY 超时后是否重试一次 */
#define EPD_DISPLAY_RETRY  1

/* ---------------- 资源 ---------------- */

#define EPD_HTTP_TASK_STACK     12288
#define EPD_HTTP_MAX_HANDLERS   10
#define EPD_HTTP_RECV_TIMEOUT_S 20
#define EPD_HTTP_SEND_TIMEOUT_S 20

#define EPD_DISPLAY_TASK_STACK  8192
#define EPD_DISPLAY_TASK_PRIO   6

/* 路径 / 文件名缓冲上限 */
#define EPD_NAME_MAX            64
#define EPD_PATH_MAX            128

/* WiFi 断线重连退避上限 */
#define EPD_WIFI_RETRY_MAX_MS   30000
