/*
 * miaobox_cmd.h — 控制台命令注册
 * ==============================
 *
 * 命令列表 (详见 miaobox_cmd.c):
 *   setcfg <key> str|int <value> — NVS 键值存储
 *   getcfg <key>                 — 读取并显示
 *   delcfg <key>                 — 删除
 *   factoryreset                 — 擦除全部 NVS + 重启
 *   reboot                       — 重启
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void register_miaobox_commands(void);

#ifdef __cplusplus
}
#endif
