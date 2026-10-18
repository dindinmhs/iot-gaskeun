#include <OneWire.h>
#include <DallasTemperature.h>

const byte MQ136_PIN = A0;
const byte MQ4_1_PIN = A1;
const byte MQ4_2_PIN = A2;
const byte MPX_PIN   = A3;
const byte DS_PIN    = 2;
const byte BUZZER_PIN = 3;

const float AMBANG_TEKANAN_KPA = 5.0;
// Ambang ADC sementara untuk uji buzzer; wajib dikalibrasi pada perangkat asli.
const float AMBANG_MQ4_2_ADC = 600.0;
const float AMBANG_MQ136_ADC = 600.0;
const unsigned long WAKTU_AMAN_GAS_MS = 10000;

OneWire oneWire(DS_PIN);
DallasTemperature ds18b20(&oneWire);

float bacaADC(byte pin) {
  analogRead(pin);
  delay(2);

  unsigned long total = 0;

  for (byte i = 0; i < 20; i++) {
    total += analogRead(pin);
    delay(2);
  }

  return total / 20.0;
}

bool perbaruiAlarmGas(bool melewatiAmbang, bool &alarmAktif, unsigned long &terakhirTerdeteksi) {
  if (melewatiAmbang) {
    alarmAktif = true;
    terakhirTerdeteksi = millis();
  } else if (alarmAktif && millis() - terakhirTerdeteksi >= WAKTU_AMAN_GAS_MS) {
    alarmAktif = false;
  }
  return alarmAktif;
}

void bunyikanAlarm(bool alarmTekanan, bool alarmMQ4_2, bool alarmMQ136) {
  if (!alarmMQ4_2 && !alarmMQ136) {
    // Pertahankan alarm tekanan lama: bunyi terus saat melewati ambang.
    digitalWrite(BUZZER_PIN, alarmTekanan ? HIGH : LOW);
    delay(1000);
    return;
  }

  // MQ-4 #2: dua bip; MQ-136: tiga bip. Jika keduanya aktif, bergantian.
  static bool giliranMQ4 = false;
  byte jumlahBip;
  if (alarmMQ4_2 && alarmMQ136) {
    giliranMQ4 = !giliranMQ4;
    jumlahBip = giliranMQ4 ? 2 : 3;
  } else {
    jumlahBip = alarmMQ4_2 ? 2 : 3;
  }

  for (byte slot = 0; slot < 10; slot++) {
    bool bunyi = slot < jumlahBip * 2 && slot % 2 == 0;
    digitalWrite(BUZZER_PIN, bunyi ? HIGH : LOW);
    delay(100);
  }
  digitalWrite(BUZZER_PIN, LOW);
}

void setup() {
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);

  Serial.begin(9600);
  analogReference(DEFAULT);
  ds18b20.begin();
}

void loop() {
  float mq136 = bacaADC(MQ136_PIN);
  float mq4_1 = bacaADC(MQ4_1_PIN);
  float mq4_2 = bacaADC(MQ4_2_PIN);

  // MPX dan referensi ADC memakai jalur suplai yang sama.
  float tekanan =
      (bacaADC(MPX_PIN) / 1023.0 - 0.04) / 0.09;

  // Alarm tekanan dan sensor gas memakai buzzer yang sama.
  bool alarmTekanan = tekanan > AMBANG_TEKANAN_KPA;
  static bool alarmMQ4_2Aktif = false;
  static bool alarmMQ136Aktif = false;
  static unsigned long terakhirMQ4_2Terdeteksi = 0;
  static unsigned long terakhirMQ136Terdeteksi = 0;
  bool alarmMQ4_2 = perbaruiAlarmGas(mq4_2 >= AMBANG_MQ4_2_ADC,
                                   alarmMQ4_2Aktif, terakhirMQ4_2Terdeteksi);
  bool alarmMQ136 = perbaruiAlarmGas(mq136 >= AMBANG_MQ136_ADC,
                                   alarmMQ136Aktif, terakhirMQ136Terdeteksi);
  bunyikanAlarm(alarmTekanan, alarmMQ4_2, alarmMQ136);

  ds18b20.requestTemperatures();
  float suhu = ds18b20.getTempCByIndex(0);

  // Kirim JSON ke ESP32 melalui TX/D1.
  Serial.print(F("{\"mq136_adc\":"));
  Serial.print(mq136, 1);

  Serial.print(F(",\"mq4_1_adc\":"));
  Serial.print(mq4_1, 1);

  Serial.print(F(",\"mq4_2_adc\":"));
  Serial.print(mq4_2, 1);

  Serial.print(F(",\"tekanan_kpa\":"));
  Serial.print(tekanan, 3);

  Serial.print(F(",\"suhu_c\":"));

  if (suhu == DEVICE_DISCONNECTED_C) {
    Serial.print(F("null"));
  } else {
    Serial.print(suhu, 2);
  }

  Serial.print(F(",\"alarm_tekanan\":"));
  Serial.print(alarmTekanan ? F("true") : F("false"));

  Serial.println(F("}"));

}
