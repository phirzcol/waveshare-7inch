#pragma once

#include "lvgl.h"

/**
 * @brief Show the on-screen keyboard targeting a textarea.
 * @param target The lv_textarea to receive key input. Must not be NULL.
 */
void osk_show(lv_obj_t *target);

/**
 * @brief Hide the on-screen keyboard (stays in memory, just hidden).
 * No-op if not currently visible.
 */
void osk_hide(void);

/**
 * @brief Check if the keyboard is currently visible.
 */
bool osk_is_visible(void);

/**
 * @brief Change the target textarea without hiding/showing the keyboard.
 */
void osk_set_target(lv_obj_t *target);

/**
 * @brief Clear internal pointers when navigating away from a screen.
 * Call this before deleting the screen that contains the keyboard.
 * The keyboard object itself is destroyed with its parent screen.
 */
void osk_cleanup(void);
