// =============================================================================
// ESP32-P4 Tablet — Photo Viewer
// =============================================================================
// Hardware JPEG decode, 3-buffer pre-loading, centered display.
// Tap left = prev, tap right = next, back button = return.
// =============================================================================

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "photo_viewer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "src/libs/tjpgd/tjpgd.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "PHOTOS";

#define PHOTOS_DIR      "/sdcard/photos"
#define MAX_PHOTOS      128
#define PATH_MAX_LEN    256
#define MAX_TJPGD_W     1024
#define MAX_TJPGD_H     768
#define MAX_PRELOAD_PIXELS (1280U * 768U)
#define SCREEN_W        1024
#define SCREEN_H        600
#define SCREEN_STRIDE   (SCREEN_W * 2)
#define SCREEN_SIZE     (SCREEN_W * SCREEN_H * 2)
#define ALIGN16(x)      (((x) + 15) & ~15)

// =============================================================================
// State
// =============================================================================

static lv_obj_t *_scr = NULL;
static lv_obj_t *_img_widget = NULL;
static lv_obj_t *_back_btn = NULL;

// Photo list (PSRAM allocated)
static char (*_photo_names)[PATH_MAX_LEN] = NULL;
static int _photo_count = 0;
static int _current_idx = 0;
static char _photo_dir[PATH_MAX_LEN] = {0};

// 3 decode buffers
static uint8_t *_buf[3] = {NULL, NULL, NULL};  // [0]=prev, [1]=current, [2]=next
static lv_image_dsc_t _dsc[3] = {0};
static bool _buf_valid[3] = {false, false, false};
static uint32_t _src_stride_px[3] = {0, 0, 0};

// Full-screen presentation surface. LVGL always sees a 1024x600 image; scaling is done before display.
static uint8_t *_screen_buf = NULL;
static size_t _screen_alloc_size = 0;
static lv_image_dsc_t _screen_dsc = {0};
static ppa_client_handle_t _ppa_srm = NULL;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} mem_jpeg_reader_t;

typedef struct {
    uint8_t *dst;
    uint32_t w;
    uint32_t h;
    uint32_t stride_px;
} tjpgd_out_t;

typedef struct {
    mem_jpeg_reader_t reader;
    tjpgd_out_t out;
} tjpgd_ctx_t;

// =============================================================================
// External
// =============================================================================
extern void show_desktop(void);

static bool ensure_present_surface(void)
{
    if (!_screen_buf) {
        jpeg_decode_memory_alloc_cfg_t cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        _screen_buf = (uint8_t *)jpeg_alloc_decoder_mem(SCREEN_SIZE, &cfg, &_screen_alloc_size);
        if (!_screen_buf) {
            ESP_LOGE(TAG, "Alloc screen surface failed (%u bytes)", (unsigned)SCREEN_SIZE);
            return false;
        }

        memset(&_screen_dsc, 0, sizeof(_screen_dsc));
        _screen_dsc.header.w = SCREEN_W;
        _screen_dsc.header.h = SCREEN_H;
        _screen_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        _screen_dsc.header.stride = SCREEN_STRIDE;
        _screen_dsc.data_size = SCREEN_SIZE;
        _screen_dsc.data = _screen_buf;
    }

    if (!_ppa_srm) {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&ppa_cfg, &_ppa_srm) != ESP_OK) {
            ESP_LOGW(TAG, "PPA scale unavailable; using CPU fallback");
            _ppa_srm = NULL;
        }
    }

    return true;
}

