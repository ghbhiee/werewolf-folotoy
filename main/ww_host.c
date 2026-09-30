// main/ww_host.c —— FoloToy 主持端按键逻辑与视图模型,见 ww_host.h。
//
// 按键映射(HANDOFF.md 里有完整表格):
//   确定长按   任何界面 = 打开主持菜单;菜单里 = 返回上一级
//   确定单击   推进类动作(开始/跳过/强推/投票/再来一局),都要 3 秒内再按一次确认
//   确定双击   大厅(热点模式)= 切换二维码;对局中 = 状态页
//   上/下单击  大厅 = 座位数 ±1;讨论 = 上一位/下一位发言人;菜单 = 移动
#include "ww_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CONFIRM_MS 3000
#define TOAST_MS   2500

// 二次确认的动作
enum {
    CA_NONE = 0,
    CA_START,
    CA_SKIP,
    CA_FORCE,
    CA_VOTE,
    CA_END_VOTE,
    CA_AGAIN,
    CA_CLEAR,
    CA_END_GAME,
    CA_NET_AP,
    CA_NET_SETUP,
    CA_NET_FORGET,
    CA_KICK_BASE = 100,   // + 座位号
};

// 菜单项
enum {
    MI_KICK = 1,
    MI_NET,
    MI_VOLUME,
    MI_CLEAR,
    MI_FORCE,
    MI_END,
    MI_BACK,
    MI_NET_AP,
    MI_NET_SETUP,
    MI_NET_FORGET,
    MI_NET_STA,
    MI_BOTS,
    MI_AI,
    MI_AI_KEYS,
    MI_AI_TEST,
    MI_AI_TOGGLE,
};

typedef struct {
    int id;
    const char *label;
} item_t;

static void fmt(char *dst, size_t cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsnprintf(dst, cap, f, ap);
    va_end(ap);
}

void ww_host_init(ww_host_t *h, ww_game_t *g)
{
    memset(h, 0, sizeof(*h));
    h->g = g;
    h->volume = 70;
    h->net.mode = WW_NET_BOOT;
}

void ww_host_toast(ww_host_t *h, const char *text, uint32_t now)
{
    fmt(h->toast, sizeof(h->toast), "%s", text);
    h->toast_until = now + TOAST_MS;
    if (!h->toast_until) h->toast_until = 1;
}

ww_req_t ww_host_take_request(ww_host_t *h)
{
    ww_req_t r = h->request;
    h->request = WW_REQ_NONE;
    return r;
}

static bool before(uint32_t now, uint32_t t) { return t && (int32_t)(t - now) > 0; }

// 第一次按:进入待确认;3 秒内同一动作再按一次:返回 true
static bool confirm(ww_host_t *h, int action, uint32_t now)
{
    if (h->confirm == action && before(now, h->confirm_until)) {
        h->confirm = CA_NONE;
        return true;
    }
    h->confirm = action;
    h->confirm_until = now + CONFIRM_MS;
    h->toast_until = 0;
    return false;
}

static const char *confirm_label(int action)
{
    switch (action) {
    case CA_START:    return "开始游戏";
    case CA_SKIP:     return "跳过等待";
    case CA_FORCE:    return "强制下一步";
    case CA_VOTE:     return "开始投票";
    case CA_END_VOTE: return "结束投票";
    case CA_AGAIN:    return "再来一局";
    case CA_CLEAR:    return "清空房间";
    case CA_END_GAME: return "结束本局";
    case CA_NET_AP:     return "改用热点";
    case CA_NET_SETUP:  return "进入配网";
    case CA_NET_FORGET: return "忘记家里Wi-Fi";
    default:          return "踢出玩家";
    }
}

static bool in_game(const ww_game_t *g) { return g->phase != WW_PH_LOBBY; }

// ---------------------------------------------------------------------------
// 菜单内容
// ---------------------------------------------------------------------------
static int main_menu(const ww_host_t *h, item_t *out)
{
    int n = 0;
    if (!in_game(h->g)) {
        out[n++] = (item_t){MI_KICK, "踢出玩家"};
        out[n++] = (item_t){MI_BOTS, "机器人补满空位"};
        out[n++] = (item_t){MI_AI, "AI 设置"};
        out[n++] = (item_t){MI_NET, "网络设置"};
        out[n++] = (item_t){MI_VOLUME, "音量"};
        out[n++] = (item_t){MI_CLEAR, "清空房间"};
    } else {
        if (h->g->phase != WW_PH_OVER) out[n++] = (item_t){MI_FORCE, "强制下一步"};
        out[n++] = (item_t){MI_END, "结束本局"};
        out[n++] = (item_t){MI_VOLUME, "音量"};
        out[n++] = (item_t){MI_CLEAR, "清空房间"};
    }
    out[n++] = (item_t){MI_BACK, "返回"};
    return n;
}

static int ai_menu(const ww_host_t *h, item_t *out)
{
    int n = 0;
    out[n++] = (item_t){MI_AI_KEYS, "填写 Key(扫码)"};
    out[n++] = (item_t){MI_AI_TEST, "测试 AI"};
    out[n++] = (item_t){MI_AI_TOGGLE, h->ai_on ? "AI 玩家：开" : "AI 玩家：关"};
    out[n++] = (item_t){MI_BACK, "返回"};
    return n;
}

