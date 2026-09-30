// main/ww_ai.c —— AI 玩家提示词与决策落地,见 ww_ai.h。
#include "ww_ai.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 规则提要与证据纪律:照搬 werewolf/prompts.py 的 RULES_BASE / FACT_RULES
static const char RULES[] =
    "狼人杀规则提要：狼人每晚袭击一人；女巫一瓶解药一瓶毒药、每晚最多用一瓶；"
    "预言家每晚查验一人阵营；白天讨论后投票放逐。狼人全灭好人胜，狼人数不少于好人数狼人胜。"
    "重要规则说明：白天发言环节【没有诚实义务】——隐藏自己的身份、伪装成其他角色、"
    "虚张声势、误导对手，都是本游戏规则完全允许的正常博弈手段（对好人阵营同样如此，"
    "例如神职通常需要权衡自曝身份被狼人夜袭的风险）。是否公开、何时公开、"
    "是否伪装身份，完全由你根据局势自行权衡。\n"
    "证据纪律：不要编造或脑补别人没说过的话，只能引用发言记录里实际出现的内容。";

typedef struct {
    char *buf;
    size_t cap, len;
    bool full;
} sb_t;

static void sb_add(sb_t *b, const char *fmt, ...)
{
    if (b->full) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->full = true;
        b->buf[b->len] = '\0';           // 半截不要
        return;
    }
    b->len += (size_t)n;
}

static const ww_player_t *seat_p(const ww_game_t *g, int seat)
{
    int i = ww_by_seat(g, seat);
    return i >= 0 ? &g->players[i] : NULL;
}

static const char *phase_cn(const ww_game_t *g)
{
    switch (g->phase) {
    case WW_PH_WOLF:    return "夜晚·狼人行动";
    case WW_PH_WITCH:   return "夜晚·女巫行动";
    case WW_PH_SEER:    return "夜晚·预言家行动";
    case WW_PH_DISCUSS: return "白天·讨论";
    case WW_PH_VOTE:    return "白天·投票";
    default:            return ww_phase_key(g->phase);
    }
}

// 候选座位列表 "[1, 3, 5]";except = 排除的座位;no_wolf = 排除狼人
static int candidates(const ww_game_t *g, int except, bool no_wolf, int *out)
{
    int n = 0;
    for (int s = 1; s <= g->seat_count; s++) {
        const ww_player_t *p = seat_p(g, s);
        if (!p || !p->alive || s == except) continue;
        if (no_wolf && p->role == WW_ROLE_WOLF) continue;
        out[n++] = s;
    }
    return n;
}

static void add_list(sb_t *b, const int *v, int n)
{
    sb_add(b, "[");
    for (int k = 0; k < n; k++) sb_add(b, "%s%d", k ? ", " : "", v[k]);
    sb_add(b, "]");
}

bool ww_ai_job_valid(const ww_game_t *g, const ww_ai_job_t *job)
{
    if (g->phase != job->phase || g->round != job->round) return false;
    int i = ww_by_seat(g, job->seat);
    if (i < 0 || !g->players[i].alive) return false;
    if (job->task == WW_AI_SPEAK) {
        return ww_current_speaker(g) == job->seat && !ww_has_spoken(g, job->seat);
    }
    return ww_is_pending(g, i);
}

