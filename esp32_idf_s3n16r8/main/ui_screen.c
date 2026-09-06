#include "ui_screen.h"
#include "st7789.h"
#include "app_config.h"
#include "font5x7.h"
#include "font_zh16.h"

#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#define MAX_LINE_CHARS 52
#define FRAMEBUFFER_BYTES (APP_UI_W * APP_UI_H / 2)
#define PANEL_RGB565(color) ((uint16_t)(~(uint16_t)(color)))

static const char *TAG = "ui";
static uint8_t *s_framebuffer;
static uint8_t *s_previous_framebuffer;
static bool s_previous_valid;
static const uint16_t s_palette[16] = {
    [UI_COLOR_BLACK] = PANEL_RGB565(0x0000),
    [UI_COLOR_WHITE] = PANEL_RGB565(0xFFFF),
    [UI_COLOR_BG] = PANEL_RGB565(0x0861),
    [UI_COLOR_PANEL] = PANEL_RGB565(0x18E3),
    [UI_COLOR_PANEL_ALT] = PANEL_RGB565(0x2945),
    [UI_COLOR_PRIMARY] = PANEL_RGB565(0x253F),
    [UI_COLOR_PRIMARY_DARK] = PANEL_RGB565(0x1297),
    [UI_COLOR_FOCUS] = PANEL_RGB565(0xFFE0),
    [UI_COLOR_SUCCESS] = PANEL_RGB565(0x07E0),
    [UI_COLOR_WARNING] = PANEL_RGB565(0xFD20),
    [UI_COLOR_DANGER] = PANEL_RGB565(0xF800),
    [UI_COLOR_TEXT] = PANEL_RGB565(0xEF7D),
    [UI_COLOR_MUTED] = PANEL_RGB565(0x8410),
    [UI_COLOR_CYAN] = PANEL_RGB565(0x07FF),
    [UI_COLOR_MAGENTA] = PANEL_RGB565(0xF81F),
    [UI_COLOR_NAVY] = PANEL_RGB565(0x0010),
};

static void truncate_into(char *dst, const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > MAX_LINE_CHARS) n = MAX_LINE_CHARS;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void ui_begin(ui_color_t color) {
    uint8_t packed = ((uint8_t)color << 4) | (uint8_t)color;
    memset(s_framebuffer, packed, FRAMEBUFFER_BYTES);
}

void ui_set_pixel(int x, int y, ui_color_t color) {
    if ((unsigned)x >= APP_UI_W || (unsigned)y >= APP_UI_H) return;
    size_t offset = ((size_t)y * APP_UI_W + x) >> 1;
    if (x & 1) {
        s_framebuffer[offset] =
            (s_framebuffer[offset] & 0xF0) | ((uint8_t)color & 0x0F);
    } else {
        s_framebuffer[offset] =
            (s_framebuffer[offset] & 0x0F) | ((uint8_t)color << 4);
    }
}

void ui_fill_rect(int x, int y, int width, int height, ui_color_t color) {
    for (int yy = y; yy < y + height; ++yy) {
        for (int xx = x; xx < x + width; ++xx) {
            ui_set_pixel(xx, yy, color);
        }
    }
}

void ui_draw_rect(int x, int y, int width, int height,
                  int thickness, ui_color_t color) {
    if (thickness < 1) thickness = 1;
    ui_fill_rect(x, y, width, thickness, color);
    ui_fill_rect(x, y + height - thickness, width, thickness, color);
    ui_fill_rect(x, y, thickness, height, color);
    ui_fill_rect(x + width - thickness, y, thickness, height, color);
}

void ui_fill_circle(int center_x, int center_y, int radius, ui_color_t color) {
    int radius_sq = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius_sq) {
                ui_set_pixel(center_x + x, center_y + y, color);
            }
        }
    }
}

void ui_fill_round_rect(int x, int y, int width, int height,
                        int radius, ui_color_t color) {
    if (radius < 1) {
        ui_fill_rect(x, y, width, height, color);
        return;
    }
    if (radius * 2 > width) radius = width / 2;
    if (radius * 2 > height) radius = height / 2;
    ui_fill_rect(x + radius, y, width - radius * 2, height, color);
    ui_fill_rect(x, y + radius, width, height - radius * 2, color);
    ui_fill_circle(x + radius, y + radius, radius, color);
    ui_fill_circle(x + width - radius - 1, y + radius, radius, color);
    ui_fill_circle(x + radius, y + height - radius - 1, radius, color);
    ui_fill_circle(x + width - radius - 1, y + height - radius - 1,
                   radius, color);
}

void ui_draw_round_rect(int x, int y, int width, int height,
                        int radius, int thickness, ui_color_t color) {
    if (thickness < 1) thickness = 1;
    ui_fill_round_rect(x, y, width, height, radius, color);
    if (width > thickness * 2 && height > thickness * 2) {
        ui_fill_round_rect(x + thickness, y + thickness,
                           width - thickness * 2, height - thickness * 2,
                           radius > thickness ? radius - thickness : 1,
                           UI_COLOR_BG);
    }
}

