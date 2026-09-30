// main/ww_net.c —— Wi-Fi STA / SoftAP 切换与配网凭据,见 ww_net.h。
#include "ww_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ww_app.h"

static const char *TAG = "ww_net";

#define NVS_NS          "wwnet"
#define STA_TIMEOUT_MS  15000
#define AP_CHANNEL      6
#define AP_MAX_CONN     10      // ESP32-C3 SoftAP 上限(esp_wifi_types_native.h)

#define EV_GOT_IP   BIT0
#define EV_REQUEST  BIT1

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta, *s_ap;
static ww_net_info_t s_info;
static char s_sta_ssid[33], s_sta_pass[65];
static volatile bool s_connecting;
static volatile ww_req_t s_pending = WW_REQ_NONE;
static bool s_started;

static void publish(void)
{
    ww_app_set_net(&s_info);
}

static void ap_identity(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ssid_cap, "Werewolf-%02X%02X", mac[4], mac[5]);
    snprintf(pass, pass_cap, "wolf%04u", (unsigned)(((mac[4] << 8) | mac[5]) % 10000u));
}

// 凭据两组:ssid/pass 是验证过能连上的;new_ssid/new_pass 是配网页刚提交、还没
// 验证的。新凭据连上了才转正,连不上就丢掉并退回旧的 —— 输错一次密码不会把
// 原来能用的配置覆盖掉。
static bool load_creds_ns(const char *ns, const char *k_ssid, const char *k_pass)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n1 = sizeof(s_sta_ssid), n2 = sizeof(s_sta_pass);
    bool ok = nvs_get_str(h, k_ssid, s_sta_ssid, &n1) == ESP_OK && s_sta_ssid[0];
    if (ok && nvs_get_str(h, k_pass, s_sta_pass, &n2) != ESP_OK) s_sta_pass[0] = '\0';
    nvs_close(h);
    if (!ok) s_sta_ssid[0] = s_sta_pass[0] = '\0';
    return ok;
}

static bool load_creds(const char *k_ssid, const char *k_pass)
{
    return load_creds_ns(NVS_NS, k_ssid, k_pass);
}

// 同一台 FoloToy 之前刷过「电梯呼救」固件的话,它在 NVS 命名空间 elevsos 里存过家里
// Wi-Fi(ssid/pass)。分段烧录不擦 NVS,所以直接借来用,省得再配一次网;连上后
// 复制一份到自己的 wwnet 命名空间,以后就不依赖它了。
#define ELEVSOS_NS "elevsos"

static bool borrow_allowed(void)
{
    nvs_handle_t h;
    uint8_t no = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "noborrow", &no);
        nvs_close(h);
    }
    return !no;
}

static void promote_pending(bool keep)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (keep) {
        nvs_set_str(h, "ssid", s_sta_ssid);
        nvs_set_str(h, "pass", s_sta_pass);
    }
    nvs_erase_key(h, "new_ssid");
    nvs_erase_key(h, "new_pass");
    nvs_commit(h);
    nvs_close(h);
}

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[%s] heap free %u  largest block %u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_info.mode == WW_NET_STA || s_connecting) {
            // 已连上后掉线:一直重连(玩家都在家里 Wi-Fi 上,不能半局切热点)
            if (s_info.mode == WW_NET_STA) {
                snprintf(s_info.note, sizeof(s_info.note), "Wi-Fi断开，正在重连");
                publish();
            }
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "有手机连上热点(还没分到 IP)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "有手机断开热点,reason %d", e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        const ip_event_ap_staipassigned_t *e = (const ip_event_ap_staipassigned_t *)data;
        ESP_LOGI(TAG, "DHCP 分配 " IPSTR, IP2STR(&e->ip));
        log_heap("phone joined");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(s_info.ip, sizeof(s_info.ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "已连上 %s,IP %s", s_sta_ssid, s_info.ip);
        if (s_info.mode == WW_NET_STA) {
            s_info.note[0] = '\0';
            publish();
        }
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static void start_ap(ww_net_mode_t mode, const char *note)
{
    s_info.mode = WW_NET_BOOT;        // 先改掉 STA 状态,免得 stop 触发的断线事件去重连
    esp_wifi_stop();
    wifi_config_t cfg = {0};
    char ssid[33], pass[65];
    ap_identity(ssid, sizeof(ssid), pass, sizeof(pass));
    memcpy(cfg.ap.ssid, ssid, strlen(ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(cfg.ap.password, pass, strlen(pass));
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = AP_MAX_CONN;
    cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    memset(&s_info, 0, sizeof(s_info));
    s_info.mode = mode;
    snprintf(s_info.ssid, sizeof(s_info.ssid), "%s", ssid);
    snprintf(s_info.pass, sizeof(s_info.pass), "%s", pass);
    snprintf(s_info.ip, sizeof(s_info.ip), "192.168.4.1");
    snprintf(s_info.note, sizeof(s_info.note), "%s", note ? note : "");
    ESP_LOGI(TAG, "热点 %s 密码 %s 已开启(%s)", ssid, pass, mode == WW_NET_SETUP ? "配网模式" : "游戏");
    log_heap("ap started");
    publish();
}

static bool try_sta(void)
{
    s_info.mode = WW_NET_BOOT;
    esp_wifi_stop();
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, s_sta_ssid, strlen(s_sta_ssid));
    memcpy(cfg.sta.password, s_sta_pass, strlen(s_sta_pass));
    cfg.sta.threshold.authmode = s_sta_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    // 家里同名多个 AP(比如 mesh 路由、一远一近两台)时,默认会连扫描到的第一台;
    // 全信道扫描 + 按信号强度选,连最近那台(电梯固件踩过这个坑)
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    memset(&s_info, 0, sizeof(s_info));
    s_info.mode = WW_NET_CONNECTING;
    snprintf(s_info.ssid, sizeof(s_info.ssid), "%s", s_sta_ssid);
    publish();

    xEventGroupClearBits(s_ev, EV_GOT_IP);
    s_connecting = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(STA_TIMEOUT_MS));
    s_connecting = false;
    if (!(b & EV_GOT_IP)) {
        ESP_LOGW(TAG, "%d 秒内没连上 %s", STA_TIMEOUT_MS / 1000, s_sta_ssid);
        return false;
    }
    s_info.mode = WW_NET_STA;
    s_info.note[0] = '\0';
    log_heap("sta connected");
    publish();
    return true;
}

static void forget_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_set_u8(h, "noborrow", 1);       // 忘记了就别再去借电梯固件那份
        nvs_commit(h);
        nvs_close(h);
    }
    s_sta_ssid[0] = s_sta_pass[0] = '\0';
}

