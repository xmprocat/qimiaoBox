/*
 * miaobox_ai.c — AI猫猫画图模块
 * ==============================
 *
 * 实现流程:
 *   1. 连接WiFi（使用共享网络模块）
 *   2. HTTP POST请求AI画图API
 *   3. 解析JSON获取图片URL
 *   4. HTTP GET下载图片 → 写入Flash分区（不占RAM）
 *   5. 回调通知UI
 *
 * Dependencies:
 *   esp_http_client, json (cJSON), miaobox_net, esp_partition
 */

#include "miaobox_ai.h"
#include "miaobox_net.h"

#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"

static const char *TAG = "miaobox_ai";

/* NVS keys */
#define NVS_NS           "cfg"
#define NVS_KEY_CAT_CRC  "cat.crc32"
#define NVS_KEY_CAT_SIZE "cat.size"

/* ================================================================
 *  API配置 (从NVS读取)
 * ================================================================ */

#define NVS_KEY_AI_WORKSPACE "ai.workspace"
#define NVS_KEY_AI_APIKEY    "ai.apikey"

/* 基础提示词（不含姿势） */
#define AI_BASE_PROMPT "一只可爱的橘色虎斑小猫，日系萌系插画风格，大头大眼比例，圆润的黄色大眼睛带高光，粉色小鼻子，白色胡须，耳朵竖立，毛发呈温暖的橘色与浅棕色条纹，胸前白色绒毛，半厚涂赛璐璐混合风格，干净的深棕色外轮廓线，毛发有细腻笔触纹理但整体色块分明，明暗过渡柔和，色彩鲜艳温暖饱和，萌系Q版可爱表情，AI插画质感，高清细节，纯黑色背景。"

/* 随机姿势列表 */
static const char *AI_POSES[] = {
    "端正坐着，抬头好奇地向上看",
    "坐着歪头，一只耳朵微微下垂，眼神无辜",
    "坐着把两只前爪收进胸前，像农民揣手一样",
    "坐着举起一只前爪，像在跟人说hi",
    "坐着低头认真舔前爪，一只脚抬起",
    "坐着张大嘴巴打哈欠，眼睛眯成一条缝",
    "正在打喷嚏，眼睛紧闭，胡须往后飞",
    "完全趴平像猫饼，下巴贴地，前爪并拢伸向前方",
    "四脚收在身下蹲着，像孵蛋的母鸡",
    "侧躺在地上，一只前爪搭在另一只上面",
    "仰面朝天躺着，四爪举起，露出白色肚皮",
    "蜷成一团闭眼睡觉，尾巴围住身体",
    "趴着伸懒腰，前爪向前伸到最长，屁股翘起",
    "趴着把下巴搁在两只前爪上，眼神放空",
    "跃起姿势，前爪伸出，眼睛盯着上方",
    "朝镜头方向飞扑过来，身体拉长，爪子张开",
    "在半空中扭转身体的抓拍瞬间",
    "四脚腾空奔跑，毛发被风吹起",
    "刚从高处跳下，后腿弯曲缓冲落地",
    "转圈追自己的尾巴，身体扭成C形",
    "从画面边缘探出半个身子，只露头和前爪",
    "从纸箱里探出头，两只前爪搭在箱子边缘",
    "从布袋口探出头，身体还在袋子里",
    "抱着树干往上爬，爪子抓着树皮",
    "站立起来伸爪子去够高处的东西",
    "低头凑近一朵花闻，眼睛微闭",
    "炸毛弓背，眼睛瞪到最大，全身毛发竖起",
    "半眯眼睛斜眼看人，嘴角微微下撇",
    "嘴巴微张O型，眼睛瞪圆，像看到了不可思议的东西",
    "面无表情直直盯着前方，眼神空洞",
    "微微仰头，眯眼微笑，尾巴翘起",
    "缩在角落瑟瑟发抖，耳朵平贴脑袋",
    "用头蹭着人的小腿，眼睛眯成弯月",
    "站在柔软的东西上两只前爪交替踩，表情陶醉",
    "举起一只爪子像要跟人击掌",
    "嘴里叼着一个毛线球，眼睛亮晶晶",
    "用一只爪子把桌上的杯子往边缘推",
    "低头做掩埋动作，一只后爪往后刨",
    "坐在窗台上望向窗外，背影轮廓，尾巴自然垂下",
    "头上戴着一顶小帽子，端正坐着，表情无奈",
};
#define AI_POSE_COUNT (sizeof(AI_POSES) / sizeof(AI_POSES[0]))

