// main/ww_core.c —— 狼人杀规则状态机,见 ww_core.h。
#include "ww_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 板子,与 werewolf/config.py 的 ROLE_TABLE 一致:{狼, 预言家, 女巫, 村民}
static const uint8_t ROLE_TABLE[WW_MAX_SEATS + 1][4] = {
    [2]  = {1, 0, 0, 1},
    [3]  = {1, 1, 0, 1},
    [4]  = {1, 1, 1, 1},
    [5]  = {1, 1, 1, 2},
    [6]  = {2, 1, 1, 2},
    [7]  = {2, 1, 1, 3},
    [8]  = {3, 1, 1, 3},
    [9]  = {3, 1, 1, 4},
    [10] = {3, 1, 1, 5},
    [11] = {4, 1, 1, 5},
    [12] = {4, 1, 1, 6},
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static uint32_t rnd(ww_game_t *g)
{
    // xorshift32:足够打乱身份,种子由设备硬件随机数混入
    uint32_t x = g->rng ? g->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static uint32_t rnd_range(ww_game_t *g, uint32_t lo, uint32_t hi)
{
    return lo + rnd(g) % (hi - lo + 1u);
}

static void bump(ww_game_t *g) { g->version++; }

static void push_cue(ww_game_t *g, ww_cue_t cue)
{
    uint8_t next = (uint8_t)((g->cue_tail + 1) % WW_CUE_QUEUE);
    if (next == g->cue_head) return;           // 满了就丢,提示音迟到不如不到
    g->cues[g->cue_tail] = (uint8_t)cue;
    g->cue_tail = next;
}

ww_cue_t ww_pop_cue(ww_game_t *g)
{
    if (g->cue_head == g->cue_tail) return WW_CUE_NONE;
    ww_cue_t c = (ww_cue_t)g->cues[g->cue_head];
    g->cue_head = (uint8_t)((g->cue_head + 1) % WW_CUE_QUEUE);
    return c;
}

// 按 UTF-8 字符边界截断复制
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// 去掉首尾空白后复制名字,返回长度
static size_t clean_name(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return 0; }
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    copy_utf8(dst, cap, src);
    size_t n = strlen(dst);
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t' ||
                     dst[n - 1] == '\n' || dst[n - 1] == '\r')) {
        dst[--n] = '\0';
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)dst[i] < 0x20) dst[i] = ' ';
    }
    return n;
}

static void announce(ww_game_t *g, const char *fmt, ...)
{
    char *slot = g->log[g->log_total % WW_LOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, WW_LOG_LEN, fmt, ap);
    va_end(ap);
    g->log_total++;
}

static void notify(ww_game_t *g, int seat, const char *fmt, ...)
{
    if (seat < 1 || seat > WW_MAX_SEATS) return;
    char *slot = g->notices[seat][g->notice_total[seat] % WW_NOTICE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, WW_NOTICE_LEN, fmt, ap);
    va_end(ap);
    g->notice_total[seat]++;
}

const char *ww_log_at(const ww_game_t *g, int back)
{
    if (back < 0 || back >= WW_LOG_MAX || back >= g->log_total) return NULL;
    return g->log[(g->log_total - 1 - back) % WW_LOG_MAX];
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
int ww_by_seat(const ww_game_t *g, int seat)
{
    if (seat < 1) return -1;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        if (g->players[i].used && g->players[i].seat == seat) return i;
    }
    return -1;
}

static const ww_player_t *seat_player(const ww_game_t *g, int seat)
{
    int i = ww_by_seat(g, seat);
    return i >= 0 ? &g->players[i] : NULL;
}

static bool seat_alive(const ww_game_t *g, int seat)
{
    const ww_player_t *p = seat_player(g, seat);
    return p && p->alive;
}

int ww_seated_count(const ww_game_t *g)
{
    int n = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        if (g->players[i].used && g->players[i].seat) n++;
    }
    return n;
}

int ww_alive_count(const ww_game_t *g)
{
    int n = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (p->used && p->seat && p->alive) n++;
    }
    return n;
}

static int alive_role_count(const ww_game_t *g, ww_role_t role)
{
    int n = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (p->used && p->seat && p->alive && p->role == role) n++;
    }
    return n;
}

static int alive_role_player(const ww_game_t *g, ww_role_t role)
{
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (p->used && p->seat && p->alive && p->role == role) return i;
    }
    return -1;
}

int ww_find(const ww_game_t *g, const char *token)
{
    if (!token || strlen(token) != WW_TOKEN_LEN) return -1;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        if (g->players[i].used && strcmp(g->players[i].token, token) == 0) return i;
    }
    return -1;
}

bool ww_is_online(const ww_game_t *g, int pidx, uint32_t now)
{
    if (pidx < 0 || pidx >= WW_MAX_PLAYERS || !g->players[pidx].used) return false;
    if (g->players[pidx].bot) return true;
    return (uint32_t)(now - g->players[pidx].last_seen) <= WW_T_ONLINE;
}

bool ww_is_pending(const ww_game_t *g, int pidx)
{
    if (pidx < 0 || pidx >= WW_MAX_PLAYERS) return false;
    const ww_player_t *p = &g->players[pidx];
    if (!p->used || !p->seat || !p->alive) return false;
    switch (g->phase) {
    case WW_PH_WOLF:
        return g->step == 0 && p->role == WW_ROLE_WOLF && g->wolf_votes[pidx] < 0;
    case WW_PH_WITCH:
        return g->step == 0 && !g->fake && p->role == WW_ROLE_WITCH && g->witch_act < 0;
    case WW_PH_SEER:
        return g->step == 0 && !g->fake && p->role == WW_ROLE_SEER && g->seer_target < 0;
    case WW_PH_VOTE:
        return g->day_votes[pidx] < 0;
    default:
        return false;
    }
}

int ww_voted_count(const ww_game_t *g)
{
    int n = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (p->used && p->seat && p->alive && g->day_votes[i] >= 0) n++;
    }
    return n;
}

int ww_pending_count(const ww_game_t *g)
{
    return ww_alive_count(g) - ww_voted_count(g);
}

bool ww_night_role_phase(ww_phase_t ph)
{
    return ph == WW_PH_WOLF || ph == WW_PH_WITCH || ph == WW_PH_SEER;
}

uint32_t ww_elapsed_ms(const ww_game_t *g, uint32_t now)
{
    return now - g->phase_start;
}

int ww_left_s(const ww_game_t *g, uint32_t now)
{
    if (!g->until) return -1;
    if ((int32_t)(g->until - now) <= 0) return 0;
    return (int)((g->until - now + 999u) / 1000u);
}

