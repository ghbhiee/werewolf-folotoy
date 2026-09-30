// tests/test_ww_host.c —— FoloToy 三键主持逻辑与视图模型的主机单测。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_ww_host.c main/ww_host.c main/ww_core.c
#include "ww_host.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

static ww_game_t G;
static ww_host_t H;
static ww_view_t V;
static uint32_t NOW;

static void key(ww_key_t k, ww_gesture_t g)
{
    NOW += 300;
    ww_host_key(&H, k, g, NOW);
    ww_tick(&G, NOW);
}

static void click(ww_key_t k) { key(k, WW_GES_CLICK); }

static void view(void) { ww_host_view(&H, NOW, &V); }

static void wait_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        NOW += 100;
        for (int i = 0; i < WW_MAX_PLAYERS; i++) if (G.players[i].used) ww_touch(&G, i, NOW);
        ww_tick(&G, NOW);
    }
}

static void fill(int n)
{
    for (int s = 1; s <= n; s++) {
        char nm[16];                                     // GCC 会按 int 最长 10 位算,给够
        snprintf(nm, sizeof(nm), "p%d", s);
        int i = ww_join(&G, nm, NOW, NULL);
        CHECK(ww_act(&G, i, WW_ACT_SEAT, s, 0, NULL, NOW) == WW_OK);
    }
}

static void setup(void)
{
    ww_init(&G, 1234);
    ww_host_init(&H, &G);
    NOW = 5000;
    H.net.mode = WW_NET_AP;
    strcpy(H.net.ssid, "Werewolf-A1B2");
    strcpy(H.net.pass, "wolf1234");
    strcpy(H.net.ip, "192.168.4.1");
}

static void test_boot_and_lobby(void)
{
    setup();
    H.net.mode = WW_NET_CONNECTING;
    view();
    CHECK(V.kind == WWV_BOOT);
    H.net.mode = WW_NET_AP;
    view();
    CHECK(V.kind == WWV_LOBBY);
    CHECK(strcmp(V.qr, "WIFI:S:Werewolf-A1B2;T:WPA;P:wolf1234;;") == 0);
    key(WW_KEY_OK, WW_GES_DOUBLE);                       // 切到网址二维码
    view();
    CHECK(strcmp(V.qr, "http://192.168.4.1/") == 0);
    key(WW_KEY_OK, WW_GES_DOUBLE);
    view();
    CHECK(strncmp(V.qr, "WIFI:", 5) == 0);

    // STA 模式只有网址二维码,双击是状态页
    H.net.mode = WW_NET_STA;
    strcpy(H.net.ip, "10.0.0.23");
    view();
    CHECK(strcmp(V.qr, "http://10.0.0.23/") == 0);
    key(WW_KEY_OK, WW_GES_DOUBLE);
    view();
    CHECK(V.kind == WWV_STATUS);
    click(WW_KEY_DOWN);                                  // 任意键返回
    view();
    CHECK(V.kind == WWV_LOBBY);

    // 座位数:上 +1,下 -1;减座位时高号的人挪到空着的低号,下限 = 已入座人数
    CHECK(G.seat_count == 8);
    click(WW_KEY_UP);
    CHECK(G.seat_count == 9);
    int i = ww_join(&G, "x", NOW, NULL);
    ww_act(&G, i, WW_ACT_SEAT, 7, 0, NULL, NOW);
    int i2 = ww_join(&G, "y", NOW, NULL);
    ww_act(&G, i2, WW_ACT_SEAT, 9, 0, NULL, NOW);
    int i3 = ww_join(&G, "z", NOW, NULL);
    ww_act(&G, i3, WW_ACT_SEAT, 2, 0, NULL, NOW);
    click(WW_KEY_DOWN);
    CHECK(G.seat_count == 8 && G.players[i2].seat == 1);      // 9 号挪到最小的空位 1 号
    for (int k = 0; k < 10; k++) click(WW_KEY_DOWN);
    CHECK(G.seat_count == 3);
    CHECK(G.players[i3].seat == 2 && G.players[i2].seat == 1 && G.players[i].seat == 3);
    view();
    CHECK(V.alert && strstr(V.hint, "已有3人入座") != NULL);
    CHECK(V.n_seats == 3 && (V.seat[0] & WWV_SEAT_OCC) && (V.seat[2] & WWV_SEAT_OCC));
    ww_kick(&G, 3);
    ww_kick(&G, 2);
    ww_kick(&G, 1);
    ww_set_seats(&G, 7);

    // 没坐满不能开始
    click(WW_KEY_OK);
    view();
    CHECK(strstr(V.hint, "还差7人") != NULL);
    CHECK(G.phase == WW_PH_LOBBY);
}

