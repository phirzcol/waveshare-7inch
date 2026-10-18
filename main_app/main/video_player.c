// =============================================================================
// ESP32-P4 Tablet - MJPEG Video Player
// =============================================================================
// First-pass video path for the 7-inch board:
// - raw MJPEG stream files from /sdcard/video
// - hardware JPEG decode
// - PPA scale/letterbox into 1024x600 RGB565 DMA buffers
// - LVGL lv_image presentation so LVGL owns panel flushing
// =============================================================================

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "video_player.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "audio_player.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "VIDEO";

#define VIDEO_DIR          "/sdcard/video"
#define MAX_VIDEOS         96
#define PATH_MAX_LEN       256
#define SCREEN_W           1024
#define SCREEN_H           600
#define SCREEN_ALLOC_H     ALIGN16(SCREEN_H)
#define SCREEN_STRIDE      (SCREEN_W * 2)
#define SCREEN_SIZE        (SCREEN_W * SCREEN_H * 2)
#define SCREEN_ALLOC_SIZE  (SCREEN_W * SCREEN_ALLOC_H * 2)
#define VIDEO_FPS_DEFAULT  30
#define FRAME_BUF_INITIAL  (256 * 1024)
#define FRAME_BUF_MAX      (1024 * 1024)
#define READ_BUF_SIZE      (128 * 1024)
#define FRAME_SLOT_COUNT   4
#define ALIGN16(x)         (((x) + 15) & ~15)

extern void show_desktop(void);

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_progress = NULL;
static lv_obj_t *s_play_label = NULL;
static lv_obj_t *s_perf_labels[5] = {NULL};

static char (*s_video_paths)[PATH_MAX_LEN] = NULL;
static int s_video_count = 0;
static int s_current_idx = -1;

static TaskHandle_t s_task = NULL;
static TaskHandle_t s_reader_task = NULL;
static volatile bool s_running = false;
static volatile bool s_paused = false;
static char s_pending_path[PATH_MAX_LEN] = {0};
static char s_audio_path[PATH_MAX_LEN] = {0};
static bool s_audio_active = false;

static uint8_t *s_screen_buf[2] = {NULL, NULL};
static size_t s_screen_alloc[2] = {0, 0};
static lv_image_dsc_t s_screen_dsc[2] = {0};
static int s_present_idx = 0;
static ppa_client_handle_t s_ppa = NULL;

static uint8_t *s_decode_buf = NULL;
static size_t s_decode_alloc = 0;
static uint32_t s_decode_stride_px = 0;
static uint32_t s_decode_w = 0;
static uint32_t s_decode_h = 0;

static uint8_t *s_read_buf = NULL;
static QueueHandle_t s_free_q = NULL;
static QueueHandle_t s_filled_q = NULL;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    long pos;
    int64_t read_us;
    bool eof;
} frame_slot_t;

static frame_slot_t s_slots[FRAME_SLOT_COUNT] = {0};

typedef enum {
    PERF_GREEN = 0,
    PERF_YELLOW,
    PERF_RED,
} perf_state_t;

typedef struct {
    FILE *f;
    size_t pos;
    size_t len;
    uint8_t prev;
    bool in_frame;
} mjpeg_reader_t;

typedef struct {
    uint32_t frames;
    uint32_t dropped;
    uint32_t drop_decode;
    uint32_t drop_present;
    size_t bytes;
    int64_t read_us;
    int64_t decode_us;
    int64_t render_us;
    int64_t lvgl_us;
    int64_t busy_us;
    int64_t frame_min_us;
    int64_t frame_max_us;
    int64_t last_frame_us;
    int64_t window_start_us;
    int64_t last_frame_start_us;
} video_perf_t;

static lv_color_t perf_color(perf_state_t state)
{
    switch (state) {
    case PERF_GREEN: return lv_color_hex(0x33aaff);
    case PERF_YELLOW: return lv_color_hex(0xffd23f);
    default: return lv_color_hex(0xff4b4b);
    }
}

static const char *perf_tag(perf_state_t state)
{
    switch (state) {
    case PERF_GREEN: return "OK";
    case PERF_YELLOW: return "MID";
    default: return "HOT";
    }
}

static perf_state_t perf_from_ratio(float value, float green_limit, float yellow_limit)
{
    if (value <= green_limit) return PERF_GREEN;
    if (value <= yellow_limit) return PERF_YELLOW;
    return PERF_RED;
}

static perf_state_t perf_from_fps(float fps)
{
    if (fps >= (VIDEO_FPS_DEFAULT * 0.93f)) return PERF_GREEN;
    if (fps >= (VIDEO_FPS_DEFAULT * 0.80f)) return PERF_YELLOW;
    return PERF_RED;
}

static bool is_video_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".mjpg") == 0 || strcasecmp(dot, ".mjpeg") == 0;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static uint32_t fps_from_path(const char *path)
{
    if (!path) return VIDEO_FPS_DEFAULT;
    if (strstr(path, "15fps") || strstr(path, "15FPS")) return 15;
    if (strstr(path, "24fps") || strstr(path, "24FPS")) return 24;
    if (strstr(path, "30fps") || strstr(path, "30FPS")) return 30;
    return VIDEO_FPS_DEFAULT;
}

