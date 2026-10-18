/*
 * ESP32-P4 Tablet - Clean Build
 * Working base: WiFi + Audio + Touch + Display confirmed
 *
 * Structure:
 * - Desktop launcher with app icons
 * - Music player app (playlist, touch to switch tracks)
 * - WiFi settings app (status, connect/disconnect)
 * - File manager app (browse SD card, tap audio to play)
 */

#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "audio_player.h"
#include "driver/jpeg_decode.h"
#include "sdmmc_cmd.h"
#include "esp_lvgl_port_usbhid.h"
#include "web_server.h"
#include "settings_manager.h"
#include "onscreen_keyboard.h"
#include "text_editor.h"
#include "photo_viewer.h"
#include "video_player.h"
#include "usb_device_monitor.h"
#include "game_input.h"
#include "raw_sd_test.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "APP";

extern esp_err_t phirz_hosted_start(void);

// ============================================================================
// WiFi config
// ============================================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_events = NULL;
static bool s_wifi_initialized = false;
bool s_wifi_connected = false;
char s_wifi_ip[20] = "";

// ============================================================================
// Music config
// ============================================================================
#define MAX_TRACKS 64

char s_tracks[MAX_TRACKS][256];
static int s_track_count = 0;
int s_current_track = -1;
static bool s_audio_initialized = false;

// ============================================================================
// UI screens
// ============================================================================
static lv_obj_t *s_desktop_scr = NULL;
static lv_obj_t *s_music_scr = NULL;
static lv_obj_t *s_wifi_scr = NULL;
static lv_obj_t *s_files_scr = NULL;
static lv_obj_t *s_settings_scr = NULL;
static lv_obj_t *s_usb_scr = NULL;
static lv_obj_t *s_raw_sd_status_lbl = NULL;
static bool s_raw_sd_test_running = false;

// Stats overlay
static bool s_stats_visible = false;
static lv_obj_t *s_stats_bar = NULL;
static lv_obj_t *s_stats_label = NULL;

// FPS overlay
static bool s_fps_visible = false;
static lv_obj_t *s_fps_bar = NULL;
static lv_obj_t *s_fps_label = NULL;
static uint32_t s_fps_frame_count = 0;
static uint32_t s_fps_last_tick = 0;

// Desktop widgets
static lv_obj_t *s_desktop_status = NULL;

// Music widgets
static lv_obj_t *s_music_track_label = NULL;
static lv_obj_t *s_music_list = NULL;

// WiFi widgets
static lv_obj_t *s_wifi_status_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;
static lv_obj_t *s_wifi_btn = NULL;
static lv_obj_t *s_wifi_btn_label = NULL;

// File manager state
static char s_fm_path[512] = "/sdcard";

// ============================================================================
// Forward declarations
// ============================================================================
void show_desktop(void);
static void show_music(void);
static void show_wifi_settings(void);
static void show_settings(void);
static void show_files(const char *path);
static void files_back_async(void *arg);
static void add_bg_to_screen(lv_obj_t *scr);

// ============================================================================
// Playlist
// ============================================================================

static void build_playlist(const char *dir_path)
{
    s_track_count = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_track_count < MAX_TRACKS) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strlen(entry->d_name);
        if (len < 5) continue;
        const char *ext = &entry->d_name[len - 4];
        if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0) {
            snprintf(s_tracks[s_track_count], sizeof(s_tracks[0]),
                     "%s/%s", dir_path, entry->d_name);
            s_track_count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Playlist: %d tracks", s_track_count);
}

// Audio player callback — auto-advance to next track when current finishes
static void audio_player_cb(audio_player_cb_ctx_t *ctx)
{
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_IDLE) {
        // Track finished — advance to next, wrap to beginning
        if (s_current_track >= 0 && s_track_count > 0) {
            int next = s_current_track + 1;
            if (next >= s_track_count) next = 0;
            // Defer playback to avoid calling from audio task context
            s_current_track = next;
            bsp_extra_player_play_file(s_tracks[next]);
            ESP_LOGI(TAG, "Auto-advance to [%d]: %s", next, s_tracks[next]);
        }
    }
}

static void play_track(int index)
{
    if (index < 0 || index >= s_track_count) return;
    s_current_track = index;

    if (!s_audio_initialized) {
        bsp_extra_player_init();
        s_audio_initialized = true;
    }
    audio_player_callback_register(audio_player_cb, NULL);

    const char *path = s_tracks[index];
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    ESP_LOGI(TAG, "Playing [%d]: %s", index, fname);
    bsp_extra_player_play_file(path);

    if (s_music_track_label) {
        lv_label_set_text_fmt(s_music_track_label, LV_SYMBOL_PLAY " %s", fname);
    }
}

// ============================================================================
// WiFi
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        web_server_stop();
        s_wifi_ip[0] = '\0';
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        web_server_start();
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
    // TODO: When AP mode is implemented, handle WIFI_EVENT_AP_START here
    // to call web_server_start() so the settings page is served at 192.168.4.1.
    // Similarly, handle WIFI_EVENT_AP_STOP to call web_server_stop().
}

static void wifi_init_once(void)
{
    if (s_wifi_initialized) return;

    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(phirz_hosted_start());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_initialized = true;
}

