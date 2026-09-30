// main/ww_voice.h —— 预录的主持语音片段(16 kHz 单声道 IMA-ADPCM)。
//
// 实现 ww_voice.c 由 tools/gen_voice.py 生成,不要手改。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// cue 是 ww_cue_t。有这句语音返回 true,并给出 ADPCM 数据(每字节两个采样,低 4 位在前)。
bool ww_voice_get(int cue, const uint8_t **data, uint32_t *len);
