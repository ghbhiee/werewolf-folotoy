// tests/test_ww_core.c —— 狼人杀状态机的主机单测 + bot 整局测试。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_ww_core.c main/ww_core.c
//
// 时间全部由测试控制:每次 ww_tick 前手动把 now 往前推。
#include "ww_core.h"

#include <stdio.h>
#include <stdlib.h>
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
static uint32_t NOW;
static int PIDX[WW_MAX_SEATS + 1];      // 座位 -> 玩家下标
static char JSON[8192];

static void run_for(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        NOW += 100;
        for (int s = 1; s <= G.seat_count; s++) if (PIDX[s] >= 0) ww_touch(&G, PIDX[s], NOW);
        ww_tick(&G, NOW);
    }
}

// 建一个 n 人房间并全部入座
static void setup(int n, uint32_t seed)
{
    ww_init(&G, seed);
    NOW = 1000;
    memset(PIDX, -1, sizeof(PIDX));
    CHECK(ww_set_seats(&G, n) == n);
    for (int s = 1; s <= n; s++) {
        char name[16];
        char tok[WW_TOKEN_LEN + 1];
        snprintf(name, sizeof(name), "P%d", s);
        int i = ww_join(&G, name, NOW, tok);
        CHECK(i >= 0);
        CHECK(ww_find(&G, tok) == i);
        CHECK(ww_act(&G, i, WW_ACT_SEAT, s, 0, NULL, NOW) == WW_OK);
        PIDX[s] = i;
    }
}

static int seat_of_role(ww_role_t r, int nth)
{
    for (int s = 1; s <= G.seat_count; s++) {
        const ww_player_t *p = &G.players[PIDX[s]];
        if (p->role == r && nth-- == 0) return s;
    }
    return 0;
}

static bool alive(int seat) { return G.players[PIDX[seat]].alive; }

// 从大厅推进到狼人睁眼
static void to_wolf_phase(void)
{
    CHECK(ww_start(&G, NOW) == WW_OK);
    CHECK(G.phase == WW_PH_DEAL);
    run_for(WW_T_DEAL + WW_T_NIGHT + 200);
    CHECK(G.phase == WW_PH_WOLF);
    CHECK(G.step == 0);
}

// 等当前夜晚角色阶段"闭眼"完,进入下一个阶段
static void finish_role(void)
{
    ww_phase_t ph = G.phase;
    // 下限是随机的(8–15 秒),等到阶段真的切换为止,不多等
    for (uint32_t t = 0; t < WW_T_FAKE_MAX + WW_T_CLOSE + 2000 && G.phase == ph; t += 100) run_for(100);
    CHECK(G.phase != ph);
}

// ---------------------------------------------------------------------------
static void test_board(void)
{
    for (int n = WW_MIN_SEATS; n <= WW_MAX_SEATS; n++) {
        int sum = ww_role_count(n, WW_ROLE_WOLF) + ww_role_count(n, WW_ROLE_SEER) +
                  ww_role_count(n, WW_ROLE_WITCH) + ww_role_count(n, WW_ROLE_VILLAGER);
        CHECK(sum == n);
        CHECK(ww_role_count(n, WW_ROLE_WOLF) >= 1);
    }
    CHECK(ww_role_count(12, WW_ROLE_WOLF) == 4);
    CHECK(ww_role_count(6, WW_ROLE_WOLF) == 2);
    CHECK(ww_role_count(4, WW_ROLE_WITCH) == 1);
    CHECK(ww_role_count(3, WW_ROLE_WITCH) == 0);
}

