#pragma once

#include <Arduino.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include "esp32_secrets.h"
#include "firebase_ca.h"

namespace FirebaseSender {

constexpr uint32_t INTERVAL_MS = 300000; // 5 menit, sesuai config.json server.
constexpr uint32_t MAX_NANO_AGE_MS = 15000;
constexpr uint16_t CAPACITY = 288;       // Maksimum 24 jam; batas flash/RAM.
constexpr uint32_t FILE_MAGIC = 0x47535131;

struct Reading {
  uint64_t timestampMs;
  uint32_t uptimeMs;
  uint32_t sequence;
  float pressureKpa;
  float temperatureC;
  float mq4Biodigester;
  float mq4Holder;
  float mq136Ambient;
  bool temperatureValid;
  bool qualityOk;
};

struct FileHeader {
  uint32_t magic;
  uint32_t generation;
  uint16_t count;
  uint16_t recordSize;
  uint32_t checksum;
};

static Reading queue[CAPACITY];
static Reading scratch[CAPACITY];
static Reading untimed[CAPACITY];
static uint16_t queueCount = 0;
static uint16_t untimedCount = 0;
static uint32_t generation = 0;
static uint8_t activeSlot = 1;
static bool flashReady = false;
static bool configReady = false;
static bool cloudReachable = false;
static Reading latestNano = {};
static bool latestPressureAlarm = false;
static bool haveNano = false;
static uint32_t lastNanoAt = 0;
static uint32_t lastSampleAt = 0;
static uint32_t lastNoNanoLogAt = 0;
static uint32_t lastWifiAttemptAt = 0;
static uint32_t lastUploadAttemptAt = 0;
static bool lastUploadSucceeded = false;
static uint8_t uploadFailures = 0;
static bool wasConnected = false;
static char line[256];
static size_t lineLength = 0;
static bool discardLine = false;
static String idToken;
static uint32_t tokenExpiresAt = 0;

static uint32_t checksum(const Reading *items, uint16_t count) {
  uint32_t value = 2166136261UL;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(items);
  for (size_t i = 0; i < size_t(count) * sizeof(Reading); ++i) {
    value = (value ^ bytes[i]) * 16777619UL;
  }
  return value;
}

static bool loadSlot(uint8_t slot, Reading *items, uint16_t &count, uint32_t &fileGeneration) {
  File file = LittleFS.open(slot == 0 ? "/queue0.bin" : "/queue1.bin", "r");
  if (!file) return false;
  FileHeader header = {};
  bool valid = file.readBytes(reinterpret_cast<char *>(&header), sizeof(header)) == sizeof(header);
  valid = valid && header.magic == FILE_MAGIC && header.recordSize == sizeof(Reading);
  valid = valid && header.count <= CAPACITY;
  valid = valid && file.size() == sizeof(header) + size_t(header.count) * sizeof(Reading);
  if (valid) {
    size_t bytes = size_t(header.count) * sizeof(Reading);
    valid = file.readBytes(reinterpret_cast<char *>(items), bytes) == bytes;
    valid = valid && checksum(items, header.count) == header.checksum;
  }
  file.close();
  if (!valid) return false;
  count = header.count;
  fileGeneration = header.generation;
  return true;
}

static void loadQueue() {
  flashReady = LittleFS.begin(false); // Jangan hapus antrean jika mount gagal.
  if (!flashReady) {
    Serial.println("LittleFS gagal; antrean hanya tersedia di RAM.");
    return;
  }
  uint16_t count0 = 0, count1 = 0;
  uint32_t gen0 = 0, gen1 = 0;
  bool valid0 = loadSlot(0, queue, count0, gen0);
  bool valid1 = loadSlot(1, scratch, count1, gen1);
  if (valid1 && (!valid0 || int32_t(gen1 - gen0) > 0)) {
    memcpy(queue, scratch, size_t(count1) * sizeof(Reading));
    queueCount = count1;
    generation = gen1;
    activeSlot = 1;
  } else if (valid0) {
    queueCount = count0;
    generation = gen0;
    activeSlot = 0;
  }
  if ((LittleFS.exists("/queue0.bin") || LittleFS.exists("/queue1.bin")) &&
      !valid0 && !valid1) {
    Serial.println("PERINGATAN: antrean flash rusak atau versi file berubah.");
  }
  Serial.printf("Antrean dipulihkan: %u pembacaan.\n", queueCount);
}

static bool saveQueue() {
  if (!flashReady) return false;
  uint8_t nextSlot = activeSlot == 0 ? 1 : 0;
  File file = LittleFS.open(nextSlot == 0 ? "/queue0.bin" : "/queue1.bin", "w");
  if (!file) return false;
  FileHeader header = {FILE_MAGIC, generation + 1, queueCount,
                       uint16_t(sizeof(Reading)), checksum(queue, queueCount)};
  size_t bytes = size_t(queueCount) * sizeof(Reading);
  bool ok = file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) == sizeof(header);
  if (ok && bytes) ok = file.write(reinterpret_cast<const uint8_t *>(queue), bytes) == bytes;
  file.flush();
  ok = ok && file.size() == sizeof(header) + bytes;
  file.close();
  if (ok) {
    activeSlot = nextSlot;
    generation = header.generation;
  } else {
    Serial.println("Gagal menyimpan antrean ke flash; data masih di RAM.");
  }
  return ok;
}

