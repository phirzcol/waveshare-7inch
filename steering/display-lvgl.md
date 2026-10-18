---
inclusion: manual
---

# Display & LVGL 9.2

## Display Stack (top to bottom)
1. **LVGL 9.2** — rendering engine, draws to buffer
2. **esp_lvgl_port** (2.7.x) — bridge, flush callback, tick, task loop
3. **esp_lcd** — MIPI-DSI controller, DMA, frame buffers
4. **BSP** — correct pin mappings, timing, EK79007 init

## LVGL Config
- `CONFIG_LV_USE_CLIB_MALLOC=y` — uses system heap (30MB PSRAM), NOT 64KB built-in pool
- `CONFIG_LV_FONT_MONTSERRAT_20=y` and `CONFIG_LV_FONT_MONTSERRAT_16=y`
- esp_lvgl_port >=2.7.0,<2.8.0 (2.8+ requires LVGL 9.3)
- Draw buffer: `BSP_LCD_H_RES * 50` in PSRAM
- Task capped at ~24fps (`task_max_sleep_ms = 42`)
- LVGL task stack: 16384 bytes (default 6KB causes stack overflow)

## Screen Transitions (CRITICAL)
**ALL transitions MUST use `lv_async_call()`** — never call show_xxx() from an event callback.
```c
static void back_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_target, NULL);
}
```
Direct transitions from callbacks cause LVGL infinite loops (object deleted while event processing).

## On-Screen Keyboard Pattern
- Show/hide via `LV_OBJ_FLAG_HIDDEN` — NEVER create/delete
- `osk_show(ta)` — show keyboard targeting textarea
- `osk_hide()` — hide keyboard
- `osk_cleanup()` — clear pointers before screen deletion
- Before navigating away: `lv_group_remove_obj(ta)` → `osk_cleanup()` → `lv_async_call(nav)`

## Image Display (Framebuffers)
**NEVER use `lv_canvas`** on ESP32-P4 MIPI-DSI. Always:
```c
// 1. Allocate DMA-aligned buffer
jpeg_decode_memory_alloc_cfg_t cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
size_t actual = 0;
uint8_t *fb = (uint8_t *)jpeg_alloc_decoder_mem(size, &cfg, &actual);

// 2. Setup descriptor
lv_image_dsc_t dsc = {
    .header.w = 1024, .header.h = 600,
    .header.cf = LV_COLOR_FORMAT_RGB565,
    .header.stride = 1024 * 2,
    .data_size = 1024 * 600 * 2,
    .data = fb,
};

// 3. Create lv_image widget
lv_obj_t *img = lv_image_create(screen);
lv_image_set_src(img, &dsc);

// 4. Refresh after buffer update
lv_image_cache_drop(&dsc);
lv_obj_invalidate(img);
```

## Hardware JPEG Decode
- Use `jpeg_decoder_get_info()` FIRST to get dimensions
- Align output: `aligned_w = (w + 15) & ~15`
- Use `jpeg_alloc_decoder_mem()` for DMA-aligned output
- Set `header.stride = aligned_w * 2` in lv_image_dsc_t
- HW decoder rejects images with dimensions not divisible by 8 in some chroma modes

## LVGL Group Rules
- Don't call `lv_group_add_obj` / `lv_group_remove_obj` dynamically during app lifetime
- Add textarea to group once at creation, remove once on exit
- Dangling group pointers cause infinite loops
