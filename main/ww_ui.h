// main/ww_ui.h —— 把 ww_view_t(视图模型)画到 240x320 屏上。
//
// 只能在持有 LVGL 锁时调用(固件里由 LVGL 定时器回调调用,本身就持锁)。
// 布局相同的连续两帧只更新文字和颜色,布局变了(换界面、换标题、座位数变了)
// 才整屏重建。
#pragma once

#include "ww_host.h"

void ww_ui_render(const ww_view_t *v);
// 电量读数刷新(右上角),读不到时显示 "--"
void ww_ui_battery_refresh(void);