bool ww_host_ai_ready(const ww_host_t *h)
{
    return h->ai_on && h->key_ds && h->key_dk;
}

static int net_menu(item_t *out)
{
    int n = 0;
    out[n++] = (item_t){MI_NET_STA, "连回家里 Wi-Fi"};
    out[n++] = (item_t){MI_NET_AP, "使用热点模式"};
    out[n++] = (item_t){MI_NET_SETUP, "配置家里 Wi-Fi"};
    out[n++] = (item_t){MI_NET_FORGET, "忘记家里 Wi-Fi"};
    out[n++] = (item_t){MI_BACK, "返回"};
    return n;
}

// 踢人列表:已入座的座位号,最后一项"返回"(座位号 0)
static int kick_list(const ww_host_t *h, int *seats)
{
    int n = 0;
    for (int s = 1; s <= h->g->seat_count; s++) {
        if (ww_by_seat(h->g, s) >= 0) seats[n++] = s;
    }
    seats[n++] = 0;
    return n;
}

static int count_items(const ww_host_t *h)
{
    item_t items[WWV_ITEMS_MAX];
    int seats[WW_MAX_SEATS + 1];
    switch (h->screen) {
    case WW_HS_MENU: return main_menu(h, items);
    case WW_HS_NET:  return net_menu(items);
    case WW_HS_AI:   return ai_menu(h, items);
    case WW_HS_KICK: return kick_list(h, seats);
    default:         return 0;
    }
}