static void video_audio_cb(audio_player_cb_ctx_t *ctx)
{
    (void)ctx;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 0;
}

static bool find_sidecar_audio(const char *video_path, char *audio_path, size_t audio_path_size)
{
    if (!video_path || !audio_path || audio_path_size == 0) return false;

    char base[PATH_MAX_LEN];
    snprintf(base, sizeof(base), "%s", video_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    const char *exts[] = { ".mp3", ".MP3", ".wav", ".WAV" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        snprintf(audio_path, audio_path_size, "%s%s", base, exts[i]);
        if (file_exists(audio_path)) return true;
    }

    audio_path[0] = '\0';
    return false;
}

static void video_audio_start(const char *video_path)
{
    s_audio_path[0] = '\0';
    s_audio_active = false;

    if (!find_sidecar_audio(video_path, s_audio_path, sizeof(s_audio_path))) {
        ESP_LOGI(TAG, "No sidecar audio for %s", path_basename(video_path));
        return;
    }

    if (bsp_extra_player_init() != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed for sidecar: %s", s_audio_path);
        s_audio_path[0] = '\0';
        return;
    }

    audio_player_callback_register(video_audio_cb, NULL);
    if (bsp_extra_player_play_file(s_audio_path) == ESP_OK) {
        s_audio_active = true;
        ESP_LOGI(TAG, "Video sidecar audio: %s", s_audio_path);
    } else {
        ESP_LOGW(TAG, "Could not play sidecar audio: %s", s_audio_path);
        s_audio_path[0] = '\0';
    }
}

static void video_audio_stop(void)
{
    if (s_audio_active) {
        audio_player_stop();
    }
    s_audio_active = false;
    s_audio_path[0] = '\0';
}

static void video_audio_pause(bool pause)
{
    if (!s_audio_active) return;
    if (pause) {
        audio_player_pause();
    } else {
        audio_player_resume();
    }
}

static bool ensure_frame_slots(void)
{
    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        if (!s_slots[i].buf) {
            s_slots[i].buf = heap_caps_malloc(FRAME_BUF_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_slots[i].buf) {
                ESP_LOGE(TAG, "Alloc frame slot %d failed", i);
                return false;
            }
            s_slots[i].cap = FRAME_BUF_MAX;
        }
    }
    return true;
}

static bool ensure_read_buffers(void)
{
    if (!s_read_buf) {
        s_read_buf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_read_buf) {
            s_read_buf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_read_buf) {
            ESP_LOGE(TAG, "Alloc read buffer failed");
            return false;
        }
    }
    return true;
}

static bool mjpeg_reader_fill(mjpeg_reader_t *reader)
{
    reader->len = fread(s_read_buf, 1, READ_BUF_SIZE, reader->f);
    reader->pos = 0;
    return reader->len > 0;
}

