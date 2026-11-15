#include "app_aquarium.h"

#include "ui_screen.h"
#include "small_apps_art.h"
#include "rgb565.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <math.h>
#include <stdio.h>

#define FISH_MAX 12
#define FISH_DEFAULT 5
#define FISH_STEP 1
#define FRAME_MS 66
#define FOOD_FALL_SPEED 8.0f
#define FOOD_EAT_RADIUS 14.0f
#define TANK_X 4
#define TANK_Y 34
#define TANK_W 312
#define TANK_H 172
#define TANK_LEFT (TANK_X + FISH_ORANGE_W / 2.0f + 2.0f)
#define TANK_RIGHT (TANK_X + TANK_W - FISH_ORANGE_W / 2.0f - 2.0f)
#define TANK_TOP (TANK_Y + FISH_ORANGE_H / 2.0f + 6.0f)
#define TANK_BOTTOM (TANK_Y + TANK_H - FISH_ORANGE_H / 2.0f - 12.0f)

typedef struct {
    float x, y;
    float vx, vy;
} fish_t;

typedef struct {
    fish_t fish[FISH_MAX];
    fish_t next[FISH_MAX];
    bool food_active;
    float food_x, food_y;
    uint16_t pixels[TANK_W * TANK_H];
} school_t;

static const char *TAG = "aquarium";
static school_t *s_school;
static int s_count = FISH_DEFAULT;
static int64_t s_last_frame_ms;
static unsigned s_frame;

static float random_unit(void) {
    return (float)(esp_random() & 0xFFFF) / 65535.0f;
}

static void add_fish(int index) {
    s_school->fish[index] = (fish_t){
        .x = TANK_LEFT + random_unit() * (TANK_RIGHT - TANK_LEFT),
        .y = TANK_TOP + random_unit() * (TANK_BOTTOM - TANK_TOP),
        .vx = (random_unit() < 0.5f ? -1.0f : 1.0f) *
              (14.0f + random_unit() * 10.0f),
        .vy = (random_unit() - 0.5f) * 12.0f,
    };
}

static void enter(void) {
    s_school = heap_caps_calloc(1, sizeof(*s_school),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_school) {
        ESP_LOGE(TAG, "cannot allocate fish school in PSRAM");
        return;
    }
    for (int i = 0; i < s_count; ++i) add_fish(i);
    s_last_frame_ms = 0;
    s_frame = 0;
}

static void exit_app(void) {
    heap_caps_free(s_school);
    s_school = NULL;
}

static bool drop_food(void) {
    if (s_school->food_active) return false;
    s_school->food_x = TANK_LEFT + random_unit() * (TANK_RIGHT - TANK_LEFT);
    s_school->food_y = TANK_TOP;
    s_school->food_active = true;
    return true;
}

static int first_food_eater(float previous_food_y) {
    int eater = -1;
    float earliest = 2.0f;
    for (int i = 0; i < s_count; ++i) {
        fish_t previous = s_school->fish[i];
        fish_t next = s_school->next[i];
        float x = previous.x - s_school->food_x;
        float y = previous.y - previous_food_y;
        float dx = next.x - previous.x;
        float dy = next.y - previous.y - (s_school->food_y - previous_food_y);
        float c = x * x + y * y - FOOD_EAT_RADIUS * FOOD_EAT_RADIUS;
        float arrival = 0.0f;
        if (c > 0) {
            float a = dx * dx + dy * dy;
            float b = x * dx + y * dy;
            float discriminant = b * b - a * c;
            if (a < 0.000001f || b >= 0 || discriminant < 0) continue;
            arrival = (-b - sqrtf(discriminant)) / a;
            if (arrival < 0 || arrival > 1) continue;
        }
        if (arrival < earliest) {
            earliest = arrival;
            eater = i;
        }
    }
    return eater;
}