#define AI_HTTP_TIMEOUT_MS  60000
#define AI_IMAGE_MAX_SIZE   (512 * 1024)   /* Flash分区大小 */
#define DL_BUF_SIZE          8192           /* 下载缓冲区8KB */

/* Flash分区标签 */
#define STORAGE_PART_LABEL "storage"

/* ================================================================
 *  内部状态
 * ================================================================ */

static TaskHandle_t s_ai_task = NULL;
static ai_event_cb_t s_event_cb = NULL;
static void *s_user_data = NULL;
static char s_image_url[512] = {0};
static const esp_partition_t *s_storage_part = NULL;
static size_t s_image_size = 0;          /* 已存储的图片大小 */

/* ================================================================
 *  HTTP请求 - AI API
 * ================================================================ */

/* ---- 事件发送辅助 ---- */

static void send_event(ai_evt_t evt, const char *msg, int err_code)
{
    if (s_event_cb) {
        ai_event_data_t data = { .evt = evt, .msg = msg, .error_code = err_code };
        s_event_cb(&data, s_user_data);
    }
}

/* ---- HTTP 事件处理器 ----
 * 使用 static buffer 收集响应体数据，FINISH 时通过 user_data 返回给调用者。
 * 注意：static 变量意味着同一时间只能有一个 HTTP 请求使用此 handler。 */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer = NULL;
    static int output_len = 0;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->user_data) {
            /* 首次收到数据时分配累积缓冲区 */
            if (!output_buffer) {
                output_buffer = calloc(1, 8192);
                output_len = 0;
            }
            if (output_buffer && (output_len + evt->data_len < 8192)) {
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        if (output_buffer) {
            /* 将累积的响应数据复制到 user_data 指向的 char* 指针 */
            char **resp_ptr = (char **)evt->user_data;
            if (resp_ptr && *resp_ptr == NULL) {
                *resp_ptr = malloc(output_len + 1);
                if (*resp_ptr) {
                    memcpy(*resp_ptr, output_buffer, output_len);
                    (*resp_ptr)[output_len] = '\0';
                }
            }
            free(output_buffer);
            output_buffer = NULL;
            output_len = 0;
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        /* 连接异常断开时清理，避免下次请求时残留脏数据 */
        if (output_buffer) {
            free(output_buffer);
            output_buffer = NULL;
            output_len = 0;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ---- NVS 工具 ---- */

/* 从NVS读取字符串，返回malloc分配的副本(调用者free) */
static char *read_nvs_str(const char *key)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return NULL;
    size_t len = 0;
    if (nvs_get_str(nvs, key, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(nvs); return NULL;
    }
    char *val = malloc(len);
    if (!val || nvs_get_str(nvs, key, val, &len) != ESP_OK) {
        free(val); nvs_close(nvs); return NULL;
    }
    nvs_close(nvs);
    return val;
}

/* ---- AI API 请求 ----
 * 流程: 读配置 → 随机选姿势 → 构建JSON → HTTP POST → 返回响应体
 * 返回值: malloc 的 JSON 字符串，调用者 free；失败返回 NULL */

static char *request_ai_image(void)
{
    /* 1. 从 NVS 读取 API 配置 */
    char *workspace = read_nvs_str(NVS_KEY_AI_WORKSPACE);
    char *apikey = read_nvs_str(NVS_KEY_AI_APIKEY);
    if (!workspace || !apikey) {
        ESP_LOGE(TAG, "AI config not set in NVS (need %s and %s)",
                 NVS_KEY_AI_WORKSPACE, NVS_KEY_AI_APIKEY);
        free(workspace);
        free(apikey);
        return NULL;
    }

    /* 2. 构建阿里云 AI 服务端点 URL */
    char *url = NULL;
    asprintf(&url,
        "https://%s.cn-beijing.maas.aliyuncs.com"
        "/api/v1/services/aigc/multimodal-generation/generation",
        workspace);
    if (!url) {
        ESP_LOGE(TAG, "Failed to build URL");
        free(workspace);
        free(apikey);
        return NULL;
    }

    /* 3. 随机选择猫猫姿势 */
    uint32_t pose_idx = esp_random() % AI_POSE_COUNT;
    const char *pose = AI_POSES[pose_idx];
    ESP_LOGI(TAG, "Random pose [%" PRIu32 "]: %s", pose_idx, pose);

    /* 4. 拼接请求体: 基础提示词 + 随机姿势 */
    char *body = NULL;
    int body_len = asprintf(&body,
        "{\"model\":\"qwen-image-2.0-pro\","
        "\"input\":{\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"text\":\"%s %s\"}]}]},"
        "\"parameters\":{"
          "\"negative_prompt\":\"低画质，肢体畸形，爪子畸形，眼睛不对称，毛发糊团，蜡像感，塑料质感，构图偏移，背景杂物，文字水印，写实照片风格，过度光滑\","
          "\"prompt_extend\":true,"
          "\"watermark\":false,"
          "\"size\":\"512*512\""
        "}}",
        AI_BASE_PROMPT, pose);
    if (body_len < 0 || !body) {
        ESP_LOGE(TAG, "Failed to build request body");
        free(workspace);
        free(apikey);
        free(url);
        return NULL;
    }

    /* 5. HTTP POST 请求
     * user_data = &response → http_event_handler 在 FINISH 时将响应写入 response */
    char *response = NULL;
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = AI_HTTP_TIMEOUT_MS,
        .buffer_size_tx = 2048,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(workspace); free(apikey); free(url); free(body);
        return NULL;
    }

    /* 设置请求头: JSON Content-Type + Bearer Token 认证 */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", apikey);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    /* 执行请求 */
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d", status_code);
    esp_http_client_cleanup(client);
    free(workspace);
    free(apikey);
    free(url);
    free(body);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "API request failed: err=%s status=%d",
                 esp_err_to_name(err), status_code);
        free(response);
        return NULL;
    }
    return response;
}

/* ---- JSON 解析 ----
 * 从 AI API 响应中提取图片 URL。
 * JSON 路径: output.choices[0].message.content[0].image */

static char *parse_image_url(const char *json_response)
{
    if (!json_response) return NULL;

    cJSON *root = cJSON_Parse(json_response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return NULL;
    }

    char *image_url = NULL;

    /* 安全遍历深层嵌套 JSON，每层都做类型检查 */
    cJSON *output = cJSON_GetObjectItem(root, "output");
    if (!output) goto done;

    cJSON *choices = cJSON_GetObjectItem(output, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
        goto done;

    cJSON *message = cJSON_GetObjectItem(
        cJSON_GetArrayItem(choices, 0), "message");
    if (!message) goto done;

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!content || !cJSON_IsArray(content) || cJSON_GetArraySize(content) == 0)
        goto done;

    cJSON *image = cJSON_GetObjectItem(
        cJSON_GetArrayItem(content, 0), "image");
    if (image && cJSON_IsString(image)) {
        image_url = strdup(image->valuestring);
        ESP_LOGI(TAG, "Got image URL: %s", image_url);
    }

done:
    cJSON_Delete(root);
    return image_url;
}

/* ================================================================
 *  图片下载 → 写入 Flash 分区
 * ================================================================
 *
 * 流程: 擦除分区 → HTTP GET 逐步下载 → 8KB 缓冲写入 Flash → CRC32 校验
 * 全程使用 8KB 小缓冲区，不将整张图片加载到 RAM。
 */

/* 延迟查找 storage 分区（首次调用时缓存） */
static const esp_partition_t *get_storage_partition(void)
{
    if (s_storage_part) return s_storage_part;
    s_storage_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, STORAGE_PART_LABEL);
    if (!s_storage_part) {
        ESP_LOGE(TAG, "Partition '%s' not found", STORAGE_PART_LABEL);
    }
    return s_storage_part;
}

