#ifndef CYD_SETUP_H
#define CYD_SETUP_H

#include <string>

// Mode pengaturan C3: hotspot "CYD-Setup" + halaman web di 192.168.4.1.
// Dinyalakan dari halaman Koneksi di CYD (paket CTRL butir 3), mati otomatis setelah 10 menit.
// Pengaturan disimpan di NVS (namespace "cydcfg"): stasiun, tabungan (url + token), judul, warna, lirik online.
namespace CydSetup {
    void Request(bool on);                 // aman dipanggil dari tugas UART: pekerjaan berat dijalankan di tugas sendiri
    bool Active();
    void ApplyToCyd();                     // kirim judul, warna, dan status mode pengaturan ke CYD
    std::string Get(const char* key, const char* def = "");   // baca nilai tersimpan (dipakai fitur online di batch berikutnya)
}

#endif
