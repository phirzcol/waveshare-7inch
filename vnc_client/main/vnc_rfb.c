// =============================================================================
// VNC RFB Protocol Client (Stripped — Tight encoding only)
// =============================================================================

#include "vnc_rfb.h"
#include "vnc_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <arpa/inet.h>

#include "miniz.h"

static const char *TAG = "VNC_RFB";

// Frame counter (for FPS measurement)
static uint32_t _frame_count = 0;
uint32_t vnc_get_frame_count(void) { return _frame_count; }

// =============================================================================
// State
// =============================================================================

static int _sock = -1;
static vnc_state_t _state = VNC_STATE_DISCONNECTED;
static vnc_config_t _config = {0};
static TaskHandle_t _vnc_task = NULL;
static uint16_t _server_w = 0;
static uint16_t _server_h = 0;

// Receive buffer (PSRAM)
#define RECV_BUF_SIZE   (256 * 1024)
static uint8_t *_recv_buf = NULL;

// Zlib decompression using tinfl (persistent per-stream state)
#define ZLIB_OUT_SIZE   (4096 * 1024)  // 4MB — eliminate any possibility of truncation
static uint8_t *_zlib_out = NULL;
static tinfl_decompressor _tinfl[4];
static bool _tinfl_inited[4] = {false, false, false, false};
#define TINFL_DICT_SIZE 32768
static uint8_t *_tinfl_dict[4] = {NULL, NULL, NULL, NULL};
static size_t _tinfl_dict_ofs[4] = {0, 0, 0, 0};

// ZRLE persistent zlib stream (single stream, separate from Tight's 4)
static tinfl_decompressor _zrle_tinfl;
static bool _zrle_tinfl_inited = false;
static uint8_t *_zrle_dict = NULL;
static size_t _zrle_dict_ofs = 0;
#define ZRLE_TILE_W 64
#define ZRLE_TILE_H 64

// =============================================================================
// Socket Helpers
// =============================================================================

static int sock_recv_exact(int sock, void *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int r = recv(sock, (uint8_t *)buf + received, len - received, 0);
        if (r <= 0) return -1;
        received += r;
    }
    return (int)received;
}

static int sock_send_all(int sock, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int s = send(sock, (const uint8_t *)buf + sent, len - sent, 0);
        if (s <= 0) return -1;
        sent += s;
    }
    return (int)sent;
}

// =============================================================================
// RFB Handshake
// =============================================================================

