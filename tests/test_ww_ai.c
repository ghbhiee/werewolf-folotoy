// tests/test_ww_ai.c —— AI 玩家提示词 / 决策落地的主机单测。
//
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_ww_ai.c main/ww_ai.c main/ww_core.c
#include "ww_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static ww_game_t G;
static uint32_t NOW;
static char SYS[6144], USR[768], OUT[512];

static void test_json(void)
{
    int v = -1;
    char s[64];
    CHECK(ww_ai_json_int("{\"target\": 3, \"reason\": \"x\"}", "target", &v) && v == 3);
    CHECK(ww_ai_json_int("{\"target\":\"5\"}", "target", &v) && v == 5);
    CHECK(ww_ai_json_int("{ \"target\" :\n 0 }", "target", &v) && v == 0);
    CHECK(!ww_ai_json_int("{\"reason\":\"3\"}", "target", &v));
    CHECK(!ww_ai_json_int(NULL, "target", &v));
    CHECK(!ww_ai_json_int("{\"target\": null}", "target", &v));
    CHECK(ww_ai_json_str("{\"action\":\"save\",\"target\":2}", "action", s, sizeof(s)) && strcmp(s, "save") == 0);
    CHECK(ww_ai_json_str("{\"speech\":\"他说\\\"不是我\\\"\\n好\"}", "speech", s, sizeof(s)) &&
          strcmp(s, "他说\"不是我\" 好") == 0);
    CHECK(ww_ai_json_str("{\"speech\":\"\\u4e2d\\u6587A\"}", "speech", s, sizeof(s)) && strcmp(s, "中文A") == 0);
    // 截断在 UTF-8 边界
    char tiny[8];
    CHECK(ww_ai_json_str("{\"speech\":\"一二三四\"}", "speech", tiny, sizeof(tiny)) && strcmp(tiny, "一二") == 0);
    CHECK(!ww_ai_json_str("{\"speech\": 3}", "speech", s, sizeof(s)));
}

static int P[WW_MAX_SEATS + 1];

static void setup(int n, int humans, uint32_t seed)
{
    ww_init(&G, seed);
    NOW = 1000;
    ww_set_seats(&G, n);
    for (int s = 1; s <= humans; s++) {
        char nm[24];                                     // GCC 会按 int 最长 10 位算,给够
        snprintf(nm, sizeof(nm), "人%d", s);
        int i = ww_join(&G, nm, NOW, NULL);
        ww_act(&G, i, WW_ACT_SEAT, s, 0, NULL, NOW);
    }
    ww_add_bots(&G, NOW);
    for (int s = 1; s <= n; s++) P[s] = ww_by_seat(&G, s);
    G.brain = true;
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        NOW += 100;
        for (int s = 1; s <= G.seat_count; s++) ww_touch(&G, P[s], NOW);
        ww_tick(&G, NOW);
    }
}

static int seat_of(ww_role_t r, bool bot)
{
    for (int s = 1; s <= G.seat_count; s++) {
        if (G.players[P[s]].role == r && G.players[P[s]].bot == bot) return s;
    }
    return 0;
}

static void test_prompt_privacy(void)
{
    // 找一个种子让 1 号真人不是狼,且有 AI 狼、AI 村民
    for (uint32_t seed = 1; seed < 200; seed++) {
        setup(8, 1, seed);
        CHECK(ww_start(&G, NOW) == WW_OK);
        if (seat_of(WW_ROLE_WOLF, true) && seat_of(WW_ROLE_VILLAGER, true)) break;
    }
    run(WW_T_DEAL + WW_T_NIGHT + 200);
    CHECK(G.phase == WW_PH_WOLF);
    ww_ai_job_t job;
    CHECK(ww_ai_next_job(&G, &job) && job.task == WW_AI_WOLF && ww_is_bot(&G, job.seat));
    CHECK(ww_ai_prompt(&G, &job, SYS, sizeof(SYS), USR, sizeof(USR)));
    CHECK(strstr(SYS, "身份【狼人】") != NULL);
    CHECK(strstr(SYS, "你的狼队友：") != NULL);          // 8 人局 3 狼
    CHECK(strstr(USR, "空刀") != NULL);
    // 候选里不能有狼队友
    for (int s = 1; s <= 8; s++) {
        if (G.players[P[s]].role != WW_ROLE_WOLF) continue;
        char needle[8];
        snprintf(needle, sizeof(needle), " %d,", s);
        CHECK(strstr(USR, needle) == NULL);
    }
    // 村民的提示词:只有自己的身份,没有队友、没有别人的身份
    ww_ai_job_t vj = job;
    vj.seat = seat_of(WW_ROLE_VILLAGER, true);
    vj.task = WW_AI_VOTE;
    CHECK(ww_ai_prompt(&G, &vj, SYS, sizeof(SYS), USR, sizeof(USR)));
    CHECK(strstr(SYS, "身份【村民】") != NULL);
    CHECK(strstr(SYS, "狼队友") == NULL);
    CHECK(strstr(SYS, "身份【狼人】") == NULL && strstr(SYS, "身份【预言家】") == NULL &&
          strstr(SYS, "身份【女巫】") == NULL);
    // 缓冲不够时报失败而不是写出半截
    CHECK(!ww_ai_prompt(&G, &job, SYS, 200, USR, sizeof(USR)));
}