const esp_partition_t *ai_cat_get_storage_partition(void)
{
    return get_storage_partition();
}

static bool download_image_to_flash(const char *url, size_t *out_size,
                                      uint32_t *out_crc, const char **err_msg)
{
    *err_msg = NULL;
    *out_size = 0;
    *out_crc = 0;

    const esp_partition_t *part = get_storage_partition();
    if (!part) { *err_msg = "Partition not found"; return false; }

    if (part->size < AI_IMAGE_MAX_SIZE) {
        *err_msg = "Partition too small"; return false;
    }

    /* 阿里云图片 CDN 强制 HTTPS，ESP32 用 HTTP 下载更稳定 */
    char http_url[640];
    if (strncmp(url, "https://", 8) == 0) {
        snprintf(http_url, sizeof(http_url), "http://%s", url + 8);
    } else {
        strncpy(http_url, url, sizeof(http_url) - 1);
    }

    /* 1. 擦除整个 storage 分区 */
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        *err_msg = "Erase failed"; return false;
    }

    /* 2. HTTP GET 下载图片 */
    esp_http_client_config_t config = {
        .url = http_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = AI_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { *err_msg = "HTTP init failed"; return false; }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        *err_msg = "HTTP connect failed";
        esp_http_client_cleanup(client); return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > AI_IMAGE_MAX_SIZE) {
        *err_msg = "Invalid image size";
        esp_http_client_close(client);
        esp_http_client_cleanup(client); return false;
    }
    ESP_LOGI(TAG, "Image size: %d bytes", content_length);

    /* 3. 逐块读取 → 写入 Flash（边下载边 CRC32 校验） */
    uint8_t *buf = malloc(DL_BUF_SIZE);
    if (!buf) {
        *err_msg = "Buffer alloc failed";
        esp_http_client_close(client);
        esp_http_client_cleanup(client); return false;
    }

    uint32_t crc = 0;
    size_t total = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, (char *)buf,
             (content_length - total) < DL_BUF_SIZE
                 ? (content_length - total) : DL_BUF_SIZE)) > 0) {

        err = esp_partition_write(part, total, buf, read_len);
        if (err != ESP_OK) {
            *err_msg = "Flash write failed";
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client); return false;
        }

        /* 流式 CRC32 累加，避免二次遍历 */
        crc = esp_rom_crc32_le(crc, buf, read_len);
        total += read_len;
        if (total >= (size_t)content_length) break;
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total != (size_t)content_length) {
        *err_msg = "Download incomplete";
        return false;
    }

    /* 4. 从 Flash mmap 读回并复核 CRC32（确保写入无位翻转） */
    const void *verify_data;
    esp_partition_mmap_handle_t verify_handle;
    err = esp_partition_mmap(part, 0, total,
        ESP_PARTITION_MMAP_DATA, &verify_data, &verify_handle);
    if (err != ESP_OK) {
        *err_msg = "Verify mmap failed"; return false;
    }
    uint32_t verify_crc = esp_rom_crc32_le(0,
        (const uint8_t *)verify_data, total);
    esp_partition_munmap(verify_handle);

    if (verify_crc != crc) {
        ESP_LOGE(TAG, "CRC mismatch: stream=0x%08" PRIx32
                 " flash=0x%08" PRIx32, crc, verify_crc);
        *err_msg = "CRC verify failed";
        return false;
    }

    /* 5. 保存 CRC32 + 大小到 NVS，下次启动可校验 */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_KEY_CAT_CRC, crc);
        nvs_set_u32(nvs, NVS_KEY_CAT_SIZE, (uint32_t)total);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    *out_size = total;
    *out_crc = crc;
    ESP_LOGI(TAG, "Image saved + verified: %d bytes, CRC 0x%08" PRIx32,
             (int)total, crc);
    return true;
}

