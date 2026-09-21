#include "power_settings.h"
#include "app_config.h"

#include <errno.h>
#include <stdio.h>
#include <esp_log.h>

#define SETTINGS_PATH APP_SPIFFS_BASE_PATH "/power_settings.txt"
#define TEMP_PATH APP_SPIFFS_BASE_PATH "/power_settings.tmp"
#define BACKUP_PATH APP_SPIFFS_BASE_PATH "/power_settings.bak"

static const char *TAG = "power_settings";
static power_settings_t s_settings;

bool power_settings_valid(const power_settings_t *settings) {
    if (!settings) return false;
    uint32_t sleep = settings->sleep_minutes;
    uint32_t screen = settings->screen_off_minutes;
    return (sleep == 0 || sleep == 5 || sleep == 10 || sleep == 30) &&
           (screen == 0 || screen == 1 || screen == 5 || screen == 10);
}

static esp_err_t read_settings(const char *path, power_settings_t *out) {
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    unsigned sleep, screen;
    char extra;
    int fields = fscanf(file, "power_v1 %u %u %c", &sleep, &screen, &extra);
    bool ok = !ferror(file) && fields == 2;
    if (fclose(file) != 0) ok = false;
    if (!ok) return ESP_FAIL;
    *out = (power_settings_t){sleep, screen};
    return power_settings_valid(out) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t power_settings_load(void) {
    power_settings_t settings;
    esp_err_t primary = read_settings(SETTINGS_PATH, &settings);
    if (primary == ESP_OK) {
        s_settings = settings;
        return ESP_OK;
    }
    esp_err_t backup = read_settings(BACKUP_PATH, &settings);
    if (backup == ESP_OK) {
        ESP_LOGW(TAG, "recovering backup settings");
        // Restore the valid copy before allowing the next replacement.
        if (remove(SETTINGS_PATH) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "cannot remove invalid settings: %d", errno);
            return ESP_FAIL;
        }
        if (rename(BACKUP_PATH, SETTINGS_PATH) != 0) {
            ESP_LOGE(TAG, "cannot restore settings: %d", errno);
            return ESP_FAIL;
        }
        s_settings = settings;
        return ESP_OK;
    }
    s_settings = (power_settings_t){0};
    if (primary == ESP_ERR_NOT_FOUND && backup == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "invalid/unreadable settings; auto power saving disabled");
    return ESP_FAIL;
}

const power_settings_t *power_settings_get(void) {
    return &s_settings;
}

esp_err_t power_settings_save(const power_settings_t *settings) {
    if (!power_settings_valid(settings)) {
        ESP_LOGE(TAG, "invalid timeout selection");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(TEMP_PATH, "w");
    if (!file) {
        ESP_LOGE(TAG, "cannot open temporary settings: %d", errno);
        return ESP_FAIL;
    }
    bool ok = fprintf(file, "power_v1 %u %u\n",
                      (unsigned)settings->sleep_minutes,
                      (unsigned)settings->screen_off_minutes) > 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) goto failed;
    if (remove(BACKUP_PATH) != 0 && errno != ENOENT) goto failed;
    if (rename(SETTINGS_PATH, BACKUP_PATH) != 0 && errno != ENOENT) goto failed;
    if (rename(TEMP_PATH, SETTINGS_PATH) != 0) {
        if (rename(BACKUP_PATH, SETTINGS_PATH) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "settings rollback failed: %d", errno);
        }
        goto failed;
    }
    s_settings = *settings;
    if (remove(BACKUP_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "cannot remove old settings backup: %d", errno);
    }
    return ESP_OK;

failed:
    ESP_LOGE(TAG, "cannot save settings: %d", errno);
    if (remove(TEMP_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "cannot remove temporary settings: %d", errno);
    }
    return ESP_FAIL;
}