static int edge_value(int ax, int ay, int bx, int by, int px, int py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void ui_fill_triangle(int x0, int y0, int x1, int y1,
                      int x2, int y2, ui_color_t color) {
    int min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x) min_x = x2;
    int max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x) max_x = x2;
    int min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y) min_y = y2;
    int max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y) max_y = y2;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            int e0 = edge_value(x0, y0, x1, y1, x, y);
            int e1 = edge_value(x1, y1, x2, y2, x, y);
            int e2 = edge_value(x2, y2, x0, y0, x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                ui_set_pixel(x, y, color);
            }
        }
    }
}

void ui_draw_line(int x0, int y0, int x1, int y1, ui_color_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ui_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * err;
        if (twice >= dy) {
            err += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_glyph(int x, int y, char c,
                       uint8_t size, ui_color_t color) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = &font5x7[(c - 0x20) * 5];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (((bits >> row) & 1) == 0) continue;
            for (int sy = 0; sy < size; ++sy) {
                for (int sx = 0; sx < size; ++sx) {
                    ui_set_pixel(x + col * size + sx,
                                 y + row * size + sy, color);
                }
            }
        }
    }
}

static uint32_t next_codepoint(const char **text) {
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t codepoint;
    if (p[0] < 0x80) {
        codepoint = p[0];
        *text += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *text += 2;
    } else if ((p[0] & 0xF0) == 0xE0 &&
               (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0F) << 12) |
                    ((uint32_t)(p[1] & 0x3F) << 6) |
                    (p[2] & 0x3F);
        *text += 3;
    } else {
        codepoint = '?';
        *text += 1;
    }
    return codepoint;
}

static int zh_scale(uint8_t size) {
    return size <= 2 ? 1 : (size + 1) / 2;
}

static void draw_zh_glyph(int x, int y, const uint8_t *bitmap,
                          int scale, ui_color_t color) {
    for (int row = 0; row < 16; ++row) {
        uint16_t bits = ((uint16_t)bitmap[row * 2] << 8) |
                        bitmap[row * 2 + 1];
        for (int col = 0; col < 16; ++col) {
            if ((bits & (0x8000U >> col)) == 0) continue;
            ui_fill_rect(x + col * scale, y + row * scale,
                         scale, scale, color);
        }
    }
}

void ui_draw_text(int x, int y, const char *text,
                  uint8_t size, ui_color_t color) {
    if (!text) return;
    if (size == 0) size = 1;
    int cursor_x = x;
    while (*text) {
        uint32_t codepoint = next_codepoint(&text);
        if (codepoint == '\n') {
            y += 18 * zh_scale(size);
            cursor_x = x;
            continue;
        }
        if (codepoint < 0x80) {
            draw_glyph(cursor_x, y, (char)codepoint, size, color);
            cursor_x += 6 * size;
        } else {
            const uint8_t *bitmap = font_zh16_bitmap(codepoint);
            int scale = zh_scale(size);
            if (bitmap) {
                draw_zh_glyph(cursor_x, y, bitmap, scale, color);
                cursor_x += 16 * scale;
            } else {
                draw_glyph(cursor_x, y, '?', size, color);
                cursor_x += 6 * size;
            }
        }
        if (cursor_x >= APP_UI_W) break;
    }
}

int ui_text_width(const char *text, uint8_t size) {
    if (!text) return 0;
    if (size == 0) size = 1;
    int width = 0;
    while (*text) {
        uint32_t codepoint = next_codepoint(&text);
        if (codepoint == '\n') break;
        width += codepoint < 0x80 ? 6 * size : 16 * zh_scale(size);
    }
    return width;
}

