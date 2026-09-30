// main/ww_api.h —— 玩家网页 HTTP 接口的"协议层"(纯 C)。
//
// 固件的 esp_http_server 和主机仿真服务器 sim/server.c 都只负责收发字节,
// 解析参数、调用 ww_core、拼响应全在这里,两边行为一定一致。
//
//   GET  /api/state?t=<token>&v=<版本>   200 JSON / 204 没变化 / 401 token 无效
//   POST /api/join   name=…              200 {"token":…} / 400 {"err":…}
//   POST /api/act    t=…&a=<动作>&…      200 {"ok":1} / 400 {"err":…} / 401
//
// 动作 a:seat(seat) unseat rename(name) leave wolf(target) witch(do,target)
//         seer(target) startvote vote(target)
#pragma once

#include "ww_core.h"

typedef struct {
    int status;               // HTTP 状态码;0 = 不是本模块管的路径
    const char *ctype;        // Content-Type
    size_t len;               // 响应体长度
} ww_api_resp_t;

// path 不含查询串;query 是 GET 的查询串或 POST 的表单体(可为 NULL)。
// 响应体写进 out。调用方负责加锁。
ww_api_resp_t ww_api_handle(ww_game_t *g, bool post, const char *path, const char *query,
                            uint32_t now, char *out, size_t cap);

// 从 x-www-form-urlencoded 串里取一个键并 URL 解码。找到返回 true。
bool ww_form_get(const char *form, const char *key, char *out, size_t cap);
