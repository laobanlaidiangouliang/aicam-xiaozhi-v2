#pragma once
#include "driver/i2c_master.h"

// I2C设备初始化函数，传入已初始化的总线句柄
void i2c_devices_init(i2c_master_bus_handle_t bus);

