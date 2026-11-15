"""Compile actual App/UI code for bounded timer/school checks and optional previews.

python tools\\test_small_apps.py --clang <ESP-Clang bin\\clang.exe>
Add --preview-dir <directory> to save production-rendered PNGs (requires Pillow).
"""

import argparse
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_idle_power import MAIN, prepare_host_environment


def compile_apps(clang, work):
    prepare_host_environment(clang, work)
    (work / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
        "#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n"
        "#define ESP_ERR_INVALID_ARG 0x102\n"
        "const char *esp_err_to_name(int);\n"
        "#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))\n",
        encoding="ascii")
    (work / "esp_log.h").write_text(
        "void host_log(const char *, const char *, ...);\n"
        "#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)\n"
        "#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)\n"
        "#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)\n", encoding="ascii")
    (work / "esp_random.h").write_text(
        "#include <stdint.h>\nuint32_t esp_random(void);\n", encoding="ascii")
    (work / "esp_heap_caps.h").write_text(
        "#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
        "void *heap_caps_calloc(size_t,size_t,unsigned);\nvoid heap_caps_free(void *);\n",
        encoding="ascii")
    (work / "stdio.h").write_text(
        "#include <stddef.h>\n"
        "__declspec(dllimport) int _snprintf(char *,size_t,const char *,...);\n"
        "__declspec(dllimport) int sscanf(const char *,const char *,...);\n"
        "#define snprintf _snprintf\n", encoding="ascii")
    (work / "string.h").write_text(
        "#include <stddef.h>\n"
        "__declspec(dllimport) void *memcpy(void *,const void *,size_t);\n"
        "__declspec(dllimport) void *memset(void *,int,size_t);\n"
        "__declspec(dllimport) int memcmp(const void *,const void *,size_t);\n"
        "__declspec(dllimport) int strcmp(const char *,const char *);\n"
        "__declspec(dllimport) size_t strlen(const char *);\n", encoding="ascii")
    (work / "math.h").write_text(
        "__declspec(dllimport) double sqrt(double);\n"
        "__declspec(dllimport) double sin(double);\n"
        "__declspec(dllimport) double cos(double);\n"
        "static inline float sqrtf(float x){return (float)sqrt(x);}\n"
        "static inline float sinf(float x){return (float)sin(x);}\n"
        "static inline float cosf(float x){return (float)cos(x);}\n"
        "static inline float fminf(float x,float y){return x<y?x:y;}\n"
        "static inline float fmaxf(float x,float y){return x>y?x:y;}\n",
        encoding="ascii")
    (work / "aquarium_test.c").write_text(
        """#include "app_aquarium.c"
int host_school_test(void) {
    enter();
    if (!s_school) return 0;
    while (s_count < FISH_MAX) activate(1);
    for (int step=0; step<3000; ++step) {
        step_school(0.066f);
        for (int i=0; i<s_count; ++i) {
            fish_t f=s_school->fish[i];
            float speed=sqrtf(f.vx*f.vx+f.vy*f.vy);
            if (!(f.x>=TANK_LEFT && f.x<=TANK_RIGHT &&
                  f.y>=TANK_TOP && f.y<=TANK_BOTTOM &&
                  speed>=11.9f && speed<=32.1f)) return 0;
        }
    }
    activate(2);
    if (tick(50000)!=GUI_ACTION_NONE) return 0;
    while (s_count>FISH_STEP) activate(0);
    activate(0);
    int ok=s_count==FISH_STEP;
    exit_app();
    return ok && s_school==NULL;
}
void host_school_preview(void) {
    s_count=12;
    enter();
    for (int i=0;i<60;++i) step_school(0.066f);
    gui_model_t model={0};
    ui_begin(UI_COLOR_BG);
    render(&model,1);
    ui_flush();
    render_overlay();
    exit_app();
}
""", encoding="ascii")
    (work / "pomodoro_test.c").write_text(
        """#include "app_pomodoro.c"
void host_advance_ms(int);
int host_timer_test(void) {
    activate(1);
    activate(0);
    int64_t start=clock_ms();
    app_pomodoro_update(start+25*MINUTE_MS-1);
    if(s_phase!=PHASE_FOCUS || s_completed) return 0;
    app_pomodoro_update(start+25*MINUTE_MS);
    if(s_phase!=PHASE_SHORT_BREAK || s_completed!=1) return 0;
    app_pomodoro_update(start+30*MINUTE_MS);
    if(s_phase!=PHASE_FOCUS) return 0;
    app_pomodoro_update(start+115*MINUTE_MS);
    if(s_phase!=PHASE_LONG_BREAK || s_completed!=4) return 0;
    app_pomodoro_update(start+130*MINUTE_MS);
    if(s_phase!=PHASE_FOCUS || s_completed!=4) return 0;
    app_pomodoro_update(start+13000*MINUTE_MS);
    if(s_phase!=PHASE_FOCUS || s_completed!=400) return 0;
    activate(1);
    activate(0);
    host_advance_ms(1234);
    activate(0);
    int64_t saved=s_remaining_ms;
    host_advance_ms(120000);
    app_pomodoro_update(clock_ms());
    if(s_running || s_remaining_ms!=saved) return 0;
    activate(0);
    app_pomodoro_pause_for_sleep(clock_ms()+1000);
    if(s_running || s_notice || s_remaining_ms!=saved-1000) return 0;
    activate(1);
    activate(0);
    int ok=app_pomodoro_announcement(clock_ms())!=NULL &&
           app_pomodoro_announcement(clock_ms()+30000)==NULL;
    activate(1);
    return ok && s_remaining_ms==25*MINUTE_MS && s_completed==0;
}
void host_timer_preview(void) {
    activate(1);
    gui_model_t model={.time_valid=true,.time="10:08:30"};
    ui_begin(UI_COLOR_BG);
    render(&model,0);
    ui_flush();
    render_overlay();
}
""", encoding="ascii")
    (work / "host.c").write_text(
        """#include <stddef.h>
#include <stdint.h>
#include "ui_screen.h"
#include "gui_shell.h"
#include "app_aquarium.h"
#include "app_pomodoro.h"
#include "rgb565.h"
__declspec(dllimport) void *calloc(size_t,size_t);
__declspec(dllimport) void free(void *);
static uint16_t screen[320*240];
static const uint8_t *font;
static int allocations, errors;
int _fltused;
void host_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
const char *esp_err_to_name(int err) {(void)err;return "host error";}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {
    if(caps!=3) errors++;
    void *p=calloc(n,size); if(p) allocations++; return p;
}
void heap_caps_free(void *p) {if(p) allocations--;free(p);}
uint32_t esp_random(void) {static uint32_t n=1;n=n*1664525+1013904223;return n;}
void host_font(const uint8_t *p) {font=p;}
const uint8_t *font_zh16_bitmap(uint32_t cp) {return cp<=65535?font+cp*32:NULL;}
esp_err_t st7789_init(void) {return ESP_OK;}
esp_err_t st7789_show_indexed4_region(const uint8_t *fb,int fw,int x,int y,
        int w,int h,const uint16_t *palette) {
    for(int row=y;row<y+h;++row) for(int col=x;col<x+w;++col) {
        uint8_t packed=fb[(row*fw+col)/2];
        screen[row*320+col]=(uint16_t)~palette[col&1?packed&15:packed>>4];
    }
    return ESP_OK;
}
esp_err_t st7789_show_indexed4(const uint8_t *fb,int w,int h,const uint16_t *p) {
    return st7789_show_indexed4_region(fb,w,0,0,w,h,p);
}
esp_err_t st7789_draw_rgb565_bitmap(int x,int y,int w,int h,const uint16_t *px,
        const uint8_t *a,uint16_t bg) {
    for(int row=0;row<h;++row) for(int col=0;col<w;++col) {
        screen[(y+row)*320+x+col]=rgb565_blend(px[row*w+col],bg,a?a[row*w+col]:255);
    }
    return ESP_OK;
}
esp_err_t st7789_draw_rgb565_sprite(int x,int y,int w,int h,const uint16_t *px,
        const uint8_t *a,const uint8_t *fb,const uint16_t *pal,bool mirror) {
    for(int row=0;row<h;++row) for(int col=0;col<w;++col) {
        int p=row*w+(mirror?w-1-col:col),sx=x+col,sy=y+row;
        uint8_t packed=fb[(sy*320+sx)/2];
        uint16_t bg=(uint16_t)~pal[sx&1?packed&15:packed>>4];
        screen[sy*320+sx]=rgb565_blend(px[p],bg,a?a[p]:255);
    }
    return ESP_OK;
}
const uint16_t *host_screen(void) {return screen;}
int host_allocations(void) {return allocations;}
int host_errors(void) {return errors;}
int host_launcher_test(void) {
    static const gui_app_t dummy={.label="App",.id="dummy"};
    const gui_app_t *apps[]={&dummy,&dummy,&dummy,&dummy,&dummy,
        app_aquarium_descriptor(),app_pomodoro_descriptor()};
    gui_shell_init(apps,7,0);
    gui_shell_show_launcher();
    gui_shell_handle_input(GUI_INPUT_DOWN);
    gui_shell_handle_input(GUI_INPUT_DOWN);
    gui_model_t model={.time_valid=true,.time="10:08:30"};
    gui_shell_render(&model);
    gui_shell_handle_input(GUI_INPUT_ACTIVATE);
    return gui_shell_current_app_is("pomodoro");
}
""", encoding="ascii")
    (work / "crt.def").write_text(
        "LIBRARY msvcrt.dll\nEXPORTS\ncalloc\nfree\nmemcpy\nmemset\nmemcmp\n"
        "strcmp\nstrlen\n_snprintf\nsscanf\nsqrt\nsin\ncos\n", encoding="ascii")
    subprocess.run([str(clang.with_name("llvm-dlltool.exe")), "-m", "i386:x86-64",
                    "-d", str(work / "crt.def"), "-l", str(work / "crt.lib")],
                   check=True, capture_output=True)
    exports = ("host_school_test", "host_timer_test", "host_launcher_test",
               "host_school_preview", "host_timer_preview", "host_screen",
               "host_font", "host_allocations", "host_errors", "ui_init")
    output = work / "small_apps.dll"
    subprocess.run(
        [str(clang), "--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
         "-shared", "-nostdlib", "-ffreestanding", "-fno-stack-protector",
         "-Wall", "-Wextra", "-Werror", "-I", str(work), "-I", str(MAIN),
         str(work / "host.c"), str(work / "input_host.c"),
         str(work / "aquarium_test.c"), str(work / "pomodoro_test.c"),
         str(MAIN / "ui_screen.c"), str(MAIN / "gui_shell.c"),
         str(MAIN / "font5x7.c"), str(work / "crt.lib"), "-Wl,/noentry",
         *[f"-Wl,/export:{name}" for name in exports], "-o", str(output)],
        check=True, capture_output=True)
    return ctypes.CDLL(str(output))


