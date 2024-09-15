/**
 * A test program for the ESP32 that uses ESP-NOW protocol to communicate between 2 ESP32-S3 devices.
 * This program compiles using the ESP-IDF library and tools. 
 * 
 * This code was created by Claude. 
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_mac.h"

static const char *TAG = "ESP-NOW COMM";

#define CHANNEL 1

static uint8_t peer_mac[ESP_NOW_ETH_ALEN] = {0};
static bool peer_found = false;
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "Last Packet Send Status: %s to MAC: "MACSTR, 
             status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail",
             MAC2STR(mac_addr));
}

static void on_data_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
{
    if (!peer_found && memcmp(esp_now_info->src_addr, broadcast_mac, ESP_NOW_ETH_ALEN) != 0) {
        memcpy(peer_mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
        peer_found = true;
        ESP_LOGI(TAG, "PEER FOUND! MAC: "MACSTR, MAC2STR(peer_mac));

        esp_now_peer_info_t peer_info = {
            .channel = CHANNEL,
            .encrypt = false,
        };
        memcpy(peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
    }

    ESP_LOGI(TAG, "Data received from MAC: "MACSTR", len: %d", 
             MAC2STR(esp_now_info->src_addr), data_len);
    ESP_LOGI(TAG, "Receiving message: %.*s", data_len, data);
}

static esp_err_t init_esp_now(void)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing ESP-NOW");
        return ret;
    }

    esp_now_register_send_cb(on_data_sent);
    esp_now_register_recv_cb(on_data_recv);

    // Add broadcast address as a peer
    esp_now_peer_info_t broadcast_peer = {
        .channel = CHANNEL,
        .encrypt = false,
    };
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    ret = esp_now_add_peer(&broadcast_peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add broadcast peer");
        return ret;
    }

    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    ESP_ERROR_CHECK(init_esp_now());

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "-----------------------------------------------");
    ESP_LOGI(TAG, "My MAC Address: "MACSTR, MAC2STR(my_mac));
    ESP_LOGI(TAG, "-----------------------------------------------");

    char message[64];
    int sequence_number = 0;

    while (1) {
        snprintf(message, sizeof(message), "Hello from %02X%02X_%d", my_mac[4], my_mac[5], sequence_number++);
        
        if (peer_found) {
            esp_err_t result = esp_now_send(peer_mac, (const uint8_t *)message, strlen(message));
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "Sending message: %s", message);
            } else {
                ESP_LOGE(TAG, "Error sending message to peer: %s", esp_err_to_name(result));
            }
        } else {
            ESP_LOGW(TAG, "Peer not found yet. Broadcasting discovery message.");
            esp_err_t result = esp_now_send(broadcast_mac, (const uint8_t *)message, strlen(message));
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "Broadcasted message: %s", message);
            } else {
                ESP_LOGE(TAG, "Error broadcasting message: %s", esp_err_to_name(result));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}