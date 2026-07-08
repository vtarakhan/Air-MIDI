#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "mac_address.h"

#if !defined(TARGET_BOARD_A) && !defined(TARGET_BOARD_B) && !defined(DEVICE_GET_MAC)
  #error "Будь ласка, виберіть правильне оточення: board_a, board_b або get_mac!"
#endif

#ifndef DEVICE_GET_MAC
  // Визначаємо адреси та імена
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
  bool lastButtonState = true; // Для відстеження зміни положення тумблера

  // Налаштування BLE MIDI (тільки для Board A)
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
    bool bleInitialized = false;

    class MyServerCallbacks: public BLEServerCallbacks {
        void onConnect(BLEServer* pServer) { deviceConnected = true; };
        void onDisconnect(BLEServer* pServer) { 
            deviceConnected = false; 
            if (isBluetoothMode) {
                BLEDevice::startAdvertising(); 
            }
        }
    };
  #endif
#endif

// Структура MIDI пакету
struct __attribute__((packed)) MidiPacket {
    uint8_t data[3];
    uint8_t length;
};
MidiPacket txPacket;

#ifndef DEVICE_GET_MAC
// Колбек прийому по ESP-NOW
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    MidiPacket rxPacket;
    memcpy(&rxPacket, incomingData, sizeof(rxPacket));
    for(int i = 0; i < rxPacket.length; i++) {
        Serial.write(rxPacket.data[i]); // Вивід у MIDI OUT через 2N7000
    }
}

// Функції старту/стопу режимів
void startEspNow() {
    WiFi.mode(WIFI_STA);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
        
        esp_now_peer_info_t peerInfo;
        memcpy(peerInfo.peer_addr, REMOTE_MAC, 6);
        peerInfo.channel = 1;  
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
    }
}

void stopEspNow() {
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
}

void startBluetooth() {
    #ifdef TARGET_BOARD_A
    if (!bleInitialized) {
        BLEDevice::init(DEVICE_NAME);
        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());
        pService = pServer->createService(BLEUUID(MIDI_SERVICE_UUID));
        pCharacteristic = pService->createCharacteristic(
            BLEUUID(MIDI_CHARACTERISTIC_UUID),
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
        );
        pCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
        pService->start();
        bleInitialized = true;
    }
    BLEDevice::getAdvertising()->addServiceUUID(MIDI_SERVICE_UUID);
    BLEDevice::startAdvertising();
    #endif
}

void stopBluetooth() {
    #ifdef TARGET_BOARD_A
    if (bleInitialized) {
        BLEDevice::getAdvertising()->stop();
        // Повністю гасити BLE стек "на льоту" через deinit(true) може призводити до паніки ядра на деяких версіях SDK, 
        // тому ми просто зупиняємо Advertising та відключаємо клієнтів. Це безпечно і швидко.
    }
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

    #ifdef HAS_MODE_SWITCH
        pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);
        delay(50);
        lastButtonState = digitalRead(MODE_SWITCH_PIN);
        isBluetoothMode = (lastButtonState == LOW);
    #endif

    // Початковий запуск залежно від тумблера
    if (isBluetoothMode) {
        startBluetooth();
    } else {
        startEspNow();
    }
#endif
}

void loop() {
#ifndef DEVICE_GET_MAC
    // 1. Динамічна перевірка тумблера «на льоту» (Тільки на Board A)
    #ifdef HAS_MODE_SWITCH
        bool currentButtonState = digitalRead(MODE_SWITCH_PIN);
        if (currentButtonState != lastButtonState) {
            delay(50); // Простий антибрязк
            if (digitalRead(MODE_SWITCH_PIN) == currentButtonState) {
                lastButtonState = currentButtonState;
                
                if (currentButtonState == LOW) {
                    // Перемикаємо на Bluetooth
                    stopEspNow();
                    isBluetoothMode = true;
                    startBluetooth();
                } else {
                    // Перемикаємо на ESP-NOW
                    stopBluetooth();
                    isBluetoothMode = false;
                    startEspNow();
                }
            }
        }
    #endif

    // 2. Читання та відправка MIDI повідомлень
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