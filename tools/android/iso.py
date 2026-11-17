"""Disc image intake: acquire, verify, detect, extract.

    acquire(source, dest_dir, expected_sha256=None, expected_sha1=None)
                                                      path or http(s)/file URL
    detect(iso_path)                                  is it XDVDFS, and where
    extract(iso_path, out_dir)                        every file, verified
    detect_files(zip_path), extract_files(zip_path, out_dir)
                                                      the same for a zip of
                                                      the disc's files
    find_default_xbe(game_dir)                        the title's executable

What the user supplies is one of:

  - a disc image (XDVDFS: a bare XISO, a rebuilt image or a full-disc dump);
  - a .zip holding exactly one such image;
  - a .zip of the disc's files -- what extract-xiso or any XISO extractor
    leaves -- with default.xbe at the zip's root, or inside the one folder at
    its root (zipping a folder does that). Archiver leftovers (__MACOSX/,
    ._* files, .DS_Store, Thumbs.db, desktop.ini) are ignored, and the '\\'
    separators some Windows archivers write are read as '/'.

The pipeline never fetches game content on its own: `source` is what the user
supplied. A URL download is streamed to disk with its SHA-256 and SHA-1
computed on the way (a local file is hashed in one read). The user may give
either expected digest or both, and every digest given must match; a mismatch
deletes the download and fails. SHA-1 is accepted because it is what disc
preservation databases (Redump and the like) list for each dump, so it is
often the only reference a user has; SHA-256 is preferred when available. An
image that arrives in a .zip passes when the digests are those of the zip or
of the image inside it; a zip of the disc's files has no image, so its
digests are those of the zip. Without any expected digest both computed
digests are recorded and the image is reported as unverified.

Extraction uses tools/xiso (pure Python, no external binary). Every file's size
is checked against its directory entry and every name is checked for path
traversal before anything is written; a manifest of path, size and SHA-256 of
each extracted file is returned (and stored by the pipeline next to the files).
A zip of files gets the same rules: every name checked (no absolute paths, no
'..', no names differing only in case, no duplicates), no symbolic links or
encrypted entries, each file's size and CRC checked, and the disk checked for
room before anything is written. Its manifest has the same form, so nothing
after intake can tell the two apart.
"""

import base64
import hashlib
import json
import lzma
import os
import shutil
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
import zlib

from tools.xiso.xdvdfs import Xiso, XisoError, BASE_CANDIDATES, SECTOR, MAGIC

MAX_IMAGE_BYTES = 9 << 30          # a dual-layer DVD is 8.5 GB


class IsoError(Exception):
    pass


# Image digests, strongest first: algorithm -> (display name, hex length).
HASHES = {"sha256": ("SHA-256", 64), "sha1": ("SHA-1", 40)}
_HEX = frozenset("0123456789abcdef")


def _hashers():
    return {algo: hashlib.new(algo) for algo in HASHES}


def _digests(path):
    """{algo: hex digest} for every algorithm in HASHES, in one read."""
    hs = _hashers()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            for h in hs.values():
                h.update(chunk)
    return {a: h.hexdigest() for a, h in hs.items()}


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def normalise_digest(value, algo):
    """A user-supplied digest for `algo` ("sha256" or "sha1"): hex digits,
    optionally labelled ("sha1:...", "SHA-1:..."); letter case and
    surrounding blanks do not matter. Returns lower-case hex, or None for
    None or a blank string. Raises IsoError for anything else, naming the
    other algorithm when the length is that of its digests."""
    if value is None or not value.strip():
        return None
    name, length = HASHES[algo]
    s = value.strip().lower()
    if ":" in s:
        label, s = s.split(":", 1)
        label, s = label.strip().replace("-", ""), s.strip()
        if label != algo:
            raise IsoError("expected %s digest, got one labelled %r" % (name, label))
    if len(s) != length or not set(s) <= _HEX:
        hint = ""
        if set(s) <= _HEX:
            for other, (oname, olen) in HASHES.items():
                if other != algo and len(s) == olen:
                    hint = ("; %d hex digits is a %s digest: give it as the %s"
                            " (--%s, workflow input iso_%s)" % (olen, oname, oname, other, other))
        raise IsoError("expected %s must be %d hex digits, got %r%s" % (name, length, s, hint))
    return s


