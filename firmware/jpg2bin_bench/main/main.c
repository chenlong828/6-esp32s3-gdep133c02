/*
 * On-device JPG -> EPD6BIN benchmark for GDEP133C02 (1200x1600, 6-color E6).
 *
 * Pipeline (mirrors host/e6_convert.py):
 *   /sdcard/genshin01.jpg (or the panel-sized genshin01_1200x1600.jpg)
 *     -> TJpgDec (ROM) RGB888 in PSRAM: 1/2 for >2.2 MP, 1/1 otherwise
 *     -> cover-crop bilinear resize to 1200x1600, row by row
 *     -> serpentine Floyd-Steinberg against the measured 6-color palette
 *     -> 4bpp pack (high nibble = left pixel), 960000 bytes
 *     -> /sdcard/jpg2bin_bench.bin (EPD6BIN container)
 *
 * Every stage is timed with esp_timer and printed over USB-Serial-JTAG.
 * SD SPI clock is swept (10/20/40 MHz) to show the read-side ceiling.
 */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"
#include "sd_card.h"

#define TAG "jpg2bin"

#define PANEL_W 1200
#define PANEL_H 1600

#define JPEG_PATH_FULL "/sdcard/genshin01.jpg"
#define JPEG_PATH_PANEL "/sdcard/genshin01_1200x1600.jpg"
#define BIN_PATH "/sdcard/jpg2bin_bench.bin"
#define BIN_INTERVAL_MS 150

static const char *s_jpeg_path = JPEG_PATH_FULL;

#define SD_CHUNK (32 * 1024)
#define TJPG_POOL (32 * 1024)

/* Real panel colors (host/e6_convert.py REAL_PALETTE order) */
static const int16_t PAL[6][3] = {
    {255, 255, 255}, /* 0 white  */
    {0, 0, 0},       /* 1 black  */
    {160, 32, 32},   /* 2 red    */
    {240, 224, 80},  /* 3 yellow */
    {80, 128, 184},  /* 4 blue   */
    {96, 128, 80},   /* 5 green  */
};

/* 4bpp pixel codes: black 0, white 1, yellow 2, red 3, blue 5, green 6 */
static const uint8_t CODE[6] = {0x1, 0x0, 0x3, 0x2, 0x5, 0x6};

typedef struct {
    uint16_t i0, i1, w; /* w = 0..256 fixed-point weight */
} lerp_t;

static lerp_t s_mapx[PANEL_W];
static lerp_t s_mapy[PANEL_H];
static int16_t s_row[PANEL_W * 3];
static uint8_t s_codes[PANEL_W];
static int16_t s_err0[PANEL_W * 3];
static int16_t s_err1[PANEL_W * 3];

typedef struct {
    FILE *fp;
    uint8_t *fb;
    int fb_w, fb_h, stride;
} dec_ctx_t;

/* Prefer the panel-sized JPEG when it is present; it decodes 1:1 and is much
 * cheaper than the 6.6 MP original. */
static const char *pick_jpeg_path(void)
{
    FILE *f = fopen(JPEG_PATH_PANEL, "rb");
    if (f != NULL) {
        fclose(f);
        return JPEG_PATH_PANEL;
    }
    return JPEG_PATH_FULL;
}

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline int lerp8(int a, int b, int w)
{
    return (a * (256 - w) + b * w + 128) >> 8;
}

/* ---------------------------------------------------------------- TJpgDec */

static UINT jpeg_in(JDEC *jd, BYTE *buff, UINT nbyte)
{
    dec_ctx_t *c = (dec_ctx_t *)jd->device;
    if (buff == NULL) {
        return fseek(c->fp, (long)nbyte, SEEK_CUR) == 0 ? nbyte : 0;
    }
    return (UINT)fread(buff, 1, nbyte, c->fp);
}