static bool mjpeg_read_frame(mjpeg_reader_t *reader, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ensure_read_buffers()) return false;

    size_t len = 0;
    reader->in_frame = false;
    reader->prev = 0;

    while (s_running) {
        if (reader->pos >= reader->len && !mjpeg_reader_fill(reader)) return false;

        if (!reader->in_frame) {
            if (reader->prev == 0xFF && reader->pos < reader->len && s_read_buf[reader->pos] == 0xD8) {
                if (out_cap < 2) return false;
                out[0] = 0xFF;
                out[1] = 0xD8;
                len = 2;
                reader->pos++;
                reader->in_frame = true;
                reader->prev = 0xD8;
            }

            while (reader->pos < reader->len) {
                uint8_t *ff = memchr(&s_read_buf[reader->pos], 0xFF, reader->len - reader->pos);
                if (!ff) {
                    reader->prev = s_read_buf[reader->len - 1];
                    reader->pos = reader->len;
                    break;
                }

                size_t idx = (size_t)(ff - s_read_buf);
                if (idx + 1 < reader->len) {
                    if (s_read_buf[idx + 1] == 0xD8) {
                        if (out_cap < 2) return false;
                        out[0] = 0xFF;
                        out[1] = 0xD8;
                        len = 2;
                        reader->pos = idx + 2;
                        reader->in_frame = true;
                        reader->prev = 0xD8;
                        break;
                    }
                    reader->pos = idx + 1;
                    reader->prev = 0xFF;
                } else {
                    reader->prev = 0xFF;
                    reader->pos = reader->len;
                    break;
                }
            }

            if (!reader->in_frame && reader->prev == 0xFF) {
                if (reader->pos >= reader->len && !mjpeg_reader_fill(reader)) return false;
                if (reader->pos < reader->len && s_read_buf[reader->pos] == 0xD8) {
                    if (out_cap < 2) return false;
                    out[0] = 0xFF;
                    out[1] = 0xD8;
                    len = 2;
                    reader->pos++;
                    reader->in_frame = true;
                    reader->prev = 0xD8;
                }
            }
        }

        while (reader->in_frame && reader->pos < reader->len) {
            if (reader->prev == 0xFF) {
                uint8_t marker = s_read_buf[reader->pos++];
                if (len + 1 > out_cap) {
                    ESP_LOGE(TAG, "MJPEG frame exceeds %u bytes", (unsigned)FRAME_BUF_MAX);
                    return false;
                }
                out[len++] = marker;
                reader->prev = marker;
                if (marker == 0xD9) {
                    reader->in_frame = false;
                    reader->prev = 0;
                    *out_len = len;
                    return true;
                }
                continue;
            }

            uint8_t *ff = memchr(&s_read_buf[reader->pos], 0xFF, reader->len - reader->pos);
            size_t end = ff ? (size_t)(ff - s_read_buf) : reader->len;
            size_t copy_len = end - reader->pos;
            if (copy_len > 0) {
                if (len + copy_len > out_cap) {
                    ESP_LOGE(TAG, "MJPEG frame exceeds %u bytes", (unsigned)FRAME_BUF_MAX);
                    return false;
                }
                memcpy(&out[len], &s_read_buf[reader->pos], copy_len);
                len += copy_len;
                reader->prev = out[len - 1];
                reader->pos = end;
            }

            if (reader->pos >= reader->len) break;

            if (reader->pos + 1 < reader->len) {
                uint8_t marker = s_read_buf[reader->pos + 1];
                if (len + 2 > out_cap) {
                    ESP_LOGE(TAG, "MJPEG frame exceeds %u bytes", (unsigned)FRAME_BUF_MAX);
                    return false;
                }
                out[len++] = 0xFF;
                out[len++] = marker;
                reader->pos += 2;
                reader->prev = marker;

                if (marker == 0xD9) {
                    reader->in_frame = false;
                    reader->prev = 0;
                    *out_len = len;
                    return true;
                }
            } else {
                if (len + 1 > out_cap) {
                    ESP_LOGE(TAG, "MJPEG frame exceeds %u bytes", (unsigned)FRAME_BUF_MAX);
                    return false;
                }
                out[len++] = 0xFF;
                reader->pos++;
                reader->prev = 0xFF;
            }
        }
    }

    return false;
}

static bool ensure_screen_surfaces(void)
{
    jpeg_decode_memory_alloc_cfg_t cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    for (int i = 0; i < 2; i++) {
        if (!s_screen_buf[i]) {
            s_screen_buf[i] = (uint8_t *)jpeg_alloc_decoder_mem(SCREEN_ALLOC_SIZE, &cfg, &s_screen_alloc[i]);
            if (!s_screen_buf[i]) {
                ESP_LOGE(TAG, "Alloc screen surface %d failed", i);
                return false;
            }
            memset(&s_screen_dsc[i], 0, sizeof(s_screen_dsc[i]));
            s_screen_dsc[i].header.w = SCREEN_W;
            s_screen_dsc[i].header.h = SCREEN_H;
            s_screen_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
            s_screen_dsc[i].header.stride = SCREEN_STRIDE;
            s_screen_dsc[i].data_size = SCREEN_SIZE;
            s_screen_dsc[i].data = s_screen_buf[i];
        }
    }

    if (!s_ppa) {
        ppa_client_config_t cfg_ppa = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&cfg_ppa, &s_ppa) != ESP_OK) {
            ESP_LOGW(TAG, "PPA unavailable; CPU scale fallback will be used");
            s_ppa = NULL;
        }
    }

    return true;
}

static void scale_cpu(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                      uint32_t src_stride_px, uint8_t *dst, uint32_t out_w,
                      uint32_t out_h, uint32_t off_x, uint32_t off_y)
{
    uint16_t *dst16 = (uint16_t *)dst;
    const uint16_t *src16 = (const uint16_t *)src;

    for (uint32_t y = 0; y < out_h; y++) {
        uint32_t sy = (uint64_t)y * src_h / out_h;
        uint16_t *dst_row = dst16 + (off_y + y) * SCREEN_W + off_x;
        const uint16_t *src_row = src16 + sy * src_stride_px;
        for (uint32_t x = 0; x < out_w; x++) {
            uint32_t sx = (uint64_t)x * src_w / out_w;
            dst_row[x] = src_row[sx];
        }
    }
}

