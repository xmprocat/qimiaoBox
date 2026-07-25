/*
 * miaobox_net.c — WiFi 扫描 + 多凭据匹配 + NTP 时间同步模块
 * =================================================================
 *
 * 整体流程（由 UI 触发）:
 *   1. 从 NVS 加载 wifi.ssid.{1..N} / wifi.pwd.{1..N} 多组凭据
 *   2. WiFi STA 模式下扫描周围 AP（抑制自动连接）
 *   3. 扫描结果 vs NVS 凭据匹配，取第一个命中的
 *   4. 用命中的凭据连接 WiFi → 获取 IP → 启动 SNTP
 *   5. SNTP 轮询 13 台服务器（每台 10s 超时，总可配置超时）
 *   6. 同步成功/失败后关闭 WiFi 释放内存
 *
 * 事件驱动:
 *   WiFi / IP 事件 → wifi_event_handler() → 设标志 + 发事件到队列
 *   NTP 轮询定时器（500ms）→ ntp_poll_cb() → 检查同步状态
 *
 * 线程模型:
 *   - net_task:      FreeRTOS 任务，阻塞等待 UI 触发，执行同步流程
 *   - wifi_event:    WiFi 驱动内部线程，仅发事件通知
 *   - ntp_poll_timer: esp_timer 回调，周期检查 NTP 状态
 *
 * Dependencies:
 *   esp_wifi, esp_netif, esp_sntp, nvs_flash, FreeRTOS
 */

#include "miaobox_net.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static QueueHandle_t      s_evt_queue;      /* 事件队列，发给 UI */
static TaskHandle_t       s_net_task;       /* net_task 句柄 */
static esp_timer_handle_t s_timeout_timer;  /* NTP 轮询定时器 */
static bool s_wifi_ok;                      /* WiFi 连接是否成功 */
static bool s_ntp_ok;                       /* NTP 同步是否成功 */
static bool s_ntp_done;                     /* NTP 同步已完成（成功或失败） */
static bool s_scanning;                     /* 扫描阶段标志，抑制自动连接 */
static char s_ssid[33];                     /* 当前连接的 SSID（UI 显示用） */
static char s_ip[16];                       /* 当前获取的 IP（UI 显示用） */

/* ---- NTP 轮询状态 ---- */
static int  s_ntp_server_idx;   /* 当前服务器索引 */
static int  s_ntp_elapsed_ms;   /* 当前服务器已等待时间 */
static int  s_ntp_total_ms;     /* 全部服务器总耗时 */
static bool s_ntp_retrying;     /* 是否已发生过服务器切换 */

/* ================================================================
 *  NVS 读写工具函数
 * ================================================================ */

/* 从 NVS "cfg" 命名空间读取字符串值，失败时返回空字符串 */
static void read_nvs_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = out_len;
    nvs_get_str(h, key, out, &len);
    nvs_close(h);
}

/* 读取 NTP 总超时配置，默认 DEFAULT_TIMEOUT_MS */
static int read_nvs_timeout(void)
{
    nvs_handle_t h;
    int32_t val = DEFAULT_TIMEOUT_MS;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return val;
    nvs_get_i32(h, NVS_KEY_TIMEOUT, &val);
    nvs_close(h);
    return (int)val;
}

/**
 * 加载全部 WiFi 凭据:
 *   优先读取 wifi.ssid.{1..MAX} / wifi.pwd.{1..MAX}
 *   如果一组都没读出来，回退读取 wifi.ssid / wifi.pwd（无后缀）
 * @return 有效凭据组数
 */
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
    /* 向后兼容：无后缀的旧格式 */
    if (count == 0) {
        read_nvs_str("wifi.ssid", ssids[0], 33);
        read_nvs_str("wifi.pwd", pwds[0], 65);
        if (ssids[0][0]) count = 1;
    }
    return count;
}

/* ---- 发送事件到 UI 队列（非阻塞） ---- */
static void send_evt(net_evt_t evt)
{
    xQueueSend(s_evt_queue, &evt, 0);
}

/* ================================================================
 *  NTP 轮询 — 多服务器循环策略
 *
 *  每 500ms 检查一次:
 *    - 同步完成 → NTP_OK，停止定时器
 *    - 当前服务器超时 10s 且总时长未超 → 切到下一台
 *    - 总时长超限 → NTP_FAIL，停止定时器
 * ================================================================ */