def expected_digests(sha256=None, sha1=None):
    """The digests the user supplied, normalised: {algo: hex}, maybe empty."""
    out = {}
    for algo, value in (("sha256", sha256), ("sha1", sha1)):
        d = normalise_digest(value, algo)
        if d:
            out[algo] = d
    return out


def _matches(expected, got):
    return bool(expected) and all(got.get(a) == d for a, d in expected.items())


def _mismatch(expected, image, container, what):
    parts = []
    for a, d in expected.items():
        if container:
            parts.append("%s expected %s, image in the zip %s, zip %s"
                         % (HASHES[a][0], d, image[a], container[a]))
        elif image[a] != d:
            parts.append("%s expected %s, got %s" % (HASHES[a][0], d, image[a]))
    return "digest mismatch for %s: %s" % (what, "; ".join(parts))


def _download(url, dest, log, retries=3, max_bytes=MAX_IMAGE_BYTES):
    """Stream url to dest; returns ({algo: hex digest}, size). Resumes on
    retry when the server honours Range."""
    hs = None
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
            if got and hs is not None:
                headers["Range"] = "bytes=%d-" % got
                mode = "ab"
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=120) as r:
                if mode == "ab" and r.status != 206:
                    mode, got, hs = "wb", 0, None    # no resume: start over
                if hs is None:
                    hs = _hashers()
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
                        for h in hs.values():
                            h.update(chunk)
                        out.write(chunk)
                        now = time.time()
                        if now - last_report > 15:
                            log("  %d MB" % (got >> 20))
                            last_report = now
            return {a: h.hexdigest() for a, h in hs.items()}, got
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


def _is_zip(path, name=""):
    """A .zip by name, or by content (a download URL need not end in .zip)."""
    if name.lower().endswith(".zip"):
        return True
    with open(path, "rb") as f:
        head = f.read(4)
    return head == b"PK\x03\x04" and zipfile.is_zipfile(path)


# What a damaged compressed entry raises; zipfile does not wrap these.
_ZIP_DATA_ERRORS = (zlib.error, lzma.LZMAError, EOFError)
_ZIP_METHOD_HINT = ("the zip uses a compression method this tool cannot read (%s); create it"
                    " with ordinary Deflate (most archivers' default) or Store")

# Entries an archiver adds that are not part of the disc.
_ZIP_JUNK_DIRS = ("__macosx/",)
_ZIP_JUNK_NAMES = (".ds_store", "thumbs.db", "desktop.ini")


def _zip_name(info):
    """The entry's path with '/' separators (some Windows archivers write '\\')."""
    return info.filename.replace("\\", "/")


def _zip_junk(name):
    low = name.lower().lstrip("/")
    base = low.rstrip("/").rsplit("/", 1)[-1]
    return low.startswith(_ZIP_JUNK_DIRS) or base in _ZIP_JUNK_NAMES or base.startswith("._")


def _zip_entries(z):
    """(name, info) for the zip's files, archiver leftovers and directories left out."""
    out = []
    for i in z.infolist():
        n = _zip_name(i)
        if n.endswith("/") or _zip_junk(n):
            continue
        out.append((n, i))
    return out