static void enqueue(const Reading &reading) {
  if (queueCount == CAPACITY) {
    memmove(queue, queue + 1, size_t(queueCount - 1) * sizeof(Reading));
    --queueCount;
    Serial.println("PERINGATAN: antrean 24 jam penuh; data tertua dibuang.");
  }
  queue[queueCount++] = reading;
  saveQueue();
}

static uint64_t epochMs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1704067200 || tv.tv_sec > 4102444800LL) return 0;
  return uint64_t(tv.tv_sec) * 1000ULL + tv.tv_usec / 1000;
}

static bool validConfig() {
  String databaseUrl(FIREBASE_DATABASE_URL);
  if (databaseUrl.endsWith("/")) databaseUrl.remove(databaseUrl.length() - 1);
  String host = databaseUrl.substring(8);
  if (!databaseUrl.startsWith("https://") ||
      !(host.endsWith(".firebasedatabase.app") || host.endsWith(".firebaseio.com")) ||
      host.indexOf('/') >= 0 || host.indexOf("PROJECT") >= 0 ||
      strlen(FIREBASE_API_KEY) < 10 || strcmp(FIREBASE_API_KEY, "WEB_API_KEY") == 0 ||
      strcmp(WIFI_SSID, "NAMA_WIFI") == 0 ||
      strcmp(WIFI_PASSWORD, "PASSWORD_WIFI") == 0 ||
      strchr(DEVICE_EMAIL, '@') == nullptr ||
      strstr(DEVICE_EMAIL, "example.com") != nullptr ||
      strlen(DEVICE_PASSWORD) < 8 ||
      strcmp(DEVICE_PASSWORD, "PASSWORD_AKUN_PERANGKAT") == 0 ||
      strlen(DEVICE_ID) == 0) return false;
  for (const char *p = DEVICE_ID; *p; ++p) {
    if (!isalnum(static_cast<unsigned char>(*p)) && *p != '-' && *p != '_') return false;
  }
  return true;
}

