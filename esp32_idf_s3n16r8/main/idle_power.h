#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    IDLE_POWER_AWAKE = 0,
    IDLE_POWER_SCREEN_OFF,
    IDLE_POWER_SLEEP,
} idle_power_state_t;

typedef enum {
    IDLE_INPUT_IGNORE = 0,
    IDLE_INPUT_DISPATCH,
    IDLE_INPUT_WAKE,
} idle_input_action_t;

idle_input_action_t idle_power_filter_input(idle_power_state_t state,
                                             bool event_ready, bool held,
                                             bool *consume_wake_input);

// Both deadlines are measured from the last physical user activity.
idle_power_state_t idle_power_target(idle_power_state_t current,
                                     int64_t idle_ms,
                                     uint32_t sleep_minutes,
                                     uint32_t screen_off_minutes);
