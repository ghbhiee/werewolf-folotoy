// main/ww_ai.h —— AI 玩家的"脑子"里不联网的那一半(纯 C,主机可测):
//   1. 现在该哪个 AI 座位出手(夜里行动 / 投票 / 轮到它发言);
//   2. 按这个座位"自己能看到的信息"写给大模型的提示词(不含任何别人的身份);
//   3. 解析模型回的 JSON,校验后落到状态机上;模型没回或乱回就随机兜底。
//
// 联网那一半(DeepSeek / 千问 TTS / ASR)在 ww_cloud.c,串起来的任务在 ww_brain.c。
// 提示词改编自 ~/cc/werewolf 的 ai_agent.py / prompts.py(中级:不灌策略,靠模型自己推理)。
//
// 线程模型同 ww_core:调用方持锁。
#pragma once

#include "ww_core.h"

typedef enum {
    WW_AI_NONE = 0,
    WW_AI_WOLF,
    WW_AI_WITCH,
    WW_AI_SEER,
    WW_AI_VOTE,
    WW_AI_SPEAK,
} ww_ai_task_t;

typedef struct {
    ww_ai_task_t task;
    int seat;
    uint8_t phase;      // 出题时的阶段,落地时核对(主持人可能已经强推过了)
    uint8_t round;
} ww_ai_job_t;

// 找下一个需要大脑出手的 AI 座位:夜晚行动/投票优先,其次讨论阶段轮到它发言。
bool ww_ai_next_job(const ww_game_t *g, ww_ai_job_t *job);

// 写提示词。返回 false 表示缓冲不够(会尽量截短发言记录来装下)。
bool ww_ai_prompt(const ww_game_t *g, const ww_ai_job_t *job,
                  char *sys, size_t sys_cap, char *user, size_t user_cap);

// 把模型输出(可能为 NULL = 调用失败)落到状态机上。
//   决策类:执行动作,非法/缺失就随机兜底;返回 true 表示已处理。
//   发言类:只把 "speech" 抽到 speech_out,不改状态(播完再由调用方 ww_add_speech)。
bool ww_ai_apply(ww_game_t *g, const ww_ai_job_t *job, const char *json, uint32_t now,
                 char *speech_out, size_t speech_cap);

// 这个 job 现在还作数吗(阶段/轮次没变、还轮到它)
bool ww_ai_job_valid(const ww_game_t *g, const ww_ai_job_t *job);

// 小工具(测试也用):从模型输出里取整数字段 / 字符串字段(处理 \" \\ \n \uXXXX)
bool ww_ai_json_int(const char *json, const char *key, int *out);
bool ww_ai_json_str(const char *json, const char *key, char *out, size_t cap);