static int s_out_calls;
static int s_max_right;
static int s_max_bottom;

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    dec_ctx_t *c = (dec_ctx_t *)jd->device;
    s_out_calls++;
    if ((int)rect->right > s_max_right) {
        s_max_right = (int)rect->right;
    }
    if ((int)rect->bottom > s_max_bottom) {
        s_max_bottom = (int)rect->bottom;
    }
    if (rect->left >= c->fb_w || rect->top >= c->fb_h) {
        return 1; /* nothing to store */
    }

    int right = rect->right < c->fb_w ? rect->right : c->fb_w - 1;
    int bottom = rect->bottom < c->fb_h ? rect->bottom : c->fb_h - 1;
    int mc_w = rect->right - rect->left + 1; /* source MCU row width */
    int nbytes = (right - rect->left + 1) * 3;
    const uint8_t *src = (const uint8_t *)bitmap;

    for (int y = rect->top; y <= bottom; y++) {
        memcpy(c->fb + (size_t)y * c->stride + (size_t)rect->left * 3, src, (size_t)nbytes);
        src += (size_t)mc_w * 3;
    }
    return 1;
}

/* ---------------------------------------------------------------- resize + dither */

static void build_map(lerp_t *map, int n, float crop_start, float crop_len, int src_len)
{
    for (int i = 0; i < n; i++) {
        float s = crop_start + (i + 0.5f) * crop_len / (float)n - 0.5f;
        if (s < 0.0f) {
            s = 0.0f;
        }
        if (s > (float)(src_len - 1)) {
            s = (float)(src_len - 1);
        }
        int i0 = (int)s;
        float frac = s - (float)i0;
        int i1 = (i0 + 1 < src_len) ? i0 + 1 : i0;
        map[i].i0 = (uint16_t)i0;
        map[i].i1 = (uint16_t)i1;
        map[i].w = (uint16_t)(frac * 256.0f + 0.5f);
    }
}