static bool rfb_handshake(void)
{
    // 1. Receive server protocol version
    char version[13] = {0};
    if (sock_recv_exact(_sock, version, 12) < 0) {
        ESP_LOGE(TAG, "Failed to receive version");
        return false;
    }
    ESP_LOGI(TAG, "Server version: %.12s", version);

    // 2. Send our version (3.8)
    const char *our_version = "RFB 003.008\n";
    sock_send_all(_sock, our_version, 12);

    // 3. Security handshake
    uint8_t num_sec;
    sock_recv_exact(_sock, &num_sec, 1);
    if (num_sec == 0) {
        ESP_LOGE(TAG, "Server refused connection");
        return false;
    }

    uint8_t sec_types[16];
    sock_recv_exact(_sock, sec_types, num_sec);

    // Look for None (1)
    bool has_none = false;
    for (int i = 0; i < num_sec; i++) {
        if (sec_types[i] == 1) has_none = true;
    }

    if (has_none) {
        uint8_t sel = 1;
        sock_send_all(_sock, &sel, 1);
    } else {
        ESP_LOGE(TAG, "Server requires auth — not supported in stripped mode");
        return false;
    }

    // 4. Security result
    uint32_t sec_result;
    sock_recv_exact(_sock, &sec_result, 4);
    if (ntohl(sec_result) != 0) {
        ESP_LOGE(TAG, "Authentication failed");
        return false;
    }

    // 5. ClientInit — shared flag = 1
    uint8_t shared = 1;
    sock_send_all(_sock, &shared, 1);

    // 6. ServerInit
    uint16_t fb_w, fb_h;
    sock_recv_exact(_sock, &fb_w, 2);
    sock_recv_exact(_sock, &fb_h, 2);
    _server_w = ntohs(fb_w);
    _server_h = ntohs(fb_h);

    // Skip server pixel format (16 bytes) and name
    uint8_t skip[16];
    sock_recv_exact(_sock, skip, 16);

    uint32_t name_len;
    sock_recv_exact(_sock, &name_len, 4);
    name_len = ntohl(name_len);
    if (name_len > 0 && name_len < 256) {
        char name[256];
        sock_recv_exact(_sock, name, name_len);
        name[name_len] = '\0';
        ESP_LOGI(TAG, "Server: %s (%ux%u)", name, _server_w, _server_h);
    } else if (name_len > 0) {
        for (uint32_t i = 0; i < name_len; i++) {
            uint8_t b;
            sock_recv_exact(_sock, &b, 1);
        }
    }

    // 7. Set pixel format to RGB565
    uint8_t set_pf_msg[20] = {0};
    set_pf_msg[0] = RFB_MSG_SET_PIXEL_FORMAT;
    vnc_pixel_format_t pf = {
        .bits_per_pixel = 16,
        .depth = 16,
        .big_endian = 0,
        .true_colour = 1,
        .red_max = htons(31),
        .green_max = htons(63),
        .blue_max = htons(31),
        .red_shift = 11,
        .green_shift = 5,
        .blue_shift = 0,
    };
    memcpy(&set_pf_msg[4], &pf, sizeof(pf));
    sock_send_all(_sock, set_pf_msg, 20);

    // 8. Set encodings based on preference
    if (_config.encoding_pref == 1) {
        // ZRLE + Raw
        uint8_t enc_msg[12];
        enc_msg[0] = RFB_MSG_SET_ENCODINGS;
        enc_msg[1] = 0;
        uint16_t num_enc = htons(2);
        memcpy(&enc_msg[2], &num_enc, 2);
        int32_t enc_zrle = htonl(16);  // ZRLE encoding number
        int32_t enc_raw = htonl(RFB_ENCODING_RAW);
        memcpy(&enc_msg[4], &enc_zrle, 4);
        memcpy(&enc_msg[8], &enc_raw, 4);
        sock_send_all(_sock, enc_msg, 12);
        ESP_LOGI(TAG, "Handshake complete. RGB565 + ZRLE encoding.");
    } else if (_config.encoding_pref == 2) {
        // Raw only (debug baseline — no compression, perfect color reference)
        uint8_t enc_msg[8];
        enc_msg[0] = RFB_MSG_SET_ENCODINGS;
        enc_msg[1] = 0;
        uint16_t num_enc = htons(1);
        memcpy(&enc_msg[2], &num_enc, 2);
        int32_t enc_raw = htonl(RFB_ENCODING_RAW);
        memcpy(&enc_msg[4], &enc_raw, 4);
        sock_send_all(_sock, enc_msg, 8);
        ESP_LOGI(TAG, "Handshake complete. RGB565 + RAW ONLY (debug mode).");
    } else {
        // Tight + Raw + quality/compression hints
        uint8_t enc_msg[20];
        enc_msg[0] = RFB_MSG_SET_ENCODINGS;
        enc_msg[1] = 0;
        uint16_t num_enc = htons(4);
        memcpy(&enc_msg[2], &num_enc, 2);
        int32_t enc_tight = htonl(RFB_ENCODING_TIGHT);
        int32_t enc_raw = htonl(RFB_ENCODING_RAW);
        int32_t enc_jpeg_quality = htonl(-32 + (_config.jpeg_quality > 0 ? _config.jpeg_quality : 5));
        int32_t enc_compress_level = htonl(-256 + (_config.comp_level > 0 ? _config.comp_level : 5));
        memcpy(&enc_msg[4], &enc_tight, 4);
        memcpy(&enc_msg[8], &enc_raw, 4);
        memcpy(&enc_msg[12], &enc_jpeg_quality, 4);
        memcpy(&enc_msg[16], &enc_compress_level, 4);
        sock_send_all(_sock, enc_msg, 20);
        ESP_LOGI(TAG, "Handshake complete. RGB565 + Tight (quality=5, compress=5).");
    }
    return true;
}

// =============================================================================
// Frame Update Processing
// =============================================================================

