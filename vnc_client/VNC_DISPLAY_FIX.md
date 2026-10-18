# VNC Display Fix — Root Cause & Solution

## Date: May 2026

## THE BUG
VNC client connected successfully but displayed scrambled/garbled pixels (vertical stripes, diagonal artifacts). The VNC protocol was working correctly — data arrived, rectangles were parsed, pixels were decoded — but the display showed garbage.

## ROOT CAUSE
**Using `lv_canvas` to display the VNC framebuffer.** 

LVGL 9.2's `lv_canvas_set_buffer()` does NOT work correctly for large full-screen framebuffers on the ESP32-P4 MIPI-DSI display. The canvas widget has internal stride/rendering behavior that doesn't match what the DPI panel expects. This causes the pixel data to be read with the wrong row offset, producing the characteristic vertical striping pattern.

## THE FIX
**Use `lv_image` widget with `lv_image_dsc_t` — the same pattern as the working photo_viewer.c.**

### Critical Requirements:

1. **Buffer allocation: `jpeg_alloc_decoder_mem()`** — NOT `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. The JPEG decoder memory allocator ensures proper DMA/cache alignment required by the ESP32-P4's display pipeline.

2. **Widget: `lv_image` (NOT `lv_canvas`)** — Create an `lv_image` widget and set its source to an `lv_image_dsc_t` struct that points to the framebuffer.

3. **Image descriptor setup:**
   ```c
   _img_dsc.header.w = 1024;              // Visible width
   _img_dsc.header.h = 600;               // Visible height
   _img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
   _img_dsc.header.stride = 1024 * 2;     // 2048 bytes per row
   _img_dsc.data_size = 1024 * 600 * 2;   // Total buffer size
   _img_dsc.data = _fb_front;             // DMA-aligned buffer
   ```

4. **Refresh method: `lv_image_cache_drop()` + `lv_obj_invalidate()`** — Don't call `lv_image_set_src()` repeatedly (LVGL caches it). Instead, drop the cache and invalidate:
   ```c
   lv_image_cache_drop(&_img_dsc);
   lv_obj_invalidate(_img_widget);
   ```

5. **Height: 600 (NOT 608)** — The 16-pixel height alignment (608) is only needed for the JPEG decoder's output buffer, not for the display framebuffer. Using 608 causes LVGL to miscalculate the image layout.

6. **Stride: 1024 × 2 = 2048** — Width 1024 is already 16-pixel aligned. No padding needed.

## WHAT DIDN'T WORK (and why)

| Approach | Result | Why |
|----------|--------|-----|
| `lv_canvas_set_buffer()` | Scrambled stripes | Canvas has internal stride behavior incompatible with full-screen DPI rendering |
| `lv_canvas` + forced stride | Still scrambled | Canvas ignores manual stride override |
| `lv_image_dsc_t` with `heap_caps_malloc` | Scrambled | Buffer not DMA/cache aligned |
| `lv_image_dsc_t` with height=608 | Lines only | LVGL confused by padded height |
| Direct `esp_lcd_panel_draw_bitmap()` without LVGL | No display | Can't have both LVGL and direct panel access |
| `esp_async_memcpy` DMA init | Scrambled | Conflicts with JPEG decoder DMA channels |
| `CONFIG_LV_USE_CLIB_MALLOC=y` | Not the cause | Works fine, photo viewer uses it too |
| `CONFIG_SPIRAM_SPEED_200M=y` | Not the cause | Working backup uses it |

## WORKING CODE PATTERN

```c
// Allocate with jpeg_alloc_decoder_mem (DMA-aligned)
jpeg_decode_memory_alloc_cfg_t alloc_cfg = {
    .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
};
size_t actual = 0;
_fb_front = (uint8_t *)jpeg_alloc_decoder_mem(FB_SIZE, &alloc_cfg, &actual);

// Create lv_image widget
_img_widget = lv_image_create(_scr);
lv_image_set_src(_img_widget, &_img_dsc);
lv_obj_align(_img_widget, LV_ALIGN_TOP_LEFT, 0, 0);

// On frame update (swap):
lv_image_cache_drop(&_img_dsc);
lv_obj_invalidate(_img_widget);
```

## KEY LESSON
The ESP32-P4's display pipeline (MIPI-DSI + DMA) has strict memory alignment requirements. `jpeg_alloc_decoder_mem()` satisfies these. Regular PSRAM malloc does not. And `lv_canvas` has different internal rendering behavior than `lv_image` — always use `lv_image` for large framebuffers on this hardware.


## HOW THE WORKING VERSION OPERATES (for future resize work)

### Data Flow
```
TightVNC Server (Windows 1024x768)
    ↓ TCP over WiFi 6 (ESP32-C6 via ESP-Hosted SDIO)
VNC RFB Protocol Parser (Core 1, vnc_rfb.c)
    ↓ Parses FramebufferUpdate messages
    ↓ Each message contains N rectangles with (x, y, w, h, encoding, data)
    ↓
For each rectangle:
    ↓ JPEG tile → HW JPEG decoder → RGB565 pixels (tile_aligned_w stride)
    ↓ Raw pixels → direct memcpy
    ↓ Fill → solid color fill loop
    ↓
Write into _fb_back buffer at (x, y) using stride_px = 1024
    ↓ Row-by-row copy: dst_row = fb_back + (y+row) * 1024 + x
    ↓ Clips to SCREEN_H (600) — rows beyond 600 are discarded
    ↓