static bool render_frame_to_surface(uint8_t *src, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride_px, int surface_idx)
{
    if (!ensure_screen_surfaces()) return false;

    uint8_t *dst = s_screen_buf[surface_idx];
    size_t dst_alloc = s_screen_alloc[surface_idx];
    memset(dst, 0, dst_alloc);

    float scale = (float)SCREEN_W / (float)src_w;
    float scale_y = (float)SCREEN_H / (float)src_h;
    if (scale_y < scale) scale = scale_y;

    uint32_t out_w = ((uint32_t)((float)src_w * scale) / 16U) * 16U;
    uint32_t out_h = ((uint32_t)((float)src_h * scale) / 16U) * 16U;
    if (out_w < 16) out_w = 16;
    if (out_h < 16) out_h = 16;
    if (out_w > SCREEN_W) out_w = SCREEN_W;
    if (out_h > SCREEN_H) out_h = SCREEN_H;

    uint32_t off_x = (SCREEN_W - out_w) / 2;
    uint32_t off_y = (SCREEN_H - out_h) / 2;
    bool ppa_done = false;

    if (src_w == SCREEN_W && src_h == SCREEN_H && src_stride_px >= SCREEN_W) {
        const size_t row_bytes = SCREEN_W * 2;
        uint8_t *dst_row = dst;
        const uint8_t *src_row = src;
        for (uint32_t y = 0; y < SCREEN_H; y++) {
            memcpy(dst_row, src_row, row_bytes);
            dst_row += row_bytes;
            src_row += src_stride_px * 2;
        }
        esp_cache_msync(dst, dst_alloc, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        return true;
    }

    if (s_ppa) {
        esp_cache_msync(src, s_decode_alloc, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(dst, dst_alloc, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        ppa_srm_oper_config_t cfg = {0};
        cfg.in.buffer = src;
        cfg.in.pic_w = src_stride_px;
        cfg.in.pic_h = ALIGN16(src_h);
        cfg.in.block_w = src_w;
        cfg.in.block_h = src_h;
        cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.out.buffer = dst;
        cfg.out.buffer_size = dst_alloc;
        cfg.out.pic_w = SCREEN_W;
        cfg.out.pic_h = SCREEN_H;
        cfg.out.block_offset_x = off_x;
        cfg.out.block_offset_y = off_y;
        cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.scale_x = (float)out_w / (float)src_w;
        cfg.scale_y = (float)out_h / (float)src_h;
        cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        cfg.mode = PPA_TRANS_MODE_BLOCKING;

        esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa, &cfg);
        if (ret == ESP_OK) {
            esp_cache_msync(dst, dst_alloc, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            ppa_done = true;
        } else {
            ESP_LOGW(TAG, "PPA frame scale failed: %s", esp_err_to_name(ret));
        }
    }

    if (!ppa_done) {
        scale_cpu(src, src_w, src_h, src_stride_px, dst, out_w, out_h, off_x, off_y);
    }

    return true;
}

static bool decode_frame(jpeg_decoder_handle_t engine, const uint8_t *jpeg, size_t jpeg_len)
{
    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(jpeg, jpeg_len, &info) != ESP_OK) {
        return false;
    }

    uint32_t aligned_w = ALIGN16(info.width);
    uint32_t aligned_h = ALIGN16(info.height);
    size_t needed = aligned_w * aligned_h * 2;

    if (needed > s_decode_alloc) {
        if (s_decode_buf) {
            heap_caps_free(s_decode_buf);
            s_decode_buf = NULL;
            s_decode_alloc = 0;
        }
        jpeg_decode_memory_alloc_cfg_t cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        s_decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(needed, &cfg, &s_decode_alloc);
        if (!s_decode_buf) {
            ESP_LOGE(TAG, "Alloc decode buffer failed (%u bytes)", (unsigned)needed);
            return false;
        }
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t decoded_size = s_decode_alloc;
    esp_err_t ret = jpeg_decoder_process(engine, &cfg,
                                         jpeg, jpeg_len,
                                         s_decode_buf, s_decode_alloc,
                                         &decoded_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG frame decode failed: %s", esp_err_to_name(ret));
        return false;
    }

    s_decode_w = info.width;
    s_decode_h = info.height;
    s_decode_stride_px = aligned_w;
    return true;
}

static bool decode_frame_to_surface(jpeg_decoder_handle_t engine, const uint8_t *jpeg,
                                    size_t jpeg_len, int surface_idx)
{
    if (!ensure_screen_surfaces()) return false;

    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(jpeg, jpeg_len, &info) != ESP_OK) {
        return false;
    }
    if (info.width != SCREEN_W || info.height != SCREEN_H) {
        return false;
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t decoded_size = s_screen_alloc[surface_idx];
    esp_err_t ret = jpeg_decoder_process(engine, &cfg,
                                         jpeg, jpeg_len,
                                         s_screen_buf[surface_idx], s_screen_alloc[surface_idx],
                                         &decoded_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG direct frame decode failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_cache_msync(s_screen_buf[surface_idx], s_screen_alloc[surface_idx], ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    s_decode_w = info.width;
    s_decode_h = info.height;
    s_decode_stride_px = SCREEN_W;
    return true;
}

static bool setup_frame_queues(void)
{
    if (!ensure_frame_slots()) return false;

    if (s_free_q) {
        vQueueDelete(s_free_q);
        s_free_q = NULL;
    }
    if (s_filled_q) {
        vQueueDelete(s_filled_q);
        s_filled_q = NULL;
    }

    s_free_q = xQueueCreate(FRAME_SLOT_COUNT, sizeof(int));
    s_filled_q = xQueueCreate(FRAME_SLOT_COUNT, sizeof(int));
    if (!s_free_q || !s_filled_q) {
        ESP_LOGE(TAG, "Frame queues alloc failed");
        return false;
    }

    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        s_slots[i].len = 0;
        s_slots[i].pos = 0;
        s_slots[i].read_us = 0;
        s_slots[i].eof = false;
        xQueueSend(s_free_q, &i, 0);
    }
    return true;
}

static void reader_task(void *arg)
{
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s", (const char *)arg);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Reader cannot open video: %s", path);
        int eof_idx = -1;
        if (s_free_q && xQueueReceive(s_free_q, &eof_idx, pdMS_TO_TICKS(100)) == pdTRUE && eof_idx >= 0) {
            s_slots[eof_idx].eof = true;
            xQueueSend(s_filled_q, &eof_idx, 0);
        }
        s_reader_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ensure_read_buffers();
    mjpeg_reader_t reader = {
        .f = f,
        .pos = 0,
        .len = 0,
        .prev = 0,
        .in_frame = false,
    };

    while (s_running) {
        int idx = -1;
        if (xQueueReceive(s_free_q, &idx, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (idx < 0 || idx >= FRAME_SLOT_COUNT) continue;

        frame_slot_t *slot = &s_slots[idx];
        slot->len = 0;
        slot->eof = false;

        int64_t read_start = esp_timer_get_time();
        bool ok = mjpeg_read_frame(&reader, slot->buf, slot->cap, &slot->len);
        slot->read_us = esp_timer_get_time() - read_start;
        slot->pos = ftell(f);

        if (!ok || slot->len == 0) {
            slot->eof = true;
            xQueueSend(s_filled_q, &idx, pdMS_TO_TICKS(100));
            break;
        }

        if (xQueueSend(s_filled_q, &idx, pdMS_TO_TICKS(100)) != pdTRUE) {
            xQueueSend(s_free_q, &idx, 0);
        }
    }

    fclose(f);
    s_reader_task = NULL;
    vTaskDelete(NULL);
}

static void update_status_locked(const char *path, long pos, long total, bool eof)
{
    if (!s_status || !s_progress) return;

    const int pct = total > 0 ? (int)((pos * 100LL) / total) : 0;
    lv_bar_set_value(s_progress, pct, LV_ANIM_OFF);

    if (eof) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_OK " Finished  %s", path_basename(path));
    } else if (s_paused) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_PAUSE " Paused  %d%%", pct);
    } else {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_PLAY " Playing  %d%%", pct);
    }
}