static void go(ww_host_t *h, ww_host_screen_t s)
{
    int sel = 0;
    if (h->screen == WW_HS_MENU && s != WW_HS_MAIN) h->parent_sel = h->sel;   // 进子菜单
    if (s == WW_HS_MENU && h->screen != WW_HS_MAIN) sel = h->parent_sel;      // 从子菜单返回
    h->screen = s;
    h->sel = sel;
    h->confirm = CA_NONE;
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------
static void main_key(ww_host_t *h, ww_key_t key, ww_gesture_t ges, uint32_t now)
{
    ww_game_t *g = h->g;
    bool armed = h->confirm != CA_NONE && before(now, h->confirm_until);

    // 配网页:单击确定 = 退出配网,连回家里 Wi-Fi(没配过就回热点);上/下不做任何事
    if (h->net.mode == WW_NET_SETUP && h->screen == WW_HS_MAIN && ges == WW_GES_CLICK) {
        if (key == WW_KEY_OK) {
            h->request = WW_REQ_NET_STA;
            ww_host_toast(h, "正在连回家里Wi-Fi…", now);
        }
        return;
    }

    if (key != WW_KEY_OK) {
        if (armed) { h->confirm = CA_NONE; return; }   // 上/下 = 取消待确认
        if (ges != WW_GES_CLICK) return;
        int d = key == WW_KEY_UP ? 1 : -1;
        if (g->phase == WW_PH_LOBBY) {
            int want = g->seat_count + d;
            int got = ww_set_seats(g, want);
            if (got != want && d < 0 && want >= WW_MIN_SEATS) {
                char t[64];
                fmt(t, sizeof(t), "已有%d人入座，不能再少", got);
                ww_host_toast(h, t, now);
            }
        } else if (g->phase == WW_PH_DISCUSS) {
            ww_speaker_move(g, key == WW_KEY_UP ? -1 : 1, now);
        }
        return;
    }

    if (ges == WW_GES_DOUBLE) {
        h->confirm = CA_NONE;
        if (g->phase == WW_PH_LOBBY) {
            if (h->net.mode == WW_NET_AP || h->net.mode == WW_NET_SETUP) h->qr_page ^= 1;
            else go(h, WW_HS_STATUS);
        } else {
            go(h, WW_HS_STATUS);
        }
        return;
    }

    // 确定单击
    switch (g->phase) {
    case WW_PH_LOBBY: {
        int missing = g->seat_count - ww_seated_count(g);
        if (missing > 0) {
            char t[64];
            fmt(t, sizeof(t), "还差%d人入座", missing);
            h->confirm = CA_NONE;
            ww_host_toast(h, t, now);
        } else if (confirm(h, CA_START, now)) {
            ww_start(g, now);
        }
        break;
    }
    case WW_PH_DEAL:
    case WW_PH_NIGHT:
    case WW_PH_DAWN:
    case WW_PH_RESULT:
        if (confirm(h, CA_SKIP, now)) ww_force(g, now);
        break;
    case WW_PH_WOLF:
    case WW_PH_WITCH:
    case WW_PH_SEER:
        if (confirm(h, CA_FORCE, now)) ww_force(g, now);
        break;
    case WW_PH_DISCUSS:
        if (confirm(h, CA_VOTE, now)) ww_force(g, now);
        break;
    case WW_PH_VOTE:
        if (confirm(h, CA_END_VOTE, now)) ww_force(g, now);
        break;
    case WW_PH_OVER:
        if (confirm(h, CA_AGAIN, now)) ww_reset(g);
        break;
    }
}

static void menu_pick(ww_host_t *h, int id, uint32_t now)
{
    ww_game_t *g = h->g;
    switch (id) {
    case MI_BACK:
        go(h, h->screen == WW_HS_MENU ? WW_HS_MAIN : WW_HS_MENU);
        break;
    case MI_KICK:
        if (ww_seated_count(g) == 0) ww_host_toast(h, "还没有人入座", now);
        else go(h, WW_HS_KICK);
        break;
    case MI_BOTS: {
        int n = ww_add_bots(g, now);
        char t[64];
        if (n) fmt(t, sizeof(t), "加了%d个机器人，可以开始了", n);
        else fmt(t, sizeof(t), "没有空座位");
        go(h, WW_HS_MAIN);
        ww_host_toast(h, t, now);
        break;
    }
    case MI_NET:
        go(h, WW_HS_NET);
        break;
    case MI_AI:
        go(h, WW_HS_AI);
        break;
    case MI_AI_KEYS:
        go(h, WW_HS_KEYS);
        break;
    case MI_AI_TEST:
        if (!h->key_ds || !h->key_dk) {
            ww_host_toast(h, "先填 Key", now);
        } else {
            h->request = WW_REQ_AI_TEST;
            fmt(h->ai_msg, sizeof(h->ai_msg), "测试中…");
        }
        break;
    case MI_AI_TOGGLE:
        h->ai_on = !h->ai_on;
        h->request = WW_REQ_AI_SAVE;
        if (h->ai_on && (!h->key_ds || !h->key_dk)) ww_host_toast(h, "已打开，但还没填齐 Key", now);
        break;
    case MI_VOLUME:
        go(h, WW_HS_VOLUME);
        break;
    case MI_CLEAR:
        if (confirm(h, CA_CLEAR, now)) {
            ww_kick_all(g);
            go(h, WW_HS_MAIN);
            ww_host_toast(h, "房间已清空", now);
        }
        break;
    case MI_FORCE:
        if (confirm(h, CA_FORCE, now)) {
            ww_force(g, now);
            go(h, WW_HS_MAIN);
        }
        break;
    case MI_END:
        if (confirm(h, CA_END_GAME, now)) {
            ww_reset(g);
            go(h, WW_HS_MAIN);
            ww_host_toast(h, "已结束本局，回到大厅", now);
        }
        break;
    // 这三项会让 FoloToy 离开家里 Wi-Fi(手机全掉线),要按两次确定
    case MI_NET_AP:
        if (confirm(h, CA_NET_AP, now)) {
            h->request = WW_REQ_NET_AP;
            go(h, WW_HS_MAIN);
        }
        break;
    case MI_NET_SETUP:
        if (confirm(h, CA_NET_SETUP, now)) {
            h->request = WW_REQ_NET_SETUP;
            h->qr_page = 0;
            go(h, WW_HS_MAIN);
        }
        break;
    case MI_NET_FORGET:
        if (confirm(h, CA_NET_FORGET, now)) {
            h->request = WW_REQ_NET_FORGET;
            go(h, WW_HS_MAIN);
        }
        break;
    case MI_NET_STA:
        h->request = WW_REQ_NET_STA;
        go(h, WW_HS_MAIN);
        break;
    default:
        break;
    }
}

static void list_key(ww_host_t *h, ww_key_t key, ww_gesture_t ges, uint32_t now)
{
    if (ges != WW_GES_CLICK) return;
    int n = count_items(h);
    if (key != WW_KEY_OK) {
        h->confirm = CA_NONE;
        h->sel = (h->sel + (key == WW_KEY_UP ? n - 1 : 1)) % n;
        return;
    }
    if (h->sel >= n) h->sel = n - 1;
    if (h->screen == WW_HS_KICK) {
        int seats[WW_MAX_SEATS + 1];
        kick_list(h, seats);
        int seat = seats[h->sel];
        if (!seat) { go(h, WW_HS_MENU); return; }
        if (confirm(h, CA_KICK_BASE + seat, now)) {
            ww_kick(h->g, seat);
            char t[64];
            fmt(t, sizeof(t), "已踢出%d号", seat);
            ww_host_toast(h, t, now);
            int left = count_items(h);
            if (left <= 1) go(h, WW_HS_MENU);
            else if (h->sel >= left) h->sel = left - 1;
        }
        return;
    }
    item_t items[WWV_ITEMS_MAX];
    if (h->screen == WW_HS_MENU) main_menu(h, items);
    else if (h->screen == WW_HS_AI) ai_menu(h, items);
    else net_menu(items);
    menu_pick(h, items[h->sel].id, now);
}

void ww_host_key(ww_host_t *h, ww_key_t key, ww_gesture_t ges, uint32_t now)
{
    // 松手:只对"按住说话"有意义,其余一律忽略
    if (ges == WW_GES_RELEASE) {
        if (h->ptt && key == WW_KEY_OK) {
            h->ptt = false;
            h->ptt_req = 2;
        }
        return;
    }
    // 状态页 / 扫码填 Key 页:任意键返回
    if (h->screen == WW_HS_STATUS) {
        go(h, WW_HS_MAIN);
        return;
    }
    if (h->screen == WW_HS_KEYS) {
        go(h, WW_HS_AI);
        return;
    }
    // 游戏结束/阶段变化时菜单里的项可能变了,选中项别越界
    int n = count_items(h);
    if (n && h->sel >= n) h->sel = n - 1;

    // 讨论阶段按住确定 = 说话(轮到真人时),松手发送;此时主持菜单改成长按"上"
    if (ges == WW_GES_LONG && key == WW_KEY_OK && h->screen == WW_HS_MAIN &&
        h->g->phase == WW_PH_DISCUSS) {
        int cur = ww_current_speaker(h->g);
        if (!ww_host_ai_ready(h)) {
            ww_host_toast(h, "没开 AI，不能录音(长按上=菜单)", now);
        } else if (!cur || ww_is_bot(h->g, cur)) {
            ww_host_toast(h, "现在轮到 AI 发言", now);
        } else if (h->g->busy) {
            ww_host_toast(h, "稍等，正在处理上一段", now);
        } else {
            h->confirm = CA_NONE;
            h->ptt = true;
            h->ptt_req = 1;
        }
        return;
    }

    if (ges == WW_GES_LONG) {
        if (key == WW_KEY_DOWN) return;
        switch (h->screen) {
        case WW_HS_MAIN:  go(h, WW_HS_MENU); break;
        case WW_HS_MENU:  go(h, WW_HS_MAIN); break;
        case WW_HS_KEYS:
        case WW_HS_NET:
        case WW_HS_AI:
        case WW_HS_KICK:
        case WW_HS_VOLUME:
        default:          go(h, h->screen == WW_HS_KEYS ? WW_HS_AI : WW_HS_MENU); break;
        }
        return;
    }

    switch (h->screen) {
    case WW_HS_MAIN:
        main_key(h, key, ges, now);
        break;
    case WW_HS_VOLUME:
        if (ges != WW_GES_CLICK) break;
        if (key == WW_KEY_OK) { go(h, WW_HS_MENU); break; }
        if (key == WW_KEY_UP && h->volume <= 90) h->volume += 10;
        if (key == WW_KEY_DOWN && h->volume >= 10) h->volume -= 10;
        h->volume_dirty = true;
        break;
    default:
        list_key(h, key, ges, now);
        break;
    }
}

// ---------------------------------------------------------------------------
// 视图
// ---------------------------------------------------------------------------
static bool is_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return false;
    return true;
}