static void test_start_confirm(void)
{
    setup();
    ww_set_seats(&G, 4);
    fill(4);
    click(WW_KEY_OK);
    view();
    CHECK(G.phase == WW_PH_LOBBY);
    CHECK(V.alert && strstr(V.hint, "再按确定：开始游戏") != NULL);
    // 超过 3 秒就失效
    NOW += 3500;
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_LOBBY);
    // 上/下取消
    click(WW_KEY_UP);
    CHECK(G.seat_count == 4);                            // 取消时不改座位数
    view();
    CHECK(!V.alert);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_DEAL);
    view();
    CHECK(V.kind == WWV_GAME && strstr(V.big, "查看手机") != NULL && V.timer_down);
}

// 对局中 FoloToy 屏幕上绝不出现身份
static bool view_leaks_role(void)
{
    const char *roles[] = {"狼人：", "预言家 ", "女巫 "};
    const char *fields[] = {V.big, V.line1, V.line2, V.hint, V.title};
    for (unsigned r = 0; r < 3; r++) {
        for (unsigned f = 0; f < 5; f++) if (strstr(fields[f], roles[r])) return true;
    }
    return false;
}

static void test_game_flow(void)
{
    setup();
    ww_set_seats(&G, 6);
    fill(6);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_DEAL);
    // 跳过发身份:两次确认
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_NIGHT);
    view();
    CHECK(V.night && strcmp(V.big, "天黑请闭眼") == 0);
    wait_ms(WW_T_NIGHT + 200);
    CHECK(G.phase == WW_PH_WOLF);
    view();
    CHECK(strcmp(V.big, "狼人请睁眼") == 0);
    CHECK(!V.timer_down);                                // 夜里只显示已等时间
    CHECK(!view_leaks_role());
    // 强推狼人 -> 闭眼 -> 女巫
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    view();
    CHECK(strcmp(V.big, "狼人请闭眼") == 0);
    wait_ms(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_WITCH);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    wait_ms(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_SEER);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    wait_ms(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_DAWN);
    view();
    CHECK(strstr(V.line1, "平安夜") != NULL);           // 空刀
    wait_ms(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_DISCUSS);
    view();
    char want[16];
    snprintf(want, sizeof(want), "%d号 发言", G.order[0]);
    CHECK(strcmp(V.big, want) == 0);
    CHECK(V.seat[G.order[0] - 1] & WWV_SEAT_SPEAK);
    click(WW_KEY_DOWN);
    CHECK(G.speaker == 1);
    click(WW_KEY_UP);
    click(WW_KEY_UP);
    CHECK(G.speaker == 0);
    CHECK(!view_leaks_role());
    // 确定两次 -> 投票
    click(WW_KEY_OK);
    view();
    CHECK(strstr(V.hint, "开始投票") != NULL);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_VOTE);
    int voter = ww_by_seat(&G, 1);
    ww_act(&G, voter, WW_ACT_VOTE, 2, 0, NULL, NOW);
    view();
    CHECK(strstr(V.line1, "已投 1 / 6") != NULL);
    CHECK((V.seat[0] & WWV_SEAT_VOTED) && !(V.seat[1] & WWV_SEAT_VOTED));
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_RESULT && G.out_seat == 2);
    view();
    CHECK(strcmp(V.big, "2号 出局") == 0);
    CHECK(strncmp(V.line1, "计票：", strlen("计票：")) == 0);
    CHECK(V.seat[1] & WWV_SEAT_OUT);
    // 状态页
    key(WW_KEY_OK, WW_GES_DOUBLE);
    view();
    CHECK(V.kind == WWV_STATUS && V.n_lines == 3 + 3);
    CHECK(strstr(V.lines[3], "出局") != NULL);           // 2号那行
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_RESULT);                      // 返回键不推进游戏
}