int ww_role_count(int n, ww_role_t role)
{
    if (n < WW_MIN_SEATS || n > WW_MAX_SEATS) return 0;
    switch (role) {
    case WW_ROLE_WOLF:     return ROLE_TABLE[n][0];
    case WW_ROLE_SEER:     return ROLE_TABLE[n][1];
    case WW_ROLE_WITCH:    return ROLE_TABLE[n][2];
    case WW_ROLE_VILLAGER: return ROLE_TABLE[n][3];
    default:               return 0;
    }
}

const char *ww_role_name(int role)
{
    switch (role) {
    case WW_ROLE_WOLF:     return "狼人";
    case WW_ROLE_SEER:     return "预言家";
    case WW_ROLE_WITCH:    return "女巫";
    case WW_ROLE_VILLAGER: return "村民";
    default:               return "";
    }
}

static const char *role_key(int role)
{
    switch (role) {
    case WW_ROLE_WOLF:     return "wolf";
    case WW_ROLE_SEER:     return "seer";
    case WW_ROLE_WITCH:    return "witch";
    case WW_ROLE_VILLAGER: return "villager";
    default:               return "";
    }
}

const char *ww_phase_key(ww_phase_t ph)
{
    static const char *const KEYS[] = {
        [WW_PH_LOBBY] = "lobby",   [WW_PH_DEAL] = "deal",     [WW_PH_NIGHT] = "night",
        [WW_PH_WOLF] = "wolf",     [WW_PH_WITCH] = "witch",   [WW_PH_SEER] = "seer",
        [WW_PH_DAWN] = "dawn",     [WW_PH_DISCUSS] = "discuss", [WW_PH_VOTE] = "vote",
        [WW_PH_RESULT] = "result", [WW_PH_OVER] = "over",
    };
    if ((unsigned)ph >= sizeof(KEYS) / sizeof(KEYS[0])) return "lobby";
    return KEYS[ph];
}

const char *ww_err_str(int code)
{
    switch (code) {
    case WW_OK:      return "";
    case WW_E_TOKEN: return "你已不在房间里，请重新加入";
    case WW_E_PHASE: return "现在不能这样做";
    case WW_E_ARG:   return "目标无效";
    case WW_E_TAKEN: return "这个座位已经有人了";
    case WW_E_FULL:  return "房间已满";
    case WW_E_NAME:  return "请先填写名字";
    case WW_E_DONE:  return "已经提交过了";
    case WW_E_ROLE:  return "你的身份不能这样做";
    case WW_E_DEAD:  return "你已出局";
    case WW_E_SEATS: return "还没坐满";
    default:         return "操作失败";
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void ww_init(ww_game_t *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));
    g->phase = WW_PH_LOBBY;
    g->seat_count = 8;
    g->rng = seed ? seed : 1u;
    g->seer_target = -1;
    g->witch_act = -1;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        g->wolf_votes[i] = -1;
        g->day_votes[i] = -1;
    }
    g->version = 1;
}

void ww_reseed(ww_game_t *g, uint32_t entropy)
{
    g->rng ^= entropy;
    if (!g->rng) g->rng = 1u;
    (void)rnd(g);
}

static void set_phase(ww_game_t *g, ww_phase_t ph, uint32_t now, uint32_t dur)
{
    g->phase = ph;
    g->step = 0;
    g->fake = false;
    g->phase_start = now;
    g->until = dur ? now + dur : 0;
    if (g->until == 0 && dur) g->until = 1;     // 0 表示"不自动",避开回绕撞 0
    g->min_until = 0;
    g->deadline = 0;
    bump(g);
}

static bool time_reached(uint32_t now, uint32_t t)
{
    return t != 0 && (int32_t)(now - t) >= 0;
}

// ---------------------------------------------------------------------------
// 加入 / 入座
// ---------------------------------------------------------------------------
static void make_token(ww_game_t *g, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    do {
        for (int i = 0; i < WW_TOKEN_LEN; i += 8) {
            uint32_t r = rnd(g);
            for (int k = 0; k < 8; k++) {
                out[i + k] = HEX[r & 0xF];
                r >>= 4;
            }
        }
        out[WW_TOKEN_LEN] = '\0';
    } while (ww_find(g, out) >= 0);
}

int ww_join(ww_game_t *g, const char *name, uint32_t now, char *out_token)
{
    char clean[WW_NAME_MAX + 1];
    if (clean_name(clean, sizeof(clean), name) == 0) return WW_E_NAME;

    int slot = -1;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        if (!g->players[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        // 满了:挤掉最久没露面的"未入座"围观者,入座的人绝不挤
        uint32_t oldest = 0;
        for (int i = 0; i < WW_MAX_PLAYERS; i++) {
            const ww_player_t *p = &g->players[i];
            if (p->seat) continue;
            uint32_t idle = now - p->last_seen;
            if (slot < 0 || idle > oldest) { slot = i; oldest = idle; }
        }
        if (slot < 0) return WW_E_FULL;
    }

    ww_player_t *p = &g->players[slot];
    memset(p, 0, sizeof(*p));
    make_token(g, p->token);            // 先生成再标记 used,否则会和自己撞
    p->used = true;
    memcpy(p->name, clean, sizeof(p->name));
    p->alive = true;
    p->last_seen = now;
    g->wolf_votes[slot] = -1;
    g->day_votes[slot] = -1;
    if (out_token) memcpy(out_token, p->token, WW_TOKEN_LEN + 1);
    bump(g);
    return slot;
}

void ww_touch(ww_game_t *g, int pidx, uint32_t now)
{
    if (pidx < 0 || pidx >= WW_MAX_PLAYERS) return;
    g->players[pidx].last_seen = now;
}

static void remove_player(ww_game_t *g, int pidx)
{
    memset(&g->players[pidx], 0, sizeof(g->players[pidx]));
    g->wolf_votes[pidx] = -1;
    g->day_votes[pidx] = -1;
}

int ww_min_seats(const ww_game_t *g)
{
    // 减座位时高号的人会挪到空着的低号,所以下限只是"已入座人数"
    int lo = ww_seated_count(g);
    return lo < WW_MIN_SEATS ? WW_MIN_SEATS : lo;
}

int ww_set_seats(ww_game_t *g, int n)
{
    if (g->phase != WW_PH_LOBBY) return g->seat_count;
    int lo = ww_min_seats(g);
    if (n < lo) n = lo;
    if (n > WW_MAX_SEATS) n = WW_MAX_SEATS;
    if (n == g->seat_count) return n;
    // 座位号大于 n 的人,按原座位号顺序挪进 1..n 里最小的空位
    for (int s = n + 1; s <= WW_MAX_SEATS; s++) {
        int i = ww_by_seat(g, s);
        if (i < 0) continue;
        for (int t = 1; t <= n; t++) {
            if (ww_by_seat(g, t) < 0) { g->players[i].seat = (uint8_t)t; break; }
        }
    }
    g->seat_count = (uint8_t)n;
    bump(g);
    return g->seat_count;
}

int ww_bot_count(const ww_game_t *g)
{
    int n = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) if (g->players[i].used && g->players[i].bot) n++;
    return n;
}