static void wifi_connect_task(void *arg)
{
    wifi_init_once();

    // Give ESP-Hosted SDIO transport time to fully establish
    vTaskDelay(pdMS_TO_TICKS(2000));

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Read WiFi credentials from NVS (source of truth), fall back to hardcoded defines
    char nvs_ssid[33] = "";
    char nvs_pass[64] = "";
    const char *use_ssid = WIFI_SSID;
    const char *use_pass = WIFI_PASS;
    bool from_nvs = false;

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(nvs_ssid);
        size_t pass_len = sizeof(nvs_pass);
        if (nvs_get_str(nvs, "ssid", nvs_ssid, &ssid_len) == ESP_OK && nvs_ssid[0] != '\0') {
            use_ssid = nvs_ssid;
            from_nvs = true;
        }
        if (nvs_get_str(nvs, "pass", nvs_pass, &pass_len) == ESP_OK && nvs_pass[0] != '\0') {
            use_pass = nvs_pass;
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "WiFi connecting to SSID: %s (source: %s)",
             use_ssid, from_nvs ? "NVS" : "hardcoded");

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, use_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, use_pass, sizeof(wifi_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Try up to 2 attempts
    for (int attempt = 0; attempt < 2; attempt++) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(10000));

        if (bits & WIFI_CONNECTED_BIT) {
            bsp_display_lock(0);
            ESP_LOGI(TAG, "WiFi connected: %s", s_wifi_ip);
            if (s_wifi_status_label)
                lv_label_set_text(s_wifi_status_label, LV_SYMBOL_WIFI " Connected");
            if (s_wifi_ip_label)
                lv_label_set_text_fmt(s_wifi_ip_label, "IP: %s\nSSID: %s", s_wifi_ip, use_ssid);
            if (s_wifi_btn_label)
                lv_label_set_text(s_wifi_btn_label, "Disconnect");
            if (s_desktop_status)
                lv_label_set_text_fmt(s_desktop_status, LV_SYMBOL_WIFI " %s", s_wifi_ip);
            bsp_display_unlock();
            vTaskDelete(NULL);
            return;
        }

        // First attempt failed — wait and retry
        if (attempt == 0) {
            ESP_LOGW(TAG, "WiFi attempt %d failed, retrying...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    // Both attempts failed
    bsp_display_lock(0);
    ESP_LOGW(TAG, "WiFi connect failed after retries");
    if (s_wifi_status_label)
        lv_label_set_text(s_wifi_status_label, LV_SYMBOL_WARNING " Failed");
    if (s_wifi_ip_label)
        lv_label_set_text(s_wifi_ip_label, "Could not connect");
    if (s_wifi_btn_label)
        lv_label_set_text(s_wifi_btn_label, "Retry");
    bsp_display_unlock();

    vTaskDelete(NULL);
}

static void wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_wifi_connected = false;
    s_wifi_ip[0] = '\0';
}

// ============================================================================
// File Manager App
// ============================================================================

static void files_back_cb(lv_event_t *e)
{
    lv_async_call((lv_async_cb_t)files_back_async, NULL);
}

static char s_fm_pending_path[512] = "";

static void files_item_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", s_fm_path, name);

    // Check if it's an audio file — play it immediately
    size_t len = strlen(name);
    if (len > 4) {
        const char *ext = &name[len - 4];
        if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0) {
            if (!s_audio_initialized) {
                bsp_extra_player_init();
                s_audio_initialized = true;
            }
            audio_player_callback_register(audio_player_cb, NULL);
            ESP_LOGI(TAG, "Playing: %s", full_path);

            // Rebuild playlist from this file's directory so music player stays in sync
            build_playlist(s_fm_path);
            // Find this file in the new playlist and set current track
            s_current_track = -1;
            for (int i = 0; i < s_track_count; i++) {
                if (strcmp(s_tracks[i], full_path) == 0) {
                    s_current_track = i;
                    break;
                }
            }

            bsp_extra_player_play_file(full_path);

            // Open the music player screen
            lv_async_call((lv_async_cb_t)show_music, NULL);
            return;
        }
    }

    // Check if it's a plaintext/code file — open in editor
    if (len > 2) {
        const char *dot = strrchr(name, '.');
        if (dot) {
            dot++;
            if (strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "c") == 0 ||
                strcasecmp(dot, "h") == 0 || strcasecmp(dot, "md") == 0 ||
                strcasecmp(dot, "py") == 0 || strcasecmp(dot, "js") == 0 ||
                strcasecmp(dot, "json") == 0 || strcasecmp(dot, "log") == 0 ||
                strcasecmp(dot, "ino") == 0 || strcasecmp(dot, "cfg") == 0) {
                ESP_LOGI(TAG, "Opening in editor: %s", full_path);
                bsp_display_lock(0);
                text_editor_open(full_path);
                bsp_display_unlock();
                return;
            }
            // Check if it's a JPEG — open in photo viewer
            if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) {
                ESP_LOGI(TAG, "Opening in photos: %s", full_path);
                bsp_display_lock(0);
                photo_viewer_open(full_path);
                bsp_display_unlock();
                return;
            }
            // Check if it's a raw MJPEG video stream - open in video player
            if (strcasecmp(dot, "mjpg") == 0 || strcasecmp(dot, "mjpeg") == 0) {
                ESP_LOGI(TAG, "Opening in videos: %s", full_path);
                bsp_display_lock(0);
                video_player_open(full_path);
                bsp_display_unlock();
                return;
            }
        }
    }

    // For directories, defer navigation (can't delete screen inside event)
    snprintf(s_fm_pending_path, sizeof(s_fm_pending_path), "%s", full_path);
    lv_async_call((lv_async_cb_t)show_files, s_fm_pending_path);
}

static void files_back_async(void *arg)
{
    char *last_slash = strrchr(s_fm_path, '/');
    if (last_slash && last_slash != s_fm_path && strcmp(s_fm_path, "/sdcard") != 0) {
        *last_slash = '\0';
        show_files(s_fm_path);
    } else {
        show_desktop();
    }
}

// Static storage for file names (needed for event user_data pointers)
#define FM_MAX_ITEMS 256
static char (*s_fm_names)[128] = NULL;  // Allocated from PSRAM
static bool *s_fm_is_dir = NULL;        // Allocated from PSRAM
static int s_fm_count = 0;

// Pre-scan directory into PSRAM table
static void fm_scan_dir(const char *path)
{
    // Allocate on first use
    if (!s_fm_names) {
        s_fm_names = heap_caps_malloc(FM_MAX_ITEMS * 128, MALLOC_CAP_SPIRAM);
        s_fm_is_dir = heap_caps_malloc(FM_MAX_ITEMS * sizeof(bool), MALLOC_CAP_SPIRAM);
    }
    s_fm_count = 0;

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_fm_count < FM_MAX_ITEMS) {
        if (entry->d_name[0] == '.') continue;

        strncpy(s_fm_names[s_fm_count], entry->d_name, 127);
        s_fm_names[s_fm_count][127] = '\0';

        // Check if directory
        char check_path[512];
        snprintf(check_path, sizeof(check_path), "%s/%s", path, entry->d_name);
        struct stat fst;
        s_fm_is_dir[s_fm_count] = (stat(check_path, &fst) == 0 && S_ISDIR(fst.st_mode));

        s_fm_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Scanned %s: %d items", path, s_fm_count);
}

static void show_files(const char *path)
{
    // Scan directory into PSRAM first (before touching LVGL)
    snprintf(s_fm_path, sizeof(s_fm_path), "%s", path);
    fm_scan_dir(path);

    bsp_display_lock(0);

    if (s_files_scr) { lv_obj_del(s_files_scr); s_files_scr = NULL; }

    s_files_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_files_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_files_scr, LV_OPA_COVER, 0);
    add_bg_to_screen(s_files_scr);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_files_scr);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, files_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    // Path label
    lv_obj_t *path_lbl = lv_label_create(s_files_scr);
    lv_obj_set_style_text_color(path_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(path_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_width(path_lbl, 800);
    lv_label_set_long_mode(path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(path_lbl, LV_ALIGN_TOP_LEFT, 100, 15);
    lv_label_set_text(path_lbl, path);

    // File list — use simple labels in a scrollable container (lighter than lv_list)
    lv_obj_t *list = lv_obj_create(s_files_scr);
    lv_obj_set_size(list, 1004, 530);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(list, 180, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_fm_count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_label_set_text(empty, "Empty folder");
    } else {
        for (int i = 0; i < s_fm_count; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), 44);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x0f3460), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 6, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_left(row, 12, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, files_item_cb, LV_EVENT_CLICKED,
                               (void *)s_fm_names[i]);

            lv_obj_t *icon = lv_label_create(row);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x00ff88), 0);
            lv_label_set_text(icon, s_fm_is_dir[i] ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);

            lv_obj_t *name_lbl = lv_label_create(row);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xe0e0e0), 0);
            lv_label_set_text(name_lbl, s_fm_names[i]);
        }
    }

    lv_scr_load(s_files_scr);
    bsp_display_unlock();
}

// ============================================================================
// Desktop
// ============================================================================

static void music_icon_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_music, NULL);
}
static void files_icon_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_files, (void *)"/sdcard");
}
static void editor_icon_cb(lv_event_t *e) {
    (void)e;
    bsp_display_lock(0);
    text_editor_show();
    bsp_display_unlock();
}
static void photos_icon_cb(lv_event_t *e) {
    (void)e;
    bsp_display_lock(0);
    photo_viewer_show();
    bsp_display_unlock();
}
static void video_icon_cb(lv_event_t *e) {
    (void)e;
    video_player_show();
}
static void settings_icon_cb(lv_event_t *e);  // forward declare
static void usb_icon_cb(lv_event_t *e);  // forward declare

typedef enum {
    BOOT_APP_RETURN_PERSIST,
    BOOT_APP_RETURN_ROLLBACK,
} boot_app_return_policy_t;

typedef struct {
    const char *name;
    const char *icon;
    const char *label;
    esp_partition_subtype_t partition_subtype;
    boot_app_return_policy_t return_policy;
} boot_app_entry_t;

static const boot_app_entry_t s_boot_apps[] = {
    {
        .name = "VNC client",
        .icon = LV_SYMBOL_WIFI,
        .label = "VNC",
        .partition_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
        .return_policy = BOOT_APP_RETURN_PERSIST,
    },
    {
        .name = "USB Display",
        .icon = LV_SYMBOL_USB,
        .label = "Display",
        .partition_subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
        .return_policy = BOOT_APP_RETURN_ROLLBACK,
    },
};

