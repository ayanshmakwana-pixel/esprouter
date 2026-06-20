#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/err.h"
#include "router_config.h"

static const char *TAG = "block_detector";

#define CHECK_TARGET_HOST "8.8.8.8"
#define CHECK_TARGET_PORT 53
#define CHECK_INTERVAL_SLOW_S 300
#define CHECK_INTERVAL_FAST_S 10
#define MAX_FAILURES 3
#define AP_CONNECT_WAIT_S 15
#define NO_CONNECT_TIMEOUT_S 30

extern volatile bool ap_connect;
#if !CONFIG_ETH_UPLINK
extern char* ssid;
#endif

static bool try_connect(void) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGW(TAG, "Failed to set socket recv timeout");
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGW(TAG, "Failed to set socket send timeout");
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CHECK_TARGET_PORT),
    };
    if (inet_pton(AF_INET, CHECK_TARGET_HOST, &addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid address: %s", CHECK_TARGET_HOST);
        closesocket(sock);
        return false;
    }

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    closesocket(sock);

    if (ret != 0) {
        ESP_LOGW(TAG, "Connectivity check to %s:%d FAILED (errno %d)", CHECK_TARGET_HOST, CHECK_TARGET_PORT, errno);
        return false;
    }
    ESP_LOGI(TAG, "Connectivity check to %s:%d OK", CHECK_TARGET_HOST, CHECK_TARGET_PORT);
    return true;
}

static void gen_random_mac(uint8_t *mac_out) {
    uint8_t base_mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(base_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read efuse MAC (%s), using fallback OUI", esp_err_to_name(err));
        base_mac[0] = 0x00;
        base_mac[1] = 0x4B;
        base_mac[2] = 0x12;
    }
    mac_out[0] = base_mac[0];
    mac_out[1] = base_mac[1];
    mac_out[2] = base_mac[2];
    for (int i = 3; i < 6; i++) {
        mac_out[i] = (uint8_t)(esp_random() & 0xFF);
    }
}

static void save_mac_to_nvs(uint8_t *new_mac) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(nvs, "mac", new_mac, 6);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_blob(mac) failed: %s", esp_err_to_name(err)); nvs_close(nvs); return; }
    err = nvs_set_i32(nvs, "mac_locked", 1);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set_i32(mac_locked) failed: %s", esp_err_to_name(err)); nvs_close(nvs); return; }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "Saved new STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             new_mac[0], new_mac[1], new_mac[2], new_mac[3], new_mac[4], new_mac[5]);
}

static void rotate_mac_and_restart(void) {
    ESP_LOGW(TAG, "Blocked detected! Rotating MAC and restarting...");

    uint8_t new_mac[6];
    gen_random_mac(new_mac);
    save_mac_to_nvs(new_mac);

    esp_restart();
}

void block_detector_task(void *pvParameter) {
    ESP_LOGI(TAG, "Block detector started (%ds init wait, %ds no-connect timeout)",
             AP_CONNECT_WAIT_S, NO_CONNECT_TIMEOUT_S);

    vTaskDelay(pdMS_TO_TICKS(AP_CONNECT_WAIT_S * 1000));

    int tcp_failures = 0;
    TickType_t last_connected_tick = 0;

    while (1) {
        if (ap_connect) {
            last_connected_tick = xTaskGetTickCount();

            if (try_connect()) {
                if (tcp_failures > 0) {
                    ESP_LOGI(TAG, "Connectivity restored after %d failures", tcp_failures);
                }
                tcp_failures = 0;
                vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_SLOW_S * 1000));
            } else {
                tcp_failures++;
                ESP_LOGW(TAG, "Connectivity failure %d/%d", tcp_failures, MAX_FAILURES);
                if (tcp_failures >= MAX_FAILURES) {
                    rotate_mac_and_restart();
                }
                vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_FAST_S * 1000));
            }
        } else {
#if !CONFIG_ETH_UPLINK
            if (ssid == NULL || ssid[0] == '\0') {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
#endif

            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_s = (now - last_connected_tick) / configTICK_RATE_HZ;

            if (elapsed_s >= NO_CONNECT_TIMEOUT_S) {
                ESP_LOGW(TAG, "No connection for %ds — assuming blocked, rotating", elapsed_s);
                rotate_mac_and_restart();
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void start_block_detector(void) {
    BaseType_t ret = xTaskCreate(block_detector_task, "block_detector", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create block_detector task (ret=%d)", (int)ret);
    } else {
        ESP_LOGI(TAG, "Block detector task created");
    }
}
