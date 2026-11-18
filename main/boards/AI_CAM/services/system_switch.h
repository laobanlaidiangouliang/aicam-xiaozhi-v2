#pragma once
#include <stdint.h>

// 全局系统模式（和UI一一对应）
typedef enum
{
    MODE_UI_MAIN,
    MODE_PHOTO,
    MODE_GALLERY,
    MODE_SENSORS,
    MODE_XIAOZHI
} SystemMode;

void system_switch_init(void);
void system_switch_loop(void);
void system_switch_set_mode(SystemMode mode);
SystemMode system_switch_get_mode(void);
