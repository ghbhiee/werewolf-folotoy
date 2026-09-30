// main/ww_app.c —— 固件胶水层,见 ww_app.h。
//
// 任务/线程与锁:
//   按键回调(button 定时器任务)  只把事件塞进队列,不阻塞、不碰锁
//   ww_game 任务(100 ms 一拍)    取按键 → ww_host_key;ww_tick;放提示音;处理网络请求
//   httpd 任务                     ww_api_handle(见 ww_http.c)
//   LVGL 定时器(200 ms)          try-lock 游戏锁,算视图,ww_ui_render
// 以上凡是碰 ww_game_t / ww_host_t 的都先拿 s_lock。LVGL 定时器已经持有 LVGL 锁,
// 它只 try-lock 游戏锁(拿不到就跳过这一帧),而别处从不在持游戏锁时去拿 LVGL 锁,
// 所以不会死锁。
#include "ww_app.h"

#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ww_brain.h"
#include "ww_cloud.h"
#include "ww_http.h"
#include "ww_net.h"
#include "ww_sound.h"
#include "ww_ui.h"

static const char *TAG = "ww_app";

static ww_game_t s_game;
static ww_host_t s_host;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_keys;
static ww_view_t s_view;           // 只在 LVGL 定时器里用

typedef struct {
    uint8_t key;
    uint8_t ges;
} key_ev_t;

void ww_app_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void ww_app_unlock(void) { xSemaphoreGive(s_lock); }
ww_game_t *ww_app_game(void) { return &s_game; }
ww_host_t *ww_app_host(void) { return &s_host; }
uint32_t ww_app_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ww_app_set_net(const ww_net_info_t *info)
{
    ww_app_lock();
    s_host.net = *info;
    if (info->note[0]) s_host.note_until = ww_app_now() + 15000;
    if (info->mode == WW_NET_SETUP || info->mode == WW_NET_AP) s_host.qr_page = 0;
    ww_app_unlock();
}

// ---------------------------------------------------------------------------
// 按键:回调里只入队
// ---------------------------------------------------------------------------
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    key_ev_t k;
    switch (ev) {
    case BSP_BTN_CLICK:  k.ges = WW_GES_CLICK; break;
    case BSP_BTN_DOUBLE: k.ges = WW_GES_DOUBLE; break;
    case BSP_BTN_LONG:   k.ges = WW_GES_LONG; break;
    case BSP_BTN_RELEASE: k.ges = WW_GES_RELEASE; break;
    default:             return;               // PRESS 不用
    }
    k.key = btn == BSP_BTN_UP ? WW_KEY_UP : btn == BSP_BTN_DOWN ? WW_KEY_DOWN : WW_KEY_OK;
    (void)xQueueSend(s_keys, &k, 0);
}

// ---------------------------------------------------------------------------
// 游戏任务
// ---------------------------------------------------------------------------
static void game_task(void *arg)
{
    (void)arg;
    uint32_t last_heap_log = 0;
    for (;;) {
        key_ev_t k;
        // 等按键最多 100 ms,顺便当节拍器
        bool got = xQueueReceive(s_keys, &k, pdMS_TO_TICKS(100)) == pdTRUE;
        uint32_t now = ww_app_now();

        ww_req_t req = WW_REQ_NONE;
        int cues[WW_CUE_QUEUE];
        int n_cues = 0;
        int vol = -1;
        int ptt = 0;

        ww_app_lock();
        ww_cloud_keys(&s_host.key_ds, &s_host.key_dk);
        s_game.brain = ww_host_ai_ready(&s_host);
        if (got) ww_host_key(&s_host, (ww_key_t)k.key, (ww_gesture_t)k.ges, now);
        ptt = s_host.ptt_req;
        s_host.ptt_req = 0;
        ww_tick(&s_game, now);
        ww_cue_t c;
        while ((c = ww_pop_cue(&s_game)) != WW_CUE_NONE && n_cues < WW_CUE_QUEUE) cues[n_cues++] = c;
        req = ww_host_take_request(&s_host);
        if (s_host.volume_dirty) {
            s_host.volume_dirty = false;
            vol = s_host.volume;
        }
        ww_app_unlock();

        // 慢活都在锁外做
        if (got && !n_cues && k.ges != WW_GES_RELEASE && !ptt) ww_sound_play(WW_SND_KEY);
        if (ptt) ww_brain_ptt(ptt);
        if (req == WW_REQ_AI_TEST) ww_brain_test();
        if (req == WW_REQ_AI_SAVE) ww_cloud_set_ai_on(s_host.ai_on);
        for (int i = 0; i < n_cues; i++) ww_sound_play(cues[i]);
        if (vol >= 0) ww_sound_set_volume((uint8_t)vol);
        if (req == WW_REQ_NET_AP || req == WW_REQ_NET_SETUP || req == WW_REQ_NET_FORGET || req == WW_REQ_NET_STA)
            ww_net_request(req);

        if (now - last_heap_log > 30000) {
            last_heap_log = now;
            ESP_LOGI(TAG, "heap free %u  min %u  largest %u  | 在座 %d  阶段 %s",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     ww_seated_count(&s_game), ww_phase_key(s_game.phase));
        }
    }
}

// ---------------------------------------------------------------------------
// 屏幕:LVGL 定时器回调(已持 LVGL 锁)
// ---------------------------------------------------------------------------
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t last_batt, last_lv;
    static size_t lv_peak;
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        size_t used = mon.total_size - mon.free_size;
        if (used > lv_peak) lv_peak = used;
        uint32_t n = ww_app_now();
        if (n - last_lv > 30000) {
            last_lv = n;
            ESP_LOGI(TAG, "LVGL 池 %u / %u B,峰值 %u", (unsigned)used, (unsigned)mon.total_size, (unsigned)lv_peak);
        }
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;      // 拿不到就等下一帧
    ww_host_view(&s_host, ww_app_now(), &s_view);
    xSemaphoreGive(s_lock);
    ww_ui_render(&s_view);
    uint32_t now = ww_app_now();
    if (now - last_batt > 10000) {
        last_batt = now;
        ww_ui_battery_refresh();
    }
}

void ww_app_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_keys = xQueueCreate(8, sizeof(key_ev_t));
    ww_init(&s_game, esp_random());
    ww_host_init(&s_host, &s_game);
    ww_cloud_init();
    s_host.ai_on = ww_cloud_ai_on();
    ww_cloud_keys(&s_host.key_ds, &s_host.key_dk);
    ESP_LOGI(TAG, "游戏状态 %u 字节(静态)", (unsigned)sizeof(s_game));

    if (bsp_lvgl_lock(1000)) {
        ww_host_view(&s_host, ww_app_now(), &s_view);
        ww_ui_render(&s_view);
        lv_timer_create(ui_timer_cb, 200, NULL);
        bsp_lvgl_unlock();
    }

    xTaskCreate(game_task, "ww_game", 4096, NULL, 5, NULL);
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGE(TAG, "按键初始化失败");
    ww_net_start();
    ww_http_start();
    ww_brain_start();
}
