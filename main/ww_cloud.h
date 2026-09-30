// main/ww_cloud.h —— FoloToy 直连云端:DeepSeek(决策/写发言)、千问 TTS(发言变声音)、
// 千问流式 ASR(真人按住确定说的话变文字)。
//
// 全部是阻塞调用,只在 ww_brain 任务里用,同一时刻最多一条 TLS 连接
// (C3 无 PSRAM,一条 TLS 约 40 KB 堆,两条就危险了)。
//
// Key 存在 NVS 命名空间 "wwai"(ds = DashScope/千问,dk = DeepSeek),
// 只能由玩家在手机上的 /ai 页面填写,固件和日志里都不打印。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void ww_cloud_init(void);
void ww_cloud_keys(bool *ds, bool *dk);
bool ww_cloud_set_keys(const char *ds, const char *dk);   // 空串 = 不改
bool ww_cloud_ai_on(void);
void ww_cloud_set_ai_on(bool on);

// DeepSeek chat/completions(JSON 输出模式、关思考),把回复的 content 写进 out。
esp_err_t ww_cloud_chat(const char *sys, const char *user, char *out, size_t cap);

// 千问 TTS:16 kHz 单声道 s16le PCM 边到边交给 sink(sink 里直接写 I2S 就是边收边播)。
typedef void (*ww_pcm_sink_t)(const uint8_t *pcm, size_t len, void *ctx);
esp_err_t ww_cloud_tts(const char *text, int voice, ww_pcm_sink_t sink, void *ctx);

// 千问流式 ASR:begin 连上并开任务 → feed 喂 16 kHz PCM → end 收尾拿全文。
esp_err_t ww_asr_begin(void);
esp_err_t ww_asr_feed(void *pcm, size_t len);   // 会被原地掩码改写,必须是 RAM 里可写的缓冲
esp_err_t ww_asr_end(char *out, size_t cap);
void ww_asr_abort(void);
