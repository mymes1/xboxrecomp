"""Disc image intake: acquire, verify, detect, extract.

    acquire(source, dest_dir, expected_sha256=None)   path or http(s)/file URL
    detect(iso_path)                                  is it XDVDFS, and where
    extract(iso_path, out_dir)                        every file, verified
    find_default_xbe(game_dir)                        the title's executable

The pipeline never fetches game content on its own: `source` is what the user
supplied. A URL download is streamed to disk with its SHA-256 computed on the
way; when the user gives the expected hash a mismatch deletes the download and
fails. Without one the computed hash is recorded and reported as unverified.

Extraction uses tools/xiso (pure Python, no external binary). Every file's size
is checked against its directory entry and every name is checked for path
traversal before anything is written; a manifest of path, size and SHA-256 of
each extracted file is returned (and stored by the pipeline next to the files).
"""

import base64
import hashlib
import json
import os
import shutil
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile

from tools.xiso.xdvdfs import Xiso, XisoError, BASE_CANDIDATES, SECTOR, MAGIC

MAX_IMAGE_BYTES = 9 << 30          # a dual-layer DVD is 8.5 GB


class IsoError(Exception):
    pass


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _normalise_sha(s):
    if s is None:
        return None
    s = s.strip().lower()
    if s.startswith("sha256:"):
        s = s[7:]
    if len(s) != 64 or any(c not in "0123456789abcdef" for c in s):
        raise IsoError("expected SHA-256 must be 64 hex digits, got %r" % s)
    return s


def _download(url, dest, log, retries=3, max_bytes=MAX_IMAGE_BYTES):
    """Stream url to dest; returns (sha256, size). Resumes on retry when the
    server honours Range."""
    h = None
    got = 0
    last = None
    # urllib does not take credentials from the URL itself: move them into a
    # Basic Authorization header and request the URL without them.
    parts = urllib.parse.urlsplit(url)
    auth = None
    if parts.username is not None:
        cred = "%s:%s" % (urllib.parse.unquote(parts.username),
                          urllib.parse.unquote(parts.password or ""))
        auth = "Basic " + base64.b64encode(cred.encode("utf-8")).decode("ascii")
        netloc = parts.hostname + (":%d" % parts.port if parts.port else "")
        url = urllib.parse.urlunsplit((parts.scheme, netloc, parts.path, parts.query,
                                       parts.fragment))
    for attempt in range(1, retries + 1):
        try:
            headers = {"User-Agent": "xboxrecomp-android-pipeline"}
            if auth:
                headers["Authorization"] = auth
            mode = "wb"
            if got and h is not None:
                headers["Range"] = "bytes=%d-" % got
                mode = "ab"
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=120) as r:
                if mode == "ab" and r.status != 206:
                    mode, got, h = "wb", 0, None     # no resume: start over
                if h is None:
                    h = hashlib.sha256()
                total = r.headers.get("Content-Length")
                log("downloading %s (attempt %d%s)" % (
                    _redact(url), attempt, ", %s bytes" % total if total else ""))
                t0 = time.time()
                last_report = t0
                with open(dest, mode) as out:
                    for chunk in iter(lambda: r.read(1 << 20), b""):
                        got += len(chunk)
                        if got > max_bytes:
                            raise IsoError("download exceeds %d bytes; not a disc image" % max_bytes)
                        h.update(chunk)
                        out.write(chunk)
                        now = time.time()
                        if now - last_report > 15:
                            log("  %d MB" % (got >> 20))
                            last_report = now
            return h.hexdigest(), got
        except IsoError:
            raise
        except Exception as e:      # network: retry (resuming if possible)
            last = e
            log("download error: %s" % e)
            time.sleep(3 * attempt)
    raise IsoError("download failed after %d attempts: %s" % (retries, last))


def _redact(url):
    """Never log credentials or signed query strings of a user's URL."""
    p = urllib.parse.urlsplit(url)
    netloc = p.hostname or ""
    if p.port:
        netloc += ":%d" % p.port
    return urllib.parse.urlunsplit((p.scheme, netloc, p.path, "", ""))


