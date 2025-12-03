#include "i2c_device.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "I2cDevice"


I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C设备初始化失败, 地址=0x%02X, 错误码=0x%x", addr, err);
        i2c_device_ = nullptr;
        initialized_ = false;
        return;
    }

    if (i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "I2C设备句柄为空, 地址=0x%02X", addr);
        initialized_ = false;
        return;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "I2C设备初始化成功, 地址=0x%02X", addr);
}

esp_err_t I2cDevice::WriteRegWithRetry(uint8_t reg, uint8_t value, int retries) {
    if (!initialized_ || i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "I2C设备未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[2] = {reg, value};
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < retries; i++) {
        err = i2c_master_transmit(i2c_device_, buffer, 2, I2C_DEFAULT_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (i < retries - 1) {
            ESP_LOGW(TAG, "I2C写入失败, reg=0x%02X, 重试 %d/%d, 错误=0x%x",
                     reg, i + 1, retries, err);
            vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "I2C写入失败, reg=0x%02X, 已重试%d次, 最后错误=0x%x", reg, retries, err);
    return err;
}

esp_err_t I2cDevice::ReadRegWithRetry(uint8_t reg, uint8_t* buffer, size_t length, int retries) {
    if (!initialized_ || i2c_device_ == nullptr) {
        ESP_LOGE(TAG, "I2C设备未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == nullptr || length == 0) {
        ESP_LOGE(TAG, "I2C读取参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < retries; i++) {
        err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, I2C_DEFAULT_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (i < retries - 1) {
            ESP_LOGW(TAG, "I2C读取失败, reg=0x%02X, 重试 %d/%d, 错误=0x%x",
                     reg, i + 1, retries, err);
            vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "I2C读取失败, reg=0x%02X, 已重试%d次, 最后错误=0x%x", reg, retries, err);
    return err;
}

bool I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    return WriteRegWithRetry(reg, value) == ESP_OK;
}

bool I2cDevice::ReadReg(uint8_t reg, uint8_t* value) {
    if (value == nullptr) {
        return false;
    }
    return ReadRegWithRetry(reg, value, 1) == ESP_OK;
}

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t value = 0;
    if (ReadRegWithRetry(reg, &value, 1) != ESP_OK) {
        ESP_LOGW(TAG, "ReadReg失败, reg=0x%02X, 返回默认值0", reg);
    }
    return value;
}

bool I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    return ReadRegWithRetry(reg, buffer, length) == ESP_OK;
}