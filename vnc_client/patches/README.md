# VNC Client Patches

These files must be copied back after any `managed_components` re-fetch or clean build.

## Files

### esp_lvgl_port_usbhid.c
**Target:** `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_usbhid.c`

**What it patches:**
- iPazzport 8-byte report detection (report ID 0x01, 16-bit axes)
- Direct axis mapping: `x += dx, y += dy` (no swap)
- SET_IDLE / SET_PROTOCOL error ignoring (iPazzport STALLs these)
- Debug logging for HID reports

**Apply after:** `idf.py reconfigure` or deleting `managed_components/`

### sdkconfig.defaults
The base sdkconfig.defaults for the VNC client. If sdkconfig is deleted, this regenerates it.
**Critical:** Must set `$env:ESP_IDF_VERSION = "5.5"` before building.

### partitions.csv
Single-partition layout (factory only, no OTA for now).

## Build Command
```powershell
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:ESP_IDF_VERSION = "5.5"
$env:PATH = "C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;" + $env:PATH
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe C:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py -p COM23 flash
```

## After Re-fetch
```powershell
Copy-Item "patches\esp_lvgl_port_usbhid.c" "managed_components\espressif__esp_lvgl_port\src\lvgl9\esp_lvgl_port_usbhid.c" -Force
```
