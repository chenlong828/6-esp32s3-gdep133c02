/*
 * 任务 2：HTTP 传图服务
 *
 * esp_http_server 自带任务；本文件里的 POST /api/v1/image 处理函数就是“任务 2”：
 *   收包 → 边收边算 CRC → 写 /sdcard/epd/incoming.part → 校验 EPD6BIN 头
 *   → 归档到 history/ 并原子更新 current → 投递刷屏任务（任务 3）→ 回 JSON。
 *
 * 上传体就是 host/e6_convert.py 产出的 .bin 原样（EPD6BIN，单帧 960 018 B），
 * 固件不做任何缩放 / 抖动 —— 与厂家链路一致。
 *
 * 接口一览
 *   GET  /                      纯文本状态页（浏览器直接看）
 *   GET  /api/v1/status         完整 JSON 状态
 *   POST /api/v1/image          上传一帧（body=EPD6BIN）
 *                                头：X-Image-Name（建议带本地时间，用于归档命名）
 *                                    X-Timestamp （Unix 秒，写进元数据）
 *                                    X-Crc32     （整文件 CRC32，十六进制）
 *                                查询：?name=xxx.bin 可覆盖文件名
 *   GET  /api/v1/history        归档列表（文件名倒序）
 *   GET  /api/v1/image?file=    下载归档文件
 *   POST /api/v1/display?file=  重新显示某张归档
 *   POST /api/v1/clear?color=   整屏单色（white/black/red/yellow/blue/green）
 *
 * 约定：先收完整 body 再回包；SD 未挂载时上传一律 503（不落盘就不算成功）。
 */

#include "upload_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "GDEP133C02.h"
#include "crc32.h"
#include "display_task.h"
#include "epd_config.h"
#include "epd_state.h"
#include "image_store.h"
#include "sd_card.h"
#include "wifi_sta.h"

static const char *TAG = "http";

#define UPLOAD_RX_CHUNK   8192
#define UPLOAD_PROGRESS_STEP (64 * 1024)
#define UPLOAD_EXPECT_BYTES (EPD_BIN_HEADER_SIZE + GDEP133C02_IMAGE_SIZE)

static httpd_handle_t s_server;
static SemaphoreHandle_t s_upload_lock;

/* ---------------- 通用响应 ---------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json print failed");
        return ESP_FAIL;
    }

    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, strlen(text));
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *code, const char *msg)
{
    ESP_LOGW(TAG, "%s：%s（%s）", code, msg, status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", msg);
    return send_json(req, root, status);
}

/* 读整个文件算 CRC32（重新显示时用来更新 current 元数据 + 让刷屏任务校验） */
static bool file_crc32(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    uint8_t buf[2048];
    uint32_t crc = EPD_CRC32_INIT;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = epd_crc32_update(crc, buf, n);
    }
    const bool ok = (ferror(f) == 0);
    fclose(f);

    if (ok && out != NULL) {
        *out = epd_crc32_final(crc);
    }
    return ok;
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, out, out_len) != ESP_OK) {
        return false;
    }
    return true;
}

/* ---------------- GET /api/v1/status ---------------- */

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    epd_state_fill_json(root);
    return send_json(req, root, "200 OK");
}

/* ---------------- GET / ---------------- */

