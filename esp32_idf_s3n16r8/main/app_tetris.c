#include "app_tetris.h"

#include "ui_screen.h"

#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#define BOARD_W 10
#define BOARD_H 16
#define BLOCK 10
#define BOARD_X 10
#define BOARD_Y 37
#define DROP_MS 520

typedef struct {
    int8_t x;
    int8_t y;
} cell_t;

static const cell_t s_shapes[7][4] = {
    {{-1, 0}, {0, 0}, {1, 0}, {2, 0}},
    {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
    {{-1, 0}, {0, 0}, {1, 0}, {0, 1}},
    {{-1, 0}, {0, 0}, {0, 1}, {1, 1}},
    {{0, 0}, {1, 0}, {-1, 1}, {0, 1}},
    {{-1, 0}, {0, 0}, {1, 0}, {-1, 1}},
    {{-1, 0}, {0, 0}, {1, 0}, {1, 1}},
};

static const ui_color_t s_piece_colors[7] = {
    UI_COLOR_CYAN, UI_COLOR_WARNING, UI_COLOR_MAGENTA, UI_COLOR_SUCCESS,
    UI_COLOR_DANGER, UI_COLOR_PRIMARY, UI_COLOR_FOCUS,
};

static uint8_t s_board[BOARD_H][BOARD_W];
static int s_piece;
static int s_next;
static int s_rotation;
static int s_piece_x;
static int s_piece_y;
static int s_score;
static int s_lines;
static int64_t s_last_drop_ms;
static bool s_running;
static bool s_game_over;

static cell_t rotated_cell(cell_t cell, int rotation, int piece) {
    if (piece == 1) return cell;
    for (int i = 0; i < rotation; ++i) {
        int8_t x = cell.x;
        cell.x = -cell.y;
        cell.y = x;
    }
    return cell;
}

static bool collides(int piece_x, int piece_y, int rotation) {
    for (int i = 0; i < 4; ++i) {
        cell_t cell = rotated_cell(s_shapes[s_piece][i], rotation, s_piece);
        int x = piece_x + cell.x;
        int y = piece_y + cell.y;
        if (x < 0 || x >= BOARD_W || y >= BOARD_H) return true;
        if (y >= 0 && s_board[y][x]) return true;
    }
    return false;
}

static void spawn_piece(void) {
    s_piece = s_next;
    s_next = (int)(esp_random() % 7);
    s_rotation = 0;
    s_piece_x = BOARD_W / 2;
    s_piece_y = 0;
    if (collides(s_piece_x, s_piece_y, s_rotation)) {
        s_running = false;
        s_game_over = true;
    }
}

static void start_game(void) {
    memset(s_board, 0, sizeof(s_board));
    s_score = 0;
    s_lines = 0;
    s_game_over = false;
    s_running = true;
    s_next = (int)(esp_random() % 7);
    spawn_piece();
    s_last_drop_ms = 0;
}

static void clear_lines(void) {
    int cleared = 0;
    for (int y = BOARD_H - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < BOARD_W; ++x) {
            if (!s_board[y][x]) {
                full = false;
                break;
            }
        }
        if (!full) continue;
        memmove(&s_board[1][0], &s_board[0][0],
                (size_t)y * BOARD_W);
        memset(&s_board[0][0], 0, BOARD_W);
        ++cleared;
        ++y;
    }
    static const int bonuses[] = {0, 100, 300, 500, 800};
    s_lines += cleared;
    s_score += bonuses[cleared];
}

static void lock_piece(void) {
    for (int i = 0; i < 4; ++i) {
        cell_t cell = rotated_cell(s_shapes[s_piece][i], s_rotation, s_piece);
        int x = s_piece_x + cell.x;
        int y = s_piece_y + cell.y;
        if (y >= 0 && y < BOARD_H && x >= 0 && x < BOARD_W) {
            s_board[y][x] = (uint8_t)(s_piece + 1);
        }
    }
    clear_lines();
    spawn_piece();
}

static void step_down(void) {
    if (!s_running) return;
    if (!collides(s_piece_x, s_piece_y + 1, s_rotation)) {
        ++s_piece_y;
        ++s_score;
    } else {
        lock_piece();
    }
}

static gui_action_t tick(int64_t now_ms) {
    if (!s_running) return GUI_ACTION_NONE;
    int interval = DROP_MS - s_lines * 12;
    if (interval < 140) interval = 140;
    if (s_last_drop_ms == 0) s_last_drop_ms = now_ms;
    if (now_ms - s_last_drop_ms >= interval) {
        step_down();
        s_last_drop_ms = now_ms;
        return GUI_ACTION_REDRAW;
    }
    return GUI_ACTION_NONE;
}

