#include "sensor_driver.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSOR";

// ==================== I2C 工具  ====================
static bool i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len)
{
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C reg read err reg=0x%02X: %d", reg, ret);
        return false;
    }
    return true;
}

static bool i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t ret = i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C reg write err reg=0x%02X: %d", reg, ret);
        return false;
    }
    return true;
}

static bool i2c_raw_write(i2c_master_dev_handle_t dev, const uint8_t* data, size_t len)
{
    esp_err_t ret = i2c_master_transmit(dev, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C raw write err len=%d: %d", len, ret);
        return false;
    }
    return true;
}

static bool i2c_raw_read(i2c_master_dev_handle_t dev, uint8_t* data, size_t len)
{
    esp_err_t ret = i2c_master_receive(dev, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C raw read err len=%d: %d", len, ret);
        return false;
    }
    return true;
}

// ==================== BME280 ====================

void BME280::set_bus(void* bus_handle)
{
    auto* bus = static_cast<i2c_master_bus_handle_t>(bus_handle);
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev_));
}

bool BME280::init()
{
    if (!i2c_dev_) { ESP_LOGE(TAG, "BME280: no I2C dev"); return false; }

    uint8_t id = 0;
    if (!i2c_reg_read(i2c_dev_, 0xD0, &id, 1) || id != 0x60) {
        ESP_LOGE(TAG, "BME280 not found at 0x%02X (id=0x%02X)", addr_, id);
        return false;
    }
    ESP_LOGI(TAG, "BME280 found at 0x%02X", addr_);

    read_calib();

    i2c_reg_write(i2c_dev_, 0xE0, 0xB6);  // soft reset
    vTaskDelay(pdMS_TO_TICKS(10));

    i2c_reg_write(i2c_dev_, 0xF2, 0x01);  // ctrl_hum: osrs_h x1
    i2c_reg_write(i2c_dev_, 0xF4, 0x25);  // ctrl_meas: osrs_t/p x1, FORCED mode (防自热)
    i2c_reg_write(i2c_dev_, 0xF5, 0x00);  // config: filter off (forced 模式忽略 t_sb)

    ESP_LOGI(TAG, "BME280 init ok");
    return true;
}

bool BME280::read_calib()
{
    uint8_t buf[26];
    if (!i2c_reg_read(i2c_dev_, 0x88, buf, 26)) return false;
    dig_T1_ = buf[0]  | (buf[1]  << 8);
    dig_T2_ = buf[2]  | (buf[3]  << 8);
    dig_T3_ = buf[4]  | (buf[5]  << 8);
    dig_P1_ = buf[6]  | (buf[7]  << 8);
    dig_P2_ = buf[8]  | (buf[9]  << 8);
    dig_P3_ = buf[10] | (buf[11] << 8);
    dig_P4_ = buf[12] | (buf[13] << 8);
    dig_P5_ = buf[14] | (buf[15] << 8);
    dig_P6_ = buf[16] | (buf[17] << 8);
    dig_P7_ = buf[18] | (buf[19] << 8);
    dig_P8_ = buf[20] | (buf[21] << 8);
    dig_P9_ = buf[22] | (buf[23] << 8);
    dig_H1_ = buf[25];

    uint8_t hbuf[7];
    if (!i2c_reg_read(i2c_dev_, 0xE1, hbuf, 7)) return false;
    dig_H2_ = hbuf[0] | (hbuf[1] << 8);
    dig_H3_ = hbuf[2];
    dig_H4_ = (hbuf[3] << 4) | (hbuf[4] & 0x0F);
    dig_H5_ = (hbuf[5] << 4) | (hbuf[4] >> 4);
    dig_H6_ = (int8_t)hbuf[6];

    return true;
}

