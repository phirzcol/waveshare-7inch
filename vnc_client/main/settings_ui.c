// =============================================================================
// ESP32-P4 VNC Client — Settings UI (Screen Layout & Widget Creation)
// =============================================================================
// Creates the LVGL settings screen with all input fields, buttons, and keyboard.
// Event handlers and validation are added in separate tasks (3.3, 3.5).
// =============================================================================

#include "settings_ui.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SETTINGS_UI";

// =============================================================================
// Theme Colors (Material Dark — VNC Settings variant)
// =============================================================================

#define SETTINGS_COLOR_BG        lv_color_hex(0x1E1E1E)
#define SETTINGS_COLOR_SURFACE   lv_color_hex(0x2D2D2D)
#define SETTINGS_COLOR_TEXT      lv_color_hex(0xFFFFFF)
#define SETTINGS_COLOR_TEXT_SEC  lv_color_hex(0xB0B0B0)
#define SETTINGS_COLOR_ERROR     lv_color_hex(0xFF5252)
#define SETTINGS_COLOR_PRIMARY   lv_color_hex(0x448AFF)

// Fonts
#define SETTINGS_FONT_LABEL   &lv_font_montserrat_20
#define SETTINGS_FONT_INPUT   &lv_font_montserrat_16
#define SETTINGS_FONT_TITLE   &lv_font_montserrat_24

// Layout
#define SETTINGS_PAD_OUTER    24
#define SETTINGS_PAD_ROW      14
#define SETTINGS_PAD_COL      10
#define SETTINGS_INPUT_HEIGHT 44
#define SETTINGS_BTN_HEIGHT   44

// =============================================================================
// Static Widget References
// =============================================================================

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_ssid_ta      = NULL;
static lv_obj_t *s_password_ta  = NULL;
static lv_obj_t *s_ip_ta        = NULL;
static lv_obj_t *s_port_ta      = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_scan_btn     = NULL;
static lv_obj_t *s_connect_btn  = NULL;
static lv_obj_t *s_clear_btn    = NULL;
static lv_obj_t *s_ap_list      = NULL;
static lv_obj_t *s_keyboard     = NULL;
static lv_obj_t *s_eye_btn      = NULL;

// =============================================================================
// Stored Callbacks (used by event handlers in task 3.5)
// =============================================================================

static settings_connect_cb_t s_on_connect = NULL;
static settings_scan_cb_t    s_on_scan    = NULL;
static settings_clear_cb_t   s_on_clear   = NULL;

// =============================================================================
// Forward Declarations (event handlers)
// =============================================================================

static void connect_btn_cb(lv_event_t *e);
static void scan_btn_cb(lv_event_t *e);
static void clear_btn_cb(lv_event_t *e);
static void eye_btn_cb(lv_event_t *e);
static void textarea_focus_cb(lv_event_t *e);
static void textarea_defocus_cb(lv_event_t *e);
static void ap_item_click_cb(lv_event_t *e);

// =============================================================================
// Helper: Style a textarea
// =============================================================================

static void style_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, SETTINGS_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, SETTINGS_FONT_INPUT, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ta, 8, LV_PART_MAIN);
    lv_textarea_set_one_line(ta, true);
}

// =============================================================================
// Helper: Create a styled button
// =============================================================================

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb, lv_color_t bg_color)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, SETTINGS_BTN_HEIGHT);
    lv_obj_set_style_bg_color(btn, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 20, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, SETTINGS_FONT_INPUT, LV_PART_MAIN);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    return btn;
}

// =============================================================================
// Helper: Create a form row (label + textarea, optionally + button)
// =============================================================================

static lv_obj_t *create_form_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, SETTINGS_PAD_COL, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// =============================================================================
// Public API: Create Settings Screen
// =============================================================================

