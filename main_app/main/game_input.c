#include "game_input.h"

#include <string.h>

#include "esp_lvgl_port_usbhid.h"
#include "usb_device_monitor.h"

static const int16_t STICK_DPAD_THRESHOLD = 12000;

static uint32_t map_lvgl_gamepad_buttons(const lvgl_port_gamepad_state_t *gp)
{
    uint32_t buttons = 0;

    if (gp->dpad & LVGL_PORT_GAMEPAD_DPAD_UP) buttons |= GAME_INPUT_BTN_UP;
    if (gp->dpad & LVGL_PORT_GAMEPAD_DPAD_DOWN) buttons |= GAME_INPUT_BTN_DOWN;
    if (gp->dpad & LVGL_PORT_GAMEPAD_DPAD_LEFT) buttons |= GAME_INPUT_BTN_LEFT;
    if (gp->dpad & LVGL_PORT_GAMEPAD_DPAD_RIGHT) buttons |= GAME_INPUT_BTN_RIGHT;

    if (gp->axis_ly < -STICK_DPAD_THRESHOLD) buttons |= GAME_INPUT_BTN_UP;
    if (gp->axis_ly > STICK_DPAD_THRESHOLD) buttons |= GAME_INPUT_BTN_DOWN;
    if (gp->axis_lx < -STICK_DPAD_THRESHOLD) buttons |= GAME_INPUT_BTN_LEFT;
    if (gp->axis_lx > STICK_DPAD_THRESHOLD) buttons |= GAME_INPUT_BTN_RIGHT;

    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_A) buttons |= GAME_INPUT_BTN_A;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_B) buttons |= GAME_INPUT_BTN_B;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_X) buttons |= GAME_INPUT_BTN_X;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_Y) buttons |= GAME_INPUT_BTN_Y;
    if (gp->buttons & (LVGL_PORT_GAMEPAD_BTN_L1 | LVGL_PORT_GAMEPAD_BTN_L2)) buttons |= GAME_INPUT_BTN_L;
    if (gp->buttons & (LVGL_PORT_GAMEPAD_BTN_R1 | LVGL_PORT_GAMEPAD_BTN_R2)) buttons |= GAME_INPUT_BTN_R;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_SELECT) buttons |= GAME_INPUT_BTN_SELECT;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_START) buttons |= GAME_INPUT_BTN_START;
    if (gp->buttons & LVGL_PORT_GAMEPAD_BTN_HOME) buttons |= GAME_INPUT_BTN_MENU;

    return buttons;
}

static void fill_from_lvgl_gamepad(game_input_state_t *state,
                                   const lvgl_port_gamepad_state_t *gp,
                                   game_input_source_t source)
{
    state->connected = gp->connected != 0;
    state->hardware = gp->connected != 0;
    state->source = source;
    state->buttons = map_lvgl_gamepad_buttons(gp);
    state->lx = gp->axis_lx;
    state->ly = gp->axis_ly;
    state->rx = gp->axis_rx;
    state->ry = gp->axis_ry;
    state->l2 = gp->brake;
    state->r2 = gp->throttle;
}

void game_input_read(game_input_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));

    lvgl_port_gamepad_state_t gp = {0};
    if (usb_device_monitor_xinput_is_connected()) {
        usb_device_monitor_xinput_get_state(&gp);
        fill_from_lvgl_gamepad(state, &gp, GAME_INPUT_SOURCE_XINPUT);
        return;
    }

    lvgl_port_usb_gamepad_get_state(&gp);
    if (gp.connected) {
        fill_from_lvgl_gamepad(state, &gp, GAME_INPUT_SOURCE_HID);
    }
}

void game_input_read_with_overlay(game_input_state_t *state, const game_touch_overlay_t *overlay)
{
    if (!state) return;
    game_input_read(state);
    if (state->hardware) {
        return;
    }

    uint32_t touch_buttons = game_touch_overlay_buttons(overlay);
    if (touch_buttons) {
        state->connected = true;
        state->hardware = false;
        state->source = GAME_INPUT_SOURCE_TOUCH;
        state->buttons = touch_buttons;
    }
}

bool game_input_hardware_available(void)
{
    game_input_state_t state;
    game_input_read(&state);
    return state.hardware;
}

const char *game_input_source_name(game_input_source_t source)
{
    switch (source) {
    case GAME_INPUT_SOURCE_TOUCH:
        return "Touch";
    case GAME_INPUT_SOURCE_HID:
        return "HID";
    case GAME_INPUT_SOURCE_XINPUT:
        return "XInput";
    default:
        return "None";
    }
}

static void touch_button_event_cb(lv_event_t *event)
{
    game_touch_overlay_t *overlay = (game_touch_overlay_t *)lv_event_get_user_data(event);
    if (!overlay) return;

    uint32_t bit = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        overlay->touch_buttons |= bit;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        overlay->touch_buttons &= ~bit;
    }
}

static lv_obj_t *make_touch_button(lv_obj_t *parent,
                                   const char *text,
                                   uint32_t bit,
                                   int w,
                                   int h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2563eb), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_user_data(btn, (void *)(uintptr_t)bit);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return btn;
}

