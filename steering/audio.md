---
inclusion: manual
---

# Audio — Ring Buffer Pipeline

## Hardware
- ES8311 DAC (I2C 0x18), ES7210 ADC (I2C 0x40)
- I2S bus, PA enable GPIO53
- Init: `bsp_extra_codec_init()` then `bsp_extra_player_init()`

## Pipeline Architecture
```
SD Card → [Reader Task, Core 1, Prio 4] → 128KB PSRAM Ring Buffer → [Decoder Task, Core 1, Prio 5] → I2S → ES8311
```

## Key Files (in components/esp-audio-player/)
- `audio_ring_buffer.h/c` — 128KB PSRAM ring buffer, reader task, event group
- `audio_player.cpp` — creates ring buffer, starts/stops around playback
- `audio_mp3.cpp` — reads from ring buffer (not fread)
- `audio_wav.cpp` — reads from ring buffer (not fread)

## How It Works
1. `is_mp3(fp)` / `is_wav(fp)` detect file type using direct FILE*
2. `audio_ring_buffer_start(rb, fp)` launches reader task, pre-fills 25% (32KB)
3. Decoder reads from ring buffer — never blocks on SD card
4. Reader task reads 4KB chunks, signals data available
5. On stop/new file: `audio_ring_buffer_stop()` aborts reader, flushes

## Why Ring Buffer (Fixed Stutter)
Original code called `fread()` in decode loop. When LVGL or ESP-Hosted caused memory bus contention, `fread()` blocked 20-50ms — longer than I2S DMA buffer (~23ms at 44.1kHz). Ring buffer provides 725ms headroom.

## Rules
- Audio player pinned to Core 1, LVGL pinned to Core 0
- 1ms yield in reader task after each fread() to share SDMMC bus
- `bsp_extra_codec_set_fs()` closes/reopens codec on every call — don't cache/skip
- Seek: MP3 by byte-percentage (finds next sync word), WAV by 4-byte aligned offset
- Auto-advance: callback on AUDIO_PLAYER_STATE_IDLE event

## DO NOT
- Use large I2S DMA buffers to fix stutter — causes reverb
- Call `is_mp3()` after seeking — it rewinds to position 0
