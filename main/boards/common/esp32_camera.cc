#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <esp_log.h>
#include <img_converters.h>

#include "esp32_camera.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Esp32Camera"

static void log_camera_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "%s heap: internal free=%u largest=%u min=%u; PSRAM free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

Esp32Camera::Esp32Camera(const camera_config_t &config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0); // Control camera mirror: 1 for mirror, 0 for normal
        }
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (streaming_on_) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string &url, const std::string &token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    log_camera_heap("Capture begin");
    if (!streaming_on_) {
        return false;
    }

    // Get the latest frame, discard old frames for real-time performance
    for (int i = 0; i < 2; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Rotate/swap the owned camera frame in place to avoid another full-frame buffer.
    if (current_fb_->format == PIXFORMAT_RGB565) {
        const size_t pixel_count = (size_t)current_fb_->width * current_fb_->height;
        const size_t data_size = pixel_count * sizeof(uint16_t);
        if (current_fb_->len < data_size) {
            ESP_LOGE(TAG, "RGB565 frame is too small: %u < %u",
                     (unsigned)current_fb_->len, (unsigned)data_size);
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
            return false;
        }

        uint16_t *pixels = (uint16_t *)current_fb_->buf;
        if (rotate_capture_180_) {
            for (size_t i = 0; i < pixel_count / 2; ++i) {
                const uint16_t first = pixels[i];
                const uint16_t last = pixels[pixel_count - 1 - i];
                pixels[i] = swap_bytes_enabled_ ? __builtin_bswap16(last) : last;
                pixels[pixel_count - 1 - i] = swap_bytes_enabled_ ? __builtin_bswap16(first) : first;
            }
            if (swap_bytes_enabled_ && pixel_count % 2 != 0) {
                pixels[pixel_count / 2] = __builtin_bswap16(pixels[pixel_count / 2]);
            }
        } else if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; ++i) {
                pixels[i] = __builtin_bswap16(pixels[i]);
            }
        }

        // Allocate separate buffer for preview display
        uint8_t *preview_data = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, current_fb_->buf, data_size);
            auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(preview_data, data_size, current_fb_->width, current_fb_->height, current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        }
    } else if (current_fb_->format == PIXFORMAT_JPEG) {
        // JPEG format preview usually requires decoding, skip preview display for now, just log
        ESP_LOGW(TAG, "JPEG capture success, len=%zu, but not supported for preview", current_fb_->len);
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%u, format=%d",
             current_fb_->width, current_fb_->height,
             (unsigned)current_fb_->len, current_fb_->format);
    log_camera_heap("Capture complete");

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_hmirror) {
        ESP_LOGE(TAG, "set_hmirror unavailable");
        return false;
    }

    int ret = s->set_hmirror(s, enabled ? 1 : 0);
    ESP_LOGI(TAG, "set_hmirror(%d): ret=%d, status=%d",
             enabled, ret, s->status.hmirror);
    return ret == 0;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_vflip) {
        ESP_LOGE(TAG, "set_vflip unavailable");
        return false;
    }

    int ret = s->set_vflip(s, enabled ? 1 : 0);
    ESP_LOGI(TAG, "set_vflip(%d): ret=%d, status=%d",
             enabled, ret, s->status.vflip);
    return ret == 0;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

bool Esp32Camera::SetCaptureRotation180(bool enabled) {
    rotate_capture_180_ = enabled;
    return true;
}

