#pragma once
#include <stdbool.h>

typedef enum {
    APP_BTN_MAIN = 0,
    APP_BTN_B,
    APP_BTN_X,
    APP_BTN_Y,
    APP_BTN_COUNT,
} app_btn_id_t;

typedef struct {
    app_btn_id_t button;
    bool pressed;
} app_btn_event_t;

void buttons_init(void);

// Poll once. If a debounced state change happened, fills `ev` and
// returns true; otherwise returns false.
bool buttons_poll(app_btn_event_t *ev);

// Compatibility helper for the GPIO4 gesture button.
bool buttons_pressed(void);

bool buttons_is_pressed(app_btn_id_t button);
const char *buttons_name(app_btn_id_t button);
