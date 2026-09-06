#include "app_chat_alarm.h"

#include "app_config.h"
#include "ui_screen.h"
#include "voice_chat.h"

#include <freertos/FreeRTOS.h>
#include <string.h>

#define CHAT_HISTORY_CAPACITY 6
#define CHAT_TEXT_BYTES 192
#define CHAT_VISIBLE_MESSAGES 3

typedef struct {
    bool from_user;
    char text[CHAT_TEXT_BYTES];
} chat_message_t;

static chat_message_t s_history[CHAT_HISTORY_CAPACITY];
static size_t s_history_count;
static portMUX_TYPE s_history_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_recording;
static int64_t s_recording_started_ms;

static void copy_utf8(char *dst, size_t capacity, const char *src) {
    if (!src || capacity == 0) return;
    size_t length = strlen(src);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((uint8_t)src[length] & 0xC0) == 0x80) {
            --length;
        }
    }
    memcpy(dst, src, length);
    dst[length] = 0;
}

static void append_message_locked(bool from_user, const char *text) {
    if (s_history_count == CHAT_HISTORY_CAPACITY) {
        memmove(&s_history[0], &s_history[1],
                sizeof(s_history[0]) * (CHAT_HISTORY_CAPACITY - 1));
        --s_history_count;
    }
    chat_message_t *message = &s_history[s_history_count++];
    message->from_user = from_user;
    copy_utf8(message->text, sizeof(message->text), text);
}

static void turn_received(const char *user_text,
                          const char *assistant_text,
                          void *ctx) {
    (void)ctx;
    taskENTER_CRITICAL(&s_history_lock);
    append_message_locked(true, user_text);
    append_message_locked(false, assistant_text);
    taskEXIT_CRITICAL(&s_history_lock);
}

static void enter(void) {
    voice_chat_set_turn_cb(turn_received, NULL);
}

static void exit_app(void) {
}

static gui_action_t tick(int64_t now_ms) {
    if (!s_recording) return GUI_ACTION_NONE;
    if (voice_chat_pump() != ESP_OK) {
        s_recording = false;
        return GUI_ACTION_NONE;
    }
    if ((uint32_t)(now_ms - s_recording_started_ms) >=
        APP_VOICE_MAX_RECORD_MS) {
        return GUI_ACTION_STOP_VOICE;
    }
    return GUI_ACTION_NONE;
}

void app_chat_alarm_set_recording(bool recording, int64_t started_ms) {
    s_recording = recording;
    if (recording) s_recording_started_ms = started_ms;
}

bool app_chat_alarm_is_recording(void) {
    return s_recording;
}

static void centered_text(int y, const char *text,
                          uint8_t size, ui_color_t color) {
    ui_draw_text((APP_UI_W - ui_text_width(text, size)) / 2,
                 y, text, size, color);
}

static void render_setup(void) {
    centered_text(18, "无线配网", 3, UI_COLOR_WARNING);
    ui_fill_rect(28, 58, 264, 126, UI_COLOR_PANEL);
    ui_draw_text(44, 76, "热点: talkingflower", 2, UI_COLOR_TEXT);
    ui_draw_text(44, 108, "密码: pangmiaomiao", 2, UI_COLOR_TEXT);
    ui_draw_text(44, 140, "地址: 192.168.4.1", 2, UI_COLOR_CYAN);
    centered_text(174, "请在设置中关闭", 2, UI_COLOR_MUTED);
}

static int utf8_char_bytes(const char *text) {
    uint8_t first = (uint8_t)text[0];
    if (first < 0x80) return 1;
    if ((first & 0xE0) == 0xC0) return 2;
    if ((first & 0xF0) == 0xE0) return 3;
    if ((first & 0xF8) == 0xF0) return 4;
    return 1;
}

static void wrap_text(const char *text,
                      char line1[CHAT_TEXT_BYTES],
                      char line2[CHAT_TEXT_BYTES]) {
    char *lines[2] = {line1, line2};
    size_t used[2] = {0, 0};
    int widths[2] = {0, 0};
    int line = 0;
    lines[0][0] = 0;
    lines[1][0] = 0;

    while (*text && line < 2) {
        int bytes = utf8_char_bytes(text);
        int width = ((uint8_t)text[0] < 0x80) ? 12 : 16;
        if (widths[line] + width > 206) {
            ++line;
            continue;
        }
        if (used[line] + (size_t)bytes >= CHAT_TEXT_BYTES) break;
        memcpy(lines[line] + used[line], text, bytes);
        used[line] += bytes;
        lines[line][used[line]] = 0;
        widths[line] += width;
        text += bytes;
    }
    if (*text && used[1] + 3 < CHAT_TEXT_BYTES) {
        memcpy(line2 + used[1], "...", 4);
    }
}