static void launch_boot_app(const boot_app_entry_t *app)
{
    if (!app) {
        return;
    }

    ESP_LOGI(TAG, "Launching %s (%s)",
             app->name,
             app->return_policy == BOOT_APP_RETURN_ROLLBACK ? "rollback" : "persistent");

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        app->partition_subtype,
        NULL);

    if (!partition) {
        ESP_LOGE(TAG, "%s partition not found (subtype=0x%02x)",
                 app->name,
                 app->partition_subtype);
        return;
    }

    esp_err_t ret = esp_ota_set_boot_partition(partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set %s boot partition: %s", app->name, esp_err_to_name(ret));
        return;
    }

    esp_restart();
}

static void boot_app_icon_cb(lv_event_t *e)
{
    launch_boot_app((const boot_app_entry_t *)lv_event_get_user_data(e));
}

// Helper: add background image to a screen (if decoded)
static lv_image_dsc_t bg_dsc = {0};
static uint8_t *bg_decoded = NULL;

static void add_bg_to_screen(lv_obj_t *scr)
{
    if (bg_decoded) {
        lv_obj_t *bg_img = lv_image_create(scr);
        lv_image_set_src(bg_img, &bg_dsc);
        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    }
}

static lv_obj_t *create_app_icon_ex(lv_obj_t *parent, const char *symbol,
                                     const char *label_text, lv_event_cb_t cb,
                                     void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 110, 130);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 10, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x00ff88), 0);
    lv_label_set_text(icon, symbol);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(lbl, label_text);

    return btn;
}

static lv_obj_t *create_app_icon(lv_obj_t *parent, const char *symbol,
                                  const char *label_text, lv_event_cb_t cb)
{
    return create_app_icon_ex(parent, symbol, label_text, cb, NULL);
}

