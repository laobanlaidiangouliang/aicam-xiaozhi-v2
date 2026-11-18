#include "ui_gallery.h"
#include "ui_main.h"
#include "services/system_switch.h"
#include "services/photo_server.h"
#include "../drivers/sdcard.h"
#include "esp_log.h"
#include <esp_lvgl_port.h>
#include "jpg/jpeg_to_image.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static const char *TAG = "UI_GALLERY";

static lv_obj_t *g_gallery_scr = nullptr;
static lv_obj_t *g_gallery_img = nullptr;
static lv_obj_t *g_gallery_dropdown = nullptr;
static lv_obj_t *g_gallery_count_label = nullptr;
static lv_obj_t *g_gallery_download_label = nullptr;
static lv_obj_t *g_gallery_prev_btn = nullptr;
static lv_obj_t *g_gallery_next_btn = nullptr;

static uint8_t *g_decoded_buf = nullptr;  // 当前解码的RGB565图像数据
static int g_photo_count = 0;
static int g_curr_index = -1;

// 下拉列表选项缓存（LVGL需要静态/全局字符串）
static std::string g_dropdown_options;

// ==================== 加载并显示照片 ====================

static void load_and_display_photo(int index)
{
    if (index < 0 || index >= g_photo_count) return;

    const char *path = sdcard_photo_path(index);
    if (!path) {
        ESP_LOGE(TAG, "无效的照片索引: %d", index);
        return;
    }

    ESP_LOGI(TAG, "加载照片: %s", path);

    // 读取JPEG文件到内存
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "无法打开文件: %s", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *jpeg_buf = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "内存不足，无法加载JPEG文件 (%zu bytes)", file_size);
        fclose(f);
        return;
    }

    size_t read_size = fread(jpeg_buf, 1, file_size, f);
    fclose(f);

    if (read_size != file_size) {
        ESP_LOGE(TAG, "读取文件不完整: %zu/%zu", read_size, file_size);
        heap_caps_free(jpeg_buf);
        return;
    }

    // 释放上一次解码的图像缓冲区
    if (g_decoded_buf) {
        heap_caps_free(g_decoded_buf);
        g_decoded_buf = nullptr;
    }

    // JPEG → RGB565 解码
    size_t out_len = 0, width = 0, height = 0, stride = 0;
    esp_err_t ret = jpeg_to_image(jpeg_buf, file_size, &g_decoded_buf, &out_len, &width, &height, &stride);
    heap_caps_free(jpeg_buf);

    if (ret != ESP_OK || !g_decoded_buf) {
        ESP_LOGE(TAG, "JPEG解码失败: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "解码成功: %zux%zu, stride=%zu, data=%zu bytes", width, height, stride, out_len);

    // 更新LVGL图像
    lv_image_dsc_t *img_dsc = (lv_image_dsc_t*)malloc(sizeof(lv_image_dsc_t));
    if (!img_dsc) {
        heap_caps_free(g_decoded_buf);
        g_decoded_buf = nullptr;
        return;
    }

    memset(img_dsc, 0, sizeof(lv_image_dsc_t));
    img_dsc->header.w = (lv_coord_t)width;
    img_dsc->header.h = (lv_coord_t)height;
    img_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc->header.stride = (lv_coord_t)(stride > 0 ? stride : width * 2);
    img_dsc->data = g_decoded_buf;
    img_dsc->data_size = out_len;

    lv_image_set_src(g_gallery_img, img_dsc);

    // 更新计数标签
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%d/%d", index + 1, g_photo_count);
    lv_label_set_text(g_gallery_count_label, count_text);

    // 更新下拉列表选中项
    lv_dropdown_set_selected(g_gallery_dropdown, (uint16_t)index);

    g_curr_index = index;

    // 更新按钮状态
    if (g_curr_index <= 0) {
        lv_obj_add_state(g_gallery_prev_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g_gallery_prev_btn, LV_STATE_DISABLED);
    }
    if (g_curr_index >= g_photo_count - 1) {
        lv_obj_add_state(g_gallery_next_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g_gallery_next_btn, LV_STATE_DISABLED);
    }
}

// ==================== 按钮回调 ====================

static void gallery_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ui_set_mode(UI_MODE_MAIN);
    system_switch_set_mode(MODE_UI_MAIN);
    ui_gallery_screen_hide();
    ui_set_visible(true);
}

static void gallery_prev_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_curr_index > 0) {
        load_and_display_photo(g_curr_index - 1);
    }
}

static void gallery_next_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_curr_index < g_photo_count - 1) {
        load_and_display_photo(g_curr_index + 1);
    }
}

static void gallery_list_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (!g_gallery_dropdown) return;

    uint16_t index = lv_dropdown_get_selected(g_gallery_dropdown);
    ESP_LOGI(TAG, "下拉切换到第 %d 张", index);
    load_and_display_photo((int)index);
}

// ==================== 构建下拉选项字符串 ====================

static void build_dropdown_options(void)
{
    g_dropdown_options.clear();
    for (int i = 0; i < g_photo_count; i++) {
        const char *name = sdcard_photo_name(i);
        if (name) {
            if (i > 0) g_dropdown_options += "\n";
            g_dropdown_options += name;
        }
    }
}

// ==================== 创建界面 ====================

