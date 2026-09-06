#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

// Mounts SPIFFS at /spiffs (formatting on mount failure, like the
// Arduino version's SPIFFS.begin(true)).
esp_err_t wifi_creds_init(void);

// Returns 0 if creds loaded, -1 if file missing, -2 on parse/IO error.
int  wifi_creds_load(wifi_creds_t *out);

// Persists credentials. Returns 0 on success.
int  wifi_creds_save(const wifi_creds_t *in);

bool wifi_creds_valid(const wifi_creds_t *c);