int ww_add_bots(ww_game_t *g, uint32_t now)
{
    if (g->phase != WW_PH_LOBBY) return 0;
    int added = 0;
    for (int s = 1; s <= g->seat_count; s++) {
        if (ww_by_seat(g, s) >= 0) continue;
        char name[16];
        snprintf(name, sizeof(name), "机器人%d", s);
        int i = ww_join(g, name, now, NULL);
        if (i < 0) break;
        g->players[i].bot = true;
        g->players[i].seat = (uint8_t)s;
        added++;
    }
    if (added) bump(g);
    return added;
}

int ww_kick(ww_game_t *g, int seat)
{
    if (g->phase != WW_PH_LOBBY) return WW_E_PHASE;
    int i = ww_by_seat(g, seat);
    if (i < 0) return WW_E_ARG;
    remove_player(g, i);
    bump(g);
    return WW_OK;
}

static void clear_round_state(ww_game_t *g)
{
    g->step = 0;
    g->fake = false;
    g->until = g->min_until = g->deadline = 0;
    g->round = 0;
    g->kill_seat = 0;
    g->seer_target = -1;
    g->witch_act = -1;
    g->witch_target = 0;
    g->saved = false;
    g->poisoned = 0;
    g->n_dead = 0;
    g->n_order = 0;
    g->speaker = 0;
    g->speech_start = 0;
    g->out_seat = 0;
    g->winner = WW_WIN_NONE;
    g->antidote = true;
    g->poison = true;
    g->log_total = 0;
    g->speech_total = 0;
    g->spoke_mask = 0;
    g->busy = WW_BUSY_NONE;
    g->busy_seat = 0;
    memset(g->notice_total, 0, sizeof(g->notice_total));
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        g->wolf_votes[i] = -1;
        g->day_votes[i] = -1;
        g->players[i].alive = true;
        g->players[i].role = WW_ROLE_NONE;
        g->players[i].bot_at = 0;
    }
}

void ww_reset(ww_game_t *g)
{
    clear_round_state(g);
    g->phase = WW_PH_LOBBY;
    bump(g);
}

void ww_kick_all(ww_game_t *g)
{
    for (int i = 0; i < WW_MAX_PLAYERS; i++) remove_player(g, i);
    ww_reset(g);
}

// ---------------------------------------------------------------------------
// 开局
// ---------------------------------------------------------------------------
int ww_start(ww_game_t *g, uint32_t now)
{
    if (g->phase != WW_PH_LOBBY) return WW_E_PHASE;
    int n = g->seat_count;
    if (n < WW_MIN_SEATS || ww_seated_count(g) != n) return WW_E_SEATS;

    clear_round_state(g);

    uint8_t roles[WW_MAX_SEATS];
    int k = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < ROLE_TABLE[n][r]; c++) roles[k++] = (uint8_t)(WW_ROLE_WOLF + r);
    }
    while (k < n) roles[k++] = WW_ROLE_VILLAGER;
    for (int i = n - 1; i > 0; i--) {                 // Fisher–Yates
        int j = (int)(rnd(g) % (uint32_t)(i + 1));
        uint8_t t = roles[i]; roles[i] = roles[j]; roles[j] = t;
    }
    for (int seat = 1; seat <= n; seat++) {
        int i = ww_by_seat(g, seat);
        if (i >= 0) g->players[i].role = roles[seat - 1];
    }

    char board[96];
    int len = snprintf(board, sizeof(board), "%d名狼人", ROLE_TABLE[n][0]);
    if (ROLE_TABLE[n][2]) len += snprintf(board + len, sizeof(board) - (size_t)len, "，1名女巫");
    if (ROLE_TABLE[n][1]) len += snprintf(board + len, sizeof(board) - (size_t)len, "，1名预言家");
    snprintf(board + len, sizeof(board) - (size_t)len, "，%d名村民", ROLE_TABLE[n][3]);
    announce(g, "游戏开始！本局%d人：%s。请查看自己的身份，不要给别人看。", n, board);

    set_phase(g, WW_PH_DEAL, now, WW_T_DEAL);
    push_cue(g, WW_CUE_DEAL);
    return WW_OK;
}

// ---------------------------------------------------------------------------
// 夜晚
// ---------------------------------------------------------------------------
static void begin_night(ww_game_t *g, uint32_t now)
{
    g->round++;
    g->kill_seat = 0;
    g->seer_target = -1;
    g->witch_act = -1;
    g->witch_target = 0;
    g->saved = false;
    g->poisoned = 0;
    g->n_dead = 0;
    g->n_order = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        g->wolf_votes[i] = -1;
        g->day_votes[i] = -1;
    }
    announce(g, "第%d夜，天黑请闭眼。", g->round);
    set_phase(g, WW_PH_NIGHT, now, WW_T_NIGHT);
    push_cue(g, WW_CUE_NIGHT);
}

static bool role_phase_real(const ww_game_t *g, ww_phase_t ph)
{
    switch (ph) {
    case WW_PH_WOLF:  return alive_role_count(g, WW_ROLE_WOLF) > 0;
    case WW_PH_WITCH: return alive_role_player(g, WW_ROLE_WITCH) >= 0 && (g->antidote || g->poison);
    case WW_PH_SEER:  return alive_role_player(g, WW_ROLE_SEER) >= 0;
    default:          return false;
    }
}

static void enter_role(ww_game_t *g, ww_phase_t ph, uint32_t now)
{
    set_phase(g, ph, now, 0);
    if (role_phase_real(g, ph)) {
        g->min_until = now + rnd_range(g, WW_T_ROLE_MIN_LO, WW_T_ROLE_MIN);
        g->deadline = now + WW_T_ROLE_TIMEOUT;
        g->until = g->deadline;                 // 倒计时显示用超时
    } else {
        // 角色已死/板子里没有/女巫没药了:假装等一会儿,不让人从节奏里猜出来
        g->fake = true;
        g->until = now + rnd_range(g, WW_T_FAKE_MIN, WW_T_FAKE_MAX);
    }
    push_cue(g, ph == WW_PH_WOLF ? WW_CUE_WOLF_OPEN :
                ph == WW_PH_WITCH ? WW_CUE_WITCH_OPEN : WW_CUE_SEER_OPEN);
}

