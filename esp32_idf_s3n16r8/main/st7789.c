#include "st7789.h"
#include "app_config.h"

#include <string.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "st7789";
static spi_device_handle_t s_spi;
static DMA_ATTR uint8_t s_line[APP_TFT_W * 2];

static esp_err_t transmit(bool data, const void *buffer, size_t size) {
    gpio_set_level(APP_TFT_DC_PIN, data ? 1 : 0);
    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = buffer,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t command(uint8_t value) {
    return transmit(false, &value, 1);
}

static esp_err_t command_data(uint8_t cmd, const void *data, size_t size) {
    esp_err_t err = command(cmd);
    return err == ESP_OK ? transmit(true, data, size) : err;
}

static esp_err_t set_window(int x, int y, int width, int height) {
    uint16_t x0 = x + APP_TFT_X_OFFSET;
    uint16_t y0 = y + APP_TFT_Y_OFFSET;
    uint16_t x1 = x0 + width - 1;
    uint16_t y1 = y0 + height - 1;
    uint8_t columns[] = {x0 >> 8, x0, x1 >> 8, x1};
    uint8_t rows[] = {y0 >> 8, y0, y1 >> 8, y1};
    esp_err_t err = command_data(0x2A, columns, sizeof(columns));
    if (err != ESP_OK) return err;
    err = command_data(0x2B, rows, sizeof(rows));
    if (err != ESP_OK) return err;
    return command(0x2C);
}

esp_err_t st7789_init(void) {
    gpio_config_t dc_config = {
        .pin_bit_mask = 1ULL << APP_TFT_DC_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&dc_config);
    if (err != ESP_OK) return err;

    spi_bus_config_t bus_config = {
        .mosi_io_num = APP_TFT_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = APP_TFT_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = APP_TFT_W * 2,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = APP_TFT_SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = APP_TFT_CS_PIN,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &device_config, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device: %s", esp_err_to_name(err));
        return err;
    }

    err = command(0x01);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));

    const uint8_t color_mode = 0x55;
    const uint8_t landscape = 0x60;
    err = command_data(0x3A, &color_mode, 1);
    if (err != ESP_OK) return err;
    err = command_data(0x36, &landscape, 1);
    if (err != ESP_OK) return err;
    err = command(0x11);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    err = command(0x29);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "ready %dx%d SCLK=%d MOSI=%d DC=%d CS=%d",
             APP_TFT_W, APP_TFT_H, APP_TFT_SCLK_PIN, APP_TFT_MOSI_PIN,
             APP_TFT_DC_PIN, APP_TFT_CS_PIN);
    return ESP_OK;
}

esp_err_t st7789_show_indexed4(const uint8_t *framebuffer,
                               int width, int height,
                               const uint16_t palette[16]) {
    if (!framebuffer || !palette ||
        width != APP_TFT_W || height != APP_TFT_H) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = set_window(0, 0, APP_TFT_W, APP_TFT_H);
    if (err != ESP_OK) return err;

    for (int y = 0; y < APP_TFT_H; ++y) {
        const uint8_t *source = framebuffer + y * (APP_TFT_W / 2);
        for (int x = 0; x < APP_TFT_W; ++x) {
            uint8_t packed = source[x >> 1];
            uint8_t index = (x & 1) ? (packed & 0x0F) : (packed >> 4);
            uint16_t color = palette[index];
            s_line[x * 2] = color >> 8;
            s_line[x * 2 + 1] = color;
        }
        err = transmit(true, s_line, sizeof(s_line));
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
