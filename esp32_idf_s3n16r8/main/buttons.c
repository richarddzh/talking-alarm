#include "buttons.h"
#include "app_config.h"

#include <driver/gpio.h>
#include <esp_timer.h>

typedef struct {
    int      pin;
    bool     stable_pressed;
    bool     last_reading_pressed;
    int64_t  last_change_us;
} btn_state_t;

static btn_state_t s_buttons[APP_BTN_COUNT];

static const int s_button_pins[APP_BTN_COUNT] = {
    [APP_BTN_MAIN] = APP_BUTTON_PIN,
    [APP_BTN_B] = APP_BUTTON_B_PIN,
    [APP_BTN_X] = APP_BUTTON_X_PIN,
    [APP_BTN_Y] = APP_BUTTON_Y_PIN,
};

static bool read_pressed(int pin) {
    return gpio_get_level((gpio_num_t)pin) == 0;
}

void buttons_init(void) {
    uint64_t pin_mask = 0;
    for (int i = 0; i < APP_BTN_COUNT; ++i) {
        pin_mask |= 1ULL << s_button_pins[i];
    }
    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < APP_BTN_COUNT; ++i) {
        btn_state_t *b = &s_buttons[i];
        b->pin = s_button_pins[i];
        b->stable_pressed = read_pressed(b->pin);
        b->last_reading_pressed = b->stable_pressed;
        b->last_change_us = now;
    }
}

bool buttons_poll(app_btn_event_t *ev) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < APP_BTN_COUNT; ++i) {
        btn_state_t *b = &s_buttons[i];
        bool reading = read_pressed(b->pin);
        if (reading != b->last_reading_pressed) {
            b->last_reading_pressed = reading;
            b->last_change_us = now;
        }
        int64_t since_ms = (now - b->last_change_us) / 1000;
        if (since_ms < APP_BUTTON_DEBOUNCE_MS ||
            reading == b->stable_pressed) {
            continue;
        }
        b->stable_pressed = reading;
        ev->button = (app_btn_id_t)i;
        ev->pressed = reading;
        return true;
    }
    return false;
}

bool buttons_pressed(void) {
    return buttons_is_pressed(APP_BTN_MAIN);
}

bool buttons_is_pressed(app_btn_id_t button) {
    return button >= 0 && button < APP_BTN_COUNT &&
           s_buttons[button].stable_pressed;
}

const char *buttons_name(app_btn_id_t button) {
    static const char *const names[APP_BTN_COUNT] = {
        [APP_BTN_MAIN] = "Button",
        [APP_BTN_B] = "B",
        [APP_BTN_X] = "X",
        [APP_BTN_Y] = "Y",
    };
    return button >= 0 && button < APP_BTN_COUNT ? names[button] : "?";
}