static void draw_block(int x, int y, ui_color_t color) {
    ui_fill_round_rect(x + 1, y + 1, BLOCK - 2, BLOCK - 2, 2, color);
    ui_draw_line(x + 2, y + 2, x + BLOCK - 4, y + 2, UI_COLOR_WHITE);
}

static void draw_piece_preview(int piece, int center_x, int center_y) {
    for (int i = 0; i < 4; ++i) {
        cell_t cell = s_shapes[piece][i];
        draw_block(center_x + cell.x * BLOCK,
                   center_y + cell.y * BLOCK,
                   s_piece_colors[piece]);
    }
}

static void render(const gui_model_t *model, int focused_item) {
    (void)model;
    ui_draw_text(10, 8, "俄罗斯方块", 2, UI_COLOR_TEXT);
    ui_draw_rect(BOARD_X - 2, BOARD_Y - 2,
                 BOARD_W * BLOCK + 4, BOARD_H * BLOCK + 4,
                 2, UI_COLOR_PANEL_ALT);
    ui_fill_rect(BOARD_X, BOARD_Y, BOARD_W * BLOCK,
                 BOARD_H * BLOCK, UI_COLOR_BLACK);

    for (int y = 0; y < BOARD_H; ++y) {
        for (int x = 0; x < BOARD_W; ++x) {
            if (s_board[y][x]) {
                draw_block(BOARD_X + x * BLOCK, BOARD_Y + y * BLOCK,
                           s_piece_colors[s_board[y][x] - 1]);
            }
        }
    }
    if (s_running) {
        for (int i = 0; i < 4; ++i) {
            cell_t cell = rotated_cell(s_shapes[s_piece][i],
                                       s_rotation, s_piece);
            draw_block(BOARD_X + (s_piece_x + cell.x) * BLOCK,
                       BOARD_Y + (s_piece_y + cell.y) * BLOCK,
                       s_piece_colors[s_piece]);
        }
    }

    char value[24];
    ui_draw_text(132, 42, "积分", 2, UI_COLOR_MUTED);
    snprintf(value, sizeof(value), "%d", s_score);
    ui_draw_text(132, 64, value, 3, UI_COLOR_FOCUS);
    ui_draw_text(132, 94, "消行", 2, UI_COLOR_MUTED);
    snprintf(value, sizeof(value), "%d", s_lines);
    ui_draw_text(132, 116, value, 2, UI_COLOR_TEXT);
    ui_draw_text(132, 142, "下一个", 2, UI_COLOR_MUTED);
    draw_piece_preview(s_next, 205, 174);

    ui_fill_round_rect(226, 42, 82, 34, 7,
                       focused_item == 0 ? UI_COLOR_PRIMARY_DARK
                                         : UI_COLOR_PANEL);
    ui_draw_rect(226, 42, 82, 34, focused_item == 0 ? 3 : 1,
                 focused_item == 0 ? UI_COLOR_FOCUS : UI_COLOR_PANEL_ALT);
    ui_draw_text(238, 51, s_running ? "重开" : "开始", 2,
                 focused_item == 0 ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
    if (s_game_over) {
        ui_fill_round_rect(28, 100, 66, 42, 6, UI_COLOR_DANGER);
        ui_draw_text(35, 113, "结束", 2, UI_COLOR_WHITE);
    }
}

static gui_action_t activate(uint8_t item) {
    if (item == 0) start_game();
    return GUI_ACTION_NONE;
}

static bool handle_input(gui_input_t input, gui_action_t *action) {
    if (!s_running) return false;
    *action = GUI_ACTION_NONE;
    if (input == GUI_INPUT_LEFT &&
        !collides(s_piece_x - 1, s_piece_y, s_rotation)) {
        --s_piece_x;
    } else if (input == GUI_INPUT_RIGHT &&
               !collides(s_piece_x + 1, s_piece_y, s_rotation)) {
        ++s_piece_x;
    } else if (input == GUI_INPUT_DOWN) {
        step_down();
    } else if (input == GUI_INPUT_ACTIVATE ||
               input == GUI_INPUT_AUX_ACTIVATE ||
               input == GUI_INPUT_UP) {
        int next_rotation = (s_rotation + 1) & 3;
        if (!collides(s_piece_x, s_piece_y, next_rotation)) {
            s_rotation = next_rotation;
        }
    } else {
        return false;
    }
    return true;
}

static const gui_focus_node_t s_focus_grid[] = {
    {.left = GUI_FOCUS_HOME, .right = GUI_FOCUS_HOME,
     .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
};

static const gui_app_t s_app = {
    .id = "tetris",
    .label = "俄罗斯方块",
    .icon = GUI_ICON_TETRIS,
    .focus_count = 1,
    .focus_grid = s_focus_grid,
    .enter = NULL,
    .exit = NULL,
    .tick = tick,
    .render = render,
    .activate = activate,
    .handle_input = handle_input,
};

const gui_app_t *app_tetris_descriptor(void) {
    return &s_app;
}
