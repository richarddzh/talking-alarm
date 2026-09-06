#pragma once

#include <stdbool.h>
#include <esp_err.h>

typedef struct {
    bool ok;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    char date[16];
    char time[16];
    char err[48];
} rtc_ds3231_time_t;

esp_err_t rtc_ds3231_init(void);
bool      rtc_ds3231_ready(void);
rtc_ds3231_time_t rtc_ds3231_read(void);
esp_err_t rtc_ds3231_set_from_strings(const char *date, const char *time);