static bool role_done(const ww_game_t *g)
{
    switch (g->phase) {
    case WW_PH_WOLF:
        for (int i = 0; i < WW_MAX_PLAYERS; i++) {
            const ww_player_t *p = &g->players[i];
            if (p->used && p->seat && p->alive && p->role == WW_ROLE_WOLF &&
                g->wolf_votes[i] < 0) {
                return false;
            }
        }
        return true;
    case WW_PH_WITCH: return g->witch_act >= 0;
    case WW_PH_SEER:  return g->seer_target >= 0;
    default:          return true;
    }
}

static uint8_t resolve_wolf_kill(ww_game_t *g)
{
    int cnt[WW_MAX_SEATS + 1] = {0};
    bool any = false;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (!p->used || !p->seat || !p->alive || p->role != WW_ROLE_WOLF) continue;
        if (g->wolf_votes[i] < 0) continue;
        cnt[g->wolf_votes[i]]++;
        any = true;
    }
    if (!any) return 0;
    int top = 0;
    for (int s = 0; s <= WW_MAX_SEATS; s++) if (cnt[s] > top) top = cnt[s];
    int cands[WW_MAX_SEATS + 1];
    int nc = 0;
    for (int s = 0; s <= WW_MAX_SEATS; s++) if (cnt[s] == top) cands[nc++] = s;
    int pick = cands[rnd(g) % (uint32_t)nc];
    if (pick && !seat_alive(g, pick)) return 0;
    return (uint8_t)pick;
}

static void resolve_role(ww_game_t *g)
{
    if (g->fake) return;
    switch (g->phase) {
    case WW_PH_WOLF:
        g->kill_seat = resolve_wolf_kill(g);
        for (int i = 0; i < WW_MAX_PLAYERS; i++) {
            const ww_player_t *p = &g->players[i];
            if (!p->used || !p->seat || !p->alive || p->role != WW_ROLE_WOLF) continue;
            if (g->kill_seat) notify(g, p->seat, "今晚袭击目标：%d号", g->kill_seat);
            else notify(g, p->seat, "今晚空刀，没有袭击目标");
        }
        break;
    case WW_PH_WITCH: {
        int w = alive_role_player(g, WW_ROLE_WITCH);
        if (w < 0 || g->witch_act < 0) break;       // 超时/强推没选:什么也不做
        int seat = g->players[w].seat;
        int tgt = g->witch_target;
        if (g->witch_act == 1 && g->antidote && tgt && tgt == g->kill_seat) {
            g->saved = true;
            g->antidote = false;
            notify(g, seat, "你使用了解药，救下了%d号", tgt);
        } else if (g->witch_act == 2 && g->poison && tgt && seat_alive(g, tgt)) {
            g->poisoned = (uint8_t)tgt;
            g->poison = false;
            notify(g, seat, "你对%d号使用了毒药", tgt);
        } else {
            notify(g, seat, "今晚你没有用药");
        }
        break;
    }
    case WW_PH_SEER: {
        int s = alive_role_player(g, WW_ROLE_SEER);
        if (s < 0 || g->seer_target <= 0) break;
        const ww_player_t *t = seat_player(g, g->seer_target);
        if (!t) break;
        notify(g, g->players[s].seat, "查验结果：%d号 %s %s", g->seer_target, t->name,
               t->role == WW_ROLE_WOLF ? "是狼人" : "是好人");
        break;
    }
    default:
        break;
    }
}

static void close_role(ww_game_t *g, uint32_t now)
{
    resolve_role(g);
    ww_phase_t ph = g->phase;
    g->step = 1;
    g->phase_start = now;
    g->until = now + WW_T_CLOSE;
    g->min_until = g->deadline = 0;
    bump(g);
    push_cue(g, ph == WW_PH_WOLF ? WW_CUE_WOLF_CLOSE :
                ph == WW_PH_WITCH ? WW_CUE_WITCH_CLOSE : WW_CUE_SEER_CLOSE);
}

static void add_dead(ww_game_t *g, int seat)
{
    int i = ww_by_seat(g, seat);
    if (i < 0 || !g->players[i].alive) return;
    g->players[i].alive = false;
    // 按座位号有序插入
    int k = g->n_dead;
    while (k > 0 && g->dead_tonight[k - 1] > seat) {
        g->dead_tonight[k] = g->dead_tonight[k - 1];
        k--;
    }
    g->dead_tonight[k] = (uint8_t)seat;
    g->n_dead++;
}

// ---------------------------------------------------------------------------
// 白天
// ---------------------------------------------------------------------------
static int first_alive_seat(const ww_game_t *g)
{
    for (int s = 1; s <= g->seat_count; s++) if (seat_alive(g, s)) return s;
    return 1;
}

static int next_alive_seat(const ww_game_t *g, int from, int dir)
{
    int n = g->seat_count;
    for (int i = 1; i <= n; i++) {
        int s = ((from - 1 + dir * i) % n + n) % n + 1;
        if (seat_alive(g, s)) return s;
    }
    return first_alive_seat(g);
}

// 发言规则:方向每天交替(奇数天小→大,偶数天大→小);有死者从死者的下一位
// 开始,平安夜从上轮首发言者的下一位开始。
static void speech_plan(ww_game_t *g)
{
    int dir = (g->round % 2 == 1) ? 1 : -1;
    int anchor;
    if (g->n_dead) anchor = g->dead_tonight[0];
    else if (g->speech_start) anchor = g->speech_start;
    else anchor = dir == 1 ? g->seat_count : 1;
    int start = next_alive_seat(g, anchor, dir);
    g->speech_start = (uint8_t)start;
    g->n_order = 0;
    g->order[g->n_order++] = (uint8_t)start;
    int cur = start;
    int alive = ww_alive_count(g);
    for (int k = 0; k < alive - 1; k++) {
        cur = next_alive_seat(g, cur, dir);
        bool dup = false;
        for (int j = 0; j < g->n_order; j++) if (g->order[j] == cur) dup = true;
        if (dup) break;
        g->order[g->n_order++] = (uint8_t)cur;
    }
    g->speaker = 0;
}

