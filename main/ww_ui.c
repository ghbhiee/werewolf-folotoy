// main/ww_ui.c —— 狼人杀主持屏的 LVGL 绘制,见 ww_ui.h。
//
// 屏幕分区(240x320,ui_pixel 主题):
//   y   8..41   标题牌(ui_pixel)            右上角电量(x≈196,y≈28,避开云)
//   y  50..284  内容区:二维码/阶段面板/座位格/菜单/状态
//   y 286..320  草地,底栏提示文字压在草地上(二次确认时变成黑底黄字)
#include "ww_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "lvgl.h"
#include "ui_pixel.h"

LV_FONT_DECLARE(font_pixel_12);
LV_FONT_DECLARE(font_pixel_24);

#define FS (&font_pixel_12)
#define FL (&font_pixel_24)

#define NIGHT_SKY  0x1B2440
#define SEAT_EMPTY 0xD9E7EC
#define SEAT_OFF   0xB8C0C4
#define SEAT_DEAD  0x4A4F55

#define BATT_X 196
#define BATT_Y 28
#define BATT_W 30
#define BATT_H 13
#define BATT_INNER_W (BATT_W - 6)

static lv_obj_t *s_scr;
static char s_sig[256];           // 当前布局签名,变了才重建

static lv_obj_t *s_batt_fill, *s_batt_text;
static lv_obj_t *s_hint;
static lv_obj_t *s_big, *s_line1, *s_line2, *s_timer;
static lv_obj_t *s_qr, *s_qr_label, *s_qr_label2, *s_info;
static lv_obj_t *s_cells[WW_MAX_SEATS], *s_cell_nums[WW_MAX_SEATS];
static lv_obj_t *s_rows[WWV_MENU_ROWS], *s_row_labels[WWV_MENU_ROWS];
static lv_obj_t *s_more_up, *s_more_down;
static lv_obj_t *s_lines[WWV_LINES_MAX];
static lv_obj_t *s_vol_fill;

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font,
                      uint32_t color, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t) != 0) lv_label_set_text(l, t);
}

// ---------------------------------------------------------------------------
// 电量
// ---------------------------------------------------------------------------
static void battery_badge(lv_obj_t *scr, bool night)
{
    block(scr, BATT_X, BATT_Y, BATT_W, BATT_H, UI_INK);
    block(scr, BATT_X + 3, BATT_Y + 3, BATT_INNER_W, BATT_H - 6, UI_PAPER);
    block(scr, BATT_X + BATT_W, BATT_Y + 4, 3, BATT_H - 8, UI_INK);
    s_batt_fill = block(scr, BATT_X + 3, BATT_Y + 3, 1, BATT_H - 6, UI_GRASS);
    s_batt_text = text(scr, BATT_X - 4, BATT_Y + BATT_H + 2, 0, FS, night ? 0xC8D0E0 : UI_PAPER,
                       LV_TEXT_ALIGN_LEFT);
    ww_ui_battery_refresh();
}

void ww_ui_battery_refresh(void)
{
    if (!s_batt_fill || !s_batt_text) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        // 读不到就不画数字,别编一个出来
        lv_obj_add_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
        set_text(s_batt_text, "  --");
        return;
    }
    if (soc > 100) soc = 100;
    int w = BATT_INNER_W * soc / 100;
    if (w < 1) w = 1;
    lv_obj_remove_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_batt_fill, w);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(soc < 20 ? UI_RED : UI_GRASS), 0);
    char buf[8];
    snprintf(buf, sizeof(buf), "%3d%%", soc);
    set_text(s_batt_text, buf);
}

// ---------------------------------------------------------------------------
// 座位格
// ---------------------------------------------------------------------------
static void seat_grid(lv_obj_t *scr, const ww_view_t *v, int y0, int cols, int size, int gap,
                      const lv_font_t *font)
{
    int n = v->n_seats;
    int rows = (n + cols - 1) / cols;
    int per_row = n < cols ? n : cols;
    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        int in_row = (r == rows - 1) ? n - r * cols : per_row;
        int x0 = (240 - (in_row * size + (in_row - 1) * gap)) / 2;
        lv_obj_t *cell = block(scr, x0 + c * (size + gap), y0 + r * (size + gap), size, size, SEAT_EMPTY);
        lv_obj_set_style_border_color(cell, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_t *num = lv_label_create(cell);
        lv_obj_set_style_text_font(num, font, 0);
        char b[4];
        snprintf(b, sizeof(b), "%d", i + 1);
        lv_label_set_text(num, b);
        lv_obj_center(num);
        s_cells[i] = cell;
        s_cell_nums[i] = num;
    }
}

