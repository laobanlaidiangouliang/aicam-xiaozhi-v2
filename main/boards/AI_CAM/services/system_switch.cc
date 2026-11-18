#include "system_switch.h"
#include "esp_log.h"
#include "application.h"
#include "device_state.h"

static const char *TAG = "SYS_SWITCH";
static SystemMode g_sys_mode = MODE_UI_MAIN;

void system_switch_init(void)
{
    g_sys_mode = MODE_UI_MAIN; // 上电默认主界面
    // 暂停原生小智的对话状态
    auto& app = Application::GetInstance();
    app.SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "System switch init, default to UI mode");
}

void system_switch_loop(void)
{
    // 后续在这里加：拍照/音乐/小智 后台逻辑
}

void system_switch_set_mode(SystemMode mode)
{
    g_sys_mode = mode;
    ESP_LOGI(TAG, "Switch to mode: %d", mode);
}

SystemMode system_switch_get_mode(void)
{
    return g_sys_mode;
}
