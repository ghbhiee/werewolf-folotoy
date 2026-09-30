// main/ww_api.c —— 玩家网页 HTTP 接口协议层,见 ww_api.h。
#include "ww_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ww_form_get(const char *form, const char *key, char *out, size_t cap)
{
    if (!form || !cap) return false;
    size_t klen = strlen(key);
    const char *p = form;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = p + seg;
            size_t n = 0;
            while (v < end && n + 1 < cap) {
                if (*v == '+') { out[n++] = ' '; v++; }
                else if (*v == '%' && end - v >= 3 && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[n++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else {
                    out[n++] = *v++;
                }
            }
            out[n] = '\0';
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static int form_int(const char *form, const char *key, int dflt)
{
    char buf[12];
    if (!ww_form_get(form, key, buf, sizeof(buf)) || !buf[0]) return dflt;
    return atoi(buf);
}

static ww_api_resp_t reply(int status, char *out, size_t cap, const char *body)
{
    ww_api_resp_t r = {status, "application/json; charset=utf-8", 0};
    int n = snprintf(out, cap, "%s", body);
    r.len = (n < 0) ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
    return r;
}

static ww_api_resp_t reply_err(int status, char *out, size_t cap, int code)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"err\":\"%s\"}", ww_err_str(code));
    return reply(status, out, cap, body);
}

typedef struct {
    const char *name;
    ww_act_t act;
} act_map_t;

static const act_map_t ACTS[] = {
    {"seat", WW_ACT_SEAT},       {"unseat", WW_ACT_UNSEAT}, {"rename", WW_ACT_RENAME},
    {"leave", WW_ACT_LEAVE},     {"wolf", WW_ACT_WOLF},     {"witch", WW_ACT_WITCH},
    {"seer", WW_ACT_SEER},       {"startvote", WW_ACT_START_VOTE}, {"vote", WW_ACT_VOTE},
};

ww_api_resp_t ww_api_handle(ww_game_t *g, bool post, const char *path, const char *query,
                            uint32_t now, char *out, size_t cap)
{
    ww_api_resp_t none = {0, NULL, 0};
    char tok[WW_TOKEN_LEN + 2];

    if (strcmp(path, "/api/state") == 0) {
        if (!ww_form_get(query, "t", tok, sizeof(tok))) tok[0] = '\0';
        int pidx = ww_find(g, tok);
        if (pidx < 0) return reply_err(401, out, cap, WW_E_TOKEN);
        ww_touch(g, pidx, now);
        char vbuf[12];
        if (ww_form_get(query, "v", vbuf, sizeof(vbuf)) &&
            strtoul(vbuf, NULL, 10) == g->version) {
            ww_api_resp_t r = {204, "application/json", 0};
            if (cap) out[0] = '\0';
            return r;
        }
        ww_api_resp_t r = {200, "application/json; charset=utf-8", 0};
        r.len = ww_state_json(g, pidx, now, out, cap);
        if (!r.len) return reply(500, out, cap, "{\"err\":\"状态太大\"}");
        return r;
    }

    if (!post) return none;

    if (strcmp(path, "/api/join") == 0) {
        char name[WW_NAME_MAX * 2];
        if (!ww_form_get(query, "name", name, sizeof(name))) name[0] = '\0';
        char token[WW_TOKEN_LEN + 1];
        int pidx = ww_join(g, name, now, token);
        if (pidx < 0) return reply_err(400, out, cap, pidx);
        char body[64];
        snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
        return reply(200, out, cap, body);
    }

    if (strcmp(path, "/api/act") == 0) {
        if (!ww_form_get(query, "t", tok, sizeof(tok))) tok[0] = '\0';
        int pidx = ww_find(g, tok);
        if (pidx < 0) return reply_err(401, out, cap, WW_E_TOKEN);
        char a[16];
        if (!ww_form_get(query, "a", a, sizeof(a))) a[0] = '\0';
        const act_map_t *m = NULL;
        for (size_t i = 0; i < sizeof(ACTS) / sizeof(ACTS[0]); i++) {
            if (strcmp(ACTS[i].name, a) == 0) m = &ACTS[i];
        }
        if (!m) return reply_err(400, out, cap, WW_E_ARG);
        int arg = 0, arg2 = 0;
        char name[WW_NAME_MAX * 2] = "";
        switch (m->act) {
        case WW_ACT_SEAT:   arg = form_int(query, "seat", 0); break;
        case WW_ACT_RENAME: ww_form_get(query, "name", name, sizeof(name)); break;
        case WW_ACT_WITCH:  arg = form_int(query, "do", -1); arg2 = form_int(query, "target", 0); break;
        case WW_ACT_WOLF:
        case WW_ACT_SEER:
        case WW_ACT_VOTE:   arg = form_int(query, "target", -1); break;
        default: break;
        }
        int rc = ww_act(g, pidx, m->act, arg, arg2, name, now);
        if (rc != WW_OK) return reply_err(400, out, cap, rc);
        return reply(200, out, cap, "{\"ok\":1}");
    }
    return none;
}
