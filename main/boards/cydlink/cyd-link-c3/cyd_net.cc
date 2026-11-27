#include <esp_system.h>
#include "cyd_net.h"
#include <time.h>
#include "cyd_link.h"
#include "cyd_setup.h"
#include "board.h"
#include "http.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#if __has_include(<esp_http_client.h>) && __has_include(<esp_crt_bundle.h>)
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#define CYD_HAVE_RAW_HTTP 1                 // ikuti alihan (302) seperti alamat archive.org/download
#else
#define CYD_HAVE_RAW_HTTP 0
#endif

#define TAG "CydNet"

namespace {
constexpr int kChunk = 480;                 // isi data per paket
constexpr int kWindow = 16384;              // data yang boleh "menggantung" belum dikonsumsi CYD
constexpr int kMaxReq = 640;

enum : uint8_t { OP_GET = 1, OP_POST = 2, OP_CANCEL = 3, OP_CREDIT = 4, OP_SETTING = 5 };
enum : uint8_t { K_STATUS = 0, K_DATA = 1, K_END = 2, K_ERR = 3 };

struct Req {
    uint8_t op = 0, id = 0;
    std::string url, body;
};

SemaphoreHandle_t g_mu = nullptr;           // melindungi g_pending
SemaphoreHandle_t g_wake = nullptr;
Req g_pending;
bool g_has_pending = false;
std::atomic<uint8_t> g_cur_id{0};           // permintaan yang sedang berjalan
std::atomic<bool> g_cancel{false};
std::atomic<int> g_credit{kWindow};         // sisa jatah kirim
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_busy{false};

void Out(uint8_t id, uint8_t kind, const uint8_t* d, int n) {
    uint8_t buf[kChunk + 2];
    buf[0] = id; buf[1] = kind;
    if (n > 0) memcpy(buf + 2, d, n);
    CydLink::GetInstance().SendNet(buf, (uint16_t)(n + 2));
}
void OutErr(uint8_t id, const char* why) { Out(id, K_ERR, (const uint8_t*)why, (int)strlen(why)); }

bool Cancelled(uint8_t id) { return g_cancel.load() || g_cur_id.load() != id; }

// kirim data ke CYD dengan kontrol aliran; false bila dibatalkan atau macet
bool Push(uint8_t id, const uint8_t* d, int n) {
    int off = 0;
    while (off < n) {
        int c = n - off > kChunk ? kChunk : n - off;
        int64_t t0 = esp_timer_get_time();
        while (g_credit.load() < c) {
            if (Cancelled(id)) return false;
            if (esp_timer_get_time() - t0 > 15000000LL) { ESP_LOGW(TAG, "CYD tidak mengambil data"); return false; }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        g_credit -= c;
        Out(id, K_DATA, d + off, c);
        off += c;
    }
    return true;
}

void DoSetting(const Req& r) {
    std::string v;
    if (r.url == "tm") {                                   // jam dari internet (epoch lokal WIB), kosong bila belum tersinkron
        time_t t = time(nullptr);
        if (t > 1700000000) v = std::to_string((long long)t + 7 * 3600);
    } else {
        v = CydSetup::Get(r.url.c_str(), "");
    }
    uint16_t st = 200;
    Out(r.id, K_STATUS, (const uint8_t*)&st, 2);
    if (!v.empty()) Push(r.id, (const uint8_t*)v.data(), (int)v.size());
    Out(r.id, K_END, nullptr, 0);
}

#if CYD_HAVE_RAW_HTTP
// GET dengan klien HTTP bawaan ESP-IDF yang mengikuti alihan 301/302/303/307/308 (maksimal 6 kali)
void DoRaw(const Req& r) {
    esp_http_client_config_t cfg = {};
    cfg.url = r.url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "CYD-Dash/1.0";
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { OutErr(r.id, "no http"); return; }
    int status = 0;
    bool ok = false;
    for (int hop = 0; hop < 6 && !Cancelled(r.id); hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) { OutErr(r.id, "open gagal"); esp_http_client_cleanup(c); return; }
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status >= 300 && status < 400) {
            if (esp_http_client_set_redirection(c) != ESP_OK) break;
            esp_http_client_close(c);
            continue;
        }
        ok = true;
        break;
    }
    if (!ok) { OutErr(r.id, "alihan gagal"); esp_http_client_cleanup(c); return; }
    uint16_t st = (uint16_t)status;
    Out(r.id, K_STATUS, (const uint8_t*)&st, 2);
    static uint8_t rb[1024];
    int idle = 0;
    bool good = true;
    while (!Cancelled(r.id)) {
        int n = esp_http_client_read(c, (char*)rb, sizeof(rb));
        if (n > 0) { idle = 0; if (!Push(r.id, rb, n)) { good = false; break; } continue; }
        if (n == -ESP_ERR_HTTP_EAGAIN && ++idle < 6) continue;
        if (n < 0 && !esp_http_client_is_complete_data_received(c)) good = false;
        break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!Cancelled(r.id)) { if (good) Out(r.id, K_END, nullptr, 0); else OutErr(r.id, "putus"); }
}
#endif