std::string Esp32Camera::Explain(const std::string &question) {
    if (current_fb_ == nullptr) {
        throw std::runtime_error("No camera frame captured");
    }
    const uint16_t frame_width = current_fb_->width;
    const uint16_t frame_height = current_fb_->height;

    // Explain owns the captured frame and releases it on success and every error path.
    auto release_frame = [this](void *) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
    };
    std::unique_ptr<void, decltype(release_frame)> frame_guard(
        this, release_frame);

    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    ESP_LOGI(TAG, "Starting image explain encoding: %ux%u",
             (unsigned)frame_width, (unsigned)frame_height);
    log_camera_heap("Before JPEG encoding");

    v4l2_pix_fmt_t enc_fmt;
    switch (current_fb_->format) {
        case PIXFORMAT_RGB565:
            enc_fmt = V4L2_PIX_FMT_RGB565;
            break;
        case PIXFORMAT_YUV422:
            enc_fmt = V4L2_PIX_FMT_YUYV;
            break;
        case PIXFORMAT_YUV420:
            enc_fmt = V4L2_PIX_FMT_YUV420;
            break;
        case PIXFORMAT_GRAYSCALE:
            enc_fmt = V4L2_PIX_FMT_GREY;
            break;
        case PIXFORMAT_JPEG:
            enc_fmt = V4L2_PIX_FMT_JPEG;
            break;
        case PIXFORMAT_RGB888:
            enc_fmt = V4L2_PIX_FMT_RGB24;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
            throw std::runtime_error("Unsupported camera pixel format");
    }

    uint8_t *jpeg_src_buf = current_fb_->buf;
    size_t jpeg_src_len = current_fb_->len;

    uint8_t *jpeg_data = nullptr;
    size_t jpeg_size = 0;
    const bool encoded = image_to_jpeg(jpeg_src_buf, jpeg_src_len,
        frame_width, frame_height, enc_fmt, 80, &jpeg_data, &jpeg_size);
    auto free_jpeg = [](uint8_t *data) {
        if (data) {
            heap_caps_free(data);
        }
    };
    std::unique_ptr<uint8_t, decltype(free_jpeg)> jpeg_guard(jpeg_data, free_jpeg);
    if (!encoded || !jpeg_data || jpeg_size == 0) {
        ESP_LOGE(TAG, "JPEG encoding failed");
        throw std::runtime_error("Failed to encode image to JPEG");
    }
    ESP_LOGI(TAG, "JPEG encoding complete: %u bytes", (unsigned)jpeg_size);
    log_camera_heap("After JPEG encoding");

    // Encoding is complete; release the only camera framebuffer before networking.
    if (current_fb_) {
        esp_camera_fb_return(current_fb_);
        current_fb_ = nullptr;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        throw std::runtime_error("Failed to connect to explain URL");
    }
    ESP_LOGI(TAG, "Image explain HTTP connection opened");

    auto write_request_data = [&](const char *data, size_t size, const char *part) {
        if (http->Write(data, size) <= 0) {
            ESP_LOGE(TAG, "Failed to write image explain request %s", part);
            return false;
        }
        return true;
    };

    std::string question_field = "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n\r\n";
    question_field += question + "\r\n";
    if (!write_request_data(question_field.c_str(), question_field.size(), "question field")) {
        http->Close();
        throw std::runtime_error("Failed to upload question field");
    }

    std::string file_header = "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n\r\n";
    if (!write_request_data(file_header.c_str(), file_header.size(), "image header")) {
        http->Close();
        throw std::runtime_error("Failed to upload image header");
    }

    size_t total_sent = 0;
    constexpr size_t upload_chunk_size = 2048;
    while (total_sent < jpeg_size) {
        size_t chunk_size = std::min(upload_chunk_size, jpeg_size - total_sent);
        if (!write_request_data(reinterpret_cast<const char *>(jpeg_data + total_sent),
                                chunk_size, "JPEG data")) {
            http->Close();
            throw std::runtime_error("Failed to upload JPEG image");
        }
        total_sent += chunk_size;
    }
    jpeg_guard.reset();
    ESP_LOGI(TAG, "Image explain upload sent: %u JPEG bytes", (unsigned)total_sent);

    std::string multipart_footer = "\r\n--" + boundary + "--\r\n";
    if (!write_request_data(multipart_footer.c_str(), multipart_footer.size(), "multipart footer") ||
        !write_request_data("", 0, "end marker")) {
        http->Close();
        throw std::runtime_error("Failed to finish image explain upload");
    }

    ESP_LOGI(TAG, "Waiting for image explain HTTP response");
    const int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "Image explain HTTP response status: %d", status_code);
    if (status_code != 200) {
        http->Close();
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Image explain response received: %u bytes", (unsigned)result.size());

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             frame_width, frame_height, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}