/* 1200x1600 cover-crop bilinear resize + serpentine FS dither + 4bpp pack. */
static void convert_frame(const uint8_t *fb, int sw, int sh, int stride,
                          uint8_t *out, int64_t *us, uint64_t hist[6])
{
    float scale = (float)PANEL_W / (float)sw;
    float sy = (float)PANEL_H / (float)sh;
    if (sy > scale) {
        scale = sy;
    }
    float cw = (float)PANEL_W / scale;
    float ch = (float)PANEL_H / scale;
    float cx = ((float)sw - cw) * 0.5f;
    float cy = ((float)sh - ch) * 0.5f;
    if (cx < 0.0f) {
        cx = 0.0f;
    }
    if (cy < 0.0f) {
        cy = 0.0f;
    }
    build_map(s_mapx, PANEL_W, cx, cw, sw);
    build_map(s_mapy, PANEL_H, cy, ch, sh);

    memset(s_err0, 0, sizeof(s_err0));
    memset(s_err1, 0, sizeof(s_err1));
    memset(hist, 0, 6 * sizeof(uint64_t));

    int16_t *err_cur = s_err0;
    int16_t *err_next = s_err1;

    int64_t t0 = esp_timer_get_time();

    for (int y = 0; y < PANEL_H; y++) {
        const lerp_t my = s_mapy[y];
        const uint8_t *p0 = fb + (size_t)my.i0 * stride;
        const uint8_t *p1 = fb + (size_t)my.i1 * stride;

        /* bilinear resize of one output row into internal RAM */
        for (int x = 0; x < PANEL_W; x++) {
            const lerp_t mx = s_mapx[x];
            const uint8_t *a = p0 + (size_t)mx.i0 * 3;
            const uint8_t *b = p0 + (size_t)mx.i1 * 3;
            const uint8_t *c = p1 + (size_t)mx.i0 * 3;
            const uint8_t *d = p1 + (size_t)mx.i1 * 3;
            int w = mx.w;
            int o = x * 3;
            s_row[o + 0] = (int16_t)lerp8(lerp8(a[0], b[0], w), lerp8(c[0], d[0], w), my.w);
            s_row[o + 1] = (int16_t)lerp8(lerp8(a[1], b[1], w), lerp8(c[1], d[1], w), my.w);
            s_row[o + 2] = (int16_t)lerp8(lerp8(a[2], b[2], w), lerp8(c[2], d[2], w), my.w);
        }

        /* serpentine Floyd-Steinberg */
        int dir = (y & 1) ? -1 : 1;
        int start = dir > 0 ? 0 : PANEL_W - 1;
        for (int n = 0; n < PANEL_W; n++) {
            int x = start + dir * n;
            int i = x * 3;

            int v0 = clamp255(s_row[i + 0] + err_cur[i + 0]);
            int v1 = clamp255(s_row[i + 1] + err_cur[i + 1]);
            int v2 = clamp255(s_row[i + 2] + err_cur[i + 2]);

            int best;
            int32_t bd, d;
            d = (v0 - 255) * (v0 - 255) + (v1 - 255) * (v1 - 255) + (v2 - 255) * (v2 - 255);
            bd = d;
            best = 0; /* white */
            d = v0 * v0 + v1 * v1 + v2 * v2;
            if (d < bd) {
                bd = d;
                best = 1; /* black */
            }
            d = (v0 - 160) * (v0 - 160) + (v1 - 32) * (v1 - 32) + (v2 - 32) * (v2 - 32);
            if (d < bd) {
                bd = d;
                best = 2; /* red */
            }
            d = (v0 - 240) * (v0 - 240) + (v1 - 224) * (v1 - 224) + (v2 - 80) * (v2 - 80);
            if (d < bd) {
                bd = d;
                best = 3; /* yellow */
            }
            d = (v0 - 80) * (v0 - 80) + (v1 - 128) * (v1 - 128) + (v2 - 184) * (v2 - 184);
            if (d < bd) {
                bd = d;
                best = 4; /* blue */
            }
            d = (v0 - 96) * (v0 - 96) + (v1 - 128) * (v1 - 128) + (v2 - 80) * (v2 - 80);
            if (d < bd) {
                bd = d;
                best = 5; /* green */
            }

            s_codes[x] = CODE[best];
            hist[best]++;

            int e0 = v0 - PAL[best][0];
            int e1 = v1 - PAL[best][1];
            int e2 = v2 - PAL[best][2];

            int nx = x + dir;
            int mx = x - dir;
            if ((unsigned)nx < PANEL_W) {
                err_cur[nx * 3 + 0] = (int16_t)(err_cur[nx * 3 + 0] + ((e0 * 7) >> 4));
                err_cur[nx * 3 + 1] = (int16_t)(err_cur[nx * 3 + 1] + ((e1 * 7) >> 4));
                err_cur[nx * 3 + 2] = (int16_t)(err_cur[nx * 3 + 2] + ((e2 * 7) >> 4));
            }
            if ((unsigned)mx < PANEL_W) {
                err_next[mx * 3 + 0] = (int16_t)(err_next[mx * 3 + 0] + ((e0 * 3) >> 4));
                err_next[mx * 3 + 1] = (int16_t)(err_next[mx * 3 + 1] + ((e1 * 3) >> 4));
                err_next[mx * 3 + 2] = (int16_t)(err_next[mx * 3 + 2] + ((e2 * 3) >> 4));
            }
            err_next[x * 3 + 0] = (int16_t)(err_next[x * 3 + 0] + ((e0 * 5) >> 4));
            err_next[x * 3 + 1] = (int16_t)(err_next[x * 3 + 1] + ((e1 * 5) >> 4));
            err_next[x * 3 + 2] = (int16_t)(err_next[x * 3 + 2] + ((e2 * 5) >> 4));
            if ((unsigned)nx < PANEL_W) {
                err_next[nx * 3 + 0] = (int16_t)(err_next[nx * 3 + 0] + ((e0 * 1) >> 4));
                err_next[nx * 3 + 1] = (int16_t)(err_next[nx * 3 + 1] + ((e1 * 1) >> 4));
                err_next[nx * 3 + 2] = (int16_t)(err_next[nx * 3 + 2] + ((e2 * 1) >> 4));
            }
        }

        /* pack two palette codes per byte, high nibble = left pixel */
        uint8_t *orow = out + (size_t)y * (PANEL_W / 2);
        for (int x = 0; x < PANEL_W; x += 2) {
            orow[x >> 1] = (uint8_t)((s_codes[x] << 4) | s_codes[x + 1]);
        }

        int16_t *tmp = err_cur;
        err_cur = err_next;
        err_next = tmp;
        memset(err_next, 0, sizeof(s_err0));
    }

    *us = esp_timer_get_time() - t0;
}

