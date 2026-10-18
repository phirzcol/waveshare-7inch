#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GAME_INPUT_BTN_UP     = 1u << 0,
    GAME_INPUT_BTN_DOWN   = 1u << 1,
    GAME_INPUT_BTN_LEFT   = 1u << 2,
    GAME_INPUT_BTN_RIGHT  = 1u << 3,
    GAME_INPUT_BTN_SELECT = 1u << 4,
    GAME_INPUT_BTN_START  = 1u << 5,
    GAME_INPUT_BTN_A      = 1u << 6,
    GAME_INPUT_BTN_B      = 1u << 7,
    GAME_INPUT_BTN_X      = 1u << 8,
    GAME_INPUT_BTN_Y      = 1u << 9,
    GAME_INPUT_BTN_L      = 1u << 10,
    GAME_INPUT_BTN_R      = 1u << 11,
    GAME_INPUT_BTN_MENU   = 1u << 12,
} game_input_button_t;

typedef enum {
    GAME_INPUT_SOURCE_NONE = 0,
    GAME_INPUT_SOURCE_TOUCH,
    GAME_INPUT_SOURCE_HID,
    GAME_INPUT_SOURCE_XINPUT,
} game_input_source_t;

typedef struct {
    bool connected;
    bool hardware;
    game_input_source_t source;
    uint32_t buttons;
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
    uint16_t l2;
    uint16_t r2;
} game_input_state_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *buttons[13];
    uint32_t touch_buttons;
    bool enabled;
    bool force_visible;
} game_touch_overlay_t;

void game_input_read(game_input_state_t *state);
void game_input_read_with_overlay(game_input_state_t *state, const game_touch_overlay_t *overlay);
bool game_input_hardware_available(void);
const char *game_input_source_name(game_input_source_t source);

void game_touch_overlay_create(game_touch_overlay_t *overlay, lv_obj_t *parent);
void game_touch_overlay_destroy(game_touch_overlay_t *overlay);
void game_touch_overlay_set_enabled(game_touch_overlay_t *overlay, bool enabled);
void game_touch_overlay_set_force_visible(game_touch_overlay_t *overlay, bool visible);
void game_touch_overlay_update(game_touch_overlay_t *overlay);
uint32_t game_touch_overlay_buttons(const game_touch_overlay_t *overlay);

#ifdef __cplusplus
}
#endif
