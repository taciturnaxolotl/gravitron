#include "camera.h"
#include "uart_bridge.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "main";

// ── Variant selection ──────────────────────────────────────────────
// Set ROBOT_VARIANT via -D flag or change here:
// 0 = ESP32-WROVER (V1): UART on GPIO33/4, LED on GPIO13
// 1 = ESP32-S3 (V2):     UART on GPIO3/40,  LED on GPIO46
#ifndef ROBOT_VARIANT
#define ROBOT_VARIANT 0
#endif

// ── LED config ─────────────────────────────────────────────────────

#if ROBOT_VARIANT == 0
  #define LED_GPIO     GPIO_NUM_13
  #define UART_RX_PIN  GPIO_NUM_33
  #define UART_TX_PIN  GPIO_NUM_4
#else
  #define LED_GPIO     GPIO_NUM_46
  #define UART_RX_PIN  GPIO_NUM_3
  #define UART_TX_PIN  GPIO_NUM_40
#endif

static const char *VARIANT_NAME = (ROBOT_VARIANT == 0) ? "WROVER" : "S3";

// ── WiFi ───────────────────────────────────────────────────────────

#define WIFI_AP_SSID_PREFIX  "ELEGOO-"
#define WIFI_MAX_STA_CONN    4

static char g_ssid[32];

static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_ssid, sizeof(g_ssid), "%s%02X%02X%02X%02X%02X%02X",
             WIFI_AP_SSID_PREFIX,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0, // null-terminated
            .max_connection = WIFI_MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(wifi_config.ap.ssid, g_ssid, strlen(g_ssid));
    wifi_config.ap.ssid[strlen(g_ssid)] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(78)); // 19.5dBm * 4

    // Set static IP 192.168.4.1
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")));

    ESP_LOGI(TAG, "WiFi AP: %s (no password)", g_ssid);
    ESP_LOGI(TAG, "IP: 192.168.4.1");
    ESP_LOGI(TAG, "Variant: %s", VARIANT_NAME);
}

// ── Status LED task ────────────────────────────────────────────────

static void led_task(void *arg)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    while (1) {
        // Blink while no WebSocket client, solid when connected
        extern int websocket_client_count(void);
        if (websocket_client_count() > 0) {
            gpio_set_level(LED_GPIO, 1);
        } else {
            gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ── Main ───────────────────────────────────────────────────────────

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Camera
    ESP_ERROR_CHECK(camera_init());

    // UART bridge
    uart_bridge_init(UART_RX_PIN, UART_TX_PIN, 9600);

    // WiFi AP
    wifi_init_ap();

    // HTTP + WebSocket server
    httpd_handle_t server = http_server_start();
    if (!server) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Status LED
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "Robot Board firmware ready");
    ESP_LOGI(TAG, "Connect to WiFi: %s", g_ssid);
    ESP_LOGI(TAG, "Open http://192.168.4.1/");
    ESP_LOGI(TAG, "WebSocket at ws://192.168.4.1/ws");
}