// Wi-Fi 二维码里 \ ; , : " 要转义
static void wifi_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (strchr("\\;,:\"", *src)) dst[n++] = '\\';
        dst[n++] = *src;
    }
    dst[n] = '\0';
}

static void fill_seats(const ww_host_t *h, uint32_t now, ww_view_t *v)
{
    const ww_game_t *g = h->g;
    v->n_seats = g->seat_count;
    for (int s = 1; s <= g->seat_count; s++) {
        uint8_t f = 0;
        int i = ww_by_seat(g, s);
        if (i >= 0) {
            f |= WWV_SEAT_OCC;
            if (ww_is_online(g, i, now)) f |= WWV_SEAT_ON;
            if (!g->players[i].alive && g->phase != WW_PH_LOBBY) f |= WWV_SEAT_DEAD;
            if (g->phase == WW_PH_VOTE && g->players[i].alive && g->day_votes[i] >= 0) f |= WWV_SEAT_VOTED;
        }
        v->seat[s - 1] = f;
    }
    if (g->phase == WW_PH_DISCUSS && g->n_order) {
        v->seat[g->order[g->speaker] - 1] |= WWV_SEAT_SPEAK;
    }
    if (g->phase == WW_PH_DAWN) {
        for (int k = 0; k < g->n_dead; k++) v->seat[g->dead_tonight[k] - 1] |= WWV_SEAT_OUT;
    }
    if (g->phase == WW_PH_RESULT && g->out_seat) v->seat[g->out_seat - 1] |= WWV_SEAT_OUT;
}

static const char *join_url(const ww_host_t *h, char *buf, size_t cap)
{
    fmt(buf, cap, "http://%s/", h->net.ip[0] ? h->net.ip : "192.168.4.1");
    return buf;
}

static void view_lobby(const ww_host_t *h, uint32_t now, ww_view_t *v)
{
    const ww_game_t *g = h->g;
    char url[40];
    v->kind = WWV_LOBBY;
    fmt(v->title, sizeof(v->title), "狼人杀");
    join_url(h, url, sizeof(url));
    if (h->net.mode == WW_NET_AP) {
        if (h->qr_page == 0) {
            char s[80], p[140];
            wifi_escape(s, sizeof(s), h->net.ssid);
            wifi_escape(p, sizeof(p), h->net.pass);
            fmt(v->qr, sizeof(v->qr), "WIFI:S:%s;T:WPA;P:%s;;", s, p);
            fmt(v->qr_label, sizeof(v->qr_label), "①扫码连Wi-Fi %s", h->net.ssid);
            fmt(v->qr_label2, sizeof(v->qr_label2), "密码 %s   双击确定→②", h->net.pass);
        } else {
            fmt(v->qr, sizeof(v->qr), "%s", url);
            fmt(v->qr_label, sizeof(v->qr_label), "②扫码打开 %s", url);
            fmt(v->qr_label2, sizeof(v->qr_label2), "双击确定→①连Wi-Fi");
        }
    } else {
        fmt(v->qr, sizeof(v->qr), "%s", url);
        fmt(v->qr_label, sizeof(v->qr_label), "扫码加入 %s", url);
        if (is_ascii(h->net.ssid) && h->net.ssid[0]) {
            fmt(v->qr_label2, sizeof(v->qr_label2), "手机先连 Wi-Fi：%s", h->net.ssid);
        } else {
            fmt(v->qr_label2, sizeof(v->qr_label2), "手机先连同一个 Wi-Fi");
        }
    }
    int online = 0;
    for (int s = 1; s <= g->seat_count; s++) {
        int i = ww_by_seat(g, s);
        if (i >= 0 && ww_is_online(g, i, now)) online++;
    }
    fmt(v->info, sizeof(v->info), "座位 %d · 已入座 %d · 在线 %d",
        g->seat_count, ww_seated_count(g), online);
    fill_seats(h, now, v);
    fmt(v->hint, sizeof(v->hint), "上下=座位数 确定=开始 长按=菜单");
}