static bool check_win(ww_game_t *g, uint32_t now)
{
    int wolves = alive_role_count(g, WW_ROLE_WOLF);
    int goods = ww_alive_count(g) - wolves;
    ww_winner_t w = WW_WIN_NONE;
    if (wolves == 0) w = WW_WIN_GOOD;
    else if (wolves >= goods) w = WW_WIN_WOLF;
    if (w == WW_WIN_NONE) return false;
    g->winner = w;
    set_phase(g, WW_PH_OVER, now, 0);
    if (w == WW_WIN_GOOD) {
        announce(g, "游戏结束！所有狼人已被消灭，好人阵营获胜！");
        push_cue(g, WW_CUE_WIN_GOOD);
    } else {
        announce(g, "游戏结束！狼人数量已不少于好人，狼人阵营获胜！");
        push_cue(g, WW_CUE_WIN_WOLF);
    }
    return true;
}

static void begin_dawn(ww_game_t *g, uint32_t now)
{
    if (g->kill_seat && !g->saved) add_dead(g, g->kill_seat);
    if (g->poisoned) add_dead(g, g->poisoned);     // 已死的 add_dead 会跳过

    if (g->n_dead) {
        char names[64] = "";
        size_t len = 0;
        for (int i = 0; i < g->n_dead; i++) {
            len += (size_t)snprintf(names + len, sizeof(names) - len, "%s%d号",
                                    i ? "、" : "", g->dead_tonight[i]);
            if (len >= sizeof(names)) break;
        }
        announce(g, "天亮了。昨晚 %s 死亡。", names);
        push_cue(g, WW_CUE_DAWN_DEATH);
    } else {
        announce(g, "天亮了。昨晚是平安夜，没有玩家死亡。");
        push_cue(g, WW_CUE_DAWN_PEACE);
    }
    set_phase(g, WW_PH_DAWN, now, WW_T_DAWN);
}

static void begin_discuss(ww_game_t *g, uint32_t now)
{
    speech_plan(g);
    int dir = (g->round % 2 == 1) ? 1 : -1;
    announce(g, "第%d天讨论：从%d号开始，按%s的方向依次发言。", g->round, g->order[0],
             dir == 1 ? "从小号到大号" : "从大号到小号");
    set_phase(g, WW_PH_DISCUSS, now, 0);
    g->speaker_since = now;
    g->spoke_mask = 0;
    push_cue(g, WW_CUE_DISCUSS);
}

static void begin_vote(ww_game_t *g, uint32_t now)
{
    for (int i = 0; i < WW_MAX_PLAYERS; i++) g->day_votes[i] = -1;
    announce(g, "讨论结束，开始投票。可以投票放逐一名玩家，也可以弃票。");
    set_phase(g, WW_PH_VOTE, now, 0);
    g->min_until = now + WW_T_VOTE_MIN;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        if (g->players[i].used && g->players[i].bot) {
            g->players[i].ai_after = now + rnd_range(g, WW_T_BOT_VOTE_LO, WW_T_BOT_VOTE_HI);
        }
    }
    push_cue(g, WW_CUE_VOTE);
}

static void resolve_vote(ww_game_t *g, uint32_t now)
{
    int cnt[WW_MAX_SEATS + 1] = {0};
    int abstain = 0;
    bool any = false;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (!p->used || !p->seat || !p->alive) continue;
        int v = g->day_votes[i];
        if (v <= 0) { abstain++; continue; }      // 弃票和没投都算弃票
        cnt[v]++;
        any = true;
    }
    g->out_seat = 0;
    if (!any) {
        announce(g, "投票结束。所有人弃票，本轮无人出局。");
    } else {
        char parts[160];
        size_t len = 0;
        int top = 0;
        for (int s = 1; s <= WW_MAX_SEATS; s++) {
            if (!cnt[s]) continue;
            if (len < sizeof(parts)) {
                len += (size_t)snprintf(parts + len, sizeof(parts) - len, "%s%d号 %d票",
                                        len ? "，" : "", s, cnt[s]);
            }
            if (cnt[s] > top) top = cnt[s];
        }
        if (abstain && len < sizeof(parts)) {
            snprintf(parts + len, sizeof(parts) - len, "，弃票 %d人", abstain);
        }
        int winners = 0, out = 0;
        for (int s = 1; s <= WW_MAX_SEATS; s++) {
            if (cnt[s] == top) { winners++; out = s; }
        }
        if (winners > 1) {
            announce(g, "投票结束。计票：%s。出现平票，本轮无人出局。", parts);
        } else {
            g->out_seat = (uint8_t)out;
            int i = ww_by_seat(g, out);
            if (i >= 0) g->players[i].alive = false;
            announce(g, "投票结束。计票：%s。%d号玩家被放逐出局。", parts, out);
        }
    }
    set_phase(g, WW_PH_RESULT, now, WW_T_RESULT);
    push_cue(g, WW_CUE_VOTE_END);
}

// ---------------------------------------------------------------------------
// 推进
// ---------------------------------------------------------------------------
static void advance(ww_game_t *g, uint32_t now)
{
    switch (g->phase) {
    case WW_PH_DEAL:
        begin_night(g, now);
        break;
    case WW_PH_NIGHT:
        enter_role(g, WW_PH_WOLF, now);
        break;
    case WW_PH_WOLF:
    case WW_PH_WITCH:
    case WW_PH_SEER:
        if (g->step == 0) {
            close_role(g, now);
        } else if (g->phase == WW_PH_WOLF) {
            enter_role(g, WW_PH_WITCH, now);        // 用户定制:女巫先于预言家
        } else if (g->phase == WW_PH_WITCH) {
            enter_role(g, WW_PH_SEER, now);
        } else {
            begin_dawn(g, now);
        }
        break;
    case WW_PH_DAWN:
        if (!check_win(g, now)) begin_discuss(g, now);
        break;
    case WW_PH_DISCUSS:
        begin_vote(g, now);
        break;
    case WW_PH_VOTE:
        resolve_vote(g, now);
        break;
    case WW_PH_RESULT:
        if (!check_win(g, now)) begin_night(g, now);
        break;
    default:
        break;
    }
}

static int do_act(ww_game_t *g, int pidx, ww_act_t act, int arg, int arg2,
                  const char *text, uint32_t now);

// 随机挑一个存活座位;except 排除,wolves_ok = false 时排除狼人
static int bot_pick(ww_game_t *g, int except, bool wolves_ok)
{
    int c[WW_MAX_SEATS], n = 0;
    for (int s = 1; s <= g->seat_count; s++) {
        int i = ww_by_seat(g, s);
        if (i < 0 || !g->players[i].alive || s == except) continue;
        if (!wolves_ok && g->players[i].role == WW_ROLE_WOLF) continue;
        c[n++] = s;
    }
    return n ? c[rnd(g) % (uint32_t)n] : 0;
}

