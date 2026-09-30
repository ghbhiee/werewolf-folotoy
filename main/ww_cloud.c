// main/ww_cloud.c —— DeepSeek / 千问 TTS / 千问流式 ASR,见 ww_cloud.h。
//
// 协议与模型照 ~/cc/werewolf 线上配置(2026-09-30 在 Mac 上实测过这几种格式):
//   DeepSeek   POST https://api.deepseek.com/chat/completions,model deepseek-flash,
//              response_format=json_object,thinking=disabled(关思考约 1 秒,开了会慢几十倍)
//   千问 TTS   wss://dashscope.aliyuncs.com/api-ws/v1/inference,SpeechSynthesizer,
//              qwen-audio-3.1-tts-flash,format=pcm sample_rate=16000(首包约 0.5 秒,比实时快 3 倍)
//   千问 ASR   同一地址,recognition,qwen-audio-3.1-asr-flash-streaming,16 kHz PCM 二进制帧,
//              sentence_end=true 的 result-generated 是一句定稿
#include "ww_cloud.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "nvs.h"
#include "ww_ai.h"

static const char *TAG = "ww_cloud";

#define NVS_NS     "wwai"
#define DS_HOST    "dashscope.aliyuncs.com"
#define DS_PATH    "/api-ws/v1/inference"
#define TTS_MODEL  "qwen-audio-3.1-tts-flash"
#define ASR_MODEL  "qwen-audio-3.1-asr-flash-streaming"
#define CHAT_MODEL "deepseek-flash"

static char s_ds[128], s_dk[128];
static bool s_ai_on = true;

// 每个座位一个声音(线上狼人杀在用的 12 个 v3.1 音色)
static const char *const VOICES[] = {
    "xieshurou_v3.1", "anmingyuan_v3.1", "huozhuoshi_v3.1", "xunanchuan_v3.1",
    "yuxiaoyun_v3.1", "wenhuaiqing_v3.1", "anxiaolan_v3.1", "xiaxiaochen_v3.1",
    "qiaoxiaojiao_v3.1", "longanhuan_v3.1", "longanlingxin_v3.1", "longanfengyue_v3.1",
};

static void log_heap(const char *what)
{
    ESP_LOGI(TAG, "[%s] heap free %u largest %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------
void ww_cloud_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n1 = sizeof(s_ds), n2 = sizeof(s_dk);
    if (nvs_get_str(h, "ds", s_ds, &n1) != ESP_OK) s_ds[0] = '\0';
    if (nvs_get_str(h, "dk", s_dk, &n2) != ESP_OK) s_dk[0] = '\0';
    uint8_t on = 1;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) s_ai_on = on != 0;
    nvs_close(h);
    ESP_LOGI(TAG, "Key:千问 %s,DeepSeek %s,AI 玩家 %s", s_ds[0] ? "已配" : "未配",
             s_dk[0] ? "已配" : "未配", s_ai_on ? "开" : "关");
}

void ww_cloud_keys(bool *ds, bool *dk)
{
    *ds = s_ds[0] != '\0';
    *dk = s_dk[0] != '\0';
}

static bool key_ok(const char *k)
{
    size_t n = strlen(k);
    if (n < 16 || n >= sizeof(s_ds)) return false;
    for (size_t i = 0; i < n; i++) {
        if (k[i] <= ' ' || k[i] > '~') return false;    // Key 只会是可见 ASCII
    }
    return true;
}

bool ww_cloud_set_keys(const char *ds, const char *dk)
{
    if ((ds && ds[0] && !key_ok(ds)) || (dk && dk[0] && !key_ok(dk))) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    if (ds && ds[0]) ok &= nvs_set_str(h, "ds", ds) == ESP_OK;
    if (dk && dk[0]) ok &= nvs_set_str(h, "dk", dk) == ESP_OK;
    ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        if (ds && ds[0]) snprintf(s_ds, sizeof(s_ds), "%s", ds);
        if (dk && dk[0]) snprintf(s_dk, sizeof(s_dk), "%s", dk);
        ESP_LOGI(TAG, "Key 已更新(千问 %s,DeepSeek %s)", s_ds[0] ? "已配" : "未配", s_dk[0] ? "已配" : "未配");
    }
    return ok;
}

