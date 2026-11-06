#include "bridge.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "esp_vfs_fat.h"
#include "esp_crc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_card.h"

#define TAG "sd_bridge"

#define FRAME_MAGIC        "SDBG"
#define FRAME_MAGIC_LEN    4
#define FRAME_HEADER_LEN   16
#define FRAME_TYPE_REQ     1
#define FRAME_TYPE_RSP     2
#define FRAME_TYPE_DATA    3
#define FRAME_MAX_PAYLOAD  32768
#define DATA_CHUNK         16384

#define CMD_MAX            1024
#define PATH_MAX_LEN       320
#define NAME_ESC_MAX       800
#define IO_TIMEOUT_MS      5000
#define READ_SLICE_MS      500
#define PUT_IDLE_TIMEOUT_MS 15000

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ovf;
} strbuf_t;

typedef struct {
    bool active;
    FILE *fp;
    char final_path[PATH_MAX_LEN];
    char tmp_path[PATH_MAX_LEN];
    uint64_t expected;
    uint64_t received;
    uint32_t expected_crc;
    uint32_t crc;
    bool force;
    uint32_t last_ms;
} put_state_t;

static uint8_t s_payload[FRAME_MAX_PAYLOAD];
static char s_resp[FRAME_MAX_PAYLOAD];
static char s_lines[FRAME_MAX_PAYLOAD];
static char s_line[CMD_MAX + 1];

static uint8_t s_frame_type;
static uint16_t s_frame_seq;
static size_t s_frame_len;

static put_state_t s_put;

/* ---------------------------------------------------------------- helpers */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void sb_init(strbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->ovf = false;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void sb_addf(strbuf_t *sb, const char *fmt, ...)
{
    if (sb->ovf || sb->len + 1 >= sb->cap) {
        sb->ovf = true;
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->ovf = true;
        sb->buf[sb->len] = '\0';
        return;
    }
    sb->len += (size_t)n;
}

static bool sb_add_raw(strbuf_t *sb, const char *s, size_t n)
{
    if (sb->ovf || sb->len + n + 1 > sb->cap) {
        sb->ovf = true;
        return false;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return true;
}

static char unescape_char(char c)
{
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case '\\': return '\\';
    case '"': return '"';
    default: return c;
    }
}

/*
 * Split a command line into arguments.  Double quotes keep spaces together and
 * backslash escapes the next character.  This is done in place.
 */
static int tokenize(char *s, char **argv, int max)
{
    int argc = 0;
    char *p = s;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (argc >= max) {
            return -1;
        }

        char *out = p;
        argv[argc++] = out;
        bool quote = false;

        while (*p != '\0') {
            char c = *p;
            if (quote) {
                if (c == '\\' && p[1] != '\0') {
                    p++;
                    c = unescape_char(*p);
                } else if (c == '"') {
                    quote = false;
                    p++;
                    continue;
                }
                *out++ = c;
                p++;
            } else {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    break;
                }
                if (c == '"') {
                    quote = true;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1] != '\0') {
                    p++;
                    c = unescape_char(*p);
                }
                *out++ = c;
                p++;
            }
        }

        char sep = *p;
        char *end = p;
        *out = '\0';
        p = end;
        if (sep != '\0') {
            p++;
        }
    }

    return argc;
}

static void escape_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        const char *rep;
        char one[2] = { (char)*p, '\0' };
        switch (*p) {
        case '\\': rep = "\\\\"; break;
        case '\t': rep = "\\t"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        default: rep = one; break;
        }
        size_t n = strlen(rep);
        if (o + n + 1 > cap) {
            break;
        }
        memcpy(out + o, rep, n);
        o += n;
    }
    out[o] = '\0';
}

/*
 * Map a remote path ("/", "foo/bar", "/foo/bar") to an absolute VFS path.
 * "..", backslashes, drive letters and control characters are rejected.
 */
