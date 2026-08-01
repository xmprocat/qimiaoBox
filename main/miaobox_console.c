/*
 * miaobox_console.c — 控制台初始化 (USB Serial JTAG / UART)
 * ==========================================================
 *
 * 两阶段初始化:
 *   console_init():         启动USB探测任务（后台）
 *   console_start_repl():   等待探测完成 → 初始化外设 → 创建REPL task
 *                           （在app_main末尾调用，确保WiFi日志洪峰已过）
 *
 * REPL task:
 *   linenoise dumb mode 阻塞读 stdin → esp_console_run() → 执行命令
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
#include "driver/uart_vfs.h"
#include "driver/uart.h"
#endif

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "soc/usb_serial_jtag_reg.h"
#endif

static const char *TAG = "console";

#define CONSOLE_MAX_CMDLINE_ARGS    8
#define CONSOLE_MAX_CMDLINE_LENGTH 256
#define CONSOLE_PROMPT_MAX_LEN      32
#define CONSOLE_REPL_STACK         4096
#define CONSOLE_REPL_PRIO             2

static char s_prompt[CONSOLE_PROMPT_MAX_LEN];

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
static bool s_usb_ready;
#endif

extern void register_miaobox_commands(void);

/* ---- peripheral init (called synchronously for UART, or async for USB) ---- */

static bool console_peripheral_init(void)
{
    fflush(stdout);

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
#if SOC_UART_SUPPORT_REF_TICK
        .source_clk = UART_SCLK_REF_TICK,
#elif SOC_UART_SUPPORT_XTAL_CLK
        .source_clk = UART_SCLK_XTAL,
#endif
    };
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t jtag_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&jtag_config));
    usb_serial_jtag_vfs_use_driver();

    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, 0);
#endif

    setvbuf(stdin, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "Console peripheral initialized");
    return true;
}

static void console_library_init(const char *history_path)
{
    esp_console_config_t cfg = {
        .max_cmdline_args   = CONSOLE_MAX_CMDLINE_ARGS,
        .max_cmdline_length = CONSOLE_MAX_CMDLINE_LENGTH,
#if CONFIG_LOG_COLORS
        .hint_color = atoi(LOG_COLOR_CYAN),
#endif
    };
    ESP_ERROR_CHECK(esp_console_init(&cfg));

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(100);
    linenoiseSetMaxLineLen(cfg.max_cmdline_length);
    linenoiseAllowEmpty(false);
    linenoiseSetDumbMode(1);   /* safe for USB / no terminal probe */

    if (history_path) {
        linenoiseHistoryLoad(history_path);
    }
    ESP_LOGI(TAG, "Console library initialized");
}

static void setup_prompt(const char *str)
{
    if (!str) str = "miaobox>";
#if CONFIG_LOG_COLORS
    snprintf(s_prompt, sizeof(s_prompt) - 1,
             LOG_COLOR_I "%s " LOG_RESET_COLOR, str);
#else
    snprintf(s_prompt, sizeof(s_prompt) - 1, "%s ", str);
#endif
}

/* ---- REPL task ---- */

static void console_repl_task(void *arg)
{
    (void)arg;
    while (1) {
        char *line = linenoise(s_prompt);
        if (line == NULL) continue;
        if (strlen(line) > 0) linenoiseHistoryAdd(line);

        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command error: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

/* ---- USB probe task (async detection) ---- */

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
static void usb_probe_task(void *arg)
{
    (void)arg;

    for (int retry = 0; retry < 30; retry++) {   /* up to 6 s */
        uint16_t sof0 = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
        vTaskDelay(pdMS_TO_TICKS(200));
        uint16_t sof1 = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
        if (sof0 != sof1) {
            s_usb_ready = true;
            ESP_LOGI(TAG, "USB detected, console will start after init sequence");
            break;
        }
    }

    if (!s_usb_ready) {
        ESP_LOGW(TAG, "USB not detected — console disabled");
    }

    vTaskDelete(NULL);
}
#endif

/* ---- public API ---- */

void console_init(void)
{
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    xTaskCreate(usb_probe_task, "usb_probe", 2048, NULL, 2, NULL);
#endif
}

void console_start_repl(const char *prompt_str)
{
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    int retry = 0;
    while (!s_usb_ready && retry < 30) {
        vTaskDelay(pdMS_TO_TICKS(200));
        retry++;
    }
    if (!s_usb_ready) {
        ESP_LOGW(TAG, "USB not detected, console REPL not started");
        return;
    }
#endif

    console_peripheral_init();
    console_library_init(NULL);
    setup_prompt(prompt_str);
    esp_console_register_help_command();
    register_miaobox_commands();

    BaseType_t ret = xTaskCreate(console_repl_task, "console", CONSOLE_REPL_STACK,
                NULL, CONSOLE_REPL_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console_repl_task! ret=%d", ret);
    } else {
        ESP_LOGI(TAG, "console_repl_task created successfully");
    }
}