static bool parseNano(const char *json, Reading &reading, bool &pressureAlarm) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return false;
  if (!doc["mq136_adc"].is<float>() || !doc["mq4_1_adc"].is<float>() ||
      !doc["mq4_2_adc"].is<float>() || !doc["tekanan_kpa"].is<float>()) return false;
  reading = {};
  reading.mq136Ambient = doc["mq136_adc"].as<float>();
  reading.mq4Biodigester = doc["mq4_1_adc"].as<float>();
  reading.mq4Holder = doc["mq4_2_adc"].as<float>();
  reading.pressureKpa = doc["tekanan_kpa"].as<float>();
  if (!isfinite(reading.mq136Ambient) || !isfinite(reading.mq4Biodigester) ||
      !isfinite(reading.mq4Holder) || !isfinite(reading.pressureKpa)) return false;
  if (reading.mq136Ambient < 0 || reading.mq136Ambient > 1023 ||
      reading.mq4Biodigester < 0 || reading.mq4Biodigester > 1023 ||
      reading.mq4Holder < 0 || reading.mq4Holder > 1023) return false;
  reading.temperatureValid = doc["suhu_c"].is<float>();
  if (reading.temperatureValid) {
    reading.temperatureC = doc["suhu_c"].as<float>();
    if (!isfinite(reading.temperatureC)) reading.temperatureValid = false;
  }
  reading.qualityOk = reading.pressureKpa >= 0 && reading.pressureKpa < 10 &&
                      reading.temperatureValid && reading.temperatureC >= -10 &&
                      reading.temperatureC <= 80;
  pressureAlarm = doc["alarm_tekanan"].is<bool>() &&
                  doc["alarm_tekanan"].as<bool>();
  return true;
}

void feedByte(char c) {
  if (c == '\n') {
    if (!discardLine && lineLength) {
      line[lineLength] = 0;
      Reading parsed;
      bool pressureAlarm = false;
      if (parseNano(line, parsed, pressureAlarm)) {
        latestNano = parsed;
        latestPressureAlarm = pressureAlarm;
        haveNano = true;
        lastNanoAt = millis();
      }
    }
    lineLength = 0;
    discardLine = false;
  } else if (c != '\r' && !discardLine) {
    if (lineLength + 1 < sizeof(line)) line[lineLength++] = c;
    else discardLine = true;
  }
}

static bool capture() {
  if (!haveNano || uint32_t(millis() - lastNanoAt) > MAX_NANO_AGE_MS) {
    if (uint32_t(millis() - lastNoNanoLogAt) >= 30000) {
      lastNoNanoLogAt = millis();
      Serial.println("Menunggu data Nano terbaru sebelum mengambil sampel.");
    }
    return false;
  }
  Reading reading = latestNano;
  reading.uptimeMs = millis();
  reading.timestampMs = epochMs();
  if (reading.timestampMs) {
    reading.sequence = uint32_t(reading.timestampMs / INTERVAL_MS);
    enqueue(reading);
  } else {
    if (untimedCount == CAPACITY) {
      memmove(untimed, untimed + 1, size_t(untimedCount - 1) * sizeof(Reading));
      --untimedCount;
      Serial.println("PERINGATAN: buffer tanpa waktu penuh; data tertua dibuang.");
    }
    untimed[untimedCount++] = reading;
    Serial.println("Jam belum sinkron; data ditampung di RAM.");
  }
  return true;
}

static void timestampUntimed() {
  uint64_t now = epochMs();
  if (!now || !untimedCount) return;
  uint32_t uptime = millis();
  for (uint16_t i = 0; i < untimedCount; ++i) {
    Reading reading = untimed[i];
    reading.timestampMs = now - uint32_t(uptime - reading.uptimeMs);
    reading.sequence = uint32_t(reading.timestampMs / INTERVAL_MS);
    enqueue(reading);
  }
  untimedCount = 0;
}

static void maintainWifi() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.println("Wi-Fi tersambung; sinkronisasi jam dimulai.");
    configTime(0, 0, "time.google.com", "pool.ntp.org");
  } else if (!connected && wasConnected) {
    Serial.println("Wi-Fi terputus; data tetap diantrikan.");
  }
  wasConnected = connected;
  if (!connected && uint32_t(millis() - lastWifiAttemptAt) >= 15000) {
    lastWifiAttemptAt = millis();
    WiFi.reconnect();
  }
}

