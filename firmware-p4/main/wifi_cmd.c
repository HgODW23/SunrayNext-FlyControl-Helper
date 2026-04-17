#include "wifi_cmd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_app.h"

static const char *TAG = "wifi_cmd";

#define WIFI_CLI_STORE_NS "wifi_cli"
#define WIFI_CONNECT_TIMEOUT_MS 25000
#define WIFI_CLI_SCAN_MAX WIFI_APP_MAX_SCAN_RESULTS
#define WIFI_CLI_PROMPT "esp32p4> "

typedef enum {
    SCAN_SORT_RSSI = 0,
    SCAN_SORT_SSID,
} scan_sort_t;

typedef struct {
    char ssid[33];
    char password[65];
    bool use_bssid;
    uint8_t bssid[6];
} saved_wifi_config_t;

typedef struct {
    char ssid[33];
    char password[65];
    bool use_bssid;
    uint8_t bssid[6];
    uint32_t timeout_ms;
} wifi_connect_job_t;

static TaskHandle_t s_connect_task = NULL;
static SemaphoreHandle_t s_connect_lock = NULL;

static void print_wifi_help(void) {
    printf("\n");
    printf("wifi scan  [-s|--sort <rssi|ssid>]\n");
    printf("  Scan visible APs and show SSID, RSSI, security, channel, BSSID\n");
    printf("Default sort: RSSI desc; optional: --sort ssid\n\n");

    printf("wifi connect  <ssid> [<password>] [-b|--bssid <mac>]\n");
    printf("  Start async connect task to target Wi-Fi\n");
    printf("Without -b, same SSID APs can be selected interactively\n\n");

    printf("wifi cancel\n");
    printf("  Cancel the current running connect task\n\n");

    printf("wifi disconnect\n");
    printf("  Disconnect current Wi-Fi link\n\n");

    printf("wifi status\n");
    printf("  Show mode, link state, DHCP/IP, SSID/RSSI, MAC, and connect pending\n\n");

    printf("wifi reconnect\n");
    printf("  Start async reconnect using last successful profile from NVS\n\n");

    printf("wifi mac  [<mac>]\n");
    printf("  Show current MAC; with arg apply temporary MAC spoofing until reboot\n\n");

    printf("wifi help\n");
    printf("  Show this help\n\n");
}

static void print_cli_prompt(void) {
    printf("%s", WIFI_CLI_PROMPT);
    fflush(stdout);
}

