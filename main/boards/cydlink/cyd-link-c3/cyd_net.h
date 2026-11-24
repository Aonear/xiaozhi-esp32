#ifndef CYD_NET_H
#define CYD_NET_H

#include <cstdint>

// Layanan jaringan untuk CYD lewat UART (paket NET_REQ 0x86 / NET_DATA 0x0A).
// Satu permintaan aktif pada satu waktu; permintaan baru membatalkan yang lama.
namespace CydNet {
    void Init();                                   // pasang ke CydLink
    bool Busy();                                   // ada permintaan aktif (WiFi jangan masuk hemat daya)
    void OnRequest(const uint8_t* p, uint16_t n);  // dipanggil tugas UART (tidak boleh memblokir)
}

#endif
