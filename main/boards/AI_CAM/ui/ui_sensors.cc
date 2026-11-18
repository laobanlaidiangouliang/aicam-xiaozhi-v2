#include "ui_sensors.h"
#include "ui_main.h"
#include "services/system_switch.h"
#include "../drivers/sensor_driver.h"
#include "esp_log.h"
#include <esp_lvgl_port.h>
#include <cstdio>
#include <vector>

static const char *TAG = "UI_SENSORS";

static lv_obj_t *g_sensors_scr = nullptr;
static lv_obj_t *g_sensors_list = nullptr;       // 传感器列表容器
static lv_obj_t *g_sensors_title = nullptr;       // 标题
static lv_timer_t *g_refresh_timer = nullptr;     // 刷新定时器

// 每个传感器在列表中对应的 label 控件
struct SensorItemWidget {
    lv_obj_t *container;
    lv_obj_t *label_name;
    lv_obj_t *label_value;
};
static std::vector<SensorItemWidget> g_sensor_items;

// ==================== 刷新回调 ====================

static void refresh_sensor_list(void)
{
    auto& mgr = SensorManager::GetInstance();
    mgr.read_all();

    size_t count = mgr.count();
    for (size_t i = 0; i < count && i < g_sensor_items.size(); i++) {
        SensorBase* s = mgr.get(i);
        if (!s) continue;

        // 更新数值
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", s->value_str(), s->unit());
        lv_label_set_text(g_sensor_items[i].label_value, buf);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    refresh_sensor_list();
}

// ==================== 按钮回调 ====================

static void sensors_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // 停止定时器
    if (g_refresh_timer) {
        lv_timer_del(g_refresh_timer);
        g_refresh_timer = nullptr;
    }

    ui_set_mode(UI_MODE_MAIN);
    system_switch_set_mode(MODE_UI_MAIN);
    ui_sensors_screen_hide();
    ui_set_visible(true);
}

// ==================== 创建传感器卡片 ====================

static lv_obj_t* create_sensor_card(lv_obj_t *parent, const char *name_str, int index)
{
    // 卡片容器
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(90), 52);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x3A3A5E), 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    // 传感器名称（左侧）
    lv_obj_t *label_name = lv_label_create(card);
    lv_label_set_text(label_name, name_str);
    lv_obj_set_style_text_color(label_name, lv_color_hex(0xCCCCDD), 0);
    lv_obj_set_style_text_font(label_name, &lv_font_montserrat_14, 0);
    lv_obj_align(label_name, LV_ALIGN_LEFT_MID, 16, 0);

    // 传感器数值（右侧）
    lv_obj_t *label_value = lv_label_create(card);
    lv_label_set_text(label_value, "--");
    lv_obj_set_style_text_color(label_value, lv_color_hex(0x00DDFF), 0);
    lv_obj_set_style_text_font(label_value, &lv_font_montserrat_14, 0);
    lv_obj_align(label_value, LV_ALIGN_RIGHT_MID, -16, 0);

    // 记录控件
    SensorItemWidget item;
    item.container = card;
    item.label_name = label_name;
    item.label_value = label_value;
    g_sensor_items.push_back(item);

    return card;
}

// ==================== 创建界面 ====================

static void create_sensors_screen(void)
{
    g_sensors_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_sensors_scr, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_sensors_scr, LV_OPA_COVER, LV_PART_MAIN);

    // ── 返回按钮（左上角）──
    lv_obj_t *btn_back = lv_btn_create(g_sensors_scr);
    lv_obj_set_size(btn_back, 64, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_70, 0);
    lv_obj_add_event_cb(btn_back, sensors_back_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text_static(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, lv_color_white(), 0);

    // ── 标题（顶部居中）──
    g_sensors_title = lv_label_create(g_sensors_scr);
    lv_label_set_text_static(g_sensors_title, "Sensors");
    lv_obj_set_style_text_color(g_sensors_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_sensors_title, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sensors_title, LV_ALIGN_TOP_MID, 0, 12);

    // ── 传感器列表（可滚动容器）──
    g_sensors_list = lv_obj_create(g_sensors_scr);
    lv_obj_set_size(g_sensors_list, LV_PCT(95), LV_PCT(70));
    lv_obj_align(g_sensors_list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(g_sensors_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_sensors_list, 0, 0);
    lv_obj_set_style_pad_all(g_sensors_list, 4, 0);
    lv_obj_set_flex_flow(g_sensors_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_sensors_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(g_sensors_list, LV_SCROLLBAR_MODE_AUTO);
    // 列表项间距
    lv_obj_set_style_pad_row(g_sensors_list, 8, 0);

    // // ── 空状态提示（无传感器时显示）──
    // lv_obj_t *hint = lv_label_create(g_sensors_scr);
    // // lv_label_set_text_static(hint, "No sensors registered.\nAdd sensors via SensorManager.");
    // lv_obj_set_style_text_color(hint, lv_color_hex(0x666688), 0);
    // lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    // lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    // lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

// ==================== 对外 API ====================

void ui_sensors_screen_show(void)
{
    lvgl_port_lock(0);

    if (!g_sensors_scr) {
        create_sensors_screen();
    }

    // 清空旧的列表项控件记录
    g_sensor_items.clear();

    // 清空列表容器内所有子控件（保留容器本身）
    lv_obj_clean(g_sensors_list);

    // 根据已注册的传感器创建卡片
    auto& mgr = SensorManager::GetInstance();
    size_t count = mgr.count();

    ESP_LOGI(TAG, "Loading sensor screen, %zu sensor(s) found", count);

    for (size_t i = 0; i < count; i++) {
        SensorBase* s = mgr.get(i);
        if (!s) continue;

        lv_obj_t *card = create_sensor_card(g_sensors_list, s->name(), (int)i);
        // 首次读取
        s->read();
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", s->value_str(), s->unit());
        lv_label_set_text(g_sensor_items.back().label_value, buf);
    }

    lv_scr_load(g_sensors_scr);

    // 启动定时刷新（每秒刷新一次）
    if (!g_refresh_timer) {
        g_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, nullptr);
    }

    ESP_LOGI(TAG, "Sensor screen shown, refresh timer started");

    lvgl_port_unlock();
}

void ui_sensors_screen_hide(void)
{
    lvgl_port_lock(0);

    // 停止定时刷新
    if (g_refresh_timer) {
        lv_timer_del(g_refresh_timer);
        g_refresh_timer = nullptr;
    }

    g_sensor_items.clear();

    lvgl_port_unlock();
}