static esp_err_t ensure_connect_lock(void) {
    if (s_connect_lock != NULL) {
        return ESP_OK;
    }
    s_connect_lock = xSemaphoreCreateMutex();
    return s_connect_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool is_connect_running(void) {
    if (ensure_connect_lock() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_connect_lock, portMAX_DELAY);
    bool running = (s_connect_task != NULL);
    xSemaphoreGive(s_connect_lock);
    return running;
}

static void set_connect_task(TaskHandle_t task) {
    if (ensure_connect_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_connect_lock, portMAX_DELAY);
    s_connect_task = task;
    xSemaphoreGive(s_connect_lock);
}

static void clear_connect_task(void) {
    if (ensure_connect_lock() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_connect_lock, portMAX_DELAY);
    s_connect_task = NULL;
    xSemaphoreGive(s_connect_lock);
}

static void mac_to_string(const uint8_t mac[6], char *out, size_t out_size) {
    snprintf(out,
             out_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static bool parse_mac(const char *input, uint8_t out[6]) {
    if (input == NULL || out == NULL) {
        return false;
    }
    unsigned int values[6] = {0};
    if (sscanf(input,
               "%2x:%2x:%2x:%2x:%2x:%2x",
               &values[0],
               &values[1],
               &values[2],
               &values[3],
               &values[4],
               &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)values[i];
    }
    return true;
}

static int compare_scan_rssi(const void *a, const void *b) {
    const wifi_app_scan_result_t *lhs = (const wifi_app_scan_result_t *)a;
    const wifi_app_scan_result_t *rhs = (const wifi_app_scan_result_t *)b;

    if (lhs->rssi != rhs->rssi) {
        return (rhs->rssi - lhs->rssi);
    }
    return strcmp(lhs->ssid, rhs->ssid);
}

static int compare_scan_ssid(const void *a, const void *b) {
    const wifi_app_scan_result_t *lhs = (const wifi_app_scan_result_t *)a;
    const wifi_app_scan_result_t *rhs = (const wifi_app_scan_result_t *)b;

    int ssid_cmp = strcmp(lhs->ssid, rhs->ssid);
    if (ssid_cmp != 0) {
        return ssid_cmp;
    }
    return (rhs->rssi - lhs->rssi);
}

static void print_scan_results(const wifi_app_scan_result_t *results, size_t count) {
    printf("%-4s %-32s %-6s %-8s %-5s %-17s\n", "No.", "SSID", "RSSI", "SEC", "CH", "BSSID");
    for (size_t i = 0; i < count; i++) {
        char bssid_str[18] = {0};
        mac_to_string(results[i].bssid, bssid_str, sizeof(bssid_str));
        printf("%-4u %-32s %-6d %-8s %-5u %-17s\n",
               (unsigned int)(i + 1),
               results[i].ssid,
               results[i].rssi,
               wifi_app_security_to_string(results[i].security),
               results[i].channel,
               bssid_str);
    }
}

static esp_err_t save_last_config(const saved_wifi_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_CLI_STORE_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "ssid", cfg->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "use_bssid", cfg->use_bssid ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "bssid", cfg->bssid, sizeof(cfg->bssid));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "valid", 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t load_last_config(saved_wifi_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_CLI_STORE_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(nvs, "valid", &valid);
    if (err != ESP_OK || valid != 1) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    size_t ssid_len = sizeof(cfg->ssid);
    err = nvs_get_str(nvs, "ssid", cfg->ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    size_t pass_len = sizeof(cfg->password);
    err = nvs_get_str(nvs, "pass", cfg->password, &pass_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    uint8_t use_bssid = 0;
    err = nvs_get_u8(nvs, "use_bssid", &use_bssid);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    cfg->use_bssid = (use_bssid != 0);

    size_t bssid_len = sizeof(cfg->bssid);
    err = nvs_get_blob(nvs, "bssid", cfg->bssid, &bssid_len);
    nvs_close(nvs);
    if (err != ESP_OK || bssid_len != sizeof(cfg->bssid)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void connect_task_entry(void *arg) {
    wifi_connect_job_t *job = (wifi_connect_job_t *)arg;
    if (job == NULL) {
        clear_connect_task();
        vTaskDelete(NULL);
        return;
    }

    wifi_app_connect_params_t params = {
        .ssid = job->ssid,
        .password = job->password,
        .bssid = job->use_bssid ? job->bssid : NULL,
        .use_bssid = job->use_bssid,
        .timeout_ms = job->timeout_ms,
    };

    ESP_LOGI(TAG, "Connecting to SSID: %s", job->ssid);
    esp_err_t err = wifi_app_connect(&params);
    if (err == ESP_OK) {
        saved_wifi_config_t saved = {0};
        strncpy(saved.ssid, job->ssid, sizeof(saved.ssid) - 1);
        strncpy(saved.password, job->password, sizeof(saved.password) - 1);
        saved.use_bssid = job->use_bssid;
        if (job->use_bssid) {
            memcpy(saved.bssid, job->bssid, sizeof(saved.bssid));
        }

        esp_err_t save_err = save_last_config(&saved);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Connected but failed to persist config: %s", esp_err_to_name(save_err));
        }
        ESP_LOGI(TAG, "Connected and DHCP ready");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "connect canceled");
    } else {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
    }

    free(job);
    clear_connect_task();
    print_cli_prompt();
    vTaskDelete(NULL);
}

static int start_connect_task(const wifi_connect_job_t *job_template) {
    if (job_template == NULL) {
        return 1;
    }
    if (is_connect_running()) {
        ESP_LOGW(TAG, "A connect task is already running; use `wifi cancel` first");
        return 1;
    }

    wifi_connect_job_t *job = (wifi_connect_job_t *)calloc(1, sizeof(wifi_connect_job_t));
    if (job == NULL) {
        ESP_LOGE(TAG, "No memory for connect job");
        return 1;
    }
    *job = *job_template;

    TaskHandle_t task = NULL;
    BaseType_t rc = xTaskCreate(connect_task_entry, "wifi_connect", 6144, job, 5, &task);
    if (rc != pdPASS || task == NULL) {
        free(job);
        ESP_LOGE(TAG, "Failed to start connect task");
        return 1;
    }

    set_connect_task(task);
    ESP_LOGI(TAG, "Connect task started. Use `wifi cancel` to interrupt.");
    return 0;
}

static int cmd_wifi_scan(int argc, char **argv) {
    scan_sort_t sort = SCAN_SORT_RSSI;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sort") == 0) && (i + 1 < argc)) {
            const char *mode = argv[i + 1];
            if (strcmp(mode, "rssi") == 0) {
                sort = SCAN_SORT_RSSI;
            } else if (strcmp(mode, "ssid") == 0) {
                sort = SCAN_SORT_SSID;
            } else {
                ESP_LOGE(TAG, "Invalid sort mode: %s", mode);
                return 1;
            }
            i++;
            continue;
        }

        ESP_LOGE(TAG, "Usage error, run: wifi help");
        return 1;
    }

    wifi_app_scan_result_t results[WIFI_CLI_SCAN_MAX] = {0};
    size_t count = 0;
    esp_err_t err = wifi_app_scan(results, WIFI_CLI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return 1;
    }

    if (sort == SCAN_SORT_SSID) {
        qsort(results, count, sizeof(results[0]), compare_scan_ssid);
    } else {
        qsort(results, count, sizeof(results[0]), compare_scan_rssi);
    }

    printf("Found %u AP(s)\n", (unsigned int)count);
    if (count > 0) {
        print_scan_results(results, count);
    }
    return 0;
}

static int select_bssid_interactively(const char *ssid, uint8_t out_bssid[6]) {
    wifi_app_scan_result_t results[WIFI_CLI_SCAN_MAX] = {0};
    size_t count = 0;
    if (wifi_app_scan(results, WIFI_CLI_SCAN_MAX, &count) != ESP_OK || count == 0) {
        return 0;
    }

    wifi_app_scan_result_t matches[WIFI_CLI_SCAN_MAX] = {0};
    size_t matches_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(results[i].ssid, ssid) == 0) {
            matches[matches_count++] = results[i];
        }
    }

    if (matches_count <= 1) {
        if (matches_count == 1) {
            memcpy(out_bssid, matches[0].bssid, 6);
            return 1;
        }
        return 0;
    }

    qsort(matches, matches_count, sizeof(matches[0]), compare_scan_rssi);

    printf("SSID '%s' has %u APs, please select index:\n", ssid, (unsigned int)matches_count);
    print_scan_results(matches, matches_count);
    printf("Index (1-%u, 0 cancel): ", (unsigned int)matches_count);
    fflush(stdout);

    char line[16] = {0};
    if (fgets(line, sizeof(line), stdin) == NULL) {
        ESP_LOGE(TAG, "No interactive input available, please retry with -b <mac>");
        return -1;
    }

    int idx = atoi(line);
    if (idx <= 0 || idx > (int)matches_count) {
        ESP_LOGW(TAG, "Canceled");
        return -1;
    }

    memcpy(out_bssid, matches[idx - 1].bssid, 6);
    return 1;
}

static int cmd_wifi_connect(int argc, char **argv) {
    if (argc < 3) {
        ESP_LOGE(TAG, "Usage error, run: wifi help");
        return 1;
    }

    const char *ssid = argv[2];
    const char *password = "";
    uint8_t bssid[6] = {0};
    bool use_bssid = false;

    int i = 3;
    if (i < argc && argv[i][0] != '-') {
        password = argv[i];
        i++;
    }

    for (; i < argc; i++) {
        if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bssid") == 0) && (i + 1 < argc)) {
            if (!parse_mac(argv[i + 1], bssid)) {
                ESP_LOGE(TAG, "Invalid BSSID: %s", argv[i + 1]);
                return 1;
            }
            use_bssid = true;
            i++;
            continue;
        }

        ESP_LOGE(TAG, "Usage error, run: wifi help");
        return 1;
    }

    if (!use_bssid) {
        int sel = select_bssid_interactively(ssid, bssid);
        if (sel < 0) {
            return 1;
        }
        if (sel > 0) {
            use_bssid = true;
        }
    }

    wifi_connect_job_t job = {0};
    strncpy(job.ssid, ssid, sizeof(job.ssid) - 1);
    strncpy(job.password, password, sizeof(job.password) - 1);
    job.use_bssid = use_bssid;
    if (use_bssid) {
        memcpy(job.bssid, bssid, sizeof(job.bssid));
    }
    job.timeout_ms = WIFI_CONNECT_TIMEOUT_MS;

    return start_connect_task(&job);
}