static void view_setup(const ww_host_t *h, ww_view_t *v)
{
    v->kind = WWV_SETUP;
    fmt(v->title, sizeof(v->title), "配置Wi-Fi");
    if (h->qr_page == 0) {
        char s[80], p[140];
        wifi_escape(s, sizeof(s), h->net.ssid);
        wifi_escape(p, sizeof(p), h->net.pass);
        fmt(v->qr, sizeof(v->qr), "WIFI:S:%s;T:WPA;P:%s;;", s, p);
        fmt(v->qr_label, sizeof(v->qr_label), "①扫码连 %s", h->net.ssid);
        fmt(v->qr_label2, sizeof(v->qr_label2), "密码 %s   双击确定→②", h->net.pass);
    } else {
        fmt(v->qr, sizeof(v->qr), "http://%s/setup", h->net.ip[0] ? h->net.ip : "192.168.4.1");
        fmt(v->qr_label, sizeof(v->qr_label), "②扫码填写家里 Wi-Fi");
        fmt(v->qr_label2, sizeof(v->qr_label2), "双击确定→①");
    }
    fmt(v->info, sizeof(v->info), "保存后自动重启连接");
    fmt(v->hint, sizeof(v->hint), "按确定=退出，连回家里Wi-Fi");
}

static void role_list(const ww_game_t *g, ww_role_t role, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (int s = 1; s <= g->seat_count && n < cap; s++) {
        int i = ww_by_seat(g, s);
        if (i >= 0 && g->players[i].role == role) {
            n += (size_t)snprintf(out + n, cap - n, "%s%d号", n ? " " : "", s);
        }
    }
}