static void process_raw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    size_t pixel_bytes = w * h * 2;

    // For full-width rects that fit the framebuffer stride, receive directly into framebuffer
    if (w == 1024 && x == 0 && _config.on_rect) {
        // Full-width: receive row by row directly into framebuffer via on_rect
        // Use _recv_buf as row buffer
        size_t row_bytes = (size_t)w * 2;
        for (uint16_t row = 0; row < h; row++) {
            if (row_bytes > RECV_BUF_SIZE) break;
            sock_recv_exact(_sock, _recv_buf, row_bytes);
            _config.on_rect(x, y + row, w, 1, _recv_buf, row_bytes, false);
        }
        return;
    }

    // For smaller rects, receive in chunks that fit RECV_BUF_SIZE
    if (pixel_bytes > RECV_BUF_SIZE) {
        // Receive row by row for large rects
        size_t row_bytes = (size_t)w * 2;
        if (row_bytes <= RECV_BUF_SIZE) {
            for (uint16_t row = 0; row < h; row++) {
                sock_recv_exact(_sock, _recv_buf, row_bytes);
                if (_config.on_rect) {
                    _config.on_rect(x, y + row, w, 1, _recv_buf, row_bytes, false);
                }
            }
        } else {
            // Row itself too large (shouldn't happen at 1024 width = 2048 bytes)
            size_t remaining = pixel_bytes;
            while (remaining > 0) {
                size_t chunk = (remaining > RECV_BUF_SIZE) ? RECV_BUF_SIZE : remaining;
                sock_recv_exact(_sock, _recv_buf, chunk);
                remaining -= chunk;
            }
        }
        return;
    }

    sock_recv_exact(_sock, _recv_buf, pixel_bytes);

    if (_config.on_rect) {
        _config.on_rect(x, y, w, h, _recv_buf, pixel_bytes, false);
    }
}