bool ww_cloud_ai_on(void) { return s_ai_on; }

void ww_cloud_set_ai_on(bool on)
{
    s_ai_on = on;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "on", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// DeepSeek
// ---------------------------------------------------------------------------
#define CHAT_RESP_MAX 4096

esp_err_t ww_cloud_chat(const char *sys, const char *user, char *out, size_t cap)
{
    if (!s_dk[0]) return ESP_ERR_INVALID_STATE;
    out[0] = '\0';
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CHAT_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 400);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "response_format"), "type", "json_object");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "thinking"), "type", "disabled");
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "system");
    cJSON_AddStringToObject(m1, "content", sys);
    cJSON_AddItemToArray(msgs, m1);
    cJSON *m2 = cJSON_CreateObject();
    cJSON_AddStringToObject(m2, "role", "user");
    cJSON_AddStringToObject(m2, "content", user);
    cJSON_AddItemToArray(msgs, m2);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1536,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); return ESP_ERR_NO_MEM; }
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_dk);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = ESP_FAIL;
    char *resp = NULL;
    size_t blen = strlen(body);
    log_heap("chat start");
    if (esp_http_client_open(c, (int)blen) != ESP_OK) goto done;
    if (esp_http_client_write(c, body, (int)blen) != (int)blen) goto done;
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    resp = malloc(CHAT_RESP_MAX);
    if (!resp) { err = ESP_ERR_NO_MEM; goto done; }
    int got = 0;
    for (;;) {
        int r = esp_http_client_read(c, resp + got, CHAT_RESP_MAX - 1 - got);
        if (r <= 0) break;
        got += r;
        if (got >= CHAT_RESP_MAX - 1) break;
    }
    resp[got] = '\0';
    log_heap("chat done");
    if (status != 200) {
        ESP_LOGW(TAG, "DeepSeek HTTP %d: %.120s", status, resp);
        goto done;
    }
    cJSON *j = cJSON_Parse(resp);
    cJSON *content = j ? cJSON_GetObjectItem(cJSON_GetObjectItem(
                             cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0), "message"), "content")
                       : NULL;
    if (cJSON_IsString(content)) {
        snprintf(out, cap, "%s", content->valuestring);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "DeepSeek 回复看不懂: %.120s", resp);
    }
    cJSON_Delete(j);
done:
    free(body);
    free(resp);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

// ---------------------------------------------------------------------------
// 百炼 WebSocket
// ---------------------------------------------------------------------------
typedef struct {
    esp_transport_handle_t ssl, ws;
} wsconn_t;

static bool ws_open(wsconn_t *w)
{
    w->ssl = esp_transport_ssl_init();
    w->ws = w->ssl ? esp_transport_ws_init(w->ssl) : NULL;
    if (!w->ws) goto fail;
    esp_transport_ssl_crt_bundle_attach(w->ssl, esp_crt_bundle_attach);
    esp_transport_ws_set_path(w->ws, DS_PATH);
    char hdr[180];
    snprintf(hdr, sizeof(hdr), "Authorization: bearer %s\r\n", s_ds);
    esp_transport_ws_set_headers(w->ws, hdr);
    if (esp_transport_connect(w->ws, DS_HOST, 443, 10000) < 0) {
        ESP_LOGW(TAG, "连不上百炼(%s)", DS_HOST);
        goto fail;
    }
    return true;
fail:
    if (w->ws) esp_transport_destroy(w->ws);
    if (w->ssl) esp_transport_destroy(w->ssl);
    w->ws = w->ssl = NULL;
    return false;
}

static void ws_close(wsconn_t *w)
{
    if (!w->ws) return;
    esp_transport_close(w->ws);
    esp_transport_destroy(w->ws);
    esp_transport_destroy(w->ssl);
    w->ws = w->ssl = NULL;
}

static bool ws_send_json(wsconn_t *w, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    if (!s) return false;
    int n = esp_transport_ws_send_raw(w->ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                      s, (int)strlen(s), 5000);
    bool ok = n == (int)strlen(s);
    free(s);
    return ok;
}

static void task_id(char out[33])
{
    for (int i = 0; i < 32; i += 8) snprintf(out + i, 9, "%08lx", (unsigned long)esp_random());
}

