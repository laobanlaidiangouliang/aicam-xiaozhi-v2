#include "ui_photo.h"
#include "ui_main.h"
#include "services/system_switch.h"
#include "../drivers/sdcard.h"
#include "esp_log.h"
#include <esp_lvgl_port.h>
#include <esp_camera.h>
#include "jpg/image_to_jpeg.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "UI_PHOTO";

#define PHOTO_WIDTH  320
#define PHOTO_HEIGHT 240

static lv_obj_t *g_photo_scr = nullptr;
static lv_obj_t *g_photo_img = nullptr;
static lv_timer_t *g_photo_timer = nullptr;
static uint8_t *g_preview_buf = nullptr;
static lv_image_dsc_t g_preview_dsc;

// 拍照成功提示
static lv_obj_t *g_success_label = nullptr;
static lv_timer_t *g_success_timer = nullptr;

// 将 RGB565 帧水平、垂直翻转，并转换为 LVGL/JPEG 编码器使用的字节序。
static bool copy_frame_flipped_rgb565(const camera_fb_t *fb, uint8_t *dst)
{
    if (!fb || !dst || fb->format != PIXFORMAT_RGB565 ||
        fb->width != PHOTO_WIDTH || fb->height != PHOTO_HEIGHT) {
        return false;
    }

    const uint16_t *src = (const uint16_t *)fb->buf;
    uint16_t *out = (uint16_t *)dst;
    for (size_t y = 0; y < PHOTO_HEIGHT; ++y) {
        size_t src_row = (PHOTO_HEIGHT - 1 - y) * PHOTO_WIDTH;
        size_t dst_row = y * PHOTO_WIDTH;
        for (size_t x = 0; x < PHOTO_WIDTH; ++x) {
            out[dst_row + x] = __builtin_bswap16(src[src_row + (PHOTO_WIDTH - 1 - x)]);
        }
    }
    return true;
}

static void success_timer_cb(lv_timer_t *timer)
{
    lvgl_port_lock(0);
    if (g_success_label) {
        lv_obj_add_flag(g_success_label, LV_OBJ_FLAG_HIDDEN);
    }
    g_success_timer = nullptr;
    lvgl_port_unlock();
}

// ==================== 预览定时器：~15fps ====================

static void photo_timer_cb(lv_timer_t *timer)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    if (g_preview_buf && g_photo_img && copy_frame_flipped_rgb565(fb, g_preview_buf)) {
        lv_obj_invalidate(g_photo_img);
    }

    esp_camera_fb_return(fb);
}

// ==================== 拍照：RGB565 → JPEG → SD卡 ====================

static void photo_capture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "拍照失败：未获取到摄像头帧");
        return;
    }

    size_t pixel_count = fb->width * fb->height;
    size_t data_size = pixel_count * 2;
    uint8_t *swapped = (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!swapped) {
        esp_camera_fb_return(fb);
        ESP_LOGE(TAG, "拍照失败：内存不足");
        return;
    }

    if (!copy_frame_flipped_rgb565(fb, swapped)) {
        heap_caps_free(swapped);
        esp_camera_fb_return(fb);
        ESP_LOGE(TAG, "拍照失败：只支持 320x240 RGB565 帧");
        return;
    }
    esp_camera_fb_return(fb);

    // RGB565 → JPEG 编码
    uint8_t *jpeg_out = nullptr;
    size_t jpeg_len = 0;
    bool ok = image_to_jpeg(swapped, data_size, PHOTO_WIDTH, PHOTO_HEIGHT,
                            V4L2_PIX_FMT_RGB565, 80, &jpeg_out, &jpeg_len);
    heap_caps_free(swapped);

    if (!ok || !jpeg_out) {
        ESP_LOGE(TAG, "JPEG 编码失败");
        return;
    }

    // 检查/创建 photo 子目录，并扫描已有文件确定起始编号
    static int photo_idx = -1;
    if (photo_idx < 0) {
        photo_idx = 0;
        mkdir("/sdcard/photo", 0755);  // 不存在则创建

        DIR *d = opendir("/sdcard/photo");
        if (d) {
            struct dirent *entry;
            int max_idx = 0;
            while ((entry = readdir(d)) != NULL) {
                int idx;
                if (sscanf(entry->d_name, "p%04d.jpg", &idx) == 1) {
                    if (idx > max_idx) max_idx = idx;
                }
            }
            closedir(d);
            photo_idx = max_idx;
            ESP_LOGI(TAG, "扫描已有照片: 最大编号=%d，下一张从 %d 开始", max_idx, max_idx + 1);
        }
    }

    photo_idx++;
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/sdcard/photo/p%04d.jpg", photo_idx);

    // 异步写入SD卡（通过专用任务，避免跨任务FatFS冲突）
    sdcard_task_save_jpeg(filepath, jpeg_out, jpeg_len);
    heap_caps_free(jpeg_out);  // 任务内部已memcpy，此处可立即释放
    ESP_LOGI(TAG, "照片已提交保存: %s (%zu bytes)", filepath, jpeg_len);

    // 显示拍照成功提示，1.5秒后自动消失
    if (g_success_label) {
        lv_obj_clear_flag(g_success_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_success_label);  // 确保在最上层
        if (g_success_timer) {
            lv_timer_del(g_success_timer);
        }
        g_success_timer = lv_timer_create(success_timer_cb, 1500, nullptr);
        lv_timer_set_repeat_count(g_success_timer, 1);
    }
}

