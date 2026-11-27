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
    void SetPowerCallback(std::function<void(uint8_t)> cb) { power_cb_ = cb; }
    void SetNetCallback(std::function<void(const uint8_t*, uint16_t)> cb) { net_cb_ = cb; }   // paket NET_REQ (0x86) dari CYD
    void SendNet(const uint8_t* p, uint16_t n) { Send(0x0A, p, n); }
    void SendTg(const uint8_t* p, uint16_t n) { Send(0x0C, p, n); }                          // pesan Telegram ke CYD [id(4), teks]
    void SendTime(const uint8_t* p, uint16_t n) { Send(0x0B, p, n); }                        // jam dari internet ke CYD                          // NET_DATA ke CYD
    void SetCtrlCallback(std::function<void(uint8_t, uint8_t)> cb) { ctrl_cb_ = cb; }   // butir CTRL >= 3 (3 = mode pengaturan, 4 = minta konfigurasi)
    std::atomic<bool> bt_enabled{false};     // saklar Bluetooth C3 dari CYD (menu Koneksi); dipakai fitur Bluetooth berikutnya
    std::atomic<bool> auto_listen{false};   // CYD di halaman AI: C3 mulai mendengarkan sendiri (tanpa tombol bicara)   // 1 = tidur, 0 = bangun (dari CYD)

    void SendState(uint8_t state, uint8_t emotion);
    void SendText(uint8_t role, const char* text);   // role: 0 pengguna, 1 asisten
    void SendNav(uint8_t page);                       // perintah suara: buka halaman di CYD
    void SendSet(uint8_t kind, uint8_t value);        // 1 = kecerahan, 2 = volume suara AI (0-100)
    void SendInfo(uint16_t heap_kb, int8_t rssi, uint8_t mic_peak, uint16_t mic_clips);
    void SendCfg(const uint8_t* p, uint16_t n);       // konfigurasi: [jenis, data...] 1 judul, 2 warna RGB, 3 mode pengaturan, 4 lirik online

    // Dipanggil dari tugas audio output: memutar PCM 16-bit mono ke CYD dengan laju real-time
    void AudioWrite(const int16_t* data, int samples, int rate, int volume_percent);

    // Status yang dipantau tampilan
    std::atomic<uint8_t> emotion{0};
    std::atomic<bool> thinking{false};
    std::atomic<uint8_t> mic_peak{0};      // puncak level mikrofon (0-100) sejak dikirim terakhir
    std::atomic<uint16_t> mic_clips{0};    // jumlah sampel mikrofon yang terpotong (kliping)

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
    std::function<void(uint8_t)> power_cb_;
    std::function<void(uint8_t, uint8_t)> ctrl_cb_;
    std::function<void(const uint8_t*, uint16_t)> net_cb_;
    bool streaming_ = false;
    int64_t play_end_us_ = 0;
};

#endif