void game_touch_overlay_create(game_touch_overlay_t *overlay, lv_obj_t *parent)
{
    if (!overlay || !parent) return;
    memset(overlay, 0, sizeof(*overlay));
    overlay->enabled = true;

    lv_obj_t *root = lv_obj_create(parent);
    overlay->root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_align(root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);

    overlay->buttons[0] = make_touch_button(root, LV_SYMBOL_UP, GAME_INPUT_BTN_UP, 66, 66);
    overlay->buttons[1] = make_touch_button(root, LV_SYMBOL_DOWN, GAME_INPUT_BTN_DOWN, 66, 66);
    overlay->buttons[2] = make_touch_button(root, LV_SYMBOL_LEFT, GAME_INPUT_BTN_LEFT, 66, 66);
    overlay->buttons[3] = make_touch_button(root, LV_SYMBOL_RIGHT, GAME_INPUT_BTN_RIGHT, 66, 66);
    lv_obj_align(overlay->buttons[0], LV_ALIGN_BOTTOM_LEFT, 92, -190);
    lv_obj_align(overlay->buttons[1], LV_ALIGN_BOTTOM_LEFT, 92, -52);
    lv_obj_align(overlay->buttons[2], LV_ALIGN_BOTTOM_LEFT, 22, -120);
    lv_obj_align(overlay->buttons[3], LV_ALIGN_BOTTOM_LEFT, 162, -120);

    overlay->buttons[4] = make_touch_button(root, "Y", GAME_INPUT_BTN_Y, 70, 70);
    overlay->buttons[5] = make_touch_button(root, "X", GAME_INPUT_BTN_X, 70, 70);
    overlay->buttons[6] = make_touch_button(root, "B", GAME_INPUT_BTN_B, 70, 70);
    overlay->buttons[7] = make_touch_button(root, "A", GAME_INPUT_BTN_A, 70, 70);
    lv_obj_align(overlay->buttons[4], LV_ALIGN_BOTTOM_RIGHT, -174, -120);
    lv_obj_align(overlay->buttons[5], LV_ALIGN_BOTTOM_RIGHT, -98, -190);
    lv_obj_align(overlay->buttons[6], LV_ALIGN_BOTTOM_RIGHT, -98, -52);
    lv_obj_align(overlay->buttons[7], LV_ALIGN_BOTTOM_RIGHT, -22, -120);

    overlay->buttons[8] = make_touch_button(root, "L", GAME_INPUT_BTN_L, 92, 56);
    overlay->buttons[9] = make_touch_button(root, "R", GAME_INPUT_BTN_R, 92, 56);
    lv_obj_set_style_radius(overlay->buttons[8], 16, 0);
    lv_obj_set_style_radius(overlay->buttons[9], 16, 0);
    lv_obj_align(overlay->buttons[8], LV_ALIGN_TOP_LEFT, 28, 20);
    lv_obj_align(overlay->buttons[9], LV_ALIGN_TOP_RIGHT, -28, 20);

    overlay->buttons[10] = make_touch_button(root, "Select", GAME_INPUT_BTN_SELECT, 108, 46);
    overlay->buttons[11] = make_touch_button(root, "Start", GAME_INPUT_BTN_START, 108, 46);
    overlay->buttons[12] = make_touch_button(root, "Menu", GAME_INPUT_BTN_MENU, 108, 46);
    lv_obj_set_style_radius(overlay->buttons[10], 16, 0);
    lv_obj_set_style_radius(overlay->buttons[11], 16, 0);
    lv_obj_set_style_radius(overlay->buttons[12], 16, 0);
    lv_obj_align(overlay->buttons[10], LV_ALIGN_BOTTOM_MID, -126, -28);
    lv_obj_align(overlay->buttons[11], LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_align(overlay->buttons[12], LV_ALIGN_BOTTOM_MID, 126, -28);

    for (size_t i = 0; i < sizeof(overlay->buttons) / sizeof(overlay->buttons[0]); ++i) {
        if (!overlay->buttons[i]) continue;
        lv_obj_add_event_cb(overlay->buttons[i], touch_button_event_cb, LV_EVENT_PRESSED, overlay);
        lv_obj_add_event_cb(overlay->buttons[i], touch_button_event_cb, LV_EVENT_PRESSING, overlay);
        lv_obj_add_event_cb(overlay->buttons[i], touch_button_event_cb, LV_EVENT_RELEASED, overlay);
        lv_obj_add_event_cb(overlay->buttons[i], touch_button_event_cb, LV_EVENT_PRESS_LOST, overlay);
    }

    game_touch_overlay_update(overlay);
}

void game_touch_overlay_destroy(game_touch_overlay_t *overlay)
{
    if (!overlay) return;
    if (overlay->root && lv_obj_is_valid(overlay->root)) {
        lv_obj_delete(overlay->root);
    }
    memset(overlay, 0, sizeof(*overlay));
}

void game_touch_overlay_set_enabled(game_touch_overlay_t *overlay, bool enabled)
{
    if (!overlay) return;
    overlay->enabled = enabled;
    if (!enabled) overlay->touch_buttons = 0;
    game_touch_overlay_update(overlay);
}

void game_touch_overlay_set_force_visible(game_touch_overlay_t *overlay, bool visible)
{
    if (!overlay) return;
    overlay->force_visible = visible;
    game_touch_overlay_update(overlay);
}

void game_touch_overlay_update(game_touch_overlay_t *overlay)
{
    if (!overlay || !overlay->root || !lv_obj_is_valid(overlay->root)) return;

    bool show = overlay->enabled && (overlay->force_visible || !game_input_hardware_available());
    if (show) {
        lv_obj_clear_flag(overlay->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(overlay->root, LV_OBJ_FLAG_HIDDEN);
        overlay->touch_buttons = 0;
    }
}

uint32_t game_touch_overlay_buttons(const game_touch_overlay_t *overlay)
{
    if (!overlay || !overlay->enabled) return 0;
    if (game_input_hardware_available() && !overlay->force_visible) return 0;
    return overlay->touch_buttons;
}