class AppTests(unittest.TestCase):
    lib = None

    def test_exact_timer_boundaries_pause_sleep_and_long_delays(self):
        self.assertEqual(self.lib.host_timer_test(), 1)

    def test_fish_count_bounds_speed_and_psram_cleanup(self):
        before = self.lib.host_allocations()
        self.assertEqual(self.lib.host_school_test(), 1)
        self.assertEqual(self.lib.host_allocations(), before)
        self.assertEqual(self.lib.host_errors(), 0)

    def test_seventh_launcher_app_can_be_opened(self):
        self.assertEqual(self.lib.host_launcher_test(), 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        try:
            lib = compile_apps(args.clang, Path(temp))
        except subprocess.CalledProcessError as error:
            print(error.stderr.decode(errors="replace"))
            raise
        font = ctypes.create_string_buffer((MAIN / "assets" / "unifont_bmp16.bin").read_bytes())
        lib.host_font.argtypes = [ctypes.c_void_p]
        lib.host_font(font)
        if lib.ui_init() != 0:
            raise RuntimeError("UI initialization failed")
        AppTests.lib = lib
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(AppTests))
        if args.preview_dir:
            from PIL import Image
            args.preview_dir.mkdir(parents=True, exist_ok=True)
            lib.host_screen.restype = ctypes.POINTER(ctypes.c_uint16)
            for name in ("school", "timer"):
                getattr(lib, f"host_{name}_preview")()
                pixels = lib.host_screen()
                rgb = bytearray()
                for i in range(320 * 240):
                    c = pixels[i]
                    rgb.extend((((c >> 11) & 31) * 255 // 31,
                                ((c >> 5) & 63) * 255 // 63,
                                (c & 31) * 255 // 31))
                Image.frombytes("RGB", (320, 240), bytes(rgb)).save(
                    args.preview_dir / f"{name}.png")
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(lib._handle)
        if not result.wasSuccessful():
            raise SystemExit(1)


if __name__ == "__main__":
    main()
