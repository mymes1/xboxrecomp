"""Tests for the project generator, the native-build checks and the evidence
rules of the compatibility report.

    python3 -m pytest tools/android/test_native.py -q
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.android import compat, elfinfo, native, project    # noqa: E402

XBE_REPORT = {"title": 'Quote "and" back\\slash \u00e9', "title_id": "4d530004",
              "entry_point": "0x00011120", "problems": [], "warnings": []}


def _zig():
    try:
        import ziglang  # noqa: F401
        return True
    except ImportError:
        return False


class ProjectTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="xbrproj")
        gen = os.path.join(self.work, "project", "native", "gen")
        os.makedirs(gen)
        with open(os.path.join(gen, "recomp_0000.c"), "w") as f:
            f.write("void xbe_entry_point(void) {}\n")

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def test_generate_writes_config_and_cmake(self):
        cfg = project.generate(self.work, XBE_REPORT, log=lambda *_: None)
        self.assertEqual(cfg["package"], "org.xboxrecomp.t4d530004")
        self.assertEqual(cfg["title_id"], "4D530004")
        native_dir = os.path.join(self.work, "project", "native")
        self.assertTrue(os.path.exists(os.path.join(native_dir, "CMakeLists.txt")))
        with open(os.path.join(native_dir, "game", "xbr_game_config.c")) as f:
            c = f.read()
        # The title is escaped, not pasted: quotes, backslash, UTF-8 as octal.
        self.assertIn('"Quote \\"and\\" back\\\\slash \\303\\251"', c)
        self.assertIn("0x4D530004u", c)
        self.assertIn("0x00011120u", c)
        self.assertIn("64u", c)

    def test_saved_edits_survive_and_identity_follows_the_disc(self):
        project.generate(self.work, XBE_REPORT, log=lambda *_: None)
        saved = os.path.join(self.work, "project", "xbr_game.json")
        with open(saved) as f:
            cfg = json.load(f)
        cfg["total_ram_mb"] = 128
        cfg["env"] = {"RECOMP_GUEST_CPUS": "one"}
        cfg["title"] = "edited title is ignored"
        with open(saved, "w") as f:
            json.dump(cfg, f)
        cfg2 = project.generate(self.work, XBE_REPORT, log=lambda *_: None)
        self.assertEqual(cfg2["total_ram_mb"], 128)
        self.assertEqual(cfg2["env"], {"RECOMP_GUEST_CPUS": "one"})
        self.assertEqual(cfg2["title"], XBE_REPORT["title"])
        with open(os.path.join(self.work, "project", "native", "game", "xbr_game_config.c")) as f:
            c = f.read()
        self.assertIn('"RECOMP_GUEST_CPUS=one",', c)
        self.assertIn("128u", c)

    def test_invalid_settings_are_refused(self):
        for bad in ({"total_ram_mb": 96}, {"env": {"lower": "1"}}, {"env": {"A": 1}},
                    {"android": {"min_sdk": 21}}, {"android": {"abi": "x86_64"}},
                    {"package": "Not.Valid"}):
            cfg = project._merge(project.DEFAULTS, {"title": "t", "title_id": "00000000",
                                                    "package": "org.x.t"})
            cfg = project._merge(cfg, bad)
            with self.assertRaises(project.ProjectError, msg=repr(bad)):
                project.validate(cfg)

    def test_missing_generated_code_is_an_error(self):
        os.remove(os.path.join(self.work, "project", "native", "gen", "recomp_0000.c"))
        with self.assertRaises(project.ProjectError):
            project.generate(self.work, XBE_REPORT, log=lambda *_: None)


BOOT_LOG = """\
[BOOT] xboxrecomp POSIX host
[BOOT] RECOMP_GUEST_LOCK=1 (default)
[BOOT] memory layout
[BOOT] entering xbe_entry_point (guest 0x00011120), esp=0x00F7FFF0
XBRTEST TIMER PASS slept_ms=20
XBRTEST DONE pass=7 fail=1
[UNIMPL] untranslated instruction REACHED: `fxsave [eax]` at 0x00012345 (a no-op)
[STATUS] t=2s flips=0 draws=0 display=0x00000000 pitch=0 fbsum=00000000 fbchanges=0
[STATUS] t=4s flips=12 draws=340 display=0x83E00000 pitch=2560 fbsum=1A2B3C4D fbchanges=1
[CRASH] signal 11 at host pc 0x1234, fault address 0x10 (boot stage: running title)
"""


class BootLogTest(unittest.TestCase):
    def test_parse(self):
        r = native.parse_boot_log(BOOT_LOG)
        self.assertTrue(r["entry_reached"])
        self.assertEqual(r["flips"], 12)
        self.assertEqual(r["display_changes"], 1)
        self.assertTrue(r["display_set"])
        self.assertEqual(r["selftest"], {"pass": 7, "fail": 1})
        self.assertEqual(len(r["crash"]), 1)
        self.assertEqual(len(r["unimpl"]), 1)
        self.assertEqual(r["status"][1]["display"], "0x83E00000")

    def test_present_and_lifecycle_lines(self):
        r = native.parse_boot_log(
            "[BOOT] present: vulkan (swapchain 2400x1080 B8G8R8A8_UNORM, 3 images, FIFO (vsync),"
            " letterboxed linear)\n"
            "[HOST] background: presenter suspended, guest held at its next flip\n"
            "[HOST] foreground: presenter resumed, guest released\n")
        self.assertEqual(r["present"]["backend"], "vulkan")
        self.assertTrue(r["present"]["detail"].startswith("swapchain 2400x1080"))
        self.assertTrue(r["present"]["detail"].endswith("letterboxed linear"))
        self.assertEqual(r["lifecycle"], ["background", "foreground"])
        # The presenter can stop mid-run: the last line wins.
        r = native.parse_boot_log("[BOOT] present: vulkan (swapchain 1x1)\n"
                                  "[BOOT] present: sdl (the Vulkan presenter stopped: off: x)\n")
        self.assertEqual(r["present"]["backend"], "sdl")
        self.assertIn("stopped", r["present"]["detail"])
        self.assertNotIn("present", native.parse_boot_log(BOOT_LOG))

    def test_usb_line(self):
        r = native.parse_boot_log("[BOOT] usb: OHCI model on (a controller on port 1 of the"
                                  " first host controller)\n")
        self.assertTrue(r["usb"].startswith("OHCI model on"))
        self.assertNotIn("usb", native.parse_boot_log(BOOT_LOG))

    def test_audio_lines(self):
        r = native.parse_boot_log(
            "[SDL2] audio initialized: driver aaudio (48000 Hz stereo 16-bit, 4 x 512-sample"
            " buffers, 42.7 ms queued max)\n")
        self.assertTrue(r["audio"]["ok"])
        self.assertTrue(r["audio"]["detail"].startswith("driver aaudio"))
        r = native.parse_boot_log("[SDL2] SDL_OpenAudioDevice failed: no audio device\n")
        self.assertFalse(r["audio"]["ok"])
        self.assertIn("no audio device", r["audio"]["detail"])
        self.assertNotIn("audio", native.parse_boot_log(BOOT_LOG))

    def test_render_line(self):
        r = native.parse_boot_log(
            "[BOOT] render: vulkan (Vulkan 1.3.0 on Mali-G57 MC2 (integrated GPU), depth D24S8)\n")
        self.assertEqual(r["render"]["backend"], "vulkan")
        self.assertTrue(r["render"]["detail"].startswith("Vulkan 1.3.0 on Mali-G57"))
        r = native.parse_boot_log("[BOOT] render: cpu (Vulkan off: no Vulkan loader (x))\n")
        self.assertEqual(r["render"]["backend"], "cpu")
        self.assertIn("no Vulkan loader", r["render"]["detail"])
        self.assertNotIn("render", native.parse_boot_log(BOOT_LOG))

    def test_failed_boot(self):
        r = native.parse_boot_log("[BOOT] read XBE\n[BOOT] FAILED: cannot read an XBE at x\n")
        self.assertFalse(r["entry_reached"])
        self.assertTrue(r["failed_step"].startswith("FAILED"))
        self.assertEqual(r["flips"], 0)


@unittest.skipUnless(_zig(), "ziglang (requirements-dev.txt) not installed")
class ElfInfoTest(unittest.TestCase):
    def test_aarch64_shared_object(self):
        d = tempfile.mkdtemp(prefix="xbrelf")
        try:
            src = os.path.join(d, "t.c")
            with open(src, "w") as f:
                f.write("__thread int tls_var;\n"
                        "int xbe_entry_point(void) { return tls_var; }\n"
                        "int sub_00011000(void) { return 1; }\n"
                        "extern int undefined_here(void);\n"
                        "int calls(void) { return undefined_here(); }\n")
            so = os.path.join(d, "libt.so")
            subprocess.run([sys.executable, "-m", "ziglang", "cc", "-target", "aarch64-linux-musl",
                            "-shared", "-fPIC", "-Wl,--allow-shlib-undefined", "-o", so, src],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            s = elfinfo.Elf(so).summary(["xbe_entry_point", "SDL_main"])
            self.assertEqual(s["machine"], "aarch64")
            self.assertEqual(s["class"], "ELF64")
            self.assertEqual(s["type"], "shared")
            self.assertTrue(s["tls"].startswith("elf"))
            self.assertEqual(s["guest_functions"], 2)
            self.assertIn("undefined_here", s["undefined"])
            self.assertEqual(s["symbols_present"], {"xbe_entry_point": True, "SDL_main": False})
            self.assertFalse(s["exec_stack"])
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_not_elf(self):
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(b"MZ" + b"\0" * 100)
            f.flush()
            with self.assertRaises(elfinfo.ElfError):
                elfinfo.Elf(f.name)


class EvidenceRulesTest(unittest.TestCase):
    """Only Android evidence raises the level; host and compile-check never do."""

    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="xbrcompat")
        self.rep = os.path.join(self.work, "reports")
        os.makedirs(self.rep)
        self._w("xbe_analysis.json", XBE_REPORT)
        self._w("recomp_summary.json", {"total": 6, "translated": 6, "failed": 0})

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def _w(self, name, obj):
        with open(os.path.join(self.rep, name), "w") as f:
            json.dump(obj, f)

    def test_host_run_and_compile_check_do_not_count(self):
        self._w("native_check.json", {"ok": True, "mode": "compile-check", "objects": 60,
                                      "target": "aarch64-linux-android26", "machines": ["aarch64"]})
        self._w("host_build.json", {"ok": True, "platform": "linux-aarch64"})
        self._w("host_runtime.json", {"platform": "linux-aarch64", "entry_reached": True,
                                      "flips": 100, "display_changes": 9, "crash": []})
        r = compat.build(self.work)
        self.assertEqual(r["level"], "Recompiled")
        self.assertTrue(r["host"]["runtime"]["entry_reached"])
        md = compat.to_markdown(r)
        self.assertIn("not Android evidence", md)
        self.assertIn("does not count as Compiled", md)

    def test_android_levels(self):
        self._w("native_build.json", {"ok": True, "target": "android-arm64-v8a", "libraries": []})
        self.assertEqual(compat.build(self.work)["level"], "Compiled")
        self._w("runtime_test.json", {"platform": "android-arm64-v8a", "entry_reached": True,
                                      "flips": 0, "display_changes": 0, "crash": []})
        self.assertEqual(compat.build(self.work)["level"], "Runtime initialized")
        self._w("runtime_test.json", {"platform": "android-arm64-v8a", "entry_reached": True,
                                      "flips": 5, "display_changes": 0, "crash": []})
        self.assertEqual(compat.build(self.work)["level"], "Game booted")
        self._w("runtime_test.json", {"platform": "android-arm64-v8a", "entry_reached": True,
                                      "flips": 5, "crash": ["[CRASH] signal 11"]})
        self.assertEqual(compat.build(self.work)["level"], "Runtime initialized")

    def test_device_run_without_a_build_claims_nothing_more(self):
        self._w("runtime_test.json", {"platform": "android-arm64-v8a", "entry_reached": True,
                                      "flips": 5, "crash": []})
        self.assertEqual(compat.build(self.work)["level"], "Recompiled")

    def test_failed_or_foreign_native_build_is_not_compiled(self):
        self._w("native_build.json", {"ok": False, "target": "android-arm64-v8a"})
        self.assertEqual(compat.build(self.work)["level"], "Recompiled")
        self._w("native_build.json", {"ok": True, "target": "linux-x86_64"})
        self.assertEqual(compat.build(self.work)["level"], "Recompiled")


if __name__ == "__main__":
    unittest.main()
