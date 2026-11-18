#include "ui_main.h"
#include "ui_photo.h"
#include "ui_gallery.h"
#include "ui_sensors.h"
#include "services/system_switch.h"
#include "esp_log.h"
#include <esp_lvgl_port.h>
#include <ctime>

static const char *TAG = "UI_MAIN";

static lv_obj_t *g_main_scr   = nullptr;
static lv_obj_t *g_native_scr = nullptr;
static lv_obj_t *g_time_label = nullptr;
static AppUIMode  g_curr_mode  = UI_MODE_MAIN;
static bool       g_ui_visible = true;
static bool       g_ui_inited  = false;

// ==================== 图标样式 ====================
/*
 * 320×240 布局
 *
 *  ┌──────────────────────────────────┐ y=0
 *  │  Main Menu            WIFI  BT   │ 顶栏 h=32
 *  ├──────────────────────────────────┤ y=32
 *  │                                  │
 *  │  [Camera] [Photos] [Xiaozhi] [Sensors] │ 图标区 y=52~220
 *  │                                  │
 *  └──────────────────────────────────┘ y=240
 *
 *  4个图标横排，每个宽64，间距10
 *  总宽 = 4*64 + 3*10 = 256 + 30 = 286
 *  左边距 = (320 - 286) / 2 = 17
 *  图标高 = 168  (y=36 ~ y=204)
 *  各图标 x 起点: 17, 91, 165, 239
 */

#define ICON_W       64
#define ICON_H       148
#define ICON_GAP     10
#define ICON_X0      17      // 第一个图标左边 x
#define ICON_Y       44      // 图标顶部 y
#define ICON_RADIUS  16

static lv_style_t g_icon_style;
static lv_style_t g_icon_pressed;

static void icon_style_init(void)
{
    lv_style_init(&g_icon_style);
    lv_style_set_radius(&g_icon_style, ICON_RADIUS);
    lv_style_set_bg_opa(&g_icon_style, LV_OPA_COVER);
    lv_style_set_border_width(&g_icon_style, 0);
    lv_style_set_pad_all(&g_icon_style, 0);
    lv_style_set_shadow_width(&g_icon_style, 10);
    lv_style_set_shadow_opa(&g_icon_style, LV_OPA_40);
    lv_style_set_shadow_color(&g_icon_style, lv_color_hex(0x000000));
    lv_style_set_shadow_ofs_y(&g_icon_style, 5);

    lv_style_init(&g_icon_pressed);
    lv_style_set_transform_zoom(&g_icon_pressed, 245);  // 按下轻微缩小
    lv_style_set_shadow_opa(&g_icon_pressed, LV_OPA_20);
}

/*
 * 斜光遮罩层：左上亮、右下暗，模拟图片里的光泽效果
 * 用一个半透明白色渐变 obj 叠在按钮上
 */
static void add_gloss(lv_obj_t *parent)
{
    lv_obj_t *gloss = lv_obj_create(parent);
    lv_obj_set_size(gloss, ICON_W, ICON_H / 2);
    lv_obj_set_pos(gloss, 0, 0);
    lv_obj_set_style_radius(gloss, ICON_RADIUS, 0);
    lv_obj_set_style_bg_color(gloss, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(gloss, LV_OPA_10, 0);
    lv_obj_set_style_bg_grad_color(gloss, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_grad_dir(gloss, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(gloss, 0, 0);
    lv_obj_set_style_bg_grad_stop(gloss, 255, 0);
    lv_obj_set_style_bg_opa(gloss, LV_OPA_20, 0);
    lv_obj_set_style_border_width(gloss, 0, 0);
    lv_obj_clear_flag(gloss, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(gloss, LV_OBJ_FLAG_SCROLLABLE);
}

/*
 * 创建单个横排图标
 *   col       — 列序号 0~3
 *   bg_color  — 图标主色
 *   symbol    — LV_SYMBOL_*
 *   label_text— 图标内底部文字
 *   cb        — 点击回调
 */
static void create_icon(lv_obj_t    *parent,
                         int          col,
                         uint32_t     bg_color,
                         const char  *symbol,
                         const char  *label_text,
                         lv_event_cb_t cb)
{
    lv_coord_t x = ICON_X0 + col * (ICON_W + ICON_GAP);
    lv_coord_t y = ICON_Y;

    // 主按钮
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_style(btn, &g_icon_style, LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &g_icon_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, ICON_W, ICON_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_STATE_DEFAULT);
    // 图标内渐变：底部略深，增加立体感
    lv_obj_set_style_bg_grad_color(btn, lv_color_mix(lv_color_hex(bg_color),
                                   lv_color_hex(0x000000), 180), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    // 符号，放在按钮上半部居中
    lv_obj_t *sym = lv_label_create(btn);
    lv_label_set_text(sym, symbol);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sym, lv_color_hex(0xffffff), 0);
    lv_obj_align(sym, LV_ALIGN_CENTER, 0, -12);  // 稍微偏上，给底部文字留空间

    // 文字，贴在按钮内部底部
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    // 斜光层
    add_gloss(btn);
}

// ==================== 按钮回调 ====================

static void btn_photo_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Camera");
        ui_set_mode(UI_MODE_PHOTO);
        system_switch_set_mode(MODE_PHOTO);
        ui_photo_screen_show();
    }
}

static void btn_gallery_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Gallery");
        ui_set_mode(UI_MODE_GALLERY);
        system_switch_set_mode(MODE_GALLERY);
        ui_gallery_screen_show();
    }
}

