// main/ww_sound.h —— 主持提示音:核心发出的 ww_cue_t 在这里变成声音。
//
// 队列 + 独立任务,调用方从不阻塞(队列满就丢)。有预录语音就播语音,没有就播
// 一段正弦提示音(不同身份的"睁眼"用不同的音高和次数区分)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ww_core.h"

#define WW_SND_KEY  100     // 按键"嘀"

bool ww_sound_init(void);
void ww_sound_play(int id);            // ww_cue_t 或 WW_SND_KEY
void ww_sound_set_volume(uint8_t percent);
// 扬声器/麦克风独占:AI 说话和录音时拿住,提示音会排队等
bool ww_sound_lock(int timeout_ms);
void ww_sound_unlock(void);
void ww_sound_beep_locked(void);        // 持锁时直接响一声
