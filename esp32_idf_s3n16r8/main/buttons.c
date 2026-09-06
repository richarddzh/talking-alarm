#include "buttons.h"

#include "app_config.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>

#define AXIS_CALIBRATION_SAMPLES 32
#define AXIS_PRESS_DELTA 850
#define AXIS_RELEASE_DELTA 500
#define AXIS_FIRST_REPEAT_MS 350
#define AXIS_REPEAT_MS 140
#define JOYSTICK_BUTTON_DEBOUNCE_MS 60
#define JOYSTICK_NEUTRAL_GUARD_MS 100

typedef struct {
    int pin;
    bool stable_pressed;
    bool last_reading_pressed;
    int64_t last_change_us;
} digital_button_t;

typedef struct {
    adc_unit_t unit;
    adc_channel_t channel;
    int center;
    int last_raw;
    int active_direction;
    int64_t next_repeat_ms;
} analog_axis_t;

static const char *TAG = "joystick";
static digital_button_t s_main;
static digital_button_t s_joystick_button;
static analog_axis_t s_axis_x;
static analog_axis_t s_axis_y;
static adc_oneshot_unit_handle_t s_adc1;
static adc_oneshot_unit_handle_t s_adc2;
static int64_t s_axes_neutral_since_ms;

static bool read_pressed(int pin) {
    return gpio_get_level((gpio_num_t)pin) == 0;
}

static adc_oneshot_unit_handle_t unit_handle(adc_unit_t unit) {
    return unit == ADC_UNIT_1 ? s_adc1 : s_adc2;
}

static void init_adc_unit(adc_unit_t unit) {
    adc_oneshot_unit_handle_t *handle =
        unit == ADC_UNIT_1 ? &s_adc1 : &s_adc2;
    if (*handle) return;
    adc_oneshot_unit_init_cfg_t config = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&config, handle));
}

static void init_axis(analog_axis_t *axis, int pin) {
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(pin, &axis->unit,
                                               &axis->channel));
    init_adc_unit(axis->unit);
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(unit_handle(axis->unit),
                                               axis->channel, &config));
    int total = 0;
    for (int i = 0; i < AXIS_CALIBRATION_SAMPLES; ++i) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(unit_handle(axis->unit),
                                         axis->channel, &raw));
        total += raw;
    }
    axis->center = total / AXIS_CALIBRATION_SAMPLES;
    axis->last_raw = axis->center;
    axis->active_direction = 0;
    axis->next_repeat_ms = 0;
}

static void init_digital_button(digital_button_t *button, int pin,
                                int64_t now_us) {
    button->pin = pin;
    button->stable_pressed = read_pressed(pin);
    button->last_reading_pressed = button->stable_pressed;
    button->last_change_us = now_us;
}

void buttons_init(void) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << APP_BUTTON_PIN) |
                        (1ULL << APP_JOYSTICK_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    int64_t now_us = esp_timer_get_time();
    init_digital_button(&s_main, APP_BUTTON_PIN, now_us);
    init_digital_button(&s_joystick_button, APP_JOYSTICK_BUTTON_PIN, now_us);
    init_axis(&s_axis_x, APP_JOYSTICK_X_PIN);
    init_axis(&s_axis_y, APP_JOYSTICK_Y_PIN);
    s_axes_neutral_since_ms = now_us / 1000;
    ESP_LOGI(TAG, "ADC centers x=%d y=%d", s_axis_x.center, s_axis_y.center);
}

static bool poll_digital(digital_button_t *button,
                         app_input_event_t pressed_event,
                         app_input_event_t released_event,
                         int debounce_ms,
                         app_input_event_t *event) {
    int64_t now_us = esp_timer_get_time();
    bool reading = read_pressed(button->pin);
    if (reading != button->last_reading_pressed) {
        button->last_reading_pressed = reading;
        button->last_change_us = now_us;
    }
    if ((now_us - button->last_change_us) / 1000 < debounce_ms ||
        reading == button->stable_pressed) {
        return false;
    }
    button->stable_pressed = reading;
    if (!reading && released_event < 0) return false;
    *event = reading ? pressed_event : released_event;
    return true;
}

