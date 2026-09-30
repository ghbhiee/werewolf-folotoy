// main/ww_net.h —— Wi-Fi:有家里 Wi-Fi 凭据就先连(STA),连不上/没配过就自己开热点(SoftAP)。
//
// 热点:SSID Werewolf-XXXX、密码 wolfNNNN(都由 MAC 派生,屏上明文显示),
// IP 192.168.4.1,ESP32-C3 最多 10 台手机。家里 Wi-Fi 凭据存 NVS 命名空间 "wwnet"。
#pragma once

#include <stdbool.h>

#include "ww_host.h"

// 启动网络(异步:连接结果通过 ww_app_set_net() 回报)
void ww_net_start(void);
// 主持菜单发来的请求:改用热点 / 进配网模式 / 忘记家里 Wi-Fi
void ww_net_request(ww_req_t req);
// 配网页面提交:存 NVS,稍后重启去连
bool ww_net_save_and_reboot(const char *ssid, const char *pass);
bool ww_net_in_setup(void);
