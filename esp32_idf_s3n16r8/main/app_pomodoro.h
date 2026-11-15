#pragma once

#include "gui_app.h"

const gui_app_t *app_pomodoro_descriptor(void);
// Main-loop service: deadlines keep advancing outside the App and screen-off.
void app_pomodoro_update(int64_t now_ms);
void app_pomodoro_pause_for_sleep(int64_t now_ms);
// The newest phase notice replaces an older unspoken notice.
const char *app_pomodoro_announcement(int64_t now_ms);
void app_pomodoro_announcement_done(const char *status);