static void test_lobby(void)
{
    ww_init(&G, 7);
    NOW = 0;
    char tok[WW_TOKEN_LEN + 1];
    CHECK(ww_join(&G, "   ", NOW, tok) == WW_E_NAME);
    int a = ww_join(&G, "  小明  ", NOW, tok);
    CHECK(a >= 0);
    CHECK(strcmp(G.players[a].name, "小明") == 0);
    CHECK(strlen(tok) == WW_TOKEN_LEN);
    int b = ww_join(&G, "Bob", NOW, NULL);
    CHECK(ww_act(&G, a, WW_ACT_SEAT, 5, 0, NULL, NOW) == WW_OK);
    CHECK(ww_act(&G, b, WW_ACT_SEAT, 5, 0, NULL, NOW) == WW_E_TAKEN);
    CHECK(ww_act(&G, b, WW_ACT_SEAT, 9, 0, NULL, NOW) == WW_E_ARG);    // 默认 8 座
    CHECK(ww_act(&G, b, WW_ACT_SEAT, 2, 0, NULL, NOW) == WW_OK);
    // 座位数下限 = 已入座人数(不小于 2);减座位时高号的人挪到空着的低号
    CHECK(ww_min_seats(&G) == 2);
    CHECK(ww_set_seats(&G, 99) == WW_MAX_SEATS);
    CHECK(ww_start(&G, NOW) == WW_E_SEATS);                             // 没坐满
    CHECK(ww_act(&G, a, WW_ACT_SEAT, 1, 0, NULL, NOW) == WW_OK);         // 换座
    CHECK(ww_set_seats(&G, 1) == 2);
    CHECK(G.players[a].seat == 1 && G.players[b].seat == 2);
    CHECK(ww_start(&G, NOW) == WW_OK);
    CHECK(ww_act(&G, b, WW_ACT_SEAT, 1, 0, NULL, NOW) == WW_E_PHASE);    // 开局后不能换座
    CHECK(ww_kick(&G, 1) == WW_E_PHASE);
    ww_reset(&G);
    CHECK(G.phase == WW_PH_LOBBY);
    CHECK(ww_seated_count(&G) == 2);                                     // 重开保留座位
    CHECK(ww_kick(&G, 1) == WW_OK);
    CHECK(ww_seated_count(&G) == 1);
    CHECK(ww_find(&G, tok) == -1);                                       // 被踢的 token 失效
    ww_kick_all(&G);
    CHECK(ww_seated_count(&G) == 0);

    // 满员时挤掉最久没露面的围观者,入座的人不挤
    ww_init(&G, 9);
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "x%d", i);
        int k = ww_join(&G, nm, (uint32_t)i * 10, NULL);
        CHECK(k == i);
        if (i < 12) {
            ww_set_seats(&G, 12);
            CHECK(ww_act(&G, k, WW_ACT_SEAT, i + 1, 0, NULL, (uint32_t)i * 10) == WW_OK);
        }
    }
    int k = ww_join(&G, "late", 1000, NULL);
    CHECK(k == 12);                         // 12 号下标是最早的未入座者
    CHECK(ww_seated_count(&G) == 12);

    // 名字按 UTF-8 边界截断
    ww_init(&G, 3);
    int c = ww_join(&G, "一二三四五六七八九十一二三四五", 0, NULL);
    CHECK(c >= 0);
    CHECK(strlen(G.players[c].name) <= WW_NAME_MAX);
    CHECK(strlen(G.players[c].name) % 3 == 0);
}

// 机器人:全机器人局不用任何人按键也能打完;真人 + 机器人混合局里机器人会行动
static void test_bots(void)
{
    for (int n = WW_MIN_SEATS; n <= WW_MAX_SEATS; n++) {
        for (uint32_t seed = 1; seed <= 5; seed++) {
            ww_init(&G, seed * 131u + (uint32_t)n);
            NOW = 1000;
            memset(PIDX, -1, sizeof(PIDX));
            ww_set_seats(&G, n);
            CHECK(ww_add_bots(&G, NOW) == n);
            CHECK(ww_add_bots(&G, NOW) == 0);
            CHECK(ww_bot_count(&G) == n);
            NOW += 60000;
            CHECK(ww_is_online(&G, ww_by_seat(&G, 1), NOW));            // 机器人永远在线
            CHECK(ww_start(&G, NOW) == WW_OK);
            for (int k = 0; k < 4000 && G.phase != WW_PH_OVER; k++) { NOW += 250; ww_tick(&G, NOW); }
            CHECK(G.phase == WW_PH_OVER);
        }
    }
    // 1 个真人 + 机器人:真人不动时夜里靠机器人/超时推进,白天主持人推进
    ww_init(&G, 77);
    NOW = 1000;
    ww_set_seats(&G, 6);
    int me = ww_join(&G, "我", NOW, NULL);
    CHECK(ww_act(&G, me, WW_ACT_SEAT, 4, 0, NULL, NOW) == WW_OK);
    CHECK(ww_add_bots(&G, NOW) == 5);
    CHECK(ww_start(&G, NOW) == WW_OK);
    int guard = 0;
    while (G.phase != WW_PH_OVER && guard++ < 20000) {
        NOW += 250;
        G.players[me].last_seen = NOW;
        if (ww_is_pending(&G, me) && G.phase == WW_PH_VOTE) ww_act(&G, me, WW_ACT_VOTE, 0, 0, NULL, NOW);
        if (ww_is_pending(&G, me) && G.phase == WW_PH_WITCH) ww_act(&G, me, WW_ACT_WITCH, 0, 0, NULL, NOW);
        if (G.phase == WW_PH_DISCUSS && ww_elapsed_ms(&G, NOW) > 3000) ww_force(&G, NOW);
        ww_tick(&G, NOW);
    }
    CHECK(G.phase == WW_PH_OVER);
    ww_state_json(&G, me, NOW, JSON, sizeof(JSON));
    CHECK(strstr(JSON, "\"bot\":1") != NULL);
}