static void process_tight_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint8_t comp_ctl_raw;
    sock_recv_exact(_sock, &comp_ctl_raw, 1);

    // Process stream reset bits (bits 0-3)
    for (int i = 0; i < 4; i++) {
        if ((comp_ctl_raw & (1 << i)) && _tinfl_inited[i]) {
            tinfl_init(&_tinfl[i]);
            _tinfl_dict_ofs[i] = 0;
        }
    }

    uint8_t comp_ctl = comp_ctl_raw >> 4;

    if (comp_ctl == 0x09) {
        // JPEG
        uint32_t jpeg_len = 0;
        uint8_t b;
        sock_recv_exact(_sock, &b, 1);
        jpeg_len = b & 0x7F;
        if (b & 0x80) {
            sock_recv_exact(_sock, &b, 1);
            jpeg_len |= (uint32_t)(b & 0x7F) << 7;
            if (b & 0x80) {
                sock_recv_exact(_sock, &b, 1);
                jpeg_len |= (uint32_t)b << 14;
            }
        }

        if (jpeg_len > RECV_BUF_SIZE) {
            ESP_LOGE(TAG, "JPEG too large: %lu", (unsigned long)jpeg_len);
            size_t remaining = jpeg_len;
            while (remaining > 0) {
                size_t chunk = (remaining > RECV_BUF_SIZE) ? RECV_BUF_SIZE : remaining;
                sock_recv_exact(_sock, _recv_buf, chunk);
                remaining -= chunk;
            }
            return;
        }

        sock_recv_exact(_sock, _recv_buf, jpeg_len);
        if (_config.on_rect) {
            _config.on_rect(x, y, w, h, _recv_buf, jpeg_len, true);
        }

    } else if (comp_ctl == 0x08) {
        // Fill — 2 bytes for RGB565
        uint8_t color[2];
        sock_recv_exact(_sock, color, 2);
        if (_config.on_rect) {
            _config.on_rect(x, y, w, h, color, 2, false);
        }

    } else {
        // Basic compression
        int stream_id = comp_ctl & 0x03;
        bool has_filter = (comp_ctl & 0x04) != 0;
        int filter_id = 0;
        int num_colors = 0;
        uint8_t palette_buf[512];

        if (has_filter) {
            uint8_t fid;
            sock_recv_exact(_sock, &fid, 1);
            filter_id = fid;

            if (filter_id == 1) {
                uint8_t nc;
                sock_recv_exact(_sock, &nc, 1);
                num_colors = nc + 1;
                size_t palette_bytes = num_colors * 2;
                if (palette_bytes <= sizeof(palette_buf)) {
                    sock_recv_exact(_sock, palette_buf, palette_bytes);
                }
            }
        }

        size_t row_size;
        if (num_colors == 2) {
            row_size = (w + 7) / 8;
        } else {
            row_size = (size_t)w * 2;
        }
        size_t data_size = row_size * h;

        if (data_size < 12) {
            // Uncompressed
            if (data_size <= RECV_BUF_SIZE) {
                sock_recv_exact(_sock, _recv_buf, data_size);
                if (num_colors == 0 && _config.on_rect) {
                    _config.on_rect(x, y, w, h, _recv_buf, data_size, false);
                }
            }
        } else {
            // Compressed
            uint32_t compressed_len = 0;
            uint8_t b;
            sock_recv_exact(_sock, &b, 1);
            compressed_len = b & 0x7F;
            if (b & 0x80) {
                sock_recv_exact(_sock, &b, 1);
                compressed_len |= (uint32_t)(b & 0x7F) << 7;
                if (b & 0x80) {
                    sock_recv_exact(_sock, &b, 1);
                    compressed_len |= (uint32_t)b << 14;
                }
            }

            if (compressed_len <= RECV_BUF_SIZE) {
                sock_recv_exact(_sock, _recv_buf, compressed_len);

                if (_zlib_out && data_size <= ZLIB_OUT_SIZE && _tinfl_dict[stream_id]) {
                    if (!_tinfl_inited[stream_id]) {
                        tinfl_init(&_tinfl[stream_id]);
                        _tinfl_dict_ofs[stream_id] = 0;
                        _tinfl_inited[stream_id] = true;
                    }

                    const uint8_t *in_ptr = _recv_buf;
                    size_t in_remaining = compressed_len;
                    size_t out_pos = 0;

                    while (in_remaining > 0 && out_pos < data_size) {
                        size_t in_bytes = in_remaining;
                        size_t out_bytes = TINFL_DICT_SIZE - _tinfl_dict_ofs[stream_id];

                        tinfl_status status = tinfl_decompress(&_tinfl[stream_id],
                            in_ptr, &in_bytes,
                            _tinfl_dict[stream_id],
                            _tinfl_dict[stream_id] + _tinfl_dict_ofs[stream_id],
                            &out_bytes,
                            TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_PARSE_ZLIB_HEADER);

                        if (out_bytes > 0 && (out_pos + out_bytes) <= ZLIB_OUT_SIZE) {
                            memcpy(_zlib_out + out_pos,
                                   _tinfl_dict[stream_id] + _tinfl_dict_ofs[stream_id],
                                   out_bytes);
                            out_pos += out_bytes;
                        }

                        _tinfl_dict_ofs[stream_id] = (_tinfl_dict_ofs[stream_id] + out_bytes) & (TINFL_DICT_SIZE - 1);
                        in_ptr += in_bytes;
                        in_remaining -= in_bytes;

                        if (status <= TINFL_STATUS_DONE) break;
                    }

                    if (out_pos > 0 && _config.on_rect) {
                        if (num_colors == 2) {
                            uint16_t color0 = palette_buf[0] | (palette_buf[1] << 8);
                            uint16_t color1 = palette_buf[2] | (palette_buf[3] << 8);
                            size_t pixel_count = (size_t)w * h;
                            size_t expanded_size = pixel_count * 2;
                            if (expanded_size <= ZLIB_OUT_SIZE - out_pos) {
                                uint8_t *expanded = _zlib_out + out_pos;
                                size_t px = 0;
                                for (uint16_t row = 0; row < h && px < pixel_count; row++) {
                                    for (uint16_t col = 0; col < w && px < pixel_count; col++) {
                                        size_t byte_idx = row * ((w + 7) / 8) + (col / 8);
                                        uint8_t bit = 0;
                                        if (byte_idx < out_pos) {
                                            bit = (_zlib_out[byte_idx] >> (7 - (col % 8))) & 1;
                                        }
                                        uint16_t color = bit ? color1 : color0;
                                        expanded[px * 2] = color & 0xFF;
                                        expanded[px * 2 + 1] = (color >> 8) & 0xFF;
                                        px++;
                                    }
                                }
                                _config.on_rect(x, y, w, h, expanded, expanded_size, false);
                            }
                        } else if (num_colors > 2 && num_colors <= 256) {
                            size_t pixel_count = (size_t)w * h;
                            size_t expanded_size = pixel_count * 2;
                            if (expanded_size <= ZLIB_OUT_SIZE - out_pos) {
                                uint8_t *expanded = _zlib_out + out_pos;
                                for (size_t px = 0; px < pixel_count && px < out_pos; px++) {
                                    uint8_t idx = _zlib_out[px];
                                    if (idx < num_colors) {
                                        expanded[px * 2] = palette_buf[idx * 2];
                                        expanded[px * 2 + 1] = palette_buf[idx * 2 + 1];
                                    } else {
                                        expanded[px * 2] = 0;
                                        expanded[px * 2 + 1] = 0;
                                    }
                                }
                                _config.on_rect(x, y, w, h, expanded, expanded_size, false);
                            }
                        } else {
                            _config.on_rect(x, y, w, h, _zlib_out, out_pos, false);
                        }
                    }
                }
            } else {
                size_t remaining = compressed_len;
                while (remaining > 0) {
                    size_t chunk = (remaining > RECV_BUF_SIZE) ? RECV_BUF_SIZE : remaining;
                    sock_recv_exact(_sock, _recv_buf, chunk);
                    remaining -= chunk;
                }
            }
        }
    }
}

