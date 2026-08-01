/*
 * miaobox_net.c — 统一网络管理模块
 * ===================================
 *
 * 架构设计:
 *   - 统一的 WiFi 连接核心逻辑（凭据加载→扫描匹配→连接→获取IP）
 *   - 连接成功后通过事件队列通知所有监听者
 *   - 不同业务模块（NTP/AI等）根据需求执行不同后续操作
 *
 * 使用方式:
 *   1. NTP 时间同步: net_sync_trigger() → 连接→NTP→断开
 *   2. 通用WiFi连接: net_wifi_connect() → 连接→保持→调用者断开
 *
 * Dependencies:
 *   esp_wifi, esp_netif, esp_sntp, nvs_flash, FreeRTOS
 */

#include "miaobox_net.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "miaobox_net";

/* ================================================================
 *  配置常量
 * ================================================================ */

#define NVS_NAMESPACE         "cfg"         /* NVS 存储命名空间 */
#define NVS_KEY_TIMEOUT       "ntp.timeout" /* 总超时 NVS key */
#define MAX_WIFI_CREDS        5             /* 最多存储的 WiFi 凭据组数 */
#define DEFAULT_TIMEOUT_MS    60000         /* NTP 总超时默认 60s */
#define PER_SERVER_TIMEOUT_MS 10000         /* 单台服务器超时 10s */
#define PER_SRV_MS            (PER_SERVER_TIMEOUT_MS)

/* ================================================================
 *  NTP 服务器列表（按优先级排列）
 *  国家授时中心 → 云厂商 → 教育网 → 国际公共
 * ================================================================ */

static const char *ntp_servers[] = {
    "ntp.ntsc.ac.cn",       /* 国家授时中心 1 */
    "time.ntsc.ac.cn",      /* 国家授时中心 2 */
    "ntp.aliyun.com",       /* 阿里云 */
    "ntp1.aliyun.com",
    "ntp2.aliyun.com",
    "ntp.tencent.com",      /* 腾讯云 */
    "ntp1.tencent.com",
    "ntp1.baidu.com",       /* 百度云 */
    "ntp2.baidu.com",
    "time.myhuaweicloud.com", /* 华为云 */
    "ntp1.edu.cn",          /* 教育网 */
    "ntp2.edu.cn",
    "pool.ntp.org",         /* 国际公共（兜底） */
};
#define NTP_NUM_SERVERS (sizeof(ntp_servers) / sizeof(ntp_servers[0]))

/* ================================================================
 *  内部状态
 * ================================================================ */

/* 事件队列 - 通知UI和业务模块 */
static QueueHandle_t s_evt_queue = NULL;

/* WiFi连接状态 */
static EventGroupHandle_t s_wifi_evt_group = NULL;
static bool s_wifi_connected = false;
static char s_ssid[33] = {0};
static char s_ip[16] = {0};

/* NTP同步状态 */
static esp_timer_handle_t s_ntp_timer = NULL;
static bool s_ntp_ok = false;
static bool s_ntp_done = false;
static int s_ntp_server_idx = 0;
static int s_ntp_elapsed_ms = 0;
static int s_ntp_total_ms = 0;

/* NTP任务 */
static TaskHandle_t s_ntp_task = NULL;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ================================================================
 *  NVS 工具函数
 * ================================================================ */

static void read_nvs_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    size_t len = out_len;
    nvs_get_str(h, key, out, &len);
    nvs_close(h);
}

static int read_nvs_timeout(void)
{
    nvs_handle_t h;
    int32_t val = DEFAULT_TIMEOUT_MS;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return val;
    nvs_get_i32(h, NVS_KEY_TIMEOUT, &val);
    nvs_close(h);
    return (int)val;
}

/* ================================================================
 *  WiFi凭据加载（统一逻辑）
 * ================================================================ */

static int load_wifi_creds(char ssids[][33], char pwds[][65])
{
    int count = 0;
    for (int i = 1; i <= MAX_WIFI_CREDS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "wifi.ssid.%d", i);
        read_nvs_str(key, ssids[count], 33);
        if (ssids[count][0] == '\0') continue;
        snprintf(key, sizeof(key), "wifi.pwd.%d", i);
        read_nvs_str(key, pwds[count], 65);
        count++;
    }
    if (count == 0) {
        read_nvs_str("wifi.ssid", ssids[0], 33);
        read_nvs_str("wifi.pwd", pwds[0], 65);
        if (ssids[0][0]) count = 1;
    }
    return count;
}

