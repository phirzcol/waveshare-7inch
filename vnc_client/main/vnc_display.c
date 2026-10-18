// =============================================================================
// VNC Display — HW JPEG Decode + lv_image (Photo Viewer Pattern)
// =============================================================================
// Uses the same approach as the working photo_viewer.c:
// - jpeg_alloc_decoder_mem() for DMA-aligned buffers
// - lv_image widget with lv_image_dsc_t (explicit stride)
// - Height padded to 16-pixel alignment (608, not 600)
// =============================================================================

#include "vnc_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"

#include <string.h>

static const char *TAG = "VNC_DISP";

#define SCREEN_W        1024
#define SCREEN_H        600
#define ALIGNED_W       1024            // Already 16-aligned
#define ALIGNED_H       600             // No height padding needed for raw pixel buffer
#define FB_STRIDE       (ALIGNED_W * 2) // 2048 bytes per row (RGB565)
#define FB_SIZE         (FB_STRIDE * ALIGNED_H)  // 1024 * 600 * 2 = 1,228,800

// Single framebuffer (DMA-aligned via jpeg_alloc_decoder_mem)
static uint8_t *_fb_front = NULL;

// LVGL objects
static lv_obj_t *_scr = NULL;
static lv_obj_t *_img_widget = NULL;
static lv_image_dsc_t _img_dsc;

// JPEG decode temp buffer
static uint8_t *_jpeg_out_buf = NULL;
static size_t _jpeg_out_buf_size = 0;

// PPA fill client
static ppa_client_handle_t _ppa_fill_client = NULL;
static size_t _fb_alloc_size = 0;  // Actual allocation size (may be > FB_SIZE due to alignment)

void vnc_display_init(void)
{
    // Single buffer mode — allocate one DMA-aligned framebuffer
    jpeg_decode_memory_alloc_cfg_t alloc_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t actual_size = 0;

    _fb_front = (uint8_t *)jpeg_alloc_decoder_mem(FB_SIZE, &alloc_cfg, &actual_size);

    if (!_fb_front) {
        ESP_LOGE(TAG, "Failed to allocate DMA-aligned framebuffer (need %d bytes)", FB_SIZE);
        return;
    }
    _fb_alloc_size = actual_size;
    memset(_fb_front, 0, FB_SIZE);

    ESP_LOGI(TAG, "Framebuffer allocated: %p (%u bytes)", _fb_front, (unsigned)actual_size);

    // Create LVGL screen
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Setup image descriptor — same pattern as photo_viewer.c
    memset(&_img_dsc, 0, sizeof(_img_dsc));
    _img_dsc.header.w = SCREEN_W;           // Visible width
    _img_dsc.header.h = SCREEN_H;           // Visible height (not aligned)
    _img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    _img_dsc.header.stride = FB_STRIDE;     // Bytes per row (aligned_w * 2)
    _img_dsc.data_size = FB_SIZE;
    _img_dsc.data = _fb_front;

    // Create lv_image widget (NOT lv_canvas)
    _img_widget = lv_image_create(_scr);
    lv_image_set_src(_img_widget, &_img_dsc);
    lv_obj_align(_img_widget, LV_ALIGN_TOP_LEFT, 0, 0);

    ESP_LOGI(TAG, "VNC display initialized: %dx%d (buffer %dx%d, stride %d)",
             SCREEN_W, SCREEN_H, ALIGNED_W, ALIGNED_H, FB_STRIDE);

    // Register PPA fill client
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    if (ppa_register_client(&ppa_cfg, &_ppa_fill_client) != ESP_OK) {
        ESP_LOGW(TAG, "PPA fill unavailable");
        _ppa_fill_client = NULL;
    } else {
        ESP_LOGI(TAG, "PPA fill enabled");
    }
}

