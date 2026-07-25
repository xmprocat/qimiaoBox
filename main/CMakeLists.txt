idf_component_register(SRCS "lv_font_alibaba_22.c" "app_main.c" "miaobox_ui.c"
                            "miaobox_cmd.c" "miaobox_console.c" "miaobox_net.c"
                       INCLUDE_DIRS "."
                       REQUIRES "esp_lcd" "console" "nvs_flash" "esp_driver_usb_serial_jtag"
                                "esp_wifi" "esp_netif" "esp_event")