// =============================================================================
// ZRLE Encoding (RFC 6143 Section 7.7.6)
// =============================================================================
// CPIXEL = PIXEL = 2 bytes (RGB565, little-endian) since bpp=16.
// Tile size: 64x64. Single persistent zlib stream.
// Subencoding byte: bit7=RLE, bits6:0=paletteSize.
//   palSize=0,rle=0 → raw pixels
//   palSize=1        → solid fill (single color)
//   palSize=2-16,rle=0 → packed palette
//   palSize=0,rle=1 → plain RLE
//   palSize=2+,rle=1 → palette RLE

static inline uint16_t zrle_read_pixel(const uint8_t *d, size_t *p) {
    uint16_t v = d[*p] | ((uint16_t)d[*p + 1] << 8);
    *p += 2;
    return v;
}

static void zrle_emit_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    uint8_t c[2] = { (uint8_t)(color & 0xFF), (uint8_t)(color >> 8) };
    if (_config.on_rect) _config.on_rect(x, y, w, h, c, 2, false);
}

static void zrle_decode_tile(uint16_t tx, uint16_t ty, uint16_t tw, uint16_t th,
                             const uint8_t *d, size_t dlen, size_t *pos)
{
    if (*pos >= dlen) return;
    uint8_t subenc = d[(*pos)++];
    bool rle = (subenc & 128) != 0;
    uint8_t pal_size = subenc & 127;
    uint32_t npix = (uint32_t)tw * th;

    // Read palette
    uint16_t palette[128];
    if (*pos + (size_t)pal_size * 2 > dlen) { *pos = dlen; return; }
    for (int i = 0; i < pal_size; i++) palette[i] = zrle_read_pixel(d, pos);

    if (pal_size == 1) {
        // Solid fill
        zrle_emit_fill(tx, ty, tw, th, palette[0]);
        return;
    }

    uint16_t row_buf[ZRLE_TILE_W];

    if (!rle) {
        if (pal_size == 0) {
            // Raw pixels
            if (*pos + npix * 2 > dlen) { *pos = dlen; return; }
            for (uint16_t row = 0; row < th; row++) {
                for (uint16_t col = 0; col < tw; col++)
                    row_buf[col] = zrle_read_pixel(d, pos);
                if (_config.on_rect) _config.on_rect(tx, ty + row, tw, 1, (const uint8_t *)row_buf, tw * 2, false);
            }
        } else {
            // Packed palette (palSize 2-16)
            uint8_t bppp = (pal_size <= 2) ? 1 : (pal_size <= 4) ? 2 : 4;
            uint8_t ppb = 8 / bppp;
            size_t row_bytes = ((size_t)tw + ppb - 1) / ppb;
            size_t packed_size = row_bytes * th;
            if (*pos + packed_size > dlen) { *pos = dlen; return; }
            uint8_t mask = (1 << bppp) - 1;
            for (uint16_t row = 0; row < th; row++) {
                size_t row_start = *pos + row * row_bytes;
                for (uint16_t col = 0; col < tw; col++) {
                    uint8_t nbits = (ppb - 1 - (col % ppb)) * bppp;
                    uint8_t idx = (d[row_start + col / ppb] >> nbits) & mask;
                    row_buf[col] = (idx < pal_size) ? palette[idx] : 0;
                }
                if (_config.on_rect) _config.on_rect(tx, ty + row, tw, 1, (const uint8_t *)row_buf, tw * 2, false);
            }
            *pos += packed_size;
        }
    } else {
        // RLE modes
        uint32_t done = 0;
        if (pal_size == 0) {
            // Plain RLE — color + run length
            while (done < npix && *pos + 2 <= dlen) {
                uint16_t color = zrle_read_pixel(d, pos);
                uint32_t run = 1;
                uint8_t b;
                do { if (*pos >= dlen) break; b = d[(*pos)++]; run += b; } while (b == 255);
                if (done + run > npix) run = npix - done;
                for (uint32_t i = 0; i < run; i++) {
                    uint32_t px = done + i;
                    row_buf[px % tw] = color;
                    if ((px % tw) == (uint32_t)(tw - 1))
                        if (_config.on_rect) _config.on_rect(tx, ty + px / tw, tw, 1, (const uint8_t *)row_buf, tw * 2, false);
                }
                done += run;
            }
        } else {
            // Palette RLE — index byte + optional run length
            while (done < npix && *pos < dlen) {
                uint8_t ib = d[(*pos)++];
                uint8_t idx = ib & 0x7F;
                uint16_t color = (idx < pal_size) ? palette[idx] : 0;
                uint32_t run = 1;
                if (ib & 128) {
                    uint8_t b;
                    do { if (*pos >= dlen) break; b = d[(*pos)++]; run += b; } while (b == 255);
                }
                if (done + run > npix) run = npix - done;
                for (uint32_t i = 0; i < run; i++) {
                    uint32_t px = done + i;
                    row_buf[px % tw] = color;
                    if ((px % tw) == (uint32_t)(tw - 1))
                        if (_config.on_rect) _config.on_rect(tx, ty + px / tw, tw, 1, (const uint8_t *)row_buf, tw * 2, false);
                }
                done += run;
            }
        }
        // Flush partial last row
        if (done > 0 && (done % tw) != 0) {
            uint16_t last_row = (done - 1) / tw;
            uint16_t cols = done % tw;
            if (_config.on_rect) _config.on_rect(tx, ty + last_row, cols, 1, (const uint8_t *)row_buf, cols * 2, false);
        }
    }
}

