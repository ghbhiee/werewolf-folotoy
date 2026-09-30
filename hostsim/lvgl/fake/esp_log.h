#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, ...) do { printf("[I][%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