static void view_game(const ww_host_t *h, uint32_t now, ww_view_t *v)
{
    const ww_game_t *g = h->g;
    v->kind = WWV_GAME;
    v->timer = -1;
    fill_seats(h, now, v);
    int el = (int)(ww_elapsed_ms(g, now) / 1000u);
    int left = ww_left_s(g, now);

    switch (g->phase) {
    case WW_PH_DEAL:
        fmt(v->title, sizeof(v->title), "发身份");
        fmt(v->big, sizeof(v->big), "请查看手机");
        fmt(v->line1, sizeof(v->line1), "本局%d人：%d狼 %d预言家 %d女巫 %d村民",
            g->seat_count, ww_role_count(g->seat_count, WW_ROLE_WOLF),
            ww_role_count(g->seat_count, WW_ROLE_SEER), ww_role_count(g->seat_count, WW_ROLE_WITCH),
            ww_role_count(g->seat_count, WW_ROLE_VILLAGER));
        fmt(v->line2, sizeof(v->line2), "记住身份，别给别人看");
        v->timer = left;
        v->timer_down = true;
        fmt(v->hint, sizeof(v->hint), "确定=跳过 双击=状态 长按=菜单");
        break;
    case WW_PH_NIGHT:
        v->night = true;
        fmt(v->title, sizeof(v->title), "第%d夜", g->round);
        fmt(v->big, sizeof(v->big), "天黑请闭眼");
        fmt(v->line1, sizeof(v->line1), "所有人闭上眼睛");
        v->timer = left;
        v->timer_down = true;
        fmt(v->hint, sizeof(v->hint), "确定=跳过 双击=状态 长按=菜单");
        break;
    case WW_PH_WOLF:
    case WW_PH_WITCH:
    case WW_PH_SEER: {
        static const char *const NAME[] = {"狼人", "女巫", "预言家"};
        static const char *const DO[] = {
            "狼人在手机上选择袭击目标",
            "女巫在手机上决定是否用药",
            "预言家在手机上查验一名玩家",
        };
        int k = g->phase == WW_PH_WOLF ? 0 : g->phase == WW_PH_WITCH ? 1 : 2;
        v->night = true;
        fmt(v->title, sizeof(v->title), "第%d夜", g->round);
        if (g->step == 0) {
            fmt(v->big, sizeof(v->big), "%s请睁眼", NAME[k]);
            fmt(v->line1, sizeof(v->line1), "%s", DO[k]);
            fmt(v->line2, sizeof(v->line2), "其他人继续闭眼");
            // 只显示已等多久,不显示倒计时:假等待和真行动的时长不同,会泄露身份
            v->timer = el;
            v->timer_down = false;
        } else {
            fmt(v->big, sizeof(v->big), "%s请闭眼", NAME[k]);
        }
        fmt(v->hint, sizeof(v->hint), "确定=强制下一步 长按=菜单");
        break;
    }
    case WW_PH_DAWN:
        fmt(v->title, sizeof(v->title), "第%d天", g->round);
        fmt(v->big, sizeof(v->big), "天亮了");
        if (g->n_dead) {
            char names[64] = "";
            size_t n = 0;
            for (int k = 0; k < g->n_dead && n < sizeof(names); k++) {
                n += (size_t)snprintf(names + n, sizeof(names) - n, "%s%d号", k ? "、" : "",
                                      g->dead_tonight[k]);
            }
            fmt(v->line1, sizeof(v->line1), "昨晚 %s 死亡", names);
        } else {
            fmt(v->line1, sizeof(v->line1), "昨晚是平安夜");
        }
        v->timer = left;
        v->timer_down = true;
        fmt(v->hint, sizeof(v->hint), "确定=跳过 双击=状态 长按=菜单");
        break;
    case WW_PH_DISCUSS: {
        fmt(v->title, sizeof(v->title), "第%d天", g->round);
        int cur = g->n_order ? g->order[g->speaker] : 0;
        fmt(v->big, sizeof(v->big), "%d号 发言", cur);
        if (g->busy == WW_BUSY_REC) fmt(v->big, sizeof(v->big), "%d号 录音中", g->busy_seat);
        else if (g->busy == WW_BUSY_ASR) fmt(v->big, sizeof(v->big), "转文字中…");
        else if (g->busy == WW_BUSY_THINK) fmt(v->big, sizeof(v->big), "%d号AI 思考中", g->busy_seat);
        else if (g->busy == WW_BUSY_SPEAK) fmt(v->big, sizeof(v->big), "%d号AI 发言中", g->busy_seat);
        else if (g->busy == WW_BUSY_CONNECT) fmt(v->big, sizeof(v->big), "准备录音…");
        size_t n = (size_t)snprintf(v->line1, sizeof(v->line1), "顺序");
        for (int k = 0; k < g->n_order && n < sizeof(v->line1); k++) {
            n += (size_t)snprintf(v->line1 + n, sizeof(v->line1) - n, " %d", g->order[k]);
        }
        fmt(v->line2, sizeof(v->line2), "第%d位/共%d位 · %s", g->speaker + 1, g->n_order,
            g->round % 2 ? "小号→大号" : "大号→小号");
        v->timer = (int)((now - g->speaker_since) / 1000u);
        v->timer_down = false;
        if (g->busy == WW_BUSY_REC) fmt(v->hint, sizeof(v->hint), "说完松手发送");
        else if (g->busy == WW_BUSY_CONNECT) fmt(v->hint, sizeof(v->hint), "按住别松，听到嘀再说");
        else if (ww_host_ai_ready(h) && cur && !ww_is_bot(g, cur))
            fmt(v->hint, sizeof(v->hint), "按住确定说话 上下=换人 长按上=菜单");
        else fmt(v->hint, sizeof(v->hint), "上下=换人 确定=投票 长按上=菜单");
        break;
    }
    case WW_PH_VOTE:
        fmt(v->title, sizeof(v->title), "第%d天", g->round);
        fmt(v->big, sizeof(v->big), "投票中");
        fmt(v->line1, sizeof(v->line1), "已投 %d / %d", ww_voted_count(g), ww_alive_count(g));
        fmt(v->line2, sizeof(v->line2), "在手机上投票，可以弃票");
        v->timer = el;
        v->timer_down = false;
        fmt(v->hint, sizeof(v->hint), "确定=结束投票 长按=菜单");
        break;
    case WW_PH_RESULT: {
        fmt(v->title, sizeof(v->title), "第%d天", g->round);
        if (g->out_seat) fmt(v->big, sizeof(v->big), "%d号 出局", g->out_seat);
        else fmt(v->big, sizeof(v->big), "无人出局");
        const char *last = ww_log_at(g, 0);
        // 公告形如"投票结束。计票：……",屏幕上去掉前缀
        const char *p = last ? strstr(last, "计票：") : NULL;
        fmt(v->line1, sizeof(v->line1), "%s", p ? p : (last ? last : ""));
        v->timer = left;
        v->timer_down = true;
        fmt(v->hint, sizeof(v->hint), "确定=跳过 双击=状态 长按=菜单");
        break;
    }
    case WW_PH_OVER: {
        fmt(v->title, sizeof(v->title), "游戏结束");
        fmt(v->big, sizeof(v->big), "%s", g->winner == WW_WIN_GOOD ? "好人胜利" : "狼人胜利");
        char wolves[64];
        role_list(g, WW_ROLE_WOLF, wolves, sizeof(wolves));
        fmt(v->line1, sizeof(v->line1), "狼人：%s", wolves);
        char seer[16], witch[16];
        role_list(g, WW_ROLE_SEER, seer, sizeof(seer));
        role_list(g, WW_ROLE_WITCH, witch, sizeof(witch));
        if (seer[0] && witch[0]) fmt(v->line2, sizeof(v->line2), "预言家 %s · 女巫 %s", seer, witch);
        else if (seer[0]) fmt(v->line2, sizeof(v->line2), "预言家 %s", seer);
        fmt(v->hint, sizeof(v->hint), "确定=再来一局 长按=菜单");
        break;
    }
    default:
        break;
    }
}

