"""Host regression tests for the production C idle policy and SPIFFS settings.

Windows: python tools\\test_idle_power.py --clang <ESP-Clang bin\\clang.exe>
Uses the installed compiler and Windows CRT; no device or extra packages.
All generated headers, libraries and settings live in a temporary directory.
"""

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "esp32_idf_s3n16r8" / "main"
AWAKE, SCREEN_OFF, SLEEP = range(3)
SLEEP_OPTIONS = (0, 5, 10, 30)
SCREEN_OPTIONS = (0, 1, 5, 10)


class Settings(ctypes.Structure):
    _fields_ = [
        ("sleep_minutes", ctypes.c_uint32),
        ("screen_off_minutes", ctypes.c_uint32),
    ]


def compile_library(clang, work):
    (work / "esp_err.h").write_text(
        "typedef int esp_err_t;\n"
        "#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_NOT_FOUND 0x105\n#define ESP_ERR_INVALID_ARG 0x102\n",
        encoding="ascii",
    )
    (work / "esp_log.h").write_text(
        "#define ESP_LOGE(tag, ...) ((void)(tag))\n"
        "#define ESP_LOGW(tag, ...) ((void)(tag))\n",
        encoding="ascii",
    )
    (work / "errno.h").write_text(
        "__declspec(dllimport) int *_errno(void);\n"
        "#define errno (*_errno())\n#define ENOENT 2\n",
        encoding="ascii",
    )
    (work / "stdio.h").write_text(
        "typedef struct host_file FILE;\n"
        "__declspec(dllimport) FILE *fopen(const char *, const char *);\n"
        "__declspec(dllimport) int fclose(FILE *);\n"
        "__declspec(dllimport) int ferror(FILE *);\n"
        "__declspec(dllimport) int fscanf(FILE *, const char *, ...);\n"
        "__declspec(dllimport) int fprintf(FILE *, const char *, ...);\n"
        "__declspec(dllimport) int remove(const char *);\n"
        "__declspec(dllimport) int rename(const char *, const char *);\n",
        encoding="ascii",
    )
    (work / "crt.def").write_text(
        "LIBRARY msvcrt.dll\nEXPORTS\n"
        "fopen\nfclose\nferror\nfscanf\nfprintf\nremove\nrename\n_errno\n",
        encoding="ascii",
    )
    subprocess.run(
        [str(clang.with_name("llvm-dlltool.exe")), "-m", "i386:x86-64",
         "-d", str(work / "crt.def"), "-l", str(work / "crt.lib")],
        check=True, capture_output=True,
    )
    exports = ("idle_power_target", "idle_power_filter_input",
               "power_settings_load", "power_settings_get",
               "power_settings_valid", "power_settings_save")
    output = work / "idle_power.dll"
    subprocess.run(
        [str(clang), "--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
         "-shared", "-nostdlib", "-ffreestanding", "-fno-stack-protector",
         "-Wall", "-Wextra", "-Werror", "-I", str(work),
         '-DAPP_SPIFFS_BASE_PATH="."',
         str(MAIN / "idle_power.c"), str(MAIN / "power_settings.c"),
         str(work / "crt.lib"), "-Wl,/noentry",
         *[f"-Wl,/export:{name}" for name in exports], "-o", str(output)],
        check=True, capture_output=True,
    )
    return ctypes.CDLL(str(output))


