#include "app_pomodoro.h"

#include "ui_screen.h"
#include "small_apps_art.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <stdio.h>

#define MINUTE_MS 60000LL
#define NOTICE_LIFETIME_MS 30000
#define TWO_PI 6.28318530718f

typedef enum {
    PHASE_FOCUS,
    PHASE_SHORT_BREAK,
    PHASE_LONG_BREAK,
} pomodoro_phase_t;

static pomodoro_phase_t s_phase;
static unsigned s_completed;
static bool s_running;
static int64_t s_remaining_ms = 25 * MINUTE_MS;
static int64_t s_deadline_ms;
static int64_t s_notice_at_ms;
static const char *s_notice;
static const char *s_notice_status = "开始后语音提醒";
static int64_t s_last_render_second = -1;

static int64_t clock_ms(void) {
    return esp_timer_get_time() / 1000;
}

static int64_t duration_ms(void) {
    if (s_phase == PHASE_SHORT_BREAK) return 5 * MINUTE_MS;
    if (s_phase == PHASE_LONG_BREAK) return 15 * MINUTE_MS;
    return 25 * MINUTE_MS;
}

static const char *phase_name(void) {
    if (s_phase == PHASE_SHORT_BREAK) return "短休息";
    if (s_phase == PHASE_LONG_BREAK) return "长休息";
    return "专注工作";
}

static void announce(const char *text, int64_t at_ms) {
    s_notice = text;
    s_notice_at_ms = at_ms;
    s_notice_status = "等待语音播报";
    ESP_LOGI("pomodoro", "%s", text);
}

void app_pomodoro_update(int64_t now_ms) {
    if (!s_running || now_ms < s_deadline_ms) return;
    const int64_t cycle_ms = 130 * MINUTE_MS;
    if (now_ms - s_deadline_ms >= cycle_ms) {
        int64_t cycles = (now_ms - s_deadline_ms) / cycle_ms;
        s_completed += (unsigned)(cycles * 4);
        s_deadline_ms += cycles * cycle_ms;
    }
    // At most eight boundaries remain after whole 130-minute cycles.
    while (now_ms >= s_deadline_ms) {
        int64_t boundary_ms = s_deadline_ms;
        if (s_phase == PHASE_FOCUS) {
            ++s_completed;
            s_phase = s_completed % 4 == 0 ? PHASE_LONG_BREAK : PHASE_SHORT_BREAK;
            announce(s_phase == PHASE_LONG_BREAK
                         ? "工作结束啦，开始长休息，放松十五分钟。"
                         : "工作结束啦，开始休息，放松五分钟。",
                     boundary_ms);
        } else {
            s_phase = PHASE_FOCUS;
            announce("休息好啦，开始工作，专注二十五分钟。", boundary_ms);
        }
        s_remaining_ms = duration_ms();
        s_deadline_ms += s_remaining_ms;
    }
}

void app_pomodoro_pause_for_sleep(int64_t now_ms) {
    app_pomodoro_update(now_ms);
    if (s_running) {
        s_remaining_ms = s_deadline_ms - now_ms;
        s_running = false;
        s_notice_status = "休眠暂停，手动继续";
    }
    s_notice = NULL;
}

const char *app_pomodoro_announcement(int64_t now_ms) {
    if (s_notice && now_ms - s_notice_at_ms >= NOTICE_LIFETIME_MS) {
        s_notice = NULL;
        s_notice_status = "播报超时";
        ESP_LOGW("pomodoro", "phase announcement expired");
    }
    return s_notice;
}

void app_pomodoro_announcement_done(const char *status) {
    s_notice = NULL;
    s_notice_status = status;
}

static gui_action_t tick(int64_t now_ms) {
    if (now_ms / 1000 == s_last_render_second) return GUI_ACTION_NONE;
    s_last_render_second = now_ms / 1000;
    return GUI_ACTION_REDRAW;
}

static void centered(int x, int y, const char *text, int size, ui_color_t color) {
    ui_draw_text(x - ui_text_width(text, size) / 2, y, text, size, color);
}

static void draw_hand(int cx, int cy, float turns, int length,
                      ui_color_t color, int width) {
    float angle = turns * TWO_PI;
    int x = cx + (int)(sinf(angle) * length);
    int y = cy - (int)(cosf(angle) * length);
    ui_draw_line(cx, cy, x, y, color);
    if (width > 1) {
        ui_draw_line(cx + 1, cy, x + 1, y, color);
        ui_draw_line(cx, cy + 1, x, y + 1, color);
    }
}

