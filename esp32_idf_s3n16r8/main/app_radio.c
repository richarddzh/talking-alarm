#include "app_radio.h"

#include "radio_player.h"
#include "radio_stations.h"
#include "ui_screen.h"

#include <stdio.h>

#define RADIO_ROW_Y 48
#define RADIO_ROW_H 28

static uint32_t s_seen_generation;
static size_t s_selected_station;

static const char *state_label(radio_player_state_t state) {
    switch (state) {
    case RADIO_PLAYER_CONNECTING: return "连接";
    case RADIO_PLAYER_BUFFERING: return "缓冲";
    case RADIO_PLAYER_PLAYING: return "播放";
    case RADIO_PLAYER_STOPPING: return "停止";
    case RADIO_PLAYER_ERROR: return "错误";
    case RADIO_PLAYER_IDLE:
    default: return "空闲";
    }
}

static void enter(void) {
    radio_player_snapshot_t snapshot;
    radio_player_get_snapshot(&snapshot);
    s_seen_generation = snapshot.generation;
    if (snapshot.station_index < radio_station_count()) {
        s_selected_station = snapshot.station_index;
    }
}

static void exit_app(void) {
    radio_player_stop(4000);
}

static gui_action_t tick(int64_t now_ms) {
    (void)now_ms;
    radio_player_snapshot_t snapshot;
    radio_player_get_snapshot(&snapshot);
    if (snapshot.generation != s_seen_generation) {
        s_seen_generation = snapshot.generation;
        return GUI_ACTION_REDRAW;
    }
    return GUI_ACTION_NONE;
}

static void render(const gui_model_t *model, int focused_item) {
    (void)model;
    radio_player_snapshot_t snapshot;
    radio_player_get_snapshot(&snapshot);

    ui_draw_text(14, 8, "网络电台", 3, UI_COLOR_TEXT);
    ui_draw_text(174, 13, state_label(snapshot.state), 2,
                 snapshot.state == RADIO_PLAYER_ERROR
                     ? UI_COLOR_DANGER
                     : snapshot.state == RADIO_PLAYER_PLAYING
                           ? UI_COLOR_SUCCESS
                           : UI_COLOR_CYAN);
    ui_draw_text(226, 13, snapshot.message, 1, UI_COLOR_MUTED);

    for (size_t i = 0; i < radio_station_count(); ++i) {
        const radio_station_t *station = radio_station_get(i);
        int y = RADIO_ROW_Y + (int)i * RADIO_ROW_H;
        bool focused = focused_item == (int)i;
        bool active = radio_player_busy() &&
                      snapshot.station_index == i;
        ui_fill_round_rect(14, y, 292, RADIO_ROW_H - 3, 5,
                           focused ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL);
        ui_draw_round_rect(14, y, 292, RADIO_ROW_H - 3, 5,
                           focused ? 3 : 1,
                           focused ? UI_COLOR_FOCUS : UI_COLOR_PANEL_ALT);
        ui_fill_circle(28, y + 12, 5,
                       active ? UI_COLOR_SUCCESS : UI_COLOR_MUTED);
        ui_draw_text(42, y + 5, station->name, 2,
                     focused ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
        ui_draw_text(270, y + 7, station->region, 1, UI_COLOR_MUTED);
    }

    char format[48];
    if (snapshot.sample_rate) {
        snprintf(format, sizeof(format), "%u Hz  %u ch  %u kbps",
                 (unsigned)snapshot.sample_rate,
                 (unsigned)snapshot.channels,
                 (unsigned)(snapshot.bitrate / 1000));
    } else {
        snprintf(format, sizeof(format), "确认键播放，再按停止");
    }
    ui_draw_text(16, 192, format, 1, UI_COLOR_MUTED);
}

static gui_action_t activate(uint8_t item) {
    if (item >= radio_station_count()) return GUI_ACTION_NONE;
    s_selected_station = item;
    return GUI_ACTION_RADIO_TOGGLE;
}

static const gui_focus_node_t s_focus_grid[] = {
    {.left = GUI_FOCUS_NONE, .right = GUI_FOCUS_NONE,
     .up = GUI_FOCUS_HOME, .down = 1},
    {.left = GUI_FOCUS_NONE, .right = GUI_FOCUS_NONE,
     .up = 0, .down = 2},
    {.left = GUI_FOCUS_NONE, .right = GUI_FOCUS_NONE,
     .up = 1, .down = 3},
    {.left = GUI_FOCUS_NONE, .right = GUI_FOCUS_NONE,
     .up = 2, .down = 4},
    {.left = GUI_FOCUS_NONE, .right = GUI_FOCUS_NONE,
     .up = 3, .down = GUI_FOCUS_HOME},
};

static const gui_app_t s_app = {
    .id = "radio",
    .label = "网络电台",
    .icon = GUI_ICON_RADIO,
    .focus_count = 5,
    .focus_grid = s_focus_grid,
    .enter = enter,
    .exit = exit_app,
    .tick = tick,
    .render = render,
    .activate = activate,
    .handle_input = NULL,
};

const gui_app_t *app_radio_descriptor(void) {
    return &s_app;
}

size_t app_radio_selected_station(void) {
    return s_selected_station;
}
