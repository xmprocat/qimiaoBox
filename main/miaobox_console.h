/*
 * miaobox_console.h — 控制台初始化接口
 * ====================================
 *
 * console_init():         启动USB探测（后台），不做外设初始化
 * console_start_repl():   等待USB就绪 → 初始化外设/linenoise/命令 → 创建REPL task
 *                         在所有HW/WiFi初始化完成后调用，避免日志洪峰竞争TX buffer
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void console_init(void);
void console_start_repl(const char *prompt_str);

#ifdef __cplusplus
}
#endif
