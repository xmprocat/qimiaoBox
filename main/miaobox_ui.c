/*
 * miaobox_ui.c — LVGL 界面模块
 * ===============================
 *
 * 页面路由:
 *   splash (1s) → key_test ←→ birthday / timesync
 *                 key_test   → [不可达: timesync 失败时退回]
 *                 birthday   → heart (DOWN短按)
 *                 heart      → key_test (长按ESC)
 *                 timesync   → birthday (NTP成功后自动跳转)
 *
 * 页面列表:
 *   0. splash      — 启动画面 "淇喵盒子" + 构建时间 (1s)
 *   1. key_test    — 按键测试页，四个方框对应 ESC/ENTER/DOWN/UP
 *   2. birthday    — 生日倒计时，4行中文逐行淡入，底部 hint 延迟显示
 *   3. timesync    — NTP时间同步状态页，WiFi扫描匹配+NTP多服务器轮询
 *   4. heart       — 粒子动画爱心，500粒子从边缘飞向心形轮廓
 *
 * 按键模型:
 *   长按 ESC (≥2次连续 KEY 事件) = 页面跳转
 *   短按 DOWN (birthday页) = 跳到 heart 页
 *   所有按键 level-triggered，通过 button 库消抖
 *
 * 内存管理:
 *   heart 页面 buffer 动态分配，切换页面时通过 LV_EVENT_DELETE 回调释放
 *   timesync timer 通过注册在 label 上的 delete callback 停止
 *   其余页面全部由 lv_obj_clean() 自动清理
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "esp_log.h"
#include "miaobox_net.h"

extern const lv_font_t lv_font_alibaba_22;

static const char *TAG = "miaobox_ui";

/* ================================================================
 *  页面路由 — 函数指针替代整数索引，方便增减页面
 * ================================================================ */
typedef void (*page_builder_t)(void);
static page_builder_t current_page = NULL;  /* 当前显示的页面 */
static page_builder_t prev_page    = NULL;  /* 上一页，用于返回 */

/* ---- config ---- */
#define BIRTHDAY1_MONTH  8
#define BIRTHDAY1_DAY   18
#define BIRTHDAY2_MONTH  9
#define BIRTHDAY2_DAY   18
static int age = 3; /* placeholder, will be fetched from network */

/* ---- shared ---- */
static lv_disp_t *ui_disp;
static lv_obj_t *ui_scr;

/* ---- page 0: key test ---- */
static lv_obj_t *box_esc, *box_enter, *box_down, *box_up;
static int box_state[4];

static void set_box_color(lv_obj_t *box, int state)
{
    lv_color_t c;
    switch (state) {
    case 0: c = lv_color_make(255, 0, 0); break;
    case 1: c = lv_color_make(0, 255, 0); break;
    case 2: c = lv_color_make(255, 255, 255); break;
    default: c = lv_color_make(255, 0, 0); break;
    }
    lv_obj_set_style_bg_color(box, c, 0);
}

static void cycle_box(int idx, lv_obj_t *box)
{
    box_state[idx] = (box_state[idx] == 0) ? 1 : (box_state[idx] == 1) ? 2 : 1;
    set_box_color(box, box_state[idx]);
}

static lv_obj_t *create_box(lv_obj_t *p, int x, int y, int w, int h, const char *t)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_make(255, 0, 0), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    return b;
}