// 机器人:轮到它时等 1.5–5 秒(像人在点手机),然后随机但合法地行动
static void bots_act(ww_game_t *g, uint32_t now)
{
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        ww_player_t *p = &g->players[i];
        if (!p->used || !p->bot) continue;
        if (!ww_is_pending(g, i)) { p->bot_at = 0; continue; }
        if (!p->bot_at) {
            // 有大脑时先等大脑(它会直接调 ww_act),等太久才随机兜底;没大脑像人一样点 1.5–5 秒
            p->bot_at = now + (g->brain ? WW_T_BRAIN :
                               g->phase == WW_PH_VOTE ? rnd_range(g, WW_T_BOT_VOTE_LO, WW_T_BOT_VOTE_HI) :
                               rnd_range(g, 1500, 5000));
            if (!p->bot_at) p->bot_at = 1;
            continue;
        }
        if (!time_reached(now, p->bot_at)) continue;
        p->bot_at = 0;
        uint32_t r = rnd(g) % 100;
        switch (g->phase) {
        case WW_PH_WOLF:
            do_act(g, i, WW_ACT_WOLF, bot_pick(g, 0, false), 0, NULL, now);
            break;
        case WW_PH_WITCH:
            if (g->antidote && g->kill_seat && r < 50) do_act(g, i, WW_ACT_WITCH, 1, 0, NULL, now);
            else if (g->poison && r >= 85) do_act(g, i, WW_ACT_WITCH, 2, bot_pick(g, p->seat, true), NULL, now);
            else do_act(g, i, WW_ACT_WITCH, 0, 0, NULL, now);
            break;
        case WW_PH_SEER:
            do_act(g, i, WW_ACT_SEER, bot_pick(g, p->seat, true), 0, NULL, now);
            break;
        case WW_PH_VOTE:
            do_act(g, i, WW_ACT_VOTE, r < 85 ? bot_pick(g, p->seat, true) : 0, 0, NULL, now);
            break;
        default:
            break;
        }
    }
    // 活着的全是机器人:讨论阶段没人会点"开始投票",过 8 秒自动开始
    if (g->phase == WW_PH_DISCUSS && time_reached(now, g->phase_start + 8000)) {
        bool human = false;
        for (int i = 0; i < WW_MAX_PLAYERS; i++) {
            const ww_player_t *p = &g->players[i];
            if (p->used && p->seat && p->alive && !p->bot) human = true;
        }
        // 有大脑时要等每个 AI 都发完言再投票
        bool all_spoke = true;
        for (int k = 0; k < g->n_order; k++) if (!ww_has_spoken(g, g->order[k])) all_spoke = false;
        if (!human && (!g->brain || all_spoke)) begin_vote(g, now);
    }
}

void ww_tick(ww_game_t *g, uint32_t now)
{
    g->now = now;
    bots_act(g, now);
    // 在线状态变化也算"可见变化"
    uint16_t mask = 0;
    for (int i = 0; i < WW_MAX_PLAYERS; i++) {
        const ww_player_t *p = &g->players[i];
        if (p->used && p->seat && ww_is_online(g, i, now)) mask |= (uint16_t)(1u << p->seat);
    }
    if (mask != g->online_mask) {
        g->online_mask = mask;
        bump(g);
    }

    // 一次 tick 最多连推几步(比如假等待到点 + 闭眼到点),防止死循环
    for (int guard = 0; guard < 4; guard++) {
        bool go = false;
        switch (g->phase) {
        case WW_PH_DEAL:
        case WW_PH_NIGHT:
        case WW_PH_DAWN:
        case WW_PH_RESULT:
            go = time_reached(now, g->until);
            break;
        case WW_PH_WOLF:
        case WW_PH_WITCH:
        case WW_PH_SEER:
            if (g->step == 1 || g->fake) go = time_reached(now, g->until);
            else go = (role_done(g) && time_reached(now, g->min_until)) ||
                      time_reached(now, g->deadline);
            break;
        case WW_PH_VOTE:
            go = ww_pending_count(g) == 0 && time_reached(now, g->min_until);
            break;
        default:
            break;
        }
        if (!go) break;
        advance(g, now);
    }
}

bool ww_force(ww_game_t *g, uint32_t now)
{
    switch (g->phase) {
    case WW_PH_LOBBY:
    case WW_PH_OVER:
        return false;
    case WW_PH_WOLF:
    case WW_PH_WITCH:
    case WW_PH_SEER:
        if (g->step == 0 && g->fake) {
            // 假等待也照常走"闭眼",节奏和真角色一样
            close_role(g, now);
            return true;
        }
        advance(g, now);
        return true;
    default:
        advance(g, now);
        return true;
    }
}

uint32_t ww_rand(ww_game_t *g) { return rnd(g); }

bool ww_is_bot(const ww_game_t *g, int seat)
{
    int i = ww_by_seat(g, seat);
    return i >= 0 && g->players[i].bot;
}