lv_obj_t *settings_ui_create(const settings_ui_config_t *config)
{
    ESP_LOGI(TAG, "Creating settings UI screen");

    // Store callbacks for later use by event handlers
    if (config) {
        s_on_connect = config->on_connect;
        s_on_scan    = config->on_scan;
        s_on_clear   = config->on_clear;
    }

    // =========================================================================
    // Create a NEW screen (not using lv_scr_act() as parent)
    // =========================================================================
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, SETTINGS_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

    // =========================================================================
    // Content container — flex column with padding
    // =========================================================================
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 1024, 600);
    lv_obj_set_style_pad_all(content, SETTINGS_PAD_OUTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, SETTINGS_PAD_ROW, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content, SETTINGS_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);

    // =========================================================================
    // Title: "VNC Client Settings"
    // =========================================================================
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, "VNC Client Settings");
    lv_obj_set_style_text_color(title, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, SETTINGS_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(title, 8, LV_PART_MAIN);

    // =========================================================================
    // Form Row: WiFi SSID (label + textarea + Scan button)
    // =========================================================================
    lv_obj_t *ssid_label = lv_label_create(content);
    lv_label_set_text(ssid_label, "WiFi SSID");
    lv_obj_set_style_text_color(ssid_label, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ssid_label, SETTINGS_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *ssid_row = create_form_row(content);

    s_ssid_ta = lv_textarea_create(ssid_row);
    lv_obj_set_flex_grow(s_ssid_ta, 1);
    lv_obj_set_height(s_ssid_ta, SETTINGS_INPUT_HEIGHT);
    lv_textarea_set_max_length(s_ssid_ta, 32);
    lv_textarea_set_placeholder_text(s_ssid_ta, "Enter SSID or tap Scan");
    style_textarea(s_ssid_ta);
    lv_obj_add_event_cb(s_ssid_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ssid_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    s_scan_btn = create_button(ssid_row, "Scan", scan_btn_cb, SETTINGS_COLOR_PRIMARY);

    // =========================================================================
    // Form Row: Password (label + textarea + eye toggle)
    // =========================================================================
    lv_obj_t *pass_label = lv_label_create(content);
    lv_label_set_text(pass_label, "Password");
    lv_obj_set_style_text_color(pass_label, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(pass_label, SETTINGS_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *pass_row = create_form_row(content);

    s_password_ta = lv_textarea_create(pass_row);
    lv_obj_set_flex_grow(s_password_ta, 1);
    lv_obj_set_height(s_password_ta, SETTINGS_INPUT_HEIGHT);
    lv_textarea_set_max_length(s_password_ta, 63);
    lv_textarea_set_password_mode(s_password_ta, true);
    lv_textarea_set_password_show_time(s_password_ta, 0);
    lv_textarea_set_placeholder_text(s_password_ta, "Enter password");
    style_textarea(s_password_ta);
    lv_obj_add_event_cb(s_password_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_password_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    s_eye_btn = create_button(pass_row, LV_SYMBOL_EYE_CLOSE, eye_btn_cb,
                              SETTINGS_COLOR_SURFACE);

    // =========================================================================
    // Form Row: Server IP (label + textarea with placeholder)
    // =========================================================================
    lv_obj_t *ip_label = lv_label_create(content);
    lv_label_set_text(ip_label, "Server IP");
    lv_obj_set_style_text_color(ip_label, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ip_label, SETTINGS_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *ip_row = create_form_row(content);

    s_ip_ta = lv_textarea_create(ip_row);
    lv_obj_set_flex_grow(s_ip_ta, 1);
    lv_obj_set_height(s_ip_ta, SETTINGS_INPUT_HEIGHT);
    lv_textarea_set_max_length(s_ip_ta, 15);
    lv_textarea_set_placeholder_text(s_ip_ta, "192.168.0.100");
    style_textarea(s_ip_ta);
    lv_obj_add_event_cb(s_ip_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ip_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // =========================================================================
    // Form Row: Port (label + textarea with default "5900")
    // =========================================================================
    lv_obj_t *port_label = lv_label_create(content);
    lv_label_set_text(port_label, "Port");
    lv_obj_set_style_text_color(port_label, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(port_label, SETTINGS_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *port_row = create_form_row(content);

    s_port_ta = lv_textarea_create(port_row);
    lv_obj_set_flex_grow(s_port_ta, 1);
    lv_obj_set_height(s_port_ta, SETTINGS_INPUT_HEIGHT);
    lv_textarea_set_max_length(s_port_ta, 5);
    lv_textarea_set_text(s_port_ta, "5900");
    lv_textarea_set_placeholder_text(s_port_ta, "5900");
    style_textarea(s_port_ta);
    lv_obj_add_event_cb(s_port_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_port_ta, textarea_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    // =========================================================================
    // Status Label (connection status / errors)
    // =========================================================================
    s_status_label = lv_label_create(content);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, SETTINGS_COLOR_TEXT_SEC, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, SETTINGS_FONT_INPUT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_status_label, 4, LV_PART_MAIN);

    // =========================================================================
    // Button Row: Connect + Clear Saved
    // =========================================================================
    lv_obj_t *btn_row = lv_obj_create(content);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    s_connect_btn = create_button(btn_row, "Connect", connect_btn_cb,
                                  SETTINGS_COLOR_PRIMARY);

    s_clear_btn = create_button(btn_row, "Clear Saved", clear_btn_cb,
                                SETTINGS_COLOR_SURFACE);

    // =========================================================================
    // AP List (hidden by default, shown after scan)
    // =========================================================================
    s_ap_list = lv_list_create(content);
    lv_obj_set_size(s_ap_list, lv_pct(100), 200);
    lv_obj_set_style_bg_color(s_ap_list, SETTINGS_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ap_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ap_list, 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ap_list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ap_list, 4, LV_PART_MAIN);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);

    // =========================================================================
    // LVGL Keyboard (hidden by default, shown on textarea focus)
    // =========================================================================
    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_keyboard, 1024, 240);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_keyboard, SETTINGS_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_keyboard, SETTINGS_COLOR_TEXT, LV_PART_MAIN);

    ESP_LOGI(TAG, "Settings UI screen created successfully");
    return s_screen;
}

// =============================================================================
// Input Validation Functions
// =============================================================================

/**
 * @brief Validate an IPv4 address string.
 * Rules: exactly 4 octets separated by dots, each 0-255, no leading zeros
 * (except the value "0" itself), total length <= 15.
 */
static bool validate_ipv4(const char *ip)
{
    if (!ip || ip[0] == '\0') return false;

    // Check total length (max "255.255.255.255" = 15 chars)
    size_t len = 0;
    const char *p = ip;
    while (*p) {
        len++;
        if (len > 15) return false;
        p++;
    }

    int octets = 0;
    p = ip;

    while (octets < 4) {
        // Each octet must start with a digit
        if (*p < '0' || *p > '9') return false;

        // Parse the octet value
        int value = 0;
        int digits = 0;
        const char *octet_start = p;

        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            if (digits > 3) return false;  // Too many digits
            p++;
        }

        // Check for leading zeros: if more than 1 digit and starts with '0'
        if (digits > 1 && *octet_start == '0') return false;

        // Check octet range
        if (value > 255) return false;

        octets++;

        // After the first 3 octets, expect a dot
        if (octets < 4) {
            if (*p != '.') return false;
            p++;  // skip the dot
        }
    }

    // Must have consumed the entire string
    if (*p != '\0') return false;

    return (octets == 4);
}

/**
 * @brief Validate a port number string and output the parsed value.
 * Rules: only digits 0-9, value 1-65535, no leading zeros, length 1-5.
 */
static bool validate_port(const char *port_str, uint16_t *out_port)
{
    if (!port_str || port_str[0] == '\0') return false;

    // Check all characters are digits and count length
    size_t len = 0;
    const char *p = port_str;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        len++;
        if (len > 5) return false;
        p++;
    }

    // No leading zeros (unless the string is just "0", but 0 is invalid anyway)
    if (len > 1 && port_str[0] == '0') return false;

    // Parse the value
    unsigned long value = 0;
    for (size_t i = 0; i < len; i++) {
        value = value * 10 + (port_str[i] - '0');
    }

    // Range check: 1-65535
    if (value < 1 || value > 65535) return false;

    if (out_port) {
        *out_port = (uint16_t)value;
    }
    return true;
}

/**
 * @brief Validate a WiFi SSID string.
 * Rules: not empty, not whitespace-only (spaces, tabs, newlines, carriage returns).
 */
static bool validate_ssid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return false;

    // Check if all characters are whitespace
    const char *p = ssid;
    while (*p) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            return true;  // Found a non-whitespace character
        }
        p++;
    }

    return false;  // All characters were whitespace
}

/**
 * @brief Validate a WiFi password string.
 * Rules: 8-63 characters inclusive, all printable ASCII (codes 32-126).
 */
static bool validate_password(const char *password)
{
    if (!password) return false;

    size_t len = 0;
    const char *p = password;
    while (*p) {
        // Check printable ASCII range (32-126)
        if (*p < 32 || *p > 126) return false;
        len++;
        if (len > 63) return false;  // Too long
        p++;
    }

    // Check minimum length
    if (len < 8) return false;

    return true;
}

/**
 * @brief Convert RSSI to signal level (0-4 bars).
 * Thresholds: <= -80: 0, <= -70: 1, <= -60: 2, <= -50: 3, > -50: 4
 */
static uint8_t rssi_to_bars(int8_t rssi)
{
    if (rssi <= -80) return 0;
    if (rssi <= -70) return 1;
    if (rssi <= -60) return 2;
    if (rssi <= -50) return 3;
    return 4;
}

// =============================================================================
// Event Handlers
// =============================================================================

/**
 * @brief Connect button handler.
 * Validates all fields, then invokes the on_connect callback with populated params.
 */
static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Connect button pressed");

    // Get text from all textareas
    const char *ssid_text = lv_textarea_get_text(s_ssid_ta);
    const char *pass_text = lv_textarea_get_text(s_password_ta);
    const char *ip_text   = lv_textarea_get_text(s_ip_ta);
    const char *port_text = lv_textarea_get_text(s_port_ta);

    // Validate SSID: must not be empty or whitespace-only
    if (!validate_ssid(ssid_text)) {
        settings_ui_set_error("WiFi SSID is required");
        return;
    }

    // Validate password: only if non-empty (allow open networks)
    if (pass_text && pass_text[0] != '\0') {
        if (!validate_password(pass_text)) {
            settings_ui_set_error("Password must be at least 8 characters");
            return;
        }
    }

    // Validate IP address
    if (!validate_ipv4(ip_text)) {
        settings_ui_set_error("Invalid IP address format (e.g., 192.168.0.100)");
        return;
    }

    // Validate port
    uint16_t port_value = 0;
    if (!validate_port(port_text, &port_value)) {
        settings_ui_set_error("Port must be between 1 and 65535");
        return;
    }

    // All valid — populate settings_params_t and invoke callback
    if (s_on_connect) {
        settings_params_t params = {0};
        strncpy(params.ssid, ssid_text, sizeof(params.ssid) - 1);
        params.ssid[sizeof(params.ssid) - 1] = '\0';
        strncpy(params.password, pass_text ? pass_text : "", sizeof(params.password) - 1);
        params.password[sizeof(params.password) - 1] = '\0';
        strncpy(params.ip_addr, ip_text, sizeof(params.ip_addr) - 1);
        params.ip_addr[sizeof(params.ip_addr) - 1] = '\0';
        params.port = port_value;

        ESP_LOGI(TAG, "Invoking on_connect: SSID=%s, IP=%s, Port=%u",
                 params.ssid, params.ip_addr, params.port);
        s_on_connect(&params);
    }
}

