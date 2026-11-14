"""End-to-end tests of the pipeline's intake -> analyze -> recompile stages on
the self-built test title (tools/android/testtitle), plus xbe_info checks.

    python3 -m pytest tools/android/test_pipeline.py

Needs the pinned zig (requirements-dev.txt) to build the test title; skips
without it.
"""

import importlib.util
import json
import os
import tempfile
import unittest

from tools.android import pipeline, xbe_info

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HAVE_ZIG = importlib.util.find_spec("ziglang") is not None


class XbeInfo(unittest.TestCase):
    def test_synthetic_xbe_with_unencoded_entry(self):
        r, img = xbe_info.analyze(os.path.join(ROOT, "tools", "conformance", "test.xbe"))
        self.assertEqual(r["entry_point"], "0x00011000")
        self.assertFalse(r["problems"])
        self.assertTrue(any("unencoded" in w for w in r["warnings"]))
        self.assertIsNone(img)

    def test_dxt1_decoder_solid_block(self):
        # One 4x4 DXT1 block: c0 = pure red, every index 0.
        blk = (0xF800).to_bytes(2, "little") + (0).to_bytes(2, "little") + b"\0\0\0\0"
        rgba = xbe_info._decode_dxt(blk, 4, 4, 1)
        self.assertEqual(rgba[:4], bytes((255, 0, 0, 255)))
        self.assertEqual(len(rgba), 64)

    def test_unswizzle_is_a_permutation(self):
        idx = {xbe_info._unswizzle_index(x, y, 8, 4) for x in range(8) for y in range(4)}
        self.assertEqual(idx, set(range(32)))


@unittest.skipUnless(HAVE_ZIG, "pinned zig (requirements-dev.txt) not installed")
class PipelineOnTestTitle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from tools.android.testtitle import build
        cls.tmp = tempfile.TemporaryDirectory()
        cls.info = build.build(os.path.join(cls.tmp.name, "tt"))
        cls.work = os.path.join(cls.tmp.name, "work")
        cls.rc = pipeline.main(["run", "--work", cls.work, "--source", cls.info["iso"],
                                "--sha256", cls.info["sha256"], "--to", "recompile"])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _json(self, *p):
        with open(os.path.join(self.work, *p), encoding="utf-8") as f:
            return json.load(f)

    def test_stages_succeed(self):
        self.assertEqual(self.rc, 0)
        st = self._json("pipeline_state.json")["stages"]
        for s in ("intake", "analyze", "recompile"):
            self.assertEqual(st[s]["status"], "ok", st[s])

    def test_reproducible_image(self):
        from tools.android.testtitle import build
        with tempfile.TemporaryDirectory() as t:
            again = build.build(os.path.join(t, "tt"))
        self.assertEqual(again["sha256"], self.info["sha256"])

    def test_analysis_matches_the_title(self):
        r = self._json("reports", "xbe_analysis.json")
        self.assertEqual(r["title"], "xboxrecomp test title")
        self.assertEqual(r["entry_point"], "0x%08X" % self.info["entry"])
        self.assertEqual(sorted(i["ordinal"] for i in r["kernel_imports"]),
                         sorted(self.info["kernel_ordinals"]))
        self.assertEqual(r["kernel_coverage"]["routed"], len(self.info["kernel_ordinals"]))
        self.assertTrue(all(s["digest_ok"] for s in r["sections"]))

    def test_recompiled_sources_exist(self):
        gen = os.path.join(self.work, "project", "native", "gen")
        for f in ("recomp_0000.c", "recomp_dispatch.c", "recomp_funcs.h", "recomp_types.h"):
            self.assertTrue(os.path.exists(os.path.join(gen, f)), f)
        c = self._json("reports", "compatibility.json")
        self.assertEqual(c["recompilation"]["failed"], 0)
        self.assertGreaterEqual(c["recompilation"]["translated"], 5)

    def test_manifest_lists_every_file(self):
        m = self._json("game_manifest.json")
        self.assertEqual(m["default_xbe"], "default.xbe")
        self.assertEqual({f["path"] for f in m["files"]},
                         {"default.xbe", "data/xbrtest.bin", "media/sub/hello.txt"})


if __name__ == "__main__":
    unittest.main()