// 有 AI 大脑:机器人等大脑(超时才随机),AI 发完言自动轮到下一位,全员发完才自动投票
static void test_brain(void)
{
    ww_init(&G, 99);
    NOW = 1000;
    ww_set_seats(&G, 8);                                 // 3 狼 5 好人:第一夜结束不了
    ww_add_bots(&G, NOW);
    G.brain = true;
    CHECK(ww_start(&G, NOW) == WW_OK);
    run_for(WW_T_DEAL + WW_T_NIGHT + 200);
    CHECK(G.phase == WW_PH_WOLF);
    run_for(WW_T_BRAIN - 1000);                          // 大脑没动静:还在等
    CHECK(G.phase == WW_PH_WOLF && G.step == 0);
    run_for(1500);                                       // 超时随机兜底
    CHECK(G.phase == WW_PH_WOLF && G.step == 1);
    // 女巫/预言家也兜底;走到天亮(可能有人死)
    for (int k = 0; k < 400 && G.phase != WW_PH_DISCUSS && G.phase != WW_PH_OVER; k++) run_for(250);
    CHECK(G.phase == WW_PH_DISCUSS);
    int first = ww_current_speaker(&G);
    CHECK(first == G.order[0] && !ww_has_spoken(&G, first));
    run_for(20000);
    CHECK(G.phase == WW_PH_DISCUSS);                     // 没发言不自动投票
    for (int k = 0; k < G.n_order; k++) {
        int seat = ww_current_speaker(&G);
        CHECK(seat == G.order[k]);
        ww_set_busy(&G, WW_BUSY_SPEAK, seat);
        ww_add_speech(&G, seat, "我是好人，过。", true, NOW);
        CHECK(ww_has_spoken(&G, seat));
    }
    ww_set_busy(&G, WW_BUSY_NONE, 0);
    CHECK(ww_current_speaker(&G) == G.order[G.n_order - 1]);   // 最后一位不再往后
    run_for(500);
    CHECK(G.phase == WW_PH_VOTE);
    ww_state_json(&G, -1, NOW, JSON, sizeof(JSON));
    CHECK(strstr(JSON, "\"speech\":[") != NULL && strstr(JSON, "我是好人") != NULL);
    // 发言记录环形:超过 WW_SPEECH_MAX 条只留最新的;UTF-8 截断;控制字符变空格
    for (int k = 0; k < WW_SPEECH_MAX + 3; k++) {
        char tx[32];
        snprintf(tx, sizeof(tx), "第%d条\n", k);
        ww_add_speech(&G, 1, tx, false, NOW);
    }
    CHECK(strstr(ww_speech_at(&G, 0)->text, "第14条 ") != NULL);
    CHECK(ww_speech_at(&G, WW_SPEECH_MAX) == NULL);
    char big[1200];
    memset(big, 0, sizeof(big));
    for (int k = 0; k + 3 < (int)sizeof(big) - 1; k += 3) memcpy(big + k, "说", 3);
    ww_add_speech(&G, 2, big, false, NOW);
    CHECK(strlen(ww_speech_at(&G, 0)->text) % 3 == 0 && strlen(ww_speech_at(&G, 0)->text) < WW_SPEECH_LEN);
}

static void test_deal(void)
{
    for (int n = WW_MIN_SEATS; n <= WW_MAX_SEATS; n++) {
        setup(n, 100u + (uint32_t)n);
        CHECK(ww_start(&G, NOW) == WW_OK);
        int cnt[5] = {0};
        for (int s = 1; s <= n; s++) cnt[G.players[PIDX[s]].role]++;
        CHECK(cnt[WW_ROLE_WOLF] == ww_role_count(n, WW_ROLE_WOLF));
        CHECK(cnt[WW_ROLE_SEER] == ww_role_count(n, WW_ROLE_SEER));
        CHECK(cnt[WW_ROLE_WITCH] == ww_role_count(n, WW_ROLE_WITCH));
        CHECK(cnt[WW_ROLE_VILLAGER] == ww_role_count(n, WW_ROLE_VILLAGER));
        CHECK(ww_pop_cue(&G) == WW_CUE_DEAL);
    }
}

