#ifndef CYD_STATUS_LED_H
#define CYD_STATUS_LED_H

#include "led/led.h"
#include "application.h"

#include <driver/gpio.h>
#include <esp_timer.h>

// LED biasa (bukan WS2812) di satu pin GPIO. Pola kedip mengikuti keadaan perangkat.
class CydStatusLed : public Led {
public:
    CydStatusLed(gpio_num_t gpio, bool active_low) : gpio_(gpio), active_low_(active_low) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << gpio_;
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&cfg);
        esp_timer_create_args_t args = {};
        args.callback = &CydStatusLed::TimerCb;
        args.arg = this;
        args.name = "cyd_status_led";
        esp_timer_create(&args, &timer_);
        Set(false);
    }

    virtual ~CydStatusLed() {
        if (timer_) {
            esp_timer_stop(timer_);
            esp_timer_delete(timer_);
        }
    }

    virtual void OnStateChanged() override {
        auto s = Application::GetInstance().GetDeviceState();
        if (s == kDeviceStateListening || s == kDeviceStateIdle) {
            Blink(0, true);          // menyala terus
        } else if (s == kDeviceStateSpeaking) {
            Blink(600, false);       // lambat
        } else if (s == kDeviceStateStarting) {
            Blink(150, false);       // cepat: mulai / setelan WiFi
        } else {
            Blink(350, false);       // sedang: menyambung dan lainnya
        }
    }

private:
    gpio_num_t gpio_;
    bool active_low_;
    esp_timer_handle_t timer_ = nullptr;
    bool on_ = false;

    void Set(bool on) {
        on_ = on;
        gpio_set_level(gpio_, (on != active_low_) ? 1 : 0);
    }

    void Blink(int period_ms, bool solid_on) {
        if (!timer_) return;
        esp_timer_stop(timer_);
        if (period_ms <= 0) {
            Set(solid_on);
            return;
        }
        Set(true);
        esp_timer_start_periodic(timer_, (uint64_t)period_ms * 1000);
    }

    static void TimerCb(void* arg) {
        auto self = static_cast<CydStatusLed*>(arg);
        self->Set(!self->on_);
    }
};

#endif
