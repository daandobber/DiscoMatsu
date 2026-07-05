#include "wifi_setup.h"

#include "bsp/power.h"
#include "esp_log.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static const char *TAG = "wifi_setup";

static bool s_radio_ok = false;

esp_err_t wifi_setup_init(void) {
    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi radio not responding, WiFi not available");
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        return ESP_OK;  // not fatal - Disc-O-Matsu works fine without WiFi
    }
    wifi_connection_init_stack();
    s_radio_ok = true;
    return ESP_OK;
}

bool wifi_setup_connect_blocking(uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!s_radio_ok) return false;

    esp_err_t res = wifi_connect_try_all();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s (no saved networks reachable)", esp_err_to_name(res));
        return false;
    }
    ESP_LOGI(TAG, "WiFi connected");
    return true;
}

void wifi_setup_disconnect(void) {
    if (!s_radio_ok) return;
    wifi_connection_disconnect();
    ESP_LOGI(TAG, "WiFi disconnected");
}

bool wifi_setup_is_connected(void) {
    return s_radio_ok && wifi_connection_is_connected();
}