static void update_perf_locked(const video_perf_t *perf, int64_t now_us)
{
    if (!perf || !s_perf_labels[0] || perf->frames == 0) return;

    int64_t elapsed_us = now_us - perf->window_start_us;
    if (elapsed_us <= 0) return;

    const float fps = (float)perf->frames * 1000000.0f / (float)elapsed_us;
    const float mbps = (float)perf->bytes * 1000000.0f / (float)elapsed_us / (1024.0f * 1024.0f);
    const float avg_read = (float)perf->read_us / (float)perf->frames / 1000.0f;
    const float avg_decode = (float)perf->decode_us / (float)perf->frames / 1000.0f;
    const float avg_render = (float)perf->render_us / (float)perf->frames / 1000.0f;
    const float avg_lvgl = (float)perf->lvgl_us / (float)perf->frames / 1000.0f;
    const float busy_pct = (float)perf->busy_us * 100.0f / (float)elapsed_us;
    const float budget_ms = 1000.0f / (float)VIDEO_FPS_DEFAULT;

    perf_state_t fps_state = perf_from_fps(fps);
    perf_state_t read_state = perf_from_ratio(avg_read / budget_ms, 0.20f, 0.35f);
    perf_state_t decode_state = perf_from_ratio(avg_decode / budget_ms, 0.25f, 0.45f);
    perf_state_t render_state = perf_from_ratio(avg_render / budget_ms, 0.35f, 0.60f);
    perf_state_t lvgl_state = perf_from_ratio(avg_lvgl / budget_ms, 0.10f, 0.25f);

    lv_label_set_text_fmt(s_perf_labels[0], "%s FPS %.1f L%.0f H%.0f",
                          perf_tag(fps_state),
                          fps,
                          perf->frame_max_us > 0 ? 1000000.0f / (float)perf->frame_max_us : 0.0f,
                          perf->frame_min_us > 0 ? 1000000.0f / (float)perf->frame_min_us : 0.0f);
    lv_label_set_text_fmt(s_perf_labels[1], "%s SD %.1fM %.1fms", perf_tag(read_state), mbps, avg_read);
    lv_label_set_text_fmt(s_perf_labels[2], "%s JPG %.1fms", perf_tag(decode_state), avg_decode);
    lv_label_set_text_fmt(s_perf_labels[3], "%s DRAW %.1fms", perf_tag(render_state), avg_render);
    lv_label_set_text_fmt(s_perf_labels[4], "%s UI %.1fms C%.0f%%", perf_tag(lvgl_state), avg_lvgl, busy_pct);

    lv_obj_set_style_text_color(s_perf_labels[0], perf_color(fps_state), 0);
    lv_obj_set_style_text_color(s_perf_labels[1], perf_color(read_state), 0);
    lv_obj_set_style_text_color(s_perf_labels[2], perf_color(decode_state), 0);
    lv_obj_set_style_text_color(s_perf_labels[3], perf_color(render_state), 0);
    lv_obj_set_style_text_color(s_perf_labels[4], perf_color(lvgl_state), 0);
}