/**
 * @brief Scan button handler. Invokes the on_scan callback.
 */
static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Scan button pressed");
    if (s_on_scan) {
        s_on_scan();
    }
}

/**
 * @brief Clear Saved button handler. Invokes the on_clear callback.
 */
static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Clear Saved button pressed");
    if (s_on_clear) {
        s_on_clear();
    }
}

/**
 * @brief Eye toggle handler.
 * Switches password textarea between password mode and plain text.
 */
static void eye_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Eye toggle pressed");

    if (!s_password_ta || !s_eye_btn) return;

    // Check current password mode and toggle it
    bool is_password_mode = lv_textarea_get_password_mode(s_password_ta);

    if (is_password_mode) {
        // Currently masked → show plain text
        lv_textarea_set_password_mode(s_password_ta, false);
        // Update button label to "eye open" (indicating password is visible)
        lv_obj_t *label = lv_obj_get_child(s_eye_btn, 0);
        if (label) {
            lv_label_set_text(label, LV_SYMBOL_EYE_OPEN);
        }
    } else {
        // Currently plain text → mask it
        lv_textarea_set_password_mode(s_password_ta, true);
        // Update button label to "eye close" (indicating password is hidden)
        lv_obj_t *label = lv_obj_get_child(s_eye_btn, 0);
        if (label) {
            lv_label_set_text(label, LV_SYMBOL_EYE_CLOSE);
        }
    }
}