bool ww_ai_next_job(const ww_game_t *g, ww_ai_job_t *job)
{
    memset(job, 0, sizeof(*job));
    job->phase = (uint8_t)g->phase;
    job->round = g->round;
    for (int s = 1; s <= g->seat_count; s++) {
        int i = ww_by_seat(g, s);
        if (i < 0 || !g->players[i].bot || !ww_is_pending(g, i)) continue;
        // 投票:到了它的随机"想好"时刻才出手,别一开投 AI 就齐刷刷投完
        if (g->phase == WW_PH_VOTE && g->players[i].ai_after &&
            (int32_t)(g->now - g->players[i].ai_after) < 0) continue;
        job->seat = s;
        switch (g->phase) {
        case WW_PH_WOLF:  job->task = WW_AI_WOLF; break;
        case WW_PH_WITCH: job->task = WW_AI_WITCH; break;
        case WW_PH_SEER:  job->task = WW_AI_SEER; break;
        case WW_PH_VOTE:  job->task = WW_AI_VOTE; break;
        default:          continue;
        }
        return true;
    }
    int cur = ww_current_speaker(g);
    if (cur && ww_is_bot(g, cur) && seat_p(g, cur)->alive && !ww_has_spoken(g, cur)) {
        job->task = WW_AI_SPEAK;
        job->seat = cur;
        return true;
    }
    job->task = WW_AI_NONE;
    return false;
}

bool ww_ai_prompt(const ww_game_t *g, const ww_ai_job_t *job,
                  char *sys, size_t sys_cap, char *user, size_t user_cap)
{
    const ww_player_t *me = seat_p(g, job->seat);
    if (!me) return false;
    sb_t b = {sys, sys_cap, 0, false};
    sys[0] = '\0';

    sb_add(&b, "你是狼人杀玩家：%d号「%s」，身份【%s】。\n", job->seat, me->name, ww_role_name(me->role));
    sb_add(&b, "第%d天，阶段：%s。\n", g->round, phase_cn(g));
    sb_add(&b, "存活玩家：");
    bool first = true;
    for (int s = 1; s <= g->seat_count; s++) {
        const ww_player_t *p = seat_p(g, s);
        if (p && p->alive) { sb_add(&b, "%s%d号%s", first ? "" : "、", s, p->name); first = false; }
    }
    sb_add(&b, "。");
    first = true;
    for (int s = 1; s <= g->seat_count; s++) {
        const ww_player_t *p = seat_p(g, s);
        if (p && !p->alive) { sb_add(&b, "%s%d号%s", first ? " 已出局：" : "、", s, p->name); first = false; }
    }
    sb_add(&b, "\n");
    if (me->role == WW_ROLE_WOLF) {
        first = true;
        for (int s = 1; s <= g->seat_count; s++) {
            const ww_player_t *p = seat_p(g, s);
            if (p && p != me && p->alive && p->role == WW_ROLE_WOLF) {
                sb_add(&b, "%s%d号", first ? "你的狼队友：" : "、", s);
                first = false;
            }
        }
        if (!first) sb_add(&b, "。\n");
    }
    int nt = g->notice_total[job->seat];
    int shown = nt < WW_NOTICE_MAX ? nt : WW_NOTICE_MAX;
    if (shown) {
        sb_add(&b, "你的私密信息：");
        for (int k = shown - 1; k >= 0; k--) {
            sb_add(&b, "%s%s", k == shown - 1 ? "" : "；", g->notices[job->seat][(nt - 1 - k) % WW_NOTICE_MAX]);
        }
        sb_add(&b, "\n");
    }
    first = true;
    for (int k = 5; k >= 0; k--) {
        const char *l = ww_log_at(g, k);
        if (!l) continue;
        sb_add(&b, "%s%s", first ? "主持人公告：" : " / ", l);
        first = false;
    }
    if (!first) sb_add(&b, "\n");

    // 发言记录:从最新往回装,总量控制在缓冲余量里(给规则和收尾留出 1.6 KB)
    size_t budget = b.cap > b.len + 1600 ? b.cap - b.len - 1600 : 0;
    int take = 0;
    size_t used = 0;
    for (int k = 0; k < WW_SPEECH_MAX; k++) {
        const ww_speech_t *sp = ww_speech_at(g, k);
        if (!sp) break;
        size_t need = strlen(sp->text) + 24;
        if (used + need > budget) break;
        used += need;
        take++;
    }
    if (take) {
        sb_add(&b, "发言记录（按时间顺序，真人发言是语音转写，可能有错字）：\n");
        for (int k = take - 1; k >= 0; k--) {
            const ww_speech_t *sp = ww_speech_at(g, k);
            sb_add(&b, "- 第%d天 %d号：%s\n", sp->round, sp->seat, sp->text);
        }
    }
    sb_add(&b, "%s\n请做出对你所在阵营最有利的决策。你必须只输出一个 JSON 对象，不要任何其他文字。", RULES);
    if (b.full) return false;

    sb_t u = {user, user_cap, 0, false};
    user[0] = '\0';
    int c[WW_MAX_SEATS], n;
    switch (job->task) {
    case WW_AI_WOLF:
        n = candidates(g, 0, true, c);
        sb_add(&u, "狼人袭击阶段。可选目标座位：");
        add_list(&u, c, n);
        sb_add(&u, "，或 0 表示空刀。返回 JSON：{\"target\": 座位号, \"reason\": \"简短理由\"}");
        break;
    case WW_AI_SEER:
        n = candidates(g, job->seat, false, c);
        sb_add(&u, "预言家查验阶段。可选座位：");
        add_list(&u, c, n);
        sb_add(&u, "。你的查验记录在私密信息里，优先查没查过的。"
                   "返回 JSON：{\"target\": 座位号, \"reason\": \"简短理由\"}");
        break;
    case WW_AI_WITCH: {
        int kill = g->antidote ? g->kill_seat : 0;
        sb_add(&u, "女巫用药阶段。");
        if (!g->antidote) sb_add(&u, "你的解药已经用过，看不到今晚刀口。");
        else if (kill) sb_add(&u, "今晚被狼人袭击的是%d号。", kill);
        else sb_add(&u, "今晚没有人被袭击。");
        sb_add(&u, "解药%s，毒药%s，一晚最多用一瓶。", g->antidote ? "还在" : "已用", g->poison ? "还在" : "已用");
        n = candidates(g, 0, false, c);
        sb_add(&u, "可下毒的座位：");
        add_list(&u, c, n);
        sb_add(&u, "。返回 JSON：{\"action\": \"save\"|\"poison\"|\"pass\", \"target\": 座位号或0, \"reason\": \"简短理由\"}");
        break;
    }
    case WW_AI_VOTE:
        n = candidates(g, job->seat, false, c);
        sb_add(&u, "放逐投票阶段。可投座位：");
        add_list(&u, c, n);
        sb_add(&u, "（不能投自己），0 表示弃票。结合发言记录与你的身份投票。"
                   "返回 JSON：{\"target\": 座位号或0, \"reason\": \"简短理由\"}");
        break;
    case WW_AI_SPEAK:
        sb_add(&u, "现在是第%d天白天讨论，轮到你（%d号）发言。综合已知信息和前面的发言，"
                   "用中文口语发言，40到100个字，像真人在牌桌上说话，观点清晰，符合你的身份利益。"
                   "不要提到自己是 AI，不要念规则。返回 JSON：{\"speech\": \"发言内容\"}",
               g->round, job->seat);
        break;
    default:
        return false;
    }
    return !u.full;
}

