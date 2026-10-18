// =============================================================================
// ESP32-P4 VNC Client — Settings Landing Page + VNC Connect
// =============================================================================
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_usbhid.h"

#include "vnc_rfb.h"
#include "vnc_display.h"
#include "vnc_input.h"

static const char *TAG = "VNC_APP";

// UI Colors (Material Dark)
#define COL_BG        0x121212
#define COL_SURFACE   0x1e1e2e
#define COL_PRIMARY   0xbb86fc
#define COL_TEXT      0xe0e0e0
#define COL_TEXT_SEC  0xa0a0a0
#define COL_DIVIDER   0x2c2c3e
#define COL_GREEN     0x03dac6
#define COL_ERROR     0xcf6679

// Settings state
static char s_wifi_ssid[33] = "";
static char s_wifi_pass[64] = "";
static char s_vnc_host[64]  = "";
static char s_vnc_port_str[8] = "5900";
static char s_vnc_pass[64]  = "";
static int s_encoding_pref = 0;  // 0=Tight, 1=ZRLE, 2=Raw (debug)
static int s_jpeg_quality = 5;  // 0-9
static int s_comp_level = 5;    // 1-9

// WiFi state
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_events = NULL;
static bool s_wifi_connected = false;
static char s_wifi_ip[20] = "";
static bool s_wifi_inited = false;
static lv_display_t *s_disp = NULL;

// UI state
static lv_obj_t *s_wifi_status_label = NULL;
static char *s_edit_dest = NULL;
static size_t s_edit_dest_size = 0;
static lv_obj_t *s_edit_ta = NULL;

static void show_settings_screen(void);

// =============================================================================
// NVS Load/Save
// =============================================================================
static void load_settings_from_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_wifi_ssid);
        nvs_get_str(nvs, "ssid", s_wifi_ssid, &len);
        len = sizeof(s_wifi_pass);
        nvs_get_str(nvs, "pass", s_wifi_pass, &len);
        nvs_close(nvs);
    }
    if (nvs_open("vnc", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_vnc_host);
        nvs_get_str(nvs, "host", s_vnc_host, &len);
        uint16_t port_val = 5900;
        if (nvs_get_u16(nvs, "port", &port_val) == ESP_OK)
            snprintf(s_vnc_port_str, sizeof(s_vnc_port_str), "%u", port_val);
        len = sizeof(s_vnc_pass);
        nvs_get_str(nvs, "vnc_pass", s_vnc_pass, &len);
        uint8_t enc_val = 0;
        if (nvs_get_u8(nvs, "encoding", &enc_val) == ESP_OK) s_encoding_pref = enc_val;
        uint8_t q_val = 5;
        if (nvs_get_u8(nvs, "jpeg_q", &q_val) == ESP_OK) s_jpeg_quality = q_val;
        uint8_t c_val = 5;
        if (nvs_get_u8(nvs, "comp_lvl", &c_val) == ESP_OK) s_comp_level = c_val;
        nvs_close(nvs);
    }
}

static void save_settings_to_nvs(void) {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", s_wifi_ssid);
        nvs_set_str(nvs, "pass", s_wifi_pass);
        nvs_commit(nvs); nvs_close(nvs);
    }
    if (nvs_open("vnc", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "host", s_vnc_host);
        nvs_set_u16(nvs, "port", (uint16_t)atoi(s_vnc_port_str));
        nvs_set_str(nvs, "vnc_pass", s_vnc_pass);
        nvs_set_u8(nvs, "encoding", (uint8_t)s_encoding_pref);
        nvs_set_u8(nvs, "jpeg_q", (uint8_t)s_jpeg_quality);
        nvs_set_u8(nvs, "comp_lvl", (uint8_t)s_comp_level);
        nvs_commit(nvs); nvs_close(nvs);
    }
}

// =============================================================================
// WiFi
// =============================================================================
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_once(void) {
    if (s_wifi_inited) return true;
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_inited = true;
    return true;
}