/* ================================================================
 *  事件发送
 * ================================================================ */

static void send_evt(net_evt_t evt)
{
    if (s_evt_queue) {
        xQueueSend(s_evt_queue, &evt, 0);
    }
}

/* ================================================================
 *  WiFi事件处理器（统一）
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* WiFi启动后自动连接 */
        ESP_LOGI(TAG, "WiFi started, connecting...");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 断开 */
        s_wifi_connected = false;
        if (s_wifi_evt_group) {
            xEventGroupSetBits(s_wifi_evt_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* 获取IP - 连接成功 */
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip, sizeof(s_ip));
        s_wifi_connected = true;
        if (s_wifi_evt_group) {
            xEventGroupSetBits(s_wifi_evt_group, WIFI_CONNECTED_BIT);
        }
        send_evt(NET_EVT_WIFI_OK);
        ESP_LOGI(TAG, "WiFi connected, IP: %s", s_ip);
    }
}

/* ================================================================
 *  WiFi核心连接函数（统一逻辑）
 *
 *  1. 加载凭据
 *  2. 扫描匹配最佳AP
 *  3. 连接并等待结果
 *
 *  @return true=连接成功, false=失败
 * ================================================================ */

static bool wifi_connect_core(uint32_t timeout_ms)
{
    if (timeout_ms == 0) timeout_ms = 30000;

    /* 已连接则直接返回 */
    if (s_wifi_connected) {
        ESP_LOGI(TAG, "WiFi already connected");
        return true;
    }

    /* 创建事件组 */
    if (!s_wifi_evt_group) {
        s_wifi_evt_group = xEventGroupCreate();
    }
    xEventGroupClearBits(s_wifi_evt_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* 1. 加载凭据 */
    char cred_ssids[MAX_WIFI_CREDS][33] = {{0}};
    char cred_pwds[MAX_WIFI_CREDS][65] = {{0}};
    int cred_count = load_wifi_creds(cred_ssids, cred_pwds);

    if (cred_count == 0) {
        ESP_LOGE(TAG, "No WiFi credentials in NVS");
        return false;
    }

    /* 2. 清理旧状态 */
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 3. 扫描AP */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Scanning WiFi APs...");
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));

    uint16_t ap_num = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
    wifi_ap_record_t *ap_list = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (ap_list) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_list));
    }

    /* 4. 匹配最佳AP */
    char *chosen_ssid = NULL, *chosen_pwd = NULL;
    for (int i = 0; i < cred_count && !chosen_ssid; i++) {
        for (int j = 0; j < ap_num && !chosen_ssid; j++) {
            if (strcmp((char *)ap_list[j].ssid, cred_ssids[i]) == 0) {
                chosen_ssid = cred_ssids[i];
                chosen_pwd = cred_pwds[i];
                ESP_LOGI(TAG, "Matched AP: %s (rssi=%d)",
                         chosen_ssid, ap_list[j].rssi);
            }
        }
    }

    if (ap_list) free(ap_list);
    esp_wifi_scan_stop();

    /* 无匹配则用第一组凭据 */
    if (!chosen_ssid) {
        chosen_ssid = cred_ssids[0];
        chosen_pwd = cred_pwds[0];
        ESP_LOGI(TAG, "No scan match, trying: %s", chosen_ssid);
    }

    strncpy(s_ssid, chosen_ssid, sizeof(s_ssid) - 1);
    send_evt(NET_EVT_WIFI_START);

    /* 5. 连接 */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, chosen_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, chosen_pwd, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WEP;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to %s...", chosen_ssid);

    /* 6. 等待结果 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }

    ESP_LOGW(TAG, "WiFi connection timeout");
    esp_wifi_disconnect();
    esp_wifi_stop();
    send_evt(NET_EVT_WIFI_FAIL);
    return false;
}

/* ================================================================
 *  WiFi核心断开函数
 * ================================================================ */

static void wifi_disconnect_core(void)
{
    if (!s_wifi_connected) return;

    ESP_LOGI(TAG, "Disconnecting WiFi");
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_connected = false;

    if (s_wifi_evt_group) {
        vEventGroupDelete(s_wifi_evt_group);
        s_wifi_evt_group = NULL;
    }
}

/* ================================================================
 *  NTP 轮询逻辑
 * ================================================================ */