void vnc_display_update_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *data, size_t len, bool is_jpeg)
{
    if (!_fb_front) return;

    // Single buffer mode — write directly to front buffer (displayed by LVGL)
    const uint32_t stride_px = ALIGNED_W;
    uint16_t *fb16 = (uint16_t *)_fb_front;

    if (is_jpeg) {
        jpeg_decoder_handle_t hw_jpeg = NULL;
        jpeg_decode_engine_cfg_t engine_cfg = { .intr_priority = 0, .timeout_ms = 2000 };
        if (jpeg_new_decoder_engine(&engine_cfg, &hw_jpeg) != ESP_OK) return;

        jpeg_decode_picture_info_t info;
        if (jpeg_decoder_get_info(data, len, &info) != ESP_OK) {
            jpeg_del_decoder_engine(hw_jpeg);
            return;
        }

        uint32_t tile_aligned_w = (info.width + 15) & ~15;
        uint32_t tile_aligned_h = (info.height + 15) & ~15;
        size_t out_size = tile_aligned_w * tile_aligned_h * 2;

        if (!_jpeg_out_buf || _jpeg_out_buf_size < out_size) {
            if (_jpeg_out_buf) heap_caps_free(_jpeg_out_buf);
            jpeg_decode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
            size_t actual = 0;
            _jpeg_out_buf = (uint8_t *)jpeg_alloc_decoder_mem(out_size, &rx_cfg, &actual);
            _jpeg_out_buf_size = actual;
            if (!_jpeg_out_buf) { jpeg_del_decoder_engine(hw_jpeg); return; }
        }

        jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        };
        uint32_t decoded_size = _jpeg_out_buf_size;
        esp_err_t ret = jpeg_decoder_process(hw_jpeg, &decode_cfg,
            data, len, _jpeg_out_buf, _jpeg_out_buf_size, &decoded_size);
        jpeg_del_decoder_engine(hw_jpeg);
        if (ret != ESP_OK) return;

        // Copy decoded tile into back buffer using ALIGNED stride
        uint16_t copy_w = (w < info.width) ? w : info.width;
        uint16_t copy_h = (h < info.height) ? h : info.height;
        uint16_t *fb16 = (uint16_t *)_fb_front;

        for (uint16_t row = 0; row < copy_h; row++) {
            if (y + row >= SCREEN_H) break;
            uint16_t *src_row = (uint16_t *)(_jpeg_out_buf + row * tile_aligned_w * 2);
            uint16_t *dst_row = fb16 + (y + row) * stride_px + x;
            uint16_t cols = (x + copy_w > SCREEN_W) ? (SCREEN_W - x) : copy_w;
            memcpy(dst_row, src_row, cols * 2);
        }

    } else if (len == 2 && w > 0 && h > 0) {
        // Solid fill
        uint16_t color = data[0] | (data[1] << 8);
        uint16_t fill_w = (x + w > SCREEN_W) ? (SCREEN_W - x) : w;
        uint16_t fill_h = (y + h > SCREEN_H) ? (SCREEN_H - y) : h;
        if (fill_w == 0 || fill_h == 0) return;

        bool ppa_done = false;
        if (_ppa_fill_client && fill_w >= 4 && fill_h >= 4) {
            // Use PPA for fills >= 4x4 pixels
            // Flush cache before PPA writes
            esp_cache_msync(_fb_front, _fb_alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

            ppa_fill_oper_config_t fill_cfg = {
                .out = {
                    .buffer = _fb_front,
                    .buffer_size = _fb_alloc_size,
                    .pic_w = ALIGNED_W,
                    .pic_h = ALIGNED_H,
                    .block_offset_x = x,
                    .block_offset_y = y,
                    .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
                },
                .fill_block_w = fill_w,
                .fill_block_h = fill_h,
                .fill_color_val = color,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            if (ppa_do_fill(_ppa_fill_client, &fill_cfg) == ESP_OK) {
                // Invalidate cache after PPA writes
                esp_cache_msync(_fb_front, _fb_alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                ppa_done = true;
            }
        }

        if (!ppa_done) {
            // CPU fallback
            uint16_t *fb16 = (uint16_t *)_fb_front;
            for (uint16_t row = 0; row < fill_h; row++) {
                uint16_t *dst_row = fb16 + (y + row) * stride_px + x;
                for (uint16_t col = 0; col < fill_w; col++) dst_row[col] = color;
            }
        }

    } else {
        // Raw RGB565 pixels
        uint16_t cols = (x + w > SCREEN_W) ? (SCREEN_W - x) : w;
        uint16_t rows = h;
        if (y + rows > SCREEN_H) rows = SCREEN_H - y;
        if (cols == 0 || rows == 0) return;

        const uint16_t *src = (const uint16_t *)data;
        uint16_t *fb16 = (uint16_t *)_fb_front;
        for (uint16_t row = 0; row < rows; row++) {
            uint16_t *dst_row = fb16 + (y + row) * stride_px + x;
            memcpy(dst_row, src + row * w, cols * 2);
        }
    }
}

void vnc_display_swap(void)
{
    if (!_fb_front || !_img_widget) return;

    // Flush CPU cache to RAM so display DMA sees fresh pixel data
    esp_cache_msync(_fb_front, _fb_alloc_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // Single buffer mode — tell LVGL to redraw from the framebuffer
    if (lvgl_port_lock(pdMS_TO_TICKS(50))) {
        lv_image_cache_drop(&_img_dsc);
        lv_obj_invalidate(_img_widget);
        lvgl_port_unlock();
    }
}

lv_obj_t *vnc_display_get_screen(void)
{
    return _scr;
}
