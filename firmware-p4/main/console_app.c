#include <stdio.h>
#include <string.h>

#include "bridge_cmd.h"
#include "console_app.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include "network/command/network_command.h"
#include "wifi_cmd.h"

static const char *TAG = "console_app";
static esp_console_repl_t *s_repl = NULL;

static int cmd_echo(int argc, char **argv) {
    if (argc < 2) {
        ESP_LOGI(TAG, "(empty)");
        return 0;
    }

    printf("\n>>> ");
    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], i < argc - 1 ? " " : "");
    }
    printf(" <<<\n\n");
    return 0;
}

static const esp_console_cmd_t s_commands[] = {
    {
        .command = "echo",
        .help = "Echo input text\n  Usage: echo <text>",
        .hint = NULL,
        .func = &cmd_echo,
    },
};

static bool starts_with(const char *text, const char *prefix) {
    if (text == NULL || prefix == NULL) {
        return false;
    }
    size_t plen = strlen(prefix);
    return strncmp(text, prefix, plen) == 0;
}

static void wifi_add_subcmd_completion(const char *buf, linenoiseCompletions *lc) {
    static const char *subcmds[] = {
        "scan",
        "connect",
        "cancel",
        "disconnect",
        "status",
        "reconnect",
        "mac",
        "help",
    };

    const char *base = "wifi ";
    if (!starts_with(buf, base)) {
        return;
    }

    const char *partial = buf + strlen(base);
    for (size_t i = 0; i < sizeof(subcmds) / sizeof(subcmds[0]); i++) {
        if (starts_with(subcmds[i], partial)) {
            char candidate[32] = {0};
            snprintf(candidate, sizeof(candidate), "wifi %s", subcmds[i]);
            linenoiseAddCompletion(lc, candidate);
        }
    }
}

static void repl_completion(const char *buf, linenoiseCompletions *lc) {
    wifi_add_subcmd_completion(buf, lc);
}

int console_app_init(void) {
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32p4> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &s_repl));

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_commands[i]));
    }

    ESP_ERROR_CHECK(network_cmd_register());
    ESP_ERROR_CHECK(bridge_cmd_register());
    ESP_ERROR_CHECK(wifi_cmd_register());
    linenoiseSetCompletionCallback(repl_completion);

    ESP_LOGI(TAG,
             "Console REPL initialized, total commands: %d",
             (int)(sizeof(s_commands) / sizeof(s_commands[0])) + 10 + 4 + 1);

    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
    return 0;
}
