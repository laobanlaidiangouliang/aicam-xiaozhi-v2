#pragma once
#include <lvgl.h>
#include "display/lcd_display.h"

// 功能模式枚举（和业务层统一）
typedef enum
{
    UI_MODE_MAIN,       // 主菜单
    UI_MODE_PHOTO,      // 拍照界面
    UI_MODE_GALLERY,    // 照片浏览
    UI_MODE_SENSORS,    // 传感器数据
    UI_MODE_XIAOZHI,    // 进入小智语音
} AppUIMode;

// 初始化主界面
void ui_main_init(void);

// 切到自定义主屏
void enter_ui_main(void);

// UI 循环（放在板级 Run() 里轮询）
void ui_main_loop(void);

// 设置当前UI模式
void ui_set_mode(AppUIMode mode);

// 获取当前UI模式
AppUIMode ui_get_mode(void);

// 隐藏/显示整个自定义UI（切回小智时用）
void ui_set_visible(bool en);

bool ui_is_visible(void);

// 切换到自定义主屏幕
void ui_load_main_screen(void);

// 设置原生小智UI所在的屏幕（供ai_cam_board调用）
void ui_set_native_screen(lv_obj_t *scr);

// 更新主界面顶部时间显示（由时钟滴答调用，每秒一次）
void ui_main_update_time(void);