static esp_err_t h_root(httpd_req_t *req)
{
    epd_state_t st;
    epd_state_snapshot(&st);

    char ip[16] = "0.0.0.0";
    wifi_sta_ip_string(ip, sizeof(ip));

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char body[1024];
    snprintf(body, sizeof(body),
             "<!doctype html><meta charset=utf-8>"
             "<title>%s</title>"
             "<h1>%s 电子纸</h1>"
             "<p>固件 %s ｜ IP %s ｜ MAC %s</p>"
             "<p>面板 1200&times;1600 ｜ SD %s ｜ 当前图 %s</p>"
             "<p>累计刷新 %u 次 ｜ 归档 %u 张</p>"
             "<p><a href=\"/api/v1/status\">/api/v1/status</a> ｜ "
             "<a href=\"/api/v1/history\">/api/v1/history</a></p>"
             "<p>传图：POST /api/v1/image（body = EPD6BIN 单帧 960018 字节）</p>",
             EPD_HOSTNAME, EPD_DEVICE_MODEL, EPD_FW_VERSION, ip, mac_str,
             st.sd_mounted ? "已挂载" : "未挂载",
             st.current_name[0] ? st.current_name : "（无）",
             (unsigned)st.refresh_count, (unsigned)image_store_count());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* ---------------- POST /api/v1/image ---------------- */

typedef struct {
    bool ok;
    char code[40];
    char message[96];
} upload_result_t;

static void upload_fail(upload_result_t *res, const char *code, const char *msg)
{
    res->ok = false;
    strlcpy(res->code, code, sizeof(res->code));
    strlcpy(res->message, msg, sizeof(res->message));
}

static esp_err_t h_upload(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        return send_error(req, "503 Service Unavailable", "sd_unavailable",
                          "SD 卡未挂载，图片无法落盘");
    }

    if (xSemaphoreTake(s_upload_lock, 0) != pdTRUE) {
        return send_error(req, "409 Conflict", "upload_busy",
                          "已有一次上传在进行，请稍后重试");
    }

    const size_t total = (size_t)req->content_len;
    if (total != UPLOAD_EXPECT_BYTES) {
        xSemaphoreGive(s_upload_lock);
        char msg[96];
        snprintf(msg, sizeof(msg), "长度应为 %u 字节（EPD6BIN 单帧），收到 %u",
                 (unsigned)UPLOAD_EXPECT_BYTES, (unsigned)total);
        return send_error(req, "400 Bad Request", "bad_length", msg);
    }

    /* 可选的请求头 */
    char name_hdr[EPD_NAME_MAX] = "";
    char ts_hdr[24] = "";
    char crc_hdr[16] = "";
    httpd_req_get_hdr_value_str(req, "X-Image-Name", name_hdr, sizeof(name_hdr));
    httpd_req_get_hdr_value_str(req, "X-Timestamp", ts_hdr, sizeof(ts_hdr));
    httpd_req_get_hdr_value_str(req, "X-Crc32", crc_hdr, sizeof(crc_hdr));

    char query_name[EPD_NAME_MAX] = "";
    if (query_value(req, "name", query_name, sizeof(query_name)) &&
        image_store_name_valid(query_name)) {
        strlcpy(name_hdr, query_name, sizeof(name_hdr));
    }

    uint32_t saved_at = 0;
    if (ts_hdr[0] != '\0') {
        const unsigned long long v = strtoull(ts_hdr, NULL, 10);
        if (v > 1600000000ULL) {
            saved_at = (uint32_t)v;
        }
    }

    uint8_t *buf = heap_caps_malloc(UPLOAD_RX_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(UPLOAD_RX_CHUNK);
    }
    FILE *f = NULL;

    upload_result_t res;
    memset(&res, 0, sizeof(res));
    res.ok = false;

    uint32_t crc = EPD_CRC32_INIT;
    size_t got = 0;
    size_t next_progress = UPLOAD_PROGRESS_STEP;
    int64_t t0 = esp_timer_get_time();
    int timeouts = 0;

    if (buf == NULL) {
        upload_fail(&res, "no_memory", "接收缓冲分配失败");
        goto done;
    }

    f = fopen(EPD_UPLOAD_TMP, "wb");
    if (f == NULL) {
        upload_fail(&res, "sd_write_failed", "无法创建临时文件（卡写保护 / 空间不足？）");
        goto done;
    }

    epd_state_upload_begin((uint32_t)total);
    ESP_LOGI(TAG, "开始接收图片：%u 字节", (unsigned)total);

    while (got < total) {
        size_t want = total - got;
        if (want > UPLOAD_RX_CHUNK) {
            want = UPLOAD_RX_CHUNK;
        }

        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                upload_fail(&res, "recv_timeout", "接收超时");
                goto done;
            }
            continue;
        }
        if (r <= 0) {
            upload_fail(&res, "recv_failed", "连接中断");
            goto done;
        }
        timeouts = 0;

        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            upload_fail(&res, "sd_write_failed", "写卡失败");
            goto done;
        }
        crc = epd_crc32_update(crc, buf, (size_t)r);
        got += (size_t)r;

        if (got >= next_progress) {
            epd_state_upload_progress((uint32_t)got);
            next_progress += UPLOAD_PROGRESS_STEP;
        }
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        upload_fail(&res, "sd_write_failed", "落盘失败");
        goto done;
    }
    fclose(f);
    f = NULL;

    crc = epd_crc32_final(crc);

    /* 校验 EPD6BIN 头 */
    {
        FILE *rf = fopen(EPD_UPLOAD_TMP, "rb");
        if (rf == NULL) {
            upload_fail(&res, "sd_read_failed", "临时文件读不回来");
            goto done;
        }
        uint8_t head[EPD_BIN_HEADER_SIZE];
        const size_t head_len = fread(head, 1, sizeof(head), rf);
        fclose(rf);
        if (head_len != sizeof(head)) {
            upload_fail(&res, "bad_length", "文件过短");
            goto done;
        }

        epd_bin_info_t info;
        if (image_store_parse(head, total, &info) != ESP_OK) {
            upload_fail(&res, "bad_format", "不是合法的单帧 EPD6BIN（检查尺寸/帧数）");
            goto done;
        }
    }

    if (crc_hdr[0] != '\0') {
        const unsigned long want_crc = strtoul(crc_hdr, NULL, 16);
        if (want_crc != 0 && (uint32_t)want_crc != crc) {
            upload_fail(&res, "crc_mismatch", "CRC 校验失败，传输可能丢字节");
            goto done;
        }
    }

    char final_name[EPD_NAME_MAX] = "";
    if (image_store_commit(EPD_UPLOAD_TMP, name_hdr, crc, saved_at, (uint32_t)total,
                           final_name, sizeof(final_name)) != ESP_OK) {
        upload_fail(&res, "commit_failed", "归档失败");
        goto done;
    }

    res.ok = true;

