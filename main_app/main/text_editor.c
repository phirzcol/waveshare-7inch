// =============================================================================
// ESP32-P4 Tablet — Text Editor (Temp File Architecture)
// =============================================================================
// Always works on temp file. Save/Save As copies temp to target.
// Save As: destroys editor screen, shows filename entry screen (one textarea
// at a time), then copies temp to entered name.
// =============================================================================

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "text_editor.h"
#include "onscreen_keyboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "EDITOR";

#define FILE_MAX_SIZE   (512 * 1024)
#define PATH_MAX_LEN    256
#define OSK_HEIGHT      240
#define TEMP_FILE_PATH  "/sdcard/text/.editor_temp"
#define DEFAULT_NAME    "note.txt"
#define DEFAULT_DIR     "/sdcard/text"
#define AUTOSAVE_MS     60000

// =============================================================================
// State
// =============================================================================

static lv_obj_t *_scr = NULL;
static lv_obj_t *_textarea = NULL;
static lv_obj_t *_status_label = NULL;
static lv_obj_t *_title_label = NULL;
static lv_timer_t *_autosave_timer = NULL;
static char _filename[128] = {0};
static int _ta_full_height = 0;
static bool _dirty = false;

// Save As screen state
static lv_obj_t *_saveas_scr = NULL;
static lv_obj_t *_saveas_ta = NULL;

// =============================================================================
// External
// =============================================================================
extern void show_desktop(void);

// Forward declarations
static void build_editor_ui(void);
static void show_saveas_screen(void);

// =============================================================================
// File I/O
// =============================================================================

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }
}

static int load_file_to_buf(const char *path, char *buf, size_t buf_size)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || (size_t)size >= buf_size) { fclose(f); return -2; }
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return (int)rd;
}

static int write_file(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "r");
    if (!in) return -1;
    FILE *out = fopen(dst, "w");
    if (!out) { fclose(in); return -1; }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

// Save current textarea to temp file
static void save_to_temp(void)
{
    if (!_textarea) return;
    const char *text = lv_textarea_get_text(_textarea);
    if (!text) return;
    write_file(TEMP_FILE_PATH, text, strlen(text));
}

// =============================================================================
// Auto-save (LVGL timer context)
// =============================================================================

static void autosave_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!_textarea || !_dirty) return;
    save_to_temp();
    _dirty = false;
    ESP_LOGI(TAG, "Auto-saved to temp");
}

// =============================================================================
// Editor Screen Callbacks
// =============================================================================

static void text_changed_cb(lv_event_t *e)
{
    (void)e;
    _dirty = true;
}

static void ta_click_cb(lv_event_t *e)
{
    (void)e;
    if (!osk_is_visible() && _textarea) {
        osk_show(_textarea);
        lv_obj_set_height(_textarea, _ta_full_height - OSK_HEIGHT);
    }
}

static void save_click_cb(lv_event_t *e)
{
    (void)e;
    if (!_textarea) return;

    // Save textarea to temp
    save_to_temp();

    // Copy temp to target
    ensure_dir(DEFAULT_DIR);
    char target[PATH_MAX_LEN];
    snprintf(target, sizeof(target), "%s/%s", DEFAULT_DIR, _filename);

    if (copy_file(TEMP_FILE_PATH, target) == 0) {
        ESP_LOGI(TAG, "Saved: %s", target);
        if (_status_label) lv_label_set_text_fmt(_status_label, "Saved: %s", _filename);
    } else {
        ESP_LOGE(TAG, "Save failed: %s", target);
        if (_status_label) lv_label_set_text(_status_label, "Save FAILED");
    }
}

static void saveas_click_cb(lv_event_t *e)
{
    (void)e;
    // Save current text to temp before switching screens
    save_to_temp();
    _dirty = false;

    // Stop autosave
    if (_autosave_timer) { lv_timer_delete(_autosave_timer); _autosave_timer = NULL; }

    // Defocus and cleanup editor screen
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(NULL);
    osk_hide();
    osk_cleanup();

    // Delete editor screen
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = NULL;
        _textarea = NULL;
        _status_label = NULL;
        _title_label = NULL;
    }

    // Show the Save As filename entry screen
    show_saveas_screen();
}

static void editor_back_async(void *arg)
{
    (void)arg;
    if (_autosave_timer) { lv_timer_delete(_autosave_timer); _autosave_timer = NULL; }
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(NULL);
    osk_hide();
    osk_cleanup();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = NULL;
        _textarea = NULL;
        _status_label = NULL;
        _title_label = NULL;
    }
    show_desktop();
}

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    if (_dirty) save_to_temp();
    osk_hide();
    lv_async_call(editor_back_async, NULL);
}

