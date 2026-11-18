#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>
#include "driver/i2c_master.h"

/* ==================== 传感器抽象基类 ==================== */
class SensorBase {
public:
    virtual ~SensorBase() = default;
    virtual const char* name() const = 0;
    virtual const char* value_str() const = 0;
    virtual const char* unit() const = 0;
    virtual bool init() = 0;
    virtual bool read() = 0;
    virtual void set_bus(void* bus_handle) = 0;
};

/* ==================== BME280 温湿度气压 ==================== */
class BME280 : public SensorBase {
public:
    BME280(uint8_t addr, const char* tag) : addr_(addr), tag_(tag) {}

    const char* name()      const override { return tag_; }
    const char* value_str() const override { return "--"; }  // 父类不直接显示值
    const char* unit()      const override { return ""; }

    bool init() override;
    bool read() override;
    void set_bus(void* bus_handle) override;

    // Bosch 公式: t_fine → 0.01°C 需经 (t_fine*5+128)>>8 换算
    float temperature() const { return temp_cx100() * 0.01f; }
    float humidity() const    { return hum_; }
    float pressure() const    { return press_; }

    int32_t temp_cx100() const { return (t_fine_ * 5 + 128) >> 8; }
    int32_t rh_x100()    const { return (int32_t)(hum_ * 100.0f); }

private:
    bool read_calib();
    bool read_raw();

    uint8_t   addr_;
    const char* tag_;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;

    uint16_t dig_T1_, dig_P1_;
    int16_t  dig_T2_, dig_T3_, dig_P2_, dig_P3_, dig_P4_, dig_P5_,
             dig_P6_, dig_P7_, dig_P8_, dig_P9_, dig_H2_, dig_H4_, dig_H5_;
    uint8_t  dig_H1_, dig_H3_;
    int8_t   dig_H6_;

    int32_t t_fine_ = 0;
    float   hum_ = 0, press_ = 0;
};

class BME280_Temp : public SensorBase {
public:
    explicit BME280_Temp(BME280* bme) : bme_(bme) { snprintf(buf_, sizeof(buf_), " --"); }
    const char* name() const override { return "BME280 Temp"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "C"; }
    bool init() override { return true; }
    void set_bus(void*) override {}
    bool read() override;
private:
    BME280* bme_;
    char buf_[16];
};

class BME280_Humi : public SensorBase {
public:
    explicit BME280_Humi(BME280* bme) : bme_(bme) { snprintf(buf_, sizeof(buf_), " --"); }
    const char* name() const override { return "BME280 Humi"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "%"; }
    bool init() override { return true; }
    void set_bus(void*) override {}
    bool read() override;
private:
    BME280* bme_;
    char buf_[16];
};

class BME280_Pres : public SensorBase {
public:
    explicit BME280_Pres(BME280* bme) : bme_(bme) { snprintf(buf_, sizeof(buf_), " --"); }
    const char* name() const override { return "BME280 Pres"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "hPa"; }
    bool init() override { return true; }
    void set_bus(void*) override {}
    bool read() override;
private:
    BME280* bme_;
    char buf_[16];
};

/* ==================== BH1750FVI 环境光 ==================== */
class BH1750 : public SensorBase {
public:
    BH1750(uint8_t addr) : addr_(addr) { snprintf(buf_, sizeof(buf_), " --"); }

    bool init() override;
    bool read() override;
    void set_bus(void* bus_handle) override;

    const char* name() const override { return "BH1750"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "lux"; }

private:
    uint8_t addr_;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    char buf_[16];
};

/* ==================== MAX17043 锂电池电量检测 ==================== */
class MAX17043 : public SensorBase {
public:
    MAX17043() : addr_(0x36) { snprintf(buf_, sizeof(buf_), " --"); }

    bool init() override;
    bool read() override;
    void set_bus(void* bus_handle) override;

    const char* name() const override { return "Battery"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "%"; }

    float voltage() const { return voltage_; }
    float soc() const { return soc_; }

private:
    uint8_t addr_;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    char buf_[16];
    float voltage_ = 0.0f;
    float soc_ = 0.0f;
};

/* ==================== SGP41 VOC / NOx ==================== */
class SGP41 : public SensorBase {
public:
    SGP41() : addr_(0x59) { snprintf(buf_, sizeof(buf_), " --"); }

    bool init() override;
    bool read() override;
    void set_bus(void* bus_handle) override;

    void set_compensation(int32_t temp_cx100, int32_t rh_x100) {
        comp_t_ = temp_cx100;
        comp_rh_ = rh_x100;
    }

    const char* name() const override { return "SGP41 VOC"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "idx"; }

    uint16_t voc_raw() const { return voc_; }
    uint16_t nox_raw() const { return nox_; }

private:
    bool cmd_execute(uint16_t cmd, uint16_t* result, uint32_t wait_ms);
    static uint8_t crc8(const uint8_t* data, size_t len);

    uint8_t  addr_;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    char     buf_[16];

    int32_t  comp_t_  = 2500;
    int32_t  comp_rh_ = 5000;

    uint16_t voc_ = 0, nox_ = 0;
};

class SGP41_VOC : public SensorBase {
public:
    explicit SGP41_VOC(SGP41* sgp) : sgp_(sgp) { snprintf(buf_, sizeof(buf_), " --"); }
    const char* name() const override { return "SGP41 VOC"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "idx"; }
    bool init() override { return true; }
    void set_bus(void*) override {}
    bool read() override;
private:
    SGP41* sgp_;
    char buf_[16];
};

class SGP41_NOx : public SensorBase {
public:
    explicit SGP41_NOx(SGP41* sgp) : sgp_(sgp) { snprintf(buf_, sizeof(buf_), " --"); }
    const char* name() const override { return "SGP41 NOx"; }
    const char* value_str() const override { return buf_; }
    const char* unit() const override { return "idx"; }
    bool init() override { return true; }
    void set_bus(void*) override {}
    bool read() override;
private:
    SGP41* sgp_;
    char buf_[16];
};

/* ==================== 传感器管理器 ==================== */
class SensorManager {
public:
    static SensorManager& GetInstance();

    void init_all(void* i2c_bus);
    void read_all();
    void register_sensor(SensorBase* sensor);

    size_t count() const { return sensors_.size(); }
    SensorBase* get(size_t index) const;
    SensorBase* find(const char* name) const;

    void link_bme280(BME280* bme)  { bme280_ = bme; }
    void link_sgp41(SGP41* sgp)    { sgp41_ = sgp; }

    BME280* bme280_parent() { return bme280_; }
    SGP41*  sgp41_parent()  { return sgp41_; }

private:
    SensorManager() = default;
    std::vector<SensorBase*> sensors_;   // 仅子传感器（用于 UI 显示）
    BME280* bme280_ = nullptr;           // 父传感器（不在 UI 中）
    SGP41*  sgp41_  = nullptr;           // 父传感器（不在 UI 中）
};