static void scale_into_surface_cpu(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                   uint32_t src_stride_px, uint32_t out_w,
                                   uint32_t out_h, uint32_t off_x, uint32_t off_y)
{
    uint16_t *dst16 = (uint16_t *)_screen_buf;
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

static bool render_to_surface(int buf_idx)
{
    if (buf_idx < 0 || buf_idx > 2 || !_buf_valid[buf_idx] || !_buf[buf_idx]) return false;
    if (!ensure_present_surface()) return false;

    const uint32_t src_w = _dsc[buf_idx].header.w;
    const uint32_t src_h = _dsc[buf_idx].header.h;
    const uint32_t src_stride_px = _src_stride_px[buf_idx];
    if (src_w == 0 || src_h == 0 || src_stride_px == 0) return false;

    float scale = (float)SCREEN_W / (float)src_w;
    float scale_y = (float)SCREEN_H / (float)src_h;
    if (scale_y < scale) scale = scale_y;

    uint32_t out_w = ((uint32_t)((float)src_w * scale) / 16U) * 16U;
    uint32_t out_h = ((uint32_t)((float)src_h * scale) / 16U) * 16U;
    if (out_w < 16) out_w = 16;
    if (out_h < 16) out_h = 16;
    if (out_w > SCREEN_W) out_w = SCREEN_W;
    if (out_h > SCREEN_H) out_h = SCREEN_H;

    const uint32_t off_x = (SCREEN_W - out_w) / 2;
    const uint32_t off_y = (SCREEN_H - out_h) / 2;
    memset(_screen_buf, 0, SCREEN_SIZE);

    bool ppa_done = false;
    if (_ppa_srm) {
        esp_cache_msync(_buf[buf_idx], _dsc[buf_idx].data_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(_screen_buf, _screen_alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        ppa_srm_oper_config_t cfg = {0};
        cfg.in.buffer = _buf[buf_idx];
        cfg.in.pic_w = src_stride_px;
        cfg.in.pic_h = ALIGN16(src_h);
        cfg.in.block_w = src_w;
        cfg.in.block_h = src_h;
        cfg.in.block_offset_x = 0;
        cfg.in.block_offset_y = 0;
        cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.out.buffer = _screen_buf;
        cfg.out.buffer_size = _screen_alloc_size;
        cfg.out.pic_w = SCREEN_W;
        cfg.out.pic_h = SCREEN_H;
        cfg.out.block_offset_x = off_x;
        cfg.out.block_offset_y = off_y;
        cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        cfg.scale_x = (float)out_w / (float)src_w;
        cfg.scale_y = (float)out_h / (float)src_h;
        cfg.mode = PPA_TRANS_MODE_BLOCKING;

        esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_srm, &cfg);
        if (ret == ESP_OK) {
            esp_cache_msync(_screen_buf, _screen_alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            ppa_done = true;
        } else {
            ESP_LOGW(TAG, "PPA scale failed: %s; using CPU fallback", esp_err_to_name(ret));
        }
    }

    if (!ppa_done) {
        scale_into_surface_cpu(_buf[buf_idx], src_w, src_h, src_stride_px, out_w, out_h, off_x, off_y);
    }

    ESP_LOGI(TAG, "Presented %lux%lu -> %lux%lu at %lu,%lu",
             (unsigned long)src_w, (unsigned long)src_h,
             (unsigned long)out_w, (unsigned long)out_h,
             (unsigned long)off_x, (unsigned long)off_y);
    return true;
}

// =============================================================================
// JPEG Hardware Decode
// =============================================================================

static void free_decode_buffer(int buf_idx)
{
    if (buf_idx < 0 || buf_idx > 2) return;
    if (_buf[buf_idx]) {
        heap_caps_free(_buf[buf_idx]);
        _buf[buf_idx] = NULL;
    }
    memset(&_dsc[buf_idx], 0, sizeof(_dsc[buf_idx]));
    _src_stride_px[buf_idx] = 0;
    _buf_valid[buf_idx] = false;
}

static bool jpeg_info_from_file(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t header[4096];
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);

    jpeg_decode_picture_info_t pic_info = {0};
    if (jpeg_decoder_get_info(header, got, &pic_info) != ESP_OK) return false;
    if (out_w) *out_w = pic_info.width;
    if (out_h) *out_h = pic_info.height;
    return true;
}

static bool jpeg_largest_sof_info(const uint8_t *buf, size_t size, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t best_w = 0;
    uint32_t best_h = 0;
    size_t pos = 2;

    if (!buf || size < 4 || buf[0] != 0xFF || buf[1] != 0xD8) return false;

    while (pos + 4 < size) {
        while (pos < size && buf[pos] != 0xFF) pos++;
        while (pos < size && buf[pos] == 0xFF) pos++;
        if (pos >= size) break;

        uint8_t marker = buf[pos++];
        if (marker == 0xD9 || marker == 0xDA) break;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (pos + 2 > size) break;

        uint16_t seg_len = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        if (seg_len < 2 || pos + seg_len > size) break;

        bool is_sof = (marker >= 0xC0 && marker <= 0xCF &&
                       marker != 0xC4 && marker != 0xC8 && marker != 0xCC);
        if (is_sof && seg_len >= 7) {
            uint32_t h = ((uint16_t)buf[pos + 3] << 8) | buf[pos + 4];
            uint32_t w = ((uint16_t)buf[pos + 5] << 8) | buf[pos + 6];
            if (w > 0 && h > 0 && (uint64_t)w * h > (uint64_t)best_w * best_h) {
                best_w = w;
                best_h = h;
            }
        }

        pos += seg_len;
    }

    if (best_w == 0 || best_h == 0) return false;
    if (out_w) *out_w = best_w;
    if (out_h) *out_h = best_h;
    return true;
}

static size_t tjpgd_input_func(JDEC *jd, uint8_t *buff, size_t ndata)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    mem_jpeg_reader_t *reader = ctx ? &ctx->reader : NULL;
    if (!reader) return 0;

    size_t remain = reader->size - reader->pos;
    if (ndata > remain) ndata = remain;

    if (buff) {
        memcpy(buff, reader->data + reader->pos, ndata);
    }
    reader->pos += ndata;
    return ndata;
}

static int tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    tjpgd_out_t *out = ctx ? &ctx->out : NULL;
    if (!out || !bitmap || !rect) return 0;

    const uint8_t *src = (const uint8_t *)bitmap;  // JD_FORMAT=RGB888
    uint16_t *dst16 = (uint16_t *)out->dst;
    uint32_t bw = rect->right - rect->left + 1;
    uint32_t bh = rect->bottom - rect->top + 1;

    for (uint32_t y = 0; y < bh; y++) {
        uint32_t dy = rect->top + y;
        if (dy >= out->h) break;
        uint16_t *dst = dst16 + dy * out->stride_px + rect->left;
        const uint8_t *row = src + y * bw * 3;
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t dx = rect->left + x;
            if (dx >= out->w) break;
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            dst[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }

    return 1;
}

static bool decode_jpeg_tjpgd(const char *path, const uint8_t *jbuf, size_t fsize,
                              uint32_t img_w, uint32_t img_h, int buf_idx)
{
    uint8_t scale_shift = 0;
    while (scale_shift < 3 && ((img_w >> scale_shift) > MAX_TJPGD_W || (img_h >> scale_shift) > MAX_TJPGD_H)) {
        scale_shift++;
    }

    uint32_t scaled_w = img_w >> scale_shift;
    uint32_t scaled_h = img_h >> scale_shift;
    if (scaled_w == 0 || scaled_h == 0) return false;

    uint32_t aligned_w = ALIGN16(scaled_w);
    uint32_t aligned_h = ALIGN16(scaled_h);
    size_t out_size = aligned_w * aligned_h * 2;

    if (_buf[buf_idx]) {
        free_decode_buffer(buf_idx);
    }

    jpeg_decode_memory_alloc_cfg_t rx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual_alloc = 0;
    _buf[buf_idx] = (uint8_t *)jpeg_alloc_decoder_mem(out_size, &rx_cfg, &actual_alloc);
    if (!_buf[buf_idx]) {
        ESP_LOGE(TAG, "TJPGD alloc output failed for %s (%u bytes)", path, (unsigned)out_size);
        return false;
    }
    memset(_buf[buf_idx], 0, actual_alloc);

    uint8_t *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) work = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!work) {
        ESP_LOGE(TAG, "TJPGD work buffer alloc failed");
        free_decode_buffer(buf_idx);
        return false;
    }

    JDEC jd = {0};
    tjpgd_ctx_t ctx = {
        .reader = {
            .data = jbuf,
            .size = fsize,
            .pos = 0,
        },
        .out = {
            .dst = _buf[buf_idx],
            .w = scaled_w,
            .h = scaled_h,
            .stride_px = aligned_w,
        },
    };

    JRESULT rc = jd_prepare(&jd, tjpgd_input_func, work, 4096, &ctx);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD prepare failed: %d for %s", rc, path);
        free(work);
        free_decode_buffer(buf_idx);
        return false;
    }

    rc = jd_decomp(&jd, tjpgd_output_func, scale_shift);
    free(work);

    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD decode failed: %d for %s", rc, path);
        free_decode_buffer(buf_idx);
        return false;
    }

    _dsc[buf_idx].header.w = scaled_w;
    _dsc[buf_idx].header.h = scaled_h;
    _dsc[buf_idx].header.cf = LV_COLOR_FORMAT_RGB565;
    _dsc[buf_idx].header.stride = aligned_w * 2;
    _dsc[buf_idx].data_size = actual_alloc;
    _dsc[buf_idx].data = _buf[buf_idx];
    _src_stride_px[buf_idx] = aligned_w;
    _buf_valid[buf_idx] = true;

    ESP_LOGI(TAG, "TJPGD decoded: %s (%lux%lu -> %lux%lu, scale=1/%u)",
             path,
             (unsigned long)img_w, (unsigned long)img_h,
             (unsigned long)scaled_w, (unsigned long)scaled_h,
             (unsigned)(1U << scale_shift));
    return true;
}

