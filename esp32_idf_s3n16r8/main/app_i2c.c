#include "app_i2c.h"
#include "app_config.h"

#include <esp_log.h>

static const char *TAG = "app_i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t app_i2c_init(void) {
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = APP_I2C_PORT,
        .sda_io_num = APP_I2C_SDA_PIN,
        .scl_io_num = APP_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init: %s", esp_err_to_name(err));
    }
    return err;
}

i2c_master_bus_handle_t app_i2c_bus(void) {
    return s_bus;
}