// ---------------------------------------------------------------------------
// 极简 JSON 取值(模型输出就几个字段,不值得拉一个解析库进来)
// ---------------------------------------------------------------------------
static const char *find_key(const char *json, const char *key)
{
    if (!json) return NULL;
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

bool ww_ai_json_int(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    if (!p) return false;
    if (*p == '"') p++;              // 有的模型会把数字写成字符串
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    *out = atoi(p);
    return true;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ww_ai_json_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = find_key(json, key);
    if (!p || *p != '"' || !cap) return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char ch[4];
        size_t k = 0;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': case 'r': case 't': ch[k++] = ' '; break;
            case 'u': {
                unsigned v = 0;
                bool ok = true;
                for (int i = 1; i <= 4; i++) {
                    int h = hexv(p[i]);
                    if (h < 0) { ok = false; break; }
                    v = v * 16 + (unsigned)h;
                }
                if (!ok) { ch[k++] = '?'; break; }
                p += 4;
                if (v < 0x80) ch[k++] = (char)v;
                else if (v < 0x800) { ch[k++] = (char)(0xC0 | (v >> 6)); ch[k++] = (char)(0x80 | (v & 0x3F)); }
                else {
                    ch[k++] = (char)(0xE0 | (v >> 12));
                    ch[k++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    ch[k++] = (char)(0x80 | (v & 0x3F));
                }
                break;
            }
            default: ch[k++] = *p; break;
            }
            p++;
        } else {
            ch[k++] = *p++;
        }
        if (n + k >= cap) break;
        memcpy(out + n, ch, k);
        n += k;
    }
    // 截断时别留半个 UTF-8 字符:找到最后一个字符的首字节,不完整就整个丢掉
    if (n > 0) {
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)out[start] & 0xC0) == 0x80) start--;
        unsigned char lead = (unsigned char)out[start];
        size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (start + want > n) n = start;
    }
    out[n] = '\0';
    return true;
}