static bool wifi_connect(void) {
    if (s_wifi_ssid[0] == '\0') { ESP_LOGE(TAG, "No SSID"); return false; }
    if (!wifi_init_once()) return false;
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_wifi_pass, sizeof(wifi_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (int attempt = 0; attempt < 10; attempt++) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_connect();
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & WIFI_CONNECTED_BIT) { ESP_LOGI(TAG, "WiFi: %s", s_wifi_ip); return true; }
        ESP_LOGW(TAG, "WiFi attempt %d failed", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

// =============================================================================
// VNC Callback
// =============================================================================
static void on_vnc_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint8_t *data, size_t len, bool is_jpeg) {
    vnc_display_update_rect(x, y, w, h, data, len, is_jpeg);
}

// =============================================================================
// Text Entry Screen (for editing fields)
// =============================================================================
static void text_entry_save_cb(lv_event_t *e) {
    (void)e;
    const char *text = lv_textarea_get_text(s_edit_ta);
    if (s_edit_dest && text) { strncpy(s_edit_dest, text, s_edit_dest_size - 1); s_edit_dest[s_edit_dest_size - 1] = '\0'; }
    s_edit_dest = NULL; s_edit_ta = NULL;
    lv_async_call((lv_async_cb_t)show_settings_screen, NULL);
}
static void text_entry_back_cb(lv_event_t *e) {
    (void)e; s_edit_dest = NULL; s_edit_ta = NULL;
    lv_async_call((lv_async_cb_t)show_settings_screen, NULL);
}

static void show_text_entry_screen(const char *title, const char *current_val, char *dest_buf, size_t dest_size) {
    s_edit_dest = dest_buf; s_edit_dest_size = dest_size;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *tl = lv_label_create(scr); lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 20, 15);

    lv_obj_t *bb = lv_btn_create(scr); lv_obj_set_size(bb, 80, 40);
    lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, -100, 10);
    lv_obj_set_style_bg_color(bb, lv_color_hex(COL_SURFACE), 0);
    lv_obj_add_event_cb(bb, text_entry_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_TEXT), 0); lv_obj_center(bl);

    lv_obj_t *sb = lv_btn_create(scr); lv_obj_set_size(sb, 80, 40);
    lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(sb, lv_color_hex(COL_GREEN), 0);
    lv_obj_add_event_cb(sb, text_entry_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb); lv_label_set_text(sl, "Save");
    lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0); lv_obj_center(sl);

    s_edit_ta = lv_textarea_create(scr); lv_obj_set_size(s_edit_ta, 700, 50);
    lv_obj_align(s_edit_ta, LV_ALIGN_TOP_MID, 0, 65);
    lv_textarea_set_one_line(s_edit_ta, true);
    lv_textarea_set_text(s_edit_ta, current_val ? current_val : "");
    lv_obj_set_style_bg_color(s_edit_ta, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_text_color(s_edit_ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_color(s_edit_ta, lv_color_hex(COL_PRIMARY), 0);

    lv_obj_t *kb = lv_keyboard_create(scr); lv_keyboard_set_textarea(kb, s_edit_ta);
    lv_obj_set_size(kb, 1024, 350); lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_text_color(kb, lv_color_hex(COL_TEXT), 0);
    lv_scr_load(scr);
}

// =============================================================================
// Settings Screen Callbacks
// =============================================================================
static void ssid_tap_cb(lv_event_t *e) { (void)e; show_text_entry_screen("WiFi SSID", s_wifi_ssid, s_wifi_ssid, sizeof(s_wifi_ssid)); }
static void pass_tap_cb(lv_event_t *e) { (void)e; show_text_entry_screen("WiFi Password", s_wifi_pass, s_wifi_pass, sizeof(s_wifi_pass)); }
static void host_tap_cb(lv_event_t *e) { (void)e; show_text_entry_screen("VNC Server IP", s_vnc_host, s_vnc_host, sizeof(s_vnc_host)); }
static void port_tap_cb(lv_event_t *e) { (void)e; show_text_entry_screen("VNC Port", s_vnc_port_str, s_vnc_port_str, sizeof(s_vnc_port_str)); }
static void vnc_pass_tap_cb(lv_event_t *e) { (void)e; show_text_entry_screen("VNC Password (optional)", s_vnc_pass, s_vnc_pass, sizeof(s_vnc_pass)); }

static void encoding_tight_cb(lv_event_t *e) { (void)e; s_encoding_pref = 0; save_settings_to_nvs(); lv_async_call((lv_async_cb_t)show_settings_screen, NULL); }
static void encoding_zrle_cb(lv_event_t *e) { (void)e; s_encoding_pref = 1; save_settings_to_nvs(); lv_async_call((lv_async_cb_t)show_settings_screen, NULL); }
static void encoding_raw_cb(lv_event_t *e) { (void)e; s_encoding_pref = 2; save_settings_to_nvs(); lv_async_call((lv_async_cb_t)show_settings_screen, NULL); }

static void quality_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    s_jpeg_quality = lv_slider_get_value(slider);
    char buf[32]; snprintf(buf, sizeof(buf), "JPEG Quality: %d", s_jpeg_quality);
    lv_label_set_text(lbl, buf);
    save_settings_to_nvs();
}