/* ================================================================
 *  AI 主任务
 * ================================================================
 *
 * 流程: WiFi连接 → AI API请求 → 解析图片URL → 下载写入Flash → 清理
 * 每个阶段通过 send_event() 向 UI 报告进度，失败时优雅退出。
 */

static void ai_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AI task started");

    /* 阶段1: WiFi 连接（复用 miaobox_net 共享连接） */
    send_event(AI_EVT_WIFI_CONNECTING, "Connecting WiFi...", 0);
    if (!net_wifi_connect(30000)) {
        send_event(AI_EVT_WIFI_FAIL, "WiFi connect failed", -1);
        vTaskDelete(NULL);
        return;
    }
    send_event(AI_EVT_WIFI_OK, "WiFi connected", 0);

    /* 阶段2: 请求 AI 生成猫猫图片 */
    send_event(AI_EVT_REQUESTING, "Requesting cat image...", 0);
    char *response = request_ai_image();
    if (!response) {
        send_event(AI_EVT_ERROR, "API request failed", -2);
        net_wifi_disconnect();
        vTaskDelete(NULL);
        return;
    }

    /* 阶段3: 解析 JSON 获取图片 URL */
    char *image_url = parse_image_url(response);
    free(response);
    if (!image_url) {
        send_event(AI_EVT_ERROR, "Parse image URL failed", -3);
        net_wifi_disconnect();
        vTaskDelete(NULL);
        return;
    }
    strncpy(s_image_url, image_url, sizeof(s_image_url) - 1);
    free(image_url);

    /* 阶段4: 下载图片 → 逐步写入 Flash → CRC32 校验 */
    send_event(AI_EVT_DOWNLOADING, "Downloading cat image...", 0);
    size_t img_size = 0;
    uint32_t img_crc = 0;
    const char *dl_err = NULL;
    if (!download_image_to_flash(s_image_url, &img_size, &img_crc, &dl_err)) {
        send_event(AI_EVT_ERROR, dl_err ? dl_err : "Image download failed", -4);
        net_wifi_disconnect();
        vTaskDelete(NULL);
        return;
    }
    s_image_size = img_size;

    /* 阶段5: 清理 */
    net_wifi_disconnect();

    /* 释放 TLS/WiFi 内部残留缓冲，减少堆碎片 */
    malloc_trim(0);
    ESP_LOGI(TAG, "post-cleanup heap=%" PRIu32 " largest=%" PRIu32,
             esp_get_free_heap_size(),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    send_event(AI_EVT_IMAGE_READY, "Cat image ready", 0);
    ESP_LOGI(TAG, "AI task completed");
    vTaskDelete(NULL);
}

