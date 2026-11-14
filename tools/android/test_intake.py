"""Tests for the Android pipeline's disc intake and XISO writer.

    python3 -m pytest tools/android/test_intake.py

No game content: images are written by xiso_build from generated files.
"""

import hashlib
import http.server
import os
import struct
import tempfile
import threading
import unittest

from tools.android import iso, xiso_build
from tools.xiso.xdvdfs import Xiso, SECTOR


def _tree(root, files):
    for rel, data in files.items():
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def _sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


FILES = {
    "default.xbe": b"XBEH" + bytes(range(256)) * 20,
    "data/level1.bin": bytes((i * 7 + 3) & 0xFF for i in range(10000)),
    "data/empty.dat": b"",
    "media/sub/hello.txt": b"hello\n",
    "Media2/UPPER.TXT": b"upper",
}


class XisoRoundTrip(unittest.TestCase):
    def test_written_image_reads_back_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src")
            _tree(src, FILES)
            img = os.path.join(tmp, "t.iso")
            info = xiso_build.build(src, img)
            self.assertEqual(info["files"], len(FILES))
            with Xiso(img) as x:
                got = {("/".join(filter(None, [d, e.name]))): x.read(e) for d, e in x.walk()}
            self.assertEqual(got, FILES)

    def test_many_entries_span_sectors_and_keep_a_valid_tree(self):
        files = {"f%03d_%s.bin" % (i, "x" * (i % 40)): bytes([i]) * (i + 1) for i in range(150)}
        files["default.xbe"] = b"XBEH"
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src")
            _tree(src, files)
            img = os.path.join(tmp, "t.iso")
            xiso_build.build(src, img)
            # Walk the tree by its links (as extract-xiso does), not linearly.
            with open(img, "rb") as f:
                f.seek(32 * SECTOR + 0x14)
                root_sector, root_size = struct.unpack("<II", f.read(8))
                f.seek(root_sector * SECTOR)
                table = f.read(root_size)
            seen = []

            def walk(off):
                l, r, _sec, _size, _attr, nlen = struct.unpack_from("<HHIIBB", table, off)
                name = table[off + 14:off + 14 + nlen].decode()
                if l:
                    walk(l * 4)
                seen.append(name)
                if r:
                    walk(r * 4)

            walk(0)
            self.assertEqual(seen, sorted(files, key=str.upper))
            self.assertGreater(root_size, SECTOR)

    def test_case_collision_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src")
            _tree(src, {"a.txt": b"1", "A.TXT": b"2"})
            if len(os.listdir(src)) < 2:
                self.skipTest("case-insensitive host file system")
            with self.assertRaises(xiso_build.XisoBuildError):
                xiso_build.build(src, os.path.join(tmp, "t.iso"))


class Intake(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        src = os.path.join(self.dir, "src")
        _tree(src, FILES)
        self.img = os.path.join(self.dir, "game.iso")
        xiso_build.build(src, self.img)
        self.sha = _sha(self.img)
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def log(self, m):
        self.logs.append(m)

    def test_local_path_with_matching_hash(self):
        info = iso.acquire(self.img, os.path.join(self.dir, "in"), self.sha, log=self.log)
        self.assertTrue(info["verified"])
        self.assertEqual(info["sha256"], self.sha)

    def test_local_path_hash_mismatch_fails(self):
        with self.assertRaises(iso.IsoError):
            iso.acquire(self.img, os.path.join(self.dir, "in"), "0" * 64, log=self.log)

    def test_missing_hash_is_reported_as_unverified(self):
        info = iso.acquire(self.img, os.path.join(self.dir, "in"), None, log=self.log)
        self.assertFalse(info["verified"])
        self.assertTrue(any("unverified" in m for m in self.logs))

    def test_bad_hash_syntax_rejected(self):
        with self.assertRaises(iso.IsoError):
            iso.acquire(self.img, os.path.join(self.dir, "in"), "abc", log=self.log)

    def test_url_download_verifies_and_redacts(self):
        directory = self.dir

        class H(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=directory, **k)

            def log_message(self, *a):
                pass

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            url = "http://user:secret@127.0.0.1:%d/game.iso?token=abc" % srv.server_address[1]
            info = iso.acquire(url, os.path.join(self.dir, "dl"), self.sha, log=self.log)
            self.assertTrue(info["verified"])
            self.assertEqual(_sha(info["path"]), self.sha)
            self.assertNotIn("secret", info["source"])
            self.assertNotIn("token", info["source"])
            self.assertFalse(any("secret" in m or "token" in m for m in self.logs))
            with self.assertRaises(iso.IsoError):
                iso.acquire(url, os.path.join(self.dir, "dl2"), "1" * 64, log=self.log)
            self.assertFalse([f for f in os.listdir(os.path.join(self.dir, "dl2"))
                              if f.endswith(".part") or f == "disc.iso"])
        finally:
            srv.shutdown()

    def test_detect_and_extract_with_manifest(self):
        det = iso.detect(self.img)
        self.assertEqual(det["partition_base"], 0)
        self.assertTrue(det["tail_magic"])
        out = os.path.join(self.dir, "game")
        man = iso.extract(self.img, out, log=self.log)
        self.assertEqual(man["file_count"], len(FILES))
        for f in man["files"]:
            self.assertEqual(f["sha256"], hashlib.sha256(FILES[f["path"]]).hexdigest())
        self.assertTrue(iso.find_default_xbe(out).endswith("default.xbe"))

    def test_video_partition_offset_is_found(self):
        padded = os.path.join(self.dir, "redump.iso")
        with open(padded, "wb") as f, open(self.img, "rb") as s:
            f.write(b"\0" * 0x00030600)
            f.write(s.read())
        self.assertEqual(iso.detect(padded)["partition_base"], 0x00030600)

    def test_non_xdvdfs_is_rejected_with_a_reason(self):
        bad = os.path.join(self.dir, "dvd.iso")
        with open(bad, "wb") as f:
            f.write(b"\0" * (16 * SECTOR) + b"\x01CD001" + b"\0" * (40 * SECTOR))
        with self.assertRaises(iso.IsoError) as cm:
            iso.detect(bad)
        self.assertIn("ISO-9660", str(cm.exception))

    def test_traversal_names_are_refused(self):
        evil = os.path.join(self.dir, "evil.iso")
        data = bytearray(open(self.img, "rb").read())
        with open(self.img, "rb") as f:
            f.seek(32 * SECTOR + 0x14)
            root_sector, _ = struct.unpack("<II", f.read(8))
        table = root_sector * SECTOR
        nlen = data[table + 13]
        name = bytes(data[table + 14:table + 14 + nlen])
        data[table + 14:table + 14 + nlen] = (b"..\\x" + b"a" * nlen)[:nlen]
        open(evil, "wb").write(bytes(data))
        with self.assertRaises(iso.IsoError):
            iso.extract(evil, os.path.join(self.dir, "out"), log=self.log)
        self.assertTrue(name)

    def test_missing_default_xbe(self):
        d = os.path.join(self.dir, "nox")
        _tree(d, {"other.xbe": b"x"})
        with self.assertRaises(iso.IsoError) as cm:
            iso.find_default_xbe(d)
        self.assertIn("other.xbe", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