void ww_add_speech(ww_game_t *g, int seat, const char *text, bool advance, uint32_t now)
{
    if (!text || !text[0] || seat < 1 || seat > WW_MAX_SEATS) return;
    ww_speech_t *sp = &g->speech[g->speech_total % WW_SPEECH_MAX];
    sp->seat = (uint8_t)seat;
    sp->round = g->round;
    copy_utf8(sp->text, sizeof(sp->text), text);
    for (char *q = sp->text; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
    g->speech_total++;
    if (g->phase == WW_PH_DISCUSS) g->spoke_mask |= (uint16_t)(1u << seat);
    bump(g);
    if (advance && g->phase == WW_PH_DISCUSS && g->n_order && g->order[g->speaker] == seat &&
        g->speaker + 1 < g->n_order) {
        ww_speaker_move(g, 1, now);
    }
}

const ww_speech_t *ww_speech_at(const ww_game_t *g, int back)
{
    if (back < 0 || back >= WW_SPEECH_MAX || back >= g->speech_total) return NULL;
    return &g->speech[(g->speech_total - 1 - back) % WW_SPEECH_MAX];
}

bool ww_has_spoken(const ww_game_t *g, int seat)
{
    return seat >= 1 && seat <= WW_MAX_SEATS && (g->spoke_mask & (1u << seat));
}

int ww_current_speaker(const ww_game_t *g)
{
    if (g->phase != WW_PH_DISCUSS || !g->n_order) return 0;
    return g->order[g->speaker];
}

void ww_set_busy(ww_game_t *g, ww_busy_t busy, int seat)
{
    if (g->busy == busy && g->busy_seat == seat) return;
    g->busy = (uint8_t)busy;
    g->busy_seat = (uint8_t)seat;
    bump(g);
}

void ww_speaker_move(ww_game_t *g, int delta, uint32_t now)
{
    if (g->phase != WW_PH_DISCUSS || g->n_order == 0) return;
    int s = (int)g->speaker + delta;
    if (s < 0) s = 0;
    if (s >= g->n_order) s = g->n_order - 1;
    if (s == g->speaker) return;
    g->speaker = (uint8_t)s;
    g->speaker_since = now;
    bump(g);
}

// ---------------------------------------------------------------------------
// 玩家动作
// ---------------------------------------------------------------------------
static int do_act(ww_game_t *g, int pidx, ww_act_t act, int arg, int arg2,
                  const char *text, uint32_t now)
{
    if (pidx < 0 || pidx >= WW_MAX_PLAYERS || !g->players[pidx].used) return WW_E_TOKEN;
    ww_player_t *p = &g->players[pidx];
    p->last_seen = now;

    switch (act) {
    case WW_ACT_SEAT:
        if (g->phase != WW_PH_LOBBY) return WW_E_PHASE;
        if (arg < 1 || arg > g->seat_count) return WW_E_ARG;
        if (ww_by_seat(g, arg) >= 0) return ww_by_seat(g, arg) == pidx ? WW_OK : WW_E_TAKEN;
        p->seat = (uint8_t)arg;
        break;
    case WW_ACT_UNSEAT:
        if (g->phase != WW_PH_LOBBY) return WW_E_PHASE;
        p->seat = 0;
        break;
    case WW_ACT_RENAME: {
        char clean[WW_NAME_MAX + 1];
        if (clean_name(clean, sizeof(clean), text) == 0) return WW_E_NAME;
        memcpy(p->name, clean, sizeof(p->name));
        break;
    }
    case WW_ACT_LEAVE:
        if (g->phase != WW_PH_LOBBY && p->seat) return WW_E_PHASE;
        remove_player(g, pidx);
        break;
    case WW_ACT_WOLF:
        if (g->phase != WW_PH_WOLF || g->step != 0) return WW_E_PHASE;
        if (!p->seat || p->role != WW_ROLE_WOLF) return WW_E_ROLE;
        if (!p->alive) return WW_E_DEAD;
        if (arg != 0 && !seat_alive(g, arg)) return WW_E_ARG;
        g->wolf_votes[pidx] = (int8_t)arg;          // 可以改票,以最后一次为准
        break;
    case WW_ACT_WITCH:
        if (g->phase != WW_PH_WITCH || g->step != 0 || g->fake) return WW_E_PHASE;
        if (!p->seat || p->role != WW_ROLE_WITCH) return WW_E_ROLE;
        if (!p->alive) return WW_E_DEAD;
        if (g->witch_act >= 0) return WW_E_DONE;
        if (arg < 0 || arg > 2) return WW_E_ARG;
        if (arg == 1 && (!g->antidote || !g->kill_seat)) return WW_E_ARG;
        if (arg == 2 && (!g->poison || !seat_alive(g, arg2))) return WW_E_ARG;
        g->witch_act = (int8_t)arg;
        g->witch_target = (uint8_t)(arg == 1 ? g->kill_seat : arg == 2 ? arg2 : 0);
        break;
    case WW_ACT_SEER:
        if (g->phase != WW_PH_SEER || g->step != 0 || g->fake) return WW_E_PHASE;
        if (!p->seat || p->role != WW_ROLE_SEER) return WW_E_ROLE;
        if (!p->alive) return WW_E_DEAD;
        if (g->seer_target >= 0) return WW_E_DONE;
        if (arg == p->seat || !seat_alive(g, arg)) return WW_E_ARG;
        g->seer_target = (int8_t)arg;
        break;
    case WW_ACT_START_VOTE:
        if (g->phase != WW_PH_DISCUSS) return WW_E_PHASE;
        if (!p->seat || !p->alive) return WW_E_DEAD;
        begin_vote(g, now);
        break;
    case WW_ACT_VOTE:
        if (g->phase != WW_PH_VOTE) return WW_E_PHASE;
        if (!p->seat || !p->alive) return WW_E_DEAD;
        if (g->day_votes[pidx] >= 0) return WW_E_DONE;
        if (arg != 0 && (arg == p->seat || !seat_alive(g, arg))) return WW_E_ARG;
        g->day_votes[pidx] = (int8_t)arg;
        break;
    default:
        return WW_E_ARG;
    }
    bump(g);
    return WW_OK;
}

int ww_act(ww_game_t *g, int pidx, ww_act_t act, int arg, int arg2,
           const char *text, uint32_t now)
{
    int rc = do_act(g, pidx, act, arg, arg2, text, now);
    if (rc == WW_OK) ww_tick(g, now);
    return rc;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool bad;
} jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    size_t n = strlen(s);
    if (w->len + n + 1 > w->cap) { w->bad = true; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    if (w->bad) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || w->len + (size_t)n + 1 > w->cap) { w->bad = true; return; }
    w->len += (size_t)n;
}

static void jw_str(jw_t *w, const char *s)
{
    jw_raw(w, "\"");
    for (; *s && !w->bad; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') jw_raw(w, "\\\"");
        else if (c == '\\') jw_raw(w, "\\\\");
        else if (c < 0x20) jw_fmt(w, "\\u%04x", c);
        else {
            char one[2] = {(char)c, 0};
            jw_raw(w, one);
        }
    }
    jw_raw(w, "\"");
}

