#include "photo_server.h"

#include "drivers/sdcard.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <algorithm>

#include <esp_http_server.h>
#include <esp_log.h>
#include <wifi_manager.h>

#define TAG "PHOTO_SERVER"
#define PHOTO_DIRECTORY "/sdcard/photo"

static httpd_handle_t photo_server = nullptr;

static bool is_jpeg_name(const char *name)
{
    if (!name || name[0] == '.' || strstr(name, "..")) {
        return false;
    }

    size_t len = strlen(name);
    if (len <= 4 || strcasecmp(name + len - 4, ".jpg") != 0) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const char *header =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"theme-color\" content=\"#f4f5f7\"><title>AI-CAM照片库</title>"
        "<style>*{box-sizing:border-box}body{margin:0;background:#f4f5f7;color:#252a32;"
        "font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}.wrap{max-width:1080px;"
        "margin:auto;padding:30px 20px 48px}.top{display:flex;align-items:center;"
        "justify-content:space-between;gap:16px;margin-bottom:22px;padding-bottom:16px;"
        "border-bottom:1px solid #dfe2e7}h1{font-size:24px;font-weight:600;margin:0}"
        ".sub{color:#737b87;margin:5px 0 0}.refresh{color:#374151;background:#fff;"
        "border:1px solid #d6dae0;border-radius:6px;padding:8px 13px;text-decoration:none;"
        "white-space:nowrap}.refresh:hover,.btn:hover{background:#eef0f3}.grid{display:grid;"
        "grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:16px}.card{overflow:hidden;"
        "background:#fff;border:1px solid #e0e3e8;border-radius:8px}"
        ".preview{display:block;aspect-ratio:4/3;background:#eceef1;overflow:hidden}"
        ".preview img{width:100%;height:100%;object-fit:cover;display:block}"
        ".info{padding:11px}.name{display:block;overflow:hidden;text-overflow:ellipsis;"
        "white-space:nowrap;color:#424955;font-size:13px;margin-bottom:10px}.actions{display:flex;gap:8px}"
        ".btn{flex:1;text-align:center;padding:7px 10px;border:1px solid #d9dde3;"
        "border-radius:5px;text-decoration:none;font-size:13px;color:#3d4652;background:#fff}"
        ".btn.primary{background:#f1f6f2;border-color:#cbd9ce;color:#315d3b}"
        ".btn.primary:hover{background:#e7f0e9}.empty{padding:32px;text-align:center;"
        "color:#737b87;background:#fff;border:1px solid #e0e3e8;border-radius:8px}"
        "@media(max-width:520px){.wrap{padding:20px 14px 32px}.grid{grid-template-columns:"
        "repeat(2,minmax(0,1fr));gap:10px}.info{padding:9px}.top{margin-bottom:16px}h1{font-size:21px}}"
        "</style></head><body><main class=\"wrap\"><header class=\"top\"><div>"
        "<h1>AI-CAM照片库</h1><p class=\"sub\">浏览并下载设备中的照片</p></div>"
        "<a class=\"refresh\" href=\"/\">刷新</a></header><section class=\"grid\">";
    esp_err_t err = httpd_resp_sendstr_chunk(req, header);
    if (err != ESP_OK) {
        return err;
    }

    if (!sdcard_begin_access()) {
        err = httpd_resp_sendstr_chunk(req, "<div class=\"empty\">SD 卡未挂载</div>");
    } else {
        DIR *dir = opendir(PHOTO_DIRECTORY);
        std::vector<std::string> names;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (is_jpeg_name(entry->d_name)) {
                    names.emplace_back(entry->d_name);
                }
            }
            closedir(dir);
        }
        sdcard_end_access();

        std::sort(names.begin(), names.end());
        if (names.empty()) {
            err = httpd_resp_sendstr_chunk(req, "<div class=\"empty\">还没有照片</div>");
        } else {
            for (const auto &name : names) {
                std::string item = "<article class=\"card\"><a class=\"preview\" href=\"/image?name=" + name +
                                   "\" target=\"_blank\"><img loading=\"lazy\" src=\"/image?name=" + name +
                                   "\" alt=\"" + name + "\"></a><div class=\"info\"><span class=\"name\" title=\"" + name +
                                   "\">" + name + "</span><div class=\"actions\"><a class=\"btn\" href=\"/image?name=" + name +
                                   "\" target=\"_blank\">预览</a><a class=\"btn primary\" href=\"/download?name=" + name +
                                   "\">下载</a></div></div></article>";
                err = httpd_resp_sendstr_chunk(req, item.c_str());
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
    }

    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_sendstr_chunk(req, "</section></main></body></html>");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t send_photo(httpd_req_t *req, bool attachment)
{
    char query[96] = {};
    char name[64] = {};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !is_jpeg_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid photo name");
    }

    if (!sdcard_begin_access()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "SD card unavailable");
    }

    char path[sizeof(PHOTO_DIRECTORY) + sizeof(name) + 2];
    snprintf(path, sizeof(path), PHOTO_DIRECTORY "/%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        sdcard_end_access();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
    }

    httpd_resp_set_type(req, "image/jpeg");
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "%s; filename=\"%s\"",
             attachment ? "attachment" : "inline", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buffer[1024];
    esp_err_t result = ESP_OK;
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        result = httpd_resp_send_chunk(req, buffer, static_cast<ssize_t>(count));
        if (result != ESP_OK) {
            break;
        }
    }
    if (ferror(file)) {
        result = ESP_FAIL;
    }
    fclose(file);
    sdcard_end_access();

    if (result != ESP_OK) {
        return result;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t download_handler(httpd_req_t *req)
{
    return send_photo(req, true);
}

static esp_err_t image_handler(httpd_req_t *req)
{
    return send_photo(req, false);
}

esp_err_t photo_server_start(void)
{
    if (!photo_server) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;  // 保留 80 端口给 Wi-Fi 配网页面
        config.max_uri_handlers = 3;
        config.max_open_sockets = 4;
        config.lru_purge_enable = true;

        esp_err_t err = httpd_start(&photo_server, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
            photo_server = nullptr;
            return err;
        }

        const httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = nullptr,
        };
        const httpd_uri_t download_uri = {
            .uri = "/download",
            .method = HTTP_GET,
            .handler = download_handler,
            .user_ctx = nullptr,
        };
        const httpd_uri_t image_uri = {
            .uri = "/image",
            .method = HTTP_GET,
            .handler = image_handler,
            .user_ctx = nullptr,
        };

        err = httpd_register_uri_handler(photo_server, &index_uri);
        if (err == ESP_OK) {
            err = httpd_register_uri_handler(photo_server, &download_uri);
        }
        if (err == ESP_OK) {
            err = httpd_register_uri_handler(photo_server, &image_uri);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(err));
            httpd_stop(photo_server);
            photo_server = nullptr;
            return err;
        }
    }

    std::string ip = WifiManager::GetInstance().GetIpAddress();
    ESP_LOGI(TAG, "Photo download page: http://%s:8080/", ip.c_str());
    return ESP_OK;
}

std::string photo_server_get_address(void)
{
    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        return "Wi-Fi 未连接";
    }
    return "http://" + ip + ":8080";
}