// 狼刀 -> 女巫救 -> 平安夜;夜晚顺序 狼 -> 女巫 -> 预言家
static void test_night_save(void)
{
    setup(6, 42);
    to_wolf_phase();
    int w0 = seat_of_role(WW_ROLE_WOLF, 0), w1 = seat_of_role(WW_ROLE_WOLF, 1);
    int witch = seat_of_role(WW_ROLE_WITCH, 0), seer = seat_of_role(WW_ROLE_SEER, 0);
    int vill = seat_of_role(WW_ROLE_VILLAGER, 0);

    CHECK(ww_is_pending(&G, PIDX[w0]));
    CHECK(!ww_is_pending(&G, PIDX[vill]));
    CHECK(ww_act(&G, PIDX[vill], WW_ACT_WOLF, witch, 0, NULL, NOW) == WW_E_ROLE);
    CHECK(ww_act(&G, PIDX[w0], WW_ACT_WOLF, vill, 0, NULL, NOW) == WW_OK);
    CHECK(ww_act(&G, PIDX[w0], WW_ACT_WOLF, witch, 0, NULL, NOW) == WW_OK);   // 可以改票
    CHECK(ww_act(&G, PIDX[w1], WW_ACT_WOLF, witch, 0, NULL, NOW) == WW_OK);
    // 行动完也要等满"睁眼"最短时长(随机 8–15 秒,不能一行动完就闭眼)
    CHECK(G.phase == WW_PH_WOLF && G.step == 0);
    uint32_t opened = G.phase_start;
    while (G.step == 0 && NOW - opened < WW_T_ROLE_MIN + 1000) run_for(100);
    CHECK(G.phase == WW_PH_WOLF && G.step == 1);
    CHECK(NOW - opened >= WW_T_ROLE_MIN_LO && NOW - opened <= WW_T_ROLE_MIN + 200);
    CHECK(G.kill_seat == witch);
    run_for(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_WITCH);                 // 女巫先于预言家

    // 女巫看到刀口,自救
    ww_state_json(&G, PIDX[witch], NOW, JSON, sizeof(JSON));
    char want[64];
    snprintf(want, sizeof(want), "\"kill\":%d", witch);
    CHECK(strstr(JSON, want) != NULL);
    // 其他人看不到倒计时(防止从假等待推断女巫死活)
    ww_state_json(&G, PIDX[vill], NOW, JSON, sizeof(JSON));
    CHECK(strstr(JSON, "\"left\":-1") != NULL);
    CHECK(strstr(JSON, "\"witch\":{") == NULL);
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 1, 0, NULL, NOW) == WW_OK);
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 2, vill, NULL, NOW) == WW_E_DONE);  // 一晚一件事
    finish_role();
    CHECK(G.phase == WW_PH_SEER);
    CHECK(!G.antidote && G.poison);

    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, seer, 0, NULL, NOW) == WW_E_ARG);    // 不能查自己
    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, w1, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_DAWN);
    CHECK(G.n_dead == 0);
    CHECK(strstr(ww_log_at(&G, 0), "平安夜") != NULL);
    ww_state_json(&G, PIDX[seer], NOW, JSON, sizeof(JSON));
    CHECK(strstr(JSON, "是狼人") != NULL);
    // 第 1 天平安夜:从最大号的下一位(1 号)开始,小号到大号
    run_for(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_DISCUSS);
    CHECK(G.n_order == 6);
    CHECK(G.order[0] == 1 && G.order[5] == 6);
}