static void build_key_test_page(void)
{
    memset(box_state, 0, sizeof(box_state));

    lv_obj_t *title = lv_label_create(ui_scr);
    lv_label_set_text(title, "KEY_TEST");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_color(title, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    int bw = 100, bh = 80, sx = 12, sy = 35, gx = 15, gy = 30;
    box_esc   = create_box(ui_scr, sx,        sy,        bw, bh, "ESC");
    box_enter = create_box(ui_scr, sx+bw+gx,  sy,        bw, bh, "ENTER");
    box_down  = create_box(ui_scr, sx,        sy+bh+gy,  bw, bh, "DOWN");
    box_up    = create_box(ui_scr, sx+bw+gx,  sy+bh+gy,  bw, bh, "UP");
}

/* ---- page 1: birthday countdown ---- */

static int days_until_date(int month, int day)
{
    time_t now = time(NULL);
    struct tm today = *localtime(&now);

    struct tm target = today;
    target.tm_mon = month - 1;
    target.tm_mday = day;
    target.tm_hour = 0;
    target.tm_min = 0;
    target.tm_sec = 0;

    if (target.tm_mon < today.tm_mon ||
        (target.tm_mon == today.tm_mon && target.tm_mday < today.tm_mday)) {
        target.tm_year++;
    }

    double diff = difftime(mktime(&target), mktime(&today));
    return (int)(diff / 86400.0);
}

static void fade_in_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void build_birthday_page(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int y = t->tm_year + 1900;
    int mon = t->tm_mon + 1;
    int d = t->tm_mday;

    int days1 = days_until_date(BIRTHDAY1_MONTH, BIRTHDAY1_DAY);
    int days2 = days_until_date(BIRTHDAY2_MONTH, BIRTHDAY2_DAY);

    char line1[64], line2[64], line3[64], line4[64];
    snprintf(line1, sizeof(line1), "今天是%d年%d月%d日", y, mon, d);
    snprintf(line2, sizeof(line2), "距离%d月%d日还有%d天", BIRTHDAY1_MONTH, BIRTHDAY1_DAY, days1);
    snprintf(line3, sizeof(line3), "距离%d月%d日还有%d天", BIRTHDAY2_MONTH, BIRTHDAY2_DAY, days2);
    snprintf(line4, sizeof(line4), "奶淇宝宝今年%d岁了", age);

    const char *lines[] = {line1, line2, line3, line4};
    int offsets[] = {-42, -15, 12, 39};
    int delays[]  = {300, 1000, 1700, 2400};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *label = lv_label_create(ui_scr);
        lv_label_set_text(label, lines[i]);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, offsets[i]);
        lv_obj_set_style_text_color(label, lv_color_make(255, 255, 255), 0);
        lv_obj_set_style_text_font(label, &lv_font_alibaba_22, 0);
        lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, label);
        lv_anim_set_exec_cb(&a, fade_in_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_delay(&a, delays[i]);
        lv_anim_start(&a);
    }

    /* hint at bottom: appears after 5s, blinks every 1s */
    lv_obj_t *hint = lv_label_create(ui_scr);
    lv_label_set_text(hint, "<---长按退出");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_set_style_text_color(hint, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(hint, &lv_font_alibaba_22, 0);
    lv_obj_set_style_opa(hint, LV_OPA_TRANSP, 0);

    lv_anim_t ha;
    lv_anim_init(&ha);
    lv_anim_set_var(&ha, hint);
    lv_anim_set_exec_cb(&ha, fade_in_cb);
    lv_anim_set_values(&ha, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ha, 1500);
    lv_anim_set_delay(&ha, 5000);
    lv_anim_set_playback_duration(&ha, 1500);
    lv_anim_set_repeat_count(&ha, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&ha);

    ESP_LOGI(TAG, "Built birthday page");
}

/* ---- NTP time check ---- */

static bool is_time_synced(void)
{
    return time(NULL) > 1000000000;  /* epoch → synced */
}

/* ---- page 2: time sync status ---- */

static void switch_to_page(page_builder_t page);

static lv_obj_t *ts_wifi_label;
static lv_obj_t *ts_ntp_label;
static lv_obj_t *ts_ssid_label;
static lv_obj_t *ts_hint;
static lv_timer_t *ts_poll_timer;

static void ts_switch_cb(void *page)
{
    switch_to_page((page_builder_t)page);
}

static void ts_delete_cb(lv_event_t *e)
{
    if (ts_poll_timer) {
        lv_timer_del(ts_poll_timer);
        ts_poll_timer = NULL;
    }
}

static void ts_stop_blink(lv_obj_t *label)
{
    lv_anim_delete(label, fade_in_cb);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
}

static void ts_set_result(lv_obj_t *label, const char *text, bool ok)
{
    ts_stop_blink(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label,
        ok ? lv_color_make(0, 255, 0) : lv_color_make(255, 0, 0), 0);
}

