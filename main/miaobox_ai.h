/*
 * miaobox_ai.h — AI猫猫画图模块公共接口
 * ======================================
 *
 * 使用方式:
 *   UI → ai_cat_init()       初始化模块
 *   UI → ai_cat_start(cb)    开始画图流程（WiFi + API请求 + 图片下载）
 *   UI → ai_cat_stop()       停止当前流程
 *
 * 事件流:
 *   AI_EVT_WIFI_CONNECTING → 开始连接WiFi
 *   AI_EVT_WIFI_OK         → WiFi连接成功
 *   AI_EVT_WIFI_FAIL       → WiFi连接失败
 *   AI_EVT_REQUESTING      → 正在请求AI API
 *   AI_EVT_DOWNLOADING     → 正在下载图片
 *   AI_EVT_IMAGE_READY     → 图片就绪，可调用 ai_cat_get_image_data()
 *   AI_EVT_ERROR           → 出错
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_partition.h"

typedef enum {
    AI_EVT_IDLE,
    AI_EVT_WIFI_CONNECTING,
    AI_EVT_WIFI_OK,
    AI_EVT_WIFI_FAIL,
    AI_EVT_REQUESTING,
    AI_EVT_DOWNLOADING,
    AI_EVT_IMAGE_READY,
    AI_EVT_ERROR,
} ai_evt_t;

/* 事件数据结构 */
typedef struct {
    ai_evt_t evt;
    const char *msg;      /* 错误信息或状态描述 */
    int error_code;       /* 错误码，0表示无错误 */
} ai_event_data_t;

/* 事件回调函数类型 */
typedef void (*ai_event_cb_t)(ai_event_data_t *data, void *user_data);

/**
 * 初始化AI模块
 * 调用一次即可，创建内部任务和队列
 */
void ai_cat_init(void);

/**
 * 检查AI配置是否已设置(workspace和apikey)
 * @return true=配置已设置, false=缺少配置
 */
bool ai_cat_check_config(void);

/**
 * 开始AI画图流程
 * @param cb 事件回调函数
 * @param user_data 用户数据，回调时原样传回
 */
void ai_cat_start(ai_event_cb_t cb, void *user_data);

/**
 * 停止当前流程
 */
void ai_cat_stop(void);

/**
 * 获取生成的图片URL
 * @return 图片URL字符串，无效时返回NULL
 */
const char *ai_cat_get_image_url(void);

/**
 * 获取图片数据（JPEG格式）
 * @param size 输出图片大小
 * @return 图片数据指针，由调用者free()，失败返回NULL
 */
uint8_t *ai_cat_get_image_data(size_t *size);

/**
 * 检查WiFi是否已连接
 */
bool ai_cat_is_wifi_connected(void);

/**
 * 校验Flash中图片CRC32是否与NVS存储的一致
 * @return true=校验通过, false=校验失败或数据不存在
 */
bool ai_cat_verify_image(void);

/**
 * 获取Flash中图片数据的只读指针(通过mmap)
 * @param size 输出图片大小
 * @param handle 输出mmap句柄，用完后调用 esp_partition_munmap 释放
 * @return 图片数据指针, NULL=失败
 */
const uint8_t *ai_cat_map_image(size_t *size, esp_partition_mmap_handle_t *handle);

/**
 * 获取storage分区指针
 */
const esp_partition_t *ai_cat_get_storage_partition(void);
