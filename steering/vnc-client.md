---
inclusion: manual
---

# VNC Client

## Architecture
```
[VNC Server] → WiFi 6 (ESP32-C6 SDIO) → [ESP32-P4] → Decode → [PSRAM Framebuffer] → MIPI-DSI → [Screen]
```

## Encodings
- **Tight**: HW JPEG decode for tiles, PPA fill for solid colors. Best for mixed content.
- **ZRLE**: Zlib-compressed tiles (64×64). Good for text-heavy UIs. Has blue tint bug on Palette RLE path.
- **Raw**: Uncompressed RGB565. Debug/baseline mode. Row-by-row receive for large rects.

## Framebuffer
- Single 1024×600 RGB565 buffer in PSRAM (1.2MB)
- Allocated with `jpeg_alloc_decoder_mem()` (DMA/cache aligned)
- Displayed via `lv_image` widget (NEVER lv_canvas)
- Refresh: `lv_image_cache_drop(&dsc)` + `lv_obj_invalidate(img)`

## PPA Hardware Fill
- Used for solid-color rectangles ≥4×4 pixels
- `ppa_do_fill()` with RGB565 color value
- Faster than CPU memset for large areas

## Tight Encoding Parser
- Shift comp_ctl right 4 bits for subencoding (bits 0-3 are stream reset flags)
- Subencodings: 0x00-0x03=basic, 0x04-0x07=basic+filter, 0x08=fill, 0x09=JPEG
- Filter handling: bit 2 set → read 1-byte filter ID (0=copy, 1=palette, 2=gradient)
- Palette 2-color: 1-bit-per-pixel packed, expand to RGB565
- Zlib: ESP ROM `tinfl_decompress()`, 4 persistent streams, 32KB dictionary each

## ZRLE
- ZLIB_OUT_SIZE = 4MB (full-screen rects can decompress to 1.2MB+)
- CPIXEL is 2 bytes, little-endian
- Blue tint on Palette RLE tiles (subenc 128+) — accepted as "good enough"

## Settings (NVS namespace "vnc")
- host, port, vnc_pass, encoding (0=Tight, 1=ZRLE, 2=Raw), jpeg_q, comp_lvl

## Performance
- Core 0: LVGL ~32%, SDIO ~4%
- Core 1: VNC ~27%
- HW JPEG decoder handles heavy lifting off-CPU

## DES Authentication
- Full VNC DES challenge-response in vnc_rfb.c
- Bit-reversed key per VNC spec
- Compatible: TightVNC, TigerVNC, droidVNC-NG