void show_desktop(void)
{
    video_player_stop();

    bsp_display_lock(0);

    if (s_music_scr) { lv_obj_del(s_music_scr); s_music_scr = NULL; }
    if (s_wifi_scr) { lv_obj_del(s_wifi_scr); s_wifi_scr = NULL; }
    if (s_files_scr) { lv_obj_del(s_files_scr); s_files_scr = NULL; }
    if (s_settings_scr) { lv_obj_del(s_settings_scr); s_settings_scr = NULL; s_raw_sd_status_lbl = NULL; }
    if (s_usb_scr) { lv_obj_del(s_usb_scr); s_usb_scr = NULL; }

    s_desktop_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_desktop_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_desktop_scr, LV_OPA_COVER, 0);

    // Background image from SD card using hardware JPEG decoder
    static bool bg_loaded = false;

    if (!bg_loaded) {
        // Find a random JPEG in /sdcard/h/
        char jpeg_path[128] = "";
        DIR *bgdir = opendir("/sdcard/h");
        if (bgdir) {
            // Count JPEGs first
            int count = 0;
            struct dirent *ent;
            while ((ent = readdir(bgdir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                size_t len = strlen(ent->d_name);
                if (len > 4 && strcasecmp(&ent->d_name[len-4], ".jpg") == 0) {
                    count++;
                }
            }
            // Pick random one using boot time as seed
            if (count > 0) {
                int pick = (int)(esp_log_timestamp() % count);
                rewinddir(bgdir);
                int idx = 0;
                while ((ent = readdir(bgdir)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    size_t len = strlen(ent->d_name);
                    if (len > 4 && strcasecmp(&ent->d_name[len-4], ".jpg") == 0) {
                        if (idx == pick) {
                            snprintf(jpeg_path, sizeof(jpeg_path), "/sdcard/h/%s", ent->d_name);
                            break;
                        }
                        idx++;
                    }
                }
            }
            closedir(bgdir);
        }

        if (jpeg_path[0]) {
            // Read JPEG file into PSRAM
            FILE *jf = fopen(jpeg_path, "rb");
            if (jf) {
                fseek(jf, 0, SEEK_END);
                size_t jsize = ftell(jf);
                fseek(jf, 0, SEEK_SET);

                // Allocate input buffer (JPEG compressed data)
                uint8_t *jbuf = heap_caps_malloc(jsize, MALLOC_CAP_SPIRAM);
                if (jbuf) {
                    fread(jbuf, 1, jsize, jf);
                    fclose(jf);

                    // Hardware JPEG decode
                    jpeg_decoder_handle_t hw_jpeg = NULL;
                    jpeg_decode_engine_cfg_t engine_cfg = {
                        .intr_priority = 0,
                        .timeout_ms = 1000,
                    };

                    if (jpeg_new_decoder_engine(&engine_cfg, &hw_jpeg) == ESP_OK) {
                        // Output buffer: 1024*608*2 (height aligned to 16 pixels)
                        size_t out_size = 1024 * 608 * 2;
                        jpeg_decode_memory_alloc_cfg_t rx_cfg = {
                            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
                        };
                        size_t actual_out_size = 0;
                        bg_decoded = (uint8_t *)jpeg_alloc_decoder_mem(out_size, &rx_cfg, &actual_out_size);

                        if (bg_decoded) {
                            jpeg_decode_cfg_t decode_cfg = {
                                .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                            };
                            uint32_t decoded_size = actual_out_size;
                            esp_err_t dec_ret = jpeg_decoder_process(hw_jpeg, &decode_cfg,
                                jbuf, jsize, bg_decoded, actual_out_size, &decoded_size);

                            if (dec_ret == ESP_OK) {
                                ESP_LOGI(TAG, "HW JPEG decoded: %s (%u bytes -> %u bytes RGB565)",
                                         jpeg_path, (unsigned)jsize, (unsigned)decoded_size);

                                // Setup LVGL image descriptor
                                bg_dsc.header.w = 1024;
                                bg_dsc.header.h = 600;
                                bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                                bg_dsc.header.stride = 1024 * 2;
                                bg_dsc.data_size = 1024 * 600 * 2;
                                bg_dsc.data = bg_decoded;
                                bg_loaded = true;
                            } else {
                                ESP_LOGE(TAG, "HW JPEG decode failed: %s", esp_err_to_name(dec_ret));
                                free(bg_decoded);
                                bg_decoded = NULL;
                            }
                        }
                        jpeg_del_decoder_engine(hw_jpeg);
                    }
                    free(jbuf);
                } else {
                    fclose(jf);
                }
            }
        }
    }

    add_bg_to_screen(s_desktop_scr);

    // Status bar
    s_desktop_status = lv_label_create(s_desktop_scr);
    lv_obj_set_style_text_color(s_desktop_status, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_desktop_status, &lv_font_montserrat_20, 0);
    lv_obj_align(s_desktop_status, LV_ALIGN_TOP_RIGHT, -10, 8);
    if (s_wifi_connected) {
        lv_label_set_text_fmt(s_desktop_status, LV_SYMBOL_WIFI " %s", s_wifi_ip);
    } else {
        lv_label_set_text(s_desktop_status, "No WiFi");
    }

    // Memory info
    lv_obj_t *mem_lbl = lv_label_create(s_desktop_scr);
    lv_obj_set_style_text_color(mem_lbl, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(mem_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(mem_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024);
    lv_label_set_text_fmt(mem_lbl, "Int: %uKB | PSRAM: %uMB", (unsigned)free_int, (unsigned)free_ps);

    // App icons container
    lv_obj_t *grid = lv_obj_create(s_desktop_scr);
    lv_obj_set_size(grid, 960, 200);
    lv_obj_center(grid);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_shadow_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, 20, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    create_app_icon(grid, LV_SYMBOL_AUDIO, "Music", music_icon_cb);
    create_app_icon(grid, LV_SYMBOL_PLAY, "Videos", video_icon_cb);
    create_app_icon(grid, LV_SYMBOL_EDIT, "Editor", editor_icon_cb);
    create_app_icon(grid, LV_SYMBOL_IMAGE, "Photos", photos_icon_cb);
    create_app_icon(grid, LV_SYMBOL_DIRECTORY, "Files", files_icon_cb);
    create_app_icon(grid, LV_SYMBOL_SETTINGS, "Settings", settings_icon_cb);
    for (size_t i = 0; i < sizeof(s_boot_apps) / sizeof(s_boot_apps[0]); ++i) {
        create_app_icon_ex(grid,
                           s_boot_apps[i].icon,
                           s_boot_apps[i].label,
                           boot_app_icon_cb,
                           (void *)&s_boot_apps[i]);
    }

    lv_scr_load(s_desktop_scr);
    bsp_display_unlock();
}

// ============================================================================
// Music App
// ============================================================================

static void music_track_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    play_track(index);
}

static bool s_muted = false;
static int s_volume = 60;
static lv_obj_t *s_time_label = NULL;

static void music_prev_cb(lv_event_t *e) {
    if (s_track_count > 0) {
        int prev = s_current_track - 1;
        if (prev < 0) prev = s_track_count - 1;
        play_track(prev);
    }
}
static void music_next_cb(lv_event_t *e) {
    if (s_track_count > 0) {
        int next = s_current_track + 1;
        if (next >= s_track_count) next = 0;
        play_track(next);
    }
}
static void music_pause_cb(lv_event_t *e) {
    if (audio_player_get_state() == AUDIO_PLAYER_STATE_PLAYING) {
        audio_player_pause();
    } else if (audio_player_get_state() == AUDIO_PLAYER_STATE_PAUSE) {
        audio_player_resume();
    }
}

static void music_seek_back_cb(lv_event_t *e) {
    if (s_current_track >= 0) play_track(s_current_track);
}

static void music_seek_fwd_cb(lv_event_t *e) {
    if (s_current_track < s_track_count - 1) play_track(s_current_track + 1);
}
static void music_vol_up_cb(lv_event_t *e) {
    s_volume += 10;
    if (s_volume > 100) s_volume = 100;
    settings_set_volume(s_volume);  // Persists to NVS + applies to codec
}
static void music_vol_dn_cb(lv_event_t *e) {
    s_volume -= 10;
    if (s_volume < 0) s_volume = 0;
    settings_set_volume(s_volume);  // Persists to NVS + applies to codec
}

static void music_back_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_desktop, NULL);
}

static void show_music(void)
{
    bsp_display_lock(0);

    s_music_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_music_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_music_scr, LV_OPA_COVER, 0);
    add_bg_to_screen(s_music_scr);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_music_scr);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, music_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    // Title
    lv_obj_t *title = lv_label_create(s_music_scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    lv_label_set_text(title, "Music Player");

    // Now playing
    s_music_track_label = lv_label_create(s_music_scr);
    lv_obj_set_style_text_color(s_music_track_label, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(s_music_track_label, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_music_track_label, 700);
    lv_label_set_long_mode(s_music_track_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_music_track_label, LV_ALIGN_TOP_LEFT, 10, 55);
    if (s_current_track >= 0) {
        const char *fname = strrchr(s_tracks[s_current_track], '/');
        fname = fname ? fname + 1 : s_tracks[s_current_track];
        lv_label_set_text_fmt(s_music_track_label, LV_SYMBOL_PLAY " %s", fname);
    } else {
        lv_label_set_text(s_music_track_label, "No track playing");
    }

    // Transport controls
    lv_obj_t *ctrl_row = lv_obj_create(s_music_scr);
    lv_obj_set_size(ctrl_row, 700, 50);
    lv_obj_align(ctrl_row, LV_ALIGN_TOP_LEFT, 10, 85);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 6, 0);

    // Helper to create a control button
    #define MAKE_CTRL_BTN(parent, symbol, cb) do { \
        lv_obj_t *b = lv_btn_create(parent); \
        lv_obj_set_size(b, 55, 40); \
        lv_obj_set_style_bg_color(b, lv_color_hex(0x16213e), 0); \
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL); \
        lv_obj_t *l = lv_label_create(b); \
        lv_label_set_text(l, symbol); \
        lv_obj_center(l); \
    } while(0)

    MAKE_CTRL_BTN(ctrl_row, LV_SYMBOL_PREV, music_prev_cb);
    MAKE_CTRL_BTN(ctrl_row, LV_SYMBOL_PAUSE, music_pause_cb);
    MAKE_CTRL_BTN(ctrl_row, LV_SYMBOL_NEXT, music_next_cb);
    MAKE_CTRL_BTN(ctrl_row, LV_SYMBOL_VOLUME_MID, music_vol_dn_cb);
    MAKE_CTRL_BTN(ctrl_row, LV_SYMBOL_VOLUME_MAX, music_vol_up_cb);

    #undef MAKE_CTRL_BTN

    // Time display
    s_time_label = lv_label_create(ctrl_row);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_time_label, "0:00 / 0:00");

    // Playlist
    s_music_list = lv_list_create(s_music_scr);
    lv_obj_set_size(s_music_list, 1004, 430);
    lv_obj_align(s_music_list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(s_music_list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(s_music_list, 180, 0);
    lv_obj_set_style_border_width(s_music_list, 0, 0);
    lv_obj_set_style_radius(s_music_list, 8, 0);

    for (int i = 0; i < s_track_count; i++) {
        const char *fname = strrchr(s_tracks[i], '/');
        fname = fname ? fname + 1 : s_tracks[i];

        lv_obj_t *btn = lv_list_add_button(s_music_list, LV_SYMBOL_AUDIO, fname);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0f3460), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xe0e0e0), 0);
        lv_obj_add_event_cb(btn, music_track_cb, LV_EVENT_CLICKED,
                           (void *)(intptr_t)i);
    }

    lv_scr_load(s_music_scr);
    bsp_display_unlock();
}

// ============================================================================
// WiFi Settings App
// ============================================================================

// WiFi credential editing state
static lv_obj_t *s_wifi_ssid_ta = NULL;
static lv_obj_t *s_wifi_pass_ta = NULL;

static void wifi_back_async(void *arg) {
    (void)arg;
    lv_obj_t *old_wifi_scr = s_wifi_scr;

    // Remove textareas from group before leaving
    lv_group_t *g = lv_group_get_default();
    if (g && s_wifi_ssid_ta) lv_group_remove_obj(s_wifi_ssid_ta);
    if (g && s_wifi_pass_ta) lv_group_remove_obj(s_wifi_pass_ta);
    osk_cleanup();

    s_wifi_scr = NULL;
    s_wifi_ssid_ta = NULL;
    s_wifi_pass_ta = NULL;
    s_wifi_status_label = NULL;
    s_wifi_ip_label = NULL;
    s_wifi_btn = NULL;
    s_wifi_btn_label = NULL;

    show_settings();

    if (old_wifi_scr) {
        lv_obj_delete_async(old_wifi_scr);
    }
}

static void wifi_back_cb(lv_event_t *e) {
    (void)e;
    lv_async_call(wifi_back_async, NULL);
}

static void wifi_save_creds(void) {
    // Save current textarea contents to NVS
    const char *ssid = lv_textarea_get_text(s_wifi_ssid_ta);
    const char *pass = lv_textarea_get_text(s_wifi_pass_ta);
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi credentials saved: SSID=%s", ssid);
    }
}

static void wifi_ta_click_cb(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    osk_show(ta);
}

