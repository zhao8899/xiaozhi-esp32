#include "backlight.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/ledc.h>
#include <cstdlib>
#include <algorithm>

#define TAG "Backlight"


Backlight::Backlight() {
    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnTransitionTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));
}

Backlight::~Backlight() {
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
}

void Backlight::RestoreBrightness() {
    // Load brightness from settings
    Settings settings("display");  
    int saved_brightness = settings.GetInt("brightness", 75);
    
    // 检查亮度值是否为0或过小，设置默认值
    if (saved_brightness <= 0) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (10)", saved_brightness);
        saved_brightness = 10;  // 设置一个较低的默认值
    }
    
    SetBrightness(saved_brightness);
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    if (brightness_ == brightness) {
        return;
    }

    if (permanent) {
        Settings settings("display", true);
        settings.SetInt("brightness", brightness);
    }

    target_brightness_ = brightness;

    // 优化：计算合适的步长，使渐变总时间约为 300ms
    // 使用 20ms 间隔代替 5ms，减少 CPU 唤醒次数
    int diff = abs(static_cast<int>(target_brightness_) - static_cast<int>(brightness_));
    if (diff > 0) {
        // 目标：约 15 步完成渐变（300ms / 20ms = 15）
        step_ = (target_brightness_ > brightness_) ?
                std::max(1, diff / 15) :
                -std::max(1, diff / 15);
    } else {
        step_ = (target_brightness_ > brightness_) ? 1 : -1;
    }

    if (transition_timer_ != nullptr) {
        // 使用 20ms 间隔代替 5ms，减少 75% 的 CPU 唤醒
        esp_timer_start_periodic(transition_timer_, 20 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d (step: %d)", brightness, step_);
}

void Backlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }

    // 使用可变步长，确保不会越过目标值
    int new_brightness = static_cast<int>(brightness_) + step_;
    if (step_ > 0) {
        // 增加亮度
        if (new_brightness >= target_brightness_) {
            brightness_ = target_brightness_;
        } else {
            brightness_ = static_cast<uint8_t>(new_brightness);
        }
    } else {
        // 减少亮度
        if (new_brightness <= target_brightness_) {
            brightness_ = target_brightness_;
        } else {
            brightness_ = static_cast<uint8_t>(new_brightness);
        }
    }

    SetBrightnessImpl(brightness_);

    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz) : Backlight() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz, //背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert,
        }
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::~PwmBacklight() {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