bool BME280::read_raw()
{
    // 强制模式：写 ctrl_meas 触发单次测量，测完自动休眠（杜绝自热）
    i2c_reg_write(i2c_dev_, 0xF4, 0x25);  // osrs_t/p x1, forced mode
    vTaskDelay(pdMS_TO_TICKS(10));        // 等待测量完成 (T+P+H max ~7ms)

    uint8_t buf[8];
    if (!i2c_reg_read(i2c_dev_, 0xF7, buf, 8)) return false;

    int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
    int32_t adc_H = ((int32_t)buf[6] << 8)  |  (int32_t)buf[7];

    // 温度 (t_fine: 0.01°C)
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1_ << 1))) * (int32_t)dig_T2_) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)dig_T1_) *
                       ((adc_T >> 4) - (int32_t)dig_T1_)) >> 12) * (int32_t)dig_T3_) >> 14;
    t_fine_ = var1 + var2;

    // 气压 (Pa → hPa)
    int64_t pvar1 = (int64_t)t_fine_ - 128000;
    int64_t pvar2 = pvar1 * pvar1 * (int64_t)dig_P6_;
    pvar2 += (pvar1 * (int64_t)dig_P5_) << 17;
    pvar2 += ((int64_t)dig_P4_) << 35;
    pvar1 = ((pvar1 * pvar1 * (int64_t)dig_P3_) >> 8) + ((pvar1 * (int64_t)dig_P2_) << 12);
    pvar1 = ((((int64_t)1) << 47) + pvar1) * (int64_t)dig_P1_ >> 33;

    if (pvar1 == 0) {
        press_ = 0;
    } else {
        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - pvar2) * 3125) / pvar1;
        pvar1 = ((int64_t)dig_P9_ * (p >> 13) * (p >> 13)) >> 25;
        pvar2 = ((int64_t)dig_P8_ * p) >> 19;
        p = ((p + pvar1 + pvar2) >> 8) + ((int64_t)dig_P7_ << 4);
        press_ = (float)p / 25600.0f;
    }

    // 湿度
    int32_t vx1 = t_fine_ - 76800;
    vx1 = (((((adc_H << 14) - ((int32_t)dig_H4_ << 20) - ((int32_t)dig_H5_ * vx1)) +
             16384) >> 15) *
           (((((((vx1 * (int32_t)dig_H6_) >> 10) *
               (((vx1 * (int32_t)dig_H3_) >> 10) + 32768)) >> 10) +
              2097152) * (int32_t)dig_H2_ + 8192) >> 14));

    vx1 = vx1 - (((((vx1 >> 15) * (vx1 >> 15)) >> 7) * (int32_t)dig_H1_) >> 4);
    vx1 = vx1 < 0 ? 0 : vx1;
    vx1 = vx1 > 419430400 ? 419430400 : vx1;
    hum_ = (float)(vx1 >> 12) / 1024.0f;

    return true;
}

bool BME280::read()
{
    return read_raw();
}

// BME280 子传感器
bool BME280_Temp::read()
{
    snprintf(buf_, sizeof(buf_), "%.1f", bme_->temperature());
    return true;
}

bool BME280_Humi::read()
{
    snprintf(buf_, sizeof(buf_), "%.1f", bme_->humidity());
    return true;
}

bool BME280_Pres::read()
{
    snprintf(buf_, sizeof(buf_), "%.1f", bme_->pressure());
    return true;
}

// ==================== BH1750 ====================

void BH1750::set_bus(void* bus_handle)
{
    auto* bus = static_cast<i2c_master_bus_handle_t>(bus_handle);
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev_));
}