// 毒药 + 发言顺序(有死者从死者下一位开始;第 2 天大号到小号)
static void test_poison_and_order(void)
{
    setup(10, 5);        // 3 狼 7 好人,死两个也不会结束
    to_wolf_phase();
    int witch = seat_of_role(WW_ROLE_WITCH, 0);
    int seer = seat_of_role(WW_ROLE_SEER, 0);
    int v0 = seat_of_role(WW_ROLE_VILLAGER, 0), v1 = seat_of_role(WW_ROLE_VILLAGER, 1);
    for (int k = 0; k < 3; k++) {
        CHECK(ww_act(&G, PIDX[seat_of_role(WW_ROLE_WOLF, k)], WW_ACT_WOLF, v0, 0, NULL, NOW) == WW_OK);
    }
    finish_role();
    CHECK(G.phase == WW_PH_WITCH);
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 2, v1, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, v0, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_DAWN);
    CHECK(G.n_dead == 2);
    CHECK(!alive(v0) && !alive(v1));
    int lo = v0 < v1 ? v0 : v1;
    run_for(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_DISCUSS);
    // 从编号较小的死者的下一位存活者开始,递增
    int expect = lo;
    do { expect = expect % 10 + 1; } while (!alive(expect));
    CHECK(G.order[0] == expect);
    CHECK(G.n_order == 8);
    for (int k = 1; k < G.n_order; k++) {
        int prev = G.order[k - 1], cur = G.order[k];
        int step = prev;
        do { step = step % 10 + 1; } while (!alive(step));
        CHECK(cur == step);
    }
    // 发言人指针
    ww_speaker_move(&G, -1, NOW);
    CHECK(G.speaker == 0);
    ww_speaker_move(&G, 1, NOW);
    CHECK(G.speaker == 1);
    for (int k = 0; k < 20; k++) ww_speaker_move(&G, 1, NOW);
    CHECK(G.speaker == G.n_order - 1);

    // 玩家发起投票 -> 全弃票 -> 无人出局
    int first = G.order[0];
    CHECK(ww_act(&G, PIDX[v0], WW_ACT_START_VOTE, 0, 0, NULL, NOW) == WW_E_DEAD);
    CHECK(ww_act(&G, PIDX[first], WW_ACT_START_VOTE, 0, 0, NULL, NOW) == WW_OK);
    CHECK(G.phase == WW_PH_VOTE);
    for (int s = 1; s <= 10; s++) {
        if (!alive(s)) {
            CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, 0, 0, NULL, NOW) == WW_E_DEAD);
            continue;
        }
        CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, s, 0, NULL, NOW) == WW_E_ARG);     // 不能投自己
        CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, 0, 0, NULL, NOW) == WW_OK);
        CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, 0, 0, NULL, NOW) == WW_E_DONE);    // 不能改票
    }
    run_for(WW_T_VOTE_MIN);
    CHECK(G.phase == WW_PH_RESULT);
    CHECK(G.out_seat == 0);
    CHECK(strstr(ww_log_at(&G, 0), "所有人弃票") != NULL);
    run_for(WW_T_RESULT + 200);
    CHECK(G.phase == WW_PH_NIGHT);
    CHECK(G.round == 2);

    // 第 2 夜:空刀、女巫没解药了只剩一瓶毒(不用)、预言家查人 -> 平安夜
    run_for(WW_T_NIGHT + 200);
    CHECK(G.phase == WW_PH_WOLF);
    for (int k = 0; k < 3; k++) {
        CHECK(ww_act(&G, PIDX[seat_of_role(WW_ROLE_WOLF, k)], WW_ACT_WOLF, 0, 0, NULL, NOW) == WW_OK);
    }
    finish_role();
    CHECK(G.kill_seat == 0);
    // 女巫毒药已用、解药还在 -> 真等待
    CHECK(G.phase == WW_PH_WITCH && !G.fake);
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 1, 0, NULL, NOW) == WW_E_ARG);   // 空刀没人可救
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 0, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, seat_of_role(WW_ROLE_WOLF, 0), 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_DAWN && G.n_dead == 0);
    int prev_start = G.speech_start;
    run_for(WW_T_DAWN + 200);
    // 第 2 天平安夜:从上轮首发言者的下一位开始,大号到小号
    int expect2 = prev_start;
    do { expect2 = (expect2 + 8) % 10 + 1; } while (!alive(expect2));
    CHECK(G.order[0] == expect2);
    for (int k = 1; k < G.n_order; k++) {
        int step = G.order[k - 1];
        do { step = (step + 8) % 10 + 1; } while (!alive(step));
        CHECK(G.order[k] == step);
    }
}

