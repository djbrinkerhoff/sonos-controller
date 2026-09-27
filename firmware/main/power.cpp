#include "power.hpp"
#include "bsp/m5stack_tab5.h"
#include "bmi270.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
const char* TAG="power";
// PI4IOE5V6408 "io_expander1" charger pins. Pin 4 is PWROFF_PULSE (driving
// it powers the device off) and pins 0/3 are owned by the BSP, so only 5,
// 6 and 7 are touched here.
constexpr uint32_t PIN_CHG_STAT=IO_EXPANDER_PIN_NUM_6; // input, high = charging
constexpr uint32_t PIN_QC_EN=IO_EXPANDER_PIN_NUM_5;    // output, low = quick charge
constexpr uint32_t PIN_CHG_EN=IO_EXPANDER_PIN_NUM_7;   // output, high = charge enabled

// INA226 monitor for the 2-cell battery pack (shunt 5 mOhm).
constexpr uint8_t INA226_ADDR=0x41;
constexpr uint8_t REG_CONFIG=0x00, REG_SHUNT=0x01, REG_BUS=0x02;
constexpr uint16_t INA226_CONFIG=0x4727; // 16x averaging, 1.1 ms conversions, continuous
constexpr int I2C_TIMEOUT_MS=100;

std::atomic<esp_io_expander_handle_t> expander{nullptr};
std::atomic<i2c_master_dev_handle_t> ina226{nullptr};

