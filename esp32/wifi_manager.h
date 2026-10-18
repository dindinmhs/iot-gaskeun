#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp32_secrets.h"

namespace WifiManager {

constexpr uint32_t RETRY_MS = 15000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;

struct Credentials {
  char ssid[33];
  char password[65];
};

static Credentials current = {};
static Credentials candidate = {};
static bool pending = false;
static bool testing = false;
static uint32_t requestedAt = 0;
static uint32_t attemptAt = 0;
static uint32_t offlineAt = 0;
static bool wasConnected = false;
static IPAddress lastStationIP;
static String message;

static bool validCredentials(const String &ssid, const String &password) {
  if (ssid.isEmpty() || ssid.length() > 32 || strlen(ssid.c_str()) != ssid.length() ||
      strlen(password.c_str()) != password.length()) return false;
  // WPA/WPA2: passphrase 8-63 byte atau PSK heksadesimal 64 digit.
  if (password.isEmpty()) return true; // Jaringan terbuka.
  if (password.length() >= 8 && password.length() <= 63) return true;
  if (password.length() != 64) return false;
  for (size_t i = 0; i < password.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(password[i]))) return false;
  }
  return true;
}

static bool save(const Credentials &credentials) {
  Preferences preferences;
  if (!preferences.begin("gaskeun-wifi", false)) return false;
  // Satu blob agar SSID dan password tidak tersimpan separuh saat listrik mati.
  bool ok = preferences.putBytes("credentials", &credentials, sizeof(credentials)) == sizeof(credentials);
  preferences.end();
  return ok;
}

static void connect(const Credentials &credentials) {
  attemptAt = millis();
  if (credentials.ssid[0]) WiFi.begin(credentials.ssid, credentials.password);
}

bool busy() {
  return pending || testing;
}

bool needsPortal() {
  return busy() || (WiFi.status() != WL_CONNECTED &&
         (!current.ssid[0] || uint32_t(millis() - offlineAt) >= 10000));
}

void begin() {
  Preferences preferences;
  if (preferences.begin("gaskeun-wifi", true)) {
    if (preferences.getBytesLength("credentials") == sizeof(current)) {
      preferences.getBytes("credentials", &current, sizeof(current));
    }
    preferences.end();
  }
  current.ssid[32] = 0;
  current.password[64] = 0;
  if (!validCredentials(current.ssid, current.password)) current = {};
  if (!current.ssid[0] && strcmp(WIFI_SSID, "NAMA_WIFI") != 0 &&
      strcmp(WIFI_PASSWORD, "PASSWORD_WIFI") != 0 &&
      validCredentials(WIFI_SSID, WIFI_PASSWORD)) {
    strlcpy(current.ssid, WIFI_SSID, sizeof(current.ssid));
    strlcpy(current.password, WIFI_PASSWORD, sizeof(current.password));
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true); // Pertahankan perilaku koneksi firmware awal.
  offlineAt = millis();
  connect(current);
  message = current.ssid[0] ? "Mencoba jaringan tersimpan." : "Belum ada jaringan. Isi WiFi di bawah.";
}

// Dipanggil handler HTTP; perpindahan jaringan ditunda agar respons sempat terkirim.
bool requestConnect(const String &ssid, const String &password) {
  if (busy() || !validCredentials(ssid, password)) return false;
  candidate = {};
  strlcpy(candidate.ssid, ssid.c_str(), sizeof(candidate.ssid));
  strlcpy(candidate.password, password.c_str(), sizeof(candidate.password));
  pending = true;
  requestedAt = millis();
  message = "Mencoba jaringan baru. Tunggu hingga 20 detik.";
  return true;
}

String statusJson() {
  StaticJsonDocument<768> doc;
  bool connected = WiFi.status() == WL_CONNECTED;
  doc["connected"] = connected;
  doc["busy"] = busy();
  doc["ssid"] = connected ? WiFi.SSID() : String(current.ssid);
  if (connected) doc["ip"] = WiFi.localIP().toString();
  else doc["ip"] = nullptr;
  doc["message"] = message;
  String json;
  serializeJson(doc, json);
  return json;
}

void tick() {
  if (pending && uint32_t(millis() - requestedAt) >= 500) {
    pending = false;
    testing = true;
    WiFi.setAutoReconnect(false); // Hanya hentikan reconnect saat mencoba jaringan pilihan pengguna.
    WiFi.disconnect(false, false); // Pertahankan hotspot dan kredensial terakhir.
    wasConnected = false;
    connect(candidate);
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && (!wasConnected || WiFi.localIP() != lastStationIP)) {
    lastStationIP = WiFi.localIP();
    Serial.print("WiFi tersambung ke ");
    Serial.println(WiFi.SSID());
    Serial.print("IP ESP32: ");
    Serial.println(lastStationIP);
  }
  if (!connected && wasConnected) offlineAt = millis();
  wasConnected = connected;

  if (testing) {
    if (connected && WiFi.SSID() == String(candidate.ssid)) {
      if (save(candidate)) {
        current = candidate;
        message = "WiFi tersambung. Jaringan baru sudah disimpan.";
        Serial.println("WiFi baru berhasil tersambung dan disimpan.");
      } else {
        message = "Gagal menyimpan jaringan. Silakan coba lagi.";
        WiFi.disconnect(false, false);
        connect(current);
      }
      candidate = {};
      testing = false;
      WiFi.setAutoReconnect(true);
    } else if (uint32_t(millis() - attemptAt) >= CONNECT_TIMEOUT_MS) {
      message = "Gagal tersambung. Periksa nama WiFi dan password, lalu coba lagi.";
      Serial.println("WiFi baru gagal; kembali mencoba jaringan sebelumnya.");
      WiFi.disconnect(false, false);
      connect(current);
      candidate = {};
      testing = false;
      WiFi.setAutoReconnect(true);
    }
    return;
  }
  if (!pending && !connected && current.ssid[0] &&
      uint32_t(millis() - attemptAt) >= RETRY_MS) {
    attemptAt = millis();
    WiFi.reconnect(); // Sama seperti retry firmware awal; jangan mulai ulang konfigurasi WiFi.
  }
}

} // namespace WifiManager