// =============================================================================
// Save As Screen (separate screen, one textarea for filename)
// =============================================================================

static void saveas_back_async(void *arg)
{
    (void)arg;
    // Cleanup Save As screen
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(NULL);
    osk_hide();
    osk_cleanup();
    if (_saveas_scr) {
        lv_obj_delete(_saveas_scr);
        _saveas_scr = NULL;
        _saveas_ta = NULL;
    }
    // Reopen editor (temp file still has the content)
    build_editor_ui();
    // Reload from temp
    char *buf = (char *)heap_caps_malloc(FILE_MAX_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (buf) {
        int r = load_file_to_buf(TEMP_FILE_PATH, buf, FILE_MAX_SIZE + 1);
        if (r > 0 && _textarea) {
            lv_textarea_set_text(_textarea, buf);
            lv_textarea_set_cursor_pos(_textarea, 0);
        }
        free(buf);
    }
}

static void saveas_back_cb(lv_event_t *e)
{
    (void)e;
    osk_hide();
    lv_async_call(saveas_back_async, NULL);
}

static void saveas_save_cb(lv_event_t *e)
{
    (void)e;
    if (!_saveas_ta) return;

    // Get the entered filename
    const char *name = lv_textarea_get_text(_saveas_ta);
    if (!name || name[0] == '\0') return;

    // Store as the new filename
    snprintf(_filename, sizeof(_filename), "%s", name);

    // Copy temp file to target with new name
    ensure_dir(DEFAULT_DIR);
    char target[PATH_MAX_LEN];
    snprintf(target, sizeof(target), "%s/%s", DEFAULT_DIR, _filename);

    if (copy_file(TEMP_FILE_PATH, target) == 0) {
        ESP_LOGI(TAG, "Save As: %s", target);
    } else {
        ESP_LOGE(TAG, "Save As failed: %s", target);
    }

    // Go back to editor with new filename
    osk_hide();
    lv_async_call(saveas_back_async, NULL);
}

static void saveas_ta_click_cb(lv_event_t *e)
{
    (void)e;
    if (!osk_is_visible() && _saveas_ta) {
        osk_show(_saveas_ta);
    }
}

static void show_saveas_screen(void)
{
    _saveas_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_saveas_scr, lv_color_hex(0x0f0f23), 0);
    lv_obj_set_style_bg_opa(_saveas_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_saveas_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header = lv_obj_create(_saveas_scr);
    lv_obj_set_size(header, 1024, 48);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 80, 38);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, saveas_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    // Title
    lv_obj_t *ttl = lv_label_create(header);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0xe0e0e0), 0);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ttl, "Save As");

    // Save button
    lv_obj_t *save_btn = lv_btn_create(header);
    lv_obj_set_size(save_btn, 90, 38);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0xBB86FC), 0);
    lv_obj_add_event_cb(save_btn, saveas_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(sl);

    // Instruction label
    lv_obj_t *instr = lv_label_create(_saveas_scr);
    lv_obj_set_style_text_color(instr, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(instr, LV_ALIGN_TOP_LEFT, 20, 60);
    lv_label_set_text(instr, "Enter filename:");

    // Filename textarea (the ONLY textarea on this screen)
    _saveas_ta = lv_textarea_create(_saveas_scr);
    lv_obj_set_size(_saveas_ta, 900, 50);
    lv_obj_align(_saveas_ta, LV_ALIGN_TOP_LEFT, 20, 90);
    lv_obj_set_style_bg_color(_saveas_ta, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(_saveas_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(_saveas_ta, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(_saveas_ta, lv_color_hex(0xBB86FC), 0);
    lv_obj_set_style_border_width(_saveas_ta, 2, 0);
    lv_obj_set_style_radius(_saveas_ta, 8, 0);
    lv_obj_set_style_pad_all(_saveas_ta, 10, 0);
    lv_textarea_set_placeholder_text(_saveas_ta, "myfile.txt");
    lv_textarea_set_one_line(_saveas_ta, true);
    lv_textarea_set_text(_saveas_ta, _filename);  // Pre-fill with current name
    lv_obj_add_event_cb(_saveas_ta, saveas_ta_click_cb, LV_EVENT_CLICKED, NULL);

    // Add to group
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _saveas_ta);
        lv_group_focus_obj(_saveas_ta);
    }

    lv_scr_load(_saveas_scr);

    // Auto-show OSK (must be after scr_load so keyboard creates on correct screen)
    osk_show(_saveas_ta);
}

// =============================================================================
// Editor UI Builder
// =============================================================================

static void build_editor_ui(void)
{
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header (48px): [Back] [Filename] [Save] [Save As]
    lv_obj_t *header = lv_obj_create(_scr);
    lv_obj_set_size(header, 1024, 48);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 80, 38);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    // Filename title
    _title_label = lv_label_create(header);
    lv_obj_set_style_text_color(_title_label, lv_color_hex(0xe0e0e0), 0);
    lv_obj_align(_title_label, LV_ALIGN_LEFT_MID, 100, 0);
    lv_label_set_text(_title_label, _filename);

    // Save As button
    lv_obj_t *saveas_btn = lv_btn_create(header);
    lv_obj_set_size(saveas_btn, 100, 38);
    lv_obj_align(saveas_btn, LV_ALIGN_RIGHT_MID, -100, 0);
    lv_obj_set_style_bg_color(saveas_btn, lv_color_hex(0x16213e), 0);
    lv_obj_add_event_cb(saveas_btn, saveas_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sal = lv_label_create(saveas_btn);
    lv_label_set_text(sal, "Save As");
    lv_obj_center(sal);

    // Save button
    lv_obj_t *save_btn = lv_btn_create(header);
    lv_obj_set_size(save_btn, 90, 38);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0xBB86FC), 0);
    lv_obj_add_event_cb(save_btn, save_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(save_btn);
    lv_label_set_text(svl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(svl);

    // Textarea
    _ta_full_height = 600 - 48 - 24;
    _textarea = lv_textarea_create(_scr);
    lv_obj_set_size(_textarea, 1024, _ta_full_height);
    lv_obj_set_pos(_textarea, 0, 48);
    lv_obj_set_style_bg_color(_textarea, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(_textarea, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_border_width(_textarea, 0, 0);
    lv_obj_set_style_radius(_textarea, 0, 0);
    lv_obj_set_style_pad_all(_textarea, 12, 0);
    lv_textarea_set_placeholder_text(_textarea, "Tap here to type...");
    lv_textarea_set_cursor_click_pos(_textarea, true);

    lv_obj_add_event_cb(_textarea, ta_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(_textarea, text_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Add to group
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _textarea);
        lv_group_focus_obj(_textarea);
    }

    // Status bar
    _status_label = lv_label_create(_scr);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(_status_label, LV_ALIGN_BOTTOM_LEFT, 12, -4);
    lv_label_set_text(_status_label, _filename);

    // Autosave timer
    _autosave_timer = lv_timer_create(autosave_cb, AUTOSAVE_MS, NULL);

    lv_scr_load(_scr);
}

// =============================================================================
// Public API
// =============================================================================

void text_editor_show(void)
{
    ESP_LOGI(TAG, "New document");
    ensure_dir(DEFAULT_DIR);
    snprintf(_filename, sizeof(_filename), "%s", DEFAULT_NAME);
    _dirty = false;

    build_editor_ui();

    // Load temp file if exists (recovery)
    struct stat st;
    if (stat(TEMP_FILE_PATH, &st) == 0 && st.st_size > 0) {
        ESP_LOGI(TAG, "Recovering from temp (%ld bytes)", (long)st.st_size);
        char *buf = (char *)heap_caps_malloc(FILE_MAX_SIZE + 1, MALLOC_CAP_SPIRAM);
        if (buf) {
            int r = load_file_to_buf(TEMP_FILE_PATH, buf, FILE_MAX_SIZE + 1);
            if (r > 0 && _textarea) {
                lv_textarea_set_text(_textarea, buf);
                lv_textarea_set_cursor_pos(_textarea, 0);
            }
            free(buf);
        }
    }
}

void text_editor_open(const char *filepath)
{
    ESP_LOGI(TAG, "Opening: %s", filepath);
    ensure_dir(DEFAULT_DIR);

    const char *fname = strrchr(filepath, '/');
    fname = fname ? fname + 1 : filepath;
    snprintf(_filename, sizeof(_filename), "%s", fname);
    _dirty = false;

    // Copy source to temp
    copy_file(filepath, TEMP_FILE_PATH);

    build_editor_ui();

    // Load temp into textarea
    char *buf = (char *)heap_caps_malloc(FILE_MAX_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "Alloc failed"); return; }
    int result = load_file_to_buf(TEMP_FILE_PATH, buf, FILE_MAX_SIZE + 1);
    if (result >= 0 && _textarea) {
        lv_textarea_set_text(_textarea, buf);
        lv_textarea_set_cursor_pos(_textarea, 0);
        ESP_LOGI(TAG, "Loaded %d bytes", result);
    }
    free(buf);
}