static cJSON *header(const char *action, const char *tid)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *h = cJSON_AddObjectToObject(j, "header");
    cJSON_AddStringToObject(h, "action", action);
    cJSON_AddStringToObject(h, "task_id", tid);
    cJSON_AddStringToObject(h, "streaming", "duplex");
    return j;
}

// 读一条完整消息:文本写进 txt(截断),二进制边读边交给 sink。
// 返回 opcode(TEXT/BINARY),0 = 超时/控制帧,-1 = 断了。
static int ws_read_msg(wsconn_t *w, char *txt, size_t cap, ww_pcm_sink_t sink, void *ctx, int tmo)
{
    static char buf[1024];
    static int last_op = WS_TRANSPORT_OPCODES_TEXT;
    int n = esp_transport_read(w->ws, buf, sizeof(buf), tmo);
    if (n < 0) return -1;
    if (n == 0) return 0;
    int op = esp_transport_ws_get_read_opcode(w->ws) & 0x0F;
    if (op == WS_TRANSPORT_OPCODES_CLOSE) return -1;
    if (op == WS_TRANSPORT_OPCODES_CONT) op = last_op;
    last_op = op;
    int total = esp_transport_ws_get_read_payload_len(w->ws);
    size_t tl = 0;
    int got = 0;
    for (;;) {
        if (op == WS_TRANSPORT_OPCODES_BINARY) {
            if (sink) sink((const uint8_t *)buf, (size_t)n, ctx);
        } else if (txt && cap) {
            size_t k = (size_t)n;
            if (tl + k >= cap) k = cap - 1 - tl;
            memcpy(txt + tl, buf, k);
            tl += k;
        }
        got += n;
        if (got >= total) break;
        n = esp_transport_read(w->ws, buf, sizeof(buf), 5000);
        if (n <= 0) return -1;
    }
    if (txt && cap) txt[tl] = '\0';
    return op;
}

static const char *event_of(cJSON *j)
{
    cJSON *e = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "header"), "event");
    return cJSON_IsString(e) ? e->valuestring : "";
}

static void log_failed(const char *what, cJSON *j)
{
    cJSON *h = cJSON_GetObjectItem(j, "header");
    cJSON *code = cJSON_GetObjectItem(h, "error_code");
    cJSON *msg = cJSON_GetObjectItem(h, "error_message");
    ESP_LOGW(TAG, "%s 失败:%s %s", what, cJSON_IsString(code) ? code->valuestring : "",
             cJSON_IsString(msg) ? msg->valuestring : "");
}

// ---------------------------------------------------------------------------
// TTS
// ---------------------------------------------------------------------------
esp_err_t ww_cloud_tts(const char *text, int voice, ww_pcm_sink_t sink, void *ctx)
{
    if (!s_ds[0]) return ESP_ERR_INVALID_STATE;
    wsconn_t w = {0};
    log_heap("tts start");
    if (!ws_open(&w)) return ESP_FAIL;
    char tid[33];
    task_id(tid);
    cJSON *run = header("run-task", tid);
    cJSON *pl = cJSON_AddObjectToObject(run, "payload");
    cJSON_AddStringToObject(pl, "task_group", "audio");
    cJSON_AddStringToObject(pl, "task", "tts");
    cJSON_AddStringToObject(pl, "function", "SpeechSynthesizer");
    cJSON_AddStringToObject(pl, "model", TTS_MODEL);
    cJSON *par = cJSON_AddObjectToObject(pl, "parameters");
    cJSON_AddStringToObject(par, "text_type", "PlainText");
    cJSON_AddStringToObject(par, "voice", VOICES[(voice < 0 ? 0 : voice) % 12]);
    cJSON_AddStringToObject(par, "format", "pcm");
    cJSON_AddNumberToObject(par, "sample_rate", 16000);
    cJSON_AddObjectToObject(pl, "input");
    bool ok = ws_send_json(&w, run);
    cJSON_Delete(run);

    esp_err_t err = ESP_FAIL;
    static char msg[1024];
    int idle = 0;
    for (int guard = 0; ok && guard < 5000; guard++) {
        int op = ws_read_msg(&w, msg, sizeof(msg), sink, ctx, 15000);
        if (op < 0) break;
        if (op == 0) {                  // 15 秒没动静(或只是 ping)两次就放弃
            if (++idle >= 2) break;
            continue;
        }
        idle = 0;
        if (op != WS_TRANSPORT_OPCODES_TEXT) continue;
        cJSON *j = cJSON_Parse(msg);
        const char *e = event_of(j);
        if (strcmp(e, "task-started") == 0) {
            cJSON *c1 = header("continue-task", tid);
            cJSON_AddStringToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(c1, "payload"), "input"),
                                    "text", text);
            cJSON *c2 = header("finish-task", tid);
            cJSON_AddObjectToObject(cJSON_AddObjectToObject(c2, "payload"), "input");
            ok = ws_send_json(&w, c1) && ws_send_json(&w, c2);
            cJSON_Delete(c1);
            cJSON_Delete(c2);
        } else if (strcmp(e, "task-finished") == 0) {
            err = ESP_OK;
            cJSON_Delete(j);
            break;
        } else if (strcmp(e, "task-failed") == 0) {
            log_failed("TTS", j);
            cJSON_Delete(j);
            break;
        }
        cJSON_Delete(j);
    }
    log_heap("tts done");
    ws_close(&w);
    return err;
}

