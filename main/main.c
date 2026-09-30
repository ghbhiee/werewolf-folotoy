// main/main.c —— FoloToy AI Passport 玩法「狼人杀 · 局域网精简版」入口。
//
// 单一用途玩法:不保留 BSP demo 菜单,开机直接进狼人杀。FoloToy 同时是游戏服务器
// 和主持屏,玩家用手机扫屏幕上的二维码加入。原来的硬件演示页(demo_*.c)仍然留在
// 仓库里,只是不编进这个玩法。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "esp_log.h"
#include "nvs_flash.h"
#include "ww_app.h"
#include "ww_sound.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "狼人杀启动");

    bsp_i2c_init();

    // 屏幕是主持人的全部载体,起不来就没有降级形态可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // NVS:存家里 Wi-Fi 凭据(命名空间 wwnet),Wi-Fi 驱动也要它。
    // 只动默认 nvs 分区,cardid 分区(0x356000)完全不碰。
    // 和上游 demo_radio.c 一样:初始化失败也不自动擦除,别的玩法可能在里面存了东西。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败:%s;未自动擦除。家里 Wi-Fi 存不住,只能用热点模式",
                 esp_err_to_name(err));
    }

    // 以下允许失败,各自降级:电量计不在 → 右上角 "--";音频不在 → 静音
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");
    ww_sound_init();

    ww_app_start();
}