static bool step_school(float dt) {
    bool eaten = false;
    float previous_food_y = s_school->food_y;
    if (s_school->food_active) {
        s_school->food_y = fminf(TANK_BOTTOM,
                                 s_school->food_y + FOOD_FALL_SPEED * dt);
    }
    for (int i = 0; i < s_count; ++i) {
        fish_t fish = s_school->fish[i];
        float sx = 0, sy = 0, ax = 0, ay = 0, cx = 0, cy = 0;
        int neighbors = 0;
        for (int j = 0; j < s_count; ++j) {
            if (i == j) continue;
            fish_t other = s_school->fish[j];
            float dx = fish.x - other.x;
            float dy = fish.y - other.y;
            float distance2 = dx * dx + dy * dy;
            if (distance2 > 64.0f * 64.0f) continue;
            ax += other.vx;
            ay += other.vy;
            cx += other.x;
            cy += other.y;
            ++neighbors;
            if (distance2 < 30.0f * 30.0f) {
                if (distance2 < 1.0f) {
                    sx += i < j ? -1.0f : 1.0f;
                } else {
                    sx += dx / distance2;
                    sy += dy / distance2;
                }
            }
        }
        float fx = sx * 220.0f;
        float fy = sy * 220.0f;
        if (neighbors > 0) {
            fx += (ax / neighbors - fish.vx) * 0.9f +
                  (cx / neighbors - fish.x) * 0.18f;
            fy += (ay / neighbors - fish.vy) * 0.9f +
                  (cy / neighbors - fish.y) * 0.18f;
        }
        if (fish.x < TANK_LEFT + 30) fx += (TANK_LEFT + 30 - fish.x) * 2;
        if (fish.x > TANK_RIGHT - 30) fx -= (fish.x - TANK_RIGHT + 30) * 2;
        if (fish.y < TANK_TOP + 20) fy += (TANK_TOP + 20 - fish.y) * 2;
        if (fish.y > TANK_BOTTOM - 20) fy -= (fish.y - TANK_BOTTOM + 20) * 2;
        if (s_school->food_active) {
            float dx = s_school->food_x - fish.x;
            float dy = s_school->food_y - fish.y;
            float distance = sqrtf(dx * dx + dy * dy);
            if (distance > 0.01f) {
                fx = fx * 0.15f + (dx * 32.0f / distance - fish.vx) * 5.0f;
                fy = fy * 0.15f + (dy * 32.0f / distance - fish.vy) * 5.0f;
            }
        }
        fish.vx += fx * dt;
        fish.vy += fy * dt;
        float speed = sqrtf(fish.vx * fish.vx + fish.vy * fish.vy);
        if (speed < 0.01f) {
            fish.vx = i & 1 ? 14.0f : -14.0f;
        } else {
            float target = fminf(32.0f, fmaxf(12.0f, speed));
            fish.vx *= target / speed;
            fish.vy *= target / speed;
        }
        fish.x += fish.vx * dt;
        fish.y += fish.vy * dt;
        if (fish.x < TANK_LEFT || fish.x > TANK_RIGHT) {
            fish.x = fminf(TANK_RIGHT, fmaxf(TANK_LEFT, fish.x));
            fish.vx = -fish.vx;
        }
        if (fish.y < TANK_TOP || fish.y > TANK_BOTTOM) {
            fish.y = fminf(TANK_BOTTOM, fmaxf(TANK_TOP, fish.y));
            fish.vy = -fish.vy;
        }
        s_school->next[i] = fish;
    }
    if (s_school->food_active) {
        // Compare within-step arrival times, not fish array order.
        int eater = first_food_eater(previous_food_y);
        if (eater >= 0) {
            s_school->food_active = false;
            eaten = true;
            ESP_LOGI(TAG, "fish %d ate the food", eater + 1);
        }
    }
    for (int i = 0; i < s_count; ++i) {
        s_school->fish[i] = s_school->next[i];
    }
    return eaten;
}

static gui_action_t tick(int64_t now_ms) {
    if (!s_school) return GUI_ACTION_NONE;
    if (s_last_frame_ms == 0) s_last_frame_ms = now_ms;
    int64_t elapsed = now_ms - s_last_frame_ms;
    if (elapsed < FRAME_MS) return GUI_ACTION_NONE;
    s_last_frame_ms = now_ms;
    bool eaten = step_school((float)(elapsed > 100 ? 100 : elapsed) / 1000.0f);
    ++s_frame;
    return eaten ? GUI_ACTION_AQUARIUM_EATEN : GUI_ACTION_REDRAW;
}

static void composite_sprite(int x, int y, int width, int height,
                              const uint16_t *pixels, const uint8_t *alpha,
                              bool mirror) {
    x -= TANK_X;
    y -= TANK_Y;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= TANK_W || py >= TANK_H) continue;
            size_t source = (size_t)row * width + (mirror ? width - 1 - col : col);
            size_t target = (size_t)py * TANK_W + px;
            s_school->pixels[target] = rgb565_blend(
                pixels[source], s_school->pixels[target], alpha[source]);
        }
    }
}