static void seat_colors(const ww_view_t *v)
{
    for (int i = 0; i < v->n_seats; i++) {
        if (!s_cells[i]) continue;
        uint8_t f = v->seat[i];
        uint32_t bg = SEAT_EMPTY, fg = 0x7A8A90, border = UI_INK;
        if (f & WWV_SEAT_OCC) { bg = (f & WWV_SEAT_ON) ? UI_PAPER : SEAT_OFF; fg = UI_INK; }
        if (f & WWV_SEAT_DEAD) { bg = SEAT_DEAD; fg = 0xAAAAAA; }
        if (f & WWV_SEAT_VOTED) { bg = UI_YELLOW; fg = UI_INK; }
        if (f & WWV_SEAT_SPEAK) { bg = UI_ORANGE; fg = UI_INK; border = UI_RED; }
        if (f & WWV_SEAT_OUT) { bg = UI_RED; fg = 0xFFFFFF; }
        lv_obj_set_style_bg_color(s_cells[i], lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(s_cells[i], lv_color_hex(border), 0);
        lv_obj_set_style_text_color(s_cell_nums[i], lv_color_hex(fg), 0);
    }
}

// ---------------------------------------------------------------------------
// 各界面的骨架
// ---------------------------------------------------------------------------
static void build_qr(lv_obj_t *scr, const ww_view_t *v, bool night)
{
    lv_obj_t *frame = ui_pixel_panel_create(scr, 46, 50, 148, 148, 0xFFFFFF);
    s_qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(s_qr, 132);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_qr);
    lv_qrcode_update(s_qr, v->qr, (uint32_t)strlen(v->qr));
    uint32_t c = night ? 0xC8D0E0 : UI_PAPER;
    s_qr_label = text(scr, 0, 204, 240, FS, c, LV_TEXT_ALIGN_CENTER);
    s_qr_label2 = text(scr, 0, 219, 240, FS, c, LV_TEXT_ALIGN_CENTER);
    s_info = text(scr, 0, 236, 240, FS, UI_YELLOW, LV_TEXT_ALIGN_CENTER);
}