done:
    if (f != NULL) {
        fclose(f);
    }
    if (!res.ok) {
        remove(EPD_UPLOAD_TMP);
    }

    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    epd_state_upload_end(res.ok, res.ok ? (uint32_t)total : 0, ms);

    if (buf != NULL) {
        free(buf);
    }

    if (!res.ok) {
        ESP_LOGE(TAG, "上传失败：%s（%s）", res.code, res.message);
        xSemaphoreGive(s_upload_lock);

        /* 区分“客户端数据不对”和“设备侧暂时不行”，方便上位机决定是否重试 */
        const char *status = "400 Bad Request";
        if (strcmp(res.code, "sd_write_failed") == 0 ||
            strcmp(res.code, "sd_read_failed") == 0 ||
            strcmp(res.code, "commit_failed") == 0 ||
            strcmp(res.code, "no_memory") == 0) {
            status = "503 Service Unavailable";
        } else if (strcmp(res.code, "recv_timeout") == 0) {
            status = "408 Request Timeout";
        }
        return send_error(req, status, res.code, res.message);
    }

    /* 通知任务 3；队列满也算上传成功，只是这一张不自动上屏 */
    display_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = DISPLAY_JOB_FILE;
    strlcpy(job.name, final_name, sizeof(job.name));
    image_store_abs_path(final_name, job.path, sizeof(job.path));
    job.crc32 = crc;
    job.saved_at = saved_at;
    job.bytes = (uint32_t)total;

    const esp_err_t qerr = display_task_submit(&job, pdMS_TO_TICKS(1000));
    const bool queued = (qerr == ESP_OK);
    if (!queued) {
        ESP_LOGW(TAG, "刷屏队列满，%s 未自动上屏（可调用 /api/v1/display 重刷）",
                 final_name);
    }

    ESP_LOGI(TAG, "上传完成：%s，%u 字节，%u ms，crc32=%08x%s",
             final_name, (unsigned)total, (unsigned)ms, (unsigned)crc,
             queued ? "，已排队刷屏" : "");

    xSemaphoreGive(s_upload_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "file", final_name);
    cJSON_AddNumberToObject(root, "bytes", (double)total);
    cJSON_AddNumberToObject(root, "crc32", (double)crc);
    cJSON_AddNumberToObject(root, "saved_at", (double)saved_at);
    cJSON_AddNumberToObject(root, "received_ms", ms);
    cJSON_AddBoolToObject(root, "queued", queued);
    return send_json(req, root, "200 OK");
}

/* ---------------- GET /api/v1/history ---------------- */

static esp_err_t h_history(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);

    cJSON *files = cJSON_AddArrayToObject(root, "files");
    esp_err_t err = image_store_list(files);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "list_failed", "读目录失败");
    }

    epd_state_t st;
    epd_state_snapshot(&st);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(files));
    cJSON_AddStringToObject(root, "current", st.current_name);
    return send_json(req, root, "200 OK");
}

/* ---------------- GET /api/v1/image?file= ----------------
 * 注意：同一个 URI 上 GET 是下载、POST 是上传，靠 HTTP 方法区分。
 */

static esp_err_t h_image_get(httpd_req_t *req)
{
    char file[EPD_NAME_MAX] = "";
    if (!query_value(req, "file", file, sizeof(file)) || !image_store_name_valid(file)) {
        return send_error(req, "400 Bad Request", "bad_file", "缺少或非法的 file 参数");
    }

    char path[EPD_PATH_MAX];
    image_store_abs_path(file, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return send_error(req, "404 Not Found", "not_found", "文件不存在");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", file);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    uint8_t buf[2048];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (err != ESP_OK) {
            break;
        }
    }
    fclose(f);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* ---------------- POST /api/v1/display?file= ---------------- */