esp_err_t ui_flush(void) {
    const int row_bytes = APP_UI_W / 2;
    if (!s_previous_valid) {
        esp_err_t err = st7789_show_indexed4(
            s_framebuffer, APP_UI_W, APP_UI_H, s_palette);
        if (err == ESP_OK) {
            memcpy(s_previous_framebuffer, s_framebuffer, FRAMEBUFFER_BYTES);
            s_previous_valid = true;
        }
        return err;
    }

    int16_t first_changed[APP_UI_H];
    int16_t last_changed[APP_UI_H];
    size_t dirty_bytes = 0;
    for (int y = 0; y < APP_UI_H; ++y) {
        const uint8_t *current = s_framebuffer + y * row_bytes;
        const uint8_t *previous = s_previous_framebuffer + y * row_bytes;
        int first = 0;
        while (first < row_bytes && current[first] == previous[first]) ++first;
        if (first == row_bytes) {
            first_changed[y] = -1;
            last_changed[y] = -1;
            continue;
        }
        int last = row_bytes - 1;
        while (last > first && current[last] == previous[last]) --last;
        first_changed[y] = (int16_t)first;
        last_changed[y] = (int16_t)last;
        dirty_bytes += (size_t)(last - first + 1);
    }

    if (dirty_bytes == 0) return ESP_OK;
    if (dirty_bytes > FRAMEBUFFER_BYTES / 3) {
        esp_err_t err = st7789_show_indexed4(
            s_framebuffer, APP_UI_W, APP_UI_H, s_palette);
        if (err == ESP_OK) {
            memcpy(s_previous_framebuffer, s_framebuffer, FRAMEBUFFER_BYTES);
        }
        return err;
    }

    for (int y = 0; y < APP_UI_H; ++y) {
        if (first_changed[y] < 0) continue;
        int first = first_changed[y];
        int last = last_changed[y];
        esp_err_t err = st7789_show_indexed4_region(
            s_framebuffer, APP_UI_W, first * 2, y,
            (last - first + 1) * 2, 1, s_palette);
        if (err != ESP_OK) return err;
        memcpy(s_previous_framebuffer + y * row_bytes + first,
               s_framebuffer + y * row_bytes + first,
               (size_t)(last - first + 1));
    }
    return ESP_OK;
}

esp_err_t ui_init(void) {
    s_framebuffer = heap_caps_calloc(
        1, FRAMEBUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "cannot allocate %u-byte UI framebuffer in PSRAM",
                 (unsigned)FRAMEBUFFER_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_previous_framebuffer = heap_caps_calloc(
        1, FRAMEBUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_previous_framebuffer) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
        ESP_LOGE(TAG, "cannot allocate previous UI framebuffer in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    s_previous_valid = false;
    esp_err_t err = st7789_init();
    if (err != ESP_OK) return err;
    ui_begin(UI_COLOR_BLACK);
    return ui_flush();
}

void ui_show_face(const char *title,
                  const char *face_line1,
                  const char *face_line2,
                  const char *footer) {
    char buf[MAX_LINE_CHARS + 1];
    ui_begin(UI_COLOR_BG);

    truncate_into(buf, title);
    ui_draw_text(12, 10, buf, 2, UI_COLOR_TEXT);
    ui_fill_rect(12, 42, 296, 140, UI_COLOR_PANEL);
    int x1 = (APP_UI_W - ui_text_width(face_line1, 5)) / 2;
    int x2 = (APP_UI_W - ui_text_width(face_line2, 5)) / 2;
    ui_draw_text(x1, 62, face_line1, 5, UI_COLOR_FOCUS);
    ui_draw_text(x2, 112, face_line2, 5, UI_COLOR_FOCUS);
    truncate_into(buf, footer);
    ui_draw_text(12, 198, buf, 2, UI_COLOR_MUTED);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_flush());
}

void ui_show_status(const char *title,
                    const char *line1, const char *line2,
                    const char *line3, const char *line4) {
    char buf[MAX_LINE_CHARS + 1];
    ui_begin(UI_COLOR_BG);
    const char *lines[5] = {title, line1, line2, line3, line4};
    for (int i = 0; i < 5; ++i) {
        truncate_into(buf, lines[i]);
        ui_draw_text(12, 12 + i * 38, buf, i == 0 ? 3 : 2,
                     i == 0 ? UI_COLOR_FOCUS : UI_COLOR_TEXT);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_flush());
}

void ui_show_clock(const char *date, const char *time,
                   const char *footer1, const char *footer2) {
    char major[8] = {0};
    char secs[4]  = {0};
    if (time && strlen(time) >= 5) {
        memcpy(major, time, 5); major[5] = 0;
    }
    if (time && strlen(time) >= 8) {
        memcpy(secs, time + 6, 2); secs[2] = 0;
    }

    ui_begin(UI_COLOR_BG);

    char buf[MAX_LINE_CHARS + 1];
    truncate_into(buf, date);
    int date_x = (APP_UI_W - ui_text_width(buf, 2)) / 2;
    ui_draw_text(date_x, 18, buf, 2, UI_COLOR_MUTED);
    ui_fill_rect(16, 54, 288, 96, UI_COLOR_PANEL);
    int text_w = ui_text_width(major, 7);
    int x = (APP_UI_W - text_w) / 2; if (x < 0) x = 0;
    ui_draw_text(x, 74, major, 7, UI_COLOR_FOCUS);
    ui_draw_text(260, 124, secs, 2, UI_COLOR_CYAN);
    truncate_into(buf, footer1);
    ui_draw_text(12, 170, buf, 2, UI_COLOR_TEXT);
    truncate_into(buf, footer2);
    ui_draw_text(12, 198, buf, 2, UI_COLOR_MUTED);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_flush());
}
