"""Compile the production audio driver with DMA-timing mocks on Windows.

python tools\\test_audio_startup.py --clang <ESP-Clang bin\\clang.exe>
"""

import argparse
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_idle_power import MAIN, prepare_host_environment


def compile_audio_library(clang, work):
    prepare_host_environment(clang, work)
    with (work / "esp_err.h").open("a", encoding="ascii") as header:
        header.write("#define ESP_ERR_NO_MEM 0x101\n")
    (work / "esp_heap_caps.h").write_text(
        "#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n"
        "#define MALLOC_CAP_8BIT 2\n"
        "void *heap_caps_malloc(size_t, unsigned);\n"
        "void heap_caps_free(void *);\n",
        encoding="ascii",
    )
    (work / "esp_log.h").write_text(
        "void host_log(const char *, const char *, ...);\n"
        "const char *esp_err_to_name(int);\n"
        "#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)\n"
        "#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)\n"
        "#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)\n",
        encoding="ascii",
    )
    (work / "string.h").write_text(
        "#include <stddef.h>\n"
        "__declspec(dllimport) void *memset(void *, int, size_t);\n"
        "__declspec(dllimport) void *memcpy(void *, const void *, size_t);\n",
        encoding="ascii",
    )
    (work / "driver" / "gpio.h").write_text(
        '#include "input_host.h"\n'
        '#define GPIO_MODE_OUTPUT 2\n#define GPIO_PULLUP_DISABLE 0\n'
        '#define GPIO_PULLDOWN_ENABLE 1\n'
        'esp_err_t gpio_set_level(int pin, int level);\n',
        encoding="ascii",
    )
    (work / "freertos").mkdir()
    (work / "freertos" / "FreeRTOS.h").write_text(
        """#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define configMAX_PRIORITIES 25
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
void vTaskDelay(uint32_t ticks);
int xPortGetCoreID(void);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
    uint32_t stack, void *arg, int priority, TaskHandle_t *handle, int core);
""",
        encoding="ascii",
    )
    (work / "freertos" / "task.h").write_text(
        '#include "FreeRTOS.h"\n', encoding="ascii")
    (work / "driver" / "i2s_std.h").write_text(
        """#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2s_chan_handle_t;
typedef struct { int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct { uint32_t sample_rate; } host_clock_t;
typedef struct { int bits, channels, slot_mask; } host_slot_t;
typedef struct { int mclk, bclk, ws, dout, din; } host_gpio_t;
typedef struct {
    host_clock_t clk_cfg; host_slot_t slot_cfg; host_gpio_t gpio_cfg;
} i2s_std_config_t;
#define I2S_NUM_0 0
#define I2S_NUM_1 1
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_MONO 1
#define I2S_SLOT_MODE_STEREO 2
#define I2S_STD_SLOT_LEFT 1
#define I2S_GPIO_UNUSED (-1)
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((host_clock_t){rate})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, mode) ((host_slot_t){width, mode, 0})
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t, const void *, size_t, size_t *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
""",
        encoding="ascii",
    )
    (work / "audio_host.c").write_text(
        """#include "audio_io.c"
static uint8_t speaker_buffer[APP_SPEAKER_BUFFER_BYTES];
void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps; return size <= sizeof(speaker_buffer) ? speaker_buffer : NULL;
}
void heap_caps_free(void *buffer) { (void)buffer; }
static unsigned rate, frames;
static int record_once;
static int injected_failure, enabled, preloads, writes, deletes, enables;
static int invalid_order;
void host_advance_ms(int ms);
void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(int err) { (void)err; return "mock error"; }
void mem_log(const char *tag) { (void)tag; }
void vTaskDelay(uint32_t ticks) { host_advance_ms((int)ticks); }
int xPortGetCoreID(void) { return 1; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
        uint32_t stack, void *arg, int priority, TaskHandle_t *handle, int core) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    *handle = (void *)1; return pdPASS;
}
esp_err_t gpio_set_level(int pin, int level) { (void)pin; (void)level; return ESP_OK; }
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg,
                          i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    frames = cfg->dma_frame_num;
    if (tx) *tx = (void *)1;
    if (rx) *rx = (void *)2;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg) {
    (void)h; rate = cfg->clk_cfg.sample_rate; return ESP_OK;
}
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t h, const void *data,
                                   size_t size, size_t *loaded) {
    (void)h; ++preloads;
    if (enabled) { invalid_order = 1; return ESP_ERR_INVALID_STATE; }
    for (size_t i = 0; i < size; ++i) {
        if (((const uint8_t *)data)[i]) { invalid_order = 1; return ESP_FAIL; }
    }
    *loaded = injected_failure == 2 ? size - 1 : size;
    return injected_failure == 1 ? ESP_FAIL : ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t h) {
    (void)h; ++enables;
    if (injected_failure == 3) return ESP_FAIL;
    enabled = 1; return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t h) {
    (void)h; enabled = 0; return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t h) {
    (void)h; ++deletes; return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *src,
                           size_t size, size_t *written, uint32_t timeout_ms) {
    (void)h; (void)src; ++writes;
    // The first TX descriptor cannot be reused until one DMA period elapsed.
    if (timeout_ms < (frames * 1000 + rate - 1) / rate) {
        *written = 0; return ESP_ERR_TIMEOUT;
    }
    *written = size; return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst,
                          size_t size, size_t *read, uint32_t timeout_ms) {
    (void)h; (void)dst; (void)size; (void)timeout_ms;
    if (record_once) {
        memset(dst, 0, size);
        *read = size;
        record_once = 0;
        s_req_stop = 1;
        return ESP_OK;
    }
    *read = 0; return ESP_ERR_TIMEOUT;
}
int test_install_after_wake(unsigned sample_rate, unsigned channels, int failure) {
    audio_io_init();
    injected_failure = failure;
    enabled = preloads = writes = deletes = enables = invalid_order = 0;
    audio_io_set_suspended(true);
    audio_io_set_suspended(false);
    esp_err_t err = audio_io_acquire_owner(AUDIO_OWNER_VOICE);
    if (err != ESP_OK) return err;
    err = audio_io_prepare_speaker(sample_rate, (uint8_t)channels);
    if (err == ESP_OK) err = install_speaker();
    if (err == ESP_OK) uninstall_speaker();
    audio_io_release_owner(AUDIO_OWNER_VOICE);
    return err;
}
int test_owner_exclusive_during_connect_and_cancel(void) {
    audio_io_init();
    if (audio_io_acquire_owner(AUDIO_OWNER_VOICE) != ESP_OK) return 0;
    if (audio_io_acquire_owner(AUDIO_OWNER_RADIO) == ESP_OK) return 0;
    audio_io_set_suspended(true);
    audio_io_set_suspended(false);
    if (audio_io_acquire_owner(AUDIO_OWNER_CHIME) == ESP_OK) return 0;
    s_phase = AUDIO_PHASE_STARTING;
    audio_io_release_owner(AUDIO_OWNER_VOICE);
    if (audio_io_acquire_owner(AUDIO_OWNER_CHIME) == ESP_OK) return 0;
    finish_phase();
    if (audio_io_acquire_owner(AUDIO_OWNER_CHIME) != ESP_OK) return 0;
    audio_io_release_owner(AUDIO_OWNER_CHIME);
    return 1;
}
int test_record_and_play_share_storage(void) {
    audio_io_init();
    if (audio_io_acquire_owner(AUDIO_OWNER_VOICE) != ESP_OK) return 0;
    injected_failure = 0;
    s_req_stop = s_req_abort = 0;
    record_once = 1;
    do_recording();
    uint8_t *storage = s_ring.buf;
    int correct = ring_capacity(&s_ring) == APP_AUDIO_ARENA_BYTES &&
                  ring_available(&s_ring) == 512;
    correct = correct && audio_io_prepare_speaker(44100, 2) == ESP_OK &&
              s_ring.buf == storage && storage == s_audio_arena &&
              ring_capacity(&s_ring) == APP_SPEAKER_BUFFER_BYTES &&
              ring_available(&s_ring) == 0;
    audio_io_release_owner(AUDIO_OWNER_VOICE);
    return correct;
}
int test_preloads(void) { return preloads; }
int test_writes(void) { return writes; }
int test_deletes(void) { return deletes; }
int test_enables(void) { return enables; }
int test_clean(void) { return !enabled && !s_spk_installed && !s_spk_chan && !invalid_order; }
""",
        encoding="ascii",
    )
    exports = ("test_install_after_wake", "test_preloads", "test_writes",
               "test_deletes", "test_enables", "test_clean",
               "test_owner_exclusive_during_connect_and_cancel",
               "test_record_and_play_share_storage")
    output = work / "audio_startup.dll"
    subprocess.run(
        [str(clang), "--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
         "-shared", "-nostdlib", "-ffreestanding", "-fno-stack-protector",
         "-Wall", "-Wextra", "-Werror", "-I", str(work), "-I", str(MAIN),
         str(work / "audio_host.c"), str(work / "input_host.c"),
         str(MAIN / "ring.c"), str(work / "crt.lib"), "-Wl,/noentry",
         *[f"-Wl,/export:{name}" for name in exports], "-o", str(output)],
        check=True, capture_output=True,
    )
    return ctypes.CDLL(str(output))


