# ZRLE Blue Boxes — Debug Log (May 20, 2026)

## Problem
When using ZRLE encoding, solid-color fill tiles render as blue/cyan instead of the correct color (e.g., VS Code dark grey background). The rest of the image (text, icons, non-solid areas) renders correctly. Moving the mouse over the blue areas "paints" the correct color (incremental updates fix it).

## MAJOR FINDING (This Session)
**The ZRLE decoder produces CORRECT color values.** Diagnostic logging confirmed that `ZRLE_FILL` and `FILL` (at display function entry) both show the correct color (e.g., `0x20E4` for VS Code dark grey — identical to what Tight sends). The bug is NOT in color parsing, byte order, or zlib decompression logic.

**Root cause identified: ZLIB_OUT_SIZE buffer too small.** The original 128KB buffer was being truncated for large ZRLE rectangles. A full-screen 1024×600 ZRLE rect can decompress to 1.2MB+ of tile data. Tiles past the buffer limit never got decoded, leaving stale/zero pixels in the framebuffer.

## What Fixed It (Partially)
- **Increasing ZLIB_OUT_SIZE from 128KB → 1.5MB**: Dramatically reduced corruption. Video playback mostly clean. Some blue tint remains on static areas.
- **Increasing to 2.5MB**: Further improvement. Video corruption limited to extremely fast sustained changes.
- **Currently testing 4MB**: Flashed, awaiting results.

## Confirmed Facts
- Tight encoding works perfectly (same PPA fill path, same framebuffer)
- ZRLE decoder parses correctly — color values are CORRECT at both decoder and display function
- CPIXEL is 2 bytes, little-endian (confirmed via RFC 6143 + TigerVNC source + logging)
- The server IS sending 2-byte CPIXELs (not 3-byte — 3-byte caused total garbage)
- CPU stats: Core 1 93% idle, Core 0 51% idle — no bottleneck
- Memory: 99KB internal free, 29MB PSRAM free — no pressure
- The blue is consistent and solid (not random garbage) — appears in areas that don't get updated
- Mouse movement triggers incremental updates that render correctly
- Single framebuffer at fixed PSRAM address (0x4823bb00), never moves, display DMA reads continuously
- PSRAM doesn't drift/decay — pixels stay until overwritten

## What We Tried (All Failed — No Change)

### Made it WORSE:
1. **3-byte CPIXEL BGR** — total multicolored garbage, stream position drift
2. **3-byte CPIXEL RGB** — same garbage
3. **Big-endian 2-byte reads** — all colors inverted
4. **Disable PPA entirely (CPU fallback)** — more visible corruption (striped multicolored boxes)

### No change at all (dead ends):
5. **Cache flush (C2M) in vnc_display_swap()** — not a cache visibility issue
6. **Cache flush+invalidate (C2M|M2C) in vnc_display_swap()** — cache sync in swap not the issue
7. **Remove full-buffer flush before PPA, targeted invalidate after** — PPA cache management doesn't matter
8. **R↔B swap on PPA fill_color_val** — PPA color interpretation not the issue

### FIXED (buffer size):
9. **ZLIB_OUT_SIZE 128KB → 1.5MB** — major improvement, video mostly clean
10. **ZLIB_OUT_SIZE 1.5MB → 2.5MB** — further improvement
11. **ZLIB_OUT_SIZE 2.5MB → 4MB** — currently testing

## Remaining Issue
Static solid-color areas (VS Code background `#1e1e1e`, solid black areas) still show a faint blue/light tint in some tiles. These are areas written once during the initial full-screen update and never touched again. The tint appears as subtle rectangular blocks aligned to 64×64 tile boundaries.

Possible remaining causes:
- Buffer STILL not large enough for complex initial frames (4MB test pending)
- Some tiles in the initial frame use non-fill subencodings (packed palette, RLE) that have a subtle bug
- The `_zlib_out` buffer is shared with Tight's decompression paths (shouldn't matter in ZRLE-only mode)

## Key Observation About "Disable PPA" Test
The multicolored striped garbage when PPA was disabled shows that the **non-fill tile paths** (raw pixels, packed palette, RLE row-by-row writes) may have issues too — not just fills. The PPA fills were actually MASKING some corruption by correctly overwriting bad data.

## Working Binary Saved
`vnc_client_working_zrle.bin` — the "liveable with blue" version (128KB buffer)

## Source State (Current)
- `vnc_client/main/vnc_rfb.c` — ZRLE decoder, ZLIB_OUT_SIZE = 4MB, debug logging REMOVED
- `vnc_client/main/vnc_display.c` — clean (no debug logging), PPA enabled
- `vnc_client/main/main.c` — encoding selector has Tight/ZRLE/Raw buttons
- `vnc_client/ZRLE_COLOR_COMPARISON.md` — detailed color comparison data from all 3 encodings
- `vnc_client/ZRLE_DEBUG_LOG.md` — this file

## Settings UI Changes (This Session)
- Added "Raw" encoding button (red, for debug) — encoding_pref=2
- Raw encoding sends only RFB_ENCODING_RAW to server (no compression)
- NVS key "encoding" stores 0=Tight, 1=ZRLE, 2=Raw

## Next Steps
1. Test 4MB buffer — if blue disappears entirely, the fix is just buffer size
2. If blue persists at 4MB, the issue is in non-fill tile paths (packed palette expansion, RLE expansion)
3. Consider adding a truncation warning log: if decompression hits ZLIB_OUT_SIZE limit, log it
4. Consider processing ZRLE rectangles in chunks instead of decompressing entire rect at once
