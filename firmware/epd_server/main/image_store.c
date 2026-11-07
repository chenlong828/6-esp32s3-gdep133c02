#include "image_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "GDEP133C02.h"

static const char *TAG = "store";

#define IMAGE_FRAME_BYTES   GDEP133C02_IMAGE_SIZE /* 960 000 */
#define IMAGE_TOTAL_BYTES   (EPD_BIN_HEADER_SIZE + IMAGE_FRAME_BYTES)
#define HISTORY_LIST_MAX    256

/* ---------------- 小工具 ---------------- */

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ensure_dir(const char *path)
{
    if (file_exists(path)) {
        return true;
    }
    if (mkdir(path, 0777) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "建目录失败 %s：errno=%d", path, errno);
    return false;
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool image_store_name_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t len = strlen(name);
    if (len < 5 || len >= EPD_NAME_MAX) { /* 至少 "a.bin" */
        return false;
    }
    if (name[0] == '.') {
        return false;
    }
    if (strcmp(name + len - 4, ".bin") != 0) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        switch (c) {
        case '/': case '\\': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            return false;
        default:
            break;
        }
    }
    return true;
}

esp_err_t image_store_abs_path(const char *name, char *out, size_t out_len)
{
    if (!image_store_name_valid(name) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_len, "%s/%s", EPD_HISTORY_DIR, name);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ---------------- 初始化 ---------------- */

esp_err_t image_store_init(void)
{
    if (!ensure_dir(EPD_STORE_DIR)) {
        return ESP_FAIL;
    }
    if (!ensure_dir(EPD_HISTORY_DIR)) {
        return ESP_FAIL;
    }

    /* 上次断电可能留下半截临时文件，启动时清掉（绝不动 history/ 里的正式文件） */
    if (file_exists(EPD_UPLOAD_TMP)) {
        remove(EPD_UPLOAD_TMP);
        ESP_LOGW(TAG, "清理上次残留的 %s", EPD_UPLOAD_TMP);
    }
    if (file_exists(EPD_CURRENT_TMP)) {
        remove(EPD_CURRENT_TMP);
    }

    const uint32_t count = image_store_count();
    ESP_LOGI(TAG, "图片仓库就绪：%s（已归档 %u 张）", EPD_HISTORY_DIR, (unsigned)count);
    return ESP_OK;
}

/* ---------------- 校验 ---------------- */

esp_err_t image_store_parse(const uint8_t *data, size_t len, epd_bin_info_t *info)
{
    if (data == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < EPD_BIN_HEADER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(data, EPD_BIN_MAGIC, EPD_BIN_MAGIC_LEN) != 0) {
        ESP_LOGW(TAG, "缺少 EPD6BIN 魔数");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t w = rd_u16(data + 8);
    const uint16_t h = rd_u16(data + 10);
    const uint16_t frames = rd_u16(data + 12);
    const uint32_t interval = rd_u32(data + 14);

    if (w != GDEP133C02_WIDTH || h != GDEP133C02_HEIGHT) {
        ESP_LOGW(TAG, "尺寸不符：%ux%u，面板 %ux%u",
                 w, h, GDEP133C02_WIDTH, GDEP133C02_HEIGHT);
        return ESP_ERR_INVALID_ARG;
    }
    if (frames != 1) {
        ESP_LOGW(TAG, "只接受单帧，收到 %u 帧", frames);
        return ESP_ERR_INVALID_ARG;
    }
    if (len != (size_t)IMAGE_TOTAL_BYTES) {
        ESP_LOGW(TAG, "长度不符：%u，应为 %u",
                 (unsigned)len, (unsigned)IMAGE_TOTAL_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(info, 0, sizeof(*info));
    info->width = w;
    info->height = h;
    info->frames = frames;
    info->interval_ms = interval;
    info->total_bytes = (uint32_t)len;
    return ESP_OK;
}

esp_err_t image_store_validate(const char *path, epd_bin_info_t *info)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((uint32_t)st.st_size != (uint32_t)IMAGE_TOTAL_BYTES) {
        ESP_LOGW(TAG, "文件长度 %u，应为 %u",
                 (unsigned)st.st_size, (unsigned)IMAGE_TOTAL_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t head[EPD_BIN_HEADER_SIZE];
    size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);

    if (got != sizeof(head)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return image_store_parse(head, (size_t)st.st_size, info);
}

/* ---------------- current 元数据 ---------------- */

static esp_err_t write_current(const char *name, uint32_t crc,
                               uint32_t saved_at, uint32_t bytes)
{
    FILE *f = fopen(EPD_CURRENT_TMP, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法写 %s：errno=%d", EPD_CURRENT_TMP, errno);
        return ESP_FAIL;
    }

    fprintf(f, "name=%s\ncrc32=%08x\nsaved_at=%u\nbytes=%u\n",
            name, (unsigned)crc, (unsigned)saved_at, (unsigned)bytes);

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        remove(EPD_CURRENT_TMP);
        return ESP_FAIL;
    }
    fclose(f);

    /*
     * FATFS 的 f_rename 在目标已存在时返回 EEXIST（POSIX 的 rename 是覆盖语义），
     * 所以必须先删旧文件再改名。两者之间有几毫秒窗口：断电只会导致“没有 current”
     * （重启不自动刷图），history/ 里的图片本身不受影响。
     */
    remove(EPD_CURRENT_FILE);
    if (rename(EPD_CURRENT_TMP, EPD_CURRENT_FILE) != 0) {
        ESP_LOGE(TAG, "current 改名失败：errno=%d", errno);
        remove(EPD_CURRENT_TMP);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t image_store_set_current(const char *name, uint32_t crc,
                                  uint32_t saved_at, uint32_t bytes)
{
    if (!image_store_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[EPD_PATH_MAX];
    esp_err_t err = image_store_abs_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (!file_exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }

    return write_current(name, crc, saved_at, bytes);
}

esp_err_t image_store_read_current(char *name, size_t name_len,
                                   uint32_t *crc, uint32_t *saved_at,
                                   uint32_t *bytes)
{
    if (name == NULL || name_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(EPD_CURRENT_FILE, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    char found_name[EPD_NAME_MAX] = "";
    unsigned found_crc = 0, found_saved = 0, found_bytes = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "name=%63s", found_name) == 1) {
            continue;
        }
        if (sscanf(line, "crc32=%x", &found_crc) == 1) {
            continue;
        }
        if (sscanf(line, "saved_at=%u", &found_saved) == 1) {
            continue;
        }
        if (sscanf(line, "bytes=%u", &found_bytes) == 1) {
            continue;
        }
    }
    fclose(f);

    if (!image_store_name_valid(found_name)) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(name, found_name, name_len);
    if (crc) *crc = found_crc;
    if (saved_at) *saved_at = found_saved;
    if (bytes) *bytes = found_bytes;
    return ESP_OK;
}

/* ---------------- 归档 ---------------- */

static void unique_name(const char *base, char *out, size_t out_len)
{
    char candidate[EPD_NAME_MAX];
    char path[128];

    for (int i = 0; i < 1000; i++) {
        if (i == 0) {
            snprintf(candidate, sizeof(candidate), "%s", base);
        } else {
            const size_t stem = strlen(base) - 4; /* 去掉 ".bin" */
            snprintf(candidate, sizeof(candidate), "%.*s-%d.bin", (int)stem, base, i);
        }

        snprintf(path, sizeof(path), "%s/%s", EPD_HISTORY_DIR, candidate);
        if (!file_exists(path)) {
            strlcpy(out, candidate, out_len);
            return;
        }
    }

    /* 极端情况：用 uptime 兜底 */
    snprintf(out, out_len, "img-%u.bin", (unsigned)(esp_timer_get_time() / 1000000));
}

esp_err_t image_store_commit(const char *tmp_path, const char *requested_name,
                             uint32_t crc, uint32_t saved_at, uint32_t total_bytes,
                             char *out_name, size_t out_len)
{
    if (tmp_path == NULL || out_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char base[EPD_NAME_MAX];
    if (image_store_name_valid(requested_name)) {
        strlcpy(base, requested_name, sizeof(base));
    } else {
        snprintf(base, sizeof(base), "img-%u.bin",
                 (unsigned)(esp_timer_get_time() / 1000000));
    }

    char final_name[EPD_NAME_MAX];
    unique_name(base, final_name, sizeof(final_name));

    char final_path[128];
    snprintf(final_path, sizeof(final_path), "%s/%s", EPD_HISTORY_DIR, final_name);

    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "归档改名失败 %s → %s：errno=%d", tmp_path, final_path, errno);
        return ESP_FAIL;
    }

    esp_err_t err = write_current(final_name, crc, saved_at, total_bytes);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(out_name, final_name, out_len);
    ESP_LOGI(TAG, "已归档 %s（%u 字节，crc32=%08x）",
             final_name, (unsigned)total_bytes, (unsigned)crc);
    return ESP_OK;
}

/* ---------------- 列表 ---------------- */

typedef struct {
    char name[EPD_NAME_MAX];
    uint32_t size;
} history_entry_t;

static int cmp_desc(const void *a, const void *b)
{
    const history_entry_t *ea = (const history_entry_t *)a;
    const history_entry_t *eb = (const history_entry_t *)b;
    return strcmp(eb->name, ea->name);
}

static int collect_entries(history_entry_t *entries, int max)
{
    DIR *dir = opendir(EPD_HISTORY_DIR);
    if (dir == NULL) {
        return 0;
    }

    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < max) {
        const size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".bin") != 0) {
            continue;
        }
        if (!image_store_name_valid(de->d_name)) {
            continue;
        }

        /* 名字最长 FATFS 255 B，这里留足空间，避免 -Wformat-truncation */
        char path[EPD_PATH_MAX + 256];
        snprintf(path, sizeof(path), "%s/%s", EPD_HISTORY_DIR, de->d_name);

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        strlcpy(entries[n].name, de->d_name, sizeof(entries[n].name));
        entries[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(dir);

    qsort(entries, n, sizeof(entries[0]), cmp_desc);
    return n;
}

esp_err_t image_store_list(cJSON *out_array)
{
    if (out_array == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    history_entry_t *entries = malloc(sizeof(history_entry_t) * HISTORY_LIST_MAX);
    if (entries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int n = collect_entries(entries, HISTORY_LIST_MAX);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "bytes", entries[i].size);
        cJSON_AddItemToArray(out_array, item);
    }
    free(entries);

    return ESP_OK;
}

uint32_t image_store_count(void)
{
    history_entry_t *entries = malloc(sizeof(history_entry_t) * HISTORY_LIST_MAX);
    if (entries == NULL) {
        return 0;
    }
    const int n = collect_entries(entries, HISTORY_LIST_MAX);
    free(entries);
    return (uint32_t)n;
}
