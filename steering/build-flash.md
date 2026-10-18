---
inclusion: manual
---

# Build & Flash

## Environment Variables (REQUIRED)
```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:ESP_IDF_VERSION = "5.5"
$env:PATH = "C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;" + $env:PATH
```

**ESP_IDF_VERSION=5.5 is critical** — esp_wifi_remote 0.14.5 uses it in Kconfig `orsource`. Without it, slave target choice won't resolve.

## Partition Table (32MB Flash)
```
nvs,      data, nvs,     0x9000,   0x4000
otadata,  data, ota,     0xd000,   0x2000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x300000  (3MB - main app)
ota_0,    app,  ota_0,   0x310000, 0x300000  (3MB - VNC client)
ota_1,    app,  ota_1,   0x610000, 0x100000  (1MB - USB display)
storage,  data, fat,     0x710000, 0xAF0000  (~11MB - FAT)
```

## Flash Commands

### Main app → factory (includes bootloader + partition table + otadata)
```powershell
cd main_app
idf.py -p COM23 flash
```

### VNC client → ota_0
```powershell
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x310000 vnc_client/build/vnc_client.bin
```

### USB display → ota_1
```powershell
python -m esptool --chip esp32p4 -p COM23 -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB 0x610000 usb_display/build/usb_touch_screen.bin
```

## Flash Rules
- **NEVER** use `idf.py flash` from vnc_client/ or usb_display/ — overwrites factory
- Device does NOT auto-reset after flash — press RESET manually
- Boot mode: hold BOOT, press RESET, release BOOT
- Close serial monitor before flashing (COM23 port conflict)

## Build Gotchas
- gdbinit OSError (ESP_ROM_ELF_DIR) — **harmless**, build succeeds, ignore exit code 1
- After clean build or managed_components re-fetch: reapply iPazzport patch
- `idf.py reconfigure` needed when sdkconfig.defaults changes
- New .c files need `idf.py reconfigure` (CMake glob doesn't auto-detect)

## iPazzport Patch Procedure
```powershell
# After first build of main_app or vnc_client:
copy patches\esp_lvgl_port_usbhid.c managed_components\espressif__esp_lvgl_port\src\lvgl9\
idf.py build
```
Must delete the cached .obj file if patch was applied after initial compile:
```powershell
Get-ChildItem -Path build -Recurse -Filter "*esp_lvgl_port_usbhid*" | Remove-Item -Force
idf.py build
```
