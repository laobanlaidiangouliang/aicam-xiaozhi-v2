#include "sdcard.h"
#include "config.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <errno.h>
#include <string.h>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <string>


static const char *TAG = "SDCARD";
static sdmmc_card_t *card = nullptr;
static bool s_mounted = false;
static SemaphoreHandle_t sdcard_mutex = nullptr;
#define MOUNT_POINT "/sdcard"

// ==================== SD卡挂载/卸载 ====================

esp_err_t sdcard_deinit(void) {
    if (!s_mounted) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unmount failed: %s", esp_err_to_name(ret));
    }
    card = nullptr;
    s_mounted = false;
    return ret;
}

esp_err_t sdcard_init(void) {
    if (!sdcard_mutex) {
        sdcard_mutex = xSemaphoreCreateMutex();
        if (!sdcard_mutex) {
            ESP_LOGE(TAG, "Failed to create SD card mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_mounted) {
        ESP_LOGW(TAG, "Already mounted, unmounting first");
        sdcard_deinit();
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk   = GPIO_NUM_47;
    slot_config.cmd   = GPIO_NUM_48;
    slot_config.d0    = GPIO_NUM_21;
    slot_config.d1    = GPIO_NUM_NC;
    slot_config.d2    = GPIO_NUM_NC;
    slot_config.d3    = GPIO_NUM_NC;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        card = nullptr;
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

// ==================== 同步文件写入 ====================

esp_err_t sdcard_save_file(const char* path, const uint8_t* data, size_t len) {
    if (!sdcard_begin_access()) {
        ESP_LOGE(TAG, "SD未挂载");
        return ESP_ERR_INVALID_STATE;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen failed for %s: errno=%d (%s)",
                 path, errno, strerror(errno));
        sdcard_end_access();
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    sdcard_end_access();

    if (written != len) {
        ESP_LOGE(TAG, "Short write: %d of %d bytes", (int)written, (int)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %d bytes to %s", (int)len, path);
    return ESP_OK;
}

bool sdcard_is_mounted(void) {
    return s_mounted;
}

bool sdcard_begin_access(void) {
    if (!sdcard_mutex || xSemaphoreTake(sdcard_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!s_mounted || !card) {
        xSemaphoreGive(sdcard_mutex);
        return false;
    }
    return true;
}

void sdcard_end_access(void) {
    if (sdcard_mutex) {
        xSemaphoreGive(sdcard_mutex);
    }
}

// ==================== 异步SD写入任务（避免跨任务FatFS冲突） ====================

struct SdSaveMsg {
    char filepath[64];
    uint8_t *data;
    size_t len;
};

static QueueHandle_t g_sd_queue = nullptr;
static TaskHandle_t g_sd_task_handle = nullptr;

static void sdcard_save_task(void *arg) {
    SdSaveMsg msg;
    while (true) {
        if (xQueueReceive(g_sd_queue, &msg, portMAX_DELAY) == pdTRUE) {
            esp_err_t ret = sdcard_save_file(msg.filepath, msg.data, msg.len);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "异步写入成功: %s (%zu bytes)", msg.filepath, msg.len);
            } else {
                ESP_LOGE(TAG, "异步写入失败: %s (err=%d)", msg.filepath, ret);
            }
            free(msg.data);
        }
    }
}

void sdcard_task_start(void) {
    if (g_sd_queue) return;

    g_sd_queue = xQueueCreate(4, sizeof(SdSaveMsg));
    if (!g_sd_queue) {
        ESP_LOGE(TAG, "创建SD写入队列失败");
        return;
    }

    BaseType_t ret = xTaskCreate(
        sdcard_save_task,
        "sdcard_save",
        4096,
        nullptr,
        tskIDLE_PRIORITY + 2,
        &g_sd_task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建SD写入任务失败");
        vQueueDelete(g_sd_queue);
        g_sd_queue = nullptr;
        return;
    }

    ESP_LOGI(TAG, "SD写入任务已启动");
}

void sdcard_task_save_jpeg(const char *filepath, const uint8_t *data, size_t len) {
    if (!g_sd_queue || !data || !filepath) return;

    SdSaveMsg msg;
    strncpy(msg.filepath, filepath, sizeof(msg.filepath) - 1);
    msg.filepath[sizeof(msg.filepath) - 1] = '\0';
    msg.len = len;

    msg.data = (uint8_t*)malloc(len);
    if (!msg.data) {
        ESP_LOGE(TAG, "malloc失败，无法入队: %s", filepath);
        return;
    }
    memcpy(msg.data, data, len);

    if (xQueueSend(g_sd_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "SD写入队列已满，丢弃: %s", filepath);
        free(msg.data);
    }
}

// ==================== 音乐文件扫描 ====================

static std::vector<std::string> g_music_names;  // 仅文件名
static std::vector<std::string> g_music_paths;  // 完整路径

int sdcard_scan_music_mp3(void) {
    g_music_names.clear();
    g_music_paths.clear();

    // 确保 music 目录存在
    mkdir("/sdcard/music", 0755);

    DIR *d = opendir("/sdcard/music");
    if (!d) {
        ESP_LOGW(TAG, "无法打开 /sdcard/music 目录");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        // 检查 .mp3 后缀（不区分大小写）
        if (len > 4 && strcasecmp(name + len - 4, ".mp3") == 0) {
            g_music_names.push_back(std::string(name));
            g_music_paths.push_back(std::string("/sdcard/music/") + name);
        }
    }
    closedir(d);

    // 按文件名排序
    std::sort(g_music_names.begin(), g_music_names.end());
    std::sort(g_music_paths.begin(), g_music_paths.end());

    ESP_LOGI(TAG, "扫描到 %d 首 MP3 文件", (int)g_music_names.size());
    return (int)g_music_names.size();
}

const char* sdcard_music_name(int index) {
    if (index < 0 || index >= (int)g_music_names.size()) return nullptr;
    return g_music_names[index].c_str();
}

const char* sdcard_music_path(int index) {
    if (index < 0 || index >= (int)g_music_paths.size()) return nullptr;
    return g_music_paths[index].c_str();
}

void sdcard_music_free(void) {
    g_music_names.clear();
    g_music_paths.clear();
}

// ==================== 照片文件扫描 ====================

static std::vector<std::string> g_photo_names;  // 仅文件名
static std::vector<std::string> g_photo_paths;  // 完整路径

int sdcard_scan_photo_jpeg(void) {
    g_photo_names.clear();
    g_photo_paths.clear();

    // 确保 photo 目录存在
    mkdir("/sdcard/photo", 0755);

    DIR *d = opendir("/sdcard/photo");
    if (!d) {
        ESP_LOGW(TAG, "无法打开 /sdcard/photo 目录");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        // 检查 .jpg 后缀（不区分大小写）
        if (len > 4 && strcasecmp(name + len - 4, ".jpg") == 0) {
            g_photo_names.push_back(std::string(name));
            g_photo_paths.push_back(std::string("/sdcard/photo/") + name);
        }
    }
    closedir(d);

    // 按文件名排序
    std::sort(g_photo_names.begin(), g_photo_names.end());
    std::sort(g_photo_paths.begin(), g_photo_paths.end());

    ESP_LOGI(TAG, "扫描到 %d 张 JPEG 照片", (int)g_photo_names.size());
    return (int)g_photo_names.size();
}

const char* sdcard_photo_name(int index) {
    if (index < 0 || index >= (int)g_photo_names.size()) return nullptr;
    return g_photo_names[index].c_str();
}

const char* sdcard_photo_path(int index) {
    if (index < 0 || index >= (int)g_photo_paths.size()) return nullptr;
    return g_photo_paths[index].c_str();
}

void sdcard_photo_free(void) {
    g_photo_names.clear();
    g_photo_paths.clear();
}