// ==================== 返回主界面 ====================

static void photo_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ui_set_mode(UI_MODE_MAIN);
    system_switch_set_mode(MODE_UI_MAIN);
    ui_photo_screen_hide();
    ui_set_visible(true);
}

// ==================== 创建拍照预览界面 ====================

static void create_photo_screen(void)
{
    g_photo_scr = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(g_photo_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_photo_scr, LV_OPA_COVER, LV_PART_MAIN);

    // 全屏预览图像
    g_photo_img = lv_image_create(g_photo_scr);
    lv_obj_set_size(g_photo_img, PHOTO_WIDTH, PHOTO_HEIGHT);
    lv_obj_align(g_photo_img, LV_ALIGN_CENTER, 0, 0);

    // 预览缓冲区（SPIRAM）
    g_preview_buf = (uint8_t*)heap_caps_malloc(PHOTO_WIDTH * PHOTO_HEIGHT * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(g_preview_buf);

    // LVGL v9 图像描述符
    memset(&g_preview_dsc, 0, sizeof(g_preview_dsc));
    g_preview_dsc.header.w = PHOTO_WIDTH;
    g_preview_dsc.header.h = PHOTO_HEIGHT;
    g_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    g_preview_dsc.header.stride = PHOTO_WIDTH * 2;
    g_preview_dsc.data = g_preview_buf;
    g_preview_dsc.data_size = PHOTO_WIDTH * PHOTO_HEIGHT * 2;
    lv_image_set_src(g_photo_img, &g_preview_dsc);

    // 拍照按钮（底部居中，圆形白色边框）
    lv_obj_t *btn_capture = lv_btn_create(g_photo_scr);
    lv_obj_set_size(btn_capture, 64, 64);
    lv_obj_align(btn_capture, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(btn_capture, 32, 0);
    lv_obj_set_style_bg_color(btn_capture, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_capture, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_capture, 4, 0);
    lv_obj_set_style_border_color(btn_capture, lv_color_hex(0xAAAAAA), 0);
    lv_obj_add_event_cb(btn_capture, photo_capture_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *label_cap = lv_label_create(btn_capture);
    lv_label_set_text(label_cap, "拍");
    lv_obj_center(label_cap);
    lv_obj_set_style_text_color(label_cap, lv_color_hex(0x333333), 0);

    // 返回按钮（左上角，半透明）
    lv_obj_t *btn_back = lv_btn_create(g_photo_scr);
    lv_obj_set_size(btn_back, 64, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_70, 0);
    lv_obj_add_event_cb(btn_back, photo_back_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "<");
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, lv_color_white(), 0);

    // 拍照成功提示标签（默认隐藏，带半透明深色背景的 Toast 样式）
    g_success_label = lv_label_create(g_photo_scr);
    lv_label_set_text(g_success_label, "OK");
    lv_obj_set_style_text_color(g_success_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(g_success_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_bg_color(g_success_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_success_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(g_success_label, 16, 0);
    lv_obj_set_style_radius(g_success_label, 12, 0);
    lv_obj_align(g_success_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(g_success_label, LV_OBJ_FLAG_HIDDEN);
}

// ==================== 对外 API ====================
void ui_photo_screen_show(void)
{
    lvgl_port_lock(0);

    if (!g_photo_scr) {
        create_photo_screen();
    }

    lv_scr_load(g_photo_scr);

    if (!g_photo_timer) {
        g_photo_timer = lv_timer_create(photo_timer_cb, 66, NULL);
    } else {
        lv_timer_resume(g_photo_timer);
    }

    lvgl_port_unlock();
}

void ui_photo_screen_hide(void)
{
    lvgl_port_lock(0);

    if (g_photo_timer) {
        lv_timer_pause(g_photo_timer);
    }

    lvgl_port_unlock();
}
