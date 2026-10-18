#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief Initialize VNC input handling (OSK overlay + mouse forwarding).
 * Call after VNC display init, before VNC connection.
 */
void vnc_input_init(lv_obj_t *parent_scr);

/**
 * @brief Show/hide the OSK overlay for text input to VNC.
 */
void vnc_input_toggle_osk(void);

/**
 * @brief Enable/disable input forwarding to VNC server.
 * Call with true after VNC connection is established.
 */
void vnc_input_set_active(bool active);
