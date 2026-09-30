// main/ww_sound.c —— 主持提示音,见 ww_sound.h。
//
// 语音片段(ww_voice.c,由 tools/gen_voice.py 生成)是 16 kHz IMA-ADPCM,
// 在这里边解码边送 I2S;某个提示没有语音就退回正弦音。
#include "ww_sound.h"

#include <math.h>
#include <stdint.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ww_voice.h"

static const char *TAG = "ww_sound";

#define SAMPLE_RATE 16000
#define CHUNK       256

typedef struct {
    uint16_t freq;      // Hz;0 = 静音间隔
    uint16_t ms;        // 0 = 结束
} note_t;

#define REST(ms) {1, ms}
#define END      {0, 0}

// 夜里大家闭着眼,只能靠耳朵:狼人 = 低音 1 声,女巫 = 中音 2 声,预言家 = 高音 3 声
static const note_t M_KEY[]         = {{1320, 30}, END};
static const note_t M_DEAL[]        = {{784, 90}, {988, 90}, {1175, 90}, {1568, 180}, END};
static const note_t M_NIGHT[]       = {{784, 260}, REST(60), {659, 260}, REST(60), {523, 420}, END};
static const note_t M_WOLF_OPEN[]   = {{330, 600}, END};
static const note_t M_WITCH_OPEN[]  = {{660, 260}, REST(160), {660, 260}, END};
static const note_t M_SEER_OPEN[]   = {{990, 180}, REST(120), {990, 180}, REST(120), {990, 180}, END};
static const note_t M_CLOSE[]       = {{523, 160}, {392, 260}, END};
static const note_t M_DAWN[]        = {{523, 140}, {659, 140}, {784, 140}, {1047, 320}, END};
static const note_t M_DISCUSS[]     = {{880, 120}, REST(80), {880, 120}, END};
static const note_t M_VOTE[]        = {{988, 100}, REST(70), {988, 100}, REST(70), {988, 100}, END};
static const note_t M_VOTE_END[]    = {{784, 400}, END};
static const note_t M_WIN_GOOD[]    = {{523, 120}, {659, 120}, {784, 120}, {1047, 120}, {1319, 360}, END};
static const note_t M_WIN_WOLF[]    = {{440, 200}, {415, 200}, {392, 200}, {330, 500}, END};

static const note_t *melody(int id)
{
    switch (id) {
    case WW_SND_KEY:          return M_KEY;
    case WW_CUE_DEAL:         return M_DEAL;
    case WW_CUE_NIGHT:        return M_NIGHT;
    case WW_CUE_WOLF_OPEN:    return M_WOLF_OPEN;
    case WW_CUE_WITCH_OPEN:   return M_WITCH_OPEN;
    case WW_CUE_SEER_OPEN:    return M_SEER_OPEN;
    case WW_CUE_WOLF_CLOSE:
    case WW_CUE_WITCH_CLOSE:
    case WW_CUE_SEER_CLOSE:   return M_CLOSE;
    case WW_CUE_DAWN_PEACE:
    case WW_CUE_DAWN_DEATH:   return M_DAWN;
    case WW_CUE_DISCUSS:      return M_DISCUSS;
    case WW_CUE_VOTE:         return M_VOTE;
    case WW_CUE_VOTE_END:     return M_VOTE_END;
    case WW_CUE_WIN_GOOD:     return M_WIN_GOOD;
    case WW_CUE_WIN_WOLF:     return M_WIN_WOLF;
    default:                  return NULL;
    }
}

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_io;       // I2S 独占
static bool s_ready;

bool ww_sound_lock(int timeout_ms)
{
    if (!s_io) return false;
    return xSemaphoreTake(s_io, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ww_sound_unlock(void)
{
    if (s_io) xSemaphoreGive(s_io);
}

static void play_note(const note_t *n)
{
    uint32_t total = (uint32_t)SAMPLE_RATE * n->ms / 1000u;
    if (!total) return;
    uint32_t fade = SAMPLE_RATE * 4u / 1000u;
    if (fade * 2u > total) fade = total / 2u;
    int16_t buf[CHUNK];
    float phase = 0.0f;
    float step = 2.0f * (float)M_PI * (float)n->freq / (float)SAMPLE_RATE;
    bool rest = n->freq <= 1;
    for (uint32_t done = 0; done < total;) {
        uint32_t count = total - done;
        if (count > CHUNK) count = CHUNK;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t pos = done + i;
            float gain = rest ? 0.0f : 0.35f;
            if (pos < fade) gain *= (float)pos / (float)fade;
            else if (total - pos < fade) gain *= (float)(total - pos) / (float)fade;
            buf[i] = (int16_t)(sinf(phase) * gain * 32767.0f);
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        bsp_audio_write(buf, count * sizeof(int16_t));
        done += count;
    }
}

// ---- IMA-ADPCM(4 bit,单声道)解码 ----
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static void play_adpcm(const uint8_t *data, uint32_t len)
{
    int pred = 0, idx = 0;
    int16_t buf[CHUNK];
    uint32_t n = 0;
    for (uint32_t i = 0; i < len * 2; i++) {
        uint8_t nib = (i & 1) ? (data[i >> 1] >> 4) : (data[i >> 1] & 0x0F);
        int step = STEP[idx];
        int diff = step >> 3;
        if (nib & 4) diff += step;
        if (nib & 2) diff += step >> 1;
        if (nib & 1) diff += step >> 2;
        pred += (nib & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        idx += INDEX[nib];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        buf[n++] = (int16_t)pred;
        if (n == CHUNK) {
            bsp_audio_write(buf, sizeof(buf));
            n = 0;
        }
    }
    if (n) bsp_audio_write(buf, n * sizeof(int16_t));
    // 尾巴补一点静音,避免 I2S DMA 里残留的最后一帧被循环播放成"嗡"
    for (int k = 0; k < CHUNK; k++) buf[k] = 0;
    bsp_audio_write(buf, sizeof(buf));
}

static void sound_task(void *arg)
{
    (void)arg;
    int id;
    for (;;) {
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE) continue;
        if (!ww_sound_lock(20000)) continue;
        if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) { ww_sound_unlock(); continue; }
        const uint8_t *clip = NULL;
        uint32_t clip_len = 0;
        if (id != WW_SND_KEY && ww_voice_get(id, &clip, &clip_len)) {
            play_adpcm(clip, clip_len);
        } else {
            const note_t *m = melody(id);
            for (; m && m->ms; m++) play_note(m);
        }
        ww_sound_unlock();
    }
}

bool ww_sound_init(void)
{
    if (s_ready) return true;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频不可用,静音运行");
        return false;
    }
    bsp_audio_set_volume(70);
    s_queue = xQueueCreate(6, sizeof(int));
    s_io = xSemaphoreCreateMutex();
    if (!s_queue) return false;
    if (xTaskCreate(sound_task, "ww_sound", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    s_ready = true;
    return true;
}

void ww_sound_play(int id)
{
    if (!s_ready) return;
    (void)xQueueSend(s_queue, &id, 0);     // 满了就丢,不能让调用方等
}

void ww_sound_beep_locked(void)
{
    // 调用方已持 I2S 锁:直接响一声短"嘀"(录音开始提示)
    static const note_t b[] = {{1047, 120}, {0, 0}};
    bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    play_note(&b[0]);
}

void ww_sound_set_volume(uint8_t percent)
{
    if (s_ready) bsp_audio_set_volume(percent);
}