static bool decode_jpeg(const char *path, int buf_idx)
{
    if (buf_idx < 0 || buf_idx > 2) return false;

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open: %s", path); return false; }

    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Read compressed JPEG into PSRAM
    uint8_t *jbuf = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!jbuf) { fclose(f); ESP_LOGE(TAG, "Alloc input failed"); return false; }
    fread(jbuf, 1, fsize, f);
    fclose(f);

    // Get image info (dimensions) from header
    jpeg_decode_picture_info_t pic_info = {0};
    esp_err_t info_ret = jpeg_decoder_get_info(jbuf, fsize, &pic_info);
    if (info_ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot get info: %s (%s) — skipping", path, esp_err_to_name(info_ret));
        free(jbuf);
        _buf_valid[buf_idx] = false;
        return false;
    }

    uint32_t img_w = pic_info.width;
    uint32_t img_h = pic_info.height;
    uint32_t sof_w = 0;
    uint32_t sof_h = 0;
    if (jpeg_largest_sof_info(jbuf, fsize, &sof_w, &sof_h) &&
        (uint64_t)sof_w * sof_h > (uint64_t)img_w * img_h) {
        ESP_LOGW(TAG, "JPEG info used embedded frame %lux%lu; using largest SOF %lux%lu",
                 (unsigned long)img_w, (unsigned long)img_h,
                 (unsigned long)sof_w, (unsigned long)sof_h);
        img_w = sof_w;
        img_h = sof_h;
    }
    uint32_t aligned_w = (img_w + 15) & ~15;
    uint32_t aligned_h = (img_h + 15) & ~15;

    ESP_LOGI(TAG, "Info: %s (%lux%lu, aligned %lux%lu)", path,
             (unsigned long)img_w, (unsigned long)img_h,
             (unsigned long)aligned_w, (unsigned long)aligned_h);

    size_t out_size = aligned_w * aligned_h * 2;

    // Free old buffer if size changed, reallocate
    if (_buf[buf_idx]) {
        free_decode_buffer(buf_idx);
    }

    jpeg_decode_memory_alloc_cfg_t rx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual_alloc = 0;
    _buf[buf_idx] = (uint8_t *)jpeg_alloc_decoder_mem(out_size, &rx_cfg, &actual_alloc);
    if (!_buf[buf_idx]) {
        ESP_LOGE(TAG, "Alloc output failed (%u bytes)", (unsigned)out_size);
        free(jbuf);
        return false;
    }

    // Create HW decoder
    jpeg_decoder_handle_t hw_jpeg = NULL;
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 2000,
    };

    if (jpeg_new_decoder_engine(&engine_cfg, &hw_jpeg) != ESP_OK) {
        free(jbuf);
        ESP_LOGE(TAG, "Decoder engine failed");
        free_decode_buffer(buf_idx);
        return false;
    }

    // Decode
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t decoded_size = actual_alloc;
    esp_err_t ret = jpeg_decoder_process(hw_jpeg, &decode_cfg,
        jbuf, fsize, _buf[buf_idx], actual_alloc, &decoded_size);

    jpeg_del_decoder_engine(hw_jpeg);
    free(jbuf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Decode failed: %s (%s)", path, esp_err_to_name(ret));
        free_decode_buffer(buf_idx);
        return false;
    }

    ESP_LOGI(TAG, "Decoded: %s (%lux%lu, %lu bytes)", path,
             (unsigned long)img_w, (unsigned long)img_h, (unsigned long)decoded_size);

    // Setup LVGL image descriptor
    // Use aligned_w as stride so LVGL skips padding bytes per row
    _dsc[buf_idx].header.w = img_w;
    _dsc[buf_idx].header.h = img_h;
    _dsc[buf_idx].header.cf = LV_COLOR_FORMAT_RGB565;
    _dsc[buf_idx].header.stride = aligned_w * 2;  // Stride includes padding
    _dsc[buf_idx].data_size = decoded_size;
    _dsc[buf_idx].data = _buf[buf_idx];
    _src_stride_px[buf_idx] = aligned_w;
    _buf_valid[buf_idx] = true;

    return true;
}

