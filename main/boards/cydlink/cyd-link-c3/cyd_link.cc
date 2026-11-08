#include "cyd_link.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "CydLink"
static constexpr uart_port_t kPort = UART_NUM_1;

enum : uint8_t {
    AIP_STATE = 0x01,
    AIP_TEXT = 0x02,
    AIP_AUDIO_START = 0x03,
    AIP_AUDIO_DATA = 0x04,
    AIP_AUDIO_END = 0x05,
    AIP_TOUCH = 0x81,
    AIP_BUFFER = 0x82,
};

CydLink& CydLink::GetInstance() {
    static CydLink instance;
    return instance;
}

void CydLink::Init(gpio_num_t tx, gpio_num_t rx, int baud) {
    if (inited_) return;
    uart_config_t cfg = {};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kPort, 2048, 4096, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    tx_mutex_ = xSemaphoreCreateMutex();
    audio_mutex_ = xSemaphoreCreateMutex();
    inited_ = true;
    xTaskCreate(RxTaskEntry, "cyd_link", 3072, this, 4, nullptr);
    ESP_LOGI(TAG, "UART ke CYD siap (%d baud)", baud);
}

void CydLink::Send(uint8_t type, const uint8_t* p, uint16_t n) {
    if (!inited_) return;
    uint8_t hdr[5] = {0xA5, 0x5A, type, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    uint8_t x = hdr[2] ^ hdr[3] ^ hdr[4];
    for (uint16_t i = 0; i < n; i++) x ^= p[i];
    xSemaphoreTake(tx_mutex_, portMAX_DELAY);
    uart_write_bytes(kPort, hdr, 5);
    if (n) uart_write_bytes(kPort, p, n);
    uart_write_bytes(kPort, &x, 1);
    xSemaphoreGive(tx_mutex_);
}

void CydLink::SendState(uint8_t state, uint8_t emo) {
    uint8_t pl[2] = {state, emo};
    Send(AIP_STATE, pl, 2);
}

void CydLink::SendText(uint8_t role, const char* text) {
    if (!text) return;
    uint8_t buf[200];
    buf[0] = role;
    size_t n = strlen(text);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf + 1, text, n);
    Send(AIP_TEXT, buf, (uint16_t)(n + 1));
}

void CydLink::AudioWrite(const int16_t* data, int samples, int rate, int volume_percent) {
    if (!inited_) return;
    xSemaphoreTake(audio_mutex_, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    if (!streaming_ || now > play_end_us_ + 400000) {
        uint8_t pl[2] = {(uint8_t)(rate & 0xFF), (uint8_t)(rate >> 8)};
        Send(AIP_AUDIO_START, pl, 2);
        streaming_ = true;
        play_end_us_ = now;
    }
    if (play_end_us_ < now) play_end_us_ = now;
    // jangan mengirim lebih dari ~250 ms di depan waktu putar (buffer CYD terbatas)
    int64_t ahead = play_end_us_ - now;
    if (ahead > 250000) vTaskDelay(pdMS_TO_TICKS((ahead - 250000) / 1000 + 1));

    int vol = volume_percent < 0 ? 0 : (volume_percent > 100 ? 100 : volume_percent);
    uint8_t out[480];
    int done = 0;
    while (done < samples) {
        int n = samples - done;
        if (n > (int)sizeof(out)) n = sizeof(out);
        for (int i = 0; i < n; i++) {
            int32_t v = ((int32_t)data[done + i] * vol) / 100;
            out[i] = (uint8_t)((v >> 8) + 128);
        }
        Send(AIP_AUDIO_DATA, out, (uint16_t)n);
        done += n;
    }
    play_end_us_ += (int64_t)samples * 1000000 / rate;
    xSemaphoreGive(audio_mutex_);
}

void CydLink::AudioIdleCheck() {
    if (xSemaphoreTake(audio_mutex_, 0) != pdTRUE) return;
    if (streaming_ && esp_timer_get_time() > play_end_us_ + 250000) {
        Send(AIP_AUDIO_END, nullptr, 0);
        streaming_ = false;
    }
    xSemaphoreGive(audio_mutex_);
}

void CydLink::Handle(uint8_t type, const uint8_t* p, uint16_t n) {
    if (type == AIP_TOUCH && n >= 1) {
        if (touch_cb_) touch_cb_(p[0]);
    }
    // AIP_BUFFER diterima tapi tidak dipakai: laju kirim diatur lewat waktu real-time di AudioWrite
}

void CydLink::RxTaskEntry(void* arg) {
    static_cast<CydLink*>(arg)->RxTask();
    vTaskDelete(nullptr);
}

void CydLink::RxTask() {
    uint8_t st = 0, type = 0, x = 0;
    uint16_t len = 0, idx = 0;
    uint8_t buf[64];
    uint8_t b;
    for (;;) {
        int r = uart_read_bytes(kPort, &b, 1, pdMS_TO_TICKS(100));
        if (r <= 0) {
            AudioIdleCheck();
            if (tick_cb_) tick_cb_();
            continue;
        }
        switch (st) {
            case 0: st = (b == 0xA5) ? 1 : 0; break;
            case 1: st = (b == 0x5A) ? 2 : ((b == 0xA5) ? 1 : 0); break;
            case 2: type = b; x = b; st = 3; break;
            case 3: len = b; x ^= b; st = 4; break;
            case 4:
                len |= (uint16_t)b << 8;
                x ^= b;
                idx = 0;
                if (len > sizeof(buf)) st = 0;
                else st = (len == 0) ? 6 : 5;
                break;
            case 5:
                buf[idx++] = b;
                x ^= b;
                if (idx >= len) st = 6;
                break;
            case 6:
                if (b == x) Handle(type, buf, len);
                st = 0;
                break;
        }
    }
}