static void wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_connected) {
        // Disconnect event owns web_server_stop(); avoid double-stopping httpd.
        wifi_disconnect();
        lv_label_set_text(s_wifi_status_label, "WiFi Off");
        lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0xa0a0a0), 0);
        lv_label_set_text(s_wifi_ip_label, "");
        lv_label_set_text(s_wifi_btn_label, LV_SYMBOL_WIFI " Connect");
        lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x03dac6), 0);
    } else {
        // Save credentials before connecting
        wifi_save_creds();
        lv_label_set_text(s_wifi_status_label, "Connecting...");
        lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0xa0a0a0), 0);
        lv_label_set_text(s_wifi_btn_label, "...");
        osk_hide();
        xTaskCreate(wifi_connect_task, "wifi_conn", 8192, NULL, 3, NULL);
    }
}

static void show_wifi_settings(void)
{
    bsp_display_lock(0);

    s_wifi_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_wifi_scr, LV_OPA_COVER, 0);
    add_bg_to_screen(s_wifi_scr);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_wifi_scr);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, wifi_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    // Title
    lv_obj_t *title = lv_label_create(s_wifi_scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    lv_label_set_text(title, "WiFi Settings");

    // Load current credentials from NVS
    char cur_ssid[33] = "";
    char cur_pass[64] = "";
    {
        nvs_handle_t nvs;
        if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(cur_ssid);
            nvs_get_str(nvs, "ssid", cur_ssid, &len);
            len = sizeof(cur_pass);
            nvs_get_str(nvs, "pass", cur_pass, &len);
            nvs_close(nvs);
        }
    }

    // Content container
    lv_obj_t *cont = lv_obj_create(s_wifi_scr);
    lv_obj_set_size(cont, 500, 320);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_80, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_style_pad_all(cont, 20, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 12, 0);

    // SSID label
    lv_obj_t *ssid_lbl = lv_label_create(cont);
    lv_label_set_text(ssid_lbl, "SSID");
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);

    // SSID textarea
    s_wifi_ssid_ta = lv_textarea_create(cont);
    lv_obj_set_size(s_wifi_ssid_ta, LV_PCT(100), 45);
    lv_textarea_set_one_line(s_wifi_ssid_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_ssid_ta, "Enter WiFi SSID");
    lv_textarea_set_text(s_wifi_ssid_ta, cur_ssid);
    lv_obj_set_style_bg_color(s_wifi_ssid_ta, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_text_color(s_wifi_ssid_ta, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_border_color(s_wifi_ssid_ta, lv_color_hex(0x2c2c3e), 0);
    lv_obj_set_style_border_width(s_wifi_ssid_ta, 1, 0);
    lv_obj_set_style_radius(s_wifi_ssid_ta, 8, 0);
    lv_obj_add_event_cb(s_wifi_ssid_ta, wifi_ta_click_cb, LV_EVENT_CLICKED, NULL);

    // Password label
    lv_obj_t *pass_lbl = lv_label_create(cont);
    lv_label_set_text(pass_lbl, "Password");
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_16, 0);

    // Password textarea
    s_wifi_pass_ta = lv_textarea_create(cont);
    lv_obj_set_size(s_wifi_pass_ta, LV_PCT(100), 45);
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_pass_ta, "Enter WiFi password");
    lv_textarea_set_text(s_wifi_pass_ta, cur_pass);
    lv_textarea_set_password_mode(s_wifi_pass_ta, true);
    lv_obj_set_style_bg_color(s_wifi_pass_ta, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_text_color(s_wifi_pass_ta, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_border_color(s_wifi_pass_ta, lv_color_hex(0x2c2c3e), 0);
    lv_obj_set_style_border_width(s_wifi_pass_ta, 1, 0);
    lv_obj_set_style_radius(s_wifi_pass_ta, 8, 0);
    lv_obj_add_event_cb(s_wifi_pass_ta, wifi_ta_click_cb, LV_EVENT_CLICKED, NULL);

    // Status label
    s_wifi_status_label = lv_label_create(cont);
    lv_obj_set_style_text_font(s_wifi_status_label, &lv_font_montserrat_16, 0);
    if (s_wifi_connected) {
        lv_label_set_text(s_wifi_status_label, LV_SYMBOL_WIFI " Connected");
        lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0x00ff88), 0);
    } else {
        lv_label_set_text(s_wifi_status_label, "Not connected");
        lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0xa0a0a0), 0);
    }

    // IP label
    s_wifi_ip_label = lv_label_create(cont);
    lv_obj_set_style_text_color(s_wifi_ip_label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(s_wifi_ip_label, &lv_font_montserrat_16, 0);
    if (s_wifi_connected) {
        lv_label_set_text_fmt(s_wifi_ip_label, "IP: %s", s_wifi_ip);
    } else {
        lv_label_set_text(s_wifi_ip_label, "");
    }

    // Connect/Disconnect button
    s_wifi_btn = lv_btn_create(s_wifi_scr);
    lv_obj_set_size(s_wifi_btn, 220, 50);
    lv_obj_align(s_wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(s_wifi_btn, 12, 0);
    if (s_wifi_connected) {
        lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0xcf6679), 0);
    } else {
        lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x03dac6), 0);
    }
    lv_obj_add_event_cb(s_wifi_btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_btn_label = lv_label_create(s_wifi_btn);
    lv_obj_center(s_wifi_btn_label);
    lv_obj_set_style_text_color(s_wifi_btn_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_wifi_btn_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_wifi_btn_label, s_wifi_connected ? LV_SYMBOL_CLOSE " Disconnect" : LV_SYMBOL_WIFI " Connect");

    // Add textareas to default group for USB keyboard
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_wifi_ssid_ta);
        lv_group_add_obj(g, s_wifi_pass_ta);
    }

    lv_scr_load(s_wifi_scr);
    bsp_display_unlock();
}

// ============================================================================
// Settings App
// ============================================================================

static void settings_back_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_desktop, NULL);
}
static void settings_wifi_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_wifi_settings, NULL);
}
static void settings_usb_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)usb_icon_cb, NULL);
}

static lv_obj_t *s_bright_val_lbl_ref = NULL;
static lv_obj_t *s_vol_val_lbl_ref = NULL;

static void raw_sd_status_async(void *arg)
{
    char *msg = (char *)arg;
    if (s_raw_sd_status_lbl && lv_obj_is_valid(s_raw_sd_status_lbl)) {
        lv_label_set_text(s_raw_sd_status_lbl, msg ? msg : "Raw SD: no status");
    }
    free(msg);
    s_raw_sd_test_running = false;
}

static void raw_sd_test_task(void *arg)
{
    (void)arg;
    char result[512];
    esp_err_t err = raw_sd_test_run(result, sizeof(result));
    if (err != ESP_OK && result[0] == '\0') {
        snprintf(result, sizeof(result), "Raw SD failed: %s", esp_err_to_name(err));
    }

    char *msg = malloc(strlen(result) + 1);
    if (msg) {
        strcpy(msg, result);
        lv_async_call(raw_sd_status_async, msg);
    } else {
        s_raw_sd_test_running = false;
    }
    vTaskDelete(NULL);
}

static void settings_raw_sd_cb(lv_event_t *e)
{
    (void)e;
    if (s_raw_sd_test_running) {
        return;
    }
    s_raw_sd_test_running = true;
    if (s_raw_sd_status_lbl && lv_obj_is_valid(s_raw_sd_status_lbl)) {
        lv_label_set_text(s_raw_sd_status_lbl, "Raw SD: writing embedded Crab seed...");
    }
    BaseType_t ok = xTaskCreate(raw_sd_test_task, "raw_sd_test", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        s_raw_sd_test_running = false;
        if (s_raw_sd_status_lbl && lv_obj_is_valid(s_raw_sd_status_lbl)) {
            lv_label_set_text(s_raw_sd_status_lbl, "Raw SD: could not start test task");
        }
    }
}