/**
 * 切换到下一台 NTP 服务器:
 *   停止当前 SNTP → 改 servername → 重新 init
 *   通知 UI 显示橙色（NET_EVT_NTP_RETRY）
 */
static void ntp_try_next_server(void)
{
    if (s_ntp_server_idx >= (int)NTP_NUM_SERVERS - 1) return;
    esp_sntp_stop();
    s_ntp_server_idx++;
    s_ntp_elapsed_ms = 0;
    esp_sntp_setservername(0, ntp_servers[s_ntp_server_idx]);
    esp_sntp_init();
    s_ntp_retrying = true;
    send_evt(NET_EVT_NTP_RETRY);
    ESP_LOGI(TAG, "NTP switching to [%d/%d]: %s",
             s_ntp_server_idx + 1, (int)NTP_NUM_SERVERS,
             ntp_servers[s_ntp_server_idx]);
}

/* 500ms 周期定时器回调 — 检查 SNTP 同步状态 */
static void ntp_poll_cb(void *arg)
{
    s_ntp_elapsed_ms += 500;
    s_ntp_total_ms   += 500;
    sntp_sync_status_t status = esp_sntp_get_sync_status();

    if (status == SNTP_SYNC_STATUS_COMPLETED || (s_ntp_total_ms % 2000 == 0)) {
        ESP_LOGI(TAG, "NTP[%d/%d] %s: %d ms, status=%d",
                 s_ntp_server_idx + 1, (int)NTP_NUM_SERVERS,
                 ntp_servers[s_ntp_server_idx],
                 s_ntp_elapsed_ms, (int)status);
    }

    /* 同步完成 */
    if (status == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "NTP sync OK via %s", ntp_servers[s_ntp_server_idx]);
        s_ntp_ok   = true;
        s_ntp_done = true;
        esp_timer_stop(s_timeout_timer);
        send_evt(NET_EVT_NTP_OK);
        return;
    }

    /* 单台服务器超时? */
    if (s_ntp_elapsed_ms >= PER_SRV_MS) {
        /* 总时长超限? */
        if (s_ntp_total_ms >= read_nvs_timeout()) {
            ESP_LOGW(TAG, "NTP total timeout (%d ms, %d servers tried)",
                     s_ntp_total_ms, s_ntp_server_idx + 1);
            s_ntp_done = true;
            esp_timer_stop(s_timeout_timer);
            send_evt(NET_EVT_NTP_FAIL);
            return;
        }
        /* 还没超——切下一台服务器 */
        ntp_try_next_server();
    }
}

/* 启动 NTP 轮询定时器（500ms 周期） */
static void start_ntp_poll_timer(void)
{
    s_ntp_server_idx = 0;
    s_ntp_elapsed_ms = 0;
    s_ntp_total_ms   = 0;
    s_ntp_done       = false;
    s_ntp_retrying   = false;
    const esp_timer_create_args_t args = {
        .callback = ntp_poll_cb,
        .name     = "ntp_poll",
    };
    if (s_timeout_timer) esp_timer_delete(s_timeout_timer);
    esp_timer_create(&args, &s_timeout_timer);
    esp_timer_start_periodic(s_timeout_timer, 500 * 1000);
}

/* 停止并删除 NTP 轮询定时器 */
static void cancel_ntp_poll_timer(void)
{
    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_delete(s_timeout_timer);
        s_timeout_timer = NULL;
    }
}

/* ================================================================
 *  WiFi 事件处理器
 *
 *  回调上下文: WiFi 驱动内部线程，仅设标志 + 发事件，不做阻塞操作
 * ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    /* STA 模式启动 — 非扫描阶段自动连接 */
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_scanning) esp_wifi_connect();
    }
    /* 断开且尚未成功连接 → WiFi 失败 */
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_wifi_ok) {
            s_ntp_done = true;
            send_evt(NET_EVT_WIFI_FAIL);
        }
    }
    /* 获取 IP → WiFi 成功 → 启动 SNTP */
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip, sizeof(s_ip));
        s_wifi_ok = true;
        send_evt(NET_EVT_WIFI_OK);

        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP started, timeout=%d ms", read_nvs_timeout());
        start_ntp_poll_timer();
    }
}

