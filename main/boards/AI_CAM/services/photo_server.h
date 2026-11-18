#pragma once

#include <esp_err.h>
#include <string>

esp_err_t photo_server_start(void);
std::string photo_server_get_address(void);