esp_err_t ina_write(uint8_t reg, uint16_t value) {
    auto dev=ina226.load();
    if(!dev) return ESP_ERR_INVALID_STATE;
    uint8_t data[3]={reg,static_cast<uint8_t>(value>>8),static_cast<uint8_t>(value)};
    return i2c_master_transmit(dev,data,sizeof(data),I2C_TIMEOUT_MS);
}
esp_err_t ina_read(uint8_t reg, uint16_t* value) {
    auto dev=ina226.load();
    if(!dev) return ESP_ERR_INVALID_STATE;
    uint8_t data[2];
    esp_err_t result=i2c_master_transmit_receive(dev,&reg,1,data,sizeof(data),I2C_TIMEOUT_MS);
    if(result==ESP_OK) *value=static_cast<uint16_t>(data[0]<<8|data[1]);
    return result;
}
}
void power_init() {
    if(esp_io_expander_handle_t io=bsp_io_expander1_init()) {
        expander=io;
        // Restore the charger pins the expander reset made inputs: charge
        // enabled, quick charge enabled, status read back as an input. The
        // level is written before the direction so CHG_EN never glitches low.
        esp_err_t result=esp_io_expander_set_output_mode(io,PIN_CHG_EN|PIN_QC_EN,IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
        if(result==ESP_OK) result=esp_io_expander_set_level(io,PIN_CHG_EN,1);
        if(result==ESP_OK) result=esp_io_expander_set_level(io,PIN_QC_EN,0);
        if(result==ESP_OK) result=esp_io_expander_set_dir(io,PIN_CHG_EN|PIN_QC_EN,IO_EXPANDER_OUTPUT);
        if(result==ESP_OK) result=esp_io_expander_set_dir(io,PIN_CHG_STAT,IO_EXPANDER_INPUT);
        if(result==ESP_OK) ESP_LOGI(TAG,"Charging pins restored: CHG_EN=1, QC enabled, CHG_STAT input");
        else ESP_LOGE(TAG,"Charging pin restore failed: %s",esp_err_to_name(result));
    } else ESP_LOGE(TAG,"IO expander 1 unavailable; charging pins not restored");

    i2c_master_bus_handle_t bus=bsp_i2c_get_handle();
    i2c_master_dev_handle_t dev=nullptr;
    i2c_device_config_t config={};
    config.dev_addr_length=I2C_ADDR_BIT_LEN_7;
    config.device_address=INA226_ADDR;
    config.scl_speed_hz=400000;
    if(!bus || i2c_master_bus_add_device(bus,&config,&dev)!=ESP_OK) {
        ESP_LOGE(TAG,"INA226 registration failed; battery readings disabled");
        return;
    }
    ina226=dev;
    if(esp_err_t result=ina_write(REG_CONFIG,INA226_CONFIG); result!=ESP_OK)
        ESP_LOGE(TAG,"INA226 configuration failed: %s",esp_err_to_name(result));
    else ESP_LOGI(TAG,"INA226 battery monitor configured");
}
Battery battery_read() {
    Battery battery;
    uint16_t bus_raw, shunt_raw;
    if(ina_read(REG_BUS,&bus_raw)!=ESP_OK || ina_read(REG_SHUNT,&shunt_raw)!=ESP_OK) return battery;
    battery.pack_mv=bus_raw*5/4; // 1.25 mV LSB
    if(battery.pack_mv<5000) return battery; // below the 2-cell minimum: no pack
    battery.present=true;
    battery.percent=std::clamp((battery.pack_mv/2-3300)*100/(4150-3300),0,100);
    // Shunt LSB is 2.5 uV across 5 mOhm = 0.5 mA/LSB; M5Unified reports the
    // charge current as the negated shunt current.
    battery.current_ma=-static_cast<int16_t>(shunt_raw)/2;
    if(auto io=expander.load()) {
        uint32_t level=0;
        if(esp_io_expander_get_level(io,PIN_CHG_STAT,&level)==ESP_OK) battery.charging=(level&PIN_CHG_STAT)!=0;
    }
    return battery;
}
namespace {
std::atomic<bool> motion_running{false};
std::atomic<void(*)()> motion_callback{nullptr};
void motion_task(void*) {
    bmi270_driver_config_t driver={};
    driver.addr=BMI270_I2C_ADDRESS_L;
    driver.interface=BMI270_USE_I2C;
    driver.i2c_bus=bsp_i2c_get_handle();
    bmi270_handle_t* imu=nullptr;
    esp_err_t result=driver.i2c_bus?bmi270_create(&driver,&imu):ESP_ERR_INVALID_STATE;
    if(result==ESP_OK) {
        bmi270_config_t config{BMI270_ACC_ODR_50_HZ,BMI270_ACC_RANGE_4_G,BMI270_GYR_ODR_50_HZ,BMI270_GYR_RANGE_500_DPS};
        result=bmi270_start(imu,&config);
    }
    if(result!=ESP_OK) {
        ESP_LOGW(TAG,"BMI270 unavailable: %s; motion wake disabled",esp_err_to_name(result));
        if(imu) bmi270_delete(imu);
        motion_running=false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG,"Motion detection started: >%d dps or >%d mg, 25 Hz",CONFIG_TAB5_MOTION_DPS,CONFIG_TAB5_MOTION_MILLI_G);
    float bx=0,by=0,bz=0;
    bool seeded=false;
    unsigned failures=0;
    TickType_t last_trigger=0, wake=xTaskGetTickCount();
#ifdef CONFIG_TAB5_MOTION_LOG
    float max_dps=0, max_mg=0;
    TickType_t log_at=wake+pdMS_TO_TICKS(1000);
#endif
    for(;;) {
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(40));
        float ax,ay,az,gx,gy,gz;
        if(bmi270_get_acce_data(imu,&ax,&ay,&az)!=ESP_OK || bmi270_get_gyro_data(imu,&gx,&gy,&gz)!=ESP_OK) {
            if(++failures==25) ESP_LOGW(TAG,"IMU reads failing on the I2C bus");
            continue;
        }
        failures=0;
        if(!seeded) { bx=ax; by=ay; bz=az; seeded=true; continue; }
        // Slow baseline (~0.8 s time constant): gravity is filtered out while
        // a pickup still counts as motion until the baseline catches up.
        float dx=ax-bx,dy=ay-by,dz=az-bz;
        bx+=dx*0.05f; by+=dy*0.05f; bz+=dz*0.05f;
        float dps=sqrtf(gx*gx+gy*gy+gz*gz);
        float mg=sqrtf(dx*dx+dy*dy+dz*dz)*1000.f;
#ifdef CONFIG_TAB5_MOTION_LOG
        max_dps=std::max(max_dps,dps); max_mg=std::max(max_mg,mg);
        if(TickType_t now=xTaskGetTickCount(); now>=log_at) {
            ESP_LOGI(TAG,"motion max: %.1f dps, %.0f mg",max_dps,max_mg);
            max_dps=max_mg=0; log_at=now+pdMS_TO_TICKS(1000);
        }
#endif
        if(dps>CONFIG_TAB5_MOTION_DPS || mg>CONFIG_TAB5_MOTION_MILLI_G) {
            TickType_t now=xTaskGetTickCount();
            if(now-last_trigger>=pdMS_TO_TICKS(500)) {
                last_trigger=now;
                if(auto callback=motion_callback.load()) callback();
            }
        }
    }
}
}
void motion_start(void (*on_motion)()) {
    motion_callback=on_motion;
    bool expected=false;
    if(!motion_running.compare_exchange_strong(expected,true)) return;
    if(xTaskCreate(motion_task,"motion",4096,nullptr,2,nullptr)!=pdPASS) {
        motion_running=false;
        ESP_LOGE(TAG,"Unable to start the motion task");
    }
}
bool motion_active() { return motion_running.load(); }