// =============================================================================
// Directory Scanning
// =============================================================================

static void scan_photos(const char *dir)
{
    snprintf(_photo_dir, sizeof(_photo_dir), "%s", dir);

    if (!_photo_names) {
        _photo_names = heap_caps_malloc(MAX_PHOTOS * PATH_MAX_LEN, MALLOC_CAP_SPIRAM);
        if (!_photo_names) { ESP_LOGE(TAG, "Alloc names failed"); return; }
    }

    _photo_count = 0;
    DIR *d = opendir(dir);
    if (!d) { ESP_LOGE(TAG, "Cannot open: %s", dir); return; }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && _photo_count < MAX_PHOTOS) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strlen(entry->d_name);
        if (len < 5) continue;
        const char *ext = &entry->d_name[len - 4];
        bool is_jpg = strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpe") == 0;
        bool is_jpeg = len > 5 && strcasecmp(&entry->d_name[len - 5], ".jpeg") == 0;
        if (is_jpg || is_jpeg) {
            snprintf(_photo_names[_photo_count], PATH_MAX_LEN, "%s/%s", dir, entry->d_name);
            _photo_count++;
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "Found %d photos in %s", _photo_count, dir);
}

// =============================================================================
// Display
// =============================================================================

static void show_current(void)
{
    if (!_img_widget || !_buf_valid[1]) return;
    if (!render_to_surface(1)) return;
    lv_image_set_src(_img_widget, &_screen_dsc);
    lv_image_cache_drop(&_screen_dsc);
    lv_obj_invalidate(_img_widget);
    lv_obj_align(_img_widget, LV_ALIGN_CENTER, 0, 0);
}

