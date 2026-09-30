// main/ww_app.h —— 固件胶水层:一把锁护住游戏状态,串起按键、屏幕、网络、HTTP、声音。
#pragma once

#include "ww_host.h"

void ww_app_start(void);

// 游戏状态锁。HTTP 任务、游戏任务、LVGL 定时器都要先拿它。
void ww_app_lock(void);
void ww_app_unlock(void);
ww_game_t *ww_app_game(void);
ww_host_t *ww_app_host(void);

// 当前毫秒时间(给状态机用)
uint32_t ww_app_now(void);

// 网络层回调:网络状态变了
void ww_app_set_net(const ww_net_info_t *info);