// ---------------------------------------------------------------------------
// ASR
// ---------------------------------------------------------------------------
static wsconn_t s_asr;
static char s_asr_tid[33];
static char s_asr_text[600];
static size_t s_asr_len;
static bool s_asr_done;
static char s_asr_partial[WW_SPEECH_LEN];   // 当前这句还没定稿的中间结果(松手太快时服务端不会给定稿)
static char s_asr_msg[2048];                // ASR 消息只收开头 2 KB,见 asr_event

static void asr_append(const char *t)
{
    size_t k = strlen(t);
    if (!k || s_asr_len + k >= sizeof(s_asr_text)) return;
    memcpy(s_asr_text + s_asr_len, t, k);
    s_asr_len += k;
    s_asr_text[s_asr_len] = '\0';
}

// 处理一条 ASR 事件。定稿结果带逐字时间戳,一条就有好几 KB(5 秒一句约 3.7 KB),
// 这里只收得下开头 2 KB —— 好在 header.event、sentence.text、sentence_end 都排在
// words 数组前面,所以不做完整 JSON 解析,直接在开头里按字段取。
static void asr_event(const char *msg)
{
    char ev[32] = "";
    ww_ai_json_str(msg, "event", ev, sizeof(ev));
    if (strcmp(ev, "result-generated") == 0) {
        const char *sent = strstr(msg, "\"sentence\"");
        static char text[WW_SPEECH_LEN];
        if (!sent || !ww_ai_json_str(sent, "text", text, sizeof(text))) return;
        bool end = strstr(sent, "\"sentence_end\":true") || strstr(sent, "\"sentence_end\": true");
        if (end) {
            if (text[0]) ESP_LOGI(TAG, "ASR 定稿:%s", text);
            asr_append(text);
            s_asr_partial[0] = '\0';
        } else {
            // 中间结果是"这句到目前为止的全文",只留最新的一份
            snprintf(s_asr_partial, sizeof(s_asr_partial), "%s", text);
        }
    } else if (strcmp(ev, "task-finished") == 0) {
        // 最后一句没等到定稿(说完马上松手)就用最后一次中间结果,不能把人说的话丢了
        if (s_asr_partial[0]) {
            ESP_LOGI(TAG, "ASR 最后一句没定稿,用中间结果:%s", s_asr_partial);
            asr_append(s_asr_partial);
            s_asr_partial[0] = '\0';
        }
        ESP_LOGI(TAG, "ASR 结束,全文 %u 字节:%s", (unsigned)s_asr_len, s_asr_text);
        s_asr_done = true;
    } else if (strcmp(ev, "task-failed") == 0) {
        char m[120] = "";
        ww_ai_json_str(msg, "error_message", m, sizeof(m));
        ESP_LOGW(TAG, "ASR 失败:%s", m);
        s_asr_done = true;
    }
}

