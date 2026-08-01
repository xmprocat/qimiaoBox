idf_component_register(SRCS "lv_font_alibaba_22.c" "lv_font_alibaba_56.c" "icon_default.c" "icon_setting.c" "icon_debug.c" "app_main.c" "miaobox_ui.c"
                            "miaobox_cmd.c" "miaobox_console.c" "miaobox_net.c" "miaobox_ai.c"
                            "pngdec/PNGdec.c" "pngdec/adler32.c" "pngdec/crc32.c"
                            "pngdec/inflate.c" "pngdec/inftrees.c" "pngdec/inffast.c"
                            "pngdec/zutil.c"
                       INCLUDE_DIRS "." "pngdec"
                       REQUIRES "esp_lcd" "console" "nvs_flash" "esp_driver_usb_serial_jtag"
                                "esp_driver_uart" "esp_wifi" "esp_netif" "esp_event"
                                "esp_http_client" "cjson" "esp-tls" "esp_partition")

# 抑制pngdec内置zlib的fallthrough警告
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-implicit-fallthrough")