static int pick(ww_game_t *g, const int *c, int n)
{
    return n ? c[ww_rand(g) % (uint32_t)n] : 0;
}

static bool in_list(int v, const int *c, int n)
{
    for (int k = 0; k < n; k++) if (c[k] == v) return true;
    return false;
}

bool ww_ai_apply(ww_game_t *g, const ww_ai_job_t *job, const char *json, uint32_t now,
                 char *speech_out, size_t speech_cap)
{
    if (!ww_ai_job_valid(g, job)) return false;
    int pidx = ww_by_seat(g, job->seat);
    int c[WW_MAX_SEATS], n, t = -1;
    switch (job->task) {
    case WW_AI_WOLF:
        n = candidates(g, 0, true, c);
        if (!ww_ai_json_int(json, "target", &t) || (t != 0 && !in_list(t, c, n))) t = pick(g, c, n);
        return ww_act(g, pidx, WW_ACT_WOLF, t, 0, NULL, now) == WW_OK;
    case WW_AI_SEER:
        n = candidates(g, job->seat, false, c);
        if (!ww_ai_json_int(json, "target", &t) || !in_list(t, c, n)) t = pick(g, c, n);
        return ww_act(g, pidx, WW_ACT_SEER, t, 0, NULL, now) == WW_OK;
    case WW_AI_WITCH: {
        char act[12] = "";
        ww_ai_json_str(json, "action", act, sizeof(act));
        ww_ai_json_int(json, "target", &t);
        n = candidates(g, 0, false, c);
        int rc = WW_E_ARG;
        if (strcmp(act, "save") == 0 && g->antidote && g->kill_seat) {
            rc = ww_act(g, pidx, WW_ACT_WITCH, 1, 0, NULL, now);
        } else if (strcmp(act, "poison") == 0 && g->poison && in_list(t, c, n)) {
            rc = ww_act(g, pidx, WW_ACT_WITCH, 2, t, NULL, now);
        }
        if (rc != WW_OK) rc = ww_act(g, pidx, WW_ACT_WITCH, 0, 0, NULL, now);   // 看不懂就不用药
        return rc == WW_OK;
    }
    case WW_AI_VOTE:
        n = candidates(g, job->seat, false, c);
        if (!ww_ai_json_int(json, "target", &t) || (t != 0 && !in_list(t, c, n))) t = pick(g, c, n);
        return ww_act(g, pidx, WW_ACT_VOTE, t, 0, NULL, now) == WW_OK;
    case WW_AI_SPEAK:
        if (!speech_out || !speech_cap) return false;
        if (!ww_ai_json_str(json, "speech", speech_out, speech_cap) || !speech_out[0]) {
            snprintf(speech_out, speech_cap, "我是%d号，这轮我先听听大家的，过。", job->seat);
        }
        return true;
    default:
        return false;
    }
}
