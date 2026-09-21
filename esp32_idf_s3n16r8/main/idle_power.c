#include "idle_power.h"

idle_input_action_t idle_power_filter_input(idle_power_state_t state,
                                             bool event_ready, bool held,
                                             bool *consume_wake_input) {
    if (state != IDLE_POWER_AWAKE && (event_ready || held)) {
        *consume_wake_input = true;
        return IDLE_INPUT_WAKE;
    }
    if (*consume_wake_input) {
        if (!event_ready && !held) *consume_wake_input = false;
        return IDLE_INPUT_IGNORE;
    }
    return event_ready ? IDLE_INPUT_DISPATCH : IDLE_INPUT_IGNORE;
}

idle_power_state_t idle_power_target(idle_power_state_t current,
                                     int64_t idle_ms,
                                     uint32_t sleep_minutes,
                                     uint32_t screen_off_minutes) {
    if (current == IDLE_POWER_SLEEP) return current;
    if (sleep_minutes && idle_ms >= (int64_t)sleep_minutes * 60000) {
        return IDLE_POWER_SLEEP;
    }
    if (screen_off_minutes && idle_ms >= (int64_t)screen_off_minutes * 60000) {
        return IDLE_POWER_SCREEN_OFF;
    }
    return current;
}