size_t ww_state_json(const ww_game_t *g, int pidx, uint32_t now, char *buf, size_t cap)
{
    jw_t w = {buf, cap, 0, false};
    if (cap) buf[0] = '\0';
    const ww_player_t *me = (pidx >= 0 && pidx < WW_MAX_PLAYERS && g->players[pidx].used)
                            ? &g->players[pidx] : NULL;
    bool playing = g->phase != WW_PH_LOBBY;
    bool god = playing && (g->phase == WW_PH_OVER || (me && me->seat && !me->alive));

    // 夜晚角色阶段的倒计时只给正在行动的本人看:假等待(角色已死)只有 5–9 秒,
    // 真行动是 60 秒超时,给所有人看等于公布"女巫/预言家还活着没"。
    int left = ww_left_s(g, now);
    if (ww_night_role_phase(g->phase) && g->step == 0 && !ww_is_pending(g, pidx)) left = -1;

    jw_fmt(&w, "{\"v\":%lu,\"ph\":\"%s\",\"step\":%d,\"round\":%d,\"n\":%d,"
               "\"el\":%lu,\"left\":%d,\"seated\":%d,\"alive\":%d",
           (unsigned long)g->version, ww_phase_key(g->phase), g->step, g->round,
           g->seat_count, (unsigned long)(ww_elapsed_ms(g, now) / 1000u),
           left, ww_seated_count(g), ww_alive_count(g));
    if (playing) {
        jw_fmt(&w, ",\"board\":[%d,%d,%d,%d]",
               ww_role_count(g->seat_count, WW_ROLE_WOLF), ww_role_count(g->seat_count, WW_ROLE_SEER),
               ww_role_count(g->seat_count, WW_ROLE_WITCH), ww_role_count(g->seat_count, WW_ROLE_VILLAGER));
    }

    jw_raw(&w, ",\"seats\":[");
    for (int s = 1; s <= g->seat_count; s++) {
        if (s > 1) jw_raw(&w, ",");
        int i = ww_by_seat(g, s);
        if (i < 0) { jw_fmt(&w, "{\"s\":%d}", s); continue; }
        const ww_player_t *p = &g->players[i];
        jw_fmt(&w, "{\"s\":%d,\"name\":", s);
        jw_str(&w, p->name);
        jw_fmt(&w, ",\"on\":%d,\"alive\":%d", ww_is_online(g, i, now) ? 1 : 0, p->alive ? 1 : 0);
        if (p->bot) jw_raw(&w, ",\"bot\":1");
        if (playing && p->role) {
            if (god || (me && p == me)) jw_fmt(&w, ",\"role\":\"%s\"", role_key(p->role));
            else if (me && me->role == WW_ROLE_WOLF && p->role == WW_ROLE_WOLF) jw_raw(&w, ",\"mate\":1");
        }
        if (g->phase == WW_PH_VOTE && p->alive && g->day_votes[i] >= 0) jw_raw(&w, ",\"voted\":1");
        jw_raw(&w, "}");
    }
    jw_raw(&w, "]");

    if (g->phase == WW_PH_DISCUSS && g->n_order) {
        jw_raw(&w, ",\"order\":[");
        for (int k = 0; k < g->n_order; k++) jw_fmt(&w, "%s%d", k ? "," : "", g->order[k]);
        jw_fmt(&w, "],\"cur\":%d,\"spk_el\":%lu", g->speaker,
               (unsigned long)((now - g->speaker_since) / 1000u));
    }
    if (g->phase == WW_PH_VOTE) {
        jw_fmt(&w, ",\"voted\":%d", ww_voted_count(g));
    }
    if (g->phase == WW_PH_DAWN || g->phase == WW_PH_DISCUSS) {
        jw_raw(&w, ",\"dead\":[");
        for (int k = 0; k < g->n_dead; k++) jw_fmt(&w, "%s%d", k ? "," : "", g->dead_tonight[k]);
        jw_raw(&w, "]");
    }
    if (g->phase == WW_PH_RESULT) jw_fmt(&w, ",\"out\":%d", g->out_seat);
    if (g->phase == WW_PH_OVER) {
        jw_fmt(&w, ",\"winner\":\"%s\"", g->winner == WW_WIN_GOOD ? "good" : "wolf");
    }

    if (g->busy) jw_fmt(&w, ",\"busy\":%d,\"busy_s\":%d", g->busy, g->busy_seat);
    // 发言记录:最近 6 条,按时间顺序
    if (g->speech_total) {
        jw_raw(&w, ",\"speech\":[");
        int n = g->speech_total < 6 ? g->speech_total : 6;
        for (int b = n - 1; b >= 0; b--) {
            const ww_speech_t *sp = ww_speech_at(g, b);
            jw_fmt(&w, "%s{\"s\":%d,\"r\":%d,\"t\":", b == n - 1 ? "" : ",", sp->seat, sp->round);
            jw_str(&w, sp->text);
            jw_raw(&w, "}");
        }
        jw_raw(&w, "]");
    }

    jw_raw(&w, ",\"log\":[");
    for (int b = 0; b < 6; b++) {
        const char *line = ww_log_at(g, b);
        if (!line) break;
        if (b) jw_raw(&w, ",");
        jw_str(&w, line);
    }
    jw_raw(&w, "]");

    if (me) {
        jw_fmt(&w, ",\"you\":{\"seat\":%d,\"alive\":%d,\"name\":", me->seat, me->alive ? 1 : 0);
        jw_str(&w, me->name);
        if (playing && me->role) jw_fmt(&w, ",\"role\":\"%s\"", role_key(me->role));
        jw_fmt(&w, ",\"pending\":%d}", ww_is_pending(g, pidx) ? 1 : 0);

        if (me->seat) {
            int total = g->notice_total[me->seat];
            int shown = total < WW_NOTICE_MAX ? total : WW_NOTICE_MAX;
            jw_raw(&w, ",\"notices\":[");
            for (int k = 0; k < shown; k++) {
                if (k) jw_raw(&w, ",");
                jw_str(&w, g->notices[me->seat][(total - 1 - k) % WW_NOTICE_MAX]);
            }
            jw_raw(&w, "]");
        }

        bool act = me->seat && me->alive && g->step == 0;
        if (act && g->phase == WW_PH_WOLF && me->role == WW_ROLE_WOLF) {
            jw_raw(&w, ",\"wolf_votes\":{");
            bool first = true;
            for (int i = 0; i < WW_MAX_PLAYERS; i++) {
                const ww_player_t *p = &g->players[i];
                if (!p->used || !p->seat || !p->alive || p->role != WW_ROLE_WOLF) continue;
                if (g->wolf_votes[i] < 0) continue;
                jw_fmt(&w, "%s\"%d\":%d", first ? "" : ",", p->seat, g->wolf_votes[i]);
                first = false;
            }
            jw_raw(&w, "}");
        }
        if (act && g->phase == WW_PH_WITCH && me->role == WW_ROLE_WITCH && !g->fake) {
            jw_fmt(&w, ",\"witch\":{\"antidote\":%d,\"poison\":%d,\"kill\":%d,\"done\":%d}",
                   g->antidote ? 1 : 0, g->poison ? 1 : 0, g->antidote ? g->kill_seat : 0,
                   g->witch_act >= 0 ? 1 : 0);
        }
        if (act && g->phase == WW_PH_SEER && me->role == WW_ROLE_SEER) {
            jw_fmt(&w, ",\"seer_done\":%d", g->seer_target >= 0 ? 1 : 0);
        }
        if (g->phase == WW_PH_VOTE && me->seat && me->alive && g->day_votes[pidx] >= 0) {
            jw_fmt(&w, ",\"my_vote\":%d", g->day_votes[pidx]);
        }
    }
    jw_raw(&w, "}");
    return w.bad ? 0 : w.len;
}