/**
 * @brief Textarea focus handler.
 * Shows the keyboard and clears any status/error message when user starts editing.
 */
static void textarea_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_keyboard && ta) {
        lv_keyboard_set_textarea(s_keyboard, ta);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    // Clear any error/status when user starts editing a field
    settings_ui_clear_status();
}

/**
 * @brief Textarea defocus handler. Hides the keyboard.
 */
static void textarea_defocus_cb(lv_event_t *e)
{
    (void)e;
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief AP list item click handler.
 * Populates the SSID field with the clicked AP's SSID and hides the AP list.
 */
static void ap_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn) return;

    // Get the label child of the list button to extract the SSID text
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    const char *text = lv_label_get_text(label);
    if (!text) return;

    // The display text format is "SSID [****]" — extract just the SSID part
    // Find the last space before the signal indicator bracket
    char ssid_buf[33] = {0};
    const char *bracket = strrchr(text, '[');
    if (bracket && bracket > text) {
        // Copy everything before the space preceding the bracket
        size_t ssid_len = (size_t)(bracket - text);
        // Trim trailing space before the bracket
        while (ssid_len > 0 && text[ssid_len - 1] == ' ') {
            ssid_len--;
        }
        if (ssid_len > sizeof(ssid_buf) - 1) {
            ssid_len = sizeof(ssid_buf) - 1;
        }
        memcpy(ssid_buf, text, ssid_len);
        ssid_buf[ssid_len] = '\0';
    } else {
        // Fallback: use the full text if no bracket found
        strncpy(ssid_buf, text, sizeof(ssid_buf) - 1);
        ssid_buf[sizeof(ssid_buf) - 1] = '\0';
    }

    ESP_LOGI(TAG, "AP item tapped: %s", ssid_buf);

    // Set the SSID textarea with the selected AP name
    if (s_ssid_ta) {
        lv_textarea_set_text(s_ssid_ta, ssid_buf);
    }

    // Hide the AP list
    if (s_ap_list) {
        lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    }
}