/* ================================================================
 *  公共 API
 * ================================================================ */

void ai_cat_init(void)
{
    /* 当前无内部状态需要初始化，保留为将来扩展预留 */
    ESP_LOGI(TAG, "AI cat module initialized");
}

bool ai_cat_check_config(void)
{
    /* 检查 NVS 中是否已配置 workspace 和 apikey */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;

    char workspace[64] = {0};
    char apikey[256] = {0};
    size_t len;

    len = sizeof(workspace);
    if (nvs_get_str(nvs, NVS_KEY_AI_WORKSPACE, workspace, &len) != ESP_OK
        || workspace[0] == '\0') {
        nvs_close(nvs); return false;
    }

    len = sizeof(apikey);
    if (nvs_get_str(nvs, NVS_KEY_AI_APIKEY, apikey, &len) != ESP_OK
        || apikey[0] == '\0') {
        nvs_close(nvs); return false;
    }

    nvs_close(nvs);
    return true;
}

void ai_cat_start(ai_event_cb_t cb, void *user_data)
{
    /* 停止正在进行的任务（如果有），然后启动新的 */
    ai_cat_stop();
    s_event_cb = cb;
    s_user_data = user_data;
    s_image_size = 0;
    s_image_url[0] = '\0';
    /* 高优先级(5)确保网络 I/O 不被 LVGL 任务阻塞 */
    xTaskCreate(ai_task, "ai_cat", 8192, NULL, 5, &s_ai_task);
}

