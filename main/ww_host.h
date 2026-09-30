// main/ww_host.h —— FoloToy 主持端:三键 → 游戏动作,以及"屏幕上该显示什么"。
//
// 纯 C,不碰 LVGL / ESP-IDF。按键在这里变成对 ww_core 的调用;屏幕内容先算成
// 一个 ww_view_t(视图模型),再交给 ww_ui.c 用 LVGL 画、或交给主机仿真服务器
// 转成 JSON 在浏览器里画。这样按键逻辑和屏幕文案只有一份,可以在主机上测。
//
// 线程模型同 ww_core:调用方负责加锁。
#pragma once

#include "ww_core.h"

typedef enum { WW_KEY_UP = 0, WW_KEY_DOWN, WW_KEY_OK } ww_key_t;
typedef enum { WW_GES_CLICK = 0, WW_GES_DOUBLE, WW_GES_LONG, WW_GES_RELEASE } ww_gesture_t;

typedef enum {
    WW_NET_BOOT = 0,      // 刚开机
    WW_NET_CONNECTING,    // 正在连家里 Wi-Fi
    WW_NET_AP,            // 自己开热点
    WW_NET_STA,           // 已连上家里 Wi-Fi
    WW_NET_SETUP,         // 配网模式(热点 + /setup 页面)
} ww_net_mode_t;

typedef struct {
    ww_net_mode_t mode;
    char ssid[33];        // 热点名 / 家里 Wi-Fi 名
    char pass[65];        // 热点密码(仅热点/配网模式)
    char ip[16];
    char note[64];        // 一句说明,例如"连不上家里 Wi-Fi,已改用热点"
} ww_net_info_t;

// 主持端请求固件做的事(网络切换要重启 Wi-Fi,不能在锁里做)
typedef enum {
    WW_REQ_NONE = 0,
    WW_REQ_NET_AP,        // 改用热点模式
    WW_REQ_NET_SETUP,     // 进入配网模式
    WW_REQ_NET_FORGET,    // 忘记家里 Wi-Fi(并改用热点)
    WW_REQ_NET_STA,       // 连回家里 Wi-Fi(用存着的凭据)
    WW_REQ_AI_TEST,       // 测一下 DeepSeek + 千问语音
    WW_REQ_AI_SAVE,       // ai_on 变了,存 NVS
} ww_req_t;

typedef enum {
    WW_HS_MAIN = 0,
    WW_HS_MENU,
    WW_HS_KICK,
    WW_HS_NET,
    WW_HS_VOLUME,
    WW_HS_STATUS,
    WW_HS_AI,             // AI 设置子菜单
    WW_HS_KEYS,           // 扫码填 Key 页
} ww_host_screen_t;

typedef struct {
    ww_game_t *g;
    ww_net_info_t net;
    uint32_t note_until;      // net.note 在大厅底栏显示到这个时刻
    ww_host_screen_t screen;
    int sel;
    int parent_sel;           // 进子菜单前主持菜单的选中项,返回时恢复
    int confirm;              // 等待二次确认的动作,0 = 无
    uint32_t confirm_until;
    char toast[64];
    uint32_t toast_until;
    int qr_page;              // 热点模式大厅:0 = Wi-Fi 二维码,1 = 网址二维码
    uint8_t volume;           // 0..100
    bool volume_dirty;
    ww_req_t request;
    // AI(由固件填/读)
    bool key_ds;              // 千问(DashScope)Key 已配
    bool key_dk;              // DeepSeek Key 已配
    bool ai_on;               // AI 玩家开关(配齐 Key 且打开才生效)
    bool ptt;                 // 正按住确定录音
    int ptt_req;              // 1 = 开始录音,2 = 松手结束(固件取走后清零)
    char ai_msg[64];          // 最近一次 AI 测试/出错的一句话
} ww_host_t;

// ---- 视图模型 ----
typedef enum {
    WWV_BOOT = 0,     // 开机/连网中
    WWV_LOBBY,        // 大厅:二维码 + 座位条
    WWV_SETUP,        // 配网:二维码
    WWV_GAME,         // 对局中:阶段大字 + 座位格
    WWV_MENU,         // 菜单/子菜单
    WWV_STATUS,       // 状态页
    WWV_VOLUME,
} ww_view_kind_t;

// 座位格标志
#define WWV_SEAT_OCC    0x01    // 有人
#define WWV_SEAT_ON     0x02    // 在线
#define WWV_SEAT_DEAD   0x04    // 出局
#define WWV_SEAT_VOTED  0x08    // 已投票
#define WWV_SEAT_SPEAK  0x10    // 正在发言
#define WWV_SEAT_OUT    0x20    // 刚被放逐/昨晚死亡(高亮)

#define WWV_ITEMS_MAX   14
#define WWV_LINES_MAX   10

typedef struct {
    ww_view_kind_t kind;
    bool night;               // 夜晚配色
    char title[24];
    // 大厅/配网
    char qr[128];
    char qr_label[64];
    char qr_label2[64];
    char info[64];
    // 对局
    char big[48];
    char line1[160];
    char line2[96];
    int timer;                // 秒;-1 不显示
    bool timer_down;          // true = 倒计时
    // 座位
    uint8_t n_seats;
    uint8_t seat[WW_MAX_SEATS];
    // 菜单
    uint8_t n_items;
    int8_t sel;
    int8_t top;               // 列表窗口第一行(屏幕放不下全部时)
    char items[WWV_ITEMS_MAX][32];
    // 状态页
    uint8_t n_lines;
    char lines[WWV_LINES_MAX][64];
    // 音量
    int volume;
    // 底栏
    char hint[80];
    bool alert;               // 底栏是二次确认/提示,要醒目
} ww_view_t;

#define WWV_MENU_ROWS 6       // 菜单一屏最多显示几行

void ww_host_init(ww_host_t *h, ww_game_t *g);
void ww_host_key(ww_host_t *h, ww_key_t key, ww_gesture_t ges, uint32_t now);
void ww_host_view(const ww_host_t *h, uint32_t now, ww_view_t *v);
ww_req_t ww_host_take_request(ww_host_t *h);
bool ww_host_ai_ready(const ww_host_t *h);    // AI 开着且两个 Key 都有
void ww_host_toast(ww_host_t *h, const char *text, uint32_t now);

// 视图 → JSON(给主机仿真的浏览器面板用)
size_t ww_view_json(const ww_view_t *v, char *buf, size_t cap);
