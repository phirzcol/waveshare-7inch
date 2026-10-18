/**
 * @file onscreen_keyboard.c
 * @brief On-screen keyboard using LVGL lv_keyboard widget.
 *
 * Dark-styled keyboard overlay at the bottom of the screen.
 * Uses show/hide (LV_OBJ_FLAG_HIDDEN) instead of create/delete to avoid
 * use-after-free crashes when hiding from within the keyboard's own event.
 * The keyboard is created once per screen and destroyed automatically when
 * the parent screen is deleted.
 */

#include "onscreen_keyboard.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "OSK";

static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_target_ta = NULL;

#define OSK_HEIGHT  240
#define OSK_WIDTH   1024

static void osk_close_cb(lv_event_t *e)
{
    (void)e;
    osk_hide();
}

/**
 * Create the keyboard on the current screen (hidden by default).
 * If it already exists on this screen, does nothing.
 */
static void osk_ensure_created(void)
{
    // If keyboard exists and is still valid on the active screen, reuse it
    if (s_keyboard != NULL && lv_obj_is_valid(s_keyboard)) {
        return;
    }

    // Create keyboard on the active screen, start hidden
    s_keyboard = lv_keyboard_create(lv_scr_act());
    lv_obj_set_size(s_keyboard, OSK_WIDTH, OSK_HEIGHT);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Keyboard background — very dark
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_keyboard, 0, LV_PART_MAIN);

    // Key buttons
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x16213e), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(0xe0e0e0), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_keyboard, 6, LV_PART_ITEMS);

    // Close on READY (Enter) and CANCEL (close icon)
    lv_obj_add_event_cb(s_keyboard, osk_close_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, osk_close_cb, LV_EVENT_CANCEL, NULL);
}

void osk_show(lv_obj_t *target)
{
    if (target == NULL) return;

    s_target_ta = target;

    osk_ensure_created();

    // Assign textarea
    lv_keyboard_set_textarea(s_keyboard, target);

    // Move textarea above where the keyboard will be
    lv_obj_align(target, LV_ALIGN_BOTTOM_MID, 0, -(OSK_HEIGHT + 10));

    // Show keyboard
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Keyboard shown");
}

void osk_hide(void)
{
    if (s_keyboard == NULL) return;
    if (!lv_obj_is_valid(s_keyboard)) {
        // Parent screen was deleted, just clear our pointer
        s_keyboard = NULL;
        s_target_ta = NULL;
        return;
    }

    // Hide keyboard (don't delete it)
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, NULL);

    // Restore textarea position
    if (s_target_ta != NULL && lv_obj_is_valid(s_target_ta)) {
        lv_obj_align(s_target_ta, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    s_target_ta = NULL;

    ESP_LOGI(TAG, "Keyboard hidden");
}

bool osk_is_visible(void)
{
    if (s_keyboard == NULL) return false;
    if (!lv_obj_is_valid(s_keyboard)) return false;
    return !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void osk_set_target(lv_obj_t *target)
{
    if (target == NULL) return;
    s_target_ta = target;
    if (s_keyboard != NULL && lv_obj_is_valid(s_keyboard)) {
        lv_keyboard_set_textarea(s_keyboard, target);
    }
}

void osk_cleanup(void)
{
    // Called when navigating away from a screen.
    // The keyboard object will be deleted with its parent screen,
    // so just clear our pointers.
    s_keyboard = NULL;
    s_target_ta = NULL;
}
