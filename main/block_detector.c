#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "router_config.h"

static const char *TAG = "block_detector";

#define CHECK_TARGET_HOST "8.8.8.8"
#define CHECK_TARGET_PORT 53
#define CHECK_INTERVAL_SLOW_S 300
#define CHECK_INTERVAL_FAST_S 10
#define MAX_FAILURES 3
#define AP_CONNECT_WAIT_S 15

extern bool ap_connect;

extern uint8_t* mac;
extern char* ssid;
extern char* passwd;

static bool try_connect(void) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CHECK_TARGET_PORT),
    };
    inet_pton(AF_INET, CHECK_TARGET_HOST, &addr.sin_addr);

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
    mac_out[0] = 0x02;
    for (int i = 1; i < 6; i++) {
        mac_out[i] = (uint8_t)(esp_random() & 0xFF);
    }
}

static void save_mac_to_nvs(uint8_t *new_mac) {
    nvs_handle_t nvs;
    if (nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "mac", new_mac, 6);
        nvs_set_i32(nvs, "mac_locked", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGW(TAG, "Saved new STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 new_mac[0], new_mac[1], new_mac[2], new_mac[3], new_mac[4], new_mac[5]);
    }
}

static void rotate_mac_and_restart(void) {
    ESP_LOGW(TAG, "Blocked detected! Rotating MAC and restarting...");

    uint8_t new_mac[6];
    gen_random_mac(new_mac);
    save_mac_to_nvs(new_mac);

    esp_restart();
}

void block_detector_task(void *pvParameter) {
    ESP_LOGI(TAG, "Block detector started (check %s:%d)", CHECK_TARGET_HOST, CHECK_TARGET_PORT);

    vTaskDelay(pdMS_TO_TICKS(AP_CONNECT_WAIT_S * 1000));

    int failures = 0;

    while (1) {
        if (!ap_connect) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (try_connect()) {
            if (failures > 0) {
                ESP_LOGI(TAG, "Connectivity restored after %d failures", failures);
            }
            failures = 0;
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_SLOW_S * 1000));
        } else {
            failures++;
            ESP_LOGW(TAG, "Connectivity failure %d/%d", failures, MAX_FAILURES);
            if (failures >= MAX_FAILURES) {
                rotate_mac_and_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_FAST_S * 1000));
        }
    }
}

void start_block_detector(void) {
    xTaskCreate(block_detector_task, "block_detector", 4096, NULL, 5, NULL);
}
