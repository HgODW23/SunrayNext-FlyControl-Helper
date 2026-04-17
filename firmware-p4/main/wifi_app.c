#include "wifi_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_app";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECT_FAIL_BIT BIT1
#define WIFI_CONNECT_CANCEL_BIT BIT2
#define WIFI_APP_CONNECT_DEFAULT_TIMEOUT_MS 25000

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static int s_retry_num = 0;
static bool s_started = false;
static bool s_connecting = false;
static esp_netif_ip_info_t s_ip_info = {0};
static bool s_got_ip = false;

#define WIFI_APP_MAC_READY_RETRY 10
#define WIFI_APP_MAC_READY_DELAY_MS 150

static wifi_app_security_t authmode_to_security(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return WIFI_APP_SECURITY_OPEN;
        case WIFI_AUTH_WEP:
            return WIFI_APP_SECURITY_WEP;
        case WIFI_AUTH_WPA_PSK:
            return WIFI_APP_SECURITY_WPA;
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WPA2_WPA3_PSK:
        case WIFI_AUTH_WPA3_ENT_192:
            return WIFI_APP_SECURITY_WPA2;
        case WIFI_AUTH_WPA3_PSK:
#ifdef WIFI_AUTH_WAPI_PSK
        case WIFI_AUTH_WAPI_PSK:
#endif
            return WIFI_APP_SECURITY_WPA3;
        default:
            return WIFI_APP_SECURITY_UNKNOWN;
    }
}

const char *wifi_app_security_to_string(wifi_app_security_t security) {
    switch (security) {
        case WIFI_APP_SECURITY_OPEN:
            return "OPEN";
        case WIFI_APP_SECURITY_WEP:
            return "WEP";
        case WIFI_APP_SECURITY_WPA:
            return "WPA";
        case WIFI_APP_SECURITY_WPA2:
            return "WPA2";
        case WIFI_APP_SECURITY_WPA3:
            return "WPA3";
        default:
            return "UNKNOWN";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        if (!s_connecting) {
            return;
        }

        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG,
                     "Wi-Fi disconnected, retry in %ds (%d/%d)",
                     WIFI_RETRY_DELAY_MS / 1000,
                     s_retry_num,
                     WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi retries exhausted");
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECT_FAIL_BIT);
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_retry_num = 0;
        ESP_LOGI(TAG, "Wi-Fi associated, waiting DHCP");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_ip_info = event->ip_info;
        s_got_ip = true;
        s_retry_num = 0;
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_app_ensure_started(void) {
    uint8_t mac[6] = {0};

    // If already started, still make sure remote MAC path is ready.
    if (s_started) {
        for (int i = 0; i < WIFI_APP_MAC_READY_RETRY; i++) {
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_APP_MAC_READY_DELAY_MS));
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_started = true;
    } else if (err == ESP_ERR_WIFI_CONN) {
        // Sometimes returned when already started by lower stack.
        s_started = true;
        err = ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    // On hosted Wi-Fi, MAC fetch may be briefly unavailable right after start.
    for (int i = 0; i < WIFI_APP_MAC_READY_RETRY; i++) {
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(WIFI_APP_MAC_READY_DELAY_MS));
    }

    ESP_LOGW(TAG, "Wi-Fi started but remote MAC not ready yet");
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_app_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create station netif");
        return ESP_FAIL;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_err_t country_err = esp_wifi_set_country(&country);
    if (country_err != ESP_OK) {
        ESP_LOGW(TAG, "set_country(CN) failed: %s", esp_err_to_name(country_err));
    }

    ESP_LOGI(TAG, "Wi-Fi initialized (STA)");
    return ESP_OK;
}

esp_err_t wifi_app_scan(wifi_app_scan_result_t *results, size_t max_results, size_t *out_count) {
    if (results == NULL || out_count == NULL || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(wifi_app_ensure_started(), TAG, "Failed to start Wi-Fi");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = ESP_OK;
    uint16_t ap_count = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "scan_get_ap_num failed");
        if (ap_count > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    uint16_t fetch_count = ap_count;
    if (fetch_count > max_results) {
        fetch_count = (uint16_t)max_results;
    }
    if (fetch_count == 0) {
        *out_count = 0;
        return ESP_OK;
    }

    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)calloc(fetch_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch_count, ap_records);
    if (err != ESP_OK) {
        free(ap_records);
        return err;
    }

    size_t out = 0;
    for (uint16_t i = 0; i < fetch_count; i++) {
        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }
        memset(&results[out], 0, sizeof(results[out]));
        strncpy(results[out].ssid, (const char *)ap_records[i].ssid, sizeof(results[out].ssid) - 1);
        results[out].rssi = ap_records[i].rssi;
        results[out].channel = ap_records[i].primary;
        memcpy(results[out].bssid, ap_records[i].bssid, sizeof(results[out].bssid));
        results[out].security = authmode_to_security(ap_records[i].authmode);
        out++;
    }

    free(ap_records);
    *out_count = out;
    return ESP_OK;
}