def _zip_kind(z):
    """What a zip holds: ("files", prefix) for the disc's files -- default.xbe
    at its root (prefix "") or in the one folder at its root ("Folder/") --
    or ("image", info) for exactly one disc image. IsoError otherwise."""
    entries = _zip_entries(z)
    if any(n.lower() == "default.xbe" for n, _ in entries):
        return "files", ""
    tops = {n.split("/", 1)[0] for n, _ in entries}
    if len(tops) == 1 and all("/" in n for n, _ in entries):
        top = tops.pop()
        if any(n.lower() == top.lower() + "/default.xbe" for n, _ in entries):
            return "files", top + "/"
    images = [i for n, i in entries if n.lower().endswith((".iso", ".xiso"))]
    if len(images) == 1:
        return "image", images[0]
    shown = sorted({n.split("/", 1)[0] for n, _ in entries})[:8]
    raise IsoError("a zip must hold either the disc's files, with default.xbe at the zip's root"
                   " (or in the one folder at its root), or exactly one disc image (.iso/.xiso);"
                   " this one has %d files, no default.xbe there and %d images%s"
                   % (len(entries), len(images),
                      " (at its root: %s)" % ", ".join(shown) if shown else ""))


def _zip_kind_path(path):
    try:
        with zipfile.ZipFile(path) as z:
            return _zip_kind(z)
    except zipfile.BadZipFile as e:
        raise IsoError("not a usable zip archive: %s" % e)


def _unwrap_zip(path, dest_dir, log):
    """A .zip holding exactly one disc image: extract it, return its path."""
    try:
        return _unwrap_zip_inner(path, dest_dir, log)
    except zipfile.BadZipFile as e:
        raise IsoError("not a usable zip archive: %s" % e)
    except _ZIP_DATA_ERRORS as e:
        raise IsoError("the image in the zip is damaged: %s" % e)
    except NotImplementedError as e:            # e.g. Deflate64
        raise IsoError(_ZIP_METHOD_HINT % e)


