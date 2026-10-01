// hostsim/lvgl/harness.c —— 主持屏离屏渲染:用 ww_core + ww_host 摆出各个真实局面,
// 交给固件同一份 ww_ui.c 画,导出 PNG,并打印每屏 LVGL 堆占用。
//
// 每一屏都是整屏重建(旧屏删掉),所以打印的堆占用就是"单屏峰值"。主机指针
// 8 字节,数字比设备上偏大,当上界看。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "ui_pixel.h"
#include "ww_core.h"
#include "ww_host.h"

// ---- 桩 ----
static int g_soc = 76;
int bsp_battery_soc(void) { return g_soc; }

#include "../../main/ww_ui.c"

// ---- 离屏显示 ----
#define W 240
#define H 320
static uint8_t fb[W * H * 2];
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}
static uint32_t g_tick;
static uint32_t tick_cb(void) { return g_tick; }

static size_t g_peak;

static void snap(const char *name)
{
    lv_refr_now(NULL);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    size_t used = (size_t)(mon.total_size - mon.free_size);
    if (used > g_peak) g_peak = used;
    printf("%-26s LVGL 堆 %6zu B  碎片 %2d%%\n", name, used, (int)mon.frag_pct);
    const char *dir = getenv("ZSIM_OUT") ? getenv("ZSIM_OUT") : "out";
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = (uint16_t)(fb[i * 2] | (fb[i * 2 + 1] << 8));
        uint8_t rgb[3] = {
            (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
            (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
            (uint8_t)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// ---- 局面 ----
static ww_game_t G;
static ww_host_t Hs;
static uint32_t NOW = 1000;
static ww_view_t V;

static void show(const char *name)
{
    // 强制整屏重建,量单屏峰值
    s_sig[0] = '\0';
    ww_host_view(&Hs, NOW, &V);
    ww_ui_render(&V);
    snap(name);
}

static void key(ww_key_t k, ww_gesture_t g)
{
    NOW += 200;
    ww_host_key(&Hs, k, g, NOW);
    ww_tick(&G, NOW);
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        NOW += 100;
        for (int i = 0; i < WW_MAX_PLAYERS; i++) if (G.players[i].used) ww_touch(&G, i, NOW);
        ww_tick(&G, NOW);
    }
}

// 夜晚各阶段的时长是随机的,不能按固定毫秒数等:一直走到指定阶段为止
static void until(ww_phase_t ph, int step)
{
    for (int k = 0; k < 2000 && !(G.phase == ph && G.step == step); k++) run(100);
}

static int P[WW_MAX_SEATS + 1];

static void fill(int n, int online_upto)
{
    static const char *NAMES[] = {"小明", "阿花", "Bob", "老王", "Lily", "张三", "李四", "王五",
                                  "赵六", "Amy", "小红", "大黄"};
    ww_set_seats(&G, n);
    for (int s = 1; s <= n; s++) {
        P[s] = ww_join(&G, NAMES[s - 1], NOW, NULL);
        ww_act(&G, P[s], WW_ACT_SEAT, s, 0, NULL, NOW);
        if (s > online_upto) G.players[P[s]].last_seen = NOW - 60000;
    }
}

static int seat_role(ww_role_t r, int nth)
{
    for (int s = 1; s <= G.seat_count; s++) {
        if (G.players[P[s]].role == r && nth-- == 0) return s;
    }
    return 0;
}

static void render_all(void)
{
    ww_init(&G, 20260929);
    ww_host_init(&Hs, &G);
    strcpy(Hs.net.ssid, "Werewolf-A1B2");
    strcpy(Hs.net.pass, "wolf1234");
    strcpy(Hs.net.ip, "192.168.4.1");

    Hs.net.mode = WW_NET_BOOT;
    show("00-boot");
    Hs.net.mode = WW_NET_CONNECTING;
    strcpy(Hs.net.ssid, "HomeWiFi");
    show("01-connecting");

    // 热点模式大厅
    Hs.net.mode = WW_NET_AP;
    strcpy(Hs.net.ssid, "Werewolf-A1B2");
    ww_set_seats(&G, 8);
    int a = ww_join(&G, "小明", NOW, NULL); ww_act(&G, a, WW_ACT_SEAT, 1, 0, NULL, NOW);
    int b = ww_join(&G, "Bob", NOW, NULL); ww_act(&G, b, WW_ACT_SEAT, 4, 0, NULL, NOW);
    int c = ww_join(&G, "阿花", NOW, NULL); ww_act(&G, c, WW_ACT_SEAT, 5, 0, NULL, NOW);
    G.players[c].last_seen = NOW - 60000;
    ww_tick(&G, NOW);
    show("02-lobby-ap-wifi-qr");
    key(WW_KEY_OK, WW_GES_DOUBLE);
    show("03-lobby-ap-url-qr");
    key(WW_KEY_OK, WW_GES_CLICK);
    show("04-lobby-missing-toast");

    // 家里 Wi-Fi 大厅,12 座
    ww_kick_all(&G);
    Hs.net.mode = WW_NET_STA;
    strcpy(Hs.net.ssid, "HomeWiFi");
    strcpy(Hs.net.ip, "192.168.31.87");
    Hs.qr_page = 0;
    fill(12, 10);
    ww_tick(&G, NOW);
    show("05-lobby-sta-12");
    key(WW_KEY_OK, WW_GES_CLICK);
    show("06-lobby-confirm-start");

    // 开局
    key(WW_KEY_OK, WW_GES_CLICK);
    show("07-deal");
    until(WW_PH_NIGHT, 0);
    show("08-night");
    until(WW_PH_WOLF, 0);
    run(7000);
    show("09-wolf-open");
    for (int k = 0; k < 4; k++) {
        ww_act(&G, P[seat_role(WW_ROLE_WOLF, k)], WW_ACT_WOLF, seat_role(WW_ROLE_VILLAGER, 0), 0, NULL, NOW);
    }
    until(WW_PH_WOLF, 1);
    show("10-wolf-close");
    until(WW_PH_WITCH, 0);
    ww_act(&G, P[seat_role(WW_ROLE_WITCH, 0)], WW_ACT_WITCH, 2, seat_role(WW_ROLE_VILLAGER, 1), NULL, NOW);
    key(WW_KEY_OK, WW_GES_CLICK);
    show("11-witch-force-confirm");
    until(WW_PH_SEER, 0);
    ww_act(&G, P[seat_role(WW_ROLE_SEER, 0)], WW_ACT_SEER, seat_role(WW_ROLE_WOLF, 0), 0, NULL, NOW);
    until(WW_PH_DAWN, 0);
    show("12-dawn-deaths");
    until(WW_PH_DISCUSS, 0);
    key(WW_KEY_DOWN, WW_GES_CLICK);
    key(WW_KEY_DOWN, WW_GES_CLICK);
    run(35000);
    show("13-discussion");
    // AI 玩家相关的状态(封面图也用这几张)
    Hs.key_ds = Hs.key_dk = Hs.ai_on = true;
    ww_set_busy(&G, WW_BUSY_SPEAK, ww_current_speaker(&G));
    show("13b-ai-speaking");
    ww_set_busy(&G, WW_BUSY_REC, ww_current_speaker(&G));
    show("13c-recording");
    ww_set_busy(&G, WW_BUSY_NONE, 0);
    key(WW_KEY_OK, WW_GES_DOUBLE);
    show("14-status");
    key(WW_KEY_OK, WW_GES_CLICK);
    key(WW_KEY_OK, WW_GES_CLICK);
    key(WW_KEY_OK, WW_GES_CLICK);
    int voted = 0;
    for (int s = 1; s <= 12 && voted < 6; s++) {
        if (!G.players[P[s]].alive) continue;
        ww_act(&G, P[s], WW_ACT_VOTE, seat_role(WW_ROLE_WOLF, 0) == s ? 0 : seat_role(WW_ROLE_WOLF, 0), 0, NULL, NOW);
        voted++;
    }
    run(12000);
    show("15-vote");
    key(WW_KEY_OK, WW_GES_CLICK);
    key(WW_KEY_OK, WW_GES_CLICK);
    show("16-result");

    // 主持菜单(对局中)
    key(WW_KEY_OK, WW_GES_LONG);
    show("17-menu-ingame");

    // 游戏结束:造一个好人胜
    Hs.screen = WW_HS_MAIN;
    for (int k = 0; k < 4; k++) G.players[P[seat_role(WW_ROLE_WOLF, k)]].alive = false;
    run(WW_T_RESULT + 200);
    show("18-over-good");

    // 大厅菜单 / 踢人 / 网络 / 音量
    ww_reset(&G);
    key(WW_KEY_OK, WW_GES_LONG);
    show("19-menu-lobby");
    key(WW_KEY_OK, WW_GES_CLICK);
    for (int k = 0; k < 7; k++) key(WW_KEY_DOWN, WW_GES_CLICK);
    show("20-menu-kick");
    key(WW_KEY_OK, WW_GES_LONG);
    key(WW_KEY_DOWN, WW_GES_CLICK);
    key(WW_KEY_OK, WW_GES_CLICK);
    show("21-menu-net");
    key(WW_KEY_OK, WW_GES_LONG);
    key(WW_KEY_DOWN, WW_GES_CLICK);
    key(WW_KEY_OK, WW_GES_CLICK);
    show("22-volume");

    // 配网模式
    Hs.screen = WW_HS_MAIN;
    Hs.net.mode = WW_NET_SETUP;
    strcpy(Hs.net.ssid, "Werewolf-A1B2");
    strcpy(Hs.net.ip, "192.168.4.1");
    Hs.qr_page = 1;
    show("23-setup-url");

    // 回落提示 + 电量读不到
    Hs.net.mode = WW_NET_AP;
    strcpy(Hs.net.note, "连不上家里Wi-Fi，已改用热点");
    Hs.note_until = NOW + 15000;
    Hs.qr_page = 0;
    g_soc = -1;
    ww_kick_all(&G);
    show("24-lobby-fallback-nobatt");
    g_soc = 12;
    Hs.net.note[0] = '\0';

    // 2 人局 狼胜
    ww_kick_all(&G);
    fill(2, 2);
    ww_start(&G, NOW);
    run(WW_T_DEAL + WW_T_NIGHT + 400);
    ww_act(&G, P[seat_role(WW_ROLE_WOLF, 0)], WW_ACT_WOLF, seat_role(WW_ROLE_VILLAGER, 0), 0, NULL, NOW);
    for (int k = 0; k < 40 && G.phase != WW_PH_OVER; k++) run(1000);
    show("25-over-wolf");
    printf("单屏峰值 LVGL 堆:%zu B(主机 64 位指针,设备上更小)\n", g_peak);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    render_all();
    return 0;
}