static void test_menu(void)
{
    setup();
    ww_set_seats(&G, 5);
    fill(3);
    key(WW_KEY_OK, WW_GES_LONG);
    view();
    CHECK(V.kind == WWV_MENU && strcmp(V.title, "主持菜单") == 0);
    CHECK(strcmp(V.items[0], "踢出玩家") == 0 && V.sel == 0);
    // 进踢人,选 2 号,确认两次
    click(WW_KEY_OK);
    view();
    CHECK(strcmp(V.title, "踢出玩家") == 0 && V.n_items == 4);
    click(WW_KEY_DOWN);
    click(WW_KEY_OK);
    CHECK(ww_seated_count(&G) == 3);
    click(WW_KEY_OK);
    CHECK(ww_seated_count(&G) == 2 && ww_by_seat(&G, 2) < 0);
    // 长按返回主持菜单,再长按回主界面
    key(WW_KEY_OK, WW_GES_LONG);
    view();
    CHECK(strcmp(V.title, "主持菜单") == 0);
    // 网络设置 -> 配置家里 Wi-Fi -> 发出请求
    click(WW_KEY_DOWN);
    click(WW_KEY_DOWN);
    click(WW_KEY_DOWN);
    click(WW_KEY_OK);
    view();
    CHECK(strcmp(V.title, "网络设置") == 0 && strcmp(V.items[0], "连回家里 Wi-Fi") == 0);
    click(WW_KEY_DOWN);
    click(WW_KEY_DOWN);
    click(WW_KEY_OK);
    CHECK(ww_host_take_request(&H) == WW_REQ_NONE);          // 要按两次
    view();
    CHECK(strstr(V.hint, "再按确定：进入配网") != NULL);
    click(WW_KEY_OK);
    CHECK(ww_host_take_request(&H) == WW_REQ_NET_SETUP);
    CHECK(ww_host_take_request(&H) == WW_REQ_NONE);
    H.net.mode = WW_NET_SETUP;
    view();
    CHECK(V.kind == WWV_SETUP && strncmp(V.qr, "WIFI:", 5) == 0);
    key(WW_KEY_OK, WW_GES_DOUBLE);
    view();
    CHECK(strcmp(V.qr, "http://192.168.4.1/setup") == 0);
    CHECK(strstr(V.hint, "按确定=退出") != NULL);
    click(WW_KEY_UP);                                       // 上/下不动
    CHECK(ww_host_take_request(&H) == WW_REQ_NONE);
    click(WW_KEY_OK);                                       // 单击确定 = 退出配网
    CHECK(ww_host_take_request(&H) == WW_REQ_NET_STA);
    H.net.mode = WW_NET_AP;
    // 音量
    key(WW_KEY_OK, WW_GES_LONG);
    for (int k = 0; k < 4; k++) click(WW_KEY_DOWN);
    click(WW_KEY_OK);
    view();
    CHECK(V.kind == WWV_VOLUME);
    click(WW_KEY_UP);
    CHECK(H.volume == 80 && H.volume_dirty);
    click(WW_KEY_OK);
    view();
    CHECK(V.kind == WWV_MENU);
    // 清空房间
    click(WW_KEY_DOWN);                                  // 音量 -> 清空房间
    view();
    CHECK(strcmp(V.items[V.sel], "清空房间") == 0);
    click(WW_KEY_OK);
    CHECK(ww_seated_count(&G) == 2);
    click(WW_KEY_OK);
    CHECK(ww_seated_count(&G) == 0);
    view();
    CHECK(V.kind == WWV_LOBBY && strstr(V.hint, "已清空") != NULL);
    // 机器人补满空位:5 座里真人 2 个,补 3 个;踢人列表里标出机器人
    ww_set_seats(&G, 5);
    fill(2);
    key(WW_KEY_OK, WW_GES_LONG);
    click(WW_KEY_DOWN);
    view();
    CHECK(strcmp(V.items[V.sel], "机器人补满空位") == 0);
    click(WW_KEY_OK);
    view();
    CHECK(V.kind == WWV_LOBBY && strstr(V.hint, "加了3个机器人") != NULL);
    CHECK(ww_seated_count(&G) == 5 && ww_bot_count(&G) == 3);
    key(WW_KEY_OK, WW_GES_LONG);
    click(WW_KEY_OK);
    view();
    CHECK(strcmp(V.items[2], "3号 机器人") == 0 && strcmp(V.items[0], "1号") == 0);
    key(WW_KEY_OK, WW_GES_LONG);
    key(WW_KEY_OK, WW_GES_LONG);
    ww_kick_all(&G);
    // 菜单上下循环
    key(WW_KEY_OK, WW_GES_LONG);
    click(WW_KEY_UP);
    view();
    CHECK(strcmp(V.items[V.sel], "返回") == 0);
    click(WW_KEY_OK);
    view();
    CHECK(V.kind == WWV_LOBBY);
}

