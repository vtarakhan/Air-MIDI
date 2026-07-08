#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mac_address.h"

// Заголовки NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#if !defined(TARGET_BOARD_A) && !defined(TARGET_BOARD_B) && !defined(DEVICE_GET_MAC)
  #error "Будь ласка, виберіть правильне оточення: board_a, board_b або get_mac!"
#endif

#define MIDI_UART_NUM      UART_NUM_1
#define TXD_PIN            (GPIO_NUM_21)
#define RXD_PIN            (GPIO_NUM_20)
#define UART_BUF_SIZE      (1024)

#ifndef DEVICE_GET_MAC
  #ifdef TARGET_BOARD_A
    #define DEVICE_NAME "Air MIDI Alpha"
    #define HAS_MODE_SWITCH
    #define MODE_SWITCH_PIN GPIO_NUM_2
    const uint8_t* REMOTE_MAC = MAC_MODULE_B;
  #else
    #define DEVICE_NAME "Air MIDI Beta"
    const uint8_t* REMOTE_MAC = MAC_MODULE_A;
  #endif

  bool isBluetoothMode = false;
#endif

// UUID для BLE MIDI згідно з специфікацією MMA
static const ble_uuid128_t midi_svc_uuid =
    BLE_UUID128_INIT(0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);

static const ble_uuid128_t midi_chr_uuid =
    BLE_UUID128_INIT(0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);

uint16_t midi_chr_val_handle;
uint16_t conn_handle;
bool ble_connected = false;

// Структура MIDI пакету для ESP-NOW
typedef struct __attribute__((packed)) {
    uint8_t data[3];
    uint8_t length;
} midi_packet_t;

// --- Ініціалізація UART драйвера ---
void init_midi_uart() {
    const uart_config_t uart_config = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MIDI_UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(MIDI_UART_NUM, &uart_config);
    uart_set_pin(MIDI_UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

#ifndef DEVICE_GET_MAC
// --- Колбек прийому по ESP-NOW ---
void wifi_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    midi_packet_t rx_packet;
    if (len == sizeof(midi_packet_t)) {
        memcpy(&rx_packet, data, sizeof(rx_packet));
        uart_write_bytes(MIDI_UART_NUM, (const char*)rx_packet.data, rx_packet.length);
    }
}

void start_esp_now() {
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(wifi_now_recv_cb));

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, REMOTE_MAC, 6);
    peer_info.channel = 1;
    peer_info.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
}

// --- NimBLE Колбеки та Налаштування (Двосторонній BLE MIDI) ---
#ifdef TARGET_BOARD_A
static int midi_chr_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t rx_buf[128];
        ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf), &len);
        
        if (len > 2) {
            // Парсинг: відкидаємо 2 байти BLE заголовків, пишемо чистий MIDI в UART
            uart_write_bytes(MIDI_UART_NUM, (const char*)&rx_buf[2], len - 2);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def midi_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &midi_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = &midi_chr_uuid.u,
            .access_cb = midi_chr_cb,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &midi_chr_val_handle,
        }, {
            0,
        } },
    },
    { 0 },
};

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen((char*)fields.name);
    fields.name_is_complete = 1;

    if (ble_gap_set_advertising_data(&fields) != 0) return;

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                ble_connected = true;
            } else {
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ble_connected = false;
            ble_app_advertise();
            break;
    }
    return 0;
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void start_ble_midi() {
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_advertise;
    ble_hs_cfg.gatts_register_cb = NULL;
    
    ble_svc_gap_init();
    ble_gatts_count_cfg(midi_svcs);
    ble_gatts_add_svcs(midi_svcs);
    ble_svc_gap_device_name_set(DEVICE_NAME);
    
    nimble_port_freertos_init(ble_host_task);
}

void send_midi_ble(uint8_t* data, uint8_t length) {
    if (!ble_connected) return;
    
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (om) {
        uint8_t header[2] = {0x80, 0x80}; // Базові лічильники Timestamp
        os_mbuf_prepend(om, 2);
        os_mbuf_copyinto(om, 0, header, 2);
        ble_gatt_notif_custom(conn_handle, midi_chr_val_handle, om);
    }
}
#endif

// --- Потік (Task) обробки фізичного входу MIDI IN ---
void midi_gate_task(void *pvParameters) {
    midi_packet_t tx_packet;
    uint8_t buffer[1];

    while (1) {
        int len = uart_read_bytes(MIDI_UART_NUM, buffer, 1, pdMS_TO_TICKS(10));
        if (len > 0) {
            tx_packet.data[0] = buffer[0];
            tx_packet.length = 1;

            int timeout_counter = 0;
            while (tx_packet.length < 3 && timeout_counter < 3) {
                len = uart_read_bytes(MIDI_UART_NUM, buffer, 1, 0);
                if (len > 0) {
                    tx_packet.data[tx_packet.length] = buffer[0];
                    tx_packet.length++;
                    timeout_counter = 0;
                } else {
                    esp_rom_delay_us(100);
                    timeout_counter++;
                }
            }

            if (!isBluetoothMode) {
                esp_now_send(REMOTE_MAC, (uint8_t *)&tx_packet, sizeof(tx_packet));
            } else {
                #ifdef TARGET_BOARD_A
                send_midi_ble(tx_packet.data, tx_packet.length);
                #endif
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

// --- Головна точка входу ESP-IDF ---
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef DEVICE_GET_MAC
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    printf("\n==========================================\n");
    printf(" MAC-АДРЕСА ЦІЄЇ ПЛАТИ: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("==========================================\n\n");
#else
    init_midi_uart();

    #ifdef HAS_MODE_SWITCH
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << MODE_SWITCH_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (gpio_get_level(MODE_SWITCH_PIN) == 0) {
            isBluetoothMode = true;
        }
    #endif

    if (isBluetoothMode) {
        #ifdef TARGET_BOARD_A
        start_ble_midi();
        #endif
    } else {
        start_esp_now();
    }

    xTaskCreate(midi_gate_task, "midi_gate_task", 4096, NULL, 10, NULL);
#endif
}