#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// RFB Protocol Constants
#define RFB_PORT_DEFAULT    5900

// Server-to-client message types
#define RFB_MSG_FRAMEBUFFER_UPDATE      0
#define RFB_MSG_SET_COLOUR_MAP          1
#define RFB_MSG_BELL                    2
#define RFB_MSG_SERVER_CUT_TEXT         3

// Client-to-server message types
#define RFB_MSG_SET_PIXEL_FORMAT        0
#define RFB_MSG_SET_ENCODINGS           2
#define RFB_MSG_FRAMEBUFFER_UPDATE_REQ  3
#define RFB_MSG_KEY_EVENT               4
#define RFB_MSG_POINTER_EVENT           5
#define RFB_MSG_CLIENT_CUT_TEXT         6

// Encoding types
#define RFB_ENCODING_RAW                0
#define RFB_ENCODING_COPYRECT           1
#define RFB_ENCODING_RRE                2
#define RFB_ENCODING_HEXTILE            5
#define RFB_ENCODING_TIGHT              7
#define RFB_ENCODING_CURSOR             -239
#define RFB_ENCODING_DESKTOP_SIZE       -223

// Tight sub-encoding
#define TIGHT_FILL                      0x08
#define TIGHT_JPEG                      0x09
#define TIGHT_BASIC                     0x00

// VNC connection state
typedef enum {
    VNC_STATE_DISCONNECTED,
    VNC_STATE_PROTOCOL_VERSION,
    VNC_STATE_SECURITY,
    VNC_STATE_AUTH,
    VNC_STATE_INIT,
    VNC_STATE_CONNECTED,
    VNC_STATE_ERROR
} vnc_state_t;

// Pixel format (we request RGB565)
typedef struct __attribute__((packed)) {
    uint8_t  bits_per_pixel;    // 16
    uint8_t  depth;             // 16
    uint8_t  big_endian;        // 0 (little-endian)
    uint8_t  true_colour;       // 1
    uint16_t red_max;           // 31
    uint16_t green_max;         // 63
    uint16_t blue_max;          // 31
    uint8_t  red_shift;         // 11
    uint8_t  green_shift;       // 5
    uint8_t  blue_shift;        // 0
    uint8_t  padding[3];
} vnc_pixel_format_t;

// Rectangle header from server
typedef struct __attribute__((packed)) {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    int32_t  encoding;
} vnc_rect_header_t;

// Callback for decoded rectangle data
typedef void (*vnc_rect_cb_t)(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              const uint8_t *pixels, size_t len, bool is_jpeg);

// VNC client configuration
typedef struct {
    const char *host;
    uint16_t port;
    const char *password;       // NULL for no auth
    vnc_rect_cb_t on_rect;      // Called when a rectangle is ready
    uint8_t encoding_pref;      // 0=Tight (default), 1=ZRLE, 2=Raw
    uint8_t jpeg_quality;       // 0-9 (0=lowest quality/best compression, 9=best quality)
    uint8_t comp_level;         // 1-9 (1=fastest, 9=best compression)
} vnc_config_t;

/**
 * @brief Start the VNC client connection task.
 */
esp_err_t vnc_client_start(const vnc_config_t *config);

/**
 * @brief Stop the VNC client and disconnect.
 */
void vnc_client_stop(void);

/**
 * @brief Send a key event to the VNC server.
 */
void vnc_send_key(uint32_t keysym, bool down);

/**
 * @brief Send a pointer (mouse) event to the VNC server.
 */
void vnc_send_pointer(uint16_t x, uint16_t y, uint8_t buttons);

/**
 * @brief Request a framebuffer update from the server.
 */
void vnc_request_update(bool incremental);

/**
 * @brief Get current connection state.
 */
vnc_state_t vnc_get_state(void);

/**
 * @brief Get total frame count (for FPS calculation).
 */
uint32_t vnc_get_frame_count(void);