static void ntp_try_next_server(void)
{
    if (s_ntp_server_idx >= (int)NTP_NUM_SERVERS - 1) return;
    esp_sntp_stop();
    s_ntp_server_idx++;
    s_ntp_elapsed_ms = 0;
    esp_sntp_setservername(0, ntp_servers[s_ntp_server_idx]);
    esp_sntp_init();
    send_evt(NET_EVT_NTP_RETRY);
    ESP_LOGI(TAG, "NTP switching to [%d/%d]: %s",
             s_ntp_server_idx + 1, (int)NTP_NUM_SERVERS,
             ntp_servers[s_ntp_server_idx]);
}

static void ntp_poll_cb(void *arg)
{
    s_ntp_elapsed_ms += 500;
    s_ntp_total_ms += 500;
    sntp_sync_status_t status = esp_sntp_get_sync_status();

    if (status == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "NTP sync OK via %s", ntp_servers[s_ntp_server_idx]);
        s_ntp_ok = true;
        s_ntp_done = true;
        esp_timer_stop(s_ntp_timer);
        send_evt(NET_EVT_NTP_OK);
        return;
    }

    if (s_ntp_elapsed_ms >= PER_SRV_MS) {
        if (s_ntp_total_ms >= read_nvs_timeout()) {
            ESP_LOGW(TAG, "NTP total timeout");
            s_ntp_done = true;
            esp_timer_stop(s_ntp_timer);
            send_evt(NET_EVT_NTP_FAIL);
            return;
        }
        ntp_try_next_server();
    }
}

static void start_ntp_poll(void)
{
    s_ntp_server_idx = 0;
    s_ntp_elapsed_ms = 0;
    s_ntp_total_ms = 0;
    s_ntp_done = false;
    s_ntp_ok = false;

    const esp_timer_create_args_t args = {
        .callback = ntp_poll_cb,
        .name = "ntp_poll",
    };
    if (s_ntp_timer) esp_timer_delete(s_ntp_timer);
    esp_timer_create(&args, &s_ntp_timer);
    esp_timer_start_periodic(s_ntp_timer, 500 * 1000);
}

/* ================================================================
 *  NTP同步任务
 *
 *  流程: 连接WiFi → 启动NTP → 等待完成 → 断开WiFi
 * ================================================================ */

static void ntp_task(void *arg)
{
    while (1) {
        /* 等待触发 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Starting NTP sync");
        esp_sntp_stop();

        /* 1. 连接WiFi */
        if (!wifi_connect_core(30000)) {
            ESP_LOGE(TAG, "WiFi connect failed for NTP");
            send_evt(NET_EVT_NTP_FAIL);
            continue;
        }

        /* 2. 启动NTP */
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, ntp_servers[0]);
        esp_sntp_init();
        start_ntp_poll();

        /* 3. 等待NTP完成 */
        while (!s_ntp_done) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (s_ntp_timer) {
            esp_timer_stop(s_ntp_timer);
            esp_timer_delete(s_ntp_timer);
            s_ntp_timer = NULL;
        }

        /* 4. 断开WiFi */
        esp_sntp_stop();
        wifi_disconnect_core();

        ESP_LOGI(TAG, "NTP sync %s", s_ntp_ok ? "OK" : "failed");
    }
}

/* ================================================================
 *  公共API - NTP同步
 * ================================================================ */

void net_sync_init(void)
{
    s_evt_queue = xQueueCreate(10, sizeof(net_evt_t));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    xTaskCreate(ntp_task, "ntp", 4096, NULL, 2, &s_ntp_task);
    ESP_LOGI(TAG, "Network module initialized");
}

void net_sync_trigger(void)
{
    if (s_ntp_task) xTaskNotifyGive(s_ntp_task);
}

QueueHandle_t net_sync_get_queue(void) { return s_evt_queue; }
const char *net_sync_get_ssid(void) { return s_ssid; }
const char *net_sync_get_ip(void) { return s_ip; }

/* ================================================================
 *  公共API - 通用WiFi连接
 *
 *  使用统一的 wifi_connect_core() 连接WiFi
 *  连接后保持连接，由调用者负责断开
 * ================================================================ */

bool net_wifi_connect(uint32_t timeout_ms)
{
    return wifi_connect_core(timeout_ms);
}

void net_wifi_disconnect(void)
{
    wifi_disconnect_core();
}

bool net_wifi_is_connected(void)
{
    return s_wifi_connected;
}

const char *net_wifi_get_ssid(void)
{
    return s_ssid;
}

const char *net_wifi_get_ip(void)
{
    return s_ip;
}