static void compress_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    s_comp_level = lv_slider_get_value(slider);
    char buf[32]; snprintf(buf, sizeof(buf), "Compression: %d", s_comp_level);
    lv_label_set_text(lbl, buf);
    save_settings_to_nvs();
}

// =============================================================================
// Connect Task (runs on separate FreeRTOS task)
// =============================================================================
static void connect_task(void *arg) {
    (void)arg;
    // Update status
    if (bsp_display_lock(pdMS_TO_TICKS(200))) {
        if (s_wifi_status_label) { lv_label_set_text(s_wifi_status_label, "Connecting WiFi..."); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_TEXT_SEC), 0); }
        bsp_display_unlock();
    }
    if (!wifi_connect()) {
        if (bsp_display_lock(pdMS_TO_TICKS(200))) { if (s_wifi_status_label) { lv_label_set_text(s_wifi_status_label, "WiFi FAILED"); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_ERROR), 0); } bsp_display_unlock(); }
        vTaskDelete(NULL); return;
    }
    if (bsp_display_lock(pdMS_TO_TICKS(200))) {
        if (s_wifi_status_label) { char buf[48]; snprintf(buf, sizeof(buf), "Connected: %s", s_wifi_ip); lv_label_set_text(s_wifi_status_label, buf); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_GREEN), 0); }
        bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Init VNC display
    if (bsp_display_lock(0)) { vnc_display_init(); vnc_input_init(vnc_display_get_screen()); bsp_display_unlock(); }

    // Start VNC connection
    uint16_t port = (uint16_t)atoi(s_vnc_port_str);
    vnc_config_t vnc_cfg = { .host = s_vnc_host, .port = port, .password = (s_vnc_pass[0] ? s_vnc_pass : NULL), .on_rect = on_vnc_rect, .encoding_pref = (uint8_t)s_encoding_pref, .jpeg_quality = (uint8_t)s_jpeg_quality, .comp_level = (uint8_t)s_comp_level };
    vnc_client_start(&vnc_cfg);

    // Wait for connection
    for (int i = 0; i < 30; i++) { vTaskDelay(pdMS_TO_TICKS(500)); if (vnc_get_state() == VNC_STATE_CONNECTED || vnc_get_state() == VNC_STATE_ERROR) break; }

    if (vnc_get_state() == VNC_STATE_CONNECTED) {
        ESP_LOGI(TAG, "VNC connected!");
        if (bsp_display_lock(0)) {
            lv_scr_load(vnc_display_get_screen());
            vnc_input_set_active(true);
            bsp_display_unlock();
        }
    } else {
        if (bsp_display_lock(pdMS_TO_TICKS(200))) { if (s_wifi_status_label) { lv_label_set_text(s_wifi_status_label, "VNC FAILED"); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_ERROR), 0); } bsp_display_unlock(); }
    }
    vTaskDelete(NULL);
}

static void connect_btn_cb(lv_event_t *e) {
    (void)e;
    save_settings_to_nvs();
    xTaskCreatePinnedToCore(connect_task, "vnc_conn", 16384, NULL, 5, NULL, 0);
}

// =============================================================================
// Settings Screen
// =============================================================================
static lv_obj_t *create_field_row(lv_obj_t *parent, const char *label_text, const char *value_text, lv_event_cb_t tap_cb) {
    lv_obj_t *row = lv_obj_create(parent); lv_obj_set_size(row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SURFACE), 0); lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0); lv_obj_set_style_radius(row, 8, 0); lv_obj_set_style_pad_hor(row, 15, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lbl = lv_label_create(row); lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_SEC), 0); lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_t *val = lv_label_create(row); lv_label_set_text(val, (value_text && value_text[0]) ? value_text : "(not set)");
    lv_obj_set_style_text_color(val, lv_color_hex(COL_TEXT), 0); lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);
    if (tap_cb) { lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(row, tap_cb, LV_EVENT_CLICKED, NULL); }
    return val;
}