static int request(const String &url, const char *method, const String &body,
                   String &response, uint16_t timeoutMs = 6000) {
  WiFiClientSecure tls;
  tls.setCACert(FIREBASE_ROOT_CA);
  HTTPClient http;
  if (!http.begin(tls, url)) return -1;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (body.length()) http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (code > 0) response = http.getString();
  http.end();
  return code;
}

bool configured() {
  return configReady;
}

bool clockReady() {
  return epochMs() != 0;
}

void setCloudReachable(bool reachable) {
  cloudReachable = reachable;
}

static bool ensureAuth();

bool probeFirebase() {
  if (!configReady || WiFi.status() != WL_CONNECTED || !epochMs() || !ensureAuth()) return false;
  String base(FIREBASE_DATABASE_URL);
  if (base.endsWith("/")) base.remove(base.length() - 1);
  String response;
  int code = request(base + "/gaskeun/v1/devices/" + DEVICE_ID + "/latest.json?auth=" +
                     idToken, "GET", "", response, 2500);
  if (code == 401) idToken = "";
  // 200 juga berlaku ketika latest belum ada (respons null).
  return code == 200;
}

bool uploadsHealthy() {
  return uploadFailures == 0 && (queueCount == 0 || lastUploadSucceeded);
}

String statusJson() {
  StaticJsonDocument<768> doc;
  doc["device_id"] = DEVICE_ID;
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["firebase_reachable"] = cloudReachable;
  doc["upload_healthy"] = cloudReachable && uploadsHealthy();
  doc["upload_failures"] = uploadFailures;
  doc["queue_count"] = queueCount;
  doc["untimed_count"] = untimedCount;
  doc["flash_queue_ready"] = flashReady;
  doc["nano_connected"] = haveNano && uint32_t(millis() - lastNanoAt) <= MAX_NANO_AGE_MS;
  if (haveNano) {
    doc["nano_age_seconds"] = uint32_t(millis() - lastNanoAt) / 1000;
    doc["pressure_kpa"] = latestNano.pressureKpa;
    if (latestNano.temperatureValid) doc["temperature_c"] = latestNano.temperatureC;
    else doc["temperature_c"] = nullptr;
    doc["mq4_1_adc"] = latestNano.mq4Biodigester;
    doc["mq4_2_adc"] = latestNano.mq4Holder;
    doc["mq136_adc"] = latestNano.mq136Ambient;
    doc["alarm_tekanan"] = latestPressureAlarm;
    doc["quality"] = latestNano.qualityOk ? "ok" : "invalid";
  }
  String json;
  serializeJson(doc, json);
  return json;
}

static bool signIn() {
  StaticJsonDocument<512> credentials;
  credentials["email"] = DEVICE_EMAIL;
  credentials["password"] = DEVICE_PASSWORD;
  credentials["returnSecureToken"] = true;
  if (credentials.overflowed()) return false;
  String body, response;
  serializeJson(credentials, body);
  int code = request(String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") +
                     FIREBASE_API_KEY, "POST", body, response);
  if (code != 200) {
    Serial.printf("Login Firebase gagal (HTTP %d); periksa akun/API key/jaringan.\n", code);
    return false;
  }
  DynamicJsonDocument result(4096);
  if (deserializeJson(result, response) || !result["idToken"].is<const char *>()) return false;
  idToken = result["idToken"].as<String>();
  const char *expiresText = result["expiresIn"] | "0";
  uint32_t expires = strtoul(expiresText, nullptr, 10);
  if (!expires || idToken.isEmpty()) {
    idToken = "";
    return false;
  }
  tokenExpiresAt = millis() + (expires > 120 ? expires - 60 : 60) * 1000UL;
  return true;
}

static bool ensureAuth() {
  if (!idToken.isEmpty() && int32_t(millis() - tokenExpiresAt) < 0) return true;
  idToken = "";
  return signIn();
}