class AudioStartupTests(unittest.TestCase):
    lib = None

    def test_owner_excludes_other_sessions_before_playback_and_until_teardown(self):
        self.assertEqual(self.lib.test_owner_exclusive_during_connect_and_cancel(), 1)

    def test_record_and_play_use_one_psram_allocation_and_reset_contents(self):
        self.assertEqual(self.lib.test_record_and_play_share_storage(), 1)

    def test_speech_and_radio_start_after_wake(self):
        for rate in (8000, 16000, 22050, 44100, 48000, 96000):
            for channels in (1, 2):
                with self.subTest(rate=rate, channels=channels):
                    self.assertEqual(self.lib.test_install_after_wake(rate, channels, 0), 0)
                    self.assertEqual(self.lib.test_preloads(), 1)
                    self.assertEqual(self.lib.test_enables(), 1)
                    self.assertEqual(self.lib.test_writes(), 0)
                    self.assertEqual(self.lib.test_deletes(), 1)
                    self.assertTrue(self.lib.test_clean())

    def test_preload_failures_never_enable_i2s(self):
        for failure in (1, 2):
            with self.subTest(failure=failure):
                self.assertNotEqual(self.lib.test_install_after_wake(16000, 1, failure), 0)
                self.assertEqual(self.lib.test_enables(), 0)
                self.assertEqual(self.lib.test_deletes(), 1)
                self.assertTrue(self.lib.test_clean())
                self.assertEqual(self.lib.test_install_after_wake(16000, 1, 0), 0)

    def test_enable_failure_cleans_up_and_allows_retry(self):
        self.assertNotEqual(self.lib.test_install_after_wake(16000, 1, 3), 0)
        self.assertEqual(self.lib.test_deletes(), 1)
        self.assertTrue(self.lib.test_clean())
        self.assertEqual(self.lib.test_install_after_wake(16000, 1, 0), 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        try:
            lib = compile_audio_library(args.clang, Path(directory))
        except subprocess.CalledProcessError as error:
            print(error.stdout.decode(errors="replace"))
            print(error.stderr.decode(errors="replace"))
            raise
        AudioStartupTests.lib = lib
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(AudioStartupTests))
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(lib._handle)
        if not result.wasSuccessful():
            raise SystemExit(1)


if __name__ == "__main__":
    main()
