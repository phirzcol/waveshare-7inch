// =============================================================================
// VNC Input — Mouse + Keyboard → VNC PointerEvent/KeyEvent
// =============================================================================

#include "vnc_input.h"
#include "vnc_rfb.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "VNC_INPUT";

static lv_obj_t *_osk = NULL;
static bool _osk_visible = false;
static int16_t _last_mouse_x = -1;
static int16_t _last_mouse_y = -1;
static uint8_t _last_buttons = 0;
static lv_timer_t *_mouse_timer = NULL;
static bool _vnc_active = false;
static lv_obj_t *_vnc_screen = NULL;

// Keysym conversion
static uint32_t special_to_keysym(const char *txt)
{
    if (!txt) return 0;
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) return 0xFF08;
    if (strcmp(txt, LV_SYMBOL_LEFT) == 0) return 0xFF51;
    if (strcmp(txt, LV_SYMBOL_RIGHT) == 0) return 0xFF53;
    if (strcmp(txt, LV_SYMBOL_UP) == 0) return 0xFF52;
    if (strcmp(txt, LV_SYMBOL_DOWN) == 0) return 0xFF54;
    if (strcmp(txt, "Enter") == 0 || strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) return 0xFF0D;
    if (strcmp(txt, " ") == 0) return 0x0020;
    if (strlen(txt) == 1 && txt[0] >= 0x20 && txt[0] <= 0x7E) return (uint32_t)txt[0];
    return 0;
}

static uint32_t lvgl_key_to_keysym(uint32_t key)
{
    switch (key) {
        case LV_KEY_ESC:       return 0xFF1B;
        case LV_KEY_DEL:       return 0xFFFF;
        case LV_KEY_BACKSPACE: return 0xFF08;
        case LV_KEY_ENTER:     return 0xFF0D;
        case LV_KEY_NEXT:      return 0xFF09;
        case LV_KEY_PREV:      return 0xFE20;
        case LV_KEY_UP:        return 0xFF52;
        case LV_KEY_DOWN:      return 0xFF54;
        case LV_KEY_RIGHT:     return 0xFF53;
        case LV_KEY_LEFT:      return 0xFF51;
        case LV_KEY_HOME:      return 0xFF50;
        case LV_KEY_END:       return 0xFF57;
        default: break;
    }
    if (key >= 0x20 && key <= 0x7E) return key;
    return 0;
}

// Mouse timer — polls LVGL cursor, sends VNC PointerEvent
static void mouse_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!_vnc_active) return;

    lv_indev_t *indev = NULL;
    lv_indev_t *iter = lv_indev_get_next(NULL);
    while (iter) {
        if (lv_indev_get_type(iter) == LV_INDEV_TYPE_POINTER) {
            indev = iter;
            break;
        }
        iter = lv_indev_get_next(iter);
    }
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_indev_state_t state = lv_indev_get_state(indev);

    int16_t x = (int16_t)point.x;
    int16_t y = (int16_t)point.y;
    if (x < 0) x = 0;
    if (x > 1023) x = 1023;
    if (y < 0) y = 0;
    if (y > 599) y = 599;

    uint8_t buttons = (state == LV_INDEV_STATE_PRESSED) ? 1 : 0;

    if (x != _last_mouse_x || y != _last_mouse_y || buttons != _last_buttons) {
        vnc_send_pointer((uint16_t)x, (uint16_t)y, buttons);
        _last_mouse_x = x;
        _last_mouse_y = y;
        _last_buttons = buttons;
    }
}

// Keyboard event — USB keyboard via LVGL group
static void vnc_screen_key_cb(lv_event_t *e)
{
    if (!_vnc_active) return;
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;

    uint32_t key = lv_event_get_key(e);
    uint32_t keysym = lvgl_key_to_keysym(key);
    if (keysym == 0) return;

    vnc_send_key(keysym, true);
    vnc_send_key(keysym, false);
}

// OSK callbacks
static void osk_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        uint32_t btn_id = lv_keyboard_get_selected_btn(kb);
        const char *txt = lv_keyboard_get_btn_text(kb, btn_id);
        if (!txt) return;
        uint32_t keysym = special_to_keysym(txt);
        if (keysym == 0) return;
        vnc_send_key(keysym, true);
        vnc_send_key(keysym, false);
    }
}

static void osk_close_cb(lv_event_t *e)
{
    (void)e;
    vnc_input_toggle_osk();
}

// Public API
void vnc_input_init(lv_obj_t *parent_scr)
{
    _vnc_screen = parent_scr;

    _osk = lv_keyboard_create(parent_scr);
    lv_obj_set_size(_osk, 1024, 240);
    lv_obj_align(_osk, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_osk, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(_osk, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_osk, 220, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_osk, lv_color_hex(0x16213e), LV_PART_ITEMS);
    lv_obj_set_style_text_color(_osk, lv_color_hex(0xe0e0e0), LV_PART_ITEMS);
    lv_obj_set_style_border_color(_osk, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_border_width(_osk, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(_osk, 6, LV_PART_ITEMS);

    lv_keyboard_set_textarea(_osk, NULL);
    lv_obj_add_event_cb(_osk, osk_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(_osk, osk_close_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(parent_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, parent_scr);
        lv_group_focus_obj(parent_scr);
    }
    lv_obj_add_event_cb(parent_scr, vnc_screen_key_cb, LV_EVENT_KEY, NULL);

    _mouse_timer = lv_timer_create(mouse_timer_cb, 20, NULL);
    _osk_visible = false;
    _vnc_active = false;
    ESP_LOGI(TAG, "VNC input initialized");
}

void vnc_input_toggle_osk(void)
{
    if (!_osk) return;
    _osk_visible = !_osk_visible;
    if (_osk_visible) lv_obj_clear_flag(_osk, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(_osk, LV_OBJ_FLAG_HIDDEN);
}

void vnc_input_set_active(bool active)
{
    _vnc_active = active;
    if (active) ESP_LOGI(TAG, "VNC input forwarding ACTIVE");
}
