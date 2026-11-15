"""The committed SPIR-V header against its shader sources. [android-port]

The full regeneration (`vk_shaders --check`) needs the pinned glslangValidator
and runs when it has been fetched; the source-hash check runs everywhere, so
a shader edited without regenerating the header fails here too.
"""
import hashlib
import os
import re
import unittest

from tools.android import vk_shaders


class VkShaderHeaderTest(unittest.TestCase):
    def setUp(self):
        with open(vk_shaders.HEADER, encoding="utf-8") as f:
            self.header = f.read()

    def test_recorded_source_hashes_match(self):
        recorded = dict(re.findall(r"src/vk/shaders/(\S+)\s+sha256 ([0-9a-f]{64})", self.header))
        self.assertEqual(sorted(recorded), sorted(f for f, _ in vk_shaders.SHADERS))
        for fname, digest in recorded.items():
            with open(os.path.join(vk_shaders.SHADER_DIR, fname), "rb") as f:
                self.assertEqual(hashlib.sha256(f.read()).hexdigest(), digest,
                                 "%s changed; run python3 -m tools.android.vk_shaders" % fname)

    def test_arrays_are_spirv(self):
        for _fname, cname in vk_shaders.SHADERS:
            m = re.search(r"static const uint32_t %s\[(\d+)\] = \{\s*0x([0-9a-f]{8})u" % cname,
                          self.header)
            self.assertIsNotNone(m, cname)
            self.assertGreater(int(m.group(1)), 5)
            self.assertEqual(m.group(2), "07230203")          # SPIR-V magic

    def test_regenerates_identically(self):
        try:
            exe = vk_shaders.find_glslang()
        except vk_shaders.ShaderError:
            self.skipTest("glslangValidator not fetched (fetch_deps --only archives)")
        self.assertEqual(vk_shaders.generate(exe), self.header)


if __name__ == "__main__":
    unittest.main()
