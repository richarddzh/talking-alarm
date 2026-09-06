#include "wifi_creds.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_spiffs.h>

static const char *TAG = "wifi_creds";

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' ||
                     s[n-1] == ' ' || s[n-1] == '\t')) {
        s[--n] = 0;
    }
}

bool wifi_creds_valid(const wifi_creds_t *c) {
    return c && c->ssid[0] && c->password[0];
}

esp_err_t wifi_creds_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = APP_SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount: %s", esp_err_to_name(e));
    }
    return e;
}

int wifi_creds_load(wifi_creds_t *out) {
    if (!out) return -2;
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(APP_WIFI_CONFIG_PATH, "r");
    if (!f) return -1;
    if (!fgets(out->ssid, sizeof(out->ssid), f)) { fclose(f); return -2; }
    if (!fgets(out->password, sizeof(out->password), f)) { fclose(f); return -2; }
    fclose(f);
    rstrip(out->ssid);
    rstrip(out->password);
    return wifi_creds_valid(out) ? 0 : -2;
}

int wifi_creds_save(const wifi_creds_t *in) {
    if (!wifi_creds_valid(in)) return -2;
    FILE *f = fopen(APP_WIFI_CONFIG_PATH, "w");
    if (!f) return -2;
    fprintf(f, "%s\n%s\n", in->ssid, in->password);
    fclose(f);
    return 0;
}
