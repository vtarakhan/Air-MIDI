#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "mac_address.h"

#if !defined(TARGET_BOARD_A) && !defined(TARGET_BOARD_B) && !defined(DEVICE_GET_MAC)
  #error "Будь ласка, виберіть правильне оточення: board_a, board_b або get_mac!"
#endif

#ifndef DEVICE_GET_MAC
  // Визначаємо адреси та імена модулів
  #ifdef TARGET_BOARD_A
    #define DEVICE_NAME "Air MIDI Alpha"
    const uint8_t* REMOTE_MAC = MAC_MODULE_B;
    #define HAS_MODE_SWITCH
    #define MODE_SWITCH_PIN 2
  #else
    #define DEVICE_NAME "Air MIDI Beta"
    const uint8_t* REMOTE_MAC = MAC_MODULE_A;
  #endif

  bool isBluetoothMode = false;

  // Налаштування BLE MIDI (компілюється тільки для Board A)
  #ifdef TARGET_BOARD_A
    #include <BLEDevice.h>
    #include <BLEUtils.h>
    #include <BLEServer.h>
    #define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
    #define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"
    
    BLEServer *pServer = nullptr;
    BLEService *pService = nullptr;
    BLECharacteristic *pCharacteristic = nullptr;
    bool deviceConnected = false;

    class MyServerCallbacks: public BLEServerCallbacks {
        void onConnect(BLEServer* pServer) { deviceConnected = true; };
        void onDisconnect(BLEServer* pServer) { 
            deviceConnected = false; 
            if (isBluetoothMode) {
                BLEDevice::startAdvertising(); 
            }
        }
    };

    // Обробник ПРИЙОМУ MIDI-команд з комп'ютера по Bluetooth (Двосторонній BLE)
    class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *pChar) {
            std::string rxValue = pChar->getValue();
            if (rxValue.length() > 2) {
                // Відкидаємо 2 службові байти BLE MIDI (Header + Timestamp)
                for (int i = 2; i < rxValue.length(); i++) {
                    Serial.write(rxValue[i]); // Випльовуємо в реальний MIDI OUT (2N7000)
                }
            }
        }
    };
  #endif
#endif

// Структура MIDI пакету для ESP-NOW
struct __attribute__((packed)) MidiPacket {
    uint8_t data[3];
    uint8_t length;
};
MidiPacket txPacket;

#ifndef DEVICE_GET_MAC
// Класична сигнатура колбеку прийому по ESP-NOW (Двосторонній радіозв'язок)
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
    MidiPacket rxPacket;
    memcpy(&rxPacket, incomingData, sizeof(rxPacket));
    for(int i = 0; i < rxPacket.length; i++) {
        Serial.write(rxPacket.data[i]); // Випльовуємо в реальний MIDI OUT (2N7000)
    }
}

void startEspNow() {
    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
        
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo)); 
        memcpy(peerInfo.peer_addr, REMOTE_MAC, 6);
        peerInfo.channel = 1;  
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
    }
}

void startBluetooth() {
    #ifdef TARGET_BOARD_A
    BLEDevice::init(DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    pService = pServer->createService(BLEUUID(MIDI_SERVICE_UUID));
    
    pCharacteristic = pService->createCharacteristic(
        BLEUUID(MIDI_CHARACTERISTIC_UUID),
        BLECharacteristic::PROPERTY_READ   | 
        BLECharacteristic::PROPERTY_NOTIFY | 
        BLECharacteristic::PROPERTY_WRITE  | 
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    
    pCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
    pCharacteristic->setCallbacks(new MyCharacteristicCallbacks()); // Вмикаємо прийом по BLE
    
    pService->start();
    BLEDevice::getAdvertising()->addServiceUUID(MIDI_SERVICE_UUID);
    BLEDevice::startAdvertising();
    #endif
}

void sendMidiBle(uint8_t* packetData, uint8_t length) {
    #ifdef TARGET_BOARD_A
    if (!deviceConnected) return;
    uint8_t bleMidiPacket[5] = {0x80, 0x80, 0, 0, 0};
    for (int i = 0; i < length; i++) bleMidiPacket[2 + i] = packetData[i];
    pCharacteristic->setValue(bleMidiPacket, length + 2);
    pCharacteristic->notify();
    #endif
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
    Serial.println("==========================================\n");
#else
    Serial.begin(31250); // Швидкість MIDI

    // Опитування тумблера ТІЛЬКИ ПІД ЧАС ЗАПУСКУ
    #ifdef HAS_MODE_SWITCH
        pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
        delay(50); // Антибрязк контактів при увімкненні
        if (digitalRead(MODE_SWITCH_PIN) == LOW) {
            isBluetoothMode = true;
        }
    #endif

    // Ініціалізація вибраного інтерфейсу один раз на старті
    if (isBluetoothMode) {
        startBluetooth();
    } else {
        startEspNow();
    }
#endif
}

void loop() {
#ifndef DEVICE_GET_MAC

    if (Serial.available() > 0) {
        txPacket.length = 0;
        
        while (Serial.available() > 0 && txPacket.length < 3) {
            txPacket.data[txPacket.length] = Serial.read();
            txPacket.length++;
            delayMicroseconds(320); 
        }

        if (txPacket.length > 0) {
            if (!isBluetoothMode) {
                esp_now_send(REMOTE_MAC, (uint8_t *) &txPacket, sizeof(txPacket));
            } else {
                sendMidiBle(txPacket.data, txPacket.length);
            }
        }
    }
#endif
}