static esp_err_t resolve_path(const char *remote, char *out, size_t out_len)
{
    if (remote == NULL || out_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    char rel[256];
    size_t rel_len = 0;
    const char *s = remote;

    while (*s != '\0') {
        while (*s == '/') {
            s++;
        }
        if (*s == '\0') {
            break;
        }

        const char *e = s;
        while (*e != '\0' && *e != '/') {
            e++;
        }
        size_t n = (size_t)(e - s);

        if (n == 0 || n > 255) {
            return ESP_ERR_INVALID_ARG;
        }
        if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\\' || c == ':' || c < 0x20) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (rel_len + (rel_len ? 1 : 0) + n >= sizeof(rel)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (rel_len > 0) {
            rel[rel_len++] = '/';
        }
        memcpy(rel + rel_len, s, n);
        rel_len += n;
        s = e;
    }

    rel[rel_len] = '\0';

    int n;
    if (rel_len == 0) {
        n = snprintf(out, out_len, "%s", SD_CARD_MOUNT_POINT);
    } else {
        n = snprintf(out, out_len, "%s/%s", SD_CARD_MOUNT_POINT, rel);
    }
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool is_dot_name(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* --------------------------------------------------------------- transport */

static bool read_exact(uint8_t *dst, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    uint32_t last = now_ms();

    while (got < n) {
        if ((uint32_t)(now_ms() - last) > timeout_ms) {
            return false;
        }

        int r = usb_serial_jtag_read_bytes(dst + got, (uint32_t)(n - got),
                                           pdMS_TO_TICKS(100));
        if (r > 0) {
            got += (size_t)r;
            last = now_ms();
        } else {
            if (!usb_serial_jtag_is_connected()) {
                return false;
            }
            vTaskDelay(1);
        }
    }
    return true;
}

static bool find_magic(uint32_t timeout_ms)
{
    static const uint8_t magic[FRAME_MAGIC_LEN] = { 'S', 'D', 'B', 'G' };
    int matched = 0;
    uint32_t start = now_ms();

    while ((uint32_t)(now_ms() - start) < timeout_ms) {
        uint8_t c;
        int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (r == 1) {
            if (c == magic[matched]) {
                matched++;
                if (matched == FRAME_MAGIC_LEN) {
                    return true;
                }
            } else {
                matched = (c == magic[0]) ? 1 : 0;
            }
            start = now_ms();
        } else if (!usb_serial_jtag_is_connected()) {
            return false;
        }
    }
    return false;
}

static esp_err_t read_frame(uint32_t timeout_ms)
{
    if (!find_magic(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t hdr[FRAME_HEADER_LEN];
    memcpy(hdr, FRAME_MAGIC, FRAME_MAGIC_LEN);
    if (!read_exact(hdr + FRAME_MAGIC_LEN, FRAME_HEADER_LEN - FRAME_MAGIC_LEN,
                    IO_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t type = hdr[4];
    uint8_t flags = hdr[5];
    uint16_t seq = rd16(hdr + 6);
    uint32_t len = rd32(hdr + 8);
    uint32_t crc = rd32(hdr + 12);

    if (flags != 0 || len > FRAME_MAX_PAYLOAD ||
        (type != FRAME_TYPE_REQ && type != FRAME_TYPE_DATA)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (len > 0 && !read_exact(s_payload, len, IO_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t calc = esp_crc32_le(0, hdr + 4, 8);
    if (len > 0) {
        calc = esp_crc32_le(calc, s_payload, len);
    }
    if (calc != crc) {
        return ESP_ERR_INVALID_CRC;
    }

    s_frame_type = type;
    s_frame_seq = seq;
    s_frame_len = len;
    return ESP_OK;
}

static esp_err_t write_all(const void *src, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t last = now_ms();

    while (len > 0) {
        size_t chunk = len > 4096 ? 4096 : len;
        int w = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(1000));

        if (w > 0) {
            p += (size_t)w;
            len -= (size_t)w;
            last = now_ms();
        } else {
            if (!usb_serial_jtag_is_connected()) {
                return ESP_FAIL;
            }
            if ((uint32_t)(now_ms() - last) > timeout_ms) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(1);
        }
    }
    return ESP_OK;
}

static esp_err_t send_frame(uint8_t type, uint16_t seq, const void *payload, size_t len)
{
    if (len > FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t hdr[FRAME_HEADER_LEN];
    memcpy(hdr, FRAME_MAGIC, FRAME_MAGIC_LEN);
    hdr[4] = type;
    hdr[5] = 0;
    wr16(hdr + 6, seq);
    wr32(hdr + 8, (uint32_t)len);

    uint32_t crc = esp_crc32_le(0, hdr + 4, 8);
    if (len > 0) {
        crc = esp_crc32_le(crc, payload, len);
    }
    wr32(hdr + 12, crc);

    esp_err_t err = write_all(hdr, sizeof(hdr), IO_TIMEOUT_MS);
    if (err == ESP_OK && len > 0) {
        err = write_all(payload, len, IO_TIMEOUT_MS);
    }
    return err;
}

static void send_err(uint16_t seq, int code, const char *fmt, ...)
{
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (char *p = msg; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }

    char out[256];
    int n = snprintf(out, sizeof(out), "ERR %d %s", code, msg);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(out)) {
        n = (int)sizeof(out) - 1;
    }
    send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n);
}

static void send_ok(uint16_t seq)
{
    send_frame(FRAME_TYPE_RSP, seq, "OK", 2);
}

/* ------------------------------------------------------------------- PUT */

static void put_abort(void)
{
    if (s_put.fp != NULL) {
        fclose(s_put.fp);
        s_put.fp = NULL;
    }
    if (s_put.tmp_path[0] != '\0') {
        unlink(s_put.tmp_path);
    }
    memset(&s_put, 0, sizeof(s_put));
}

static void put_finalize(uint16_t seq)
{
    FILE *fp = s_put.fp;
    s_put.fp = NULL;
    int err = 0;

    if (fp != NULL) {
        if (fflush(fp) != 0) {
            err = errno ? errno : EIO;
        }
        if (fclose(fp) != 0 && err == 0) {
            err = errno ? errno : EIO;
        }
    }

    if (err != 0) {
        put_abort();
        send_err(seq, err, "flush/close failed");
        return;
    }

    if (s_put.received != s_put.expected) {
        put_abort();
        send_err(seq, EIO, "size mismatch");
        return;
    }

    if (s_put.crc != s_put.expected_crc) {
        put_abort();
        send_err(seq, EIO, "crc mismatch");
        return;
    }

    int r = rename(s_put.tmp_path, s_put.final_path);
    if (r != 0 && s_put.force && errno == EEXIST) {
        unlink(s_put.final_path);
        r = rename(s_put.tmp_path, s_put.final_path);
    }
    if (r != 0) {
        int e = errno ? errno : EIO;
        put_abort();
        send_err(seq, e, "rename failed");
        return;
    }

    uint64_t total = s_put.received;
    uint32_t crc = s_put.crc;
    memset(&s_put, 0, sizeof(s_put));

    char out[64];
    int n = snprintf(out, sizeof(out), "DONE %llu %u",
                     (unsigned long long)total, (unsigned)crc);
    send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n);
}

static void handle_data(uint16_t seq)
{
    if (!s_put.active || s_put.fp == NULL) {
        send_err(seq, EPROTO, "no PUT in progress");
        return;
    }

    if (s_put.received + s_frame_len > s_put.expected) {
        put_abort();
        send_err(seq, EINVAL, "too much data");
        return;
    }

    size_t w = fwrite(s_payload, 1, s_frame_len, s_put.fp);
    if (w != s_frame_len) {
        int e = errno ? errno : EIO;
        put_abort();
        send_err(seq, e, "write failed");
        return;
    }

    s_put.received += s_frame_len;
    if (s_frame_len > 0) {
        s_put.crc = esp_crc32_le(s_put.crc, s_payload, s_frame_len);
    }
    s_put.last_ms = now_ms();

    if (s_put.received == s_put.expected) {
        put_finalize(seq);
        return;
    }

    char out[48];
    int n = snprintf(out, sizeof(out), "ACK %llu", (unsigned long long)s_put.received);
    if (send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n) != ESP_OK) {
        put_abort();
    }
}

static void handle_put(uint16_t seq, int argc, char **argv)
{
    if (argc < 4) {
        send_err(seq, EINVAL, "usage: PUT path size crc [force]");
        return;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long size_arg = strtoull(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || size_arg > (unsigned long long)LONG_MAX) {
        send_err(seq, EINVAL, "bad size");
        return;
    }
    unsigned long crc_arg = strtoul(argv[3], &end, 0);
    if (end == argv[3] || *end != '\0') {
        send_err(seq, EINVAL, "bad crc");
        return;
    }
    bool force = (argc >= 5 && atoi(argv[4]) != 0);

    char full[PATH_MAX_LEN];
    esp_err_t r = resolve_path(argv[1], full, sizeof(full));
    if (r != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    struct stat st;
    if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            send_err(seq, EISDIR, "target is a directory");
            return;
        }
        if (!force) {
            send_err(seq, EEXIST, "target exists (use overwrite)");
            return;
        }
    }

    uint64_t fs_total = 0;
    uint64_t fs_free = 0;
    if (esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &fs_total, &fs_free) == ESP_OK) {
        if ((uint64_t)size_arg > fs_free) {
            send_err(seq, ENOSPC, "not enough free space");
            return;
        }
    }

    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s", full);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    if (dir[0] == '\0') {
        snprintf(dir, sizeof(dir), "%s", SD_CARD_MOUNT_POINT);
    }

    char tmp[PATH_MAX_LEN] = { 0 };
    bool tmp_ok = false;
    size_t dir_len = strlen(dir);
    for (int i = 0; i < 16; i++) {
        char suffix[24];
        int suffix_len = snprintf(suffix, sizeof(suffix), "/.sdbtmp-%08lx",
                                  (unsigned long)esp_random());
        if (suffix_len < 0 || (size_t)suffix_len >= sizeof(suffix) ||
            dir_len + (size_t)suffix_len >= sizeof(tmp)) {
            break;
        }
        memcpy(tmp, dir, dir_len);
        memcpy(tmp + dir_len, suffix, (size_t)suffix_len + 1);
        struct stat ts;
        if (stat(tmp, &ts) != 0) {
            tmp_ok = true;
            break;
        }
    }
    if (!tmp_ok) {
        send_err(seq, EEXIST, "could not allocate temp name");
        return;
    }

    FILE *fp = fopen(tmp, "wb");
    if (fp == NULL) {
        send_err(seq, errno ? errno : EIO, "open temp failed");
        return;
    }

    memset(&s_put, 0, sizeof(s_put));
    s_put.active = true;
    s_put.fp = fp;
    snprintf(s_put.final_path, sizeof(s_put.final_path), "%s", full);
    snprintf(s_put.tmp_path, sizeof(s_put.tmp_path), "%s", tmp);
    s_put.expected = (uint64_t)size_arg;
    s_put.expected_crc = (uint32_t)crc_arg;
    s_put.crc = 0;
    s_put.force = force;
    s_put.last_ms = now_ms();

    char out[64];
    int n = snprintf(out, sizeof(out), "OK\nchunk=%d\n", DATA_CHUNK);
    if (send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n) != ESP_OK) {
        put_abort();
        return;
    }

    if (s_put.expected == 0) {
        put_finalize(seq);
    }
}

/* ------------------------------------------------------------------- GET */

static void handle_get(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: GET path [offset]");
        return;
    }

    uint64_t offset = 0;
    if (argc >= 3) {
        char *end = NULL;
        errno = 0;
        offset = strtoull(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || offset > (uint64_t)LONG_MAX) {
            send_err(seq, EINVAL, "bad offset");
            return;
        }
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        send_err(seq, errno ? errno : ENOENT, "stat failed");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        send_err(seq, EISDIR, "not a regular file");
        return;
    }

    uint64_t size = (uint64_t)st.st_size;
    if (offset > size) {
        send_err(seq, EINVAL, "offset beyond EOF");
        return;
    }

    FILE *fp = fopen(full, "rb");
    if (fp == NULL) {
        send_err(seq, errno ? errno : EIO, "open failed");
        return;
    }
    if (offset > 0 && fseek(fp, (long)offset, SEEK_SET) != 0) {
        fclose(fp);
        send_err(seq, errno ? errno : EIO, "seek failed");
        return;
    }

    char out[128];
    int n = snprintf(out, sizeof(out), "OK\nsize=%llu\noffset=%llu\nchunk=%d\n",
                     (unsigned long long)size, (unsigned long long)offset, DATA_CHUNK);
    if (send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n) != ESP_OK) {
        fclose(fp);
        return;
    }

    uint64_t remaining = size - offset;
    uint64_t sent = 0;
    uint32_t crc = 0;

    while (sent < remaining) {
        size_t want = (remaining - sent) > DATA_CHUNK ? DATA_CHUNK
                                                      : (size_t)(remaining - sent);
        size_t got = fread(s_payload, 1, want, fp);
        if (got == 0) {
            break;
        }
        crc = esp_crc32_le(crc, s_payload, got);
        if (send_frame(FRAME_TYPE_DATA, seq, s_payload, got) != ESP_OK) {
            fclose(fp);
            return;
        }
        sent += got;
    }

    fclose(fp);

    if (sent != remaining) {
        send_err(seq, EIO, "short read");
        return;
    }

    n = snprintf(out, sizeof(out), "DONE %llu %u",
                 (unsigned long long)sent, (unsigned)crc);
    send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n);
}