class PowerTests(unittest.TestCase):
    lib = None

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.previous_cwd = os.getcwd()
        os.chdir(self.directory.name)
        self.assertEqual(self.lib.power_settings_load(), 0)

    def tearDown(self):
        os.chdir(self.previous_cwd)
        self.directory.cleanup()

    def values(self):
        settings = self.lib.power_settings_get().contents
        return settings.sleep_minutes, settings.screen_off_minutes

    def save(self, sleep, screen):
        return self.lib.power_settings_save(ctypes.byref(Settings(sleep, screen)))

    def test_all_deadlines_and_priority(self):
        for sleep in SLEEP_OPTIONS:
            for screen in SCREEN_OPTIONS:
                deadlines = {0, 1, 60_000, 300_000, 600_000, 1_800_000}
                for deadline in tuple(deadlines):
                    deadlines.update((deadline - 1, deadline + 1))
                for idle in sorted(deadlines):
                    expected = (
                        SLEEP if sleep and idle >= sleep * 60_000 else
                        SCREEN_OFF if screen and idle >= screen * 60_000 else AWAKE
                    )
                    with self.subTest(sleep=sleep, screen=screen, idle=idle):
                        self.assertEqual(
                            self.lib.idle_power_target(AWAKE, idle, sleep, screen),
                            expected,
                        )

    def test_screen_off_does_not_restart_sleep_deadline(self):
        self.assertEqual(self.lib.idle_power_target(AWAKE, 60_000, 5, 1), SCREEN_OFF)
        self.assertEqual(
            self.lib.idle_power_target(SCREEN_OFF, 299_999, 5, 1), SCREEN_OFF)
        self.assertEqual(self.lib.idle_power_target(SCREEN_OFF, 300_000, 5, 1), SLEEP)

    def test_states_do_not_wake_without_input(self):
        self.assertEqual(self.lib.idle_power_target(SLEEP, 0, 0, 0), SLEEP)
        self.assertEqual(self.lib.idle_power_target(SCREEN_OFF, 0, 0, 0), SCREEN_OFF)
        self.assertEqual(self.lib.idle_power_target(AWAKE, 0, 5, 1), AWAKE)
        self.assertEqual(self.lib.idle_power_target(AWAKE, 2**40, 0, 0), AWAKE)

    def test_entire_wake_gesture_is_consumed(self):
        ignore, dispatch, wake = range(3)
        for state in (SCREEN_OFF, SLEEP):
            for event, held in ((True, True), (True, False), (False, True)):
                with self.subTest(state=state, event=event, held=held):
                    consume = ctypes.c_bool(False)

                    def poll(current, ready, active):
                        return self.lib.idle_power_filter_input(
                            current, ready, active, ctypes.byref(consume))

                    self.assertEqual(poll(state, False, False), ignore)
                    self.assertEqual(poll(state, event, held), wake)
                    self.assertTrue(consume.value)
                    # Holding, axis repeat and button release cannot activate.
                    self.assertEqual(poll(AWAKE, False, True), ignore)
                    self.assertEqual(poll(AWAKE, True, True), ignore)
                    self.assertEqual(poll(AWAKE, True, False), ignore)
                    self.assertTrue(consume.value)
                    self.assertEqual(poll(AWAKE, False, False), ignore)
                    self.assertFalse(consume.value)
                    self.assertEqual(poll(AWAKE, True, True), dispatch)
                    self.assertEqual(poll(AWAKE, True, False), dispatch)

    def test_defaults_and_every_selection_survive_reload(self):
        self.assertEqual(self.values(), (0, 0))
        for sleep in SLEEP_OPTIONS:
            for screen in SCREEN_OPTIONS:
                with self.subTest(sleep=sleep, screen=screen):
                    self.assertEqual(self.save(sleep, screen), 0)
                    self.assertEqual(self.lib.power_settings_load(), 0)
                    self.assertEqual(self.values(), (sleep, screen))
                    self.assertEqual(
                        Path("power_settings.txt").read_text(),
                        f"power_v1 {sleep} {screen}\n",
                    )

    def test_invalid_selections_are_not_saved(self):
        self.assertEqual(self.save(10, 5), 0)
        for sleep, screen in ((1, 5), (5, 30), (0xFFFFFFFF, 0), (0, 2)):
            self.assertNotEqual(self.save(sleep, screen), 0)
            self.assertEqual(self.values(), (10, 5))
        self.assertNotEqual(self.lib.power_settings_save(None), 0)
        self.assertEqual(self.lib.power_settings_load(), 0)
        self.assertEqual(self.values(), (10, 5))

    def test_corrupt_settings_report_error_and_disable_timers(self):
        for content in ("", "power_v2 5 1\n", "power_v1 5\n",
                        "power_v1 5 30\n", "power_v1 5 1 junk\n",
                        "power_v1 -1 1\n", "power_v1 1 1\n"):
            with self.subTest(content=content):
                Path("power_settings.txt").write_text(content)
                self.assertNotEqual(self.lib.power_settings_load(), 0)
                self.assertEqual(self.values(), (0, 0))

    def test_interrupted_replace_recovers_backup(self):
        for primary in (None, "corrupt"):
            with self.subTest(primary=primary):
                Path("power_settings.txt").unlink(missing_ok=True)
                if primary:
                    Path("power_settings.txt").write_text(primary)
                Path("power_settings.bak").write_text("power_v1 30 10\n")
                Path("power_settings.tmp").write_text("power_v1 5")
                self.assertEqual(self.lib.power_settings_load(), 0)
                self.assertEqual(self.values(), (30, 10))
                self.assertTrue(Path("power_settings.txt").is_file())
                self.assertEqual(self.save(5, 1), 0)
                self.assertEqual(self.lib.power_settings_load(), 0)
                self.assertEqual(self.values(), (5, 1))

    def test_committed_primary_wins_over_old_backup(self):
        Path("power_settings.txt").write_text("power_v1 10 5\n")
        Path("power_settings.bak").write_text("power_v1 30 10\n")
        self.assertEqual(self.lib.power_settings_load(), 0)
        self.assertEqual(self.values(), (10, 5))

    def test_write_failure_preserves_previous_selection(self):
        self.assertEqual(self.save(30, 10), 0)
        Path("power_settings.tmp").mkdir()
        self.assertNotEqual(self.save(5, 1), 0)
        self.assertEqual(self.values(), (30, 10))
        self.assertEqual(self.lib.power_settings_load(), 0)
        self.assertEqual(self.values(), (30, 10))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        try:
            lib = compile_library(args.clang, Path(directory))
        except subprocess.CalledProcessError as error:
            print(error.stdout.decode(errors="replace"))
            print(error.stderr.decode(errors="replace"))
            raise
        lib.idle_power_target.argtypes = [
            ctypes.c_int, ctypes.c_int64, ctypes.c_uint32, ctypes.c_uint32]
        lib.idle_power_filter_input.argtypes = [
            ctypes.c_int, ctypes.c_bool, ctypes.c_bool, ctypes.POINTER(ctypes.c_bool)]
        lib.power_settings_get.restype = ctypes.POINTER(Settings)
        lib.power_settings_save.argtypes = [ctypes.POINTER(Settings)]
        PowerTests.lib = lib
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(PowerTests))
        # Windows cannot delete a DLL while it is loaded.
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(lib._handle)
        if not result.wasSuccessful():
            raise SystemExit(1)


if __name__ == "__main__":
    main()