static void ts_start_blink(lv_obj_t *label)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, fade_in_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_playback_duration(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void ts_poll_cb(lv_timer_t *timer)
{
    net_evt_t evt;
    QueueHandle_t q = net_sync_get_queue();
    while (xQueueReceive(q, &evt, 0) == pdTRUE) {
        switch (evt) {
        case NET_EVT_WIFI_OK:
            ts_set_result(ts_wifi_label, "网络连接 --- 【成功】", true);
            /* Show SSID + IP and NTP line */
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s (%s)", net_sync_get_ssid(), net_sync_get_ip());
                lv_label_set_text(ts_ssid_label, buf);
            }
            lv_obj_clear_flag(ts_ssid_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ts_ntp_label, LV_OBJ_FLAG_HIDDEN);
            ts_start_blink(ts_ntp_label);
            break;
        case NET_EVT_WIFI_FAIL:
            ts_set_result(ts_wifi_label, "网络连接 --- 【失败】", false);
            lv_obj_clear_flag(ts_hint, LV_OBJ_FLAG_HIDDEN);
            break;
        case NET_EVT_NTP_OK:
            ts_set_result(ts_ntp_label, "时间同步 --- 【成功】", true);
            lv_timer_del(ts_poll_timer);
            ts_poll_timer = NULL;
            /* defer switch to avoid LVGL re-entrancy in timer callback */
            lv_async_call(ts_switch_cb,
                          (void *)(prev_page ? prev_page : build_key_test_page));
            return;
        case NET_EVT_NTP_FAIL:
            ts_set_result(ts_ntp_label, "时间同步 --- 【失败】", false);
            lv_obj_clear_flag(ts_hint, LV_OBJ_FLAG_HIDDEN);
            break;
        case NET_EVT_NTP_RETRY:
            /* One server failed, trying next — show orange */
            ts_stop_blink(ts_ntp_label);
            lv_obj_set_style_text_color(ts_ntp_label,
                lv_color_make(255, 165, 0), 0);  /* orange */
            ts_start_blink(ts_ntp_label);
            break;
        default:
            break;
        }
    }
}