static void reset_perf_window(video_perf_t *perf, int64_t now_us)
{
    memset(perf, 0, sizeof(*perf));
    perf->window_start_us = now_us;
    perf->frame_min_us = INT64_MAX;
}

static bool present_surface(int surface_idx, const char *path, long pos, long total,
                            uint32_t frame_no, video_perf_t *perf, int64_t *lvgl_us)
{
    int64_t t0 = esp_timer_get_time();
    if (bsp_display_lock(60)) {
        if (s_img && s_scr) {
            s_screen_dsc[surface_idx].data = s_screen_buf[surface_idx];
            lv_image_set_src(s_img, &s_screen_dsc[surface_idx]);
            lv_image_cache_drop(&s_screen_dsc[surface_idx]);
            lv_obj_invalidate(s_img);
            s_present_idx = surface_idx;
        }
        if ((frame_no % VIDEO_FPS_DEFAULT) == 0) {
            update_status_locked(path, pos, total, false);
        }
        if ((frame_no % VIDEO_FPS_DEFAULT) == 0) {
            update_perf_locked(perf, esp_timer_get_time());
        }
        bsp_display_unlock();
    } else {
        return false;
    }

    if (lvgl_us) *lvgl_us = esp_timer_get_time() - t0;
    return true;
}

static bool present_frame(const char *path, long pos, long total, uint32_t frame_no,
                          video_perf_t *perf, int64_t *render_us, int64_t *lvgl_us)
{
    int next = 1 - s_present_idx;
    int64_t t0 = esp_timer_get_time();
    if (!render_frame_to_surface(s_decode_buf, s_decode_w, s_decode_h, s_decode_stride_px, next)) {
        return false;
    }
    int64_t t1 = esp_timer_get_time();

    if (render_us) *render_us = t1 - t0;
    return present_surface(next, path, pos, total, frame_no, perf, lvgl_us);
}

