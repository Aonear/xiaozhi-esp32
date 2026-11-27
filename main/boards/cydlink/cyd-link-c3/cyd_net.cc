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
}  // namespace

void CydNet::OnRequest(const uint8_t* p, uint16_t n) {
    if (n < 2) return;
    uint8_t op = p[0], id = p[1];
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

bool CydNet::Busy() { return g_busy.load(); }

void CydNet::Init() {
    if (g_task) return;
    g_mu = xSemaphoreCreateMutex();
    g_wake = xSemaphoreCreateBinary();
    xTaskCreate(Worker, "cyd_net", 8192, nullptr, 3, &g_task);
    CydLink::GetInstance().SetNetCallback([](const uint8_t* p, uint16_t n) { CydNet::OnRequest(p, n); });
}
