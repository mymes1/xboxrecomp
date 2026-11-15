"""Tests for tools/android/screen_check.py (synthetic screens). [android-port]"""

import struct
import unittest

from tools.android import screen_check as sc


class ScreenCheckTest(unittest.TestCase):
    def test_letterboxed_landscape(self):
        r = sc.check(sc.synth(320, 180, phase=3))
        self.assertTrue(r["ok"], r)
        x0, y0, pw, ph = r["picture"]
        self.assertAlmostEqual(pw / ph, 4 / 3, delta=0.05)
        self.assertGreaterEqual(r["letterbox_samples"], 2)
        self.assertEqual(r["bars"][0], 3)

    def test_every_phase_and_bgra_and_old_header(self):
        for phase in range(8):
            r = sc.check(sc.synth(200, 120, phase=phase, fmt=5, header=12))
            self.assertTrue(r["ok"], (phase, r))

    def test_stretched_fullscreen(self):
        r = sc.check(sc.synth(240, 120, rect=(0, 0, 240, 120)))
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["letterbox_samples"], 0)

    def test_failed_title_band_still_recognised(self):
        r = sc.check(sc.synth(200, 120, fail_band=True))
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["band"], "fail")

    def test_red_blue_swap_fails(self):
        r = sc.check(sc.synth(200, 120, swap_rb=True))
        self.assertFalse(r["ok"], r)

    def test_black_screen_fails(self):
        raw = bytearray(16 + 100 * 80 * 4)
        struct.pack_into("<III", raw, 0, 100, 80, 1)
        r = sc.check(bytes(raw))
        self.assertFalse(r["ok"])
        self.assertIn("band", r["reason"])

    def test_mirrored_fails(self):
        w, h = 200, 120
        raw = bytearray(sc.synth(w, h, phase=1))
        out = bytearray(raw)
        for y in range(h):
            for x in range(w):
                s = 16 + (y * w + (w - 1 - x)) * 4
                d = 16 + (y * w + x) * 4
                out[d:d + 4] = raw[s:s + 4]
        r = sc.check(bytes(out))
        self.assertFalse(r["ok"], r)

    def test_controls_over_the_letterbox(self):
        # A grey disc in the left bar (an on-screen control): a failure unless
        # the check is told to look around it.
        import struct as _s
        w, h = 320, 180
        raw = bytearray(sc.synth(w, h, phase=2))
        cx, cy, r = 20, 90, 12
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    i = 16 + (y * w + x) * 4
                    raw[i:i + 4] = bytes((160, 160, 160, 255))
        self.assertFalse(sc.check(bytes(raw))["ok"])
        res = sc.check(bytes(raw), avoid=[(cx, cy, r)])
        self.assertTrue(res["ok"], res)
        self.assertGreater(res["letterbox_skipped"], 0)
        self.assertGreater(res["letterbox_samples"], 0)

    def test_bad_input(self):
        self.assertFalse(sc.check(b"\x00" * 8)["ok"])
        raw = bytearray(16 + 10 * 10 * 4)
        struct.pack_into("<III", raw, 0, 10, 10, 7)
        self.assertIn("format", sc.check(bytes(raw))["reason"])


if __name__ == "__main__":
    unittest.main()