static void return_to_factory_app(void)
{
    ESP_LOGI(TAG, "Returning to main app (factory partition)...");

    const esp_partition_t *factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        ESP_LOGE(TAG, "Factory partition not found!");
        return;
    }

    esp_err_t ret = esp_ota_set_boot_partition(factory);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set factory boot partition: %s", esp_err_to_name(ret));
        return;
    }

    esp_restart();
}

static void switch_to_main_app_cb(lv_event_t *e)
{
    (void)e;
    return_to_factory_app();
}

static void show_settings_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  VNC Client");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // Content container
    lv_obj_t *cont = lv_obj_create(scr); lv_obj_set_size(cont, 700, 420);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 5, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 8, 0);

    // WiFi section
    lv_obj_t *wh = lv_label_create(cont); lv_label_set_text(wh, "WiFi");
    lv_obj_set_style_text_color(wh, lv_color_hex(COL_PRIMARY), 0);
    lv_obj_set_style_text_font(wh, &lv_font_montserrat_18, 0);
    create_field_row(cont, "SSID", s_wifi_ssid, ssid_tap_cb);
    char masked[64] = ""; size_t plen = strlen(s_wifi_pass);
    if (plen > 0) { memset(masked, '*', plen > 20 ? 20 : plen); masked[plen > 20 ? 20 : plen] = '\0'; }
    create_field_row(cont, "Password", masked, pass_tap_cb);

    // Divider
    lv_obj_t *dv = lv_obj_create(cont); lv_obj_set_size(dv, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(dv, lv_color_hex(COL_DIVIDER), 0);
    lv_obj_set_style_bg_opa(dv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dv, 0, 0); lv_obj_set_style_pad_all(dv, 0, 0);

    // VNC section
    lv_obj_t *vh = lv_label_create(cont); lv_label_set_text(vh, "VNC Server");
    lv_obj_set_style_text_color(vh, lv_color_hex(COL_PRIMARY), 0);
    lv_obj_set_style_text_font(vh, &lv_font_montserrat_18, 0);
    create_field_row(cont, "Server IP", s_vnc_host, host_tap_cb);
    create_field_row(cont, "Port", s_vnc_port_str, port_tap_cb);
    char vnc_masked[64] = ""; size_t vplen = strlen(s_vnc_pass);
    if (vplen > 0) { memset(vnc_masked, '*', vplen > 20 ? 20 : vplen); vnc_masked[vplen > 20 ? 20 : vplen] = '\0'; }
    create_field_row(cont, "Password", vplen > 0 ? vnc_masked : "(none)", vnc_pass_tap_cb);

    // Encoding selector
    lv_obj_t *enc_row = lv_obj_create(cont); lv_obj_set_size(enc_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(enc_row, lv_color_hex(COL_SURFACE), 0); lv_obj_set_style_bg_opa(enc_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(enc_row, 0, 0); lv_obj_set_style_radius(enc_row, 8, 0); lv_obj_set_style_pad_hor(enc_row, 15, 0);
    lv_obj_set_flex_flow(enc_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enc_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *enc_lbl = lv_label_create(enc_row); lv_label_set_text(enc_lbl, "Encoding");
    lv_obj_set_style_text_color(enc_lbl, lv_color_hex(COL_TEXT_SEC), 0); lv_obj_set_style_text_font(enc_lbl, &lv_font_montserrat_18, 0);
    lv_obj_t *enc_btns = lv_obj_create(enc_row); lv_obj_set_size(enc_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(enc_btns, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(enc_btns, 0, 0);
    lv_obj_set_style_pad_all(enc_btns, 0, 0); lv_obj_set_style_pad_column(enc_btns, 6, 0);
    lv_obj_set_flex_flow(enc_btns, LV_FLEX_FLOW_ROW);
    lv_obj_t *bt = lv_btn_create(enc_btns); lv_obj_set_size(bt, 110, 35); lv_obj_set_style_radius(bt, 6, 0);
    lv_obj_set_style_bg_color(bt, lv_color_hex(s_encoding_pref == 0 ? COL_PRIMARY : COL_SURFACE), 0);
    if (s_encoding_pref != 0) { lv_obj_set_style_border_color(bt, lv_color_hex(COL_DIVIDER), 0); lv_obj_set_style_border_width(bt, 1, 0); }
    lv_obj_add_event_cb(bt, encoding_tight_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btl = lv_label_create(bt); lv_label_set_text(btl, "Tight");
    lv_obj_set_style_text_color(btl, lv_color_hex(s_encoding_pref == 0 ? 0x000000 : COL_TEXT_SEC), 0); lv_obj_center(btl);
    lv_obj_t *bz = lv_btn_create(enc_btns); lv_obj_set_size(bz, 110, 35); lv_obj_set_style_radius(bz, 6, 0);
    lv_obj_set_style_bg_color(bz, lv_color_hex(s_encoding_pref == 1 ? COL_PRIMARY : COL_SURFACE), 0);
    if (s_encoding_pref != 1) { lv_obj_set_style_border_color(bz, lv_color_hex(COL_DIVIDER), 0); lv_obj_set_style_border_width(bz, 1, 0); }
    lv_obj_add_event_cb(bz, encoding_zrle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bzl = lv_label_create(bz); lv_label_set_text(bzl, "ZRLE");
    lv_obj_set_style_text_color(bzl, lv_color_hex(s_encoding_pref == 1 ? 0x000000 : COL_TEXT_SEC), 0); lv_obj_center(bzl);
    lv_obj_t *br = lv_btn_create(enc_btns); lv_obj_set_size(br, 90, 35); lv_obj_set_style_radius(br, 6, 0);
    lv_obj_set_style_bg_color(br, lv_color_hex(s_encoding_pref == 2 ? COL_ERROR : COL_SURFACE), 0);
    if (s_encoding_pref != 2) { lv_obj_set_style_border_color(br, lv_color_hex(COL_DIVIDER), 0); lv_obj_set_style_border_width(br, 1, 0); }
    lv_obj_add_event_cb(br, encoding_raw_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *brl = lv_label_create(br); lv_label_set_text(brl, "Raw");
    lv_obj_set_style_text_color(brl, lv_color_hex(s_encoding_pref == 2 ? 0x000000 : COL_TEXT_SEC), 0); lv_obj_center(brl);

    // Quality slider (Tight mode)
    char q_buf[32]; snprintf(q_buf, sizeof(q_buf), "JPEG Quality: %d", s_jpeg_quality);
    lv_obj_t *q_row = lv_obj_create(cont); lv_obj_set_size(q_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(q_row, lv_color_hex(COL_SURFACE), 0); lv_obj_set_style_bg_opa(q_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(q_row, 0, 0); lv_obj_set_style_radius(q_row, 8, 0); lv_obj_set_style_pad_hor(q_row, 15, 0);
    lv_obj_set_flex_flow(q_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(q_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *q_lbl = lv_label_create(q_row); lv_label_set_text(q_lbl, q_buf);
    lv_obj_set_style_text_color(q_lbl, lv_color_hex(COL_TEXT_SEC), 0); lv_obj_set_style_text_font(q_lbl, &lv_font_montserrat_16, 0);
    lv_obj_t *q_slider = lv_slider_create(q_row); lv_obj_set_size(q_slider, 200, 10);
    lv_slider_set_range(q_slider, 0, 9); lv_slider_set_value(q_slider, s_jpeg_quality, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(q_slider, lv_color_hex(COL_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(q_slider, lv_color_hex(COL_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(q_slider, lv_color_hex(COL_PRIMARY), LV_PART_KNOB);
    lv_obj_add_event_cb(q_slider, quality_slider_cb, LV_EVENT_VALUE_CHANGED, q_lbl);

    // Compression slider (Tight mode)
    char c_buf[32]; snprintf(c_buf, sizeof(c_buf), "Compression: %d", s_comp_level);
    lv_obj_t *c_row = lv_obj_create(cont); lv_obj_set_size(c_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(c_row, lv_color_hex(COL_SURFACE), 0); lv_obj_set_style_bg_opa(c_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c_row, 0, 0); lv_obj_set_style_radius(c_row, 8, 0); lv_obj_set_style_pad_hor(c_row, 15, 0);
    lv_obj_set_flex_flow(c_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *c_lbl = lv_label_create(c_row); lv_label_set_text(c_lbl, c_buf);
    lv_obj_set_style_text_color(c_lbl, lv_color_hex(COL_TEXT_SEC), 0); lv_obj_set_style_text_font(c_lbl, &lv_font_montserrat_16, 0);
    lv_obj_t *c_slider = lv_slider_create(c_row); lv_obj_set_size(c_slider, 200, 10);
    lv_slider_set_range(c_slider, 1, 9); lv_slider_set_value(c_slider, s_comp_level, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(c_slider, lv_color_hex(COL_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(c_slider, lv_color_hex(COL_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c_slider, lv_color_hex(COL_PRIMARY), LV_PART_KNOB);
    lv_obj_add_event_cb(c_slider, compress_slider_cb, LV_EVENT_VALUE_CHANGED, c_lbl);

    // Status label
    s_wifi_status_label = lv_label_create(cont);
    if (s_wifi_connected) { char buf[48]; snprintf(buf, sizeof(buf), "Connected: %s", s_wifi_ip); lv_label_set_text(s_wifi_status_label, buf); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_GREEN), 0); }
    else { lv_label_set_text(s_wifi_status_label, "Not connected"); lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(COL_TEXT_SEC), 0); }
    lv_obj_set_style_text_font(s_wifi_status_label, &lv_font_montserrat_16, 0);

    // Connect button (bottom-right)
    lv_obj_t *cb = lv_btn_create(scr); lv_obj_set_size(cb, 250, 50);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_RIGHT, -40, -20);
    lv_obj_set_style_bg_color(cb, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_radius(cb, 12, 0);
    lv_obj_add_event_cb(cb, connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cbl = lv_label_create(cb);
    lv_label_set_text(cbl, LV_SYMBOL_PLAY "  Connect");
    lv_obj_set_style_text_color(cbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(cbl, &lv_font_montserrat_20, 0);
    lv_obj_center(cbl);

    // Main App button (bottom-left) — switch to factory partition
    lv_obj_t *ma = lv_btn_create(scr); lv_obj_set_size(ma, 200, 50);
    lv_obj_align(ma, LV_ALIGN_BOTTOM_LEFT, 40, -20);
    lv_obj_set_style_bg_color(ma, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_border_color(ma, lv_color_hex(COL_DIVIDER), 0);
    lv_obj_set_style_border_width(ma, 1, 0);
    lv_obj_set_style_radius(ma, 12, 0);
    lv_obj_add_event_cb(ma, switch_to_main_app_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mal = lv_label_create(ma);
    lv_label_set_text(mal, LV_SYMBOL_HOME "  Main App");
    lv_obj_set_style_text_color(mal, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(mal, &lv_font_montserrat_18, 0);
    lv_obj_center(mal);

    lv_scr_load(scr);
}

// =============================================================================
// app_main
// =============================================================================
void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-P4 VNC Client (Settings) ===");

    // Mark this app as valid (prevents rollback on reboot — VNC persists until user taps Main App)
    esp_ota_mark_app_valid_cancel_rollback();

    // NVS init
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init(); }
    ESP_ERROR_CHECK(err);
    load_settings_from_nvs();

    // Display init
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 30,
        .double_buffer = false,
        .flags = { .buff_dma = false, .buff_spiram = true, .sw_rotate = false }
    };
    disp_cfg.lvgl_port_cfg.task_max_sleep_ms = 42;
    disp_cfg.lvgl_port_cfg.task_stack = 16384;
    s_disp = bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    // USB Host + HID
    esp_err_t usb_ret = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (usb_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB Host OK");
        lvgl_port_hid_mouse_cfg_t mouse_cfg = { .disp = s_disp, .sensitivity = 1 };
        lvgl_port_add_usb_hid_mouse_input(&mouse_cfg);
        lvgl_port_hid_keyboard_cfg_t kb_cfg = { .disp = s_disp };
        lv_indev_t *kb = lvgl_port_add_usb_hid_keyboard_input(&kb_cfg);
        if (kb) { lv_group_t *group = lv_group_create(); lv_group_set_default(group); lv_indev_set_group(kb, group); }
    }

    // Show settings screen
    bsp_display_lock(0);
    show_settings_screen();
    bsp_display_unlock();

    // Main loop — periodic diagnostics
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // Every 10 seconds

        // Memory stats
        size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t int_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "[MEM] Internal: %u free, %u min | PSRAM: %u free",
                 (unsigned)int_free, (unsigned)int_min, (unsigned)psram_free);

        // CPU idle time (via FreeRTOS runtime stats)
        static char stats_buf[512];
        vTaskGetRunTimeStats(stats_buf);
        // Print just the first 500 chars (task name + % columns)
        stats_buf[500] = '\0';
        ESP_LOGI(TAG, "[CPU]\n%s", stats_buf);
    }
}