static void build_panel(lv_obj_t *scr, int h)
{
    lv_obj_t *p = ui_pixel_panel_create(scr, 8, 52, 224, h, UI_PAPER);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(p, 10, 0);
    lv_obj_set_style_pad_row(p, 6, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_big = lv_label_create(p);
    lv_obj_set_style_text_font(s_big, FL, 0);
    lv_obj_set_style_text_color(s_big, lv_color_hex(UI_INK), 0);
    s_line1 = lv_label_create(p);
    s_line2 = lv_label_create(p);
    lv_obj_t *ls[2] = {s_line1, s_line2};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(ls[i], 206);
        lv_label_set_long_mode(ls[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(ls[i], FS, 0);
        lv_obj_set_style_text_align(ls[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(ls[i], lv_color_hex(i ? 0x55606A : UI_INK), 0);
        lv_obj_set_style_text_line_space(ls[i], 3, 0);
        lv_label_set_text(ls[i], "");
    }
    lv_label_set_text(s_big, "");
    s_timer = lv_label_create(p);
    lv_obj_add_flag(s_timer, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(s_timer, FS, 0);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(0x55606A), 0);
    lv_obj_align(s_timer, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_label_set_text(s_timer, "");
}

static void build_menu(lv_obj_t *scr, const ww_view_t *v)
{
    int rows = v->n_items < WWV_MENU_ROWS ? v->n_items : WWV_MENU_ROWS;
    lv_obj_t *p = ui_pixel_panel_create(scr, 8, 52, 224, rows * 34 + 10, UI_PAPER);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    for (int r = 0; r < rows; r++) {
        s_rows[r] = block(p, 2, 3 + r * 34, 214, 32, UI_PAPER);
        s_row_labels[r] = text(s_rows[r], 10, 4, 0, FL, UI_INK, LV_TEXT_ALIGN_LEFT);
    }
    if (v->n_items > WWV_MENU_ROWS) {
        s_more_up = text(p, 196, 2, 0, FS, UI_INK, LV_TEXT_ALIGN_LEFT);
        s_more_down = text(p, 196, rows * 34 - 8, 0, FS, UI_INK, LV_TEXT_ALIGN_LEFT);
    }
    s_info = text(scr, 0, 264, 240, FS, UI_PAPER, LV_TEXT_ALIGN_CENTER);
}

static void build_status(lv_obj_t *scr, const ww_view_t *v)
{
    lv_obj_t *p = ui_pixel_panel_create(scr, 4, 52, 232, v->n_lines * 17 + 10, UI_PAPER);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < v->n_lines; i++) s_lines[i] = text(p, 4, 4 + i * 17, 0, FS, UI_INK, LV_TEXT_ALIGN_LEFT);
}

static void build(const ww_view_t *v)
{
    s_batt_fill = s_batt_text = s_hint = NULL;
    s_big = s_line1 = s_line2 = s_timer = NULL;
    s_qr = s_qr_label = s_qr_label2 = s_info = NULL;
    s_more_up = s_more_down = s_vol_fill = NULL;
    memset(s_cells, 0, sizeof(s_cells));
    memset(s_cell_nums, 0, sizeof(s_cell_nums));
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_labels, 0, sizeof(s_row_labels));
    memset(s_lines, 0, sizeof(s_lines));

    // 先清空旧屏的子对象再建新屏:LVGL 池只有几十 KB,新旧两屏同时存在会翻倍
    if (s_scr) lv_obj_clean(s_scr);
    lv_obj_t *scr = ui_pixel_screen_create_font(v->title, FL);
    if (v->night) lv_obj_set_style_bg_color(scr, lv_color_hex(NIGHT_SKY), 0);
    battery_badge(scr, v->night);

    switch (v->kind) {
    case WWV_LOBBY:
        build_qr(scr, v, v->night);
        seat_grid(scr, v, 256, 12, 17, 2, FS);
        break;
    case WWV_SETUP:
        build_qr(scr, v, v->night);
        break;
    case WWV_GAME:
        build_panel(scr, 118);
        seat_grid(scr, v, 180, 6, 32, 4, FL);
        break;
    case WWV_BOOT:
        build_panel(scr, 100);
        ui_pixel_mascot_create(scr, 101, 200);
        break;
    case WWV_VOLUME: {
        build_panel(scr, 60);
        lv_obj_t *bar = ui_pixel_panel_create(scr, 30, 140, 180, 26, UI_PAPER);
        lv_obj_set_style_pad_all(bar, 0, 0);
        s_vol_fill = block(bar, 2, 2, 1, 16, UI_YELLOW);
        ui_pixel_mascot_create(scr, 101, 200);
        break;
    }
    case WWV_MENU:
        build_menu(scr, v);
        break;
    case WWV_STATUS:
        build_status(scr, v);
        break;
    }

    s_hint = text(scr, 0, 297, 240, FS, UI_INK, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_hint, 2, 0);

    lv_obj_t *old = s_scr;
    s_scr = scr;
    lv_screen_load(scr);
    if (old) lv_obj_delete(old);
}

// ---------------------------------------------------------------------------
// 每帧更新
// ---------------------------------------------------------------------------
static void update(const ww_view_t *v)
{
    char buf[32];
    set_text(s_hint, v->hint);
    lv_obj_set_style_bg_opa(s_hint, v->alert ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_hint, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(v->alert ? UI_YELLOW : UI_INK), 0);

    set_text(s_qr_label, v->qr_label);
    set_text(s_qr_label2, v->qr_label2);
    set_text(s_info, v->info);
    set_text(s_big, v->big);
    set_text(s_line1, v->line1);
    set_text(s_line2, v->line2);
    if (s_line2) {
        if (v->line2[0]) lv_obj_remove_flag(s_line2, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_line2, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_timer) {
        if (v->timer < 0) buf[0] = '\0';
        else if (v->timer_down) snprintf(buf, sizeof(buf), "%ds", v->timer);
        else snprintf(buf, sizeof(buf), "%d:%02d", v->timer / 60, v->timer % 60);
        set_text(s_timer, buf);
    }
    seat_colors(v);

    if (s_rows[0]) {
        for (int r = 0; r < WWV_MENU_ROWS; r++) {
            if (!s_rows[r]) continue;
            int idx = v->top + r;
            bool sel = idx == v->sel;
            lv_obj_set_style_bg_color(s_rows[r], lv_color_hex(sel ? UI_YELLOW : UI_PAPER), 0);
            set_text(s_row_labels[r], idx < v->n_items ? v->items[idx] : "");
        }
        set_text(s_more_up, v->top > 0 ? "▲" : "");
        set_text(s_more_down, v->top + WWV_MENU_ROWS < v->n_items ? "▼" : "");
    }
    for (int i = 0; i < v->n_lines; i++) set_text(s_lines[i], v->lines[i]);
    if (s_vol_fill) lv_obj_set_width(s_vol_fill, v->volume * 172 / 100 < 1 ? 1 : v->volume * 172 / 100);
}

void ww_ui_render(const ww_view_t *v)
{
    // 视图一点没变就什么都不做:设样式即使值相同也会触发重绘,每 200 ms 重刷整屏
    // 会让 SPI 一直忙。视图里有秒表,所以对局中最多每秒更新一次。
    // (ww_host_view 先 memset 再填,结构体里的填充字节也是确定的,可以 memcmp。)
    static ww_view_t last;
    if (s_scr && s_sig[0] && memcmp(v, &last, sizeof(last)) == 0) return;
    memcpy(&last, v, sizeof(last));

    char sig[256];
    snprintf(sig, sizeof(sig), "%d|%d|%d|%d|%d|%s|%s", v->kind, v->night, v->n_seats, v->n_items,
             v->n_lines, v->title, v->qr);
    if (!s_scr || strcmp(sig, s_sig) != 0) {
        memcpy(s_sig, sig, sizeof(s_sig));
        build(v);
    }
    update(v);
}
