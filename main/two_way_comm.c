/**
 * two_way_comm.c
 * 
 * A program for the ESP32 that uses the ESP-NOW protocol for bidirectional communication between two ESP32-S3 devices.
 * This program compiles using the ESP-IDF library and tools. 
 * 
 * Author: Philip Giacalone
 * Date: 2024-09-15 
 * Modified: [Current Date]
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_timer.h"

static const char *TAG = "ESP-NOW COMM";

#define CHANNEL 1
#define PEER_TIMEOUT_MS 10000       // 10 seconds
#define DISCOVERY_INTERVAL_MS 5000  // 5 seconds
#define MAX_RETRIES 5               // Maximum number of retries
#define RETRY_DELAY_MS 13          // Delay between retries in milliseconds
#define TRANSMIT_DELAY_MS 1000     // Delay between message transmissions in milliseconds

static uint8_t peer_mac[ESP_NOW_ETH_ALEN] = {0};
static bool peer_found = false;
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static int64_t last_peer_time = 0;
static char peer_mac_str[18] = {0};  // Store formatted MAC string
static uint8_t my_mac_address[6];    // Global declaration of my_mac_address

static SemaphoreHandle_t send_semaphore;
static volatile bool send_success;

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
    send_success = (status == ESP_NOW_SEND_SUCCESS);
    xSemaphoreGive(send_semaphore);
}

static void on_data_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)
{
    if (memcmp(esp_now_info->src_addr, broadcast_mac, ESP_NOW_ETH_ALEN) != 0) {
        if (!peer_found || memcmp(esp_now_info->src_addr, peer_mac, ESP_NOW_ETH_ALEN) != 0) {
            if (peer_found) {
                ESP_LOGI(TAG, "New peer found. Replacing old peer.");
                esp_now_del_peer(peer_mac);
            }
            memcpy(peer_mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
            peer_found = true;
            snprintf(peer_mac_str, sizeof(peer_mac_str), MACSTR, MAC2STR(peer_mac));
            ESP_LOGI(TAG, "****************");
            ESP_LOGI(TAG, "PEER FOUND! MAC: %s", peer_mac_str);
            ESP_LOGI(TAG, "****************");

            esp_now_peer_info_t peer_info = {
                .channel = CHANNEL,
                .encrypt = false,
            };
            memcpy(peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
            ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
        }
        last_peer_time = esp_timer_get_time() / 1000; // Update last seen time
    }

    ESP_LOGI(TAG, "-->Received: %.*s (from: %s, len: %d)", data_len, (const char*)data, peer_mac_str, data_len);

    // Simple command handling
    if (strncmp((char*)data, "CMD:", 4) == 0) {
        ESP_LOGI(TAG, "Command received: %.*s", data_len - 4, data + 4);
        // Handle commands here
    }
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

static bool send_with_retry(const uint8_t *mac_addr, const uint8_t *data, int len) {
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        esp_err_t result = esp_now_send(mac_addr, data, len);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue message (Attempt %d): %s", retry + 1, esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        if (xSemaphoreTake(send_semaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (send_success) {
                ESP_LOGI(TAG, "<--Sending: %.*s (Attempt %d)", len, (const char*)data, retry + 1);
                return true;
            } else {
                ESP_LOGW(TAG, "Delivery failed (Attempt %d)", retry + 1);
            }
        } else {
            ESP_LOGW(TAG, "Timeout waiting for send callback (Attempt %d)", retry + 1);
        }

        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
    return false;  // All retries failed
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

    send_semaphore = xSemaphoreCreateBinary();
    if (send_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    esp_read_mac(my_mac_address, ESP_MAC_WIFI_STA);
    char my_mac_str[18];
    snprintf(my_mac_str, sizeof(my_mac_str), MACSTR, MAC2STR(my_mac_address));
    ESP_LOGI(TAG, "-----------------------------------------------");
    ESP_LOGI(TAG, "My MAC Address: %s", my_mac_str);
    ESP_LOGI(TAG, "-----------------------------------------------");

    char message[64];
    int sequence_number = 0;
    int64_t last_discovery_time = 0;

    while (1) {
        int64_t current_time = esp_timer_get_time() / 1000;

        if (peer_found && (current_time - last_peer_time > PEER_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Peer timed out. Removing peer.");
            esp_now_del_peer(peer_mac);
            peer_found = false;
            memset(peer_mac_str, 0, sizeof(peer_mac_str));
        }

        snprintf(message, sizeof(message), "%02X%02X_%d", my_mac_address[4], my_mac_address[5], sequence_number++);
        
        if (peer_found) {
            if (!send_with_retry(peer_mac, (const uint8_t *)message, strlen(message))) {
                ESP_LOGE(TAG, "Failed to send message after %d attempts. Removing peer.", MAX_RETRIES);
                esp_now_del_peer(peer_mac);
                peer_found = false;
                memset(peer_mac_str, 0, sizeof(peer_mac_str));
            }
        }

        // Send discovery message periodically
        if (!peer_found || (current_time - last_discovery_time > DISCOVERY_INTERVAL_MS)) {
            ESP_LOGI(TAG, "Broadcasting discovery message.");
            if (send_with_retry(broadcast_mac, (const uint8_t *)message, strlen(message))) {
                last_discovery_time = current_time;
            } else {
                ESP_LOGE(TAG, "Failed to broadcast discovery message after %d attempts.", MAX_RETRIES);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TRANSMIT_DELAY_MS)); // Check more frequently
    }
}