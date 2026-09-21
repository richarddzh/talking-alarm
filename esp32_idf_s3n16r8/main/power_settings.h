#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

typedef struct {
    uint32_t sleep_minutes;
    uint32_t screen_off_minutes;
} power_settings_t;

// Call after SPIFFS is mounted. Missing files use "never" for both timers.
esp_err_t power_settings_load(void);
const power_settings_t *power_settings_get(void);
bool power_settings_valid(const power_settings_t *settings);
esp_err_t power_settings_save(const power_settings_t *settings);
