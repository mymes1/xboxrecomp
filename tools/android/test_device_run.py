"""Tests for the log parsers of tools/android/device_run.py. [android-port]"""

import unittest

from tools.android import device_run


LOG = (
    "[TOUCH] controls hidden (mode auto)\n"
    "[TOUCH] controls shown (mode on)\n"
    "[TOUCH] layout 640x320 LS=68,252,46 RS=477,265,37 DPAD=68,151,37 A=569,276,17 "
    "B=599,246,17 X=539,246,17 Y=569,216,17 BK=603,176,14 WH=535,176,14 LT=52,27,41 "
    "RT=588,27,41 BACK=284,25,19 START=356,25,19\n"
    "[INPUT] port 0: not connected\n"
    "[INPUT] port 0: buttons=0x0000 A=255 B=0 X=0 Y=0 BK=0 WH=0 LT=0 RT=0 L=(0,0) R=(0,0)\n"
    "[INPUT] port 0: buttons=0x0000 A=0 B=0 X=0 Y=0 BK=0 WH=0 LT=0 RT=0 L=(23170,23170)"
    " R=(-1,0)\n"
    "[INPUT] port 0: buttons=0x0010 A=0 B=0 X=0 Y=0 BK=0 WH=0 LT=0 RT=255 L=(0,-32767) R=(0,0)\n"
)


class DeviceRunParsersTest(unittest.TestCase):
    def test_touch_layout(self):
        ctl = device_run.parse_touch_layout(LOG)
        self.assertEqual(ctl["LS"], (68, 252, 46))
        self.assertEqual(ctl["START"], (356, 25, 19))
        self.assertEqual(len(ctl), 13)
        self.assertEqual(device_run.parse_touch_layout("nothing here"), {})

    def test_inputs(self):
        st = device_run.parse_inputs(LOG)
        self.assertEqual(len(st), 3)            # "not connected" is not a state
        self.assertEqual(st[0]["A"], 255)
        self.assertEqual(st[1]["L"], (23170, 23170))
        self.assertEqual(st[1]["R"], (-1, 0))
        self.assertTrue(st[2]["buttons"] & 0x0010)
        self.assertEqual(st[2]["RT"], 255)
        self.assertEqual(st[2]["L"], (0, -32767))

    def test_files_lines(self):
        ok = device_run.parse_files_lines(
            "[BOOT] something\n"
            "[FILES] importing the game files (0.00 GB, 3 files) from /sdcard/Android/obb/p/game.zip\n"
            "[FILES] imported 3 files, 24601 bytes from /sdcard/Android/obb/p/game.zip (at the"
            " zip's root), each matching its SHA-256, in 0.2s; 0 other zip entries ignored\n"
            "[FILES] removed /sdcard/Android/obb/p/game.zip (imported)\n")
        self.assertTrue(ok["ok"])
        self.assertIn("24601 bytes", ok["imported"])
        self.assertEqual(len(ok["lines"]), 3)
        bad = device_run.parse_files_lines(
            "[FILES] importing the game files (0.00 GB, 3 files) from x\n"
            "[FILES] Importing the game files failed: default.xbe in the zip is not the file"
            " this app was built from (SHA-256 0123..., expected 4567...)\n")
        self.assertFalse(bad["ok"])
        self.assertIn("default.xbe", bad["problems"][0])
        need = device_run.parse_files_lines("[FILES] game files needed: not in the APK, and no"
                                            " game.zip in /sdcard/Android/obb/p\n")
        self.assertFalse(need["ok"])
        self.assertEqual(device_run.parse_files_lines("")["imported"], None)

    def test_game_zip_is_sent_over_adb_content_write(self):
        import os
        import tempfile
        from unittest import mock

        class FakeAdb:
            def __init__(self, provider_ok):
                self.calls, self.fed, self.provider_ok = [], [], provider_ok

            def cmd(self, *args, **kw):
                self.calls.append(args)
                return "x.zip: 1 file pushed, 0 skipped. 1.0 MB/s" if args[0] == "push" else ""

            def feed(self, path, *args, **kw):
                self.calls.append(args)
                with open(path, "rb") as f:
                    self.fed.append(f.read())
                if self.provider_ok:
                    return {"rc": 0, "output": ""}
                return {"rc": 1, "output": "[ERROR] Unable to find provider info for p.gamezip"}

        with tempfile.NamedTemporaryFile(suffix=".zip") as z:
            z.write(b"PK\x05\x06" + b"\0" * 18)
            z.flush()
            adb = FakeAdb(True)
            out = device_run._place_game_zip(adb, "p", z.name, log=lambda *a: None)
            self.assertEqual(out["size"], os.path.getsize(z.name))
            self.assertEqual(adb.fed, [b"PK\x05\x06" + b"\0" * 18])     # byte for byte
            # An older system without the provider route: the OBB folder.
            old = FakeAdb(False)
            with mock.patch.object(device_run.time, "sleep"):
                out2 = device_run._place_game_zip(old, "p", z.name, log=lambda *a: None)
        self.assertEqual(out["sent_to"], "content://p.gamezip/game.zip")
        self.assertEqual(adb.calls, [("shell", "pm", "clear", "p"),
                                     ("exec-in", "content", "write", "--uri",
                                      "content://p.gamezip/game.zip")])
        self.assertEqual(out2["sent_to"], "/sdcard/Android/obb/p/game.zip")
        self.assertEqual([t["ok"] for t in out2["tries"]], [False, True])
        self.assertEqual([c[0] for c in old.calls], ["shell", "exec-in", "shell", "push"])

    def test_report_fails_when_a_requested_import_failed(self):
        import json
        import os
        import shutil
        import tempfile
        from tools.android import ci_report
        d = tempfile.mkdtemp(prefix="xbrci")
        try:
            os.makedirs(os.path.join(d, "reports"))
            with open(os.path.join(d, "pipeline_state.json"), "w") as f:
                json.dump({"stages": {"apk": {"status": "ok"}, "verify": {"status": "ok"}}}, f)
            rt = {"platform": "android-arm64-v8a", "files_import": {"ok": False, "problems": ["x"]}}
            with open(os.path.join(d, "reports", "runtime_test.json"), "w") as f:
                json.dump(rt, f)
            text, ok = ci_report.summarize(d, "t")
            self.assertFalse(ok)
            self.assertIn('"files_import"', text)
            rt["files_import"]["ok"] = True
            with open(os.path.join(d, "reports", "runtime_test.json"), "w") as f:
                json.dump(rt, f)
            self.assertTrue(ci_report.summarize(d, "t")[1])
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
