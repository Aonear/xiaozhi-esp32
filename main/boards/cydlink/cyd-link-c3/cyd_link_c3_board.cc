#include "wifi_board.h"
#include "uart_audio_codec.h"
#include "cyd_display.h"
#include "cyd_link.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "cyd_status_led.h"

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "CydLinkC3Board"

class CydLinkC3Board : public WifiBoard {
private:
    Button boot_button_;
    CydDisplay* display_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // Status CYD: 0 diam, 1 mendengarkan, 2 berpikir, 3 berbicara, 4 menghubungkan
    static uint8_t ComputeState() {
        auto& link = CydLink::GetInstance();
        auto s = Application::GetInstance().GetDeviceState();
        if (s == kDeviceStateSpeaking) {
            link.thinking = false;
            return 3;
        }
        if (s == kDeviceStateListening) {
            return link.thinking ? 2 : 1;
        }
        if (s == kDeviceStateIdle) {
            link.thinking = false;
            return 0;
        }
        return 4;
    }

    void InitializeLink() {
        auto& link = CydLink::GetInstance();
        link.Init(CYD_UART_TX_GPIO, CYD_UART_RX_GPIO, CYD_UART_BAUD);
        link.SetTouchCallback([](uint8_t action) {
            // 1 = mulai/berhenti bicara, 2 = hentikan jawaban: keduanya lewat ToggleChatState
            Application::GetInstance().ToggleChatState();
        });
        link.SetTickCallback([]() {
            static uint8_t last_state = 255, last_emo = 255;
            static int64_t last_send_us = 0;
            auto& l = CydLink::GetInstance();
            uint8_t st = ComputeState();
            uint8_t emo = l.emotion;
            int64_t now = esp_timer_get_time();
            if (st != last_state || emo != last_emo || now - last_send_us >= 2000000) {
                l.SendState(st, emo);
                last_state = st;
                last_emo = emo;
                last_send_us = now;
            }
        });
    }

public:
    CydLinkC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        display_ = new CydDisplay();
        InitializeButtons();
        InitializeLink();
    }

    virtual Led* GetLed() override {
        static CydStatusLed led(LED_GPIO, LED_ACTIVE_LOW != 0);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static UartAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            MIC_I2S_GPIO_SCK, MIC_I2S_GPIO_WS, MIC_I2S_GPIO_DIN);
        return &audio_codec;
    }
};

DECLARE_BOARD(CydLinkC3Board);