/* -------------------------------------------------------------------- LS */

static void handle_ls(uint16_t seq, int argc, char **argv)
{
    const char *remote = (argc >= 2) ? argv[1] : "/";
    int start = (argc >= 3) ? atoi(argv[2]) : 0;
    int limit = (argc >= 4) ? atoi(argv[3]) : 128;

    if (start < 0) {
        start = 0;
    }
    if (limit <= 0) {
        limit = 128;
    }
    if (limit > 256) {
        limit = 256;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(remote, full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    DIR *dir = opendir(full);
    if (dir == NULL) {
        send_err(seq, errno ? errno : ENOENT, "opendir failed");
        return;
    }

    int total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!is_dot_name(de->d_name)) {
            total++;
        }
    }

    rewinddir(dir);

    int idx = 0;
    int returned = 0;
    strbuf_t lines;
    sb_init(&lines, s_lines, sizeof(s_lines));

    while ((de = readdir(dir)) != NULL) {
        if (is_dot_name(de->d_name)) {
            continue;
        }
        if (idx++ < start) {
            continue;
        }
        if (returned >= limit) {
            break;
        }

        char child[PATH_MAX_LEN];
        int cn = snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        if (cn < 0 || (size_t)cn >= sizeof(child)) {
            continue;
        }

        struct stat st;
        bool have_stat = (stat(child, &st) == 0);
        bool is_dir = have_stat ? S_ISDIR(st.st_mode) : (de->d_type == DT_DIR);
        unsigned long long size = (have_stat && !is_dir) ? (unsigned long long)st.st_size : 0;
        long long mtime = have_stat ? (long long)st.st_mtime : 0;

        char esc[NAME_ESC_MAX];
        escape_name(de->d_name, esc, sizeof(esc));

        char line[1200];
        int ln = snprintf(line, sizeof(line), "%c\t%s\t%llu\t%lld\n",
                          is_dir ? 'd' : 'f', esc, size, mtime);
        if (ln < 0 || (size_t)ln >= sizeof(line) || !sb_add_raw(&lines, line, (size_t)ln)) {
            break;
        }
        returned++;
    }

    closedir(dir);

    int head = snprintf(s_resp, sizeof(s_resp), "OK %d %d\n", total, returned);
    if (head < 0 || (size_t)head + lines.len > sizeof(s_resp)) {
        send_err(seq, ENOMEM, "listing too large");
        return;
    }
    memcpy(s_resp + head, s_lines, lines.len);
    send_frame(FRAME_TYPE_RSP, seq, s_resp, (size_t)head + lines.len);
}