static void draw_clock(const gui_model_t *model) {
    const int cx = 99, cy = 146;
    ui_fill_circle(cx, cy, 37, UI_COLOR_WHITE);
    for (int number = 1; number <= 12; ++number) {
        float angle = number * TWO_PI / 12;
        char label[3];
        snprintf(label, sizeof(label), "%d", number);
        centered(cx + (int)(sinf(angle) * 29),
                 cy - (int)(cosf(angle) * 29) - 3,
                 label, 1, UI_COLOR_PANEL);
    }
    int hour = 0, minute = 0, second = 0;
    if (model->time_valid && model->time &&
        sscanf(model->time, "%d:%d:%d", &hour, &minute, &second) == 3 &&
        hour >= 0 && hour < 24 && minute >= 0 && minute < 60 &&
        second >= 0 && second < 60) {
        draw_hand(cx, cy, ((hour % 12) + minute / 60.0f) / 12, 14, UI_COLOR_PANEL, 2);
        draw_hand(cx, cy, (minute + second / 60.0f) / 60, 23, UI_COLOR_PANEL, 2);
        draw_hand(cx, cy, second / 60.0f, 25, UI_COLOR_DANGER, 1);
        char time[12];
        snprintf(time, sizeof(time), "%02d:%02d:%02d", hour, minute, second);
        centered(84, 191, time, 2, UI_COLOR_TEXT);
    } else {
        centered(cx, cy, "--:--", 1, UI_COLOR_MUTED);
        centered(84, 191, "等待校时", 1, UI_COLOR_MUTED);
    }
    ui_fill_circle(cx, cy, 3, UI_COLOR_DANGER);
}

static esp_err_t render_overlay(void) {
    esp_err_t err = ui_draw_rgb565_sprite(12, 42, TOMATO_CLOCK_W, TOMATO_CLOCK_H,
                                          s_tomato_clock_rgb565,
                                          s_tomato_clock_alpha, false);
    if (err != ESP_OK) return err;
    return ui_draw_rgb565_sprite(278, 35, TOMATO_FACE_W, TOMATO_FACE_H,
                                 s_tomato_face_rgb565, s_tomato_face_alpha, false);
}

static void render(const gui_model_t *model, int focused_item) {
    ui_fill_rect(0, 0, 320, 32, UI_COLOR_NAVY);
    ui_draw_text(12, 8, "番茄时钟", 2, UI_COLOR_TEXT);
    ui_draw_text(216, 11, "25 / 5 / 15", 1, UI_COLOR_MUTED);
    draw_clock(model);
    ui_color_t accent = s_phase == PHASE_FOCUS ? UI_COLOR_WARNING : UI_COLOR_SUCCESS;
    centered(218, 43, phase_name(), 2, accent);
    int64_t remaining = s_running ? s_deadline_ms - clock_ms() : s_remaining_ms;
    if (remaining < 0) remaining = 0;
    int seconds = (int)((remaining + 999) / 1000);
    char countdown[16];
    snprintf(countdown, sizeof(countdown), "%02d:%02d", seconds / 60, seconds % 60);
    centered(237, 76, countdown, 4, UI_COLOR_TEXT);
    centered(237, 111, s_running ? "进行中" : "已暂停", 1, accent);
    int progress = (int)((duration_ms() - remaining) * 142 / duration_ms());
    if (progress < 0) progress = 0;
    if (progress > 142) progress = 142;
    ui_fill_round_rect(166, 130, 142, 6, 3, UI_COLOR_PANEL_ALT);
    if (progress > 0) ui_fill_rect(166, 130, progress, 6, accent);
    char rounds[40];
    snprintf(rounds, sizeof(rounds), "完成 %u 次", s_completed);
    centered(237, 146, rounds, 1, UI_COLOR_MUTED);
    const char *labels[] = {s_running ? "暂停" : "开始", "重置"};
    for (int i = 0; i < 2; ++i) {
        int x = 164 + i * 75;
        bool focus = focused_item == i;
        ui_fill_round_rect(x, 164, 69, 23, 4,
                           focus ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL);
        ui_draw_rect(x, 164, 69, 23, focus ? 2 : 1,
                     focus ? UI_COLOR_FOCUS : UI_COLOR_PANEL_ALT);
        centered(x + 34, 168, labels[i], 1, focus ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
    }
    centered(237, 191, s_notice_status, 1, UI_COLOR_MUTED);
}

static gui_action_t activate(uint8_t item) {
    int64_t now = clock_ms();
    app_pomodoro_update(now);
    if (item == 0) {
        if (s_running) {
            s_remaining_ms = s_deadline_ms - now;
            s_running = false;
            s_notice = NULL;
            s_notice_status = "已暂停";
        } else {
            s_deadline_ms = now + s_remaining_ms;
            s_running = true;
            announce(s_phase == PHASE_FOCUS
                         ? "开始工作啦，专心做好眼前的事情。"
                         : "开始休息啦，放松一下眼睛和肩膀。",
                     now);
        }
    } else if (item == 1) {
        s_running = false;
        s_phase = PHASE_FOCUS;
        s_completed = 0;
        s_remaining_ms = duration_ms();
        s_notice = NULL;
        s_notice_status = "已重置";
    }
    return GUI_ACTION_REDRAW;
}

static const gui_focus_node_t s_focus_grid[] = {
    {.left = GUI_FOCUS_HOME, .right = 1, .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
    {.left = 0, .right = GUI_FOCUS_HOME, .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
};

static const gui_app_t s_app = {
    .id = "pomodoro",
    .label = "番茄时钟",
    .icon = GUI_ICON_POMODORO,
    .focus_count = 2,
    .focus_grid = s_focus_grid,
    .tick = tick,
    .render = render,
    .render_overlay = render_overlay,
    .activate = activate,
};

const gui_app_t *app_pomodoro_descriptor(void) {
    return &s_app;
}
