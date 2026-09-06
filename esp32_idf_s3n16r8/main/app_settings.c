#include "app_settings.h"

#include "ui_screen.h"

static void draw_row(int y, const char *label, const char *value,
                     bool focused, ui_color_t value_color) {
    ui_fill_rect(18, y, 284, 42,
                 focused ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL);
    ui_draw_rect(18, y, 284, 42, focused ? 3 : 1,
                 focused ? UI_COLOR_FOCUS : UI_COLOR_PANEL_ALT);
    ui_draw_text(30, y + 12, label, 2,
                 focused ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
    int value_x = 288 - ui_text_width(value, 2);
    ui_draw_text(value_x, y + 12, value, 2, value_color);
}

static void render(const gui_model_t *model, int focused_item) {
    ui_draw_text(18, 12, "设置", 4, UI_COLOR_TEXT);
    ui_draw_text(20, 50, "设备控制", 2, UI_COLOR_MUTED);

    draw_row(70, "安静模式", model->quiet_mode ? "开启" : "关闭",
             focused_item == 0,
             model->quiet_mode ? UI_COLOR_WARNING : UI_COLOR_SUCCESS);
    draw_row(116, "网络配置", model->setup_mode ? "配网中" : "关闭",
             focused_item == 1,
             model->setup_mode ? UI_COLOR_WARNING : UI_COLOR_MUTED);
    draw_row(162, "时间同步", "执行",
             focused_item == 2, UI_COLOR_CYAN);
}

static gui_action_t activate(uint8_t item) {
    switch (item) {
    case 0: return GUI_ACTION_TOGGLE_QUIET;
    case 1: return GUI_ACTION_TOGGLE_SETUP;
    case 2: return GUI_ACTION_SYNC_TIME;
    default: return GUI_ACTION_NONE;
    }
}

static const gui_app_t s_app = {
    .id = "settings",
    .label = "设置",
    .icon = GUI_ICON_SETTINGS,
    .focus_count = 3,
    .enter = NULL,
    .exit = NULL,
    .tick = NULL,
    .render = render,
    .activate = activate,
    .handle_input = NULL,
};

const gui_app_t *app_settings_descriptor(void) {
    return &s_app;
}
