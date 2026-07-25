/*
 * miaobox_net.h — 网络同步模块公共接口
 * ======================================
 *
 * 使用方式:
 *   app_main   → net_sync_init()      初始化（创建任务+队列）
 *   UI         → net_sync_trigger()   触发 WiFi+NTP 同步
 *   UI 轮询    → net_sync_get_queue() 获取事件队列，接收 net_evt_t
 *   UI 显示    → net_sync_get_ssid() / net_sync_get_ip()
 *
 * 事件流:
 *   NET_EVT_WIFI_START  → 开始扫描/连接
 *   NET_EVT_WIFI_OK     → WiFi 连接成功 (SSID+IP可用)
 *   NET_EVT_WIFI_FAIL   → WiFi 失败
 *   NET_EVT_NTP_START   → NTP 同步开始
 *   NET_EVT_NTP_OK      → 时间同步成功 → UI 自动跳转
 *   NET_EVT_NTP_FAIL    → 时间同步失败 → UI 显示提示
 *   NET_EVT_NTP_RETRY   → 单台服务器超时，切换下一台 (UI 变橙色)
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    NET_EVT_WIFI_START,
    NET_EVT_WIFI_OK,
    NET_EVT_WIFI_FAIL,
    NET_EVT_NTP_START,
    NET_EVT_NTP_OK,
    NET_EVT_NTP_FAIL,
    NET_EVT_NTP_RETRY,
} net_evt_t;

void        net_sync_init(void);
void        net_sync_trigger(void);
QueueHandle_t net_sync_get_queue(void);
const char *net_sync_get_ssid(void);
const char *net_sync_get_ip(void);
