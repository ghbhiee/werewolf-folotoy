// main/ww_brain.h —— AI 大脑任务:所有联网的活都在这一个任务里排队做
// (同一时刻最多一条 TLS 连接):
//   · AI 座位的夜间行动 / 投票      ww_ai 出题 → DeepSeek → ww_ai 落地(失败随机兜底)
//   · 轮到 AI 发言                  DeepSeek 写发言 → 千问 TTS 边收边从喇叭播 → 记进发言记录
//   · 真人按住确定说话              麦克风 16 kHz → 千问流式 ASR → 记进发言记录
//   · 主持菜单"测试 AI"
#pragma once

void ww_brain_start(void);
void ww_brain_ptt(int req);     // 1 = 按下开始录音,2 = 松手
void ww_brain_test(void);