static void test_ingame_menu_and_over(void)
{
    setup();
    ww_set_seats(&G, 2);
    fill(2);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    key(WW_KEY_OK, WW_GES_LONG);
    view();
    CHECK(strcmp(V.items[0], "强制下一步") == 0);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_NIGHT);
    view();
    CHECK(V.kind == WWV_GAME);
    // 2 人局:狼刀村民 -> 狼胜
    wait_ms(WW_T_NIGHT + 200);
    int wolf = -1, vill = -1;
    for (int s = 1; s <= 2; s++) {
        int i = ww_by_seat(&G, s);
        if (G.players[i].role == WW_ROLE_WOLF) wolf = i; else vill = i;
    }
    CHECK(ww_act(&G, wolf, WW_ACT_WOLF, G.players[vill].seat, 0, NULL, NOW) == WW_OK);
    wait_ms(WW_T_ROLE_MIN + WW_T_CLOSE + 200);
    while (G.phase == WW_PH_WITCH || G.phase == WW_PH_SEER) wait_ms(500);
    wait_ms(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_OVER);
    view();
    CHECK(strcmp(V.big, "狼人胜利") == 0);
    char want[32];
    snprintf(want, sizeof(want), "狼人：%d号", G.players[wolf].seat);
    CHECK(strcmp(V.line1, want) == 0);                  // 结束后可以公开
    // 结束后菜单里没有"强制下一步"
    key(WW_KEY_OK, WW_GES_LONG);
    view();
    CHECK(strcmp(V.items[0], "结束本局") == 0);
    key(WW_KEY_OK, WW_GES_LONG);
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_LOBBY && ww_seated_count(&G) == 2);
}