static int cmd_wifi_cancel(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!is_connect_running()) {
        ESP_LOGW(TAG, "No running connect task");
        return 1;
    }

    esp_err_t err = wifi_app_cancel_connect();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "cancel failed: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_LOGI(TAG, "Cancel requested");
    return 0;
}

static int cmd_wifi_disconnect(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (is_connect_running()) {
        esp_err_t cancel_err = wifi_app_cancel_connect();
        if (cancel_err != ESP_OK && cancel_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "cancel connect before disconnect failed: %s", esp_err_to_name(cancel_err));
        }
    }

    esp_err_t err = wifi_app_disconnect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disconnect failed: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_LOGI(TAG, "Disconnected");
    return 0;
}

static const char *mode_to_string(wifi_mode_t mode) {
    switch (mode) {
        case WIFI_MODE_STA:
            return "STA";
        case WIFI_MODE_AP:
            return "AP";
        case WIFI_MODE_APSTA:
            return "APSTA";
        default:
            return "UNKNOWN";
    }
}

static int cmd_wifi_status(int argc, char **argv) {
    (void)argc;
    (void)argv;

    wifi_app_status_t status = {0};
    esp_err_t err = wifi_app_get_status(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status failed: %s", esp_err_to_name(err));
        return 1;
    }

    char mac_str[18] = {0};
    mac_to_string(status.mac, mac_str, sizeof(mac_str));

    printf("Mode: %s\n", mode_to_string(status.mode));
    printf("Connected: %s\n", status.connected ? "yes" : "no");
    printf("Connect Pending: %s\n", is_connect_running() ? "yes" : "no");
    printf("DHCP Ready: %s\n", status.got_ip ? "yes" : "no");
    printf("MAC: %s\n", mac_str);

    if (status.connected) {
        char bssid_str[18] = {0};
        mac_to_string(status.bssid, bssid_str, sizeof(bssid_str));
        printf("SSID: %s\n", status.ssid);
        printf("RSSI: %d dBm\n", status.rssi);
        printf("BSSID: %s\n", bssid_str);
    }

    if (status.got_ip) {
        printf("IP: " IPSTR "\n", IP2STR(&status.ip_info.ip));
        printf("Mask: " IPSTR "\n", IP2STR(&status.ip_info.netmask));
        printf("Gateway: " IPSTR "\n", IP2STR(&status.ip_info.gw));
    }

    return 0;
}