static void playback_task(void *arg)
{
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s", (const char *)arg);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open video: %s", path);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fclose(f);

    if (!setup_frame_queues()) {
        ESP_LOGE(TAG, "Could not setup video frame queues");
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    jpeg_decoder_handle_t engine = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    if (jpeg_new_decoder_engine(&engine_cfg, &engine) != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder engine failed");
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playing MJPEG: %s (%ld bytes)", path, total);

    if (xTaskCreatePinnedToCore(reader_task, "video_read", 6144,
                                s_pending_path, 4, &s_reader_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "Could not create video reader task");
        jpeg_del_decoder_engine(engine);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    video_audio_start(path);

    const uint32_t target_fps = fps_from_path(path);
    ESP_LOGI(TAG, "MJPEG target fps: %lu", (unsigned long)target_fps);
    TickType_t next_wake = xTaskGetTickCount();
    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / target_fps);
    uint32_t frames = 0;
    int64_t last_diag = esp_timer_get_time();
    video_perf_t perf = {0};
    reset_perf_window(&perf, last_diag);

    while (s_running) {
        while (s_running && s_paused) {
    if (bsp_display_lock(20)) {
                update_status_locked(path, ftell(f), total, false);
                update_perf_locked(&perf, esp_timer_get_time());
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        if (!s_running) break;

        int64_t frame_start = esp_timer_get_time();
        if (perf.last_frame_start_us > 0) {
            int64_t frame_delta = frame_start - perf.last_frame_start_us;
            if (frame_delta < perf.frame_min_us) perf.frame_min_us = frame_delta;
            if (frame_delta > perf.frame_max_us) perf.frame_max_us = frame_delta;
            perf.last_frame_us = frame_delta;
        }
        perf.last_frame_start_us = frame_start;

        int slot_idx = -1;
        if (xQueueReceive(s_filled_q, &slot_idx, pdMS_TO_TICKS(250)) != pdTRUE) {
            if (!s_reader_task) break;
            continue;
        }
        if (slot_idx < 0 || slot_idx >= FRAME_SLOT_COUNT) {
            continue;
        }
        frame_slot_t *slot = &s_slots[slot_idx];
        if (slot->eof) {
            xQueueSend(s_free_q, &slot_idx, 0);
            break;
        }

        int64_t decode_start = esp_timer_get_time();
        int next_surface = 1 - s_present_idx;
        bool direct_surface = false;
        if (decode_frame_to_surface(engine, slot->buf, slot->len, next_surface)) {
            direct_surface = true;
        } else if (!decode_frame(engine, slot->buf, slot->len)) {
            perf.dropped++;
            perf.drop_decode++;
            frames++;
            xQueueSend(s_free_q, &slot_idx, 0);
            goto frame_delay;
        }

        {
            int64_t decode_end = esp_timer_get_time();
            int64_t render_us = 0;
            int64_t lvgl_us = 0;
            bool presented = direct_surface ?
                present_surface(next_surface, path, slot->pos, total, frames, &perf, &lvgl_us) :
                present_frame(path, slot->pos, total, frames, &perf, &render_us, &lvgl_us);

            if (presented) {
                perf.frames++;
                perf.bytes += slot->len;
                perf.read_us += slot->read_us;
                perf.decode_us += decode_end - decode_start;
                perf.render_us += render_us;
                perf.lvgl_us += lvgl_us;
                perf.busy_us += (decode_end - decode_start) + render_us + lvgl_us;
            } else {
                perf.dropped++;
                perf.drop_present++;
            }
            frames++;
            xQueueSend(s_free_q, &slot_idx, 0);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_diag > 5000000) {
            int64_t elapsed_us = now - perf.window_start_us;
            float fps = elapsed_us > 0 ? (float)perf.frames * 1000000.0f / (float)elapsed_us : 0.0f;
            float mbps = elapsed_us > 0 ? (float)perf.bytes * 1000000.0f / (float)elapsed_us / (1024.0f * 1024.0f) : 0.0f;
            float avg_read = perf.frames ? (float)perf.read_us / (float)perf.frames / 1000.0f : 0.0f;
            float avg_decode = perf.frames ? (float)perf.decode_us / (float)perf.frames / 1000.0f : 0.0f;
            float avg_render = perf.frames ? (float)perf.render_us / (float)perf.frames / 1000.0f : 0.0f;
            float avg_lvgl = perf.frames ? (float)perf.lvgl_us / (float)perf.frames / 1000.0f : 0.0f;
            float busy_pct = elapsed_us > 0 ? (float)perf.busy_us * 100.0f / (float)elapsed_us : 0.0f;
            float low_fps = perf.frame_max_us > 0 ? 1000000.0f / (float)perf.frame_max_us : 0.0f;
            float high_fps = perf.frame_min_us > 0 && perf.frame_min_us != INT64_MAX ? 1000000.0f / (float)perf.frame_min_us : 0.0f;
            ESP_LOGI(TAG,
                     "perf fps=%.1f low=%.1f high=%.1f busy=%.0f%% sd=%.1fMB/s read=%.1fms jpg=%.1fms draw=%.1fms ui=%.1fms drop=%lu dec=%lu prs=%lu pos=%ld/%ld src=%lux%lu free_psram=%u",
                     fps, low_fps, high_fps, busy_pct, mbps,
                     avg_read, avg_decode, avg_render, avg_lvgl,
                     (unsigned long)perf.dropped,
                     (unsigned long)perf.drop_decode,
                     (unsigned long)perf.drop_present,
                     slot->pos, total,
                     (unsigned long)s_decode_w, (unsigned long)s_decode_h,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            reset_perf_window(&perf, now);
            last_diag = now;
        }

frame_delay:
        vTaskDelayUntil(&next_wake, frame_ticks);
    }

    jpeg_del_decoder_engine(engine);
    video_audio_stop();

    for (int i = 0; i < 20 && s_reader_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (bsp_display_lock(10)) {
        update_status_locked(path, total, total, true);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "Stopped MJPEG: %s (%lu frames)", path, (unsigned long)frames);
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

void video_player_stop(void)
{
    s_running = false;
    s_paused = false;
    video_audio_stop();
    for (int i = 0; i < 30 && (s_task || s_reader_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void start_video_path(const char *path)
{
    if (!path || !path[0]) return;

    video_player_stop();
    snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
    s_running = true;
    s_paused = false;

    if (s_play_label) lv_label_set_text(s_play_label, LV_SYMBOL_PAUSE);
    if (s_title) lv_label_set_text(s_title, path_basename(path));
    if (s_status) lv_label_set_text(s_status, "Opening video...");
    if (s_progress) lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    for (int i = 0; i < 5; i++) {
        if (s_perf_labels[i]) {
            lv_label_set_text(s_perf_labels[i], i == 0 ? "FPS --" :
                                               i == 1 ? "SD --" :
                                               i == 2 ? "JPG --" :
                                               i == 3 ? "PPA --" : "UI --");
            lv_obj_set_style_text_color(s_perf_labels[i], lv_color_hex(0xaaaaaa), 0);
        }
    }

    BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "video_play", 8192,
                                           s_pending_path, 5, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create video task");
        s_running = false;
        s_task = NULL;
        if (s_status) lv_label_set_text(s_status, "Could not start video task");
    }
}

static void scan_videos(const char *dir_path)
{
    if (!s_video_paths) {
        s_video_paths = heap_caps_malloc(MAX_VIDEOS * PATH_MAX_LEN, MALLOC_CAP_SPIRAM);
        if (!s_video_paths) {
            ESP_LOGE(TAG, "Video path table alloc failed");
            return;
        }
    }
    s_video_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "No video directory: %s", dir_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_video_count < MAX_VIDEOS) {
        if (entry->d_name[0] == '.') continue;
        if (!is_video_file(entry->d_name)) continue;
        snprintf(s_video_paths[s_video_count], PATH_MAX_LEN, "%s/%s", dir_path, entry->d_name);
        s_video_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Found %d videos in %s", s_video_count, dir_path);
}

static void video_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_video_count) return;
    s_current_idx = idx;
    start_video_path(s_video_paths[idx]);
}

static void video_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_video_count <= 0) return;
    s_current_idx = (s_current_idx <= 0) ? s_video_count - 1 : s_current_idx - 1;
    start_video_path(s_video_paths[s_current_idx]);
}

static void video_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_video_count <= 0) return;
    s_current_idx = (s_current_idx + 1) % s_video_count;
    start_video_path(s_video_paths[s_current_idx]);
}

static void video_pause_cb(lv_event_t *e)
{
    (void)e;
    if (!s_running) {
        if (s_current_idx >= 0 && s_current_idx < s_video_count) {
            start_video_path(s_video_paths[s_current_idx]);
        }
        return;
    }
    s_paused = !s_paused;
    video_audio_pause(s_paused);
    if (s_play_label) lv_label_set_text(s_play_label, s_paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void video_back_async(void *arg)
{
    (void)arg;
    video_player_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_img = NULL;
        s_list = NULL;
        s_title = NULL;
        s_status = NULL;
        s_progress = NULL;
        s_play_label = NULL;
        memset(s_perf_labels, 0, sizeof(s_perf_labels));
    }
    show_desktop();
}

static void video_back_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(video_back_async, NULL);
}

static void make_ctrl_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 55, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);
    if (label_out) *label_out = lbl;
}