bool BH1750::init()
{
    if (!i2c_dev_) { ESP_LOGE(TAG, "BH1750: no I2C dev"); return false; }

    // Power On (0x01)
    // 若此处 NACK，请检查：① VCC/GND 连接 ② ADDR 引脚电平
    // ADDR=LOW→0x23, ADDR=HIGH→0x5C
    if (!i2c_raw_write(i2c_dev_, (const uint8_t*)"\x01", 1)) {
        ESP_LOGE(TAG, "BH1750@0x%02X power on FAIL (check ADDR pin: LOW=0x23, HIGH=0x5C)", addr_);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Continuous High-Resolution Mode (0x10)
    if (!i2c_raw_write(i2c_dev_, (const uint8_t*)"\x10", 1)) {
        ESP_LOGE(TAG, "BH1750 set mode failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(180));

    snprintf(buf_, sizeof(buf_), "--");
    ESP_LOGI(TAG, "BH1750@0x%02X init ok", addr_);
    return true;
}

bool BH1750::read()
{
    if (!i2c_dev_) { snprintf(buf_, sizeof(buf_), "err"); return false; }

    uint8_t data[2] = {0};
    if (!i2c_raw_read(i2c_dev_, data, 2)) {
        snprintf(buf_, sizeof(buf_), "err");
        return false;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    float lux = (float)raw / 1.2f;

    if (lux < 1.0f)
        snprintf(buf_, sizeof(buf_), "%.1f", lux);
    else if (lux < 1000.0f)
        snprintf(buf_, sizeof(buf_), "%.0f", lux);
    else
        snprintf(buf_, sizeof(buf_), "%.0f", lux);

    return true;
}

// ==================== MAX17043 ====================

void MAX17043::set_bus(void* bus_handle)
{
    auto* bus = static_cast<i2c_master_bus_handle_t>(bus_handle);
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev_));
}

// MAX17043 I2C 读寄存器：先发寄存器地址(0x6C+reg)，再读数据(0x6D)
// 使用分离的 transmit + receive，符合数据手册协议：
//   S → SAddr(W)=0x6C → A → MAddr → A → P
//   S → SAddr(R)=0x6D → A → Data0 → A → Data1 → N → P
static bool max17043_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len)
{
    esp_err_t ret = i2c_master_transmit(dev, &reg, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MAX17043 tx reg=0x%02X err: %d", reg, ret);
        return false;
    }
    ret = i2c_master_receive(dev, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MAX17043 rx reg=0x%02X err: %d", reg, ret);
        return false;
    }
    return true;
}

bool MAX17043::init()
{
    if (!i2c_dev_) { ESP_LOGE(TAG, "MAX17043: no I2C dev"); return false; }

    // 读取 VERSION 寄存器 (0x08) 验证芯片是否存在
    uint8_t ver[2] = {0};
    if (!max17043_reg_read(i2c_dev_, 0x08, ver, 2)) {
        ESP_LOGE(TAG, "MAX17043 not found at 0x%02X", addr_);
        return false;
    }
    uint16_t version = ((uint16_t)ver[0] << 8) | ver[1];
    ESP_LOGI(TAG, "MAX17043@0x%02X version=0x%04X", addr_, version);

    // Quick-Start: 写 0x4000 到 MODE 寄存器 (0x06), 重置 ModelGauge 算法
    // I2C Write: S → 0x6C → A → 0x06 → A → 0x40 → A → 0x00 → A → P
    uint8_t qs[3] = {0x06, 0x40, 0x00};
    if (!i2c_raw_write(i2c_dev_, qs, 3)) {
        ESP_LOGW(TAG, "MAX17043 quick-start failed");
        return false;
    }
    ESP_LOGI(TAG, "MAX17043 quick-start done");

    snprintf(buf_, sizeof(buf_), "--");
    ESP_LOGI(TAG, "MAX17043 init ok");
    return true;
}

bool MAX17043::read()
{
    if (!i2c_dev_) { snprintf(buf_, sizeof(buf_), "err"); return false; }

    // 读 VCELL (0x02-0x03): 12-bit ADC 在 bits[15:4], bits[3:0] 始终为 0
    // 分辨率: 1.25mV/LSB (在 bit-4 位置)
    // 转换: voltage(V) = (raw >> 4) * 1.25 / 1000
    uint8_t vc[2] = {0};
    if (max17043_reg_read(i2c_dev_, 0x02, vc, 2)) {
        uint16_t raw_vcell = ((uint16_t)vc[0] << 8) | vc[1];
        voltage_ = (raw_vcell >> 4) * 1.25f / 1000.0f;
    } else {
        voltage_ = 0.0f;
    }

    // 读 SOC (0x04-0x05): MSB=整数%, LSB=小数 1/256%
    uint8_t sc[2] = {0};
    if (max17043_reg_read(i2c_dev_, 0x04, sc, 2)) {
        soc_ = (float)sc[0] + (float)sc[1] / 256.0f;
    } else {
        snprintf(buf_, sizeof(buf_), "err");
        return false;
    }

    // 格式化显示：剩余电量 SOC 百分比
    if (soc_ < 10.0f)
        snprintf(buf_, sizeof(buf_), "%.1f", soc_);
    else
        snprintf(buf_, sizeof(buf_), "%.0f", soc_);

    return true;
}

// ==================== SGP41 ====================

uint8_t SGP41::crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
    return crc;
}

void SGP41::set_bus(void* bus_handle)
{
    auto* bus = static_cast<i2c_master_bus_handle_t>(bus_handle);
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev_));
}