/* ------------------------------------------------------------- other ops */

static void handle_stat(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: STAT path");
        return;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        send_err(seq, errno ? errno : ENOENT, "stat failed");
        return;
    }

    const char *type = S_ISDIR(st.st_mode) ? "dir" : (S_ISREG(st.st_mode) ? "file" : "other");
    char out[192];
    int n = snprintf(out, sizeof(out), "OK\ntype=%s\nsize=%llu\nmtime=%lld\n",
                     type, (unsigned long long)st.st_size, (long long)st.st_mtime);
    send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n);
}

static void handle_crc(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: CRC path");
        return;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        send_err(seq, errno ? errno : ENOENT, "stat failed");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        send_err(seq, EISDIR, "not a regular file");
        return;
    }

    FILE *fp = fopen(full, "rb");
    if (fp == NULL) {
        send_err(seq, errno ? errno : EIO, "open failed");
        return;
    }

    uint32_t crc = 0;
    uint64_t total = 0;
    size_t got;
    while ((got = fread(s_payload, 1, DATA_CHUNK, fp)) > 0) {
        crc = esp_crc32_le(crc, s_payload, got);
        total += got;
    }
    bool bad = ferror(fp) != 0;
    fclose(fp);

    if (bad) {
        send_err(seq, EIO, "read failed");
        return;
    }

    char out[128];
    int n = snprintf(out, sizeof(out), "OK\nsize=%llu\ncrc=%u\n",
                     (unsigned long long)total, (unsigned)crc);
    send_frame(FRAME_TYPE_RSP, seq, out, (size_t)n);
}