void ai_cat_stop(void)
{
    if (s_ai_task) {
        vTaskDelete(s_ai_task);
        s_ai_task = NULL;
    }
    net_wifi_disconnect();
}

const char *ai_cat_get_image_url(void)
{
    return s_image_url[0] ? s_image_url : NULL;
}

/* 从 Flash 读取完整图片数据到堆内存（调用者负责 free） */
uint8_t *ai_cat_get_image_data(size_t *size)
{
    const esp_partition_t *part = get_storage_partition();
    if (!part || s_image_size == 0) return NULL;

    /* mmap 零拷贝映射 Flash → 复制到堆 → unmap */
    const void *data;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, s_image_size,
        ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return NULL;
    }

    uint8_t *copy = malloc(s_image_size);
    if (copy) {
        memcpy(copy, data, s_image_size);
        *size = s_image_size;
    }
    esp_partition_munmap(handle);
    return copy;
}

bool ai_cat_is_wifi_connected(void)
{
    return net_wifi_is_connected();
}

/* 校验 Flash 中图片数据完整性（NVS CRC32 vs Flash 实时计算） */
bool ai_cat_verify_image(void)
{
    /* 1. 从 NVS 读取上次存储的 CRC32 和 size */
    nvs_handle_t nvs;
    uint32_t stored_crc = 0, stored_size = 0;

    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for verify");
        return false;
    }

    nvs_get_u32(nvs, NVS_KEY_CAT_CRC, &stored_crc);
    nvs_get_u32(nvs, NVS_KEY_CAT_SIZE, &stored_size);
    nvs_close(nvs);

    if (stored_crc == 0 || stored_size == 0) {
        ESP_LOGW(TAG, "No stored image data in NVS");
        return false;
    }

    /* 2. 从 Flash mmap 读取并重新计算 CRC32 */
    const esp_partition_t *part = get_storage_partition();
    if (!part) return false;

    esp_partition_mmap_handle_t handle;
    const void *data;
    esp_err_t err = esp_partition_mmap(part, 0, stored_size,
        ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Verify mmap failed");
        return false;
    }

    uint32_t calc_crc = esp_rom_crc32_le(0,
        (const uint8_t *)data, stored_size);
    esp_partition_munmap(handle);

    if (calc_crc != stored_crc) {
        ESP_LOGE(TAG, "CRC mismatch: stored=0x%08" PRIx32
                 " calc=0x%08" PRIx32, stored_crc, calc_crc);
        return false;
    }

    ESP_LOGI(TAG, "Image verified OK: size=%" PRIu32 " CRC32=0x%08" PRIx32,
             stored_size, calc_crc);
    return true;
}

/* 零拷贝映射 Flash 图片数据（调用者负责 esp_partition_munmap 释放） */
const uint8_t *ai_cat_map_image(size_t *size,
                                 esp_partition_mmap_handle_t *handle)
{
    /* 从 NVS 读取图片大小 */
    nvs_handle_t nvs;
    uint32_t stored_size = 0;

    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return NULL;
    nvs_get_u32(nvs, NVS_KEY_CAT_SIZE, &stored_size);
    nvs_close(nvs);

    if (stored_size == 0) return NULL;

    const esp_partition_t *part = get_storage_partition();
    if (!part) return NULL;

    const void *data;
    esp_err_t err = esp_partition_mmap(part, 0, stored_size,
        ESP_PARTITION_MMAP_DATA, &data, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Map image failed: %s", esp_err_to_name(err));
        return NULL;
    }

    *size = stored_size;
    return (const uint8_t *)data;
}