def _unwrap_zip_inner(path, dest_dir, log):
    with zipfile.ZipFile(path) as z:
        kind, info = _zip_kind(z)
        if kind != "image":
            raise IsoError("zip holds the disc's files, not a disc image")
        if info.file_size > MAX_IMAGE_BYTES:
            raise IsoError("image in zip is larger than a DVD")
        out = os.path.join(dest_dir, "disc.iso")
        log("unpacking %s from zip" % info.filename)
        with z.open(info) as src, open(out, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    return out


def acquire(source, dest_dir, expected_sha256=None, log=print, link_local=True,
            expected_sha1=None):
    """Bring the user's image into dest_dir/disc.iso and verify it -- or, for
    a zip of the disc's files, the zip into dest_dir/game.zip.

    expected_sha256, expected_sha1: digests of the image, or of the .zip it
    comes in (see normalise_digest for the accepted spellings). Every one
    given must match; for a zip, either all of them match the zip or all of
    them match the image inside it. A zip of files has only its own.

    Returns a dict: path, kind ("image", or "files" for a zip of the disc's
    files, with zip_prefix the folder they are in), sha256 and sha1 (of the
    disc image itself; of the zip for kind "files"), size, source
    (redacted), verified (bool), verified_with (the algorithms checked),
    matched ("image", "container" or None) and container (the zip's
    digests, or None)."""
    expected = expected_digests(expected_sha256, expected_sha1)
    os.makedirs(dest_dir, exist_ok=True)
    target = os.path.join(dest_dir, "disc.iso")
    zip_target = os.path.join(dest_dir, "game.zip")
    scheme = urllib.parse.urlsplit(source).scheme.lower()
    if scheme in ("http", "https"):
        fd, src = tempfile.mkstemp(dir=dest_dir, suffix=".part")
        os.close(fd)
        try:
            outer, _ = _download(source, src, log)
        except Exception:
            os.remove(src)
            raise
        temporary, name, shown, what = True, urllib.parse.urlsplit(source).path, _redact(source), \
            "the download"
    else:
        path = urllib.parse.urlsplit(source).path if scheme == "file" else source
        path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isfile(path):
            raise IsoError("no such file: %s" % path)
        if os.path.getsize(path) > MAX_IMAGE_BYTES:
            raise IsoError("%s is larger than a DVD; not a disc image" % path)
        log("hashing %s" % path)
        outer = _digests(path)
        src, temporary, name, shown, what = path, False, path, path, path

    container, matched, unzipping = None, None, False
    kind, zip_prefix = "image", None
    # Re-running on the work directory's own copy: nothing to place.
    in_place = not temporary and os.path.abspath(src) == os.path.abspath(target)
    try:
        if _is_zip(src, name):
            zkind, where = _zip_kind_path(src)
        else:
            zkind = None
        if zkind == "files":
            # The disc's files, not an image: the zip itself is kept and
            # extracted by extract_files. Its digests are the only ones.
            kind, zip_prefix, container = "files", where, outer
            if expected and not _matches(expected, container):
                raise IsoError(_mismatch(expected, container, None,
                                         what + " (a zip of the game's files)"))
            matched = "container" if expected else None
            log("a zip of the game's files (%s)" % ("in the folder %s" % where if where
                                                   else "at its root"))
            if not (not temporary and os.path.abspath(src) == os.path.abspath(zip_target)):
                for stale in (target, zip_target):
                    if os.path.lexists(stale):
                        os.remove(stale)
                if temporary:
                    os.replace(src, zip_target)
                elif link_local:
                    try:
                        os.symlink(src, zip_target)
                    except OSError:
                        shutil.copyfile(src, zip_target)
                else:
                    shutil.copyfile(src, zip_target)
            elif os.path.lexists(target):
                os.remove(target)               # an image a previous run left
            image = container
        elif zkind == "image":
            if in_place:
                raise IsoError("%s is a zip archive; give the archive's own path" % src)
            if os.path.lexists(target):
                os.remove(target)
            unzipping = True
            _unwrap_zip(src, dest_dir, log)
            container, image = outer, _digests(target)
            log("image inside zip: sha256 %s, sha1 %s" % (image["sha256"], image["sha1"]))
            if temporary:
                os.remove(src)
        else:
            image = outer
        if expected and kind == "image":
            if _matches(expected, image):
                matched = "image"
            elif container and _matches(expected, container):
                matched = "container"
            else:
                raise IsoError(_mismatch(expected, image, container, what))
        if kind == "image" and os.path.lexists(zip_target):
            os.remove(zip_target)               # a zip of files a previous run left
        if kind == "image" and container is None and not in_place:
            # Only now, verified, does it replace what a previous run left.
            if os.path.lexists(target):
                os.remove(target)
            if temporary:
                os.replace(src, target)
            elif link_local:
                try:
                    os.symlink(src, target)
                except OSError:
                    shutil.copyfile(src, target)
            else:
                shutil.copyfile(src, target)
    except Exception:
        if temporary and os.path.exists(src):
            os.remove(src)
        if unzipping and os.path.lexists(target):
            os.remove(target)               # an unverified image is not kept
        raise
    path = zip_target if kind == "files" else target
    size = os.path.getsize(path)
    order = list(HASHES)
    info = {"path": path, "kind": kind, "zip_prefix": zip_prefix, "sha256": image["sha256"],
            "sha1": image["sha1"], "size": size, "source": shown, "verified": bool(expected),
            "verified_with": sorted(expected, key=order.index), "matched": matched,
            "container": container}
    if expected:
        log("verified: %s of the %s" % (" and ".join(HASHES[a][0] for a in info["verified_with"]),
                                        "zip of the game's files" if kind == "files"
                                        else "zip" if matched == "container" else "image"))
    else:
        log("WARNING: no expected SHA-256 or SHA-1 given; recorded sha256 %s, sha1 %s,"
            " but authenticity is unverified" % (image["sha256"], image["sha1"]))
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


def detect_files(zip_path):
    """Identify a zip of the disc's files (see _zip_kind). IsoError otherwise."""
    try:
        with zipfile.ZipFile(zip_path) as z:
            kind, prefix = _zip_kind(z)
            if kind != "files":
                raise IsoError("%s holds a disc image, not the disc's files" % zip_path)
            entries = [(n, i) for n, i in _zip_entries(z) if n.startswith(prefix)]
            ignored = sum(1 for i in z.infolist()
                          if not _zip_name(i).endswith("/") and _zip_junk(_zip_name(i)))
    except zipfile.BadZipFile as e:
        raise IsoError("not a usable zip archive: %s" % e)
    return {"format": "files", "layout": "zip of the game's files %s" % (
                "in the folder %s" % prefix if prefix else "at its root"),
            "prefix": prefix, "file_count": len(entries),
            "total_bytes": sum(i.file_size for _, i in entries),
            "archiver_entries_ignored": ignored, "image_size": os.path.getsize(zip_path)}


def extract_files(zip_path, out_dir, prefix="", log=print, hash_files=True):
    """Extract a zip of the disc's files (those under `prefix`); returns the
    same manifest as extract(). Every entry is checked before anything is
    written."""
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    t0 = time.time()
    try:
        with zipfile.ZipFile(zip_path) as z:
            todo, seen, ignored = [], {}, 0
            for info in z.infolist():
                name = _zip_name(info)
                if _zip_junk(name):
                    ignored += 1
                    continue
                if name.endswith("/"):
                    continue                    # folders come from the files' paths
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise IsoError("%s is a symbolic link; the disc's files are plain files" % name)
                if info.flag_bits & 0x1:
                    raise IsoError("%s is encrypted; give a zip without a password" % name)
                if prefix:
                    if not name.startswith(prefix):
                        raise IsoError("%s is outside the game folder %s" % (name, prefix))
                    rel = name[len(prefix):]
                else:
                    rel = name
                if rel.startswith("/") or (len(rel) > 1 and rel[1] == ":"):
                    raise IsoError("absolute path in the zip: %r" % name)
                try:
                    parts = [_safe_component(c) for c in rel.split("/")]
                except IsoError:
                    raise IsoError("unsafe file name in the zip: %r" % name)
                rel = "/".join(parts)
                key = rel.lower()
                if key in seen:
                    raise IsoError("two files in the zip have the same name, ignoring letter"
                                   " case: %s and %s" % (seen[key], rel))
                seen[key] = rel
                todo.append((rel, parts, info))
            total = sum(i.file_size for _, _, i in todo)
            if total > MAX_IMAGE_BYTES:
                raise IsoError("the zip's files add up to %d bytes, more than a disc holds" % total)
            free = shutil.disk_usage(out_dir).free
            if free < total + (64 << 20):
                raise IsoError("not enough disk space for the game files: %.2f GiB needed, %.2f GiB"
                               " free" % (total / 1024.0 ** 3, free / 1024.0 ** 3))
            files = []
            for rel, parts, info in todo:
                dest = os.path.join(out_dir, *parts)
                if not os.path.abspath(dest).startswith(out_dir + os.sep):
                    raise IsoError("path escapes the output directory: %s" % rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                h = hashlib.sha256() if hash_files else None
                n = 0
                with z.open(info) as src, open(dest, "wb") as dst:
                    for chunk in iter(lambda: src.read(1 << 20), b""):
                        n += len(chunk)
                        if n > info.file_size:
                            raise IsoError("%s: more data than its zip entry declares" % rel)
                        if h:
                            h.update(chunk)
                        dst.write(chunk)
                if n != info.file_size:
                    raise IsoError("%s: extracted %d bytes, the zip says %d" % (rel, n, info.file_size))
                entry = {"path": rel, "size": n}
                if h:
                    entry["sha256"] = h.hexdigest()
                files.append(entry)
    except zipfile.BadZipFile as e:             # includes a bad CRC
        raise IsoError("not a usable zip archive: %s" % e)
    except _ZIP_DATA_ERRORS as e:
        raise IsoError("a file in the zip is damaged: %s" % e)
    except NotImplementedError as e:            # e.g. Deflate64
        raise IsoError(_ZIP_METHOD_HINT % e)
    except RuntimeError as e:                   # zipfile: "... encrypted, password required"
        raise IsoError("cannot read the zip: %s" % e)
    log("extracted %d files, %d bytes from the zip in %.1fs%s" % (
        len(files), total, time.time() - t0,
        " (%d archiver entries ignored)" % ignored if ignored else ""))
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