static void view_list(const ww_host_t *h, ww_view_t *v)
{
    v->kind = WWV_MENU;
    if (h->screen == WW_HS_KICK) {
        int seats[WW_MAX_SEATS + 1];
        int n = kick_list(h, seats);
        fmt(v->title, sizeof(v->title), "踢出玩家");
        for (int k = 0; k < n; k++) {
            if (!seats[k]) { fmt(v->items[k], sizeof(v->items[k]), "返回"); continue; }
            int pi = ww_by_seat(h->g, seats[k]);
            fmt(v->items[k], sizeof(v->items[k]), "%d号%s", seats[k],
                pi >= 0 && h->g->players[pi].bot ? " 机器人" : "");
        }
        v->n_items = (uint8_t)n;
    } else {
        item_t items[WWV_ITEMS_MAX];
        int n = h->screen == WW_HS_MENU ? main_menu(h, items) :
                h->screen == WW_HS_AI ? ai_menu(h, items) : net_menu(items);
        fmt(v->title, sizeof(v->title), "%s", h->screen == WW_HS_MENU ? "主持菜单" :
                                             h->screen == WW_HS_AI ? "AI 设置" : "网络设置");
        if (h->screen == WW_HS_AI) {
            fmt(v->info, sizeof(v->info), "千问Key:%s  DeepSeek:%s", h->key_ds ? "已配" : "未配",
                h->key_dk ? "已配" : "未配");
            fmt(v->line1, sizeof(v->line1), "%s", h->ai_msg);
        }
        for (int k = 0; k < n; k++) fmt(v->items[k], sizeof(v->items[k]), "%s", items[k].label);
        v->n_items = (uint8_t)n;
        if (h->screen == WW_HS_NET) {
            static const char *const MODE[] = {"启动中", "连接中", "热点", "家里Wi-Fi", "配网"};
            fmt(v->info, sizeof(v->info), "当前：%s %s", MODE[h->net.mode],
                is_ascii(h->net.ssid) ? h->net.ssid : "");
        }
    }
    int sel = h->sel < v->n_items ? h->sel : v->n_items - 1;
    v->sel = (int8_t)sel;
    v->top = (int8_t)(sel < WWV_MENU_ROWS ? 0 : sel - WWV_MENU_ROWS + 1);
    fmt(v->hint, sizeof(v->hint), "上下=选择 确定=执行 长按=返回");
}

static void view_status(const ww_host_t *h, uint32_t now, ww_view_t *v)
{
    const ww_game_t *g = h->g;
    static const char *const PH[] = {
        "大厅", "发身份", "天黑", "狼人", "女巫", "预言家", "天亮", "讨论", "投票", "放逐", "结束",
    };
    char url[40];
    v->kind = WWV_STATUS;
    fmt(v->title, sizeof(v->title), "状态");
    int n = 0;
    if (g->phase == WW_PH_LOBBY) fmt(v->lines[n++], 64, "大厅 · 座位 %d", g->seat_count);
    else fmt(v->lines[n++], 64, "第%d轮 %s · 存活 %d/%d", g->round, PH[g->phase],
             ww_alive_count(g), g->seat_count);
    static const char *const MODE[] = {"启动中", "连接中", "热点", "家里Wi-Fi", "配网"};
    fmt(v->lines[n++], 64, "网络：%s %s", MODE[h->net.mode], is_ascii(h->net.ssid) ? h->net.ssid : "");
    fmt(v->lines[n++], 64, "地址：%s", join_url(h, url, sizeof(url)));
    for (int s = 1; s <= g->seat_count && n < WWV_LINES_MAX; s += 2) {
        char a[32], b[32] = "";
        for (int k = 0; k < 2 && s + k <= g->seat_count; k++) {
            int seat = s + k;
            int i = ww_by_seat(g, seat);
            char *dst = k ? b : a;
            if (i < 0) fmt(dst, 32, "%2d号 空位    ", seat);
            else fmt(dst, 32, "%2d号 %s %s", seat, ww_is_online(g, i, now) ? "在线" : "离线",
                     g->phase == WW_PH_LOBBY ? "    " : g->players[i].alive ? "存活" : "出局");
        }
        fmt(v->lines[n++], 64, "%s  %s", a, b);
    }
    v->n_lines = (uint8_t)n;
    fill_seats(h, now, v);
    fmt(v->hint, sizeof(v->hint), "按任意键返回");
}