static bool poll_axis(analog_axis_t *axis, bool inverted,
                      app_input_event_t negative_event,
                      app_input_event_t positive_event,
                      app_input_event_t *event) {
    int raw = 0;
    if (adc_oneshot_read(unit_handle(axis->unit), axis->channel, &raw) !=
        ESP_OK) {
        return false;
    }
    axis->last_raw = raw;
    int delta = raw - axis->center;
    if (inverted) delta = -delta;
    int64_t now = esp_timer_get_time() / 1000;

    if (axis->active_direction != 0) {
        if (delta > -AXIS_RELEASE_DELTA && delta < AXIS_RELEASE_DELTA) {
            axis->active_direction = 0;
            return false;
        }
        if (now < axis->next_repeat_ms) return false;
        axis->next_repeat_ms = now + AXIS_REPEAT_MS;
        *event = axis->active_direction < 0 ? negative_event : positive_event;
        return true;
    }

    if (delta <= -AXIS_PRESS_DELTA || delta >= AXIS_PRESS_DELTA) {
        axis->active_direction = delta < 0 ? -1 : 1;
        axis->next_repeat_ms = now + AXIS_FIRST_REPEAT_MS;
        *event = delta < 0 ? negative_event : positive_event;
        return true;
    }
    return false;
}

static bool axis_is_neutral(const analog_axis_t *axis) {
    int delta = axis->last_raw - axis->center;
    return delta > -AXIS_RELEASE_DELTA && delta < AXIS_RELEASE_DELTA;
}

static bool poll_joystick_button(bool axes_neutral,
                                 app_input_event_t *event) {
    int64_t now_us = esp_timer_get_time();
    bool reading = read_pressed(s_joystick_button.pin);
    if (reading != s_joystick_button.last_reading_pressed) {
        s_joystick_button.last_reading_pressed = reading;
        s_joystick_button.last_change_us = now_us;
    }
    if ((now_us - s_joystick_button.last_change_us) / 1000 <
            JOYSTICK_BUTTON_DEBOUNCE_MS ||
        reading == s_joystick_button.stable_pressed) {
        return false;
    }

    s_joystick_button.stable_pressed = reading;
    if (!reading) return false;

    int64_t now_ms = now_us / 1000;
    bool neutral_stable =
        axes_neutral && s_axes_neutral_since_ms > 0 &&
        now_ms - s_axes_neutral_since_ms >= JOYSTICK_NEUTRAL_GUARD_MS;
    if (!neutral_stable) {
        ESP_LOGW(TAG,
                 "suppressed B while axes active x=%d/%d y=%d/%d",
                 s_axis_x.last_raw, s_axis_x.center,
                 s_axis_y.last_raw, s_axis_y.center);
        return false;
    }

    *event = APP_INPUT_JOYSTICK_PRESSED;
    return true;
}

bool buttons_poll(app_input_event_t *event) {
    if (!event) return false;
    if (poll_digital(&s_main, APP_INPUT_MAIN_PRESSED,
                     APP_INPUT_MAIN_RELEASED,
                     APP_BUTTON_DEBOUNCE_MS, event)) {
        return true;
    }

    app_input_event_t x_event = APP_INPUT_LEFT;
    app_input_event_t y_event = APP_INPUT_UP;
    bool x_ready = poll_axis(&s_axis_x, APP_JOYSTICK_X_INVERTED,
                             APP_INPUT_LEFT, APP_INPUT_RIGHT, &x_event);
    bool y_ready = poll_axis(&s_axis_y, APP_JOYSTICK_Y_INVERTED,
                             APP_INPUT_UP, APP_INPUT_DOWN, &y_event);

    bool axes_neutral =
        axis_is_neutral(&s_axis_x) && axis_is_neutral(&s_axis_y);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (axes_neutral) {
        if (s_axes_neutral_since_ms == 0) s_axes_neutral_since_ms = now_ms;
    } else {
        s_axes_neutral_since_ms = 0;
    }

    app_input_event_t button_event = APP_INPUT_JOYSTICK_PRESSED;
    bool button_ready = poll_joystick_button(axes_neutral, &button_event);

    if (x_ready) {
        *event = x_event;
        return true;
    }
    if (y_ready) {
        *event = y_event;
        return true;
    }
    if (button_ready) {
        *event = button_event;
        return true;
    }
    return false;
}

bool buttons_pressed(void) {
    return s_main.stable_pressed;
}

const char *buttons_event_name(app_input_event_t event) {
    static const char *const names[] = {
        [APP_INPUT_MAIN_PRESSED] = "Main pressed",
        [APP_INPUT_MAIN_RELEASED] = "Main released",
        [APP_INPUT_JOYSTICK_PRESSED] = "Joystick pressed",
        [APP_INPUT_LEFT] = "Joystick left",
        [APP_INPUT_RIGHT] = "Joystick right",
        [APP_INPUT_UP] = "Joystick up",
        [APP_INPUT_DOWN] = "Joystick down",
    };
    return event >= 0 && event <= APP_INPUT_DOWN ? names[event] : "?";
}
