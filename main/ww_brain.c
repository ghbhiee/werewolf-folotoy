// main/ww_brain.c —— AI 大脑任务,见 ww_brain.h。
//
// 锁的顺序:只在很短的时间里拿游戏锁(出题/落地/改状态),联网和放声音时一律不持游戏锁,
// 否则手机轮询和按键都会卡住。放声音/录音时持 I2S 锁(ww_sound_lock)。
#include "ww_brain.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ww_ai.h"
#include "ww_app.h"
#include "ww_cloud.h"
#include "ww_sound.h"

static const char *TAG = "ww_brain";

#define REC_MAX_MS   60000
#define REC_CHUNK    3200        // 100 ms 的 16 kHz s16 单声道

static TaskHandle_t s_task;
static volatile bool s_want_test, s_want_ptt, s_ptt_up;

// 大缓冲都静态放着,只有这个任务用
static char s_sys[5120];
static char s_user[768];
static char s_reply[1024];
static char s_speech[WW_SPEECH_LEN];
static int16_t s_pcm[REC_CHUNK / 2];

void ww_brain_ptt(int req)
{
    if (req == 1) {
        s_ptt_up = false;
        s_want_ptt = true;
    } else if (req == 2) {
        s_ptt_up = true;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

void ww_brain_test(void)
{
    s_want_test = true;
    if (s_task) xTaskNotifyGive(s_task);
}

// TTS 的 PCM 可能在奇数字节处切开:留一个字节等下一块
typedef struct {
    uint8_t carry;
    bool has_carry;
    uint32_t bytes;
} sink_ctx_t;

static void play_sink(const uint8_t *pcm, size_t len, void *arg)
{
    sink_ctx_t *c = arg;
    c->bytes += (uint32_t)len;
    if (c->has_carry && len) {
        uint8_t two[2] = {c->carry, pcm[0]};
        bsp_audio_write(two, 2);
        pcm++;
        len--;
        c->has_carry = false;
    }
    if (len & 1) {
        c->carry = pcm[len - 1];
        c->has_carry = true;
        len--;
    }
    if (len) bsp_audio_write(pcm, len);
}

static void flush_silence(void)
{
    // 尾巴垫一点静音,免得 I2S DMA 把最后一块循环播成"嗡"
    memset(s_pcm, 0, sizeof(s_pcm));
    bsp_audio_write(s_pcm, 1024);
}

static esp_err_t say(const char *text, int voice)
{
    if (!ww_sound_lock(30000)) return ESP_ERR_TIMEOUT;
    bsp_audio_set_format(16000, 16, 1);
    sink_ctx_t ctx = {0};
    esp_err_t err = ww_cloud_tts(text, voice, play_sink, &ctx);
    flush_silence();
    ww_sound_unlock();
    ESP_LOGI(TAG, "TTS %s:%.1f 秒音频", err == ESP_OK ? "完成" : "失败", ctx.bytes / 32000.0);
    return err;
}

static void set_msg(const char *m)
{
    ww_app_lock();
    snprintf(ww_app_host()->ai_msg, sizeof(ww_app_host()->ai_msg), "%s", m);
    ww_app_unlock();
}

// ---------------------------------------------------------------------------
// 测试:DeepSeek 写一句 → 千问读出来
// ---------------------------------------------------------------------------
static void run_test(void)
{
    uint32_t t0 = ww_app_now();
    esp_err_t e = ww_cloud_chat("你是狼人杀主持人。你必须只输出一个 JSON 对象。",
                                "用一句不超过20个字的中文欢迎大家来玩狼人杀。返回 JSON：{\"speech\": \"内容\"}",
                                s_reply, sizeof(s_reply));
    uint32_t t1 = ww_app_now();
    if (e != ESP_OK || !ww_ai_json_str(s_reply, "speech", s_speech, sizeof(s_speech)) || !s_speech[0]) {
        set_msg("DeepSeek 连不上，检查 Key/网络");
        return;
    }
    char m[64];
    snprintf(m, sizeof(m), "DeepSeek %.1f秒，正在说…", (t1 - t0) / 1000.0);
    set_msg(m);
    e = say(s_speech, 0);
    snprintf(m, sizeof(m), e == ESP_OK ? "OK：DeepSeek %.1f秒 语音正常" : "DeepSeek OK，千问语音失败",
             (t1 - t0) / 1000.0);
    set_msg(m);
    ESP_LOGI(TAG, "测试:%s | 堆最低 %u", s_speech, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

// ---------------------------------------------------------------------------
// 真人按住确定说话
// ---------------------------------------------------------------------------
static void set_busy(ww_busy_t b, int seat)
{
    ww_app_lock();
    ww_set_busy(ww_app_game(), b, seat);
    ww_app_unlock();
}

static void toast(const char *t)
{
    ww_app_lock();
    ww_host_toast(ww_app_host(), t, ww_app_now());
    ww_app_unlock();
}

static void run_ptt(void)
{
    ww_app_lock();
    ww_game_t *g = ww_app_game();
    int seat = ww_current_speaker(g);
    bool ok = seat && !ww_is_bot(g, seat) && !g->busy;
    if (ok) ww_set_busy(g, WW_BUSY_CONNECT, seat);
    ww_app_unlock();
    if (!ok) return;

    if (ww_asr_begin() != ESP_OK) {
        set_busy(WW_BUSY_NONE, 0);
        toast("语音识别连不上");
        return;
    }
    if (s_ptt_up) {                       // 连上之前就松手了
        ww_asr_abort();
        set_busy(WW_BUSY_NONE, 0);
        toast("要按住确定，听到嘀再说");
        return;
    }
    if (!ww_sound_lock(10000)) {
        ww_asr_abort();
        set_busy(WW_BUSY_NONE, 0);
        return;
    }
    ww_sound_beep_locked();
    set_busy(WW_BUSY_REC, seat);
    uint32_t t0 = ww_app_now();
    bool fail = false;
    int peak = 0;
    uint64_t sq = 0;
    uint32_t samples = 0;
    bsp_audio_set_format(16000, 16, 1);
    while (!s_ptt_up && ww_app_now() - t0 < REC_MAX_MS) {
        if (bsp_audio_read(s_pcm, REC_CHUNK) != ESP_OK) { fail = true; ESP_LOGW(TAG, "麦克风读失败"); break; }
        for (int i = 0; i < REC_CHUNK / 2; i++) {
            int v = s_pcm[i] < 0 ? -s_pcm[i] : s_pcm[i];
            if (v > peak) peak = v;
            sq += (uint64_t)((int32_t)s_pcm[i] * s_pcm[i]);
        }
        samples += REC_CHUNK / 2;
        if (ww_asr_feed(s_pcm, REC_CHUNK) != ESP_OK) { fail = true; ESP_LOGW(TAG, "发给语音识别失败"); break; }
    }
    // 诊断:麦克风电平(满量程 32767;正常说话峰值应在几千以上)
    uint32_t rms = 0;
    if (samples) { uint64_t m = sq / samples; while ((uint64_t)(rms + 1) * (rms + 1) <= m) rms++; }
    ESP_LOGI(TAG, "录音 %lu 个采样,峰值 %d,RMS %lu", (unsigned long)samples, peak, (unsigned long)rms);
    ww_sound_unlock();
    uint32_t secs = (ww_app_now() - t0) / 1000;
    set_busy(WW_BUSY_ASR, seat);
    s_speech[0] = '\0';
    if (!fail) ww_asr_end(s_speech, sizeof(s_speech));
    else ww_asr_abort();
    ESP_LOGI(TAG, "%d号说了 %lu 秒:%s", seat, (unsigned long)secs, s_speech);

    ww_app_lock();
    g = ww_app_game();
    ww_set_busy(g, WW_BUSY_NONE, 0);
    if (s_speech[0] && g->phase == WW_PH_DISCUSS) ww_add_speech(g, seat, s_speech, false, ww_app_now());
    else ww_host_toast(ww_app_host(), fail ? "录音出错，再试一次" : "没听清，再按住说一次", ww_app_now());
    ww_app_unlock();
}

// ---------------------------------------------------------------------------
// AI 座位
// ---------------------------------------------------------------------------
static bool run_ai(void)
{
    ww_ai_job_t job;
    ww_app_lock();
    ww_game_t *g = ww_app_game();
    bool ready = ww_host_ai_ready(ww_app_host()) && !g->busy;
    bool has = ready && ww_ai_next_job(g, &job) &&
               ww_ai_prompt(g, &job, s_sys, sizeof(s_sys), s_user, sizeof(s_user));
    if (has && job.task == WW_AI_SPEAK) ww_set_busy(g, WW_BUSY_THINK, job.seat);
    ww_app_unlock();
    if (!has) return false;

    uint32_t t0 = ww_app_now();
    esp_err_t e = ww_cloud_chat(s_sys, s_user, s_reply, sizeof(s_reply));
    ESP_LOGI(TAG, "%d号 AI(任务 %d)%s %lu ms:%.160s", job.seat, job.task, e == ESP_OK ? "" : "调用失败",
             (unsigned long)(ww_app_now() - t0), e == ESP_OK ? s_reply : "");

    ww_app_lock();
    g = ww_app_game();
    if (job.task != WW_AI_SPEAK) {
        ww_ai_apply(g, &job, e == ESP_OK ? s_reply : NULL, ww_app_now(), NULL, 0);
        ww_app_unlock();
        return true;
    }
    bool ok = ww_ai_apply(g, &job, e == ESP_OK ? s_reply : NULL, ww_app_now(), s_speech, sizeof(s_speech));
    ww_set_busy(g, ok ? WW_BUSY_SPEAK : WW_BUSY_NONE, ok ? job.seat : 0);
    ww_app_unlock();
    if (!ok) return true;

    say(s_speech, job.seat - 1);

    ww_app_lock();
    g = ww_app_game();
    if (ww_ai_job_valid(g, &job)) ww_add_speech(g, job.seat, s_speech, true, ww_app_now());
    ww_set_busy(g, WW_BUSY_NONE, 0);
    ww_app_unlock();
    return true;
}

static void brain_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300));
        if (s_want_test) {
            s_want_test = false;
            run_test();
            continue;
        }
        if (s_want_ptt) {
            s_want_ptt = false;
            run_ptt();
            continue;
        }
        while (run_ai()) {
            if (s_want_ptt || s_want_test) break;
        }
    }
}

void ww_brain_start(void)
{
    // 栈:cJSON 构造 + TLS 握手里的 mbedTLS 调用链比较深,给足
    xTaskCreate(brain_task, "ww_brain", 8192, NULL, 4, &s_task);
}