static void handle_mkdir(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: MKDIR path");
        return;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    if (mkdir(full, 0777) == 0) {
        static const char ok[] = "OK\ncreated=1\n";
        send_frame(FRAME_TYPE_RSP, seq, ok, sizeof(ok) - 1);
        return;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            static const char ok[] = "OK\nexists=1\n";
            send_frame(FRAME_TYPE_RSP, seq, ok, sizeof(ok) - 1);
            return;
        }
    }
    send_err(seq, errno ? errno : EIO, "mkdir failed");
}

static void handle_rm(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: RM path");
        return;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        send_err(seq, errno ? errno : ENOENT, "stat failed");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        send_err(seq, EISDIR, "is a directory (use RMDIR)");
        return;
    }
    if (unlink(full) != 0) {
        send_err(seq, errno ? errno : EIO, "unlink failed");
        return;
    }
    send_ok(seq);
}

static void handle_rmdir(uint16_t seq, int argc, char **argv)
{
    if (argc < 2) {
        send_err(seq, EINVAL, "usage: RMDIR path");
        return;
    }

    char full[PATH_MAX_LEN];
    if (resolve_path(argv[1], full, sizeof(full)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }
    if (strcmp(full, SD_CARD_MOUNT_POINT) == 0) {
        send_err(seq, EINVAL, "refusing to remove mount point");
        return;
    }
    if (rmdir(full) != 0) {
        send_err(seq, errno ? errno : EIO, "rmdir failed");
        return;
    }
    send_ok(seq);
}

static void handle_mv(uint16_t seq, int argc, char **argv)
{
    if (argc < 3) {
        send_err(seq, EINVAL, "usage: MV src dst [force]");
        return;
    }

    char src[PATH_MAX_LEN];
    char dst[PATH_MAX_LEN];
    if (resolve_path(argv[1], src, sizeof(src)) != ESP_OK ||
        resolve_path(argv[2], dst, sizeof(dst)) != ESP_OK) {
        send_err(seq, EINVAL, "bad path");
        return;
    }

    bool force = (argc >= 4 && atoi(argv[3]) != 0);
    if (force) {
        struct stat st;
        if (stat(dst, &st) == 0 && !S_ISDIR(st.st_mode)) {
            unlink(dst);
        }
    }

    if (rename(src, dst) != 0) {
        send_err(seq, errno ? errno : EIO, "rename failed");
        return;
    }
    send_ok(seq);
}

static void handle_sync(uint16_t seq)
{
    /* Every upload is fflush()+fclose()d and renamed before DONE is sent, so
     * there is no dirty FATFS data to flush here.  Keep the command as a
     * protocol-level no-op for host compatibility. */
    send_ok(seq);
}

static void handle_mount(uint16_t seq)
{
    esp_err_t err = sd_card_mount();
    if (err == ESP_OK) {
        static const char ok[] = "OK\nmounted=1\n";
        send_frame(FRAME_TYPE_RSP, seq, ok, sizeof(ok) - 1);
    } else {
        send_err(seq, err, "%s", sd_card_last_error());
    }
}

static void handle_unmount(uint16_t seq)
{
    esp_err_t err = sd_card_unmount();
    if (err == ESP_OK) {
        static const char ok[] = "OK\nmounted=0\n";
        send_frame(FRAME_TYPE_RSP, seq, ok, sizeof(ok) - 1);
    } else {
        send_err(seq, err, "%s", sd_card_last_error());
    }
}

static void handle_info(uint16_t seq)
{
    strbuf_t sb;
    sb_init(&sb, s_resp, sizeof(s_resp));
    sb_addf(&sb, "OK\n");
    sb_addf(&sb, "proto=1\n");
    sb_addf(&sb, "max_frame=%d\n", FRAME_MAX_PAYLOAD);
    sb_addf(&sb, "data_chunk=%d\n", DATA_CHUNK);
    sb_addf(&sb, "mount=%s\n", SD_CARD_MOUNT_POINT);

    sd_card_info_t ci;
    if (sd_card_get_info(&ci) == ESP_OK) {
        sb_addf(&sb, "mounted=1\n");
        sb_addf(&sb, "card_type=%s\n", ci.card_type);
        sb_addf(&sb, "card_name=%s\n", ci.card_name);
        sb_addf(&sb, "capacity_bytes=%llu\n", (unsigned long long)ci.capacity_bytes);
        sb_addf(&sb, "sector_size=%u\n", (unsigned)ci.sector_size);
        sb_addf(&sb, "max_freq_khz=%u\n", (unsigned)ci.max_freq_khz);
        sb_addf(&sb, "real_freq_khz=%d\n", ci.real_freq_khz);
        sb_addf(&sb, "fs_total_bytes=%llu\n", (unsigned long long)ci.fs_total_bytes);
        sb_addf(&sb, "fs_free_bytes=%llu\n", (unsigned long long)ci.fs_free_bytes);
    } else {
        sb_addf(&sb, "mounted=0\n");
        sb_addf(&sb, "error=%s\n", sd_card_last_error());
    }

    if (sb.ovf) {
        send_err(seq, ENOMEM, "info too large");
        return;
    }
    send_frame(FRAME_TYPE_RSP, seq, s_resp, sb.len);
}

/* --------------------------------------------------------------- dispatch */

static void handle_request(uint16_t seq)
{
    if (s_frame_len > CMD_MAX) {
        send_err(seq, EINVAL, "command too long");
        return;
    }

    memcpy(s_line, s_payload, s_frame_len);
    s_line[s_frame_len] = '\0';
    while (s_frame_len > 0 &&
           (s_line[s_frame_len - 1] == '\n' || s_line[s_frame_len - 1] == '\r')) {
        s_line[--s_frame_len] = '\0';
    }

    char *argv[8];
    int argc = tokenize(s_line, argv, 8);
    if (argc <= 0) {
        send_err(seq, EINVAL, "empty command");
        return;
    }

    const char *cmd = argv[0];

    if (s_put.active) {
        if (strcasecmp(cmd, "ABORT") == 0) {
            put_abort();
            send_ok(seq);
        } else {
            send_err(seq, EBUSY, "PUT in progress");
        }
        return;
    }

    if (strcasecmp(cmd, "PING") == 0) {
        static const char pong[] = "OK\npong=1\nproto=1\n";
        send_frame(FRAME_TYPE_RSP, seq, pong, sizeof(pong) - 1);
    } else if (strcasecmp(cmd, "INFO") == 0) {
        handle_info(seq);
    } else if (strcasecmp(cmd, "LS") == 0) {
        handle_ls(seq, argc, argv);
    } else if (strcasecmp(cmd, "STAT") == 0) {
        handle_stat(seq, argc, argv);
    } else if (strcasecmp(cmd, "GET") == 0) {
        handle_get(seq, argc, argv);
    } else if (strcasecmp(cmd, "PUT") == 0) {
        handle_put(seq, argc, argv);
    } else if (strcasecmp(cmd, "CRC") == 0) {
        handle_crc(seq, argc, argv);
    } else if (strcasecmp(cmd, "MKDIR") == 0) {
        handle_mkdir(seq, argc, argv);
    } else if (strcasecmp(cmd, "RM") == 0) {
        handle_rm(seq, argc, argv);
    } else if (strcasecmp(cmd, "RMDIR") == 0) {
        handle_rmdir(seq, argc, argv);
    } else if (strcasecmp(cmd, "MV") == 0) {
        handle_mv(seq, argc, argv);
    } else if (strcasecmp(cmd, "SYNC") == 0) {
        handle_sync(seq);
    } else if (strcasecmp(cmd, "MOUNT") == 0) {
        handle_mount(seq);
    } else if (strcasecmp(cmd, "UNMOUNT") == 0) {
        handle_unmount(seq);
    } else if (strcasecmp(cmd, "ABORT") == 0) {
        send_ok(seq);
    } else {
        send_err(seq, ENOSYS, "unknown command");
    }
}

void bridge_run(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 16384;
    cfg.rx_buffer_size = 16384;

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    ESP_LOGI(TAG, "USB bridge ready (proto 1)");

    while (true) {
        esp_err_t err = read_frame(READ_SLICE_MS);
        if (err == ESP_OK) {
            if (s_frame_type == FRAME_TYPE_REQ) {
                handle_request(s_frame_seq);
            } else if (s_frame_type == FRAME_TYPE_DATA) {
                handle_data(s_frame_seq);
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "frame error: %s", esp_err_to_name(err));
            if (s_put.active) {
                put_abort();
            }
            vTaskDelay(1);
        }

        if (s_put.active &&
            (uint32_t)(now_ms() - s_put.last_ms) > PUT_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "PUT idle timeout, aborting");
            put_abort();
        }
    }
}