static void load_adjacent(void)
{
    // Load prev
    if (_photo_count > 1) {
        int prev_idx = (_current_idx - 1 + _photo_count) % _photo_count;
        uint32_t w = 0, h = 0;
        if (jpeg_info_from_file(_photo_names[prev_idx], &w, &h) && (uint64_t)w * h <= MAX_PRELOAD_PIXELS) {
            decode_jpeg(_photo_names[prev_idx], 0);
        } else {
            free_decode_buffer(0);
            ESP_LOGI(TAG, "Skipped prev preload for large photo: %s", _photo_names[prev_idx]);
        }
    }
    // Load next
    if (_photo_count > 1) {
        int next_idx = (_current_idx + 1) % _photo_count;
        uint32_t w = 0, h = 0;
        if (jpeg_info_from_file(_photo_names[next_idx], &w, &h) && (uint64_t)w * h <= MAX_PRELOAD_PIXELS) {
            decode_jpeg(_photo_names[next_idx], 2);
        } else {
            free_decode_buffer(2);
            ESP_LOGI(TAG, "Skipped next preload for large photo: %s", _photo_names[next_idx]);
        }
    }
}

// =============================================================================
// Navigation Callbacks
// =============================================================================

static void nav_click_cb(lv_event_t *e)
{
    if (_photo_count <= 1) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    if (point.x < 512) {
        // Left half — previous
        _current_idx = (_current_idx - 1 + _photo_count) % _photo_count;
    } else {
        // Right half — next
        _current_idx = (_current_idx + 1) % _photo_count;
    }

    // Decode current — skip if it fails, try next in same direction
    int attempts = 0;
    while (attempts < _photo_count) {
        if (decode_jpeg(_photo_names[_current_idx], 1)) {
            show_current();
            load_adjacent();
            return;
        }
        // Skip this one, move in same direction
        if (point.x < 512) {
            _current_idx = (_current_idx - 1 + _photo_count) % _photo_count;
        } else {
            _current_idx = (_current_idx + 1) % _photo_count;
        }
        attempts++;
    }
}

