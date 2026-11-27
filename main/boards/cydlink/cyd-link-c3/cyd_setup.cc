#include "cyd_setup.h"
#include "cyd_link.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "CydSetup"

static const char kApSsid[] = "CYD-Setup";
static const char kApPass[] = "cydsetup1";
static const char kNs[] = "cydcfg";

static std::atomic<bool> g_active{false};
static std::atomic<int> g_req{-1};              // permintaan yang menunggu: 1 nyala, 0 mati
static httpd_handle_t g_srv = nullptr;
static esp_netif_t* g_ap_netif = nullptr;
static esp_timer_handle_t g_timeout = nullptr;
static bool g_task_running = false;

// ---------------------------------------------------------------- halaman web (satu berkas, tanpa internet)
static const char kPage[] = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>CYD Setup</title>
<style>
body{font:15px system-ui,sans-serif;background:#10131a;color:#e8eaee;margin:0;padding:14px;max-width:560px;margin:auto}
h1{font-size:20px;margin:0 0 8px}h2{font-size:16px;color:#00c896;margin:20px 0 6px}
input,textarea{width:100%;box-sizing:border-box;background:#1b2030;color:#e8eaee;border:1px solid #394159;border-radius:8px;padding:9px;font:inherit;margin:3px 0}
input[type=color]{height:44px;padding:2px}input[type=checkbox]{width:auto}
textarea{height:130px}button{background:#00c896;color:#04201a;border:0;border-radius:8px;padding:11px 16px;font:inherit;font-weight:600;margin:8px 6px 0 0}
button.g{background:#2a3144;color:#e8eaee}small{color:#8b93a1;display:block}
.st{background:#1b2030;border-radius:8px;padding:8px;font-size:13px;color:#9aa5bd;white-space:pre-line}
</style></head><body>
<h1>CYD Setup</h1><div class="st" id="st">memuat...</div>

<h2>Stasiun radio / musik</h2>
<small>Satu per baris, format: Nama|alamat (siaran mp3 atau file mp3). Dipakai fitur online berikutnya.</small>
<textarea id="st2" placeholder="Radio Contoh|http://contoh.com/stream.mp3"></textarea>
<label><input type="checkbox" id="lo"> Lirik online (LRCLIB)</label>
<button onclick="save(['st','lo'])">Simpan stasiun</button>

<h2>Tabungan (Google Sheets)</h2>
<small>Alamat Apps Script dan token rahasia. Token hanya disimpan di C3.</small>
<input id="su" placeholder="https://script.google.com/macros/s/.../exec">
<input id="sk" placeholder="token rahasia">
<button onclick="save(['su','sk'])">Simpan tabungan</button>

<h2>Telegram</h2>
<small>Buat bot lewat @BotFather, tempel token-nya. Kirim pesan ke bot dari Telegram, pesannya tampil di CYD (slot 1). ID chat boleh dikosongkan; isi bila hanya mau menerima dari satu orang.</small>
<input id="tt" placeholder="token bot (123456:ABC...)"><input id="tc" placeholder="ID chat (opsional)">
<button onclick="save(['tt','tc'])">Simpan Telegram</button>

<h2>Tampilan CYD</h2>
<input id="ti" maxlength="24" placeholder="Nama / sapaan (maks 24 huruf)">
<small>Warna aksen halaman TFT</small><input type="color" id="ac" value="#00c896">
<button onclick="save(['ti','ac'])">Simpan tampilan</button>

<h2>WiFi rumah</h2>
<small>Mengganti WiFi: C3 restart setelah disimpan.</small>
<input id="ws" placeholder="Nama WiFi (SSID)"><input id="wp" type="password" placeholder="Sandi WiFi">
<button onclick="wifi()">Simpan WiFi &amp; restart</button>

<h2>Selesai</h2>
<button class="g" onclick="fetch('/api/exit',{method:'POST'}).then(()=>document.body.innerHTML='<h1>Mode pengaturan ditutup</h1>')">Tutup mode pengaturan</button>
<script>
const $=i=>document.getElementById(i);
function load(){fetch('/api/get').then(r=>r.json()).then(d=>{
 $('st2').value=d.st||'';$('lo').checked=d.lo==='1';$('su').value=d.su||'';$('sk').value=d.sk||'';$('tt').value=d.tt||'';$('tc').value=d.tc||'';
 $('ti').value=d.ti||'';$('ac').value='#'+(d.ac||'00c896');
 $('st').textContent='IP '+d.ip+'  |  sinyal '+d.rssi+' dBm\nRAM bebas '+d.heap+' KB  |  WiFi: '+(d.ssid||'-')});}
function save(keys){const o={};
 for(const k of keys){if(k==='st')o.st=$('st2').value;else if(k==='lo')o.lo=$('lo').checked?'1':'0';
  else if(k==='ac')o.ac=$('ac').value.slice(1);else o[k]=$(k).value;}
 fetch('/api/set',{method:'POST',body:JSON.stringify(o)}).then(r=>r.ok?alert('Tersimpan'):alert('Gagal menyimpan'));}
function wifi(){if(!$('ws').value){alert('Isi nama WiFi');return}
 fetch('/api/wifi',{method:'POST',body:JSON.stringify({ssid:$('ws').value,pass:$('wp').value})}).then(r=>alert(r.ok?'Tersimpan, C3 restart...':'Gagal'));}
load();
</script></body></html>)HTML";

// ---------------------------------------------------------------- penyimpanan
std::string CydSetup::Get(const char* key, const char* def) {
    Settings s(kNs, false);
    return s.GetString(key, def);
}
static void Put(const char* key, const std::string& v) {
    Settings s(kNs, true);
    s.SetString(key, v);
}

// ---------------------------------------------------------------- kirim ke CYD
void CydSetup::ApplyToCyd() {
    auto& l = CydLink::GetInstance();
    std::string ti = Get("ti");
    uint8_t buf[40];
    buf[0] = 1;                                              // judul
    size_t n = ti.size() > 24 ? 24 : ti.size();
    memcpy(buf + 1, ti.data(), n);
    l.SendCfg(buf, (uint16_t)(n + 1));
    std::string ac = Get("ac", "00C896");
    unsigned rgb = 0x00C896;
    sscanf(ac.c_str(), "%x", &rgb);
    uint8_t c[4] = {2, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};   // warna aksen
    l.SendCfg(c, 4);
    uint8_t lo[2] = {4, (uint8_t)(Get("lo", "0") == "1" ? 1 : 0)};                // lirik online
    l.SendCfg(lo, 2);
    if (g_active.load()) {
        uint8_t st[40];
        st[0] = 3; st[1] = 1;                                // mode pengaturan aktif: "ssid\0sandi"
        int m = snprintf((char*)st + 2, sizeof(st) - 2, "%s%c%s", kApSsid, 0, kApPass);
        l.SendCfg(st, (uint16_t)(2 + (m > 0 ? m + 1 : 0)));
    } else {
        uint8_t st[2] = {3, 0};
        l.SendCfg(st, 2);
    }
}

// ---------------------------------------------------------------- server HTTP
static esp_err_t SendJson(httpd_req_t* req, cJSON* root) {
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return e;
}
static esp_err_t HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t HandleGet(httpd_req_t* req) {
    cJSON* o = cJSON_CreateObject();
    static const char* keys[] = {"st", "lo", "su", "sk", "ti", "ac", "tt", "tc"};
    for (const char* k : keys) cJSON_AddStringToObject(o, k, CydSetup::Get(k).c_str());
    esp_netif_ip_info_t ip = {};
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    char ipb[20] = "-";
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) snprintf(ipb, sizeof(ipb), IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(o, "ip", ipb);
    wifi_ap_record_t ap;
    bool ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cJSON_AddNumberToObject(o, "rssi", ok ? ap.rssi : 0);
    cJSON_AddStringToObject(o, "ssid", ok ? (const char*)ap.ssid : "");
    cJSON_AddNumberToObject(o, "heap", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return SendJson(req, o);
}
static bool ReadBody(httpd_req_t* req, char* buf, size_t max) {
    if (req->content_len == 0 || req->content_len >= max) return false;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    buf[got] = 0;
    return true;
}
static esp_err_t HandleSet(httpd_req_t* req) {
    char* buf = (char*)malloc(3072);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    bool ok = ReadBody(req, buf, 3072);
    cJSON* o = ok ? cJSON_Parse(buf) : nullptr;
    free(buf);
    if (!o) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad"); return ESP_FAIL; }
    static const char* keys[] = {"st", "lo", "su", "sk", "ti", "ac", "tt", "tc"};
    for (const char* k : keys) {
        cJSON* it = cJSON_GetObjectItem(o, k);
        if (cJSON_IsString(it) && it->valuestring) {
            std::string v = it->valuestring;
            if (v.size() > 1800) v.resize(1800);
            Put(k, v);
        }
    }
    cJSON_Delete(o);
    CydSetup::ApplyToCyd();
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}
static void RestartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}
static esp_err_t HandleWifi(httpd_req_t* req) {
    char* buf = (char*)malloc(512);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    bool ok = ReadBody(req, buf, 512);
    cJSON* o = ok ? cJSON_Parse(buf) : nullptr;
    free(buf);
    cJSON* s = o ? cJSON_GetObjectItem(o, "ssid") : nullptr;
    cJSON* p = o ? cJSON_GetObjectItem(o, "pass") : nullptr;
    if (!cJSON_IsString(s) || !s->valuestring || !s->valuestring[0]) {
        if (o) cJSON_Delete(o);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad");
        return ESP_FAIL;
    }
    SsidManager::GetInstance().AddSsid(s->valuestring, cJSON_IsString(p) && p->valuestring ? p->valuestring : "");
    cJSON_Delete(o);
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(RestartTask, "cs_rst", 2048, nullptr, 3, nullptr);
    return ESP_OK;
}
static esp_err_t HandleExit(httpd_req_t* req) {
    httpd_resp_sendstr(req, "ok");
    CydSetup::Request(false);
    return ESP_OK;
}

// ---------------------------------------------------------------- nyala / mati
static void TimeoutCb(void*) { CydSetup::Request(false); }

static void DoStart() {
    if (g_active.load()) return;
    if (!g_ap_netif) g_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!g_ap_netif) g_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {};
    memcpy(ap.ap.ssid, kApSsid, sizeof(kApSsid) - 1);
    ap.ap.ssid_len = sizeof(kApSsid) - 1;
    memcpy(ap.ap.password, kApPass, sizeof(kApPass));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e != ESP_OK) { ESP_LOGE(TAG, "hotspot gagal: %s", esp_err_to_name(e)); return; }
    esp_wifi_set_ps(WIFI_PS_NONE);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    if (httpd_start(&g_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server web gagal");
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    static const httpd_uri_t uris[] = {
        {"/", HTTP_GET, HandleRoot, nullptr},
        {"/api/get", HTTP_GET, HandleGet, nullptr},
        {"/api/set", HTTP_POST, HandleSet, nullptr},
        {"/api/wifi", HTTP_POST, HandleWifi, nullptr},
        {"/api/exit", HTTP_POST, HandleExit, nullptr},
    };
    for (const auto& u : uris) httpd_register_uri_handler(g_srv, &u);

    if (!g_timeout) {
        esp_timer_create_args_t a = {};
        a.callback = TimeoutCb;
        a.name = "cs_to";
        esp_timer_create(&a, &g_timeout);
    }
    esp_timer_stop(g_timeout);
    esp_timer_start_once(g_timeout, 10LL * 60 * 1000000);
    g_active = true;
    ESP_LOGW(TAG, "Mode pengaturan aktif: WiFi %s, sandi %s, buka 192.168.4.1", kApSsid, kApPass);
    CydSetup::ApplyToCyd();
}
static void DoStop() {
    if (!g_active.load()) { CydSetup::ApplyToCyd(); return; }
    if (g_timeout) esp_timer_stop(g_timeout);
    if (g_srv) { httpd_stop(g_srv); g_srv = nullptr; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    g_active = false;
    ESP_LOGW(TAG, "Mode pengaturan ditutup");
    CydSetup::ApplyToCyd();
}
static void WorkTask(void*) {
    for (;;) {
        int r = g_req.exchange(-1);
        if (r < 0) break;
        if (r == 1) DoStart(); else DoStop();
    }
    g_task_running = false;
    vTaskDelete(nullptr);
}

bool CydSetup::Active() { return g_active.load(); }

void CydSetup::Request(bool on) {
    g_req = on ? 1 : 0;
    if (!g_task_running) {
        g_task_running = true;
        if (xTaskCreate(WorkTask, "cyd_setup", 6144, nullptr, 3, nullptr) != pdPASS) g_task_running = false;
    }
}