bool SGP41::cmd_execute(uint16_t cmd, uint16_t* result, uint32_t wait_ms)
{
    if (!i2c_dev_) return false;

    uint8_t tx[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    if (!i2c_raw_write(i2c_dev_, tx, 2)) return false;

    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    uint8_t rx[3] = {0};
    if (!i2c_raw_read(i2c_dev_, rx, 3)) return false;

    uint8_t calc = crc8(rx, 2);
    if (calc != rx[2]) {
        ESP_LOGW(TAG, "SGP41 CRC err: calc=0x%02X got=0x%02X", calc, rx[2]);
        return false;
    }

    *result = ((uint16_t)rx[0] << 8) | rx[1];
    return true;
}

// SGP41 带补偿参数的测量
static bool sgp41_measure(i2c_master_dev_handle_t dev, uint16_t cmd,
                          uint16_t arg1, uint16_t arg2,
                          uint16_t* result, uint32_t wait_ms,
                          uint8_t (*crc8_fn)(const uint8_t*, size_t))
{
    uint8_t tx[8];
    tx[0] = (uint8_t)(cmd >> 8);
    tx[1] = (uint8_t)(cmd & 0xFF);
    tx[2] = (uint8_t)(arg1 >> 8);
    tx[3] = (uint8_t)(arg1 & 0xFF);
    tx[4] = crc8_fn(&tx[2], 2);
    tx[5] = (uint8_t)(arg2 >> 8);
    tx[6] = (uint8_t)(arg2 & 0xFF);
    tx[7] = crc8_fn(&tx[5], 2);

    if (!i2c_raw_write(dev, tx, 8)) return false;
    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    uint8_t rx[3] = {0};
    if (!i2c_raw_read(dev, rx, 3)) return false;

    uint8_t c = crc8_fn(rx, 2);
    if (c != rx[2]) {
        ESP_LOGW(TAG, "SGP41 CRC err: calc=0x%02X got=0x%02X", c, rx[2]);
        return false;
    }

    *result = ((uint16_t)rx[0] << 8) | rx[1];
    return true;
}

bool SGP41::init()
{
    if (!i2c_dev_) { ESP_LOGE(TAG, "SGP41: no I2C dev"); return false; }

    // 启动芯片内置 conditioning（约 10s，芯片自主运行，无需 CPU 阻塞等待）
    // 刚启动 conditioning 期间 VOC 读数偏高（~500），用户进入传感器页面时已自然完成
    ESP_LOGI(TAG, "SGP41 triggering conditioning...");
    uint16_t dummy;
    if (!cmd_execute(0x280E, &dummy, 100)) {
        ESP_LOGW(TAG, "SGP41 conditioning cmd failed");
        return false;
    }

    snprintf(buf_, sizeof(buf_), "--");
    ESP_LOGI(TAG, "SGP41 init ok (conditioning runs on-chip)");
    return true;
}

bool SGP41::read()
{
    if (!i2c_dev_) { snprintf(buf_, sizeof(buf_), "err"); return false; }

    float rh_pct  = comp_rh_ * 0.01f;
    float temp_c  = comp_t_  * 0.01f;
    uint16_t rh_ticks = (uint16_t)(rh_pct * 65535.0f / 100.0f + 0.5f);
    uint16_t t_ticks  = (uint16_t)((temp_c + 45.0f) * 65535.0f / 175.0f + 0.5f);

    // VOC Index (0x2619)
    uint16_t v = 0;
    if (!sgp41_measure(i2c_dev_, 0x2619, rh_ticks, t_ticks, &v, 50, crc8)) {
        snprintf(buf_, sizeof(buf_), "err");
        return false;
    }
    voc_ = v;

    // NOx Index (0x271A)
    uint16_t n = 0;
    if (!sgp41_measure(i2c_dev_, 0x271A, rh_ticks, t_ticks, &n, 50, crc8)) {
        nox_ = 0;
    } else {
        nox_ = n;
    }

    snprintf(buf_, sizeof(buf_), "%u", voc_);
    return true;
}

bool SGP41_VOC::read()
{
    if (!sgp_) { snprintf(buf_, sizeof(buf_), "err"); return false; }
    snprintf(buf_, sizeof(buf_), "%u", sgp_->voc_raw());
    return true;
}

bool SGP41_NOx::read()
{
    if (!sgp_) { snprintf(buf_, sizeof(buf_), "err"); return false; }
    snprintf(buf_, sizeof(buf_), "%u", sgp_->nox_raw());
    return true;
}

// ==================== SensorManager ====================

SensorManager& SensorManager::GetInstance()
{
    static SensorManager inst;
    return inst;
}

void SensorManager::init_all(void* i2c_bus)
{
    // 先初始化父传感器（驱动层，不在 UI 中显示）
    if (bme280_) {
        bme280_->set_bus(i2c_bus);
        bme280_->init();
    }
    if (sgp41_) {
        sgp41_->set_bus(i2c_bus);
        sgp41_->init();
    }

    // 再初始化子传感器（用于 UI 显示）
    for (auto* s : sensors_) {
        s->set_bus(i2c_bus);
        s->init();
    }
    ESP_LOGI(TAG, "%zu display sensor(s) init complete", sensors_.size());
}

void SensorManager::read_all()
{
    // 1. 读父传感器 BME280（为子传感器和 SGP41 补偿提供数据）
    if (bme280_)  bme280_->read();

    // 2. SGP41 补偿 = BME280 温湿度
    if (bme280_ && sgp41_) {
        sgp41_->set_compensation(bme280_->temp_cx100(), bme280_->rh_x100());
    }

    // 3. 读父传感器 SGP41（更新 VOC/NOx 原始值）
    if (sgp41_)  sgp41_->read();

    // 4. 读所有子传感器（从父传感器取数据，格式化自己的 buf_）
    for (auto* s : sensors_) {
        s->read();
    }
}

void SensorManager::register_sensor(SensorBase* sensor)
{
    sensors_.push_back(sensor);
    ESP_LOGI(TAG, "Registered: %s", sensor->name());
}

SensorBase* SensorManager::get(size_t index) const
{
    return (index < sensors_.size()) ? sensors_[index] : nullptr;
}

SensorBase* SensorManager::find(const char* name) const
{
    for (auto* s : sensors_) {
        if (std::strcmp(s->name(), name) == 0) return s;
    }
    return nullptr;
}
