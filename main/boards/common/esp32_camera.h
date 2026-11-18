#pragma once
#include "sdkconfig.h"

#include <lvgl.h>

#include "camera.h"
#include "esp_camera.h"

class Esp32Camera : public Camera
{
private:
    bool streaming_on_ = false;
    bool swap_bytes_enabled_ = true;  // Swap pixel byte order for RGB565, enabled by default
    bool rotate_capture_180_ = false;
    std::string explain_url_;
    std::string explain_token_;
    camera_fb_t *current_fb_ = nullptr;

public:
    Esp32Camera(const camera_config_t &config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string &url, const std::string &token) override;
    virtual bool Capture() override;
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual bool SetSwapBytes(bool enabled) override;
    virtual bool SetCaptureRotation180(bool enabled) override;
    virtual std::string Explain(const std::string &question) override;
};
