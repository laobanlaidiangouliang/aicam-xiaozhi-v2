#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdcard_init(void);
esp_err_t sdcard_deinit(void);
esp_err_t sdcard_save_file(const char* path, const uint8_t* data, size_t len);
bool      sdcard_is_mounted(void);
bool      sdcard_begin_access(void);
void      sdcard_end_access(void);

// 启动SD卡写入任务（在sdcard_init成功后调用，避免跨任务FatFS冲突）
void sdcard_task_start(void);

// 异步保存JPEG到SD卡（内部memcpy数据，调用方可立即释放原缓冲区）
void sdcard_task_save_jpeg(const char *filepath, const uint8_t *data, size_t len);

// ==================== 音乐文件扫描 ====================

// 扫描 /sdcard/music/ 目录下的 .mp3 文件，返回文件数量（-1表示出错）
int sdcard_scan_music_mp3(void);

// 获取第 index 首的文件名（仅文件名，用于UI列表显示）
const char* sdcard_music_name(int index);

// 获取第 index 首的完整路径（用于播放器打开）
const char* sdcard_music_path(int index);

// 释放缓存的音乐文件列表
void sdcard_music_free(void);

// ==================== 照片文件扫描 ====================

// 扫描 /sdcard/photo/ 目录下的 .jpg 文件，返回文件数量（-1表示出错）
int sdcard_scan_photo_jpeg(void);

// 获取第 index 张照片的文件名（仅文件名，用于UI列表显示）
const char* sdcard_photo_name(int index);

// 获取第 index 张照片的完整路径（用于图片加载）
const char* sdcard_photo_path(int index);

// 释放缓存的照片文件列表
void sdcard_photo_free(void);

#ifdef __cplusplus
}
#endif