// =============================================================================
// Public API: State Management Functions
// =============================================================================

void settings_ui_set_params(const settings_params_t *params)
{
    if (!params) return;

    if (s_ssid_ta && params->ssid[0]) {
        lv_textarea_set_text(s_ssid_ta, params->ssid);
    }
    if (s_password_ta && params->password[0]) {
        lv_textarea_set_text(s_password_ta, params->password);
    }
    if (s_ip_ta && params->ip_addr[0]) {
        lv_textarea_set_text(s_ip_ta, params->ip_addr);
    }
    if (s_port_ta && params->port > 0) {
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%u", params->port);
        lv_textarea_set_text(s_port_ta, port_str);
    }
}

void settings_ui_show_ap_list(const settings_ap_record_t *ap_records, uint8_t count)
{
    if (!s_ap_list) return;

    ESP_LOGI(TAG, "show_ap_list called with %d records", count);

    // Clear existing list items
    lv_obj_clean(s_ap_list);

    // Add each AP as a list button
    for (uint8_t i = 0; i < count; i++) {
        // Build display text with signal bars indicator
        uint8_t bars = rssi_to_bars(ap_records[i].rssi);
        char item_text[48];
        const char *signal_icons[] = {"[----]", "[*---]", "[**--]", "[***-]", "[****]"};
        snprintf(item_text, sizeof(item_text), "%s %s",
                 ap_records[i].ssid, signal_icons[bars]);

        lv_obj_t *btn = lv_list_add_button(s_ap_list, NULL, item_text);
        if (btn) {
            lv_obj_add_event_cb(btn, ap_item_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_text_color(btn, SETTINGS_COLOR_TEXT, LV_PART_MAIN);
        }
    }

    // Show the AP list
    lv_obj_clear_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
}

void settings_ui_set_status(const char *text)
{
    if (s_status_label && text) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, SETTINGS_COLOR_TEXT_SEC, LV_PART_MAIN);
    }
}

