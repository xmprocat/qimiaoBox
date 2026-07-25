/*
 * app_main.c — 硬件初始化 + 主入口
 * ================================
 *
 * 初始化流程:
 *   1. NVS flash 初始化（WiFi凭据/配置存储）
 *   2. Console（USB Serial JTAG, 异步探测USB连接）
 *   3. SPI 总线 (SPI2_HOST, 40MHz)
 *   4. ST7789 LCD 面板驱动 (240×240, RGB565)
 *   5. LVGL 库 + 显示 (双缓冲 60行, 部分刷新)
 *   6. LVGL tick 定时器 (esp_timer 2ms)
 *   7. 硬件按键 (GPIO 18-21, active-low, iot_button 库消抖)
 *   8. LVGL keypad 输入设备
 *   9. LVGL FreeRTOS 移植任务 (prio=2, 8KB stack)
 *  10. 网络同步模块初始化 (miaobox_net)
 *  11. UI 入口 miaobox_ui(display)
 *
 * 引脚分配:
 *   SCLK=0, MOSI=7, DC=4, RST=6, CS=5
 *   KEY_ESC=18, KEY_ENTER=20, KEY_DOWN=21, KEY_UP=19
 *
 * 线程:
 *   - LVGL task:      lv_timer_handler + UI 渲染 (prio=2)
 *   - net task:       WiFi 扫描/连接/NTP 同步 (prio=2)
 *   - console/usb_probe: 控制台 REPL / USB 探测 (prio=2)
 *   - button 回调:    硬件按键中断上下文 → 设置 volatile 标志
 *
 * 线程安全:
 *   LVGL 非线程安全 → _lock_t lvgl_api_lock 保护所有 lv_* 调用
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "miaobox_net.h"
#include "nvs_flash.h"
#include "miaobox_console.h"

static const char *TAG = "miaobox";

// Using SPI2 in the example
#define LCD_HOST  SPI2_HOST

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  0
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_SCLK           0
#define EXAMPLE_PIN_NUM_MOSI           7
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_LCD_DC         4
#define EXAMPLE_PIN_NUM_LCD_RST        6
#define EXAMPLE_PIN_NUM_LCD_CS         5
#define EXAMPLE_PIN_NUM_BK_LIGHT       -1
// Buttons (active-low with internal pull-up)
#define EXAMPLE_PIN_NUM_KEY_ESC        18
#define EXAMPLE_PIN_NUM_KEY_ENTER      20
#define EXAMPLE_PIN_NUM_KEY_DOWN       21
#define EXAMPLE_PIN_NUM_KEY_UP         19

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              240
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8

#define EXAMPLE_LVGL_DRAW_BUF_LINES    60 // number of display lines in each draw buffer
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 2
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

// LVGL library is not thread-safe, this example will call LVGL APIs from different tasks, so use a mutex to protect it
static _lock_t lvgl_api_lock;

extern void miaobox_ui(lv_disp_t *disp);

/* Console is initialized via console_init() — see miaobox_console.c */

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // because SPI LCD is big-endian, we need to swap the RGB bytes order
    // lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

/* Button handles and their LVGL key mapping */
static button_handle_t key_btns[4];
static uint32_t key_map[4];
volatile int esc_repeat_reset;  /* set by PRESS_UP, read by miaobox_ui */

static void esc_up_cb(void *btn, void *data)
{
    esc_repeat_reset = 1;
}

static void example_keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* Level-triggered: report PRESSED while any button is held (debounced by button lib) */
    for (int i = 0; i < 4; i++) {
        if (iot_button_get_key_level(key_btns[i])) {
            data->key = key_map[i];
            data->state = LV_INDEV_STATE_PRESSED;
            return;
        }
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler_run_in_period(10);
        _lock_release(&lvgl_api_lock);
        // in case of triggering a task watch dog time out
        time_till_next_ms = MAX(time_till_next_ms, EXAMPLE_LVGL_TASK_MIN_DELAY_MS);
        // in case of lvgl display not ready yet
        time_till_next_ms = MIN(time_till_next_ms, EXAMPLE_LVGL_TASK_MAX_DELAY_MS);
        // Ensure at least 1 FreeRTOS tick to allow context switch (100Hz = 10ms/tick)
        uint32_t ticks = pdMS_TO_TICKS(time_till_next_ms);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
    }
}

void app_main(void)
{
    /* ---- NVS ---- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS initialized");

    /* ---- Console (USB: async probe; UART: immediate) ---- */
    console_init("miao>");

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,
        .miso_io_num = EXAMPLE_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_DRAW_BUF_LINES * sizeof(uint16_t),  /* keep under C6 DMA 32KB limit */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_LCD_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));

    // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    if (EXAMPLE_PIN_NUM_BK_LIGHT >= 0) {
        ESP_LOGI(TAG, "Turn on LCD backlight");
        gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    // create a lvgl display
    lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);

    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    size_t draw_buffer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);

    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    // initialize LVGL draw buffers
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    // associate the mipi panel handle to the display
    lv_display_set_user_data(display, panel_handle);
    // set color depth
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    // set the callback which can copy the rendered image to an area of the display
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = example_notify_lvgl_flush_ready,
    };
    /* Register done callback */
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

    /* Register hardware buttons via button library (built-in debounce) */
    {
        static const struct {
            int gpio;
            uint32_t lv_key;
        } btn_map[] = {
            {EXAMPLE_PIN_NUM_KEY_ESC,   LV_KEY_ESC},
            {EXAMPLE_PIN_NUM_KEY_ENTER, LV_KEY_ENTER},
            {EXAMPLE_PIN_NUM_KEY_DOWN,  LV_KEY_DOWN},
            {EXAMPLE_PIN_NUM_KEY_UP,    LV_KEY_UP},
        };

        for (int i = 0; i < 4; i++) {
            button_config_t cfg = {
                .long_press_time = 1000,
                .short_press_time = 50,
            };
            button_gpio_config_t gpio_cfg = {
                .gpio_num = btn_map[i].gpio,
                .active_level = 0,
            };
            iot_button_new_gpio_device(&cfg, &gpio_cfg, &key_btns[i]);
            key_map[i] = btn_map[i].lv_key;
        }
        /* ESC release resets long-press counter */
        iot_button_register_cb(key_btns[0], BUTTON_PRESS_UP, NULL, esc_up_cb, NULL);

        lv_indev_t *keypad = lv_indev_create();
        lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_display(keypad, display);
        lv_indev_set_read_cb(keypad, example_keypad_read_cb);
        ESP_LOGI(TAG, "Keypad input registered via button library");
    }

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Init network sync module");
    net_sync_init();

    ESP_LOGI(TAG, "Display Key Test UI");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    _lock_acquire(&lvgl_api_lock);
    miaobox_ui(display);
    _lock_release(&lvgl_api_lock);
}
