/*
 * miaobox_console.h — 控制台初始化接口
 * ====================================
 *
 * console_init():
 *   - USB_SERIAL_JTAG: 启动后台 usb_probe_task，每200ms检测USB SOF帧
 *     检测到host后异步初始化driver + linenoise + 注册命令 + 启动REPL task
 *     未检测到则30次(6s)后退出，设备正常启动（日志走ROM默认输出）
 *   - UART: 同步初始化（需外接USB-UART转换器到UART0引脚）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void console_init(const char *prompt_str);

#ifdef __cplusplus
}
#endif