static void btn_xiaozhi_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Xiaozhi");
        ui_set_mode(UI_MODE_XIAOZHI);
        system_switch_set_mode(MODE_XIAOZHI);
        ui_set_visible(false);
    }
}

static void btn_sensors_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Sensors");
        ui_set_mode(UI_MODE_SENSORS);
        system_switch_set_mode(MODE_SENSORS);
        ui_sensors_screen_show();
    }
}

// ==================== 创建主菜单界面 ====================

static void create_main_screen(void)
{
    icon_style_init();

    g_main_scr = lv_obj_create(NULL);
    lv_scr_load(g_main_scr);
    lv_obj_clear_flag(g_main_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 背景：深蓝灰，类似图片里的深色底板
    lv_obj_set_style_bg_color(g_main_scr, lv_color_hex(0x1a2233), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_main_scr, lv_color_hex(0x0d1520), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_main_scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_main_scr, LV_OPA_COVER, LV_PART_MAIN);

    // ---------- 顶部状态栏 ----------
    lv_obj_t *bar = lv_obj_create(g_main_scr);
    lv_obj_set_size(bar, 320, 36);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // 左侧标题
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "AI CAM    XIAO");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8ab4d4), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    // 中间时间（初始显示 --:--，服务器下发时间后自动更新）
    g_time_label = lv_label_create(bar);
    lv_label_set_text(g_time_label, "--:--");
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(g_time_label, LV_ALIGN_CENTER, 0, 0);

    // 右侧 Wi-Fi 图标
    lv_obj_t *status = lv_label_create(bar);
    lv_label_set_text(status, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(0x8ab4d4), 0);
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -10, 0);

    // 顶栏分隔线
    lv_obj_t *sep = lv_obj_create(g_main_scr);
    lv_obj_set_size(sep, 320, 1);
    lv_obj_set_pos(sep, 0, 36);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x2a3f5f), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    // ---------- 4个横排图标 ----------
    // col=0  Camera  紫粉色（对应图片COPY）
    create_icon(g_main_scr, 0, 0xc2185b, LV_SYMBOL_EYE_OPEN,       "CAMERA",  btn_photo_cb);
    // col=1  Photos  蓝紫色（对应图片SCAN）
    create_icon(g_main_scr, 1, 0x3949ab, LV_SYMBOL_IMAGE,          "PHOTOS",  btn_gallery_cb);
    // col=2  Xiaozhi 绿色（对应图片PRINT）
    create_icon(g_main_scr, 2, 0x2e7d32, LV_SYMBOL_CALL,           "XIAOZHI", btn_xiaozhi_cb);
    // col=3  Sensors 红色（对应图片SETUP）
    create_icon(g_main_scr, 3, 0xc62828, LV_SYMBOL_LIST,           "SENSORS", btn_sensors_cb);

    g_main_scr = lv_scr_act();
}

// ==================== 对外 API ====================

void ui_main_init(void)
{
    system_switch_init();
    lvgl_port_lock(0);
    if (g_ui_inited) { lvgl_port_unlock(); return; }
    g_ui_inited  = true;
    g_curr_mode  = UI_MODE_MAIN;
    g_ui_visible = true;
    create_main_screen();
    ESP_LOGI(TAG, "Main UI init ok");
    lvgl_port_unlock();
}

void enter_ui_main(void)
{
    lvgl_port_lock(0);
    lv_scr_load_anim(g_main_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    lvgl_port_unlock();
}

void ui_main_loop(void)
{
    if (!g_ui_visible) return;
    lv_timer_handler();
}

void ui_set_mode(AppUIMode mode) { g_curr_mode = mode; }
AppUIMode ui_get_mode(void)      { return g_curr_mode; }

void ui_set_visible(bool en)
{
    g_ui_visible = en;
    if (en) {
        if (g_main_scr)   lv_scr_load_anim(g_main_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    } else {
        if (g_native_scr) lv_scr_load(g_native_scr);
    }
}

bool ui_is_visible(void)         { return g_ui_visible; }

void ui_load_main_screen(void)
{
    if (g_main_scr != nullptr) lv_scr_load(g_main_scr);
}

void ui_set_native_screen(lv_obj_t *scr) { g_native_scr = scr; }

void ui_main_update_time(void)
{
    if (g_time_label == nullptr) return;

    lvgl_port_lock(0);

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);

    // 只有服务器下发过时间后才更新（年份 >= 2025）
    if (tm->tm_year >= 2025 - 1900) {
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", tm);
        lv_label_set_text(g_time_label, time_str);
    }

    lvgl_port_unlock();
}