void DoHttp(const Req& r) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) { OutErr(r.id, "no http"); return; }
    http->SetTimeout(10000);
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("User-Agent", "CYD-Dash/1.0");
    if (r.op == OP_POST) http->SetHeader("Content-Type", "application/json");
    auto opened = http->Open(r.op == OP_POST ? "POST" : "GET", r.url);
    if (!opened) { OutErr(r.id, "open gagal"); return; }
    if (r.op == OP_POST) {
        if (!r.body.empty()) {
            auto w = http->Write(r.body.data(), r.body.size());
            if (!w) { OutErr(r.id, "write gagal"); http->Close(); return; }
        }
        auto w = http->Write("", 0);
        if (!w) { OutErr(r.id, "write gagal"); http->Close(); return; }
    }
    auto status = http->GetStatusCode();
    if (!status) { OutErr(r.id, "status gagal"); http->Close(); return; }
    uint16_t st = (uint16_t)*status;
#if CYD_HAVE_RAW_HTTP
    if (r.op == OP_GET && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)) {
        http->Close();
        http.reset();                       // bebaskan RAM sebelum membuka sambungan TLS kedua
        DoRaw(r);
        return;
    }
#endif
    Out(r.id, K_STATUS, (const uint8_t*)&st, 2);
    static uint8_t rb[1024];
    bool ok = true;
    while (!Cancelled(r.id)) {
        auto n = http->Read((char*)rb, sizeof(rb));
        if (!n) { ok = false; break; }
        if (*n == 0) break;
        if (!Push(r.id, rb, (int)*n)) { ok = false; break; }
    }
    http->Close();
    if (!Cancelled(r.id)) { if (ok) Out(r.id, K_END, nullptr, 0); else OutErr(r.id, "putus"); }
}

void Worker(void*) {
    for (;;) {
        xSemaphoreTake(g_wake, portMAX_DELAY);
        for (;;) {
            Req r;
            xSemaphoreTake(g_mu, portMAX_DELAY);
            bool has = g_has_pending;
            if (has) { r = g_pending; g_has_pending = false; }
            xSemaphoreGive(g_mu);
            if (!has) break;
            g_cur_id = r.id;
            g_cancel = false;
            g_credit = kWindow;
            ESP_LOGI(TAG, "permintaan %d op=%d %s", r.id, r.op, r.url.c_str());
            g_busy = true;
            if (r.op == OP_SETTING) DoSetting(r); else DoHttp(r);
            g_busy = false;
        }
    }
}

// ---- screenshot CYD -> Google Drive (aliran: baris RGB565 dari CYD, dikonversi ke BMP 24-bit, base64, POST ke Apps Script) ----
struct ShotRow { uint16_t y; uint8_t half; uint8_t d[320]; };
QueueHandle_t g_shotq = nullptr;
std::atomic<bool> g_shot_run{false};

void ShotAck(uint8_t kind, uint16_t y = 0) {
    uint8_t pl[4] = {0, kind, (uint8_t)(y & 0xFF), (uint8_t)(y >> 8)};
    CydLink::GetInstance().SendNet(pl, (kind == 11 || kind == 13) ? 4 : 2);
}

size_t B64(const uint8_t* in, size_t n, char* out) {      // n kelipatan 3
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63]; out[o++] = t[(v >> 6) & 63]; out[o++] = t[v & 63];
    }
    return o;
}