static esp_err_t h_display(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        return send_error(req, "503 Service Unavailable", "sd_unavailable", "SD 卡未挂载");
    }

    char file[EPD_NAME_MAX] = "";
    if (!query_value(req, "file", file, sizeof(file)) || !image_store_name_valid(file)) {
        return send_error(req, "400 Bad Request", "bad_file", "缺少或非法的 file 参数");
    }

    char path[EPD_PATH_MAX];
    image_store_abs_path(file, path, sizeof(path));

    epd_bin_info_t info;
    if (image_store_validate(path, &info) != ESP_OK) {
        return send_error(req, "404 Not Found", "not_found", "文件不存在或不合法");
    }

    uint32_t crc = 0;
    if (!file_crc32(path, &crc)) {
        return send_error(req, "503 Service Unavailable", "sd_read_failed", "读文件失败");
    }

    display_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = DISPLAY_JOB_FILE;
    strlcpy(job.name, file, sizeof(job.name));
    strlcpy(job.path, path, sizeof(job.path));
    job.crc32 = crc;
    job.bytes = info.total_bytes;

    if (display_task_submit(&job, pdMS_TO_TICKS(1000)) != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "queue_full", "刷屏队列已满");
    }

    /* 重新显示也更新 current：重启后保持一致 */
    if (image_store_set_current(file, crc, 0, info.total_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "更新 current 失败（不影响本次刷屏）");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "file", file);
    cJSON_AddNumberToObject(root, "crc32", (double)crc);
    cJSON_AddBoolToObject(root, "queued", true);
    return send_json(req, root, "202 Accepted");
}

/* ---------------- POST /api/v1/clear?color= ---------------- */

static esp_err_t h_clear(httpd_req_t *req)
{
    static const struct {
        const char *name;
        uint8_t code;
    } colors[] = {
        { "white",  GDEP133C02_WHITE  },
        { "black",  GDEP133C02_BLACK  },
        { "red",    GDEP133C02_RED    },
        { "yellow", GDEP133C02_YELLOW },
        { "blue",   GDEP133C02_BLUE   },
        { "green",  GDEP133C02_GREEN  },
    };

    char color[16] = "white";
    query_value(req, "color", color, sizeof(color));

    uint8_t code = 0;
    bool found = false;
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        if (strcasecmp(color, colors[i].name) == 0) {
            code = colors[i].code;
            found = true;
            break;
        }
    }
    if (!found) {
        return send_error(req, "400 Bad Request", "bad_color", "颜色取值不合法");
    }

    display_job_t job;
    memset(&job, 0, sizeof(job));
    job.kind = DISPLAY_JOB_SOLID;
    job.solid_color = code;
    snprintf(job.name, sizeof(job.name), "solid-%s", color);

    if (display_task_submit(&job, pdMS_TO_TICKS(1000)) != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "queue_full", "刷屏队列已满");
    }

    /* 清屏后不再有“当前图”，否则重启会把上一张又刷回来 */
    remove(EPD_CURRENT_FILE);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "color", color);
    cJSON_AddBoolToObject(root, "queued", true);
    return send_json(req, root, "202 Accepted");
}

/* ---------------- 启动 ---------------- */

static const httpd_uri_t s_uris[] = {
    { .uri = "/",                 .method = HTTP_GET,  .handler = h_root,       },
    { .uri = "/api/v1/status",    .method = HTTP_GET,  .handler = h_status,     },
    { .uri = "/api/v1/image",     .method = HTTP_POST, .handler = h_upload,     },
    { .uri = "/api/v1/image",     .method = HTTP_GET,  .handler = h_image_get,  },
    { .uri = "/api/v1/history",   .method = HTTP_GET,  .handler = h_history,    },
    { .uri = "/api/v1/display",   .method = HTTP_POST, .handler = h_display,    },
    { .uri = "/api/v1/clear",     .method = HTTP_POST, .handler = h_clear,      },
};

esp_err_t upload_server_start(void)
{
    s_upload_lock = xSemaphoreCreateMutex();
    if (s_upload_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = EPD_HTTP_PORT;
    cfg.max_uri_handlers = EPD_HTTP_MAX_HANDLERS;
    cfg.stack_size = EPD_HTTP_TASK_STACK;
    cfg.recv_wait_timeout = EPD_HTTP_RECV_TIMEOUT_S;
    cfg.send_wait_timeout = EPD_HTTP_SEND_TIMEOUT_S;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败：%s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", s_uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "HTTP 服务已启动：端口 %d（%u 个接口）",
             EPD_HTTP_PORT, (unsigned)(sizeof(s_uris) / sizeof(s_uris[0])));
    return ESP_OK;
}
