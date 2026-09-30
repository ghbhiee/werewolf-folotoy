// main/ww_http.h —— 板上 HTTP 服务器(esp_http_server,端口 80)。
//
// 只负责收发字节;接口语义全在 ww_api.c(主机仿真服务器用的也是它)。
// 玩家网页 web/player.html 构建时 gzip 后嵌进固件,原样以 Content-Encoding: gzip 吐出。
#pragma once

#include "esp_err.h"

esp_err_t ww_http_start(void);
