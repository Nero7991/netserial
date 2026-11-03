// netserial basic example: start capture before Wi-Fi, join the network, log a counter every second.
// On the host: python3 host/netserial.py <device-ip> --log run.log
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netserial.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "example";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(netserial_start(NULL));   // capture from here on, before the network is up
    ESP_LOGI(TAG, "netserial started");

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    for (uint32_t i = 0;; i++) {
        uint32_t used, dropped;
        netserial_stats(&used, &dropped);
        ESP_LOGI(TAG, "count %lu (ring used %lu, dropped %lu)", (unsigned long)i, (unsigned long)used,
                 (unsigned long)dropped);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