static void build_video_ui(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x05070d), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(s_scr);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *panel = lv_obj_create(s_scr);
    lv_obj_set_size(panel, 1004, 184);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101827), 0);
    lv_obj_set_style_bg_opa(panel, 210, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t *back_btn = lv_btn_create(panel);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, video_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    s_title = lv_label_create(panel);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_title, 760);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 95, 8);
    lv_label_set_text(s_title, "Video Player");

    s_status = lv_label_create(panel);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_status, 760);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 95, 44);
    lv_label_set_text(s_status, "Choose an MJPEG file");

    lv_obj_t *ctrl_row = lv_obj_create(panel);
    lv_obj_set_size(ctrl_row, 300, 50);
    lv_obj_align(ctrl_row, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 6, 0);
    make_ctrl_btn(ctrl_row, LV_SYMBOL_PREV, video_prev_cb, NULL);
    make_ctrl_btn(ctrl_row, LV_SYMBOL_PLAY, video_pause_cb, &s_play_label);
    make_ctrl_btn(ctrl_row, LV_SYMBOL_NEXT, video_next_cb, NULL);

    lv_obj_t *perf_row = lv_obj_create(panel);
    lv_obj_set_size(perf_row, 964, 24);
    lv_obj_align(perf_row, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_opa(perf_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(perf_row, 0, 0);
    lv_obj_set_style_pad_all(perf_row, 0, 0);
    lv_obj_set_flex_flow(perf_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(perf_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const char *perf_init[5] = {"FPS --", "SD --", "JPG --", "PPA --", "UI --"};
    for (int i = 0; i < 5; i++) {
        s_perf_labels[i] = lv_label_create(perf_row);
        lv_obj_set_style_text_font(s_perf_labels[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_perf_labels[i], lv_color_hex(0xaaaaaa), 0);
        lv_label_set_text(s_perf_labels[i], perf_init[i]);
    }

    s_progress = lv_bar_create(panel);
    lv_obj_set_size(s_progress, 964, 8);
    lv_obj_align(s_progress, LV_ALIGN_TOP_MID, 0, 108);
    lv_bar_set_range(s_progress, 0, 100);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);

    s_list = lv_list_create(panel);
    lv_obj_set_size(s_list, 964, 50);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    if (s_video_count == 0) {
        lv_obj_t *empty = lv_list_add_text(s_list, "Put .mjpg or .mjpeg files in /sdcard/video");
        lv_obj_set_style_text_color(empty, lv_color_hex(0xaaaaaa), 0);
    } else {
        for (int i = 0; i < s_video_count; i++) {
            lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_PLAY, path_basename(s_video_paths[i]));
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x0f3460), 0);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xe0e0e0), 0);
            lv_obj_add_event_cb(btn, video_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    lv_scr_load(s_scr);
}

void video_player_show(void)
{
    scan_videos(VIDEO_DIR);
    s_current_idx = (s_video_count > 0) ? 0 : -1;
    bsp_display_lock(0);
    build_video_ui();
    bsp_display_unlock();
}

void video_player_open(const char *filepath)
{
    if (!filepath || !filepath[0]) return;

    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), "%s", VIDEO_DIR);

    scan_videos(dir);
    s_current_idx = 0;
    for (int i = 0; i < s_video_count; i++) {
        if (strcmp(s_video_paths[i], filepath) == 0) {
            s_current_idx = i;
            break;
        }
    }

    build_video_ui();
    start_video_path(filepath);
}
