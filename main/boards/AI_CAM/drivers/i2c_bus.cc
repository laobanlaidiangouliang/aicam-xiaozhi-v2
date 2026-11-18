#include "i2c_bus.h"
#include "sensor_driver.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "I2C_BUS";

void i2c_devices_init(i2c_master_bus_handle_t bus) {
    auto& mgr = SensorManager::GetInstance();

    // BME280 温湿度气压 — I2C: 0x76 (SDO → GND)
    // 父传感器（驱动层，不在 UI 中显示）
    auto* bme = new BME280(0x76, "bme");
    mgr.link_bme280(bme);

    // 子传感器（UI 显示用）
    mgr.register_sensor(new BME280_Temp(bme));
    mgr.register_sensor(new BME280_Humi(bme));
    mgr.register_sensor(new BME280_Pres(bme));

    // BH1750FVI 环境光 — I2C: 0x23 (ADDR → LOW)
    mgr.register_sensor(new BH1750(0x23));

    // SGP41-D-R4 VOC / NOx — I2C: 0x59 (固定)
    // 父传感器（驱动层，不在 UI 中显示）
    auto* sgp = new SGP41();
    mgr.link_sgp41(sgp);

    // 子传感器（UI 显示用）
    mgr.register_sensor(new SGP41_VOC(sgp));
    mgr.register_sensor(new SGP41_NOx(sgp));

    // MAX17043G+T 锂电池电量检测 — I2C: 0x36
    mgr.register_sensor(new MAX17043());

    mgr.init_all((void*)bus);

    ESP_LOGI(TAG, "All I2C devices initialized (%zu sensors)", mgr.count());
}