/* ---------------------------------------------------------------- stages */

static bool sd_read_bench(const char *path, uint8_t *buf, size_t cap,
                          size_t *out_size, int64_t *out_us, uint32_t *out_crc)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > cap) {
        fclose(f);
        return false;
    }

    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (total < (size_t)sz) {
        size_t want = (size_t)sz - total;
        if (want > SD_CHUNK) {
            want = SD_CHUNK;
        }
        size_t n = fread(buf + total, 1, want, f);
        if (n == 0) {
            break;
        }
        total += n;
    }
    *out_us = esp_timer_get_time() - t0;
    *out_crc = esp_rom_crc32_le(0, buf, total);
    *out_size = total;
    fclose(f);
    return total == (size_t)sz;
}

static bool decode_jpeg(const char *path, uint8_t **fb_out, int *w_out, int *h_out,
                        int *stride_out, int64_t *us, uint32_t *crc)
{
    dec_ctx_t ctx = {0};
    ctx.fp = fopen(path, "rb");
    if (ctx.fp == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    setvbuf(ctx.fp, NULL, _IOFBF, 32 * 1024);

    void *pool = heap_caps_malloc(TJPG_POOL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pool == NULL) {
        ESP_LOGE(TAG, "no internal RAM for TJpgDec pool");
        fclose(ctx.fp);
        return false;
    }

    JDEC jd;
    memset(&jd, 0, sizeof(jd));
    int64_t t0 = esp_timer_get_time();
    JRESULT res = jd_prepare(&jd, jpeg_in, pool, TJPG_POOL, &ctx);
    int64_t t_prepare = esp_timer_get_time() - t0;
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", (int)res);
        fclose(ctx.fp);
        heap_caps_free(pool);
        return false;
    }

    /* Pick a decode scale that keeps RGB888 inside 8 MB PSRAM.  A panel-sized
     * JPEG (~1.9 MP) decodes 1:1; anything above ~2.2 MP is halved. */
    int scale_code = 0, scale_div = 1;
    if ((uint64_t)jd.width * (uint64_t)jd.height > 2200000ULL) {
        scale_code = 1;
        scale_div = 2;
    }
    ctx.fb_w = (int)jd.width / scale_div; /* TJpgDec clips to floor(w/scale) */
    ctx.fb_h = (int)jd.height / scale_div;
    ctx.stride = ctx.fb_w * 3;
    size_t fb_bytes = (size_t)ctx.stride * (size_t)ctx.fb_h;
    ctx.fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx.fb == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed: %u bytes (largest=%u)", (unsigned)fb_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        fclose(ctx.fp);
        heap_caps_free(pool);
        return false;
    }

    s_out_calls = 0;
    s_max_right = -1;
    s_max_bottom = -1;
    int64_t t1 = esp_timer_get_time();
    res = jd_decomp(&jd, jpeg_out, scale_code);
    int64_t t_decomp = esp_timer_get_time() - t1;
    *us = t_prepare + t_decomp; /* exclude the 4.9 MB PSRAM allocation */

    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jpg decode failed: %d", (int)res);
        heap_caps_free(ctx.fb);
        fclose(ctx.fp);
        heap_caps_free(pool);
        return false;
    }

    uint32_t c = 0;
    uint64_t sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < ctx.fb_h; y++) {
        const uint8_t *row = ctx.fb + (size_t)y * ctx.stride;
        c = esp_rom_crc32_le(c, row, (size_t)ctx.stride);
        for (int x = 0; x < ctx.fb_w; x++) {
            sr += row[x * 3 + 0];
            sg += row[x * 3 + 1];
            sb += row[x * 3 + 2];
        }
    }
    uint64_t npx = (uint64_t)ctx.fb_w * (uint64_t)ctx.fb_h;
    const uint8_t *p00 = ctx.fb;
    const uint8_t *pc = ctx.fb + (size_t)(ctx.fb_h / 2) * ctx.stride + (size_t)(ctx.fb_w / 2) * 3;
    printf("DECODE_META out_calls=%d max_rect_right=%d max_rect_bottom=%d fb=%dx%d\n",
           s_out_calls, s_max_right, s_max_bottom, ctx.fb_w, ctx.fb_h);
    printf("DECODE_AVG r=%.1f g=%.1f b=%.1f px00=%d,%d,%d center=%d,%d,%d\n",
           (double)sr / (double)npx, (double)sg / (double)npx, (double)sb / (double)npx,
           p00[0], p00[1], p00[2], pc[0], pc[1], pc[2]);

    ESP_LOGI(TAG, "JPEG %ux%u -> %dx%d (1/%d), prepare=%" PRId64 " us decomp=%" PRId64
             " us (alloc excluded from decode_us)",
             (unsigned)jd.width, (unsigned)jd.height, ctx.fb_w, ctx.fb_h, scale_div,
             t_prepare, t_decomp);
    ESP_LOGI(TAG, "decoded RGB888: %u bytes PSRAM, crc32=%08" PRIx32, (unsigned)fb_bytes, c);

    *fb_out = ctx.fb;
    *w_out = ctx.fb_w;
    *h_out = ctx.fb_h;
    *stride_out = ctx.stride;
    *crc = c;

    fclose(ctx.fp);
    heap_caps_free(pool);
    return true;
}