// 网络切换要 stop/start Wi-Fi 还要等连接,不能在按键回调或 HTTP 任务里做,放这个任务里
static void net_task(void *arg)
{
    (void)arg;
    log_heap("before wifi");
    bool up = false;
    const char *note = NULL;
    if (load_creds("new_ssid", "new_pass")) {
        // 配网页刚提交的新凭据:连上才转正
        up = try_sta();
        promote_pending(up);
        if (!up) note = "新Wi-Fi连不上，已丢弃";
    }
    bool have_own = load_creds("ssid", "pass");
    if (!up && !have_own && borrow_allowed() && load_creds_ns(ELEVSOS_NS, "ssid", "pass")) {
        ESP_LOGI(TAG, "借用电梯固件留下的家里 Wi-Fi「%s」", s_sta_ssid);
        up = try_sta();
        if (up) promote_pending(true);          // 转存到 wwnet
        else note = "连不上家里Wi-Fi，已改用热点";
    }
    if (!up && have_own && load_creds("ssid", "pass")) {
        bool new_failed = note != NULL;
        up = try_sta();
        if (up && new_failed) {
            // 新的没连上,旧的还能用:连回旧的,并在大厅底栏说明
            snprintf(s_info.note, sizeof(s_info.note), "新Wi-Fi连不上，仍用原来的");
            publish();
        }
        if (!up) note = "连不上家里Wi-Fi，已改用热点";
    }
    if (!up) start_ap(WW_NET_AP, note);
    for (;;) {
        xEventGroupWaitBits(s_ev, EV_REQUEST, pdTRUE, pdFALSE, portMAX_DELAY);
        ww_req_t r = s_pending;
        s_pending = WW_REQ_NONE;
        switch (r) {
        case WW_REQ_NET_AP:
            start_ap(WW_NET_AP, NULL);
            break;
        case WW_REQ_NET_SETUP:
            start_ap(WW_NET_SETUP, NULL);
            break;
        case WW_REQ_NET_FORGET:
            forget_creds();
            start_ap(WW_NET_AP, "已忘记家里Wi-Fi");
            break;
        case WW_REQ_NET_STA: {
            bool ok = load_creds("ssid", "pass") ||
                      (borrow_allowed() && load_creds_ns(ELEVSOS_NS, "ssid", "pass"));
            if (!ok) start_ap(WW_NET_AP, "还没配过家里Wi-Fi");
            else if (!try_sta()) start_ap(WW_NET_AP, "连不上家里Wi-Fi，已改用热点");
            break;
        }
        default:
            break;
        }
    }
}

void ww_net_start(void)
{
    if (s_started) return;
    s_started = true;
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    // 凭据我们自己存,别让驱动再存一份
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, NULL, NULL));
    xTaskCreate(net_task, "ww_net", 4096, NULL, 4, NULL);
}

void ww_net_request(ww_req_t req)
{
    if (req == WW_REQ_NONE || !s_ev) return;
    s_pending = req;
    xEventGroupSetBits(s_ev, EV_REQUEST);
}

bool ww_net_in_setup(void)
{
    return s_info.mode == WW_NET_SETUP;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));        // 先让 HTTP 响应发出去
    esp_restart();
}

bool ww_net_save_and_reboot(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "new_ssid", ssid) == ESP_OK &&
              nvs_set_str(h, "new_pass", pass ? pass : "") == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "已暂存家里 Wi-Fi「%s」,1.5 秒后重启验证(连上才替换旧配置)", ssid);
        xTaskCreate(reboot_task, "ww_reboot", 2048, NULL, 3, NULL);
    }
    return ok;
}