After all rectangles in frame: vnc_display_swap()
    ↓ memcpy(_fb_front, _fb_back, 1024*600*2)
    ↓ lv_image_cache_drop(&_img_dsc)
    ↓ lv_obj_invalidate(_img_widget)
    ↓
LVGL renders _fb_front through its normal flush pipeline
    ↓ Reads 30 lines at a time from _fb_front using header.stride=2048
    ↓ Copies to its small draw buffer
    ↓ Flush callback → esp_lcd_panel_draw_bitmap() → DPI panel DMA framebuffer
    ↓
MIPI-DSI controller continuously scans DPI framebuffer → 7" IPS screen
```

### Buffer Layout
```
_fb_front / _fb_back:
  Allocated with: jpeg_alloc_decoder_mem(1024 * 600 * 2)
  Size: 1,228,800 bytes (1.17 MB each)
  Format: RGB565, little-endian
  Stride: 1024 pixels = 2048 bytes per row
  Rows: 600 (visible), no padding
  
  Row 0:   pixels[0..1023] at offset 0
  Row 1:   pixels[0..1023] at offset 2048
  ...
  Row 599: pixels[0..1023] at offset 599*2048
```

### Server Resolution Handling
- Server is 1024x768, display is 1024x600
- Width matches (1024) — no horizontal clipping needed
- Height: server sends rows 0-767, we only store rows 0-599
- Bottom 168 rows are silently discarded (bounds check: `if (y + row >= SCREEN_H) break`)
- No scaling, no centering, no resolution negotiation
- If server width != 1024, tiles would still be placed at correct (x,y) but anything beyond x=1023 is clipped

### For Future Resize
- To support different server resolutions, change SCREEN_W/SCREEN_H constants
- Buffer must be reallocated if resolution changes
- Stride must always = buffer_width * 2
- lv_image_dsc_t header must be updated with new w/h/stride
- lv_image_set_src() must be called after descriptor change
- For scaling: use lv_image_set_scale() like photo_viewer does
- For centering: use lv_obj_align(img, LV_ALIGN_CENTER, 0, 0)
```

### JPEG Tile Decode Detail
```
VNC JPEG tile arrives: (x=100, y=200, w=73, h=46, jpeg_data[])
    ↓
jpeg_decoder_get_info() → info.width=73, info.height=46
    ↓
tile_aligned_w = (73 + 15) & ~15 = 80  (16-pixel aligned)
tile_aligned_h = (46 + 15) & ~15 = 48
    ↓
jpeg_decoder_process() → decodes into _jpeg_out_buf
    Output stride: tile_aligned_w * 2 = 160 bytes/row
    Output has 48 rows of 80 pixels each (padded)
    ↓
Copy to framebuffer:
    for row 0..45:  (copy_h = min(46, info.height) = 46)
        src = _jpeg_out_buf + row * 160  (tile stride)
        dst = _fb_back + (200+row) * 1024 + 100  (framebuffer stride)
        memcpy(dst, src, 73 * 2)  (copy only visible pixels, not padding)
```


## SERVER RESOLUTION REQUIREMENTS

### For clean initial display:
- Set TightVNC server to **1024×600** or **800×600** before connecting
- The first FramebufferUpdate must fit within buffer limits

### What happens at higher resolutions:
- **1024×768**: Works after initial connect — displays upper-left 1024×600, bottom 168px clipped
- **1920×1080**: Initial frame overwhelms buffers (squares/scramble), but incremental updates work after
- **Any resolution**: After the first frame settles, only the upper-left 1024×600 corner is displayed

### Why high-res initial frame fails:
- Server sends the entire screen as JPEG tiles in one burst
- At 1920×1080, tiles are larger and more numerous
- RECV_BUF_SIZE (256KB) can't hold a single large raw rectangle
- Multiple large JPEG tiles in rapid succession overwhelm processing

### Recommended server settings:
- Resolution: 1024×600 (exact match) or 800×600
- Tight encoding (default)
- JPEG quality: 5 (midrange)
- Compression: 5 (midrange)
- No password (simplest)

### Future fix for arbitrary resolution:
- Use PPA SRM to scale server framebuffer down to 1024×600
- Or request DesktopSize pseudo-encoding to ask server to resize
- Or handle initial burst by processing tiles across multiple frames


## iPAZZPORT MOUSE — VNC MODE (CORRECTED)

### The Fix
For VNC mode, the iPazzport axes work **directly** — NO swap needed:
```c
hid_ctx->mouse.x += dx;
hid_ctx->mouse.y += dy;
```

### Why No Swap in VNC Mode
The steering file says "axes swapped" but that was for the main app which uses LVGL's display rotation (mirror_x=true, mirror_y=true in BSP). The VNC client doesn't use rotation — the lv_image is at TOP_LEFT with no transform. So the raw HID axes map directly to screen coordinates.

### The Patched File
`managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_usbhid.c`

iPazzport detection: `if (data_length >= 6 && data[0] == 0x01)`
- Byte 0: Report ID (always 0x01)
- Byte 1: Buttons (bit 0 = left, bit 1 = right)
- Bytes 2-3: X displacement (int16_t LE)
- Bytes 4-5: Y displacement (int16_t LE)
- Bytes 6-7: Scroll wheel / unused

### Sensitivity
Set to 1 in main.c: `mouse_cfg.sensitivity = 1`
The iPazzport sends appropriately-scaled values (±3 to ±30 per report).

### Known Quirks
- Fast downward swipe sends GUI+Z keyboard report (device firmware, unfixable)
- Device STALLs on SET_IDLE and SET_PROTOCOL (errors ignored in patched file)
- Continuous idle reports at ~100Hz when connected (filtered in debug logging)
