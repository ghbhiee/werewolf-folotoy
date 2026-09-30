// main/ww_http.c —— 板上 HTTP 服务器,见 ww_http.h。
//
// 内存:最多 12 个 socket(LWIP_MAX_SOCKETS=16,httpd 自己占 3 个),满了按
// LRU 踢掉最久没用的 keep-alive 连接 —— 玩家端是 1 秒短轮询,被踢了下一秒自动
// 重连,无感。所有缓冲都是静态的,只有 httpd 这一个任务在用。
#include "ww_http.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "ww_api.h"
#include "ww_app.h"
#include "ww_cloud.h"
#include "ww_net.h"

static const char *TAG = "ww_http";

extern const uint8_t player_html_gz_start[] asm("_binary_player_html_gz_start");
extern const uint8_t player_html_gz_end[] asm("_binary_player_html_gz_end");
extern const uint8_t setup_html_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[] asm("_binary_setup_html_gz_end");
extern const uint8_t keys_html_gz_start[] asm("_binary_keys_html_gz_start");
extern const uint8_t keys_html_gz_end[] asm("_binary_keys_html_gz_end");

static httpd_handle_t s_server;
static char s_out[8192];       // 响应体(状态 JSON 带发言记录最坏约 6 KB)
static char s_in[1024];        // 请求体 / 查询串

static esp_err_t send_gz(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return send_gz(req, player_html_gz_start, player_html_gz_end);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    if (!ww_net_in_setup()) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req, "<meta charset=utf-8><p style='font:18px sans-serif;padding:20px'>"
                                       "要先在 FoloToy 上长按确定 → 网络设置 → 配置家里 Wi-Fi。</p>");
    }
    return send_gz(req, setup_html_gz_start, setup_html_gz_end);
}

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    default:  return "500 Internal Server Error";
    }
}

// 读 POST 体到 s_in。超过上限直接拒绝(不截断:剩下半截留在连接里会被当成
// 下一个请求),返回 false 时 httpd 会关掉这个连接。
static bool read_body(httpd_req_t *req)
{
    size_t want = req->content_len;
    if (want >= sizeof(s_in)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return false;
    }
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, s_in + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        got += (size_t)r;
    }
    s_in[got] = '\0';
    return true;
}

static esp_err_t h_api(httpd_req_t *req)
{
    bool post = req->method == HTTP_POST;
    s_in[0] = '\0';
    if (post) {
        if (!read_body(req)) return ESP_FAIL;
    } else if (httpd_req_get_url_query_str(req, s_in, sizeof(s_in)) != ESP_OK) {
        s_in[0] = '\0';
    }
    // 路径去掉查询串
    char path[32];
    size_t n = strcspn(req->uri, "?");
    if (n >= sizeof(path)) n = sizeof(path) - 1;
    memcpy(path, req->uri, n);
    path[n] = '\0';

    ww_app_lock();
    ww_game_t *g = ww_app_game();
    ww_reseed(g, esp_random());
    ww_api_resp_t r = ww_api_handle(g, post, path, s_in, ww_app_now(), s_out, sizeof(s_out));
    ww_app_unlock();

    if (!r.status) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_status(req, status_line(r.status));
    httpd_resp_set_type(req, r.ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_out, (ssize_t)r.len);
}

static esp_err_t h_keys_page(httpd_req_t *req)
{
    return send_gz(req, keys_html_gz_start, keys_html_gz_end);
}

// GET /api/keys:只回"配没配",绝不回 Key 本身
static esp_err_t h_keys_get(httpd_req_t *req)
{
    bool ds, dk;
    ww_cloud_keys(&ds, &dk);
    ww_app_lock();
    bool open = ww_app_host()->screen == WW_HS_KEYS;
    ww_app_unlock();
    char body[48];
    snprintf(body, sizeof(body), "{\"ds\":%d,\"dk\":%d,\"open\":%d}", ds, dk, open);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

// POST /api/keys:只有 FoloToy 正显示"填写 Key"页时才收(防止局中被别人改)
static esp_err_t h_keys_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    ww_app_lock();
    bool open = ww_app_host()->screen == WW_HS_KEYS;
    ww_app_unlock();
    if (!open) return httpd_resp_sendstr(req, "{\"err\":\"FoloToy 上没打开「填写 Key」页面\"}");
    if (!read_body(req)) return ESP_FAIL;
    static char ds[128], dk[128];
    if (!ww_form_get(s_in, "ds", ds, sizeof(ds))) ds[0] = '\0';
    if (!ww_form_get(s_in, "dk", dk, sizeof(dk))) dk[0] = '\0';
    bool ok = ww_cloud_set_keys(ds, dk);
    memset(ds, 0, sizeof(ds));
    memset(dk, 0, sizeof(dk));
    memset(s_in, 0, sizeof(s_in));
    return httpd_resp_sendstr(req, ok ? "{\"ok\":1}" : "{\"err\":\"Key 格式不对（应是 sk- 开头的一串字母数字）\"}");
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (!ww_net_in_setup()) return httpd_resp_sendstr(req, "{\"err\":\"不在配网模式\"}");
    if (!read_body(req)) return ESP_FAIL;
    char ssid[40], pass[70];
    if (!ww_form_get(s_in, "ssid", ssid, sizeof(ssid))) ssid[0] = '\0';
    if (!ww_form_get(s_in, "pass", pass, sizeof(pass))) pass[0] = '\0';
    if (!ww_net_save_and_reboot(ssid, pass)) return httpd_resp_sendstr(req, "{\"err\":\"保存失败\"}");
    return httpd_resp_sendstr(req, "{\"ok\":1}");
}

esp_err_t ww_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 12;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/setup", .method = HTTP_GET, .handler = h_setup},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/ai", .method = HTTP_GET, .handler = h_keys_page},
        {.uri = "/api/keys", .method = HTTP_GET, .handler = h_keys_get},
        {.uri = "/api/keys", .method = HTTP_POST, .handler = h_keys_post},
        {.uri = "/api/*", .method = HTTP_GET, .handler = h_api},
        {.uri = "/api/*", .method = HTTP_POST, .handler = h_api},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "HTTP 服务已启动(端口 80,最多 %d 个连接),heap free %u largest %u",
             cfg.max_open_sockets, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
}
