#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#if !defined(DEVICE_TRANSMITTER) && !defined(DEVICE_RECEIVER) && !defined(DEVICE_GET_MAC)
  #error "Будь ласка, виберіть правильне оточення в PlatformIO: transmitter, receiver або get_mac!"
#endif

#ifdef DEVICE_TRANSMITTER
  #include <BLEDevice.h>
  #include <BLEUtils.h>
  #include <BLEServer.h>
  #include "mac_address.h"
  
  #define MODE_SWITCH_PIN 2
  bool isBluetoothMode = false;
  bool deviceConnected = false;

  // Стандартні UUID для BLE MIDI за специфікацією MMA
  #define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
  #define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"

  BLECharacteristic *pCharacteristic;

  // Колбеки для відстеження підключення до Windows
  class MyServerCallbacks: public BLEServerCallbacks {
      void onConnect(BLEServer* pServer) { deviceConnected = true; };
      void onDisconnect(BLEServer* pServer) { 
          deviceConnected = false; 
          BLEDevice::startAdvertising(); // Перезапуск пошуку при обриві
      }
  };
#endif

struct __attribute__((packed)) MidiPacket {
    uint8_t data[3];
    uint8_t length;
};
MidiPacket midiPacket;

#ifdef DEVICE_RECEIVER
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    MidiPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));
    for(int i = 0; i < packet.length; i++) {
        Serial.write(packet.data[i]);
    }
}
#endif

// Функція для відправки сирих MIDI байтів через BLE
#ifdef DEVICE_TRANSMITTER
void sendMidiBle(uint8_t* packetData, uint8_t length) {
    if (!deviceConnected) return;

    // Пакет BLE MIDI вимагає заголовок (Header) та таймстамп (Timestamp)
    uint8_t bleMidiPacket[5];
    bleMidiPacket[0] = 0x80; // Header byte
    bleMidiPacket[1] = 0x80; // Timestamp byte

    for (int i = 0; i < length; i++) {
        bleMidiPacket[2 + i] = packetData[i];
    }

    pCharacteristic->setValue(bleMidiPacket, length + 2);
    pCharacteristic->notify();
}
#endif

void setup() {
#ifdef DEVICE_GET_MAC
    Serial.begin(115200);
    delay(1000); 
    WiFi.mode(WIFI_STA);
    Serial.println("\n==========================================");
    Serial.print(" MAC-АДРЕСА ЦІЄЇ ПЛАТИ: ");
    Serial.println(WiFi.macAddress());
    Serial.println(" Скопіюйте її у файл include/mac_address.h");
    Serial.println("==========================================\n");
    pinMode(8, OUTPUT); 
#else
    Serial.begin(31250); // Швидкість класичного MIDI
#endif

#ifdef DEVICE_TRANSMITTER
    pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
    delay(50);
    
    if (digitalRead(MODE_SWITCH_PIN) == LOW) {
        isBluetoothMode = true;
    }

    if (isBluetoothMode) {
        // Ініціалізація чистого BLE MIDI сервера
        BLEDevice::init("Pedalboard Wireless");
        BLEServer *pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());

        BLEService *pService = pServer->createService(BLEUUID(MIDI_SERVICE_UUID));
        pCharacteristic = pService->createCharacteristic(
            BLEUUID(MIDI_CHARACTERISTIC_UUID),
            BLECharacteristic::PROPERTY_READ   |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_WRITE  |
            BLECharacteristic::PROPERTY_WRITE_NR
        );
        
        // BLE MIDI вимагає шифрування/дозволів
        pCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);

        pService->start();
        
        BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
        pAdvertising->addServiceUUID(MIDI_SERVICE_UUID);
        pAdvertising->setScanResponse(true);
        BLEDevice::startAdvertising();
    } else {
        WiFi.mode(WIFI_STA);
        if (esp_now_init() == ESP_OK) {
            esp_now_peer_info_t peerInfo;
            memcpy(peerInfo.peer_addr, RECEIVER_MAC_ADDRESS, 6);
            peerInfo.channel = 1;  
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        }
    }
#endif

#ifdef DEVICE_RECEIVER
    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    }
#endif
}

void loop() {
#ifdef DEVICE_GET_MAC
    digitalWrite(8, HIGH); delay(200);
    digitalWrite(8, LOW);  delay(200);
#endif

#ifdef DEVICE_TRANSMITTER
    if (Serial.available() > 0) {
        midiPacket.length = 0;
        
        // Спільний парсер пакетів для обох режимів (читаємо команду повністю)
        while (Serial.available() > 0 && midiPacket.length < 3) {
            midiPacket.data[midiPacket.length] = Serial.read();
            midiPacket.length++;
            delayMicroseconds(320); 
        }

        if (!isBluetoothMode) {
            // === РЕЖИМ ESP-NOW ===
            esp_now_send(RECEIVER_MAC_ADDRESS, (uint8_t *) &midiPacket, sizeof(midiPacket));
        } else {
            // === РЕЖИМ BLUETOOTH LE MIDI ===
            sendMidiBle(midiPacket.data, midiPacket.length);
        }
    }
#endif

#ifdef DEVICE_RECEIVER
    delay(10); 
#endif
}