#include <Arduino.h>
#include "firebase_sender.h"
#include "offline_portal.h"

const int NANO_RX_PIN = 16;  // Menerima dari TX Nano
const int NANO_TX_PIN = 17;  // Mengirim ke RX Nano

void setup() {
  // Serial Monitor melalui USB ESP32
  Serial.begin(115200);

  // UART untuk komunikasi dengan Nano
  Serial2.setRxBufferSize(8192);
  Serial2.begin(
    9600,
    SERIAL_8N1,
    NANO_RX_PIN,
    NANO_TX_PIN
  );

  Serial.println("Menunggu data Arduino Nano...");
  FirebaseSender::begin();
  OfflinePortal::begin();
}

void loop() {
  while (Serial2.available() > 0) {
    char c = char(Serial2.read());
    Serial.write(c);
    FirebaseSender::feedByte(c);
  }
  OfflinePortal::tick();
  FirebaseSender::tick();
}