static void test_ai_ptt(void)
{
    setup();
    ww_set_seats(&G, 4);
    fill(1);                                             // 1 号真人,其余机器人
    ww_add_bots(&G, NOW);
    // AI 设置:菜单第 3 项
    key(WW_KEY_OK, WW_GES_LONG);
    click(WW_KEY_DOWN);
    click(WW_KEY_DOWN);
    view();
    CHECK(strcmp(V.items[V.sel], "AI 设置") == 0);
    click(WW_KEY_OK);
    view();
    CHECK(strcmp(V.title, "AI 设置") == 0 && strstr(V.info, "千问Key:未配") != NULL);
    CHECK(strcmp(V.items[2], "AI 玩家：关") == 0);
    click(WW_KEY_DOWN);
    click(WW_KEY_OK);                                    // 测试 AI:没 Key
    view();
    CHECK(strstr(V.hint, "先填 Key") != NULL && ww_host_take_request(&H) == WW_REQ_NONE);
    click(WW_KEY_DOWN);
    click(WW_KEY_OK);                                    // 打开 AI 玩家
    CHECK(H.ai_on && ww_host_take_request(&H) == WW_REQ_AI_SAVE);
    view();
    CHECK(strcmp(V.items[2], "AI 玩家：开") == 0);
    click(WW_KEY_UP);
    click(WW_KEY_UP);
    click(WW_KEY_OK);                                    // 填写 Key 页:二维码指向 /ai
    view();
    CHECK(V.kind == WWV_SETUP && strcmp(V.qr, "http://192.168.4.1/ai") == 0);
    H.key_ds = H.key_dk = true;
    click(WW_KEY_DOWN);                                  // 任意键回 AI 设置
    view();
    CHECK(strcmp(V.title, "AI 设置") == 0 && strstr(V.info, "千问Key:已配") != NULL);
    key(WW_KEY_OK, WW_GES_LONG);
    key(WW_KEY_OK, WW_GES_LONG);
    CHECK(H.screen == WW_HS_MAIN);

    // 开局推到讨论(机器人随机行动,真人强推)
    click(WW_KEY_OK);
    click(WW_KEY_OK);
    CHECK(G.phase == WW_PH_DEAL);
    for (int k = 0; k < 2000 && G.phase != WW_PH_DISCUSS && G.phase != WW_PH_OVER; k++) {
        if (ww_is_pending(&G, ww_by_seat(&G, 1))) ww_force(&G, NOW);
        wait_ms(500);
    }
    if (G.phase != WW_PH_DISCUSS || !G.players[ww_by_seat(&G, 1)].alive) return;
    while (ww_current_speaker(&G) != 1 && G.speaker + 1 < G.n_order) ww_speaker_move(&G, 1, NOW);
    if (ww_current_speaker(&G) != 1) return;
    view();
    CHECK(strstr(V.hint, "按住确定说话") != NULL);
    key(WW_KEY_OK, WW_GES_LONG);                         // 按住说话
    CHECK(H.ptt && H.ptt_req == 1 && H.screen == WW_HS_MAIN);
    H.ptt_req = 0;
    ww_set_busy(&G, WW_BUSY_REC, 1);
    view();
    CHECK(strcmp(V.big, "1号 录音中") == 0 && strstr(V.hint, "松手") != NULL);
    key(WW_KEY_OK, WW_GES_RELEASE);
    CHECK(!H.ptt && H.ptt_req == 2);
    ww_set_busy(&G, WW_BUSY_NONE, 0);
    key(WW_KEY_UP, WW_GES_LONG);                         // 讨论时长按上 = 菜单
    CHECK(H.screen == WW_HS_MENU);
    key(WW_KEY_OK, WW_GES_LONG);
    // 轮到机器人时按住确定不录音
    ww_speaker_move(&G, 1, NOW);
    if (ww_is_bot(&G, ww_current_speaker(&G))) {
        H.ptt_req = 0;
        key(WW_KEY_OK, WW_GES_LONG);
        view();
        CHECK(!H.ptt && H.ptt_req == 0 && strstr(V.hint, "轮到 AI") != NULL);
    }
    // AI 关着时也不录音
    H.ai_on = false;
    ww_speaker_move(&G, -G.n_order, NOW);
    key(WW_KEY_OK, WW_GES_LONG);
    CHECK(!H.ptt);
}

static void test_json(void)
{
    setup();
    view();
    char buf[4096];
    size_t n = ww_view_json(&V, buf, sizeof(buf));
    CHECK(n > 0 && buf[0] == '{' && buf[n - 1] == '}');
    CHECK(strstr(buf, "\"kind\":\"lobby\"") != NULL);
    CHECK(ww_view_json(&V, buf, 16) == 0);
}

int main(void)
{
    test_boot_and_lobby();
    test_start_confirm();
    test_game_flow();
    test_menu();
    test_ingame_menu_and_over();
    test_ai_ptt();
    test_json();
    if (g_fail) {
        fprintf(stderr, "ww_host: %d/%d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("ww_host: all %d checks passed\n", g_checks);
    return 0;
}
