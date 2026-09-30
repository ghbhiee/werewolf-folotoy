// hostsim/server.c —— 在 Mac 上跑的"假 FoloToy":同一份 ww_core / ww_host / ww_api,
// 用 POSIX socket 提供和固件一样的 HTTP 接口,外加一个浏览器里的虚拟三键面板。
//
//   ./hostsim/build.sh && ./hostsim/out/server [端口] [时间倍速]
//
//   http://localhost:8080/          玩家网页(和固件里嵌的是同一个文件,每次从磁盘读)
//   http://localhost:8080/sim       虚拟 FoloToy(屏幕视图 + 上/下/确定按钮)+ 多个"手机"
//   POST /sim/key  k=up|down|ok g=click|double|long
//   POST /sim/net  mode=ap|sta|setup|connecting
//   GET  /sim/view                  屏幕视图模型 JSON
//   GET  /sim/cues                  最近的提示音事件
//
// 单线程 + poll(),每个请求处理完就关连接,够仿真用。
#define _GNU_SOURCE        // strcasestr:glibc 只在 _GNU_SOURCE 下声明(CI 跑在 Linux 上)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ww_api.h"
#include "ww_core.h"
#include "ww_host.h"

static ww_game_t G;
static ww_host_t H;
static double SPEED = 1.0;
static char ROOT[1024] = ".";
static uint64_t T0;
static char CUES[1024];     // 最近的提示音,给 /sim 页面显示

static const char *CUE_NAME[] = {
    "", "身份已发放", "天黑请闭眼", "狼人请睁眼", "狼人请闭眼", "女巫请睁眼", "女巫请闭眼",
    "预言家请睁眼", "预言家请闭眼", "天亮了·平安夜", "天亮了·有人死亡", "请按顺序发言",
    "开始投票", "投票结束", "好人胜利", "狼人胜利",
};

static uint64_t real_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static uint32_t now_ms(void)
{
    return (uint32_t)((double)(real_ms() - T0) * SPEED) + 1000u;
}

static char *read_file(const char *rel, size_t *len)
{
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s", ROOT, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k <= 0) {
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) { usleep(1000); continue; }
            return;
        }
        p += k;
        n -= (size_t)k;
    }
}

static void respond(int fd, int status, const char *ctype, const char *body, size_t len)
{
    const char *reason = status == 200 ? "OK" : status == 204 ? "No Content" :
                         status == 401 ? "Unauthorized" : status == 404 ? "Not Found" :
                         status == 400 ? "Bad Request" : "Error";
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, reason, ctype ? ctype : "text/plain", len);
    send_all(fd, head, (size_t)n);
    if (len) send_all(fd, body, len);
}

static void drain_cues(void)
{
    ww_cue_t c;
    while ((c = ww_pop_cue(&G)) != WW_CUE_NONE) {
        char line[64];
        snprintf(line, sizeof(line), "%s\n", CUE_NAME[c]);
        size_t have = strlen(CUES), add = strlen(line);
        if (have + add >= sizeof(CUES)) {
            char *cut = strchr(CUES + have / 2, '\n');
            if (cut) memmove(CUES, cut + 1, strlen(cut + 1) + 1);
        }
        strncat(CUES, line, sizeof(CUES) - strlen(CUES) - 1);
        printf("[cue] %s", line);
    }
}

static void handle(int fd, char *req, size_t len)
{
    (void)len;
    static char out[16384];
    char *sp1 = strchr(req, ' ');
    if (!sp1) return;
    *sp1 = '\0';
    bool post = strcmp(req, "POST") == 0;
    char *path = sp1 + 1;
    char *sp2 = strchr(path, ' ');
    if (!sp2) return;
    *sp2 = '\0';
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';
    char *body = strstr(sp2 + 1, "\r\n\r\n");
    body = body ? body + 4 : NULL;
    const char *form = post ? body : query;
    uint32_t now = now_ms();

    ww_api_resp_t r = ww_api_handle(&G, post, path, form, now, out, sizeof(out));
    if (r.status) {
        ww_tick(&G, now);
        respond(fd, r.status, r.ctype, out, r.len);
        return;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0 ||
        strcmp(path, "/setup") == 0 || strcmp(path, "/sim") == 0) {
        const char *file = strcmp(path, "/setup") == 0 ? "web/setup.html" :
                           strcmp(path, "/sim") == 0 ? "hostsim/sim.html" : "web/player.html";
        size_t n = 0;
        char *html = read_file(file, &n);
        if (!html) { respond(fd, 404, NULL, "no file", 7); return; }
        respond(fd, 200, "text/html; charset=utf-8", html, n);
        free(html);
        return;
    }
    if (strcmp(path, "/sim/view") == 0) {
        static ww_view_t v;
        ww_host_view(&H, now, &v);
        size_t n = ww_view_json(&v, out, sizeof(out));
        respond(fd, 200, "application/json; charset=utf-8", out, n);
        return;
    }
    if (strcmp(path, "/sim/cues") == 0) {
        respond(fd, 200, "text/plain; charset=utf-8", CUES, strlen(CUES));
        return;
    }
    if (post && strcmp(path, "/sim/key") == 0) {
        char k[8] = "", g[8] = "click";
        ww_form_get(form, "k", k, sizeof(k));
        ww_form_get(form, "g", g, sizeof(g));
        ww_key_t key = strcmp(k, "up") == 0 ? WW_KEY_UP : strcmp(k, "down") == 0 ? WW_KEY_DOWN : WW_KEY_OK;
        ww_gesture_t ges = strcmp(g, "double") == 0 ? WW_GES_DOUBLE :
                           strcmp(g, "long") == 0 ? WW_GES_LONG : WW_GES_CLICK;
        ww_host_key(&H, key, ges, now);
        ww_tick(&G, now);
        ww_req_t rq = ww_host_take_request(&H);
        if (rq == WW_REQ_NET_SETUP) { H.net.mode = WW_NET_SETUP; H.qr_page = 0; }
        else if (rq == WW_REQ_NET_AP || rq == WW_REQ_NET_FORGET) H.net.mode = WW_NET_AP;
        respond(fd, 200, "application/json", "{\"ok\":1}", 8);
        return;
    }
    if (post && strcmp(path, "/sim/net") == 0) {
        char m[16] = "";
        ww_form_get(form, "mode", m, sizeof(m));
        if (strcmp(m, "sta") == 0) {
            H.net.mode = WW_NET_STA;
            snprintf(H.net.ssid, sizeof(H.net.ssid), "HomeWiFi");
            H.net.note[0] = '\0';
        } else if (strcmp(m, "setup") == 0) {
            H.net.mode = WW_NET_SETUP;
        } else if (strcmp(m, "connecting") == 0) {
            H.net.mode = WW_NET_CONNECTING;
        } else {
            H.net.mode = WW_NET_AP;
            snprintf(H.net.ssid, sizeof(H.net.ssid), "Werewolf-A1B2");
        }
        respond(fd, 200, "application/json", "{\"ok\":1}", 8);
        return;
    }
    if (post && strcmp(path, "/api/wifi") == 0) {
        char ssid[40] = "", pass[70] = "";
        ww_form_get(form, "ssid", ssid, sizeof(ssid));
        ww_form_get(form, "pass", pass, sizeof(pass));
        printf("[wifi] 仿真:收到家里 Wi-Fi 配置 ssid=%s (密码 %zu 位),真机会写 NVS 并重启\n",
               ssid, strlen(pass));
        respond(fd, 200, "application/json", "{\"ok\":1}", 8);
        return;
    }
    respond(fd, 404, "text/plain", "not found", 9);
}

