# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

miaomiaoBox is an ESP32-C6 firmware project that drives an ST7789 240×240 SPI LCD with an LVGL-based touch/keypad UI. It is built on the ESP-IDF framework (v6.0.1) and uses component registry dependencies (LVGL 9.3.0, espressif/button 4.x).

## Build & Flash Commands

The ESP-IDF environment must be active (the `idf.py` command must be available on `PATH`):

```bash
# Configure the project (opens menuconfig)
idf.py menuconfig

# Build
idf.py build

# Flash to device (over JTAG on COM21, target esp32c6)
idf.py flash

# Build + flash + open serial monitor
idf.py build flash monitor

# Serial monitor only (Ctrl+] to exit)
idf.py monitor

# Clean build artifacts
idf.py fullclean
```

The VS Code settings (`.vscode/settings.json`) configure the ESP-IDF plugin:
- IDF_PATH: `C:\esp-idf\v6.0.1\esp-idf`
- Target: `esp32c6`
- Port: `COM21`
- Flash method: JTAG

## Architecture

### Initialization Flow (`main/app_main.c`)

`app_main()` is the entry point and follows this sequence:
1. Initialize SPI bus (SPI2_HOST) at 40 MHz — shared between LCD and optional touch controller
2. Create LCD panel IO handle (SPI transport), then install ST7789 panel driver
3. Power on display, initialize LVGL library (`lv_init`)
4. Create LVGL display with double-buffered partial-render mode (60-line draw buffers, RGB565)
5. Register LVGL flush callback → `esp_lcd_panel_draw_bitmap()`
6. Set up LVGL tick timer via `esp_timer` (2ms periodic)
7. Optionally initialize touch controller (STMPE610/XPT2046) on the same SPI bus
8. Register hardware buttons (ESC/ENTER/DOWN/UP on GPIO 18-21) via espressif/button library
9. Create the LVGL port task (FreeRTOS task at priority 2, 8KB stack)
10. Call `miaobox_ui(display)` to build the UI

### LVGL Thread Safety

LVGL is NOT thread-safe. A `_lock_t` mutex (`lvgl_api_lock`) guards all LVGL API calls:
- The LVGL port task acquires it before calling `lv_timer_handler_run_in_period()`
- `app_main()` acquires it before calling `miaobox_ui()`

### UI Architecture (`main/miaobox_ui.c`)

`miaobox_ui()` is the UI entry point called once at startup:
1. Creates an LVGL input group and binds the keypad input device to it
2. Registers `key_event_cb` on the active screen for `LV_EVENT_KEY`
3. Shows a splash screen ("MiaomiaoBox") for 1000ms, then transitions to page 0

**Page system:**
- Page 0 (Key Test): Four colored boxes (ESC/ENTER/DOWN/UP) that cycle colors (red→green→white) on key press
- Page 1 (Birthday countdown): Shows days until the configured birthday (month/day)
- Long-press ESC (≥2 repeated KEY events) toggles between pages
- ESC release resets the long-press counter and re-enables switching

**Key input model:** Level-triggered via `iot_button_get_key_level()` — keys repeat while held. Debounce, long-press detection, and release callbacks are handled by espressif/button.

### Pin Configuration (edit in `main/app_main.c`)

| Signal | GPIO | Notes |
|--------|------|-------|
| SCLK   | 0    | SPI clock |
| MOSI   | 7    | SPI data |
| LCD_DC | 4    | Data/Command |
| LCD_RST| 6    | Reset |
| LCD_CS | 5    | Chip select |
| KEY_ESC| 18   | Active low, internal pull-up |
| KEY_ENTER|20 | Active low |
| KEY_DOWN| 21  | Active low |
| KEY_UP | 19   | Active low |

### Build System

- Root `CMakeLists.txt`: includes ESP-IDF project, enables `MINIMAL_BUILD`
- `main/CMakeLists.txt`: registers `app_main.c` + `miaobox_ui.c`, requires `esp_lcd`
- `main/idf_component.yml`: declares LVGL 9.3.0 and espressif/button >=4.0 dependencies
- `sdkconfig.defaults`: enables LVGL observer, sysmon, and perf monitor features
- LVGL is configured via `sdkconfig` (not `lv_conf.h`) — `CONFIG_LV_CONF_SKIP=y` skips the header-based config

### Kconfig (`main/Kconfig.projbuild`)

Adds an "Example Configuration" menu with options for touch enable/disable, touch controller selection (STMPE610/XPT2046), and mirroring. Access via `idf.py menuconfig`.

### Font (`main/lv_font_alibaba_22.c`)

UI 使用的汉字存储在 `main/lv_font_alibaba_22.c`，由阿里巴巴普惠体通过 `lv_font_conv` 生成。**每次在 UI 中新增汉字时，必须重新生成字体文件**，否则新字在屏幕上显示为空白。

生成命令（在项目根目录执行）：

```bash
lv_font_conv --size 22 --bpp 4 --format lvgl \
  --font font/Alibaba-PuHuiTi-Regular.ttf \
  -r 0x20-0x7F -r 0x3010-0x3011 \
  --symbols "<所有需要用到的汉字>" \
  --lv-include lvgl.h --lv-font-name lv_font_alibaba_22 \
  --no-compress --force-fast-kern-format \
  -o main/lv_font_alibaba_22.c
```

`--symbols` 参数列出所有需要的中文字符（空格分隔）。当前已包含的汉字可从生成的 `.c` 文件头部注释中查到。新增字符时追加到 `--symbols` 末尾即可。

## Key Files

| File | Purpose |
|------|---------|
| `main/app_main.c` | Hardware init (SPI, LCD, touch, buttons, LVGL setup) |
| `main/miaobox_ui.c` | All UI logic — splash, pages, key handling |
| `main/CMakeLists.txt` | Component registration and dependencies |
| `main/idf_component.yml` | Component registry dependencies |
| `main/Kconfig.projbuild` | Menuconfig options |
| `sdkconfig.defaults` | Default LVGL Kconfig overrides |
