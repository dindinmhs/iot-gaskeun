#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include "firebase_sender.h"
#include "wifi_manager.h"

namespace OfflinePortal {

static WebServer server(80);
static DNSServer dnsServer;
static bool apActive = false;
static bool apSettingsValid = false;
static bool wasStationConnected = false;
static uint8_t onlineChecks = 0;
static uint32_t startedAt = 0;
static uint32_t lastProbeAt = 0;
static uint32_t lastAPAttemptAt = 0;
static bool triedAP = false;
static uint32_t lastWifiSetupAt = 0;

static const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Gaskeun - Pantau Sensor</title>
  <style>
    :root{color-scheme:light;font-family:system-ui,Arial,sans-serif}
    *{box-sizing:border-box}
    body{margin:0;background:#f4f7f6;color:#17322d}
    main{max-width:760px;margin:auto;padding:22px}
    h1{margin:0 0 8px;font-size:1.7rem}
    p{line-height:1.5}
    .muted{color:#526d66}
    .banner{padding:12px 16px;border-radius:10px;background:#dcefe7;margin:18px 0}
    .warning{background:#fff0cc}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(175px,1fr));gap:12px}
    .card{padding:16px;background:white;border:1px solid #d7e3df;border-radius:12px}
    .label{font-size:.9rem;color:#526d66;margin-bottom:8px}
    .value{font-size:1.5rem;font-weight:700;overflow-wrap:anywhere}
    .small{font-size:1rem}
    footer{margin-top:20px;font-size:.85rem;color:#526d66}
    section{margin-top:22px}
    label{display:block;margin:12px 0 6px}
    input,button{font:inherit;padding:10px;border:1px solid #a7bdb5;border-radius:8px}
    input{width:100%}
    button{margin-top:14px;background:#173e32;color:white;cursor:pointer}
    button:disabled{opacity:.6;cursor:wait}
  </style>
</head>
<body>
<main>
  <h1>Gaskeun · Pantau Sensor</h1>
  <p class="muted">Data langsung dari Arduino Nano lewat ESP32. Halaman ini tetap bekerja tanpa internet.</p>
  <div id="banner" class="banner">Memuat pembacaan...</div>
  <div class="grid">
    <div class="card"><div class="label">Tekanan</div><div class="value" id="pressure">—</div></div>
    <div class="card"><div class="label">Suhu</div><div class="value" id="temperature">—</div></div>
    <div class="card"><div class="label">MQ-4 #1 · biodigester</div><div class="value" id="mq4_1">—</div></div>
    <div class="card"><div class="label">MQ-4 #2 · penampung</div><div class="value" id="mq4_2">—</div></div>
    <div class="card"><div class="label">MQ-136 · sekitar filter</div><div class="value" id="mq136">—</div></div>
    <div class="card"><div class="label">Alarm tekanan Nano</div><div class="value small" id="pressure_alarm">—</div></div>
    <div class="card"><div class="label">Kualitas data</div><div class="value small" id="quality">—</div></div>
    <div class="card"><div class="label">Antrean ke Firebase</div><div class="value" id="queue">—</div></div>
  </div>
  <section class="card" aria-labelledby="wifi-heading">
    <h2 id="wifi-heading">Pengaturan WiFi</h2>
    <p class="muted">Masukkan jaringan WiFi 2,4 GHz baru. Jaringan disimpan setelah berhasil tersambung dan tetap digunakan setelah ESP32 dinyalakan ulang.</p>
    <p id="wifi-status" role="status" aria-live="polite">Memuat status WiFi...</p>
    <p id="wifi-feedback" role="alert"></p>
    <form id="wifi-form">
      <label for="wifi-ssid">Nama WiFi (SSID)</label>
      <input id="wifi-ssid" name="ssid" required autocomplete="off">
      <label for="wifi-password">Password WiFi</label>
      <input id="wifi-password" name="password" type="password" maxlength="64" autocomplete="new-password" aria-describedby="wifi-password-help">
      <p class="muted" id="wifi-password-help">Kosongkan untuk jaringan tanpa password.</p>
      <button id="wifi-submit" type="submit">Hubungkan dan simpan</button>
    </form>
    <p class="muted">Jika gagal, hotspot tetap aktif agar Anda bisa mencoba lagi. Setelah koneksi internet pulih, hotspot akan mati otomatis.</p>
  </section>
  <footer>
    <div id="status">—</div>
    <p>Nilai MQ adalah ADC mentah (0–1023), bukan konsentrasi gas dalam ppm.
    Ambang kebocoran purwarupa belum dikalibrasi.</p>
  </footer>
</main>
<script>
const el = id => document.getElementById(id);
let wifiSubmitting = false;
async function refreshWifi() {
  try {
    const response = await fetch('/api/wifi', {cache:'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const data = await response.json();
    el('wifi-status').textContent = (data.connected ? 'Tersambung ke ' : 'Jaringan terakhir: ') +
      (data.ssid || 'belum diatur') +
      (data.connected && data.ip ? ' | IP ESP32: ' + data.ip : '') + '. ' + data.message;
    if (!wifiSubmitting) el('wifi-submit').disabled = data.busy;
  } catch (error) {
    el('wifi-status').textContent = 'Status belum tersedia. Sambungkan kembali ke hotspot ESP32 jika masih aktif.';
  }
}
el('wifi-form').addEventListener('submit', async event => {
  event.preventDefault();
  wifiSubmitting = true;
  el('wifi-submit').disabled = true;
  try {
    const response = await fetch('/api/wifi', {
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded', 'X-Gaskeun-WiFi':'1'},
      body:new URLSearchParams({ssid:el('wifi-ssid').value, password:el('wifi-password').value})
    });
    const data = await response.json();
    el('wifi-feedback').textContent = data.message;
    if (response.ok) el('wifi-password').value = '';
  } catch (error) {
    el('wifi-feedback').textContent = 'Permintaan belum terkonfirmasi. Sambungkan kembali ke hotspot dan periksa status.';
  } finally {
    wifiSubmitting = false;
    el('wifi-submit').disabled = false;
    await refreshWifi();
  }
});
const number = (value, digits, unit) =>
  typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits) + unit : '—';
async function refresh() {
  try {
    const response = await fetch('/api/sensors', {cache:'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const data = await response.json();
    const fresh = data.nano_connected === true;
    el('banner').textContent = !fresh
      ? 'Data Nano tidak tersedia atau sudah lama. Nilai terakhir di bawah mungkin bukan kondisi saat ini.'
      : 'Data Nano aktif · pembacaan terakhir ' + data.nano_age_seconds + ' detik lalu.';
    el('banner').className = 'banner' + (fresh ? '' : ' warning');
    el('pressure').textContent = number(data.pressure_kpa, 3, ' kPa');
    el('temperature').textContent = number(data.temperature_c, 2, ' °C');
    el('mq4_1').textContent = number(data.mq4_1_adc, 1, ' ADC');
    el('mq4_2').textContent = number(data.mq4_2_adc, 1, ' ADC');
    el('mq136').textContent = number(data.mq136_adc, 1, ' ADC');
    el('pressure_alarm').textContent = data.alarm_tekanan == null
      ? '—' : (data.alarm_tekanan ? 'Aktif' : 'Tidak aktif');
    el('quality').textContent = data.quality === 'ok' ? 'Valid untuk model' :
      (data.quality === 'invalid' ? 'Tidak valid untuk model' : '—');
    el('queue').textContent = String(data.queue_count + data.untimed_count);
    el('status').textContent = 'Perangkat: ' + data.device_id +
      ' · Wi-Fi: ' + (data.wifi_connected ? 'tersambung' : 'terputus') +
      ' · Firebase: ' + (data.firebase_reachable ? 'terjangkau' : 'tidak terjangkau') +
      ' · Pengiriman: ' + (data.upload_healthy ? 'normal' : 'tertunda') +
      ' · Flash antrean: ' + (data.flash_queue_ready ? 'siap' : 'tidak siap');
  } catch (error) {
    el('banner').textContent = 'Koneksi ke ESP32 terputus. Coba sambungkan lagi ke hotspot.';
    el('banner').className = 'banner warning';
  }
}
refresh();
refreshWifi();
setInterval(refresh, 2000);
setInterval(refreshWifi, 2000);
</script>
</body>
</html>
)HTML";

static bool fromHotspot() {
  return apActive && server.client().localIP() == WiFi.softAPIP();
}

static void sendForbidden() {
  server.send(403, "text/plain", "Hanya tersedia melalui hotspot ESP32.");
}

void begin() {
  startedAt = millis();
  const char *headers[] = {"X-Gaskeun-WiFi"};
  server.collectHeaders(headers, 1);
  apSettingsValid = strlen(OFFLINE_AP_SSID) > 0 && strlen(OFFLINE_AP_SSID) <= 32 &&
                    strlen(OFFLINE_AP_PASSWORD) >= 8 && strlen(OFFLINE_AP_PASSWORD) <= 63 &&
                    strcmp(OFFLINE_AP_PASSWORD, "GANTI_PASSWORD_HOTSPOT") != 0;
  if (!apSettingsValid) {
    Serial.println("Hotspot nonaktif: isi OFFLINE_AP_SSID/PASSWORD di esp32_secrets.h.");
  }
  server.on("/", HTTP_GET, []() {
    if (!fromHotspot()) { sendForbidden(); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/sensors", HTTP_GET, []() {
    if (!fromHotspot()) { sendForbidden(); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", FirebaseSender::statusJson());
  });
  server.on("/api/wifi", HTTP_GET, []() {
    if (!fromHotspot()) { sendForbidden(); return; }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", WifiManager::statusJson());
  });
  server.on("/api/wifi", HTTP_POST, []() {
    if (!fromHotspot()) { sendForbidden(); return; }
    server.sendHeader("Cache-Control", "no-store");
    if (server.header("X-Gaskeun-WiFi") != "1") { sendForbidden(); return; }
    if (WifiManager::busy()) {
      server.send(409, "application/json", "{\"message\":\"Sedang mencoba jaringan. Tunggu hingga selesai.\"}");
      return;
    }
    if (!server.hasArg("ssid") || !server.hasArg("password") ||
        !WifiManager::requestConnect(server.arg("ssid"), server.arg("password"))) {
      server.send(400, "application/json", "{\"message\":\"SSID harus 1-32 byte. Password harus kosong, 8-63 byte, atau PSK heksadesimal 64 digit.\"}");
      return;
    }
    server.send(202, "application/json", "{\"message\":\"Mencoba WiFi baru. Tunggu hingga 20 detik.\"}");
  });
  server.onNotFound([]() {
    if (!fromHotspot()) { sendForbidden(); return; }
    if (server.method() == HTTP_GET) {
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
      server.send(302, "text/plain", "Buka pengaturan WiFi ESP32.");
      return;
    }
    server.send(404, "text/plain", "Halaman tidak ditemukan.");
  });
}

static void startAP() {
  if (apActive || !apSettingsValid) return;
  if (triedAP && uint32_t(millis() - lastAPAttemptAt) < 15000) return;
  triedAP = true;
  lastAPAttemptAt = millis();
  WiFi.mode(WIFI_AP_STA); // STA tetap mencoba reconnect ke Wi-Fi rumah.
  if (!WiFi.softAP(OFFLINE_AP_SSID, OFFLINE_AP_PASSWORD)) {
    Serial.println("Gagal mengaktifkan hotspot offline.");
    return;
  }
  server.begin();
  apActive = true;
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.print("Hotspot offline aktif. Buka http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");
}

static void stopAP() {
  if (!apActive) return;
  if (WifiManager::busy() || uint32_t(millis() - lastWifiSetupAt) < 15000) return;
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  apActive = false;
  triedAP = false;
  Serial.println("Firebase kembali terjangkau; hotspot offline dimatikan.");
}

void tick() {
  if (apActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
  if (WifiManager::busy()) {
    lastWifiSetupAt = millis();
    FirebaseSender::setCloudReachable(false);
    onlineChecks = 0;
    wasStationConnected = false;
    return;
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected || !FirebaseSender::configured()) {
    FirebaseSender::setCloudReachable(false);
    onlineChecks = 0;
    wasStationConnected = false;
    if (WifiManager::needsPortal() ||
        (!FirebaseSender::configured() && uint32_t(millis() - startedAt) >= 10000)) startAP();
    return;
  }

  // Saat boot, beri NTP kesempatan sinkron sebelum menyimpulkan internet putus.
  if (!FirebaseSender::clockReady() && uint32_t(millis() - startedAt) < 20000) {
    FirebaseSender::setCloudReachable(false);
    return;
  }

  // Probe boleh berhasil saat antrean tetap gagal dikirim; jangan matikan hotspot.
  if (!FirebaseSender::uploadsHealthy()) {
    onlineChecks = 0;
    if (uint32_t(millis() - startedAt) >= 10000) startAP();
  }

  if (!wasStationConnected || uint32_t(millis() - lastProbeAt) >= 30000) {
    wasStationConnected = true;
    lastProbeAt = millis();
    bool reachable = FirebaseSender::probeFirebase();
    FirebaseSender::setCloudReachable(reachable);
    if (reachable && FirebaseSender::uploadsHealthy()) {
      if (onlineChecks < 2) ++onlineChecks;
      if (onlineChecks >= 2) stopAP();
    } else {
      onlineChecks = 0;
      if (uint32_t(millis() - startedAt) >= 10000) startAP();
    }
  }
}

} // namespace OfflinePortal