/* ================================================================
 *  net_task — 主控任务
 *
 *  FreeRTOS 任务，由 net_sync_trigger() 唤醒
 *  执行流程: 加载凭据 → 扫描 → 匹配 → 连接 → 等待 NTP → 清理
 * ================================================================ */

static void net_task(void *arg)
{
    while (1) {
        /* ---- 阻塞等待 UI 触发 ---- */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Starting network sync");
        s_wifi_ok   = false;
        s_ntp_ok    = false;
        s_ntp_done  = false;
        s_ssid[0]   = '\0';
        esp_sntp_stop();

        /* ---- 1. 加载凭据 ---- */
        char cred_ssids[MAX_WIFI_CREDS][33] = {{0}};
        char cred_pwds[MAX_WIFI_CREDS][65] = {{0}};
        int cred_count = load_wifi_creds(cred_ssids, cred_pwds);

        if (cred_count == 0) {
            ESP_LOGE(TAG, "No WiFi credentials in NVS");
            send_evt(NET_EVT_WIFI_FAIL);
            send_evt(NET_EVT_NTP_FAIL);
            continue;
        }

        /* ---- 2. 清理旧状态，准备扫描 ---- */
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(200));

        s_scanning = true;  /* 扫描期间禁止自动连接 */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        vTaskDelay(pdMS_TO_TICKS(200));

        /* ---- 3. 扫描周围 AP（阻塞等待） ---- */
        ESP_LOGI(TAG, "WiFi scanning...");
        wifi_scan_config_t scan_cfg = {
            .show_hidden = false,
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        };
        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));
        uint16_t ap_num = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_num));
        wifi_ap_record_t *ap_list = calloc(ap_num, sizeof(wifi_ap_record_t));
        if (ap_list) {
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_list));
        }

        /* ---- 4. NVS 凭据 vs 扫描结果 匹配 ---- */
        char *chosen_ssid = NULL, *chosen_pwd = NULL;
        for (int i = 0; i < cred_count && !chosen_ssid; i++) {
            for (int j = 0; j < ap_num && !chosen_ssid; j++) {
                if (strcmp((char *)ap_list[j].ssid, cred_ssids[i]) == 0) {
                    chosen_ssid = cred_ssids[i];
                    chosen_pwd  = cred_pwds[i];
                    ESP_LOGI(TAG, "Matched AP: %s (rssi=%d)",
                             chosen_ssid, ap_list[j].rssi);
                }
            }
        }

        if (ap_list) free(ap_list);
        s_scanning = false;
        esp_wifi_scan_stop();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        /* 无匹配 → 回退用第一组凭据直接尝试 */
        if (!chosen_ssid) {
            chosen_ssid = cred_ssids[0];
            chosen_pwd  = cred_pwds[0];
            ESP_LOGI(TAG, "No scan match — trying first: %s", chosen_ssid);
        }
        strncpy(s_ssid, chosen_ssid, sizeof(s_ssid) - 1);

        /* ---- 5. 连接选中的 AP ---- */
        s_wifi_ok = false;
        send_evt(NET_EVT_WIFI_START);

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid,     chosen_ssid,
                sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, chosen_pwd,
                sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi connecting to %s...", chosen_ssid);

        /* ---- 6. 等待 NTP 完成（事件处理器设 s_ntp_done） ---- */
        while (!s_ntp_done) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        cancel_ntp_poll_timer();

        /* ---- 7. 释放 WiFi 资源（同步成功/失败都释放） ---- */
        ESP_LOGI(TAG, "Sync %s, cleaning up WiFi",
                 s_ntp_ok ? "OK" : "failed");
        esp_sntp_stop();
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
}

/* ================================================================
 *  公共 API
 * ================================================================ */

QueueHandle_t net_sync_get_queue(void) { return s_evt_queue; }
const char *net_sync_get_ssid(void)    { return s_ssid; }
const char *net_sync_get_ip(void)      { return s_ip; }

void net_sync_trigger(void)
{
    if (s_net_task) xTaskNotifyGive(s_net_task);
}

/* 初始化: 创建队列 + netif + WiFi + 事件处理器 + net_task */
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

    xTaskCreate(net_task, "net", 4096, NULL, 2, &s_net_task);
    ESP_LOGI(TAG, "Network sync module initialized");
}