static void draw_bubble(int y, const chat_message_t *message) {
    const int width = 230;
    const int height = 38;
    int x = message->from_user ? APP_UI_W - width - 12 : 12;
    ui_color_t background =
        message->from_user ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL;
    ui_color_t border =
        message->from_user ? UI_COLOR_PRIMARY : UI_COLOR_PANEL_ALT;

    ui_fill_rect(x, y, width, height, background);
    ui_draw_rect(x, y, width, height, 1, border);
    if (message->from_user) {
        ui_fill_rect(x + width, y + 23, 6, 8, background);
    } else {
        ui_fill_rect(x - 6, y + 23, 6, 8, background);
    }

    char line1[CHAT_TEXT_BYTES];
    char line2[CHAT_TEXT_BYTES];
    wrap_text(message->text, line1, line2);
    ui_draw_text(x + 8, y + 3, line1, 2, UI_COLOR_TEXT);
    ui_draw_text(x + 8, y + 20, line2, 2, UI_COLOR_TEXT);
}

static void render_chat(const gui_model_t *model, int focused_item) {
    ui_fill_rect(0, 0, APP_UI_W, 32, UI_COLOR_NAVY);
    ui_draw_text(12, 8, "闲聊闹钟", 2, UI_COLOR_TEXT);
    if (model->voice_visible) {
        int status_x = APP_UI_W - ui_text_width(model->voice_title, 2) - 10;
        ui_draw_text(status_x, 8, model->voice_title, 2, UI_COLOR_CYAN);
    }

    chat_message_t snapshot[CHAT_HISTORY_CAPACITY];
    size_t count;
    taskENTER_CRITICAL(&s_history_lock);
    count = s_history_count;
    memcpy(snapshot, s_history, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_history_lock);

    if (count == 0) {
        chat_message_t greeting = {
            .from_user = false,
            .text = "你好，按住主按钮和我聊天。",
        };
        draw_bubble(48, &greeting);
    } else {
        size_t visible = count < CHAT_VISIBLE_MESSAGES
                             ? count : CHAT_VISIBLE_MESSAGES;
        size_t first = count - visible;
        for (size_t i = 0; i < visible; ++i) {
            draw_bubble(38 + (int)i * 43, &snapshot[first + i]);
        }
    }

    ui_fill_rect(44, 174, 232, 28,
                 focused_item == 0 ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL_ALT);
    ui_draw_rect(44, 174, 232, 28, focused_item == 0 ? 3 : 1,
                 focused_item == 0 ? UI_COLOR_FOCUS : UI_COLOR_MUTED);
    centered_text(180, s_recording ? "松开结束录音" : "短按闲聊 / 长按说话", 2,
                  focused_item == 0 ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
}

static void render(const gui_model_t *model, int focused_item) {
    if (model->setup_mode) {
        render_setup();
    } else {
        render_chat(model, focused_item);
    }
}

static gui_action_t activate(uint8_t item) {
    return item == 0 ? GUI_ACTION_CHIME : GUI_ACTION_NONE;
}

static bool handle_input(gui_input_t input, gui_action_t *action) {
    if (input == GUI_INPUT_LONG_PRESS) {
        *action = GUI_ACTION_START_VOICE;
        return true;
    }
    if (input == GUI_INPUT_RELEASE && s_recording) {
        *action = GUI_ACTION_STOP_VOICE;
        return true;
    }
    if (s_recording) {
        *action = GUI_ACTION_NONE;
        return true;
    }
    return false;
}

static const gui_app_t s_app = {
    .id = "chat_alarm",
    .label = "闲聊闹钟",
    .icon = GUI_ICON_CHAT,
    .focus_count = 1,
    .enter = enter,
    .exit = exit_app,
    .tick = tick,
    .render = render,
    .activate = activate,
    .handle_input = handle_input,
};

const gui_app_t *app_chat_alarm_descriptor(void) {
    return &s_app;
}