esp_err_t wifi_app_connect(const wifi_app_connect_params_t *params) {
    if (params == NULL || params->ssid == NULL || params->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(wifi_app_ensure_started(), TAG, "Failed to start Wi-Fi");

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, params->ssid, sizeof(cfg.sta.ssid) - 1);

    const char *password = (params->password != NULL) ? params->password : "";
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);

    if (password[0] == '\0') {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }

    if (params->use_bssid) {
        if (params->bssid == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, params->bssid, 6);
    }

    esp_wifi_disconnect();

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config failed");

    s_connecting = true;
    s_retry_num = 0;
    s_got_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    xEventGroupClearBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_CONNECT_CANCEL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi_connect failed");

    uint32_t timeout_ms = params->timeout_ms > 0 ? params->timeout_ms : WIFI_APP_CONNECT_DEFAULT_TIMEOUT_MS;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_CONNECT_CANCEL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    s_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & WIFI_CONNECT_FAIL_BIT) {
        return ESP_FAIL;
    }
    if (bits & WIFI_CONNECT_CANCEL_BIT) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "connect timeout waiting DHCP (%lu ms)", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_app_cancel_connect(void) {
    if (!s_connecting) {
        return ESP_ERR_INVALID_STATE;
    }

    s_connecting = false;
    s_got_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    if (s_wifi_event_group != NULL) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECT_CANCEL_BIT);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }
    return err;
}

esp_err_t wifi_app_connect_with_creds(const char *ssid, const char *password) {
    wifi_app_connect_params_t params = {
        .ssid = ssid,
        .password = password,
        .bssid = NULL,
        .use_bssid = false,
        .timeout_ms = WIFI_APP_CONNECT_DEFAULT_TIMEOUT_MS,
    };
    return wifi_app_connect(&params);
}

esp_err_t wifi_app_disconnect(void) {
    s_connecting = false;
    s_got_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_CONNECT_CANCEL_BIT);
    }
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }
    return err;
}

esp_err_t wifi_app_reconnect(void) {
    ESP_RETURN_ON_ERROR(wifi_app_ensure_started(), TAG, "Failed to start Wi-Fi");

    s_connecting = true;
    s_retry_num = 0;
    s_got_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    xEventGroupClearBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_CONNECT_CANCEL_BIT);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connecting = false;
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT | WIFI_CONNECT_CANCEL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_APP_CONNECT_DEFAULT_TIMEOUT_MS));
    s_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_CONNECT_FAIL_BIT) {
        return ESP_FAIL;
    }
    if (bits & WIFI_CONNECT_CANCEL_BIT) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_app_get_status(wifi_app_status_t *out_status) {
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_status, 0, sizeof(*out_status));

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) {
        out_status->mode = mode;
    }

    wifi_ap_record_t ap_info = {0};
    err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK) {
        out_status->connected = true;
        strncpy(out_status->ssid, (const char *)ap_info.ssid, sizeof(out_status->ssid) - 1);
        out_status->rssi = ap_info.rssi;
        memcpy(out_status->bssid, ap_info.bssid, sizeof(out_status->bssid));
    }

    out_status->started = s_started;
    out_status->got_ip = s_got_ip;
    out_status->ip_info = s_ip_info;

    esp_wifi_get_mac(WIFI_IF_STA, out_status->mac);

    return ESP_OK;
}

esp_err_t wifi_app_get_mac(uint8_t out_mac[6]) {
    if (out_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_wifi_get_mac(WIFI_IF_STA, out_mac);
}

esp_err_t wifi_app_set_mac_temporary(const uint8_t mac[6]) {
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_connected = wifi_app_is_connected();
    if (was_connected) {
        esp_wifi_disconnect();
    }

    ESP_RETURN_ON_ERROR(wifi_app_ensure_started(), TAG, "Failed to start Wi-Fi");
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        return err;
    }

    if (was_connected) {
        return wifi_app_reconnect();
    }

    return ESP_OK;
}

bool wifi_app_get_ip_info(esp_netif_ip_info_t *out_ip) {
    if (out_ip == NULL) {
        return false;
    }
    if (!s_got_ip) {
        return false;
    }
    *out_ip = s_ip_info;
    return true;
}

void wifi_app_deinit(void) {
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);

    s_started = false;
    s_connecting = false;
    s_got_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
}

bool wifi_app_is_connected(void) {
    return s_got_ip;
}