static void bright_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    settings_set_brightness((uint8_t)val);
    if (s_bright_val_lbl_ref) lv_label_set_text_fmt(s_bright_val_lbl_ref, " %d%%", val);
}

static void vol_slider_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    settings_set_volume((uint8_t)val);
    if (s_vol_val_lbl_ref) lv_label_set_text_fmt(s_vol_val_lbl_ref, " %d%%", val);
}

static void stats_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    s_stats_visible = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_stats_bar) {
        if (s_stats_visible) lv_obj_clear_flag(s_stats_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_stats_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fps_toggle_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    s_fps_visible = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s_fps_bar) {
        if (s_fps_visible) lv_obj_clear_flag(s_fps_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_fps_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settings_icon_cb(lv_event_t *e) {
    lv_async_call((lv_async_cb_t)show_settings, NULL);
}

static void show_settings(void)
{
    bsp_display_lock(0);

    if (s_settings_scr) { lv_obj_del(s_settings_scr); s_settings_scr = NULL; s_raw_sd_status_lbl = NULL; }

    s_settings_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_settings_scr, LV_OPA_COVER, 0);
    add_bg_to_screen(s_settings_scr);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_settings_scr);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    // Title
    lv_obj_t *title = lv_label_create(s_settings_scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    lv_label_set_text(title, "Settings");

    // Settings list container
    lv_obj_t *list = lv_obj_create(s_settings_scr);
    lv_obj_set_size(list, 1004, 520);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(list, 180, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);

    // WiFi item
    lv_obj_t *wifi_row = lv_obj_create(list);
    lv_obj_set_size(wifi_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(wifi_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(wifi_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_row, 6, 0);
    lv_obj_set_style_border_width(wifi_row, 0, 0);
    lv_obj_set_style_pad_left(wifi_row, 16, 0);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_row, settings_wifi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_icon = lv_label_create(wifi_row);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);

    lv_obj_t *wifi_lbl = lv_label_create(wifi_row);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(wifi_lbl, "  WiFi");

    // USB HID item
    lv_obj_t *usb_row = lv_obj_create(list);
    lv_obj_set_size(usb_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(usb_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(usb_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(usb_row, 6, 0);
    lv_obj_set_style_border_width(usb_row, 0, 0);
    lv_obj_set_style_pad_left(usb_row, 16, 0);
    lv_obj_set_flex_flow(usb_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(usb_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(usb_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(usb_row, settings_usb_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *usb_icon = lv_label_create(usb_row);
    lv_obj_set_style_text_color(usb_icon, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(usb_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(usb_icon, LV_SYMBOL_USB);

    lv_obj_t *usb_lbl = lv_label_create(usb_row);
    lv_obj_set_style_text_color(usb_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(usb_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(usb_lbl, "  USB HID Debug");

    // Raw SD video-area test item
    lv_obj_t *raw_row = lv_obj_create(list);
    lv_obj_set_size(raw_row, LV_PCT(100), 92);
    lv_obj_set_style_bg_color(raw_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(raw_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(raw_row, 6, 0);
    lv_obj_set_style_border_width(raw_row, 0, 0);
    lv_obj_set_style_pad_left(raw_row, 16, 0);
    lv_obj_set_style_pad_right(raw_row, 16, 0);
    lv_obj_set_flex_flow(raw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(raw_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(raw_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *raw_icon = lv_label_create(raw_row);
    lv_obj_set_style_text_color(raw_icon, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(raw_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(raw_icon, LV_SYMBOL_SAVE);

    lv_obj_t *raw_btn = lv_btn_create(raw_row);
    lv_obj_set_size(raw_btn, 180, 54);
    lv_obj_set_style_bg_color(raw_btn, lv_color_hex(0x2563eb), 0);
    lv_obj_add_event_cb(raw_btn, settings_raw_sd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *raw_btn_lbl = lv_label_create(raw_btn);
    lv_label_set_text(raw_btn_lbl, "Raw SD Test");
    lv_obj_center(raw_btn_lbl);

    s_raw_sd_status_lbl = lv_label_create(raw_row);
    lv_obj_set_width(s_raw_sd_status_lbl, 730);
    lv_label_set_long_mode(s_raw_sd_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_raw_sd_status_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(s_raw_sd_status_lbl, "Write embedded Crab sample to unformatted SD area, verify, benchmark.");

    // --- Brightness Slider ---
    lv_obj_t *bright_row = lv_obj_create(list);
    lv_obj_set_size(bright_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(bright_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(bright_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bright_row, 6, 0);
    lv_obj_set_style_border_width(bright_row, 0, 0);
    lv_obj_set_style_pad_left(bright_row, 16, 0);
    lv_obj_set_style_pad_right(bright_row, 16, 0);
    lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bright_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bright_icon = lv_label_create(bright_row);
    lv_obj_set_style_text_color(bright_icon, lv_color_hex(0x00ff88), 0);
    lv_label_set_text(bright_icon, LV_SYMBOL_IMAGE);

    lv_obj_t *bright_lbl = lv_label_create(bright_row);
    lv_obj_set_style_text_color(bright_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(bright_lbl, " Brightness");

    static lv_obj_t *s_bright_slider = NULL;
    s_bright_slider = lv_slider_create(bright_row);
    lv_obj_set_width(s_bright_slider, 400);
    lv_slider_set_range(s_bright_slider, 0, 100);
    lv_slider_set_value(s_bright_slider, settings_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bright_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bright_slider, lv_color_hex(0xBB86FC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bright_slider, lv_color_hex(0xBB86FC), LV_PART_KNOB);
    lv_obj_add_event_cb(s_bright_slider, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright_val_lbl_ref = lv_label_create(bright_row);
    lv_obj_set_style_text_color(s_bright_val_lbl_ref, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text_fmt(s_bright_val_lbl_ref, " %d%%", settings_get_brightness());

    // --- Volume Slider ---
    lv_obj_t *vol_row = lv_obj_create(list);
    lv_obj_set_size(vol_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(vol_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vol_row, 6, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_left(vol_row, 16, 0);
    lv_obj_set_style_pad_right(vol_row, 16, 0);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_obj_set_style_text_color(vol_icon, lv_color_hex(0x00ff88), 0);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);

    lv_obj_t *vol_lbl = lv_label_create(vol_row);
    lv_obj_set_style_text_color(vol_lbl, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(vol_lbl, " Volume");

    static lv_obj_t *s_vol_slider = NULL;
    s_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_width(s_vol_slider, 400);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, settings_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0xBB86FC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0xBB86FC), LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_vol_val_lbl_ref = lv_label_create(vol_row);
    lv_obj_set_style_text_color(s_vol_val_lbl_ref, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text_fmt(s_vol_val_lbl_ref, " %d%%", settings_get_volume());

    // --- Device Status ---
    lv_obj_t *status_row = lv_obj_create(list);
    lv_obj_set_size(status_row, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(status_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_row, 6, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 12, 0);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024);

    lv_obj_t *status_lbl = lv_label_create(status_row);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text_fmt(status_lbl, "Internal RAM: %uKB free | PSRAM: %uMB free\nWiFi: %s",
        (unsigned)free_int, (unsigned)free_ps,
        s_wifi_connected ? s_wifi_ip : "Not connected");

    // --- Stats Display Toggle ---
    lv_obj_t *stats_row = lv_obj_create(list);
    lv_obj_set_size(stats_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(stats_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(stats_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stats_row, 6, 0);
    lv_obj_set_style_border_width(stats_row, 0, 0);
    lv_obj_set_style_pad_left(stats_row, 16, 0);
    lv_obj_set_style_pad_right(stats_row, 16, 0);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stats_icon = lv_label_create(stats_row);
    lv_obj_set_style_text_color(stats_icon, lv_color_hex(0x00ff88), 0);
    lv_label_set_text(stats_icon, LV_SYMBOL_EYE_OPEN);

    lv_obj_t *stats_txt = lv_label_create(stats_row);
    lv_obj_set_style_text_color(stats_txt, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(stats_txt, " Memory Stats");

    lv_obj_t *stats_sw = lv_switch_create(stats_row);
    lv_obj_set_style_bg_color(stats_sw, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(stats_sw, lv_color_hex(0xBB86FC), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_stats_visible) lv_obj_add_state(stats_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(stats_sw, stats_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // --- FPS Counter Toggle ---
    lv_obj_t *fps_row = lv_obj_create(list);
    lv_obj_set_size(fps_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(fps_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(fps_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fps_row, 6, 0);
    lv_obj_set_style_border_width(fps_row, 0, 0);
    lv_obj_set_style_pad_left(fps_row, 16, 0);
    lv_obj_set_style_pad_right(fps_row, 16, 0);
    lv_obj_set_flex_flow(fps_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fps_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(fps_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fps_icon = lv_label_create(fps_row);
    lv_obj_set_style_text_color(fps_icon, lv_color_hex(0x00ff88), 0);
    lv_label_set_text(fps_icon, LV_SYMBOL_REFRESH);

    lv_obj_t *fps_txt = lv_label_create(fps_row);
    lv_obj_set_style_text_color(fps_txt, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(fps_txt, " FPS Counter");

    lv_obj_t *fps_sw = lv_switch_create(fps_row);
    lv_obj_set_style_bg_color(fps_sw, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(fps_sw, lv_color_hex(0xBB86FC), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_fps_visible) lv_obj_add_state(fps_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(fps_sw, fps_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_scr_load(s_settings_scr);
    bsp_display_unlock();
}

// ============================================================================
// USB Debug App
// ============================================================================

static lv_obj_t *s_usb_log_label = NULL;
static lv_timer_t *s_usb_debug_timer = NULL;
static game_touch_overlay_t s_usb_game_overlay = {0};

static lv_obj_t *s_usb_ta = NULL;  // Track textarea for group cleanup

static void append_game_button(char *buf, size_t len, const char *name)
{
    if (!buf || len == 0 || !name) return;
    size_t used = strlen(buf);
    if (used >= len - 1) return;
    snprintf(buf + used, len - used, "%s%s", used ? " " : "", name);
}

static void format_game_buttons(uint32_t buttons, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (buttons & GAME_INPUT_BTN_UP) append_game_button(buf, len, "Up");
    if (buttons & GAME_INPUT_BTN_DOWN) append_game_button(buf, len, "Down");
    if (buttons & GAME_INPUT_BTN_LEFT) append_game_button(buf, len, "Left");
    if (buttons & GAME_INPUT_BTN_RIGHT) append_game_button(buf, len, "Right");
    if (buttons & GAME_INPUT_BTN_SELECT) append_game_button(buf, len, "Select");
    if (buttons & GAME_INPUT_BTN_START) append_game_button(buf, len, "Start");
    if (buttons & GAME_INPUT_BTN_A) append_game_button(buf, len, "A");
    if (buttons & GAME_INPUT_BTN_B) append_game_button(buf, len, "B");
    if (buttons & GAME_INPUT_BTN_X) append_game_button(buf, len, "X");
    if (buttons & GAME_INPUT_BTN_Y) append_game_button(buf, len, "Y");
    if (buttons & GAME_INPUT_BTN_L) append_game_button(buf, len, "L");
    if (buttons & GAME_INPUT_BTN_R) append_game_button(buf, len, "R");
    if (buttons & GAME_INPUT_BTN_MENU) append_game_button(buf, len, "Menu");
    if (buf[0] == '\0') strlcpy(buf, "none", len);
}

static void usb_debug_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_usb_log_label || !lv_obj_is_valid(s_usb_log_label)) {
        return;
    }

    game_touch_overlay_update(&s_usb_game_overlay);

    game_input_state_t game = {0};
    game_input_read_with_overlay(&game, &s_usb_game_overlay);

    lvgl_port_gamepad_state_t gp = {0};
    lvgl_port_usb_gamepad_get_state(&gp);

    uint16_t vid = 0;
    uint16_t pid = 0;
    lvgl_port_usb_gamepad_get_vid_pid(&vid, &pid);

    uint8_t raw[16] = {0};
    int raw_len = lvgl_port_usb_gamepad_get_raw_report(raw, sizeof(raw));
    const char *source = "HID";

    if (usb_device_monitor_xinput_is_connected()) {
        usb_device_monitor_xinput_get_state(&gp);
        usb_device_monitor_xinput_get_vid_pid(&vid, &pid);
        raw_len = usb_device_monitor_xinput_get_raw_report(raw, sizeof(raw));
        source = "XInput";
    }

    char raw_hex[64] = {0};
    int pos = 0;
    for (int i = 0; i < raw_len && i < 12; ++i) {
        pos += snprintf(raw_hex + pos, sizeof(raw_hex) - pos, "%02X ", raw[i]);
    }

    char game_buttons[128];
    format_game_buttons(game.buttons, game_buttons, sizeof(game_buttons));

    if (game.connected || gp.connected) {
        lv_label_set_text_fmt(s_usb_log_label,
            "Console input: %s%s\n"
            "NES/SNES buttons: %s\n"
            "Gamepad %s connected\n"
            "VID:PID %04X:%04X\n"
            "Buttons: 0x%08lx  DPad: 0x%02x\n"
            "LX:%d LY:%d  RX:%d RY:%d\n"
            "L2:%u R2:%u\n"
            "Raw[%d]: %s",
            game_input_source_name(game.source),
            game.hardware ? " hardware" : "",
            game_buttons,
            source, vid, pid,
            (unsigned long)gp.buttons, gp.dpad,
            gp.axis_lx, gp.axis_ly, gp.axis_rx, gp.axis_ry,
            gp.brake, gp.throttle,
            raw_len, raw_hex);
    } else {
        lv_label_set_text(s_usb_log_label,
            "Waiting for USB gamepad...\n"
            "Transparent touch controls are active.\n"
            "Hardware controller takes priority when connected.");
    }
}

static void usb_back_async(void *arg)
{
    if (s_usb_debug_timer) {
        lv_timer_delete(s_usb_debug_timer);
        s_usb_debug_timer = NULL;
    }
    // Remove textarea from default group before screen deletion
    if (s_usb_ta != NULL) {
        lv_group_t *g = lv_group_get_default();
        if (g) {
            lv_group_remove_obj(s_usb_ta);
        }
        s_usb_ta = NULL;
    }
    // Clear OSK pointers (keyboard will be destroyed with the screen)
    osk_cleanup();
    game_touch_overlay_destroy(&s_usb_game_overlay);
    show_settings();
}

static void usb_back_cb(lv_event_t *e) {
    lv_async_call(usb_back_async, NULL);
}

static void ta_click_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    osk_show(ta);
}

static void usb_icon_cb(lv_event_t *e)
{
    bsp_display_lock(0);

    s_usb_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_usb_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(s_usb_scr, LV_OPA_COVER, 0);
    add_bg_to_screen(s_usb_scr);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_usb_scr);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, usb_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);

    // Title
    lv_obj_t *title = lv_label_create(s_usb_scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    lv_label_set_text(title, "USB HID Debug");

    // Info
    lv_obj_t *info = lv_label_create(s_usb_scr);
    lv_obj_set_style_text_color(info, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_20, 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 20, 60);
    lv_label_set_text(info,
        "USB Host active.\n"
        "Plug in a mouse or keyboard.\n"
        "Mouse cursor should appear on screen.\n"
        "Keyboard input goes to focused LVGL widget.");

    // Log area for raw data
    s_usb_log_label = lv_label_create(s_usb_scr);
    lv_obj_set_style_text_color(s_usb_log_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_usb_log_label, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_usb_log_label, 960);
    lv_label_set_long_mode(s_usb_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_usb_log_label, LV_ALIGN_TOP_LEFT, 20, 180);
    lv_label_set_text(s_usb_log_label, "Waiting for USB device...");
    s_usb_debug_timer = lv_timer_create(usb_debug_timer_cb, 250, NULL);

    // Text area for keyboard testing
    lv_obj_t *ta = lv_textarea_create(s_usb_scr);
    s_usb_ta = ta;
    lv_obj_set_size(ta, 960, 100);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(ta, 230, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xffffff), 0);
    lv_textarea_set_placeholder_text(ta, "Tap here to type...");

    // Show on-screen keyboard when textarea is clicked
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);

    // Add textarea to default group so keyboard input reaches it
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ta);
        lv_group_focus_obj(ta);
    }

    game_touch_overlay_create(&s_usb_game_overlay, s_usb_scr);
    usb_debug_timer_cb(s_usb_debug_timer);

    lv_scr_load(s_usb_scr);
    bsp_display_unlock();
}

// ============================================================================
// USB HID init task (needs its own stack - main task too small)
// ============================================================================

static lv_display_t *s_disp = NULL;

static void usb_hid_init_task(void *arg)
{
    ESP_LOGI(TAG, "Initializing USB HID...");
    lvgl_port_hid_mouse_cfg_t mouse_cfg = {
        .disp = s_disp,
        .sensitivity = 1,
        .cursor_img = NULL,
    };
    lv_indev_t *mouse = lvgl_port_add_usb_hid_mouse_input(&mouse_cfg);
    ESP_LOGI(TAG, "USB HID mouse: %s", mouse ? "OK" : "FAILED");

    lvgl_port_hid_keyboard_cfg_t kb_cfg = {
        .disp = s_disp,
    };
    lv_indev_t *kb = lvgl_port_add_usb_hid_keyboard_input(&kb_cfg);
    ESP_LOGI(TAG, "USB HID keyboard: %s", kb ? "OK" : "FAILED");

    // Create default group and assign keyboard to it so keys reach focused widgets
    if (kb) {
        bsp_display_lock(0);
        lv_group_t *group = lv_group_create();
        lv_group_set_default(group);
        lv_indev_set_group(kb, group);
        bsp_display_unlock();
        ESP_LOGI(TAG, "USB HID keyboard group assigned");
    }

    vTaskDelete(NULL);
}

// ============================================================================
// Main
// ============================================================================

// FPS counter timer callback (runs in LVGL thread every 33ms — counts frames)
static void fps_count_cb(lv_timer_t *t)
{
    (void)t;
    s_fps_frame_count++;

    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_fps_last_tick;
    if (elapsed >= 1000) {
        if (s_fps_visible && s_fps_label) {
            uint32_t fps = (s_fps_frame_count * 1000) / elapsed;
            lv_label_set_text_fmt(s_fps_label, "%lu fps", (unsigned long)fps);
        }
        s_fps_frame_count = 0;
        s_fps_last_tick = now;
    }
}

// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 Tablet - Clean Build ===");

    // Mark this app as valid (prevents rollback to previous OTA partition on reboot)
    esp_ota_mark_app_valid_cancel_rollback();

    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Codec
    ESP_ERROR_CHECK(bsp_extra_codec_init());

    // Display - keep UI interactions responsive; video/USB display have their own apps.
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 50,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    cfg.lvgl_port_cfg.task_max_sleep_ms = 16;  // ~60fps cap for smoother menus

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    s_disp = disp;
    bsp_display_backlight_on();

    // Mount SD card. Keep boot alive if the card is missing, busy, or formatted oddly.
    bool sdcard_ok = false;
    esp_err_t sd_ret = bsp_sdcard_mount();
    if (sd_ret == ESP_OK) {
        sdcard_ok = true;
        ESP_LOGI(TAG, "SD card mounted");
        sdmmc_card_print_info(stdout, bsp_sdcard);
    } else {
        ESP_LOGW(TAG, "SD card mount failed: %s. Continuing without SD.", esp_err_to_name(sd_ret));
    }

    // USB Host + HID (delayed to let system settle)
    esp_err_t usb_ret = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (usb_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB Host started OK");
        esp_err_t mon_ret = usb_device_monitor_start();
        if (mon_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB descriptor monitor failed: %s", esp_err_to_name(mon_ret));
        }
        xTaskCreate(usb_hid_init_task, "hid_init", 8192, NULL, 3, NULL);
    }

    // Build playlist
    if (sdcard_ok) {
        build_playlist("/sdcard/music");
    } else {
        ESP_LOGW(TAG, "Skipping playlist scan because SD card is not mounted");
    }

    // Show desktop
    show_desktop();

    // === Stats overlay on top layer (persists across all screens) ===
    bsp_display_lock(0);
    lv_obj_t *top = lv_layer_top();
    s_stats_bar = lv_obj_create(top);
    lv_obj_set_size(s_stats_bar, 380, 36);
    lv_obj_align(s_stats_bar, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_set_style_bg_color(s_stats_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_stats_bar, 180, 0);
    lv_obj_set_style_radius(s_stats_bar, 6, 0);
    lv_obj_set_style_border_width(s_stats_bar, 0, 0);
    lv_obj_set_style_pad_all(s_stats_bar, 4, 0);
    lv_obj_clear_flag(s_stats_bar, LV_OBJ_FLAG_CLICKABLE);

    s_stats_label = lv_label_create(s_stats_bar);
    lv_obj_set_style_text_color(s_stats_label, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(s_stats_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_stats_label);
    lv_label_set_text(s_stats_label, "...");
    if (!s_stats_visible) lv_obj_add_flag(s_stats_bar, LV_OBJ_FLAG_HIDDEN);

    // FPS counter overlay (top-right, hidden by default)
    s_fps_bar = lv_obj_create(top);
    lv_obj_set_size(s_fps_bar, 100, 36);
    lv_obj_align(s_fps_bar, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_bg_color(s_fps_bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_fps_bar, 180, 0);
    lv_obj_set_style_radius(s_fps_bar, 6, 0);
    lv_obj_set_style_border_width(s_fps_bar, 0, 0);
    lv_obj_set_style_pad_all(s_fps_bar, 4, 0);
    lv_obj_clear_flag(s_fps_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_fps_bar, LV_OBJ_FLAG_HIDDEN);

    s_fps_label = lv_label_create(s_fps_bar);
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(0xffff00), 0);
    lv_obj_set_style_text_font(s_fps_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_fps_label);
    lv_label_set_text(s_fps_label, "-- fps");

    // Create FPS counting timer. Keep it light while hidden; it only updates once per second.
    s_fps_last_tick = lv_tick_get();
    lv_timer_create(fps_count_cb, 250, NULL);

    bsp_display_unlock();

    // Stats update loop (runs in app_main task)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (s_stats_visible) {
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
            size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
            size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024);

            bsp_display_lock(0);
            lv_label_set_text_fmt(s_stats_label,
                "Int:%uKB min:%uKB PS:%uMB",
                (unsigned)free_int, (unsigned)min_int, (unsigned)free_ps);
            bsp_display_unlock();
        }

        // Update time display if music screen is active
        bsp_display_lock(0);
        if (s_time_label && lv_obj_is_valid(s_time_label)) {
            uint32_t pos_ms = audio_player_get_position_ms();
            uint32_t dur_ms = audio_player_get_duration_ms();
            uint32_t pos_s = pos_ms / 1000;
            uint32_t dur_s = dur_ms / 1000;
            lv_label_set_text_fmt(s_time_label, "%d:%02d / %d:%02d",
                (int)(pos_s / 60), (int)(pos_s % 60), (int)(dur_s / 60), (int)(dur_s % 60));
        }
        bsp_display_unlock();
    }
}
