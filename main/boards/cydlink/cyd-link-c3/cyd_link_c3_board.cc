#include "wifi_board.h"
#include "uart_audio_codec.h"
#include "cyd_display.h"
#include "cyd_link.h"
#include "application.h"
#include "mcp_server.h"
#include "button.h"
#include "config.h"
#include "cyd_status_led.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <string>
#include <esp_sleep.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "CydLinkC3Board"

// ---- Mode tidur C3 (diperintah dari CYD) ----
// Saat CYD mengirim perintah tidur, C3 restart lalu masuk "dormant" SEBELUM WiFi dinyalakan:
// tidak ada WiFi/hotspot, tidak ada mikrofon, hanya light sleep dan mendengarkan UART.
// CYD mengirim perintah bangun -> C3 restart normal. Cabut/pasang daya = C3 selalu hidup normal.
static RTC_NOINIT_ATTR uint32_t g_dormant_magic;
#define DORMANT_MAGIC 0xC3D0A5A5u

static void RunDormant() {
    ESP_LOGW(TAG, "C3 tidur: WiFi dan mikrofon tidak dinyalakan, menunggu perintah bangun dari CYD");
    uart_config_t cfg = {};
    cfg.baud_rate = CYD_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0);
    uart_param_config(UART_NUM_1, &cfg);
    uart_set_pin(UART_NUM_1, CYD_UART_TX_GPIO, CYD_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // paket bangun: A5 5A | 0x83 | panjang=1 | isi=0 | XOR
    static const uint8_t kWake[7] = {0xA5, 0x5A, 0x83, 0x01, 0x00, 0x00, 0x82};
    uint8_t win[7] = {0};
    for (;;) {
        int64_t until = esp_timer_get_time() + 40000;          // jendela baca 40 ms
        while (esp_timer_get_time() < until) {
            uint8_t b;
            if (uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(10)) > 0) {
                memmove(win, win + 1, 6);
                win[6] = b;
                if (memcmp(win, kWake, 7) == 0) {
                    ESP_LOGW(TAG, "Perintah bangun diterima, restart normal");
                    g_dormant_magic = 0;
                    esp_restart();
                }
            }
        }
        esp_sleep_enable_timer_wakeup(250000);                  // tidur ringan 250 ms (hemat daya)
        esp_light_sleep_start();
        vTaskDelay(1);
    }
}

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
        link.SetPowerCallback([](uint8_t cmd) {
            if (cmd == 1) {
                ESP_LOGW(TAG, "Perintah tidur dari CYD");
                g_dormant_magic = DORMANT_MAGIC;
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
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

            // 1) WiFi tanpa hemat daya saat sedang bercakap (suara tidak tertahan), hemat daya saat diam
            static int last_ps = -1;
            int want_ps = (st == 1 || st == 2 || st == 3) ? 0 : 1;
            if (want_ps != last_ps &&
                esp_wifi_set_ps(want_ps == 0 ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM) == ESP_OK) {
                last_ps = want_ps;
            }

            // 2) Kirim info C3 ke CYD tiap 2 detik: memori bebas, sinyal WiFi, level mikrofon
            static int64_t last_info_us = 0;
            uint32_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (now - last_info_us >= 2000000) {
                last_info_us = now;
                wifi_ap_record_t ap;
                int8_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
                uint8_t peak = l.mic_peak.exchange(0);
                l.SendInfo((uint16_t)(free_b / 1024), rssi, peak, l.mic_clips.load());
            }

            // 3) Pemulihan memori: kalau memori bebas turun jauh dan asisten sedang diam, restart C3
            static uint32_t baseline = 0;
            static int64_t idle_since_us = 0;
            if (st == 0) { if (!idle_since_us) idle_since_us = now; } else idle_since_us = 0;
            if (baseline == 0 && st == 0 && now > 90000000LL) baseline = free_b;
            if (baseline && st == 0 && idle_since_us && now - idle_since_us > 30000000LL &&
                free_b < baseline * 60 / 100) {
                ESP_LOGW(TAG, "Memori bebas turun (%u < %u), restart C3", (unsigned)free_b, (unsigned)baseline);
                esp_restart();
            }
        });
    }

public:
    // Perintah suara: AI memanggil fungsi ini untuk mengontrol layar CYD.
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.cyd.open_page",
            "Open a page on the motorcycle dashboard screen (the CYD). Use when the user asks to open, show, go to, "
            "or switch to an app or page. page must be exactly one of: home, map, chronos, music, clock, calendar, "
            "info, settings, menu, assistant. 'map' opens the Montirin navigation map. 'chronos' switches the "
            "dashboard to Chronos smartwatch mode and restarts it.",
            PropertyList({Property("page", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string page = properties["page"].value<std::string>();
                uint8_t id = 255;
                if (page == "home") id = 0;
                else if (page == "map") id = 1;
                else if (page == "clock") id = 2;
                else if (page == "calendar") id = 3;
                else if (page == "music") id = 4;
                else if (page == "info") id = 5;
                else if (page == "assistant") id = 6;
                else if (page == "settings") id = 7;
                else if (page == "menu") id = 8;
                else if (page == "chronos") id = 9;
                if (id == 255) return false;
                CydLink::GetInstance().SendNav(id);
                return true;
            });
        mcp.AddTool("self.cyd.set_volume",
            "Set the volume of the assistant voice on the dashboard speaker, from 0 to 100 percent.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [](const PropertyList& properties) -> ReturnValue {
                CydLink::GetInstance().SendSet(2, (uint8_t)properties["volume"].value<int>());
                return true;
            });
        mcp.AddTool("self.cyd.set_brightness",
            "Set the screen brightness of the motorcycle dashboard, from 5 to 100 percent.",
            PropertyList({Property("brightness", kPropertyTypeInteger, 5, 100)}),
            [](const PropertyList& properties) -> ReturnValue {
                CydLink::GetInstance().SendSet(1, (uint8_t)properties["brightness"].value<int>());
                return true;
            });
    }

public:
    CydLinkC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        if (g_dormant_magic == DORMANT_MAGIC && esp_reset_reason() == ESP_RST_SW) {
            RunDormant();            // tidak kembali: keluar lewat restart
        }
        g_dormant_magic = 0;
        display_ = new CydDisplay();
        InitializeButtons();
        InitializeLink();
        InitializeTools();
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