void settings_ui_set_error(const char *text)
{
    if (s_status_label && text) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label, SETTINGS_COLOR_ERROR, LV_PART_MAIN);
    }
}

void settings_ui_set_inputs_enabled(bool enabled)
{
    ESP_LOGI(TAG, "set_inputs_enabled(%d)", enabled);

    if (enabled) {
        // Clear LV_STATE_DISABLED from all input widgets and buttons
        if (s_ssid_ta)     lv_obj_clear_state(s_ssid_ta, LV_STATE_DISABLED);
        if (s_password_ta) lv_obj_clear_state(s_password_ta, LV_STATE_DISABLED);
        if (s_ip_ta)       lv_obj_clear_state(s_ip_ta, LV_STATE_DISABLED);
        if (s_port_ta)     lv_obj_clear_state(s_port_ta, LV_STATE_DISABLED);
        if (s_connect_btn) lv_obj_clear_state(s_connect_btn, LV_STATE_DISABLED);
        if (s_scan_btn)    lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
        if (s_clear_btn)   lv_obj_clear_state(s_clear_btn, LV_STATE_DISABLED);
    } else {
        // Add LV_STATE_DISABLED to all input widgets and buttons
        if (s_ssid_ta)     lv_obj_add_state(s_ssid_ta, LV_STATE_DISABLED);
        if (s_password_ta) lv_obj_add_state(s_password_ta, LV_STATE_DISABLED);
        if (s_ip_ta)       lv_obj_add_state(s_ip_ta, LV_STATE_DISABLED);
        if (s_port_ta)     lv_obj_add_state(s_port_ta, LV_STATE_DISABLED);
        if (s_connect_btn) lv_obj_add_state(s_connect_btn, LV_STATE_DISABLED);
        if (s_scan_btn)    lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
        if (s_clear_btn)   lv_obj_add_state(s_clear_btn, LV_STATE_DISABLED);
    }
}

void settings_ui_set_scanning(bool scanning)
{
    ESP_LOGI(TAG, "set_scanning(%d)", scanning);

    if (scanning) {
        // Disable the Scan button while scanning is in progress
        if (s_scan_btn) lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
        // Show scanning status indicator
        settings_ui_set_status("Scanning...");
    } else {
        // Re-enable the Scan button when scanning completes
        if (s_scan_btn) lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
        // Clear the scanning status
        settings_ui_clear_status();
    }
}

void settings_ui_clear_status(void)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, "");
    }
}
