/*
 * miaobox_net.h — 网络模块公共接口
 * ==================================
 *
 * 两种使用方式:
 *
 * 1. NTP 时间同步 (用于 birthday 页面):
 *   app_main   → net_sync_init()      初始化（创建任务+队列）
 *   UI         → net_sync_trigger()   触发 WiFi+NTP 同步流程
 *   UI 轮询    → net_sync_get_queue() 获取事件队列，接收 net_evt_t
 *   UI 显示    → net_sync_get_ssid() / net_sync_get_ip()
 *
 * 2. 通用 WiFi 连接 (用于 AI 画图等 HTTP 请求):
 *   UI         → net_wifi_connect()   连接WiFi，阻塞等待结果
 *   UI         → net_wifi_disconnect() 断开WiFi
 *   UI 查询    → net_wifi_is_connected() 查询连接状态
 *
 * NTP 同步事件流:
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
#include <stdbool.h>

typedef enum {
    NET_EVT_WIFI_START,
    NET_EVT_WIFI_OK,
    NET_EVT_WIFI_FAIL,
    NET_EVT_NTP_START,
    NET_EVT_NTP_OK,
    NET_EVT_NTP_FAIL,
    NET_EVT_NTP_RETRY,
} net_evt_t;

/* ================================================================
 *  NTP 同步 API (原有)
 * ================================================================ */

void        net_sync_init(void);
void        net_sync_trigger(void);
QueueHandle_t net_sync_get_queue(void);
const char *net_sync_get_ssid(void);
const char *net_sync_get_ip(void);

/* ================================================================
 *  通用 WiFi API (新增)
 *  用于 HTTP 请求等需要保持 WiFi 连接的场景
 * ================================================================ */

/**
 * 连接 WiFi (阻塞等待结果)
 * 从 NVS 加载凭据，连接后保持连接状态
 * @param timeout_ms 超时时间(毫秒)，0 表示使用默认 30s
 * @return true=连接成功, false=超时或失败
 */
bool net_wifi_connect(uint32_t timeout_ms);

/**
 * 断开 WiFi
 */
void net_wifi_disconnect(void);

/**
 * 检查 WiFi 是否已连接
 */
bool net_wifi_is_connected(void);

/**
 * 获取当前连接的 SSID
 */
const char *net_wifi_get_ssid(void);

/**
 * 获取当前 IP 地址
 */
const char *net_wifi_get_ip(void);