static bool write_bin(const char *path, const uint8_t *data, size_t len, int64_t *us)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    uint8_t hdr[18];
    memcpy(hdr, "EPD6BIN\0", 8);
    hdr[8] = PANEL_W & 0xFF;
    hdr[9] = (PANEL_W >> 8) & 0xFF;
    hdr[10] = PANEL_H & 0xFF;
    hdr[11] = (PANEL_H >> 8) & 0xFF;
    hdr[12] = 1; /* frames */
    hdr[13] = 0;
    hdr[14] = BIN_INTERVAL_MS & 0xFF;
    hdr[15] = (BIN_INTERVAL_MS >> 8) & 0xFF;
    hdr[16] = (BIN_INTERVAL_MS >> 16) & 0xFF;
    hdr[17] = (BIN_INTERVAL_MS >> 24) & 0xFF;

    int64_t t0 = esp_timer_get_time();
    size_t n = fwrite(hdr, 1, sizeof(hdr), f);
    n += fwrite(data, 1, len, f);
    fflush(f);
    fclose(f);
    *us = esp_timer_get_time() - t0;
    return n == len + sizeof(hdr);
}

/* ---------------------------------------------------------------- main */

void app_main(void)
{
    /* Long, CPU-bound stages: keep the idle-task watchdog out of the way. */
    esp_task_wdt_deinit();

    /* Give the USB-Serial-JTAG host time to attach after reset. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("\nBENCH_BEGIN jpg2bin on ESP32-S3 @ %d MHz, PSRAM free=%u\n",
           (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- SD clock sweep: read the whole JPEG at 10/20/40 MHz ---- */
    uint8_t *read_buf = NULL;
    size_t read_cap = 0;
    uint32_t ref_crc = 0;
    bool have_ref = false;

    static const int freqs[] = {10000, 20000, 40000};
    for (int i = 0; i < 3; i++) {
        int freq = freqs[i];
        if (sd_card_mount(freq) != ESP_OK) {
            printf("SD_SWEEP freq=%d mount=FAIL (%s)\n", freq, sd_card_last_error());
            continue;
        }
        sd_card_info_t info;
        sd_card_get_info(&info);
        s_jpeg_path = pick_jpeg_path();

        FILE *f = fopen(s_jpeg_path, "rb");
        if (f == NULL) {
            printf("SD_SWEEP freq=%d open=FAIL %s\n", freq, s_jpeg_path);
            sd_card_unmount();
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        fclose(f);

        if (read_buf == NULL && sz > 0) {
            read_cap = (size_t)sz;
            read_buf = heap_caps_malloc(read_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (read_buf == NULL || sz <= 0 || (size_t)sz > read_cap) {
            printf("SD_SWEEP freq=%d buffer=FAIL size=%ld\n", freq, sz);
            sd_card_unmount();
            continue;
        }

        size_t got = 0;
        uint32_t crc = 0;
        int64_t us = 0;
        bool ok = sd_read_bench(s_jpeg_path, read_buf, read_cap, &got, &us, &crc);
        double kb_s = us > 0 ? (double)got * 1000000.0 / (double)us / 1024.0 : 0.0;
        if (i == 0 && ok) {
            ref_crc = crc;
            have_ref = true;
        }
        bool crc_ok = have_ref && crc == ref_crc;
        printf("SD_SWEEP freq=%d real=%d size=%u us=%" PRId64 " speed=%.1f KiB/s crc=%08" PRIx32
               " %s\n",
               freq, info.real_freq_khz, (unsigned)got, us, kb_s, crc,
               ok ? (crc_ok ? "OK" : "CRC_MISMATCH") : "READ_FAIL");
        sd_card_unmount();
    }

    if (read_buf != NULL) {
        heap_caps_free(read_buf);
        read_buf = NULL;
    }

    /* ---- main run at 20 MHz (known-good on this board), 10 MHz fallback ---- */
    int main_freq = 20000;
    if (sd_card_mount(main_freq) != ESP_OK) {
        printf("WARN mount 20MHz failed (%s), retry 10MHz\n", sd_card_last_error());
        main_freq = 10000;
        if (sd_card_mount(main_freq) != ESP_OK) {
            printf("FATAL mount failed: %s\n", sd_card_last_error());
            printf("BENCH_DONE\n");
            return;
        }
    }
    s_jpeg_path = pick_jpeg_path();
    printf("MAIN_RUN sd_freq=%d jpeg=%s\n", main_freq, s_jpeg_path);

    uint8_t *fb = NULL;
    int fw = 0, fh = 0, stride = 0;
    int64_t decode_us = 0;
    uint32_t rgb_crc = 0;
    if (!decode_jpeg(s_jpeg_path, &fb, &fw, &fh, &stride, &decode_us, &rgb_crc)) {
        printf("FATAL decode failed\nBENCH_DONE\n");
        sd_card_unmount();
        return;
    }

    uint8_t *packed = heap_caps_malloc((size_t)PANEL_W * PANEL_H / 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packed == NULL) {
        printf("FATAL PSRAM alloc for 960000-byte output failed\nBENCH_DONE\n");
        sd_card_unmount();
        return;
    }

    int64_t convert_us = 0;
    uint64_t hist[6] = {0};
    convert_frame(fb, fw, fh, stride, packed, &convert_us, hist);

    /* Decoded frame is no longer needed. */
    heap_caps_free(fb);
    fb = NULL;

    uint32_t bin_crc = esp_rom_crc32_le(0, packed, (size_t)PANEL_W * PANEL_H / 2);
    uint64_t px = (uint64_t)PANEL_W * PANEL_H;

    int64_t write_us = 0;
    bool wrote = write_bin(BIN_PATH, packed, (size_t)PANEL_W * PANEL_H / 2, &write_us);

    printf("BENCH decode_us=%" PRId64 " convert_us=%" PRId64 " write_us=%" PRId64
           " total_convert_us=%" PRId64 " total_with_write_us=%" PRId64 "\n",
           decode_us, convert_us, write_us, decode_us + convert_us,
           decode_us + convert_us + write_us);
    printf("BENCH decode_MPix_s=%.2f convert_ns_per_px=%.1f decode_ms=%.1f convert_ms=%.1f\n",
           6586569.0 / (double)decode_us,
           (double)convert_us * 1000.0 / (double)px,
           (double)decode_us / 1000.0, (double)convert_us / 1000.0);
    printf("BENCH hist white=%.4f black=%.4f red=%.4f yellow=%.4f blue=%.4f green=%.4f\n",
           (double)hist[0] / (double)px, (double)hist[1] / (double)px,
           (double)hist[2] / (double)px, (double)hist[3] / (double)px,
           (double)hist[4] / (double)px, (double)hist[5] / (double)px);
    printf("BENCH bin_crc=%08" PRIx32 " wrote=%s write_path=%s\n", bin_crc,
           wrote ? "yes" : "no", BIN_PATH);
    printf("BENCH_DONE\n");
    fflush(stdout);

    sd_card_unmount();
}
