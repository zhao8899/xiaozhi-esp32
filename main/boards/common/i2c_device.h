#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <driver/i2c_master.h>
#include <esp_err.h>

// I2C操作默认重试次数
#define I2C_DEFAULT_RETRY_COUNT 3
// I2C操作默认超时时间（毫秒）
#define I2C_DEFAULT_TIMEOUT_MS 100
// I2C重试间隔（毫秒）
#define I2C_RETRY_DELAY_MS 10

class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
    virtual ~I2cDevice() = default;

    // 检查设备是否初始化成功
    bool IsInitialized() const { return initialized_; }

protected:
    i2c_master_dev_handle_t i2c_device_ = nullptr;
    bool initialized_ = false;

    // 带重试机制的寄存器操作（推荐使用）
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadReg(uint8_t reg, uint8_t* value);
    bool ReadRegs(uint8_t reg, uint8_t* buffer, size_t length);

    // 兼容旧API：读取单个寄存器（失败返回0）
    uint8_t ReadReg(uint8_t reg);

    // 底层操作（带重试）
    esp_err_t WriteRegWithRetry(uint8_t reg, uint8_t value, int retries = I2C_DEFAULT_RETRY_COUNT);
    esp_err_t ReadRegWithRetry(uint8_t reg, uint8_t* buffer, size_t length, int retries = I2C_DEFAULT_RETRY_COUNT);
};

#endif // I2C_DEVICE_H