esp_err_t ww_asr_begin(void)
{
    if (!s_ds[0]) return ESP_ERR_INVALID_STATE;
    s_asr_len = 0;
    s_asr_text[0] = '\0';
    s_asr_partial[0] = '\0';
    s_asr_done = false;
    log_heap("asr start");
    if (!ws_open(&s_asr)) return ESP_FAIL;
    task_id(s_asr_tid);
    cJSON *run = header("run-task", s_asr_tid);
    cJSON *pl = cJSON_AddObjectToObject(run, "payload");
    cJSON_AddStringToObject(pl, "task_group", "audio");
    cJSON_AddStringToObject(pl, "task", "asr");
    cJSON_AddStringToObject(pl, "function", "recognition");
    cJSON_AddStringToObject(pl, "model", ASR_MODEL);
    cJSON_AddObjectToObject(pl, "input");
    cJSON *par = cJSON_AddObjectToObject(pl, "parameters");
    cJSON_AddStringToObject(par, "format", "pcm");
    cJSON_AddNumberToObject(par, "sample_rate", 16000);
    cJSON_AddItemToObject(par, "language_hints", cJSON_CreateStringArray((const char *[]){"zh"}, 1));
    bool ok = ws_send_json(&s_asr, run);
    cJSON_Delete(run);
    for (int i = 0; ok && i < 10; i++) {
        int op = ws_read_msg(&s_asr, s_asr_msg, sizeof(s_asr_msg), NULL, NULL, 1000);
        if (op < 0) break;
        if (op != WS_TRANSPORT_OPCODES_TEXT) continue;
        cJSON *j = cJSON_Parse(s_asr_msg);
        const char *e = event_of(j);
        bool started = strcmp(e, "task-started") == 0;
        if (strcmp(e, "task-failed") == 0) log_failed("ASR", j);
        cJSON_Delete(j);
        if (started) return ESP_OK;
        if (s_asr_done) break;
    }
    ws_close(&s_asr);
    return ESP_FAIL;
}

esp_err_t ww_asr_feed(void *pcm, size_t len)
{
    if (!s_asr.ws) return ESP_ERR_INVALID_STATE;
    int n = esp_transport_ws_send_raw(s_asr.ws, WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN,
                                      pcm, (int)len, 3000);
    if (n != (int)len) return ESP_FAIL;
    // 顺手把服务器发来的中间结果读掉,别让接收窗口堵死
    while (esp_transport_poll_read(s_asr.ws, 0) > 0) {
        int op = ws_read_msg(&s_asr, s_asr_msg, sizeof(s_asr_msg), NULL, NULL, 200);
        if (op < 0) return ESP_FAIL;
        if (op == WS_TRANSPORT_OPCODES_TEXT) asr_event(s_asr_msg);
    }
    return ESP_OK;
}

esp_err_t ww_asr_end(char *out, size_t cap)
{
    out[0] = '\0';
    if (!s_asr.ws) return ESP_ERR_INVALID_STATE;
    // 先补 0.8 秒静音:服务端靠静音判断"一句说完了",松手往往正好卡在句中
    // 注意:WebSocket 客户端会在原缓冲上做掩码(异或),所以这块必须在 RAM 里、每次重新清零;
    // 写成 static const 会被放进 flash,掩码一写就 Store access fault(真机踩过)。
    static int16_t zeros[800];
    for (int i = 0; i < 16; i++) {
        memset(zeros, 0, sizeof(zeros));
        if (ww_asr_feed(zeros, sizeof(zeros)) != ESP_OK) break;
    }
    cJSON *fin = header("finish-task", s_asr_tid);
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(fin, "payload"), "input");
    bool ok = ws_send_json(&s_asr, fin);
    cJSON_Delete(fin);
    for (int i = 0; ok && !s_asr_done && i < 200; i++) {
        int op = ws_read_msg(&s_asr, s_asr_msg, sizeof(s_asr_msg), NULL, NULL, 10000);
        if (op < 0) break;
        if (op == WS_TRANSPORT_OPCODES_TEXT) asr_event(s_asr_msg);
    }
    log_heap("asr done");
    ws_close(&s_asr);
    snprintf(out, cap, "%s", s_asr_text);
    return s_asr_done ? ESP_OK : ESP_FAIL;
}

void ww_asr_abort(void)
{
    ws_close(&s_asr);
}