static void build_timesync_page(void)
{
    /* Trigger network sync */
    net_sync_trigger();

    /* Title */
    lv_obj_t *title = lv_label_create(ui_scr);
    lv_label_set_text(title, "正在同步设备时间");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(title, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_text_font(title, &lv_font_alibaba_22, 0);

    /* WiFi status line — blinking until result */
    ts_wifi_label = lv_label_create(ui_scr);
    lv_obj_add_event_cb(ts_wifi_label, ts_delete_cb, LV_EVENT_DELETE, NULL);
    lv_label_set_text(ts_wifi_label, "网络连接 --- 【进行中】");
    lv_obj_align(ts_wifi_label, LV_ALIGN_CENTER, 0, -18);
    lv_obj_set_style_text_color(ts_wifi_label, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_text_font(ts_wifi_label, &lv_font_alibaba_22, 0);
    ts_start_blink(ts_wifi_label);

    /* SSID — hidden until WiFi succeeds */
    ts_ssid_label = lv_label_create(ui_scr);
    lv_label_set_text(ts_ssid_label, "");
    lv_obj_align(ts_ssid_label, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_text_color(ts_ssid_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(ts_ssid_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(ts_ssid_label, LV_OBJ_FLAG_HIDDEN);

    /* NTP status line — hidden until WiFi succeeds */
    ts_ntp_label = lv_label_create(ui_scr);
    lv_label_set_text(ts_ntp_label, "时间同步 --- 【进行中】");
    lv_obj_align(ts_ntp_label, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_text_color(ts_ntp_label, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_text_font(ts_ntp_label, &lv_font_alibaba_22, 0);
    lv_obj_add_flag(ts_ntp_label, LV_OBJ_FLAG_HIDDEN);

    /* Poll timer */
    ts_poll_timer = lv_timer_create(ts_poll_cb, 200, NULL);

    /* Bottom hint — hidden until failure */
    ts_hint = lv_label_create(ui_scr);
    lv_label_set_text(ts_hint, "<---长按退出");
    lv_obj_align(ts_hint, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_set_style_text_color(ts_hint, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(ts_hint, &lv_font_alibaba_22, 0);
    lv_obj_add_flag(ts_hint, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t ha;
    lv_anim_init(&ha);
    lv_anim_set_var(&ha, ts_hint);
    lv_anim_set_exec_cb(&ha, fade_in_cb);
    lv_anim_set_values(&ha, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ha, 1500);
    lv_anim_set_playback_duration(&ha, 1500);
    lv_anim_set_repeat_count(&ha, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&ha);

    ESP_LOGI(TAG, "Built timesync page");
}

/* ---- page 3: beating heart ---- */

#include <math.h>
#include <stdlib.h>

#define HEART_SIZE   200
#define HEART_POINTS 200

static lv_image_dsc_t heart_img;
static uint16_t *heart_pixels;         /* malloc in build, free in delete */
static lv_obj_t *heart_img_obj;
static lv_timer_t *heart_timer;
static int heart_frame;

#define HEART_OUTLINE_SAMPLES (HEART_POINTS * 5) /* 1000 */
#define MAX_PARTICLES 500

typedef struct {
    int16_t sx, sy, ex, ey, cx, cy;
    int16_t life, max_life;
    uint16_t color;
} heart_particle_t;

/* pre-computed outline points (dynamically allocated) */
static int16_t *heart_ox;
static int16_t *heart_oy;

/* particle array (dynamically allocated) */
static heart_particle_t *particles;
static int particle_count;
static bool heart_text_shown;

#define HEART_PIXELS_BYTES  (HEART_SIZE * HEART_SIZE * sizeof(uint16_t))
#define HEART_OUTLINE_BYTES (HEART_OUTLINE_SAMPLES * sizeof(int16_t))
#define HEART_PARTICLES_BYTES (MAX_PARTICLES * sizeof(heart_particle_t))

static void particle_init(heart_particle_t *p)
{
    /* target: near a random outline point (±7px jitter) */
    int idx = rand() % HEART_OUTLINE_SAMPLES;
    p->ex = heart_ox[idx] + (rand() % 15) - 7;
    p->ey = heart_oy[idx] + (rand() % 15) - 7;

    /* start: random screen edge */
    int edge = rand() % 4;
    if (edge == 0)      { p->sx = 0;                   p->sy = rand() % HEART_SIZE; }
    else if (edge == 1) { p->sx = HEART_SIZE - 1;      p->sy = rand() % HEART_SIZE; }
    else if (edge == 2) { p->sx = rand() % HEART_SIZE; p->sy = 0; }
    else                { p->sx = rand() % HEART_SIZE; p->sy = HEART_SIZE - 1; }

    p->cx = p->sx;
    p->cy = p->sy;
    p->max_life = 80 + (rand() % 171); /* 80-250 frames (5-15s at 60ms/frame) */
    p->life = p->max_life;

    int rc = rand() % 100;
    if (rc < 5)       p->color = 0xffff;  /* white  5% */
    else if (rc < 15) p->color = 0xfe5a;  /* pink  10% */
    else              p->color = 0xf800;  /* red   85% */
}

static void heart_frame_cb(lv_timer_t *timer)
{
    if (heart_timer == NULL) return;

    /* clear canvas */
    memset(heart_pixels, 0, HEART_PIXELS_BYTES);

    /* spawn new particles (ramp up gradually) */
    int target = 40 + (heart_frame * 8); /* ~245 frames (15s) to reach 2000 */
    if (target > MAX_PARTICLES) target = MAX_PARTICLES;
    while (particle_count < target) {
        particle_init(&particles[particle_count]);
        particle_count++;
    }

    /* update & draw all particles (arrived ones stay at endpoint) */
    for (int i = 0; i < particle_count; i++) {
        heart_particle_t *p = &particles[i];
        if (p->life > 0) {
            int elapsed = p->max_life - p->life;
            p->cx = p->sx + (p->ex - p->sx) * elapsed / p->max_life;
            p->cy = p->sy + (p->ey - p->sy) * elapsed / p->max_life;
            p->life--;
            if (p->life == 0) {
                p->cx = p->ex;  /* snap to exact endpoint */
                p->cy = p->ey;
            }
        }

        if ((uint16_t)p->cx < HEART_SIZE && (uint16_t)p->cy < HEART_SIZE) {
            heart_pixels[p->cy * HEART_SIZE + p->cx] = p->color;
        }
    }

    /* all particles arrived? → show love-you text once */
    if (!heart_text_shown && particle_count >= MAX_PARTICLES) {
        bool all_arrived = true;
        for (int i = 0; i < particle_count; i++) {
            if (particles[i].life > 0) { all_arrived = false; break; }
        }
        if (all_arrived && heart_img_obj) {
            heart_text_shown = true;
            lv_obj_t *label = lv_label_create(lv_obj_get_parent(heart_img_obj));
            lv_label_set_text(label, "love you");
            lv_obj_align_to(label, heart_img_obj, LV_ALIGN_CENTER, -8, 0);
            lv_obj_set_style_text_color(label, lv_color_make(255, 0, 0), 0);
            lv_obj_set_style_text_font(label, &lv_font_alibaba_22, 0);
            lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, label);
            lv_anim_set_exec_cb(&a, fade_in_cb);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&a, 1500);
            lv_anim_set_playback_duration(&a, 1500);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    }

    heart_frame++;
    if (heart_img_obj) lv_image_set_src(heart_img_obj, &heart_img);
}

static void heart_delete_cb(lv_event_t *e)
{
    if (heart_timer) {
        lv_timer_del(heart_timer);
        heart_timer = NULL;
    }
    heart_img_obj = NULL;
    free(heart_pixels);  heart_pixels = NULL;
    free(heart_ox);      heart_ox = NULL;
    free(heart_oy);      heart_oy = NULL;
    free(particles);     particles = NULL;
}

static void precompute_heart(void)
{
    int cx = HEART_SIZE / 2, cy = HEART_SIZE / 2;
    float scale = 5.5f;

    for (int i = 0; i < HEART_OUTLINE_SAMPLES; i++) {
        float t = 2.0f * M_PI * i / HEART_OUTLINE_SAMPLES;
        float st = sinf(t);
        float x = 16.0f * st * st * st;
        float y = -(13.0f * cosf(t) - 5.0f * cosf(2.0f * t)
                    - 2.0f * cosf(3.0f * t) - cosf(4.0f * t));
        heart_ox[i] = cx + (int16_t)(x * scale);
        heart_oy[i] = cy + (int16_t)(y * scale);
    }
}

static void build_heart_page(void)
{
    /* allocate buffers — freed in heart_delete_cb on page switch */
    heart_pixels = calloc(1, HEART_PIXELS_BYTES);
    heart_ox     = malloc(HEART_OUTLINE_BYTES);
    heart_oy     = malloc(HEART_OUTLINE_BYTES);
    particles    = malloc(HEART_PARTICLES_BYTES);
    if (!heart_pixels || !heart_ox || !heart_oy || !particles) {
        ESP_LOGE(TAG, "Heart: out of memory");
        free(heart_pixels); free(heart_ox); free(heart_oy); free(particles);
        return;
    }

    srand(time(NULL) ^ (uint32_t)heart_pixels);

    heart_img.header.w = HEART_SIZE;
    heart_img.header.h = HEART_SIZE;
    heart_img.header.cf = LV_COLOR_FORMAT_RGB565;
    heart_img.data = (uint8_t *)heart_pixels;
    heart_img.data_size = HEART_PIXELS_BYTES;

    precompute_heart();
    heart_frame = 0;
    particle_count = 0;
    heart_text_shown = false;

    heart_img_obj = lv_image_create(ui_scr);
    lv_image_set_src(heart_img_obj, &heart_img);
    lv_obj_center(heart_img_obj);
    lv_obj_add_event_cb(heart_img_obj, heart_delete_cb, LV_EVENT_DELETE, NULL);

    heart_timer = lv_timer_create(heart_frame_cb, 60, NULL);

    /* Bottom hint */
    lv_obj_t *hint = lv_label_create(ui_scr);
    lv_label_set_text(hint, "<---长按退出");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    lv_obj_set_style_text_color(hint, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(hint, &lv_font_alibaba_22, 0);
    lv_obj_set_style_opa(hint, LV_OPA_TRANSP, 0);

    lv_anim_t ha;
    lv_anim_init(&ha);
    lv_anim_set_var(&ha, hint);
    lv_anim_set_exec_cb(&ha, fade_in_cb);
    lv_anim_set_values(&ha, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ha, 1500);
    lv_anim_set_delay(&ha, 20000);
    lv_anim_set_playback_duration(&ha, 1500);
    lv_anim_set_repeat_count(&ha, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&ha);

    ESP_LOGI(TAG, "Built heart page");
}

/* ---- page switching ---- */

static void switch_to_page(page_builder_t page)
{
    lv_obj_clean(ui_scr);
    lv_obj_set_style_bg_color(ui_scr, lv_color_make(0, 0, 0), 0);
    prev_page = current_page;
    current_page = page;
    if (page) page();
}

/* ---- key handler ---- */

static const char *color_name(int s)
{
    switch (s) {
    case 0: return "RED";
    case 1: return "GREEN";
    case 2: return "WHITE";
    default: return "?";
    }
}

extern volatile int esc_repeat_reset;
static int esc_repeat_cnt;
static bool page_switched;  /* block re-switch until ESC released */

static void key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);

    /* ESC release resets switch lock */
    if (esc_repeat_reset) {
        esc_repeat_cnt = 0;
        esc_repeat_reset = 0;
        page_switched = false;
    }

    /* ESC long press detection: count repeated KEY events */
    if (key == LV_KEY_ESC) {
        esc_repeat_cnt++;
        if (esc_repeat_cnt >= 2 && !page_switched) {
            page_switched = true;
            if (current_page == build_key_test_page) {
                if (is_time_synced()) {
                    switch_to_page(build_birthday_page);
                } else {
                    switch_to_page(build_timesync_page);
                    prev_page = build_birthday_page;  /* real target */
                }
            } else if (current_page == build_birthday_page) {
                switch_to_page(build_key_test_page);
            } else {
                /* timesync — go back to key_test; unknown — fallback */
                switch_to_page(build_key_test_page);
            }
            return;
        }
    } else {
        esc_repeat_cnt = 0;
    }

    /* DOWN short press: birthday → heart page */
    if (current_page == build_birthday_page && key == LV_KEY_DOWN && !page_switched) {
        page_switched = true;
        switch_to_page(build_heart_page);
        return;
    }

    if (current_page != build_key_test_page) return;

    int idx = -1;
    lv_obj_t *box = NULL;
    switch (key) {
    case LV_KEY_ESC:   idx = 0; box = box_esc;   break;
    case LV_KEY_ENTER: idx = 1; box = box_enter; break;
    case LV_KEY_DOWN:  idx = 2; box = box_down;  break;
    case LV_KEY_UP:    idx = 3; box = box_up;    break;
    }

    if (idx >= 0) {
        cycle_box(idx, box);
        ESP_LOGI(TAG, "Key: %s -> %s",
                 idx == 0 ? "ESC" : idx == 1 ? "ENTER" : idx == 2 ? "DOWN" : "UP",
                 color_name(box_state[idx]));
    }
}

/* ---- splash ---- */

static void splash_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    lv_obj_clean(ui_scr);
    lv_obj_set_style_bg_color(ui_scr, lv_color_make(0, 0, 0), 0);
    switch_to_page(build_key_test_page);
}

static void show_splash(void)
{
    lv_obj_remove_style_all(ui_scr);
    lv_obj_set_style_bg_color(ui_scr, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(ui_scr, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_label_create(ui_scr);
    lv_label_set_text(logo, "淇喵盒子");
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_text_color(logo, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_text_font(logo, &lv_font_alibaba_22, 0);

    lv_obj_t *build = lv_label_create(ui_scr);
    lv_label_set_text(build, __DATE__ " " __TIME__);
    lv_obj_align(build, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_text_color(build, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(build, &lv_font_montserrat_14, 0);

    lv_timer_create(splash_timer_cb, 1000, (void *)ui_scr);
    ESP_LOGI(TAG, "Splash screen shown");
}

/* ---- entry ---- */

void miaobox_ui(lv_disp_t *disp)
{
    ESP_LOGI(TAG, "Initializing miaobox UI");
    ui_disp = disp;
    ui_scr = lv_display_get_screen_active(disp);

    /* Setup group + key handler (once, before splash) */
    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, ui_scr);

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, group);
            lv_indev_set_long_press_time(indev, 2000);
            break;
        }
        indev = lv_indev_get_next(indev);
    }

    lv_obj_add_event_cb(ui_scr, key_event_cb, LV_EVENT_KEY, NULL);

    show_splash();
}