static void test_apply_and_fallback(void)
{
    setup(6, 0, 5);
    CHECK(ww_start(&G, NOW) == WW_OK);
    run(WW_T_DEAL + WW_T_NIGHT + 200);
    ww_ai_job_t job;
    int wolves = 0;
    // 狼:第一只给合法目标,第二只给乱码(兜底随机),都要落地
    while (ww_ai_next_job(&G, &job) && job.task == WW_AI_WOLF) {
        const char *reply = wolves++ == 0 ? "{\"target\": 1}" : "我不知道";
        if (G.players[P[1]].role == WW_ROLE_WOLF) reply = "{\"target\": 99}";
        CHECK(ww_ai_apply(&G, &job, reply, NOW, NULL, 0));
        CHECK(!ww_ai_job_valid(&G, &job));                   // 做过就不再是待办
    }
    CHECK(wolves == 2);
    run(WW_T_ROLE_MIN + WW_T_CLOSE + 200);
    CHECK(G.phase == WW_PH_WITCH);
    CHECK(ww_ai_next_job(&G, &job) && job.task == WW_AI_WITCH);
    CHECK(ww_ai_prompt(&G, &job, SYS, sizeof(SYS), USR, sizeof(USR)));
    CHECK(strstr(USR, "被狼人袭击") != NULL || strstr(USR, "没有人被袭击") != NULL);
    CHECK(ww_ai_apply(&G, &job, "{\"action\":\"save\",\"target\":0}", NOW, NULL, 0));
    CHECK(G.witch_act == 1 || G.kill_seat == 0);
    run(WW_T_ROLE_MIN + WW_T_CLOSE + 200);
    CHECK(ww_ai_next_job(&G, &job) && job.task == WW_AI_SEER);
    CHECK(ww_ai_apply(&G, &job, NULL, NOW, NULL, 0));          // 调用失败 = NULL,随机查
    CHECK(G.seer_target > 0 && G.seer_target != job.seat);
    // 过期的 job:阶段变了就不落地
    ww_ai_job_t stale = job;
    run(WW_T_ROLE_MIN + WW_T_CLOSE + 200);
    CHECK(!ww_ai_apply(&G, &stale, "{\"target\":2}", NOW, NULL, 0));
}

// AI 投票不能一开投就出手:每个 AI 有随机 3–15 秒的"想一会儿"
static void test_vote_delay(void)
{
    setup(6, 0, 31);
    CHECK(ww_start(&G, NOW) == WW_OK);
    for (int k = 0; k < 4000 && G.phase != WW_PH_DISCUSS && G.phase != WW_PH_OVER; k++) {
        ww_ai_job_t j;
        if (ww_ai_next_job(&G, &j) && j.task != WW_AI_SPEAK) ww_ai_apply(&G, &j, NULL, NOW, NULL, 0);
        run(200);
    }
    if (G.phase != WW_PH_DISCUSS) return;
    CHECK(ww_force(&G, NOW) && G.phase == WW_PH_VOTE);
    uint32_t t0 = NOW;
    int voters = ww_alive_count(&G);
    ww_ai_job_t j;
    CHECK(!ww_ai_next_job(&G, &j));                      // 刚开投:谁都还没"想好"
    int voted = 0;
    uint32_t first = 0;
    while (G.phase == WW_PH_VOTE && NOW - t0 < WW_T_BOT_VOTE_HI + 2000) {
        while (ww_ai_next_job(&G, &j) && j.task == WW_AI_VOTE) {
            if (!first) first = NOW - t0;
            CHECK(ww_ai_apply(&G, &j, NULL, NOW, NULL, 0));
            voted++;
        }
        run(100);
    }
    CHECK(first >= WW_T_BOT_VOTE_LO && voted == voters);
}

// 全 AI 局:所有决策和发言都走 ww_ai(假模型随机回合法/不合法 JSON 或失败),必须能打完
static void test_full_games(void)
{
    int games = 0;
    for (int n = 4; n <= 12; n += 2) {
        for (uint32_t seed = 1; seed <= 6; seed++) {
            setup(n, 0, seed * 17u + (uint32_t)n);
            CHECK(ww_start(&G, NOW) == WW_OK);
            int speeches = 0;
            for (int k = 0; k < 20000 && G.phase != WW_PH_OVER; k++) {
                ww_ai_job_t job;
                if (ww_ai_next_job(&G, &job)) {
                    CHECK(ww_ai_prompt(&G, &job, SYS, sizeof(SYS), USR, sizeof(USR)));
                    char reply[160];
                    uint32_t r = ww_rand(&G) % 4;
                    int t = (int)(ww_rand(&G) % (uint32_t)(n + 2));
                    if (r == 0) snprintf(reply, sizeof(reply), "{\"target\": %d}", t);
                    else if (r == 1) snprintf(reply, sizeof(reply), "{\"action\":\"poison\",\"target\":%d}", t);
                    else if (r == 2) snprintf(reply, sizeof(reply), "{\"speech\":\"我是%d号，我觉得%d号有问题。\"}", job.seat, t);
                    else reply[0] = '\0';
                    if (job.task == WW_AI_SPEAK) {
                        CHECK(ww_ai_apply(&G, &job, r == 3 ? NULL : reply, NOW, OUT, sizeof(OUT)));
                        CHECK(OUT[0] != '\0');
                        ww_add_speech(&G, job.seat, OUT, true, NOW);
                        speeches++;
                    } else {
                        CHECK(ww_ai_apply(&G, &job, r == 3 ? NULL : reply, NOW, NULL, 0));
                    }
                }
                run(200);
            }
            CHECK(G.phase == WW_PH_OVER);
            if (G.round > 1) CHECK(speeches > 0);
            games++;
        }
    }
    printf("AI 整局(假模型):%d 局全部打完\n", games);
}

int main(void)
{
    test_json();
    test_prompt_privacy();
    test_apply_and_fallback();
    test_vote_delay();
    test_full_games();
    if (g_fail) { fprintf(stderr, "ww_ai: %d/%d checks FAILED\n", g_fail, g_checks); return 1; }
    printf("ww_ai: all %d checks passed\n", g_checks);
    return 0;
}
