#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the VNC display (allocate DMA-aligned framebuffer, create lv_image).
 * Must be called after LVGL/display init (bsp_display_start_with_config).
 */
void vnc_display_init(void);

/**
 * @brief Handle an incoming rectangle (raw pixels or JPEG).
 * Called from the VNC protocol task. Writes to back buffer.
 */
void vnc_display_update_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint8_t *data, size_t len, bool is_jpeg);

/**
 * @brief Get the LVGL screen object for the VNC display.
 */
lv_obj_t *vnc_display_get_screen(void);

/**
 * @brief Swap back buffer to front and refresh display.
 * Call after a full frame update.
 */
void vnc_display_swap(void);