// 超时兜底、强推、假等待、平票、胜负
static void test_timeout_force_tie_win(void)
{
    setup(4, 11);           // 狼 1 预言家 1 女巫 1 村民 1
    to_wolf_phase();
    int wolf = seat_of_role(WW_ROLE_WOLF, 0);
    int seer = seat_of_role(WW_ROLE_SEER, 0);
    int witch = seat_of_role(WW_ROLE_WITCH, 0);
    int vill = seat_of_role(WW_ROLE_VILLAGER, 0);
    // 狼不操作 -> 60 秒超时空刀
    run_for(WW_T_ROLE_TIMEOUT - 1000);
    CHECK(G.phase == WW_PH_WOLF && G.step == 0);
    run_for(1200);
    CHECK(G.phase == WW_PH_WOLF && G.step == 1);
    CHECK(G.kill_seat == 0);
    run_for(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_WITCH);
    // 主持人强推女巫:没选就是不用药
    CHECK(ww_force(&G, NOW));
    CHECK(G.step == 1);
    CHECK(G.antidote && G.poison);
    CHECK(ww_force(&G, NOW));           // 闭眼也可以跳过
    CHECK(G.phase == WW_PH_SEER);
    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, vill, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_DAWN);
    CHECK(ww_force(&G, NOW));
    CHECK(G.phase == WW_PH_DISCUSS);
    CHECK(ww_force(&G, NOW));
    CHECK(G.phase == WW_PH_VOTE);
    // 平票:狼投村民、村民投狼,另两人弃票
    CHECK(ww_act(&G, PIDX[wolf], WW_ACT_VOTE, vill, 0, NULL, NOW) == WW_OK);
    CHECK(ww_act(&G, PIDX[vill], WW_ACT_VOTE, wolf, 0, NULL, NOW) == WW_OK);
    CHECK(ww_force(&G, NOW));           // 强制结束,未投=弃票
    CHECK(G.phase == WW_PH_RESULT);
    CHECK(G.out_seat == 0);
    CHECK(strstr(ww_log_at(&G, 0), "平票") != NULL);
    CHECK(strstr(ww_log_at(&G, 0), "弃票 2人") != NULL);
    run_for(WW_T_RESULT + WW_T_NIGHT + 400);
    CHECK(G.phase == WW_PH_WOLF && G.round == 2);

    // 第 2 夜:刀女巫;女巫自救
    CHECK(ww_act(&G, PIDX[wolf], WW_ACT_WOLF, witch, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 1, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_SEER);
    ww_force(&G, NOW);      // 预言家不查,主持人强推 + 跳过闭眼
    ww_force(&G, NOW);
    CHECK(G.phase == WW_PH_DAWN && G.n_dead == 0);
    run_for(WW_T_DAWN + 200);
    CHECK(ww_force(&G, NOW));
    // 全票出狼 -> 好人胜
    for (int s = 1; s <= 4; s++) {
        if (s == wolf) CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, vill, 0, NULL, NOW) == WW_OK);
        else CHECK(ww_act(&G, PIDX[s], WW_ACT_VOTE, wolf, 0, NULL, NOW) == WW_OK);
    }
    run_for(WW_T_VOTE_MIN);
    CHECK(G.phase == WW_PH_RESULT && G.out_seat == wolf);
    run_for(WW_T_RESULT + 200);
    CHECK(G.phase == WW_PH_OVER && G.winner == WW_WIN_GOOD);
    ww_state_json(&G, PIDX[vill], NOW, JSON, sizeof(JSON));
    CHECK(strstr(JSON, "\"winner\":\"good\"") != NULL);
    CHECK(strstr(JSON, "\"role\":\"wolf\"") != NULL);         // 结束后全亮身份
    CHECK(!ww_force(&G, NOW));
}