void ShotTask(void*) {
    bool ok = false;
    int why = 9;
    do {
        std::string url = CydSetup::Get("su", ""), tok = CydSetup::Get("sk", "");
        if (url.compare(0, 4, "http") != 0 || tok.empty()) { ESP_LOGW(TAG, "shot: URL/token Sheet kosong"); why = 1; break; }
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (!http) { why = 2; break; }
        http->SetTimeout(20000);
        http->SetHeader("Content-Type", "application/json");
        http->SetHeader("User-Agent", "CYD-Dash/1.0");
        if (!http->Open("POST", url)) { ESP_LOGW(TAG, "shot: open gagal, heap %u", (unsigned)esp_get_free_heap_size()); why = 3; break; }
        std::string pre = "{\"token\":\"" + tok + "\",\"shot\":\"";
        if (!http->Write(pre.data(), pre.size())) { why = 4; break; }
        // kepala BMP 54 bita, 320x240, 24 bit, tinggi negatif (baris dari atas)
        uint8_t h[54] = {0};
        const uint32_t pix = 320 * 240 * 3, fsz = 54 + pix;
        h[0] = 'B'; h[1] = 'M';
        h[2] = fsz & 0xFF; h[3] = (fsz >> 8) & 0xFF; h[4] = (fsz >> 16) & 0xFF; h[5] = (fsz >> 24) & 0xFF;
        h[10] = 54; h[14] = 40;
        h[18] = 320 & 0xFF; h[19] = 320 >> 8;
        int32_t hh = -240; memcpy(h + 22, &hh, 4);
        h[26] = 1; h[28] = 24;
        h[34] = pix & 0xFF; h[35] = (pix >> 8) & 0xFF; h[36] = (pix >> 16) & 0xFF; h[37] = (pix >> 24) & 0xFF;
        h[38] = 0x13; h[39] = 0x0B; h[42] = 0x13; h[43] = 0x0B;
        static char out[2700];
        size_t ol = B64(h, 54, out);
        ShotAck(10);                                           // siap: CYD boleh mulai mengirim baris
        ShotRow row;
        int got = 0;
        bool fail = false;
        while (got < 480) {
            if (xQueueReceive(g_shotq, &row, pdMS_TO_TICKS(6000)) != pdTRUE) { fail = true; why = 7; break; }
            uint8_t bgr[480];
            for (int i = 0; i < 160; i++) {
                uint16_t px = row.d[2 * i] | (row.d[2 * i + 1] << 8);
                uint8_t r = (px >> 11) & 31, g = (px >> 5) & 63, b = px & 31;
                bgr[3 * i] = (b << 3) | (b >> 2);
                bgr[3 * i + 1] = (g << 2) | (g >> 4);
                bgr[3 * i + 2] = (r << 3) | (r >> 2);
            }
            if (ol + 640 > sizeof(out)) { if (!http->Write(out, ol)) { fail = true; break; } ol = 0; }
            ol += B64(bgr, 480, out + ol);
            got++;
        }
        if (fail) { http->Close(); break; }
        out[ol++] = '"'; out[ol++] = '}';
        if (!http->Write(out, ol)) break;
        if (!http->Write("", 0)) break;
        auto st = http->GetStatusCode();
        ESP_LOGI(TAG, "shot: status %d", st ? (int)*st : -1);
        ok = st && (*st == 200 || *st == 302 || *st == 301 || *st == 303);
        http->Close();
    } while (false);
    if (ok) ShotAck(12); else ShotAck(13, why);
    g_shot_run = false;
    vTaskDelete(nullptr);
}

void ShotRequest(const uint8_t* p, uint16_t n) {
    uint8_t op = p[0];
    if (op == 8) {                                             // mulai
        if (g_shot_run.load()) { ShotAck(13); return; }
        if (!g_shotq) g_shotq = xQueueCreate(8, sizeof(ShotRow));
        xQueueReset(g_shotq);
        g_shot_run = true;
        if (xTaskCreate(ShotTask, "cyd_shot", 8192, nullptr, 3, nullptr) != pdPASS) { g_shot_run = false; ShotAck(13); }
    } else if (op == 9 && n >= 5 + 320) {                      // baris: [9, 0, y(2), half, 320 byte]
        if (!g_shot_run.load() || !g_shotq) return;
        ShotRow r;
        r.y = p[2] | (p[3] << 8); r.half = p[4];
        memcpy(r.d, p + 5, 320);
        if (xQueueSend(g_shotq, &r, pdMS_TO_TICKS(3000)) == pdTRUE && r.half == 1) ShotAck(11, r.y);
    }
}

}  // namespace

void CydNet::OnRequest(const uint8_t* p, uint16_t n) {
    if (n < 2) return;
    uint8_t op = p[0], id = p[1];
    if (op == 8 || op == 9) { ShotRequest(p, n); return; }
    if (op == OP_CREDIT) {
        if (n >= 4 && id == g_cur_id.load()) {
            int v = p[2] | (p[3] << 8);
            int cur = g_credit.load();
            g_credit = cur + v > kWindow ? kWindow : cur + v;
        }
        return;
    }
    if (op == OP_CANCEL) {
        if (id == g_cur_id.load()) g_cancel = true;
        return;
    }
    if (op != OP_GET && op != OP_POST && op != OP_SETTING) return;
    Req r;
    r.op = op; r.id = id;
    const char* s = (const char*)p + 2;
    size_t rem = n - 2;
    size_t ul = strnlen(s, rem);
    r.url.assign(s, ul);
    if (op == OP_POST && ul + 1 < rem) r.body.assign(s + ul + 1, rem - ul - 1);
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_pending = r; g_has_pending = true;
    xSemaphoreGive(g_mu);
    g_cancel = true;                         // hentikan permintaan yang sedang berjalan agar yang baru jalan
    xSemaphoreGive(g_wake);
}

bool CydNet::Busy() { return g_busy.load() || g_shot_run.load(); }

void CydNet::Init() {
    if (g_task) return;
    g_mu = xSemaphoreCreateMutex();
    g_wake = xSemaphoreCreateBinary();
    xTaskCreate(Worker, "cyd_net", 8192, nullptr, 3, &g_task);
    CydLink::GetInstance().SetNetCallback([](const uint8_t* p, uint16_t n) { CydNet::OnRequest(p, n); });
}