static esp_err_t render_overlay(void) {
    if (!s_school) return ESP_OK;
    esp_err_t err = ui_copy_canvas_rgb565(s_school->pixels, TANK_X, TANK_Y,
                                         TANK_W, TANK_H);
    if (err != ESP_OK) return err;
    for (int i = 0; i < s_count; ++i) {
        fish_t fish = s_school->fish[i];
        int x = (int)fish.x - FISH_ORANGE_W / 2;
        int y = (int)fish.y - FISH_ORANGE_H / 2;
        const uint16_t *pixels = i % 2 ? s_fish_purple_rgb565 : s_fish_orange_rgb565;
        const uint8_t *alpha = i % 2 ? s_fish_purple_alpha : s_fish_orange_alpha;
        composite_sprite(x, y, FISH_ORANGE_W, FISH_ORANGE_H, pixels, alpha,
                          fish.vx > 0);
    }
    if (s_school->food_active) {
        composite_sprite((int)s_school->food_x - FOOD_WORM_W / 2,
                          (int)s_school->food_y - FOOD_WORM_H / 2,
                          FOOD_WORM_W, FOOD_WORM_H,
                          s_food_worm_rgb565, s_food_worm_alpha, false);
    }
    return ui_draw_rgb565_bitmap(TANK_X, TANK_Y, TANK_W, TANK_H,
                                 s_school->pixels, NULL, UI_COLOR_PRIMARY_DARK);
}

static void render(const gui_model_t *model, int focused_item) {
    (void)model;
    ui_fill_rect(0, 0, 320, 32, UI_COLOR_NAVY);
    ui_draw_text(12, 8, "小鱼缸", 2, UI_COLOR_TEXT);
    char count[32];
    snprintf(count, sizeof(count), "%d / %d 条", s_count, FISH_MAX);
    ui_draw_text(88, 10, count, 1, UI_COLOR_CYAN);
    ui_fill_round_rect(TANK_X, TANK_Y, TANK_W, TANK_H, 8, UI_COLOR_PRIMARY_DARK);
    ui_draw_line(TANK_X + 10, TANK_Y + 6,
                 TANK_X + TANK_W - 10, TANK_Y + 6, UI_COLOR_CYAN);
    int floor_y = TANK_Y + TANK_H - 10;
    ui_fill_rect(TANK_X + 7, floor_y, TANK_W - 14, 6, UI_COLOR_WARNING);
    for (int i = 0; i < 5; ++i) {
        int x = 28 + i * 64;
        ui_draw_line(x, floor_y, x - 5, floor_y - 18, UI_COLOR_SUCCESS);
        ui_draw_line(x, floor_y, x + 6, floor_y - 23, UI_COLOR_SUCCESS);
        ui_draw_line(x + 2, floor_y - 5, x + 12, floor_y - 15, UI_COLOR_SUCCESS);
    }
    for (int i = 0; i < 6; ++i) {
        int y = floor_y - 8 -
                (int)((s_frame * 2 + (unsigned)i * 23) % (TANK_H - 36));
        int x = 18 + i * 55;
        ui_fill_circle(x, y, 2, UI_COLOR_CYAN);
        ui_set_pixel(x, y, UI_COLOR_PRIMARY_DARK);
    }
    if (!s_school) {
        ui_draw_text(65, 92, "内存不足，请返回后重试", 1, UI_COLOR_DANGER);
    }
    const char *labels[] = {"-", "+",
                            s_school && s_school->food_active ? "追食" : "喂食"};
    for (int i = 0; i < 3; ++i) {
        int x = i == 2 ? 262 : 182 + i * 40;
        int width = i == 2 ? 52 : 32;
        bool focus = focused_item == i;
        ui_fill_round_rect(x, 5, width, 22, 4,
                           focus ? UI_COLOR_PRIMARY_DARK : UI_COLOR_PANEL);
        ui_draw_rect(x, 5, width, 22, focus ? 2 : 1,
                     focus ? UI_COLOR_FOCUS : UI_COLOR_PANEL_ALT);
        ui_draw_text(x + 10, 9, labels[i], i == 2 ? 1 : 2,
                     focus ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
    }
}

static gui_action_t activate(uint8_t item) {
    if (!s_school) return GUI_ACTION_NONE;
    if (item == 0 && s_count > FISH_STEP) s_count -= FISH_STEP;
    if (item == 1 && s_count < FISH_MAX) {
        for (int i = s_count; i < s_count + FISH_STEP; ++i) add_fish(i);
        s_count += FISH_STEP;
    }
    if (item == 2 && drop_food()) return GUI_ACTION_AQUARIUM_DROP;
    return GUI_ACTION_REDRAW;
}

static const gui_focus_node_t s_focus_grid[] = {
    {.left = GUI_FOCUS_HOME, .right = 1, .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
    {.left = 0, .right = 2, .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
    {.left = 1, .right = GUI_FOCUS_HOME, .up = GUI_FOCUS_HOME, .down = GUI_FOCUS_HOME},
};

static const gui_app_t s_app = {
    .id = "aquarium",
    .label = "小鱼缸",
    .icon = GUI_ICON_AQUARIUM,
    .focus_count = 3,
    .focus_grid = s_focus_grid,
    .enter = enter,
    .exit = exit_app,
    .tick = tick,
    .render = render,
    .render_overlay = render_overlay,
    .activate = activate,
};

const gui_app_t *app_aquarium_descriptor(void) {
    return &s_app;
}
