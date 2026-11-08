#ifndef CYD_LINK_H
#define CYD_LINK_H

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdint>
#include <functional>

// Tautan UART ke CYD. Format paket: A5 5A | tipe | panjang(2B LE) | isi | XOR(tipe,panjang,isi)
// Harus sama dengan motor_dash_hybrid_v8.ino di sisi CYD.
class CydLink {
public:
    static CydLink& GetInstance();

    void Init(gpio_num_t tx, gpio_num_t rx, int baud);
    void SetTouchCallback(std::function<void(uint8_t)> cb) { touch_cb_ = cb; }
    void SetTickCallback(std::function<void()> cb) { tick_cb_ = cb; }

    void SendState(uint8_t state, uint8_t emotion);
    void SendText(uint8_t role, const char* text);   // role: 0 pengguna, 1 asisten

    // Dipanggil dari tugas audio output: memutar PCM 16-bit mono ke CYD dengan laju real-time
    void AudioWrite(const int16_t* data, int samples, int rate, int volume_percent);

    // Status yang dipantau tampilan
    std::atomic<uint8_t> emotion{0};
    std::atomic<bool> thinking{false};

private:
    CydLink() = default;
    void Send(uint8_t type, const uint8_t* p, uint16_t n);
    void AudioIdleCheck();
    static void RxTaskEntry(void* arg);
    void RxTask();
    void Handle(uint8_t type, const uint8_t* p, uint16_t n);

    bool inited_ = false;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    SemaphoreHandle_t audio_mutex_ = nullptr;
    std::function<void(uint8_t)> touch_cb_;
    std::function<void()> tick_cb_;
    bool streaming_ = false;
    int64_t play_end_us_ = 0;
};

#endif
