#include "rtc_ds3231.h"
#include "app_i2c.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <driver/i2c_master.h>
#include <esp_log.h>

static const char *TAG = "rtc_ds3231";

#define DS3231_ADDR         0x68
#define DS3231_REG_TIME     0x00
#define DS3231_REG_STATUS   0x0F
#define DS3231_OSF_BIT      0x80

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static uint8_t to_bcd(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int from_bcd(uint8_t v) {
    return ((v >> 4) * 10) + (v & 0x0F);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t write_regs(uint8_t reg, const uint8_t *buf, size_t len) {
    uint8_t tmp[1 + 7];
    if (len > 7) return ESP_ERR_INVALID_ARG;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 100);
}

static esp_err_t read_status(uint8_t *status) {
    return read_regs(DS3231_REG_STATUS, status, 1);
}

esp_err_t rtc_ds3231_init(void) {
    if (s_ready) return ESP_OK;
    esp_err_t err = app_i2c_init();
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = APP_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(app_i2c_bus(), &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

bool rtc_ds3231_ready(void) {
    return s_ready;
}

rtc_ds3231_time_t rtc_ds3231_read(void) {
    rtc_ds3231_time_t out = {0};
    if (!s_ready) {
        snprintf(out.err, sizeof(out.err), "rtc init");
        return out;
    }

    uint8_t status = 0;
    esp_err_t err = read_status(&status);
    if (err != ESP_OK) {
        snprintf(out.err, sizeof(out.err), "%s", esp_err_to_name(err));
        return out;
    }
    if (status & DS3231_OSF_BIT) {
        snprintf(out.err, sizeof(out.err), "RTC not set");
        return out;
    }

    uint8_t regs[7];
    err = read_regs(DS3231_REG_TIME, regs, sizeof(regs));
    if (err != ESP_OK) {
        snprintf(out.err, sizeof(out.err), "%s", esp_err_to_name(err));
        return out;
    }

    out.second = from_bcd(regs[0] & 0x7F);
    out.minute = from_bcd(regs[1] & 0x7F);
    if (regs[2] & 0x40) {
        int hour = from_bcd(regs[2] & 0x1F);
        bool pm = (regs[2] & 0x20) != 0;
        if (hour == 12) out.hour = pm ? 12 : 0;
        else out.hour = pm ? hour + 12 : hour;
    } else {
        out.hour = from_bcd(regs[2] & 0x3F);
    }
    out.day = from_bcd(regs[4] & 0x3F);
    out.month = from_bcd(regs[5] & 0x1F);
    out.year = 2000 + from_bcd(regs[6]);
    snprintf(out.date, sizeof(out.date), "%04d-%02d-%02d",
             out.year, out.month, out.day);
    snprintf(out.time, sizeof(out.time), "%02d:%02d:%02d",
             out.hour, out.minute, out.second);
    out.ok = true;
    return out;
}

esp_err_t rtc_ds3231_set_from_strings(const char *date, const char *time) {
    if (!s_ready || !date || !time) return ESP_ERR_INVALID_STATE;

    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3) {
        if (sscanf(date, "%d/%d/%d", &month, &day, &year) != 3) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (sscanf(time, "%d:%d:%d", &hour, &minute, &second) != 3) {
        if (sscanf(time, "%d:%d", &hour, &minute) != 2) {
            return ESP_ERR_INVALID_ARG;
        }
        second = 0;
    }
    if (year < 2000 || year > 2099 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t regs[7] = {
        to_bcd(second),
        to_bcd(minute),
        to_bcd(hour),
        1,
        to_bcd(day),
        to_bcd(month),
        to_bcd(year - 2000),
    };
    esp_err_t err = write_regs(DS3231_REG_TIME, regs, sizeof(regs));
    if (err != ESP_OK) return err;

    uint8_t status = 0;
    err = read_status(&status);
    if (err != ESP_OK) return err;
    status &= (uint8_t)~DS3231_OSF_BIT;
    err = write_regs(DS3231_REG_STATUS, &status, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "set time %s %s", date, time);
    }
    return err;
}
