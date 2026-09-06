#pragma once

#include <stdbool.h>

typedef enum {
    APP_INPUT_MAIN_PRESSED = 0,
    APP_INPUT_MAIN_RELEASED,
    APP_INPUT_JOYSTICK_PRESSED,
    APP_INPUT_LEFT,
    APP_INPUT_RIGHT,
    APP_INPUT_UP,
    APP_INPUT_DOWN,
} app_input_event_t;

void buttons_init(void);

// Emits debounced digital-button edges and rate-limited analog-axis
// direction events. Axis movement never emits a pressed event.
bool buttons_poll(app_input_event_t *event);

bool buttons_pressed(void);
const char *buttons_event_name(app_input_event_t event);