static void process_zrle_rect(uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh)
{
    // Read compressed length (4 bytes, big-endian per RFC)
    uint8_t len_buf[4];
    if (sock_recv_exact(_sock, len_buf, 4) < 0) return;
    uint32_t comp_len = ((uint32_t)len_buf[0] << 24) | ((uint32_t)len_buf[1] << 16) |
                        ((uint32_t)len_buf[2] << 8) | len_buf[3];

    if (comp_len == 0 || comp_len > RECV_BUF_SIZE) {
        // Skip oversized data
        size_t rem = comp_len;
        while (rem > 0) { size_t c = (rem > RECV_BUF_SIZE) ? RECV_BUF_SIZE : rem; sock_recv_exact(_sock, _recv_buf, c); rem -= c; }
        return;
    }

    if (sock_recv_exact(_sock, _recv_buf, comp_len) < 0) return;
    if (!_zrle_dict || !_zlib_out) return;

    // Initialize zlib stream on first use
    if (!_zrle_tinfl_inited) {
        tinfl_init(&_zrle_tinfl);
        _zrle_dict_ofs = 0;
        _zrle_tinfl_inited = true;
    }

    // Decompress
    const uint8_t *in_ptr = _recv_buf;
    size_t in_rem = comp_len;
    size_t out_total = 0;

    while (in_rem > 0 && out_total < ZLIB_OUT_SIZE) {
        size_t in_bytes = in_rem;
        size_t out_bytes = TINFL_DICT_SIZE - _zrle_dict_ofs;
        tinfl_status st = tinfl_decompress(&_zrle_tinfl, in_ptr, &in_bytes,
            _zrle_dict, _zrle_dict + _zrle_dict_ofs, &out_bytes,
            TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (out_bytes > 0 && (out_total + out_bytes) <= ZLIB_OUT_SIZE) {
            memcpy(_zlib_out + out_total, _zrle_dict + _zrle_dict_ofs, out_bytes);
            out_total += out_bytes;
        }
        _zrle_dict_ofs = (_zrle_dict_ofs + out_bytes) & (TINFL_DICT_SIZE - 1);
        in_ptr += in_bytes;
        in_rem -= in_bytes;
        if (st < TINFL_STATUS_DONE) { ESP_LOGE(TAG, "ZRLE zlib error %d", st); return; }
        if (st == TINFL_STATUS_DONE) break;
    }

    // Decode tiles from decompressed data
    const uint8_t *d = _zlib_out;
    size_t dlen = out_total;
    size_t pos = 0;

    for (uint16_t ty = ry; ty < ry + rh; ty += ZRLE_TILE_H) {
        uint16_t th = ((ty + ZRLE_TILE_H) > (ry + rh)) ? (ry + rh - ty) : ZRLE_TILE_H;
        for (uint16_t tx = rx; tx < rx + rw; tx += ZRLE_TILE_W) {
            uint16_t tw = ((tx + ZRLE_TILE_W) > (rx + rw)) ? (rx + rw - tx) : ZRLE_TILE_W;
            zrle_decode_tile(tx, ty, tw, th, d, dlen, &pos);
        }
    }
}

static void process_framebuffer_update(void)
{
    uint8_t hdr[3];
    sock_recv_exact(_sock, hdr, 3);
    uint16_t num_rects = (hdr[1] << 8) | hdr[2];

    for (int i = 0; i < num_rects; i++) {
        uint8_t rect_hdr[12];
        sock_recv_exact(_sock, rect_hdr, 12);

        uint16_t x = (rect_hdr[0] << 8) | rect_hdr[1];
        uint16_t y = (rect_hdr[2] << 8) | rect_hdr[3];
        uint16_t w = (rect_hdr[4] << 8) | rect_hdr[5];
        uint16_t h = (rect_hdr[6] << 8) | rect_hdr[7];
        int32_t enc = (rect_hdr[8] << 24) | (rect_hdr[9] << 16) | (rect_hdr[10] << 8) | rect_hdr[11];

        if (enc == RFB_ENCODING_RAW) {
            process_raw_rect(x, y, w, h);
        } else if (enc == RFB_ENCODING_TIGHT) {
            process_tight_rect(x, y, w, h);
        } else if (enc == 16) {
            process_zrle_rect(x, y, w, h);
        } else {
            ESP_LOGE(TAG, "Unknown encoding %ld — disconnecting", (long)enc);
            _state = VNC_STATE_ERROR;
            return;
        }
    }
    _frame_count++;
    vnc_display_swap();
}

// =============================================================================
// Main VNC Task
// =============================================================================

static void vnc_task(void *arg)
{
    ESP_LOGI(TAG, "VNC task started. Connecting to %s:%u", _config.host, _config.port);

    _recv_buf = heap_caps_malloc(RECV_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!_recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        _state = VNC_STATE_ERROR;
        vTaskDelete(NULL);
        return;
    }

    _zlib_out = heap_caps_malloc(ZLIB_OUT_SIZE, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 4; i++) {
        if (!_tinfl_dict[i]) {
            _tinfl_dict[i] = heap_caps_malloc(TINFL_DICT_SIZE, MALLOC_CAP_SPIRAM);
        }
        _tinfl_inited[i] = false;
    }

    // ZRLE zlib stream dictionary
    _zrle_dict = heap_caps_malloc(TINFL_DICT_SIZE, MALLOC_CAP_SPIRAM);
    _zrle_tinfl_inited = false;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(_config.port),
    };
    inet_pton(AF_INET, _config.host, &dest.sin_addr);

    _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        _state = VNC_STATE_ERROR;
        goto cleanup;
    }

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(_sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "Connection failed");
        _state = VNC_STATE_ERROR;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Connected to VNC server");
    _state = VNC_STATE_PROTOCOL_VERSION;

    if (!rfb_handshake()) {
        _state = VNC_STATE_ERROR;
        goto cleanup;
    }

    _state = VNC_STATE_CONNECTED;
    vnc_request_update(false);

    // Main receive loop
    while (_state == VNC_STATE_CONNECTED) {
        uint8_t msg_type;
        int r = sock_recv_exact(_sock, &msg_type, 1);
        if (r < 0) {
            ESP_LOGW(TAG, "Connection lost");
            _state = VNC_STATE_DISCONNECTED;
            break;
        }

        switch (msg_type) {
        case RFB_MSG_FRAMEBUFFER_UPDATE:
            process_framebuffer_update();
            if (_state == VNC_STATE_CONNECTED) {
                vnc_request_update(true);
            }
            break;

        case RFB_MSG_SET_COLOUR_MAP: {
            uint8_t skip[5];
            sock_recv_exact(_sock, skip, 5);
            uint16_t num = (skip[3] << 8) | skip[4];
            for (int i = 0; i < num * 6; i++) {
                uint8_t b; sock_recv_exact(_sock, &b, 1);
            }
            break;
        }

        case RFB_MSG_BELL:
            break;

        case RFB_MSG_SERVER_CUT_TEXT: {
            uint8_t pad[3];
            sock_recv_exact(_sock, pad, 3);
            uint32_t text_len;
            sock_recv_exact(_sock, &text_len, 4);
            text_len = ntohl(text_len);
            for (uint32_t i = 0; i < text_len; i++) {
                uint8_t b; sock_recv_exact(_sock, &b, 1);
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown server message: %u", msg_type);
            _state = VNC_STATE_ERROR;
            break;
        }
    }

cleanup:
    if (_sock >= 0) { close(_sock); _sock = -1; }
    if (_recv_buf) { heap_caps_free(_recv_buf); _recv_buf = NULL; }
    if (_zlib_out) { heap_caps_free(_zlib_out); _zlib_out = NULL; }
    if (_zrle_dict) { heap_caps_free(_zrle_dict); _zrle_dict = NULL; }
    _zrle_tinfl_inited = false;
    for (int i = 0; i < 4; i++) { _tinfl_inited[i] = false; }
    ESP_LOGI(TAG, "VNC task ended (state=%d)", _state);
    _vnc_task = NULL;
    vTaskDelete(NULL);
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t vnc_client_start(const vnc_config_t *config)
{
    if (_state != VNC_STATE_DISCONNECTED) {
        vnc_client_stop();
    }

    _config = *config;
    _state = VNC_STATE_DISCONNECTED;

    xTaskCreatePinnedToCore(vnc_task, "vnc_client", 8192, NULL, 4, &_vnc_task, 1);
    return ESP_OK;
}

void vnc_client_stop(void)
{
    _state = VNC_STATE_ERROR;
    if (_sock >= 0) {
        shutdown(_sock, SHUT_RDWR);
        close(_sock);
        _sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}

void vnc_send_key(uint32_t keysym, bool down)
{
    if (_sock < 0 || _state != VNC_STATE_CONNECTED) return;

    uint8_t msg[8];
    msg[0] = RFB_MSG_KEY_EVENT;
    msg[1] = down ? 1 : 0;
    msg[2] = 0; msg[3] = 0;
    msg[4] = (keysym >> 24) & 0xFF;
    msg[5] = (keysym >> 16) & 0xFF;
    msg[6] = (keysym >> 8) & 0xFF;
    msg[7] = keysym & 0xFF;
    sock_send_all(_sock, msg, 8);
}

void vnc_send_pointer(uint16_t x, uint16_t y, uint8_t buttons)
{
    if (_sock < 0 || _state != VNC_STATE_CONNECTED) return;

    uint8_t msg[6];
    msg[0] = RFB_MSG_POINTER_EVENT;
    msg[1] = buttons;
    msg[2] = (x >> 8) & 0xFF;
    msg[3] = x & 0xFF;
    msg[4] = (y >> 8) & 0xFF;
    msg[5] = y & 0xFF;
    sock_send_all(_sock, msg, 6);
}

void vnc_request_update(bool incremental)
{
    if (_sock < 0 || _state != VNC_STATE_CONNECTED) return;

    uint8_t msg[10];
    msg[0] = RFB_MSG_FRAMEBUFFER_UPDATE_REQ;
    msg[1] = incremental ? 1 : 0;
    msg[2] = 0; msg[3] = 0;
    msg[4] = 0; msg[5] = 0;
    msg[6] = (_server_w >> 8) & 0xFF;
    msg[7] = _server_w & 0xFF;
    msg[8] = (_server_h >> 8) & 0xFF;
    msg[9] = _server_h & 0xFF;
    sock_send_all(_sock, msg, 10);
}

vnc_state_t vnc_get_state(void)
{
    return _state;
}