static void viewer_back_async(void *arg)
{
    (void)arg;
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = NULL;
        _img_widget = NULL;
        _back_btn = NULL;
    }
    show_desktop();
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(viewer_back_async, NULL);
}

// =============================================================================
// UI Builder
// =============================================================================

static void build_viewer_ui(void)
{
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Image widget (centered)
    _img_widget = lv_image_create(_scr);
    lv_obj_align(_img_widget, LV_ALIGN_CENTER, 0, 0);

    // Full-screen clickable overlay for navigation
    lv_obj_t *nav_area = lv_obj_create(_scr);
    lv_obj_set_size(nav_area, 1024, 600);
    lv_obj_set_pos(nav_area, 0, 0);
    lv_obj_set_style_bg_opa(nav_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_area, 0, 0);
    lv_obj_clear_flag(nav_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nav_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nav_area, nav_click_cb, LV_EVENT_CLICKED, NULL);

    // Translucent back button (top-left)
    _back_btn = lv_btn_create(_scr);
    lv_obj_set_size(_back_btn, 80, 40);
    lv_obj_align(_back_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(_back_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_back_btn, 150, 0);
    lv_obj_set_style_radius(_back_btn, 8, 0);
    lv_obj_set_style_border_width(_back_btn, 0, 0);
    lv_obj_add_event_cb(_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);

    lv_scr_load(_scr);
}

// =============================================================================
// Public API
// =============================================================================

void photo_viewer_show(void)
{
    ESP_LOGI(TAG, "Opening photo viewer (random)");
    scan_photos(PHOTOS_DIR);

    if (_photo_count == 0) {
        ESP_LOGW(TAG, "No photos found in %s", PHOTOS_DIR);
        return;
    }

    // Pick random
    _current_idx = (int)(esp_log_timestamp() % _photo_count);

    build_viewer_ui();

    // Decode and show current
    if (decode_jpeg(_photo_names[_current_idx], 1)) {
        show_current();
    }

    // Pre-load adjacent
    load_adjacent();
}

void photo_viewer_open(const char *filepath)
{
    ESP_LOGI(TAG, "Opening photo: %s", filepath);

    // Extract directory from filepath
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) *last_slash = '\0';
    else snprintf(dir, sizeof(dir), "%s", PHOTOS_DIR);

    scan_photos(dir);

    if (_photo_count == 0) {
        ESP_LOGW(TAG, "No photos found");
        return;
    }

    // Find the requested file in the list
    _current_idx = 0;
    for (int i = 0; i < _photo_count; i++) {
        if (strcmp(_photo_names[i], filepath) == 0) {
            _current_idx = i;
            break;
        }
    }

    build_viewer_ui();

    if (decode_jpeg(_photo_names[_current_idx], 1)) {
        show_current();
    }

    load_adjacent();
}