typedef struct {
    int fd;
    char buf[8192];
    size_t len;
    uint64_t since;
} conn_t;

#define MAX_CONN 64

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    SPEED = argc > 2 ? atof(argv[2]) : 1.0;
    if (SPEED <= 0) SPEED = 1.0;
    const char *root = getenv("WW_ROOT");
    if (root) snprintf(ROOT, sizeof(ROOT), "%s", root);
    signal(SIGPIPE, SIG_IGN);

    T0 = real_ms();
    ww_init(&G, (uint32_t)time(NULL));
    ww_host_init(&H, &G);
    H.net.mode = WW_NET_AP;
    snprintf(H.net.ssid, sizeof(H.net.ssid), "Werewolf-A1B2");
    snprintf(H.net.pass, sizeof(H.net.pass), "wolf1234");
    snprintf(H.net.ip, sizeof(H.net.ip), "%s", getenv("WW_IP") ? getenv("WW_IP") : "127.0.0.1");
    if (getenv("WW_SEATS")) ww_set_seats(&G, atoi(getenv("WW_SEATS")));

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(getenv("WW_LAN") ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 64) < 0) {
        perror("bind/listen");
        return 1;
    }
    fcntl(ls, F_SETFL, O_NONBLOCK);
    printf("狼人杀仿真服务器:http://localhost:%d/sim (玩家页 /,倍速 %.1fx)\n", port, SPEED);
    fflush(stdout);

    static conn_t conns[MAX_CONN];
    int nconn = 0;
    for (;;) {
        struct pollfd pfd[MAX_CONN + 1];
        pfd[0].fd = ls;
        pfd[0].events = POLLIN;
        for (int i = 0; i < nconn; i++) { pfd[i + 1].fd = conns[i].fd; pfd[i + 1].events = POLLIN; }
        poll(pfd, (nfds_t)nconn + 1, 50);

        if ((pfd[0].revents & POLLIN) && nconn < MAX_CONN) {
            int c;
            while (nconn < MAX_CONN && (c = accept(ls, NULL, NULL)) >= 0) {
                fcntl(c, F_SETFL, O_NONBLOCK);
                conns[nconn].fd = c;
                conns[nconn].len = 0;
                conns[nconn].since = real_ms();
                nconn++;
            }
        }
        for (int i = 0; i < nconn; i++) {
            conn_t *c = &conns[i];
            bool done = false;
            ssize_t k = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);
            if (k > 0) {
                c->len += (size_t)k;
                c->buf[c->len] = '\0';
                char *hdr_end = strstr(c->buf, "\r\n\r\n");
                if (hdr_end) {
                    size_t need = 0;
                    char *cl = strcasestr(c->buf, "content-length:");
                    if (cl && cl < hdr_end) need = (size_t)atol(cl + 15);
                    if (c->len >= (size_t)(hdr_end + 4 - c->buf) + need) {
                        handle(c->fd, c->buf, c->len);
                        done = true;
                    }
                }
                if (c->len >= sizeof(c->buf) - 1) done = true;
            } else if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                done = true;
            }
            if (real_ms() - c->since > 5000) done = true;
            if (done) {
                close(c->fd);
                conns[i] = conns[--nconn];
                i--;
            }
        }
        ww_tick(&G, now_ms());
        drain_cues();
        fflush(stdout);
    }
}