// 假等待:女巫死了以后女巫阶段仍然存在,5–9 秒
static void test_fake_wait(void)
{
    setup(4, 21);
    to_wolf_phase();
    int wolf = seat_of_role(WW_ROLE_WOLF, 0);
    int witch = seat_of_role(WW_ROLE_WITCH, 0);
    int seer = seat_of_role(WW_ROLE_SEER, 0);
    CHECK(ww_act(&G, PIDX[wolf], WW_ACT_WOLF, witch, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 0, 0, NULL, NOW) == WW_OK);   // 不救自己
    finish_role();
    CHECK(ww_act(&G, PIDX[seer], WW_ACT_SEER, wolf, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_DAWN && !alive(witch));
    // 1 狼 vs 2 好人:还没结束
    run_for(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_DISCUSS);
    ww_force(&G, NOW);
    ww_force(&G, NOW);                  // 全弃票
    run_for(WW_T_RESULT + WW_T_NIGHT + 400);
    CHECK(G.phase == WW_PH_WOLF);
    CHECK(ww_act(&G, PIDX[wolf], WW_ACT_WOLF, seer, 0, NULL, NOW) == WW_OK);
    finish_role();
    CHECK(G.phase == WW_PH_WITCH && G.fake);
    CHECK(ww_act(&G, PIDX[witch], WW_ACT_WITCH, 0, 0, NULL, NOW) == WW_E_PHASE);
    uint32_t t0 = NOW;
    while (G.phase == WW_PH_WITCH && G.step == 0) run_for(100);
    uint32_t took = NOW - t0;
    CHECK(took >= WW_T_FAKE_MIN - 200 && took <= WW_T_FAKE_MAX + 200);
    run_for(WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_SEER && !G.fake);
    ww_force(&G, NOW);
    ww_force(&G, NOW);
    CHECK(G.phase == WW_PH_DAWN && !alive(seer));
    // 1 狼 vs 1 村民 -> 狼胜
    run_for(WW_T_DAWN + 200);
    CHECK(G.phase == WW_PH_OVER && G.winner == WW_WIN_WOLF);
    CHECK(strstr(ww_log_at(&G, 1), "死亡") != NULL);    // 先公布死讯再宣布结果
}

// JSON 不泄露身份
static void test_secrecy(void)
{
    setup(8, 77);
    CHECK(ww_start(&G, NOW) == WW_OK);
    int w0 = seat_of_role(WW_ROLE_WOLF, 0), vill = seat_of_role(WW_ROLE_VILLAGER, 0);

    CHECK(ww_state_json(&G, -1, NOW, JSON, sizeof(JSON)) > 0);
    CHECK(strstr(JSON, "\"role\"") == NULL);
    CHECK(strstr(JSON, "\"mate\"") == NULL);
    CHECK(strstr(JSON, "\"you\"") == NULL);

    CHECK(ww_state_json(&G, PIDX[vill], NOW, JSON, sizeof(JSON)) > 0);
    CHECK(strstr(JSON, "\"role\":\"villager\"") != NULL);     // 自己的
    CHECK(strstr(JSON, "\"role\":\"wolf\"") == NULL);
    CHECK(strstr(JSON, "\"mate\"") == NULL);

    CHECK(ww_state_json(&G, PIDX[w0], NOW, JSON, sizeof(JSON)) > 0);
    int mates = 0;
    for (const char *q = JSON; (q = strstr(q, "\"mate\":1")) != NULL; q++) mates++;
    CHECK(mates == 2);                                       // 8 人局 3 狼,看到另外两个

    // 死人看上帝视角
    G.players[PIDX[vill]].alive = false;
    CHECK(ww_state_json(&G, PIDX[vill], NOW, JSON, sizeof(JSON)) > 0);
    CHECK(strstr(JSON, "\"role\":\"wolf\"") != NULL);
    G.players[PIDX[vill]].alive = true;

    // 名字里的引号/反斜杠要转义
    ww_act(&G, PIDX[1], WW_ACT_RENAME, 0, 0, "a\"b\\c", NOW);
    CHECK(ww_state_json(&G, -1, NOW, JSON, sizeof(JSON)) > 0);
    CHECK(strstr(JSON, "a\\\"b\\\\c") != NULL);

    // 缓冲太小返回 0,不越界
    char tiny[32];
    CHECK(ww_state_json(&G, -1, NOW, tiny, sizeof(tiny)) == 0);
}

// 版本号:变化才 +1,在线状态变化也算
static void test_version(void)
{
    setup(3, 1);
    uint32_t v = G.version;
    ww_tick(&G, NOW);
    CHECK(G.version == v || G.version == v + 1);
    v = G.version;
    ww_tick(&G, NOW);
    CHECK(G.version == v);
    NOW += WW_T_ONLINE + 1000;       // 所有人掉线
    ww_tick(&G, NOW);
    CHECK(G.version == v + 1);
    CHECK(!ww_is_online(&G, PIDX[1], NOW));
    ww_touch(&G, PIDX[1], NOW);
    ww_tick(&G, NOW);
    CHECK(G.version == v + 2);
}

// ---------------------------------------------------------------------------
// bot 整局:所有人数、很多种子,随机但合法地乱玩,检查不变量并且一定能结束
// ---------------------------------------------------------------------------
static int pick_alive(uint32_t *r, int except, bool allow_zero)
{
    int c[WW_MAX_SEATS + 1], n = 0;
    if (allow_zero) c[n++] = 0;
    for (int s = 1; s <= G.seat_count; s++) if (alive(s) && s != except) c[n++] = s;
    if (!n) return 0;
    *r = *r * 1103515245u + 12345u;
    return c[(*r >> 8) % (uint32_t)n];
}

static int bot_game(int n, uint32_t seed, int *rounds_out)
{
    setup(n, seed);
    uint32_t r = seed * 2654435761u;
    CHECK(ww_start(&G, NOW) == WW_OK);
    int guard = 0;
    int alive_prev = n;
    while (G.phase != WW_PH_OVER && guard++ < 20000) {
        // 每个待操作的人有一定概率这一拍就操作(模拟人慢慢点)
        for (int s = 1; s <= n; s++) {
            int i = PIDX[s];
            if (!ww_is_pending(&G, i)) continue;
            r = r * 1103515245u + 12345u;
            if ((r >> 16) % 4) continue;
            int rc = WW_OK;
            switch (G.phase) {
            case WW_PH_WOLF:
                rc = ww_act(&G, i, WW_ACT_WOLF, pick_alive(&r, 0, true), 0, NULL, NOW);
                break;
            case WW_PH_WITCH: {
                int a = (int)((r >> 20) % 3);
                if (a == 1 && !(G.antidote && G.kill_seat)) a = 0;
                if (a == 2 && !G.poison) a = 0;
                rc = ww_act(&G, i, WW_ACT_WITCH, a, a == 2 ? pick_alive(&r, s, false) : 0, NULL, NOW);
                break;
            }
            case WW_PH_SEER:
                rc = ww_act(&G, i, WW_ACT_SEER, pick_alive(&r, s, false), 0, NULL, NOW);
                break;
            case WW_PH_VOTE:
                rc = ww_act(&G, i, WW_ACT_VOTE, pick_alive(&r, s, true), 0, NULL, NOW);
                break;
            default:
                break;
            }
            CHECK(rc == WW_OK);
        }
        if (G.phase == WW_PH_DISCUSS) {
            r = r * 1103515245u + 12345u;
            if ((r >> 16) % 10 == 0) ww_speaker_move(&G, 1, NOW);
            if (ww_elapsed_ms(&G, NOW) > 3000) {
                // 一半靠主持人,一半靠玩家发起投票
                if ((r >> 24) & 1) ww_force(&G, NOW);
                else CHECK(ww_act(&G, PIDX[G.order[0]], WW_ACT_START_VOTE, 0, 0, NULL, NOW) == WW_OK);
            }
        }
        run_for(500);
        // 不变量:存活人数只减不增;狼人数合法;JSON 总能生成
        int a = ww_alive_count(&G);
        CHECK(a <= alive_prev);
        alive_prev = a;
        CHECK(ww_state_json(&G, PIDX[1 + (int)(r % (uint32_t)n)], NOW, JSON, sizeof(JSON)) > 0);
    }
    CHECK(G.phase == WW_PH_OVER);
    int wolves = 0;
    for (int s = 1; s <= n; s++) if (alive(s) && G.players[PIDX[s]].role == WW_ROLE_WOLF) wolves++;
    if (G.winner == WW_WIN_GOOD) CHECK(wolves == 0);
    else CHECK(G.winner == WW_WIN_WOLF && wolves >= ww_alive_count(&G) - wolves);
    int winner = G.winner;
    if (rounds_out) *rounds_out = G.round;
    // 结束后回大厅,座位保留
    ww_reset(&G);
    CHECK(G.phase == WW_PH_LOBBY && ww_seated_count(&G) == n);
    return winner;
}

static void test_bot_games(void)
{
    int games = 0, good = 0, wolf = 0;
    for (int n = WW_MIN_SEATS; n <= WW_MAX_SEATS; n++) {
        for (uint32_t seed = 1; seed <= 40; seed++) {
            int w = bot_game(n, seed * 7919u + (uint32_t)n, NULL);
            games++;
            if (w == WW_WIN_GOOD) good++;
            if (w == WW_WIN_WOLF) wolf++;
        }
    }
    printf("bot 整局:%d 局(2–12 人各 40 局),好人胜 %d,狼人胜 %d\n", games, good, wolf);
    CHECK(good + wolf == games);
    CHECK(good > 0 && wolf > 0);
}

int main(void)
{
    test_board();
    test_lobby();
    test_deal();
    test_bots();
    test_brain();
    test_night_save();
    test_poison_and_order();
    test_timeout_force_tie_win();
    test_fake_wait();
    test_secrecy();
    test_version();
    if (!getenv("SKIP_BOT")) test_bot_games();
    if (g_fail) {
        fprintf(stderr, "ww_core: %d/%d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("ww_core: all %d checks passed\n", g_checks);
    return 0;
}
