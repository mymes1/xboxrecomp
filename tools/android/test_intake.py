"""Tests for the Android pipeline's disc intake and XISO writer.

    python3 -m pytest tools/android/test_intake.py

No game content: images are written by xiso_build from generated files.
"""

import hashlib
import http.server
import json
import os
import struct
import tempfile
import threading
import unittest
import zipfile

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


def _sha1(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest()


class _Server:
    """A local HTTP server for a directory (no network in the tests)."""

    def __init__(self, directory):
        class H(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=directory, **k)

            def log_message(self, *a):
                pass

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.srv.shutdown()


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
        self.sha1 = _sha1(self.img)
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

    def test_every_image_records_both_digests(self):
        info = iso.acquire(self.img, os.path.join(self.dir, "in"), None, log=self.log)
        self.assertEqual((info["sha256"], info["sha1"]), (self.sha, self.sha1))
        self.assertEqual(info["verified_with"], [])
        self.assertTrue(any(self.sha1 in m for m in self.logs))

    def test_sha1_alone_verifies(self):
        info = iso.acquire(self.img, os.path.join(self.dir, "in"), expected_sha1=self.sha1,
                           log=self.log)
        self.assertTrue(info["verified"])
        self.assertEqual(info["verified_with"], ["sha1"])
        self.assertEqual(info["matched"], "image")
        self.assertEqual(_sha(info["path"]), self.sha)
        self.assertTrue(any(m.startswith("verified: SHA-1") for m in self.logs))

    def test_sha1_mismatch_fails_and_places_nothing(self):
        out = os.path.join(self.dir, "in")
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(self.img, out, expected_sha1="0" * 40, log=self.log)
        self.assertIn("SHA-1 expected " + "0" * 40, str(cm.exception))
        self.assertIn(self.sha1, str(cm.exception))
        self.assertFalse(os.path.lexists(os.path.join(out, "disc.iso")))

    def test_both_digests_given_both_must_match(self):
        info = iso.acquire(self.img, os.path.join(self.dir, "a"), self.sha,
                           expected_sha1=self.sha1, log=self.log)
        self.assertEqual(info["verified_with"], ["sha256", "sha1"])
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(self.img, os.path.join(self.dir, "b"), self.sha,
                        expected_sha1="0" * 40, log=self.log)
        self.assertIn("SHA-1", str(cm.exception))
        self.assertNotIn("SHA-256", str(cm.exception))      # only what failed
        with self.assertRaises(iso.IsoError):
            iso.acquire(self.img, os.path.join(self.dir, "c"), "0" * 64,
                        expected_sha1=self.sha1, log=self.log)

    def test_digest_spellings(self):
        n = iso.normalise_digest
        self.assertEqual(n("  SHA-1:%s \n" % self.sha1.upper(), "sha1"), self.sha1)
        self.assertEqual(n("sha1:" + self.sha1, "sha1"), self.sha1)
        self.assertEqual(n("SHA256: " + self.sha, "sha256"), self.sha)
        self.assertIsNone(n(None, "sha1"))
        self.assertIsNone(n("   ", "sha1"))
        self.assertEqual(iso.expected_digests(self.sha, ""), {"sha256": self.sha})
        with self.assertRaises(iso.IsoError):
            n("sha256:" + self.sha, "sha1")                  # labelled as the other one
        with self.assertRaises(iso.IsoError):
            n("g" * 40, "sha1")
        with self.assertRaises(iso.IsoError):
            n(self.sha1[:-1], "sha1")

    def test_sha1_given_as_sha256_says_so(self):
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(self.img, os.path.join(self.dir, "in"), self.sha1, log=self.log)
        self.assertIn("SHA-1 digest", str(cm.exception))
        self.assertIn("--sha1", str(cm.exception))
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(self.img, os.path.join(self.dir, "in"), expected_sha1=self.sha,
                        log=self.log)
        self.assertIn("--sha256", str(cm.exception))

    def test_zip_verified_by_the_image_inside_or_by_the_zip(self):
        z = os.path.join(self.dir, "Game (USA).zip")
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(self.img, "Game (USA).iso")
        # Disc databases list the image's digest, not the archive's.
        info = iso.acquire(z, os.path.join(self.dir, "a"), expected_sha1=self.sha1, log=self.log)
        self.assertEqual(info["matched"], "image")
        self.assertEqual((info["sha256"], info["sha1"]), (self.sha, self.sha1))
        self.assertEqual(info["container"], {"sha256": _sha(z), "sha1": _sha1(z)})
        self.assertEqual(_sha(info["path"]), self.sha)
        info = iso.acquire(z, os.path.join(self.dir, "b"), _sha(z), expected_sha1=_sha1(z),
                           log=self.log)
        self.assertEqual(info["matched"], "container")
        # Mixed (one digest of each) is not a match for either file.
        with self.assertRaises(iso.IsoError):
            iso.acquire(z, os.path.join(self.dir, "c"), _sha(z), expected_sha1=self.sha1,
                        log=self.log)
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(z, os.path.join(self.dir, "d"), expected_sha1="1" * 40, log=self.log)
        self.assertIn("image in the zip " + self.sha1, str(cm.exception))
        self.assertFalse(os.path.lexists(os.path.join(self.dir, "c", "disc.iso")))
        self.assertFalse(os.path.lexists(os.path.join(self.dir, "d", "disc.iso")))

    def test_broken_zip_is_an_intake_error(self):
        z = os.path.join(self.dir, "broken.zip")
        with open(z, "wb") as f:
            f.write(b"PK\x03\x04 not really a zip")
        with self.assertRaises(iso.IsoError):
            iso.acquire(z, os.path.join(self.dir, "in"), None, log=self.log)

    def test_rerun_on_the_placed_copy_keeps_it(self):
        out = os.path.join(self.dir, "in")
        iso.acquire(self.img, out, None, log=self.log, link_local=False)
        placed = os.path.join(out, "disc.iso")
        info = iso.acquire(placed, out, expected_sha1=self.sha1, log=self.log)
        self.assertTrue(info["verified"])
        self.assertEqual(_sha(placed), self.sha)

    def test_url_zip_without_suffix_verified_by_sha1(self):
        serve = os.path.join(self.dir, "srv")
        os.makedirs(serve)
        with zipfile.ZipFile(os.path.join(serve, "download"), "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(self.img, "game.iso")
        with _Server(serve) as srv:
            url = srv.base + "/download?id=1234"
            info = iso.acquire(url, os.path.join(self.dir, "dl"), expected_sha1=self.sha1,
                               log=self.log)
            self.assertEqual(info["matched"], "image")
            self.assertEqual(_sha(info["path"]), self.sha)
            out = os.path.join(self.dir, "dl2")
            with self.assertRaises(iso.IsoError):
                iso.acquire(url, out, expected_sha1="2" * 40, log=self.log)
            self.assertFalse([f for f in os.listdir(out) if f.endswith(".part") or f == "disc.iso"])

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


def _zip_files(path, files, prefix="", extra=(), backslashes=False):
    """A zip of `files` under `prefix` (plus raw (name, data) entries)."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for rel, data in files.items():
            name = prefix + rel
            if backslashes:
                name = name.replace("/", "\\")
            zf.writestr(name, data)
        for name, data in extra:
            zf.writestr(name, data)
    return path


class FilesZip(unittest.TestCase):
    """A zip of the disc's files instead of an image."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def log(self, m):
        self.logs.append(m)

    def _intake(self, z, sha256=None, sha1=None):
        out = os.path.join(self.dir, "w-" + os.path.basename(z))
        info = iso.acquire(z, os.path.join(out, "input"), sha256, log=self.log,
                           expected_sha1=sha1)
        det = iso.detect_files(info["path"])
        man = iso.extract_files(info["path"], os.path.join(out, "game"), prefix=det["prefix"],
                                log=self.log)
        return info, det, man, os.path.join(out, "game")

    def _same_files(self, man, game):
        want = {k: hashlib.sha256(v).hexdigest() for k, v in FILES.items()}
        self.assertEqual({f["path"]: f["sha256"] for f in man["files"]}, want)
        self.assertEqual(man["total_bytes"], sum(len(v) for v in FILES.values()))
        for rel, data in FILES.items():
            with open(os.path.join(game, *rel.split("/")), "rb") as f:
                self.assertEqual(f.read(), data)
        self.assertEqual(os.path.basename(iso.find_default_xbe(game)), "default.xbe")

    def test_files_at_the_root(self):
        z = _zip_files(os.path.join(self.dir, "game.zip"), FILES)
        info, det, man, game = self._intake(z, sha1=_sha1(z))
        self.assertEqual(info["kind"], "files")
        self.assertEqual(info["matched"], "container")
        self.assertEqual((info["sha256"], info["sha1"]), (_sha(z), _sha1(z)))
        self.assertEqual(det["prefix"], "")
        self.assertIn("at its root", det["layout"])
        self._same_files(man, game)

    def test_files_in_one_folder_with_archiver_leftovers(self):
        # What zipping a folder on a Mac gives: the folder, __MACOSX and ._ files.
        z = _zip_files(os.path.join(self.dir, "Game (USA).zip"), FILES, prefix="Game (USA)/",
                       extra=[("__MACOSX/Game (USA)/._default.xbe", b"x"),
                              ("Game (USA)/.DS_Store", b"x"), ("Game (USA)/data/._level1.bin", b"x")])
        info, det, man, game = self._intake(z)
        self.assertEqual(det["prefix"], "Game (USA)/")
        self.assertEqual(det["archiver_entries_ignored"], 3)
        self.assertFalse(info["verified"])
        self._same_files(man, game)
        self.assertFalse(os.path.exists(os.path.join(game, ".DS_Store")))

    def test_windows_backslash_names(self):
        z = _zip_files(os.path.join(self.dir, "win.zip"), FILES, backslashes=True)
        info, det, man, game = self._intake(z)
        self._same_files(man, game)

    def test_same_manifest_as_the_image(self):
        src = os.path.join(self.dir, "src")
        _tree(src, FILES)
        img = os.path.join(self.dir, "game.iso")
        xiso_build.build(src, img)
        from_image = iso.extract(img, os.path.join(self.dir, "img-game"), log=self.log)
        z = _zip_files(os.path.join(self.dir, "game.zip"), FILES)
        _info, _det, from_zip, _game = self._intake(z)
        self.assertEqual(from_zip, from_image)

    def test_a_zip_with_one_image_is_still_an_image(self):
        src = os.path.join(self.dir, "src")
        _tree(src, FILES)
        img = os.path.join(self.dir, "game.iso")
        xiso_build.build(src, img)
        z = os.path.join(self.dir, "img.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.write(img, "game.iso")
        info = iso.acquire(z, os.path.join(self.dir, "in"), expected_sha1=_sha1(img), log=self.log)
        self.assertEqual(info["kind"], "image")
        self.assertEqual(info["matched"], "image")

    def test_digest_mismatch_places_nothing(self):
        z = _zip_files(os.path.join(self.dir, "game.zip"), FILES)
        out = os.path.join(self.dir, "in")
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(z, out, expected_sha1="4" * 40, log=self.log)
        self.assertIn("a zip of the game's files", str(cm.exception))
        self.assertIn("got " + _sha1(z), str(cm.exception))
        self.assertFalse(os.path.lexists(os.path.join(out, "game.zip")))

    def test_neither_files_nor_an_image(self):
        z = _zip_files(os.path.join(self.dir, "odd.zip"), {"readme.txt": b"x", "a/b.bin": b"y"})
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(z, os.path.join(self.dir, "in"), log=self.log)
        msg = str(cm.exception)
        self.assertIn("default.xbe", msg)
        self.assertIn("readme.txt", msg)
        # Two folders at the root, default.xbe a level down: not taken as files.
        z = _zip_files(os.path.join(self.dir, "two.zip"), {"A/default.xbe": b"x", "B/x": b"y"})
        with self.assertRaises(iso.IsoError):
            iso.acquire(z, os.path.join(self.dir, "in2"), log=self.log)

    def test_unsafe_entries_are_refused_before_writing(self):
        bad = [("../escape.bin", b"x"), ("/abs.bin", b"x"), ("data/../../up.bin", b"x"),
               ("C:/win.bin", b"x"), ("data//double.bin", b"x")]
        for name, data in bad:
            z = _zip_files(os.path.join(self.dir, "bad.zip"), FILES, extra=[(name, data)])
            game = os.path.join(self.dir, "g")
            with self.assertRaises(iso.IsoError, msg=name):
                iso.extract_files(z, game, log=self.log)
            self.assertFalse(os.path.exists(os.path.join(self.dir, "escape.bin")))
            self.assertFalse(os.path.exists(os.path.join(game, "default.xbe")), name)

    def test_case_twins_and_duplicates_are_refused(self):
        z = _zip_files(os.path.join(self.dir, "case.zip"), FILES, extra=[("DATA/LEVEL1.BIN", b"z")])
        with self.assertRaises(iso.IsoError) as cm:
            iso.extract_files(z, os.path.join(self.dir, "g1"), log=self.log)
        self.assertIn("letter case", str(cm.exception))
        z = os.path.join(self.dir, "dup.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("default.xbe", b"a")
            zf.writestr("default.xbe", b"b")
        with self.assertRaises(iso.IsoError):
            iso.extract_files(z, os.path.join(self.dir, "g2"), log=self.log)

    def test_symlink_entry_is_refused(self):
        z = os.path.join(self.dir, "link.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("default.xbe", b"x")
            li = zipfile.ZipInfo("data/link")
            li.external_attr = (0o120777 << 16)
            zf.writestr(li, "/etc/passwd")
        with self.assertRaises(iso.IsoError) as cm:
            iso.extract_files(z, os.path.join(self.dir, "g"), log=self.log)
        self.assertIn("symbolic link", str(cm.exception))

    def test_corrupt_entry_is_caught(self):
        z = _zip_files(os.path.join(self.dir, "crc.zip"), {"default.xbe": b"A" * 5000})
        raw = bytearray(open(z, "rb").read())
        hdr = raw.index(b"PK\x03\x04")
        name_len, extra_len = struct.unpack_from("<HH", raw, hdr + 26)
        data_at = hdr + 30 + name_len + extra_len
        raw[data_at + 2] ^= 0xFF                 # damage the compressed data
        open(z, "wb").write(bytes(raw))
        with self.assertRaises(iso.IsoError) as cm:
            iso.extract_files(z, os.path.join(self.dir, "g"), log=self.log)
        self.assertIn("damaged", str(cm.exception))

    def test_deflate64_is_refused_with_a_hint(self):
        def method9(path):
            # Rewrite every entry's compression method to 9 (Deflate64) in
            # the local headers and the central directory.
            raw = bytearray(open(path, "rb").read())
            for sig, off in ((b"PK\x03\x04", 8), (b"PK\x01\x02", 10)):
                i = raw.find(sig)
                while i >= 0:
                    struct.pack_into("<H", raw, i + off, 9)
                    i = raw.find(sig, i + 4)
            open(path, "wb").write(bytes(raw))
            return path
        z = os.path.join(self.dir, "d64.zip")
        with zipfile.ZipFile(z, "w", zipfile.ZIP_STORED) as zf:
            for rel, data in FILES.items():
                zf.writestr(rel, data)
        method9(z)
        with self.assertRaises(iso.IsoError) as cm:
            iso.extract_files(z, os.path.join(self.dir, "g"), log=self.log)
        self.assertIn("Deflate", str(cm.exception))
        # The same for a zip holding an image.
        zi = os.path.join(self.dir, "d64img.zip")
        with zipfile.ZipFile(zi, "w", zipfile.ZIP_STORED) as zf:
            zf.writestr("game.iso", b"\0" * 4096)
        method9(zi)
        with self.assertRaises(iso.IsoError) as cm:
            iso.acquire(zi, os.path.join(self.dir, "in"), log=self.log)
        self.assertIn("Deflate", str(cm.exception))

    def test_not_enough_disk_space(self):
        from unittest import mock
        z = _zip_files(os.path.join(self.dir, "game.zip"), FILES)
        tiny = shutil_usage(total=1 << 30, used=(1 << 30) - 1024, free=1024)
        with mock.patch("tools.android.iso.shutil.disk_usage", return_value=tiny):
            with self.assertRaises(iso.IsoError) as cm:
                iso.extract_files(z, os.path.join(self.dir, "g"), log=self.log)
        self.assertIn("not enough disk space", str(cm.exception))

    def test_url_download_of_a_files_zip(self):
        serve = os.path.join(self.dir, "srv")
        os.makedirs(serve)
        z = _zip_files(os.path.join(serve, "download"), FILES, prefix="My Game/")
        with _Server(serve) as srv:
            url = srv.base + "/download?token=secret"        # no .zip in the URL
            out = os.path.join(self.dir, "dl")
            info = iso.acquire(url, out, expected_sha1=_sha1(z), log=self.log)
            self.assertEqual((info["kind"], info["zip_prefix"]), ("files", "My Game/"))
            self.assertEqual(_sha(info["path"]), _sha(z))
            self.assertNotIn("secret", info["source"])
            self.assertEqual(sorted(os.listdir(out)), ["game.zip"])   # no .part left
            bad = os.path.join(self.dir, "dl2")
            with self.assertRaises(iso.IsoError):
                iso.acquire(url, bad, expected_sha1="5" * 40, log=self.log)
            self.assertEqual(os.listdir(bad), [])

    def test_rerun_on_the_placed_zip_and_switching_kinds(self):
        z = _zip_files(os.path.join(self.dir, "game.zip"), FILES)
        out = os.path.join(self.dir, "in")
        iso.acquire(z, out, log=self.log, link_local=False)
        placed = os.path.join(out, "game.zip")
        info = iso.acquire(placed, out, expected_sha1=_sha1(z), log=self.log)
        self.assertEqual((info["kind"], info["path"]), ("files", placed))
        # Then an image: the zip a previous run left goes.
        src = os.path.join(self.dir, "src")
        _tree(src, FILES)
        img = os.path.join(self.dir, "game.iso")
        xiso_build.build(src, img)
        iso.acquire(img, out, log=self.log)
        self.assertFalse(os.path.lexists(placed))
        self.assertTrue(os.path.lexists(os.path.join(out, "disc.iso")))


def shutil_usage(total, used, free):
    import collections
    return collections.namedtuple("usage", "total used free")(total, used, free)


class PipelineIntakeFlags(unittest.TestCase):
    """--sha1 / --sha256 through the pipeline's intake stage (no zig needed)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        src = os.path.join(self.tmp.name, "src")
        _tree(src, FILES)
        self.img = os.path.join(self.tmp.name, "game.iso")
        xiso_build.build(src, self.img)

    def tearDown(self):
        self.tmp.cleanup()

    def _run(self, work, *flags):
        from tools.android import pipeline
        return pipeline.main(["run", "--work", work, "--source", self.img, "--only", "intake",
                              *flags])

    def _json(self, *p):
        with open(os.path.join(*p), encoding="utf-8") as f:
            return json.load(f)

    def test_sha1_flag_verifies_and_is_reported(self):
        work = os.path.join(self.tmp.name, "w")
        self.assertEqual(self._run(work, "--sha1", _sha1(self.img)), 0)
        img = self._json(work, "reports", "intake.json")["image"]
        self.assertTrue(img["verified"])
        self.assertEqual(img["verified_with"], ["sha1"])
        self.assertEqual((img["sha256"], img["sha1"]), (_sha(self.img), _sha1(self.img)))
        result = self._json(work, "pipeline_state.json")["stages"]["intake"]["result"]
        self.assertEqual(result["image_sha1"], _sha1(self.img))
        from tools.android import compat
        md = compat.to_markdown(compat.build(work))
        self.assertIn("verified against the supplied SHA-1 of the image", md)

    def test_drop_image_removes_only_the_work_copy(self):
        work = os.path.join(self.tmp.name, "w")
        self.assertEqual(self._run(work, "--sha1", _sha1(self.img), "--drop-image"), 0)
        self.assertFalse(os.path.lexists(os.path.join(work, "input", "disc.iso")))
        self.assertTrue(os.path.exists(self.img))          # the user's own file stays
        man = self._json(work, "game_manifest.json")
        self.assertTrue(man["image"]["dropped_after_extraction"])
        self.assertEqual(man["image"]["sha1"], _sha1(self.img))
        self.assertTrue(os.path.exists(os.path.join(work, "game", man["default_xbe"])))
        # Without the flag the image is kept.
        work2 = os.path.join(self.tmp.name, "w2")
        self.assertEqual(self._run(work2, "--sha1", _sha1(self.img)), 0)
        self.assertTrue(os.path.lexists(os.path.join(work2, "input", "disc.iso")))

    def _assets(self, app):
        return os.path.join(app, "app", "src", "main", "assets")

    def test_game_files_bundled_when_they_fit(self):
        from tools.android import apk
        work = os.path.join(self.tmp.name, "w")
        self.assertEqual(self._run(work, "--sha1", _sha1(self.img)), 0)
        self.assertTrue(self._json(work, "reports", "intake.json")["fits_in_apk"])
        app = os.path.join(self.tmp.name, "app")
        n, total, mode = apk.stage_assets(work, app, log=lambda *a: None)
        self.assertEqual((n, mode), (len(FILES), "bundle"))
        listed = self._json(self._assets(app), "xbr", "game_files.json")
        self.assertTrue(listed["bundled"])
        self.assertEqual(listed["default_xbe"], "default.xbe")
        want = {k: hashlib.sha256(v).hexdigest() for k, v in FILES.items()}
        self.assertEqual({f["path"]: f["sha256"] for f in listed["files"]}, want)
        for rel, data in FILES.items():
            with open(os.path.join(self._assets(app), "game", *rel.split("/")), "rb") as f:
                self.assertEqual(f.read(), data)

    def test_game_too_large_for_an_apk_goes_external(self):
        from tools.android import apk
        work = os.path.join(self.tmp.name, "w")
        self.assertEqual(self._run(work, "--sha1", _sha1(self.img)), 0)
        man_path = os.path.join(work, "game_manifest.json")
        man = self._json(man_path)
        man["files"].append({"path": "huge.bin", "size": apk.APK_GAME_BYTES_MAX, "sha256": "ab" * 32})
        with open(man_path, "w", encoding="utf-8") as f:
            json.dump(man, f)
        app = os.path.join(self.tmp.name, "app")
        # A stale bundled copy from an earlier build must not stay in the APK.
        stale = os.path.join(self._assets(app), "game", "default.xbe")
        os.makedirs(os.path.dirname(stale))
        open(stale, "wb").close()
        n, total, mode = apk.stage_assets(work, app, log=lambda *a: None)
        self.assertEqual((n, mode), (len(FILES) + 1, "external"))
        self.assertFalse(os.path.exists(os.path.join(self._assets(app), "game")))
        listed = self._json(self._assets(app), "xbr", "game_files.json")
        self.assertFalse(listed["bundled"])
        self.assertEqual(listed["total_bytes"], total)
        self.assertEqual(listed["source"]["sha1"], _sha1(self.img))
        self.assertTrue(all(len(f["sha256"]) == 64 for f in listed["files"]))
        # Asked to bundle it anyway: refused, with the way out.
        with self.assertRaises(apk.ApkError) as cm:
            apk.stage_assets(work, app, log=lambda *a: None, mode="bundle")
        self.assertIn("--game-files external", str(cm.exception))
        # External on request, for a game that would fit.
        self.assertEqual(apk.game_files_mode(10, "external"), "external")
        self.assertEqual(apk.game_files_mode(10), "bundle")
        with self.assertRaises(apk.ApkError):
            apk.game_files_mode(10, "sideways")

    def test_apk_game_file_checks(self):
        from tools.android import apk
        man = {"files": [{"path": "default.xbe", "size": 3, "sha256": "a" * 64},
                         {"path": "data/x.bin", "size": 5, "sha256": "b" * 64}]}
        listed = {"bundled": True, "files": [dict(f) for f in man["files"]]}
        ok = apk.check_game_files({"assets/game/default.xbe": 3, "assets/game/data/x.bin": 5},
                                  listed, man)
        self.assertEqual((ok["mode"], ok["problems"]), ("bundle", []))
        bad = apk.check_game_files({"assets/game/default.xbe": 4}, listed, man)
        self.assertEqual((bad["missing"], bad["wrong_size"]), (1, 1))
        ext = dict(listed, bundled=False)
        self.assertEqual(apk.check_game_files({}, ext, man)["problems"], [])
        r = apk.check_game_files({"assets/game/default.xbe": 3}, ext, man)
        self.assertIn("holds 1 of them", r["problems"][0])
        ext["files"][1]["sha256"] = "c" * 64     # a listing that would reject the right file
        r = apk.check_game_files({}, ext, man)
        self.assertEqual(r["unlisted"], 1)

    def test_intake_says_when_the_game_cannot_fit_an_apk(self):
        from unittest import mock
        from tools.android import apk
        work = os.path.join(self.tmp.name, "w")
        with mock.patch.object(apk, "APK_GAME_BYTES_MAX", 1000):
            self.assertEqual(self._run(work, "--sha1", _sha1(self.img)), 0)   # still passes
        self.assertFalse(self._json(work, "reports", "intake.json")["fits_in_apk"])
        with open(os.path.join(work, "reports", "logs", "intake.log"), encoding="utf-8") as f:
            self.assertIn("imports them on the device from your zip", f.read())

    def test_zip_of_files_through_the_pipeline(self):
        z = _zip_files(os.path.join(self.tmp.name, "Game.zip"), FILES, prefix="Game/")
        work = os.path.join(self.tmp.name, "wz")
        from tools.android import pipeline, compat
        rc = pipeline.main(["run", "--work", work, "--source", z, "--only", "intake",
                            "--sha1", _sha1(z), "--drop-image"])
        self.assertEqual(rc, 0)
        man = self._json(work, "game_manifest.json")
        self.assertEqual(man["default_xbe"], "default.xbe")
        self.assertEqual(man["image"]["kind"], "files")
        self.assertEqual(man["detect"]["prefix"], "Game/")
        self.assertTrue(man["image"]["dropped_after_extraction"])
        self.assertTrue(os.path.exists(z))                 # the user's zip stays
        # The same manifest as from the image of the same files.
        work_i = os.path.join(self.tmp.name, "wi")
        self.assertEqual(self._run(work_i, "--sha1", _sha1(self.img)), 0)
        self.assertEqual(man["files"], self._json(work_i, "game_manifest.json")["files"])
        md = compat.to_markdown(compat.build(work))
        self.assertIn("Game files (a zip of the disc's files)", md)
        self.assertIn("verified against the supplied SHA-1 of the zip.", md)

    def test_sha1_flag_mismatch_fails_the_stage(self):
        work = os.path.join(self.tmp.name, "w")
        self.assertEqual(self._run(work, "--sha256", _sha(self.img), "--sha1", "3" * 40), 1)
        st = self._json(work, "pipeline_state.json")["stages"]["intake"]
        self.assertEqual(st["status"], "failed")
        self.assertIn("SHA-1 expected " + "3" * 40, st["error"])


if __name__ == "__main__":
    unittest.main()