void ww_host_view(const ww_host_t *h, uint32_t now, ww_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->timer = -1;
    v->volume = h->volume;

    switch (h->screen) {
    case WW_HS_MENU:
    case WW_HS_KICK:
    case WW_HS_NET:
    case WW_HS_AI:
        view_list(h, v);
        break;
    case WW_HS_KEYS: {
        char url[40];
        v->kind = WWV_SETUP;
        fmt(v->title, sizeof(v->title), "填写 Key");
        fmt(v->qr, sizeof(v->qr), "%sai", join_url(h, url, sizeof(url)));
        fmt(v->qr_label, sizeof(v->qr_label), "手机扫码填写 AI Key");
        fmt(v->qr_label2, sizeof(v->qr_label2), "%sai", url);
        fmt(v->info, sizeof(v->info), "千问:%s  DeepSeek:%s", h->key_ds ? "已配" : "未配", h->key_dk ? "已配" : "未配");
        fmt(v->hint, sizeof(v->hint), "填好后按任意键返回");
        break;
    }
    case WW_HS_STATUS:
        view_status(h, now, v);
        break;
    case WW_HS_VOLUME:
        v->kind = WWV_VOLUME;
        fmt(v->title, sizeof(v->title), "音量");
        fmt(v->big, sizeof(v->big), "音量 %d%%", h->volume);
        fmt(v->hint, sizeof(v->hint), "上下=调节 确定=返回");
        break;
    default:
        if (h->net.mode == WW_NET_BOOT || h->net.mode == WW_NET_CONNECTING) {
            v->kind = WWV_BOOT;
            fmt(v->title, sizeof(v->title), "狼人杀");
            fmt(v->big, sizeof(v->big), "%s", h->net.mode == WW_NET_BOOT ? "启动中" : "连接Wi-Fi");
            fmt(v->line1, sizeof(v->line1), "%s", is_ascii(h->net.ssid) ? h->net.ssid : "");
            fmt(v->line2, sizeof(v->line2), "%s", h->net.note);
            fmt(v->hint, sizeof(v->hint), "请稍候");
        } else if (h->net.mode == WW_NET_SETUP) {
            view_setup(h, v);
        } else if (h->g->phase == WW_PH_LOBBY) {
            view_lobby(h, now, v);
        } else {
            view_game(h, now, v);
        }
        break;
    }

    // 底栏:待确认 > 临时提示 > 常规操作说明
    if (h->confirm != CA_NONE && before(now, h->confirm_until)) {
        int s = (int)((h->confirm_until - now + 999u) / 1000u);
        fmt(v->hint, sizeof(v->hint), "再按确定：%s（%d）", confirm_label(h->confirm), s);
        v->alert = true;
    } else if (before(now, h->toast_until)) {
        fmt(v->hint, sizeof(v->hint), "%s", h->toast);
        v->alert = true;
    } else if (h->net.note[0] && v->kind == WWV_LOBBY && before(now, h->note_until)) {
        // 网络提示(比如回落到热点)在大厅底栏显示一会儿,之后恢复操作说明
        fmt(v->hint, sizeof(v->hint), "%s", h->net.note);
        v->alert = true;
    }
}

// ---------------------------------------------------------------------------
// 视图 → JSON
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t cap, len;
    bool bad;
} vw_t;

static void vw_raw(vw_t *w, const char *s, size_t n)
{
    if (w->bad || w->len + n + 1 > w->cap) { w->bad = true; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void vw_fmt(vw_t *w, const char *f, ...)
{
    if (w->bad) return;
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, f, ap);
    va_end(ap);
    if (n < 0 || w->len + (size_t)n + 1 > w->cap) { w->bad = true; return; }
    w->len += (size_t)n;
}

static void vw_str(vw_t *w, const char *key, const char *s)
{
    vw_fmt(w, "\"%s\":\"", key);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; vw_raw(w, e, 2); }
        else if (c < 0x20) vw_fmt(w, "\\u%04x", c);
        else vw_raw(w, s, 1);
    }
    vw_raw(w, "\",", 2);
}

size_t ww_view_json(const ww_view_t *v, char *buf, size_t cap)
{
    static const char *const KIND[] = {"boot", "lobby", "setup", "game", "menu", "status", "volume"};
    vw_t w = {buf, cap, 0, false};
    vw_raw(&w, "{", 1);
    vw_str(&w, "kind", KIND[v->kind]);
    vw_str(&w, "title", v->title);
    vw_str(&w, "qr", v->qr);
    vw_str(&w, "qr_label", v->qr_label);
    vw_str(&w, "qr_label2", v->qr_label2);
    vw_str(&w, "info", v->info);
    vw_str(&w, "big", v->big);
    vw_str(&w, "line1", v->line1);
    vw_str(&w, "line2", v->line2);
    vw_str(&w, "hint", v->hint);
    vw_fmt(&w, "\"night\":%d,\"alert\":%d,\"timer\":%d,\"timer_down\":%d,\"sel\":%d,\"top\":%d,"
               "\"volume\":%d,\"seats\":[",
           v->night, v->alert, v->timer, v->timer_down, v->sel, v->top, v->volume);
    for (int k = 0; k < v->n_seats; k++) vw_fmt(&w, "%s%d", k ? "," : "", v->seat[k]);
    vw_raw(&w, "],\"items\":[", 11);
    for (int k = 0; k < v->n_items; k++) {
        vw_fmt(&w, "%s\"%s\"", k ? "," : "", v->items[k]);
    }
    vw_raw(&w, "],\"lines\":[", 11);
    for (int k = 0; k < v->n_lines; k++) {
        vw_fmt(&w, "%s\"%s\"", k ? "," : "", v->lines[k]);
    }
    vw_raw(&w, "]}", 2);
    return w.bad ? 0 : w.len;
}