static String toJson(const Reading &reading) {
  StaticJsonDocument<512> doc;
  doc["timestamp_ms"] = reading.timestampMs;
  doc["sequence"] = reading.sequence;
  doc["pressure_kpa"] = reading.pressureKpa;
  if (reading.temperatureValid) doc["temperature_c"] = reading.temperatureC;
  else doc["temperature_c"] = nullptr;
  doc["mq4_biodigester_raw"] = reading.mq4Biodigester;
  doc["mq4_holder_raw"] = reading.mq4Holder;
  doc["mq136_filter_ambient_raw"] = reading.mq136Ambient;
  doc["quality"] = reading.qualityOk ? "ok" : "invalid";
  doc["source"] = "device";
  if (doc.overflowed()) {
    Serial.println("JSON pembacaan terlalu besar; data tetap di antrean.");
    return "";
  }
  String json;
  serializeJson(doc, json);
  return json;
}

static bool uploadOldest() {
  if (!ensureAuth()) return false;
  const Reading &reading = queue[0];
  String base(FIREBASE_DATABASE_URL);
  if (base.endsWith("/")) base.remove(base.length() - 1);
  base += String("/gaskeun/v1/devices/") + DEVICE_ID;
  String token = String("?auth=") + idToken;
  char key[24];
  snprintf(key, sizeof(key), "%llu", static_cast<unsigned long long>(reading.timestampMs));
  String json = toJson(reading);
  if (json.isEmpty()) return false;
  String response;
  int code = request(base + "/readings/" + key + ".json" + token, "PUT", json, response);
  if (code == 401) idToken = "";
  if (code < 200 || code >= 300) {
    Serial.printf("Gagal tulis readings (HTTP %d); akan dicoba lagi.\n", code);
    return false;
  }

  response = "";
  code = request(base + "/latest.json" + token, "GET", "", response);
  if (code == 401) idToken = "";
  if (code != 200) {
    Serial.printf("Gagal baca latest (HTTP %d); data tetap di antrean.\n", code);
    return false;
  }
  DynamicJsonDocument latest(1024);
  if (deserializeJson(latest, response)) return false;
  uint64_t remoteTime = latest["timestamp_ms"].as<uint64_t>();
  if (remoteTime < reading.timestampMs) {
    response = "";
    code = request(base + "/latest.json" + token, "PUT", json, response);
    if (code == 401) idToken = "";
    if (code < 200 || code >= 300) {
      Serial.printf("Gagal tulis latest (HTTP %d); akan dicoba lagi.\n", code);
      return false;
    }
  }
  memmove(queue, queue + 1, size_t(queueCount - 1) * sizeof(Reading));
  --queueCount;
  saveQueue();
  Serial.printf("Pembacaan terkirim; sisa antrean: %u.\n", queueCount);
  return true;
}

void begin() {
  loadQueue();
  lastSampleAt = millis() - INTERVAL_MS;
  configReady = validConfig();
  if (!configReady) {
    Serial.println("Konfigurasi belum lengkap. Isi esp32_secrets.h lalu upload ulang.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
}

void tick() {
  if (!configReady) return;
  maintainWifi();
  timestampUntimed();
  if (uint32_t(millis() - lastSampleAt) >= INTERVAL_MS) {
    if (capture()) lastSampleAt = millis();
  }
  uint32_t retryMs = lastUploadSucceeded ? 1000 : 15000;
  if (WiFi.status() == WL_CONNECTED && cloudReachable && epochMs() && queueCount &&
      uint32_t(millis() - lastUploadAttemptAt) >= retryMs) {
    lastUploadAttemptAt = millis();
    lastUploadSucceeded = uploadOldest();
    if (lastUploadSucceeded) uploadFailures = 0;
    else if (uploadFailures < UINT8_MAX) ++uploadFailures;
  }
  delay(10);
}

} // namespace FirebaseSender