static int cmd_wifi_reconnect(int argc, char **argv) {
    (void)argc;
    (void)argv;

    saved_wifi_config_t saved = {0};
    esp_err_t err = load_last_config(&saved);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No saved Wi-Fi profile (%s)", esp_err_to_name(err));
        return 1;
    }

    wifi_connect_job_t job = {0};
    strncpy(job.ssid, saved.ssid, sizeof(job.ssid) - 1);
    strncpy(job.password, saved.password, sizeof(job.password) - 1);
    job.use_bssid = saved.use_bssid;
    if (saved.use_bssid) {
        memcpy(job.bssid, saved.bssid, sizeof(job.bssid));
    }
    job.timeout_ms = WIFI_CONNECT_TIMEOUT_MS;

    return start_connect_task(&job);
}

static int cmd_wifi_mac(int argc, char **argv) {
    uint8_t mac[6] = {0};

    if (argc == 2) {
        esp_err_t err = wifi_app_get_mac(mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "get mac failed: %s", esp_err_to_name(err));
            return 1;
        }

        char mac_str[18] = {0};
        mac_to_string(mac, mac_str, sizeof(mac_str));
        printf("MAC: %s\n", mac_str);
        return 0;
    }

    if (argc == 3) {
        if (!parse_mac(argv[2], mac)) {
            ESP_LOGE(TAG, "Invalid MAC format, use XX:XX:XX:XX:XX:XX");
            return 1;
        }

        esp_err_t err = wifi_app_set_mac_temporary(mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set mac failed: %s", esp_err_to_name(err));
            return 1;
        }

        char mac_str[18] = {0};
        mac_to_string(mac, mac_str, sizeof(mac_str));
        printf("Temporary MAC applied: %s\n", mac_str);
        printf("Note: this spoofing only lasts until reboot.\n");
        return 0;
    }

    ESP_LOGE(TAG, "Usage error, run: wifi help");
    return 1;
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 2) {
        print_wifi_help();
        return 0;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "help") == 0) {
        print_wifi_help();
        return 0;
    }
    if (strcmp(sub, "scan") == 0) {
        return cmd_wifi_scan(argc, argv);
    }
    if (strcmp(sub, "connect") == 0) {
        return cmd_wifi_connect(argc, argv);
    }
    if (strcmp(sub, "cancel") == 0) {
        return cmd_wifi_cancel(argc, argv);
    }
    if (strcmp(sub, "disconnect") == 0) {
        return cmd_wifi_disconnect(argc, argv);
    }
    if (strcmp(sub, "status") == 0) {
        return cmd_wifi_status(argc, argv);
    }
    if (strcmp(sub, "reconnect") == 0) {
        return cmd_wifi_reconnect(argc, argv);
    }
    if (strcmp(sub, "mac") == 0) {
        return cmd_wifi_mac(argc, argv);
    }

    ESP_LOGE(TAG, "Unknown subcommand: %s, run: wifi help", sub);
    return 1;
}

esp_err_t wifi_cmd_register(void) {
    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "Wi-Fi command entry (run `wifi help` for details)",
        .hint = NULL,
        .func = &cmd_wifi,
    };
    return esp_console_cmd_register(&cmd);
}