def _unwrap_zip(path, dest_dir, log):
    """A .zip holding exactly one disc image: extract it, return its path."""
    with zipfile.ZipFile(path) as z:
        images = [i for i in z.infolist()
                  if not i.is_dir() and i.filename.lower().endswith((".iso", ".xiso"))]
        if len(images) != 1:
            raise IsoError("zip must contain exactly one .iso/.xiso, found %d" % len(images))
        info = images[0]
        if info.file_size > MAX_IMAGE_BYTES:
            raise IsoError("image in zip is larger than a DVD")
        out = os.path.join(dest_dir, "disc.iso")
        log("unpacking %s from zip" % info.filename)
        with z.open(info) as src, open(out, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    return out


def acquire(source, dest_dir, expected_sha256=None, log=print, link_local=True):
    """Bring the user's image into dest_dir/disc.iso and verify it.

    Returns a dict: path, sha256, size, source (redacted), verified (bool)."""
    expected = _normalise_sha(expected_sha256)
    os.makedirs(dest_dir, exist_ok=True)
    target = os.path.join(dest_dir, "disc.iso")
    scheme = urllib.parse.urlsplit(source).scheme.lower()
    if scheme in ("http", "https"):
        fd, tmp = tempfile.mkstemp(dir=dest_dir, suffix=".part")
        os.close(fd)
        try:
            digest, size = _download(source, tmp, log)
        except Exception:
            os.remove(tmp)
            raise
        if expected and digest != expected:
            os.remove(tmp)
            raise IsoError("SHA-256 mismatch: expected %s, downloaded %s" % (expected, digest))
        os.replace(tmp, target + (".zip" if source.lower().split("?")[0].endswith(".zip") else ""))
        if os.path.exists(target + ".zip"):
            zpath = target + ".zip"
            target = _unwrap_zip(zpath, dest_dir, log)
            os.remove(zpath)
            digest_inner = _sha256(target)
            log("image inside zip: sha256 %s" % digest_inner)
        shown = _redact(source)
    else:
        path = urllib.parse.urlsplit(source).path if scheme == "file" else source
        path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isfile(path):
            raise IsoError("no such file: %s" % path)
        if os.path.getsize(path) > MAX_IMAGE_BYTES:
            raise IsoError("%s is larger than a DVD; not a disc image" % path)
        log("hashing %s" % path)
        digest = _sha256(path)
        if expected and digest != expected:
            raise IsoError("SHA-256 mismatch for %s: expected %s, got %s" % (path, expected, digest))
        if path.lower().endswith(".zip"):
            target = _unwrap_zip(path, dest_dir, log)
        else:
            if os.path.lexists(target):
                os.remove(target)
            if link_local:
                try:
                    os.symlink(path, target)
                except OSError:
                    shutil.copyfile(path, target)
            else:
                shutil.copyfile(path, target)
        shown = path
    size = os.path.getsize(target)
    info = {"path": target, "sha256": digest, "size": size, "source": shown,
            "verified": bool(expected)}
    if not expected:
        log("WARNING: no expected SHA-256 given; recorded %s but authenticity is unverified"
            % digest)
    return info


def detect(iso_path):
    """Identify an XDVDFS image. Raises IsoError for anything else."""
    size = os.path.getsize(iso_path)
    if size < 33 * SECTOR:
        raise IsoError("%s is too small to be a disc image (%d bytes)" % (iso_path, size))
    try:
        with Xiso(iso_path) as x:
            base = x.base
            root_sector, root_size = x.root_sector, x.root_size
            with open(iso_path, "rb") as f:
                f.seek(base + 32 * SECTOR + 0x7EC)
                tail_magic = f.read(len(MAGIC)) == MAGIC
    except XisoError as e:
        with open(iso_path, "rb") as f:
            f.seek(16 * SECTOR + 1)
            iso9660 = f.read(5) == b"CD001"
        hint = (" It is an ISO-9660 image (DVD-video or a PC disc), not an Xbox game disc."
                if iso9660 else "")
        raise IsoError(str(e) + hint)
    kinds = {0x00000000: "XISO (bare game partition)",
             0x0000FD90: "rebuilt image (partition at 0xFD90)",
             0x00030600: "redump-style (video partition first)",
             0x0FD90000: "full Xbox disc layout",
             0x18300000: "redump full-disc dump"}
    return {"format": "xdvdfs", "layout": kinds.get(base, "partition at 0x%X" % base),
            "partition_base": base, "root_sector": root_sector, "root_size": root_size,
            "tail_magic": tail_magic, "image_size": size,
            "known_bases": [hex(b) for b in BASE_CANDIDATES]}


def _safe_component(name):
    if not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name:
        raise IsoError("unsafe file name in image: %r" % name)
    return name


def extract(iso_path, out_dir, log=print, hash_files=True):
    """Extract every file; returns the manifest dict (also written to
    out_dir/../game_manifest.json by the pipeline)."""
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    files = []
    total = 0
    t0 = time.time()
    with Xiso(iso_path) as x:
        seen = {}
        for d, e in x.walk():
            parts = [_safe_component(p) for p in d.split("/") if p] + [_safe_component(e.name)]
            rel = "/".join(parts)
            key = rel.lower()
            if key in seen:
                raise IsoError("two files differ only in case: %s and %s" % (seen[key], rel))
            seen[key] = rel
            dest = os.path.join(out_dir, *parts)
            if not os.path.abspath(dest).startswith(out_dir + os.sep):
                raise IsoError("path escapes the output directory: %s" % rel)
            x.extract(e, dest)
            actual = os.path.getsize(dest)
            if actual != e.size:
                raise IsoError("%s: extracted %d bytes, directory says %d" % (rel, actual, e.size))
            entry = {"path": rel, "size": e.size}
            if hash_files:
                entry["sha256"] = _sha256(dest)
            files.append(entry)
            total += e.size
    log("extracted %d files, %d bytes in %.1fs" % (len(files), total, time.time() - t0))
    return {"files": sorted(files, key=lambda f: f["path"].lower()),
            "file_count": len(files), "total_bytes": total}


def find_default_xbe(game_dir):
    """default.xbe in the root of the extracted disc (any letter case)."""
    for n in os.listdir(game_dir):
        if n.lower() == "default.xbe" and os.path.isfile(os.path.join(game_dir, n)):
            return os.path.join(game_dir, n)
    others = sorted(n for n in os.listdir(game_dir) if n.lower().endswith(".xbe"))
    raise IsoError("no default.xbe in the disc root%s" % (
        " (found %s)" % ", ".join(others) if others else ""))


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=False)
        f.write("\n")
    os.replace(tmp, path)