static void create_gallery_screen(void)
{
    g_gallery_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_gallery_scr, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_gallery_scr, LV_OPA_COVER, LV_PART_MAIN);

    // ── 返回按钮（左上角）──
    lv_obj_t *btn_back = lv_btn_create(g_gallery_scr);
    lv_obj_set_size(btn_back, 64, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_70, 0);
    lv_obj_add_event_cb(btn_back, gallery_back_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text_static(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, lv_color_white(), 0);

    // ── 照片计数标签（顶部居中）──
    g_gallery_count_label = lv_label_create(g_gallery_scr);
    lv_label_set_text_static(g_gallery_count_label, "0/0");
    lv_obj_set_style_text_color(g_gallery_count_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_gallery_count_label, &lv_font_montserrat_14, 0);
    lv_obj_align(g_gallery_count_label, LV_ALIGN_TOP_MID, 0, 30);

    // ── 照片显示区域（居中，320x240）──
    g_gallery_img = lv_image_create(g_gallery_scr);
    lv_obj_set_size(g_gallery_img, 320, 240);
    lv_obj_align(g_gallery_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_gallery_img, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_gallery_img, LV_OPA_COVER, 0);

    // ── 上一张按钮（左侧）──
    g_gallery_prev_btn = lv_btn_create(g_gallery_scr);
    lv_obj_set_size(g_gallery_prev_btn, 48, 48);
    lv_obj_set_style_radius(g_gallery_prev_btn, 24, 0);
    lv_obj_align(g_gallery_prev_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(g_gallery_prev_btn, lv_color_hex(0x333333), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_gallery_prev_btn, LV_OPA_70, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(g_gallery_prev_btn, gallery_prev_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_prev = lv_label_create(g_gallery_prev_btn);
    lv_label_set_text_static(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_prev, lv_color_white(), 0);
    lv_obj_center(label_prev);

    // ── 下一张按钮（右侧）──
    g_gallery_next_btn = lv_btn_create(g_gallery_scr);
    lv_obj_set_size(g_gallery_next_btn, 48, 48);
    lv_obj_set_style_radius(g_gallery_next_btn, 24, 0);
    lv_obj_align(g_gallery_next_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(g_gallery_next_btn, lv_color_hex(0x333333), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_gallery_next_btn, LV_OPA_70, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(g_gallery_next_btn, gallery_next_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_next = lv_label_create(g_gallery_next_btn);
    lv_label_set_text_static(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_next, lv_color_white(), 0);
    lv_obj_center(label_next);

    // ── 照片下拉列表（底部）──
    g_gallery_dropdown = lv_dropdown_create(g_gallery_scr);
    lv_dropdown_clear_options(g_gallery_dropdown);
    lv_dropdown_set_options_static(g_gallery_dropdown, "No Photos");
    lv_obj_set_style_text_font(g_gallery_dropdown, &lv_font_montserrat_14, LV_STATE_ANY);
    lv_obj_set_style_text_color(g_gallery_dropdown, lv_color_white(), LV_STATE_ANY);
    lv_obj_set_style_bg_color(g_gallery_dropdown, lv_color_hex(0x2A2A3E), LV_STATE_ANY);
    lv_obj_set_style_border_color(g_gallery_dropdown, lv_color_hex(0x555577), LV_STATE_ANY);
    lv_obj_set_size(g_gallery_dropdown, 220, 36);
    lv_obj_align(g_gallery_dropdown, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(g_gallery_dropdown, gallery_list_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ── 照片下载地址（右上角，放在照片图像之后创建以确保可见）──
    g_gallery_download_label = lv_label_create(g_gallery_scr);
    lv_label_set_text_static(g_gallery_download_label, "Wi-Fi 未连接");
    lv_obj_set_width(g_gallery_download_label, 210);
    lv_label_set_long_mode(g_gallery_download_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(g_gallery_download_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(g_gallery_download_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(g_gallery_download_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(g_gallery_download_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_gallery_download_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(g_gallery_download_label, 4, 0);
    lv_obj_align(g_gallery_download_label, LV_ALIGN_TOP_RIGHT, -8, 8);
}

// ==================== 对外 API ====================

void ui_gallery_screen_show(void)
{
    lvgl_port_lock(0);

    if (!g_gallery_scr) {
        create_gallery_screen();
    }

    std::string download_address = photo_server_get_address();
    lv_label_set_text(g_gallery_download_label, download_address.c_str());
    ESP_LOGI(TAG, "相册下载地址标签: %s", download_address.c_str());

    // 扫描SD卡中的照片
    g_photo_count = sdcard_scan_photo_jpeg();
    ESP_LOGI(TAG, "扫描到 %d 张照片", g_photo_count);

    // 构建下拉选项并更新
    build_dropdown_options();
    lv_dropdown_clear_options(g_gallery_dropdown);
    if (g_photo_count > 0) {
        lv_dropdown_set_options_static(g_gallery_dropdown, g_dropdown_options.c_str());
    } else {
        lv_dropdown_set_options_static(g_gallery_dropdown, "No Photos");
    }

    lv_scr_load(g_gallery_scr);

    // 加载第一张照片
    if (g_photo_count > 0) {
        g_curr_index = -1;  // 强制重新加载
        load_and_display_photo(0);
    } else {
        g_curr_index = -1;
        lv_label_set_text(g_gallery_count_label, "0/0");
        lv_obj_add_state(g_gallery_prev_btn, LV_STATE_DISABLED);
        lv_obj_add_state(g_gallery_next_btn, LV_STATE_DISABLED);
    }
    lvgl_port_unlock();
}

void ui_gallery_screen_hide(void)
{
    lvgl_port_lock(0);

    // 释放解码缓冲区
    if (g_decoded_buf) {
        heap_caps_free(g_decoded_buf);
        g_decoded_buf = nullptr;
    }
    g_curr_index = -1;

    lvgl_port_unlock();
}
