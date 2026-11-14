"""Structured XBE analysis for the Android pipeline.

Built on tools.xbe_parser (the toolkit's own parser) and
tools.kernel_audit.coverage (the runtime's kernel routing), adding what the
pipeline and its reports need:

  * validation: entry point inside an executable section, kernel thunk table
    inside a section and zero-terminated, section digests recomputed
  * XDK library inventory and what each means for the runtime path
  * kernel import coverage by the runtime's bridge
  * the certificate (title, title ID, regions, media, ratings, version)
  * the title image ($$XTIMAGE, an XPR0 texture) decoded to RGBA for the
    launcher icon, when present and in a format decoded here

    python3 -m tools.android.xbe_info default.xbe [--json out.json] [--icon out.png]
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

from tools.xbe_parser.xbe_parser import XBEParser

SEC_WRITABLE, SEC_PRELOAD, SEC_EXECUTABLE = 0x1, 0x2, 0x4

# What a statically linked XDK library means for this runtime. The pipeline
# uses the pushbuffer executor for graphics, so D3D8 is fine as lifted code.
LIBRARY_NOTES = {
    "D3D8": "Direct3D 8 (lifted; frames come from the NV2A pushbuffer executor)",
    "D3D8LTCG": "Direct3D 8, LTCG build (lifted; pushbuffer executor)",
    "D3DX8": "D3DX helpers (lifted)",
    "XGRAPHC": "XGraphics helpers (lifted)",
    "DSOUND": "DirectSound (lifted; audio comes from the emulated MCPX APU)",
    "XACTENG": "XACT audio engine (lifted; emulated APU)",
    "XAPILIB": "XAPI (lifted; kernel calls go to the runtime)",
    "XONLINE": "Xbox Live: no service exists; online features will fail",
    "XONLINES": "Xbox Live (secure): no service exists",
    "XNET": "network stack: not emulated",
    "XNETS": "network stack (secure): not emulated",
    "XNETN": "network stack: not emulated",
    "XVOICE": "voice communicator: not emulated",
    "XMV": "XMV video playback (lifted)",
    "LIBC": "C runtime (lifted)", "LIBCMT": "C runtime (lifted)",
    "LIBCP": "C++ runtime (lifted)", "LIBCPMT": "C++ runtime (lifted)",
    "XKBD": "USB keyboard support (lifted)",
}

REGIONS = {0x1: "North America", 0x2: "Japan", 0x4: "Rest of world", 0x80000000: "Manufacturing"}
MEDIA = {0x1: "HDD", 0x2: "DVD-X2", 0x4: "DVD/CD", 0x8: "CD", 0x10: "DVD-5 RO",
         0x20: "DVD-9 RO", 0x40: "DVD-5 RW", 0x80: "DVD-9 RW", 0x100: "dongle",
         0x200: "media board", 0x40000000: "non-secure HDD", 0x80000000: "non-secure mode"}
INIT_FLAGS = {0x1: "mount utility drive", 0x2: "format utility drive",
              0x4: "limit devkit memory to 64 MB", 0x8: "don't set up hard disk"}


def _bits(value, table):
    return [name for bit, name in table.items() if value & bit]


def _section_of(sections, va):
    for s in sections:
        if s.virtual_addr <= va < s.virtual_addr + max(s.virtual_size, 1):
            return s
    return None


def _read_va(xbe, va, size):
    """Bytes at a VA from section raw data (or the header page)."""
    h = xbe.header
    if h.base_address <= va < h.base_address + h.headers_size:
        off = va - h.base_address
        return xbe.raw_data[off:off + size]
    s = _section_of(xbe.sections, va)
    if not s:
        return None
    off = va - s.virtual_addr
    if off >= s.raw_size:
        return b"\0" * size
    return xbe.raw_data[s.raw_addr + off:s.raw_addr + min(off + size, s.raw_size)].ljust(size, b"\0")


# ── title image (XPR0) ──────────────────────────────────────────────────────

def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _decode_dxt(data, w, h, kind):
    out = bytearray(w * h * 4)
    block_bytes = 8 if kind == 1 else 16
    pos = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = data[pos:pos + block_bytes]
            pos += block_bytes
            if len(blk) < block_bytes:
                return bytes(out)
            alpha = [255] * 16
            cb = blk
            if kind == 3:       # DXT2/3: explicit 4-bit alpha
                a64 = int.from_bytes(blk[:8], "little")
                alpha = [((a64 >> (4 * i)) & 15) * 17 for i in range(16)]
                cb = blk[8:]
            elif kind == 5:     # DXT4/5: interpolated alpha
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                pal = [a0, a1]
                if a0 > a1:
                    pal += [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
                else:
                    pal += [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                cb = blk[8:]
            c0, c1, idx = struct.unpack_from("<HHI", cb, 0)
            p0, p1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or kind != 1:
                p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
                p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
                pal_c = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
            else:
                p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
                pal_c = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x >= w or y >= h:
                    continue
                r, g, b, a = pal_c[(idx >> (2 * i)) & 3]
                if kind != 1:
                    a = alpha[i]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes((r, g, b, a))
    return bytes(out)


def _unswizzle_index(x, y, w, h):
    """Morton order as the NV2A lays out a swizzled power-of-two texture."""
    off, bit, mx, my = 0, 0, w - 1, h - 1
    xb = yb = 1
    while xb <= mx or yb <= my:
        if xb <= mx:
            if x & xb:
                off |= 1 << bit
            bit += 1
            xb <<= 1
        if yb <= my:
            if y & yb:
                off |= 1 << bit
            bit += 1
            yb <<= 1
    return off


def decode_xpr_texture(blob):
    """(w, h, rgba) from an XPR0 blob holding one texture, or None."""
    if len(blob) < 32 or blob[:4] != b"XPR0":
        return None
    _total, hdr_size = struct.unpack_from("<II", blob, 4)
    common, data_off, _lock, fmt, _size = struct.unpack_from("<IIIII", blob, 12)
    color = (fmt >> 8) & 0xFF
    w, h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
    if w > 1024 or h > 1024:
        return None
    data = blob[hdr_size + data_off:] if hdr_size + data_off < len(blob) else blob[hdr_size:]
    if color == 0x0C:
        return w, h, _decode_dxt(data, w, h, 1)
    if color == 0x0E:
        return w, h, _decode_dxt(data, w, h, 3)
    if color == 0x0F:
        return w, h, _decode_dxt(data, w, h, 5)
    if color in (0x06, 0x07):           # swizzled A8R8G8B8 / X8R8G8B8
        out = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                i = _unswizzle_index(x, y, w, h) * 4
                if i + 4 > len(data):
                    return None
                b, g, r, a = data[i:i + 4]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes((r, g, b, a if color == 0x06 else 255))
        return w, h, bytes(out)
    return None


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def resize_rgba(w, h, rgba, nw, nh):
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy = y * h // nh
        for x in range(nw):
            sx = x * w // nw
            o, i = (y * nw + x) * 4, (sy * w + sx) * 4
            out[o:o + 4] = rgba[i:i + 4]
    return bytes(out)


# ── analysis ────────────────────────────────────────────────────────────────

def analyze(xbe_path):
    xbe = XBEParser(xbe_path).parse()
    h, c = xbe.header, xbe.certificate
    problems, warnings = [], []

    sections = []
    for s in xbe.sections:
        raw = xbe.raw_data[s.raw_addr:s.raw_addr + s.raw_size]
        digest_ok = hashlib.sha1(struct.pack("<I", s.raw_size) + raw).digest() == s.section_digest
        sections.append({
            "name": s.name, "va": "0x%08X" % s.virtual_addr, "virtual_size": s.virtual_size,
            "raw_offset": s.raw_addr, "raw_size": s.raw_size,
            "flags": _bits(s.flags, {SEC_WRITABLE: "writable", SEC_PRELOAD: "preload",
                                     SEC_EXECUTABLE: "executable", 0x8: "inserted file",
                                     0x10: "head page RO", 0x20: "tail page RO"}),
            "digest_ok": digest_ok,
        })
        if s.raw_addr + s.raw_size > len(xbe.raw_data):
            problems.append("section %s raw data runs past the end of the file" % s.name)

    entry_sec = _section_of(xbe.sections, h.entry_point)
    entry_key = "debug" if h.is_debug else "retail"
    if entry_sec is None or not entry_sec.flags & SEC_EXECUTABLE:
        raw = h.entry_point_raw
        raw_sec = _section_of(xbe.sections, raw)
        if raw_sec is not None and raw_sec.flags & SEC_EXECUTABLE:
            warnings.append("entry point is stored unencoded (0x%08X); using it as is" % raw)
            entry_key, entry = "none (unencoded)", raw
            entry_sec = raw_sec
        else:
            problems.append("entry point 0x%08X is not in an executable section" % h.entry_point)
            entry = h.entry_point
    else:
        entry = h.entry_point

    thunk_ok = False
    thunk_count = 0
    if h.kernel_thunk_addr:
        data = _read_va(xbe, h.kernel_thunk_addr, 4 * 400) or b""
        for i in range(0, len(data) - 3, 4):
            v = struct.unpack_from("<I", data, i)[0]
            if v == 0:
                thunk_ok = True
                break
            thunk_count += 1
        if not thunk_ok:
            (problems if xbe.kernel_imports else warnings).append(
                "kernel thunk table at 0x%08X is not readable and zero-terminated"
                % h.kernel_thunk_addr)
    elif xbe.kernel_imports:
        problems.append("kernel imports present but no thunk table address")

    try:
        from tools.kernel_audit.coverage import classify
        imports = [{"ordinal": k.ordinal, "name": k.name} for k in xbe.kernel_imports]
        covered, wrap, data_exports, todo, _sizes = classify(imports)
        coverage = {"imported": len(imports), "routed": len(covered),
                    "needs_wrapper": [i["ordinal"] for i, _ in wrap],
                    "data_exports_missing": [i["ordinal"] for i in data_exports],
                    "unimplemented": [{"ordinal": i["ordinal"], "name": i["name"]} for i in todo]}
    except Exception as e:      # the report must not die on the audit
        coverage = {"error": str(e)}

    libs = []
    for lib in xbe.libraries:
        name = lib.name.strip("\0").upper()
        libs.append({"name": name, "version": "%d.%d.%d" % (lib.major, lib.minor, lib.build),
                     "flags": lib.flags, "note": LIBRARY_NOTES.get(name, "")})
    lib_names = {l["name"] for l in libs}
    graphics = ("XDK Direct3D, executed by the pushbuffer executor"
                if lib_names & {"D3D8", "D3D8LTCG"} else
                "no XDK D3D library: the title drives the NV2A directly, executed by the pushbuffer executor"
                if xbe.kernel_imports else "unknown")
    if lib_names & {"XONLINE", "XONLINES", "XNET", "XNETS", "XNETN"}:
        warnings.append("links network/Xbox Live libraries; online play is not available")

    xpr = None
    for s in xbe.sections:
        if s.name.upper() == "$$XTIMAGE" and s.raw_size:
            xpr = xbe.raw_data[s.raw_addr:s.raw_addr + s.raw_size]
    image = decode_xpr_texture(xpr) if xpr else None

    tls = None
    if xbe.tls:
        t = xbe.tls
        tls = {"raw_data": ["0x%08X" % t.raw_data_start, "0x%08X" % t.raw_data_end],
               "index_va": "0x%08X" % t.tls_index_addr, "zero_fill": t.zero_fill_size}

    report = {
        "file": os.path.abspath(xbe_path),
        "file_size": len(xbe.raw_data),
        "sha256": hashlib.sha256(xbe.raw_data).hexdigest(),
        "title": c.title_name.strip("\0").strip() or os.path.basename(xbe_path),
        "title_id": "%08X" % c.title_id,
        "title_id_ascii": _title_id_str(c.title_id),
        "version": c.version, "disc": c.disc_number,
        "regions": _bits(c.game_region, REGIONS) or ["none"],
        "media": _bits(c.allowed_media, MEDIA),
        "ratings": c.game_ratings,
        "base": "0x%08X" % h.base_address, "image_size": h.image_size,
        "entry_point": "0x%08X" % entry, "entry_key": entry_key,
        "entry_section": entry_sec.name if entry_sec else None,
        "kernel_thunk": "0x%08X" % h.kernel_thunk_addr, "kernel_thunk_entries": thunk_count,
        "init_flags": _bits(h.init_flags, INIT_FLAGS),
        "init_flags_raw": h.init_flags,
        "stack_commit": h.pe_stack_commit, "heap_reserve": h.pe_heap_reserve,
        "tls": tls,
        "sections": sections,
        "libraries": libs,
        "kernel_imports": [{"ordinal": k.ordinal, "name": k.name} for k in xbe.kernel_imports],
        "kernel_coverage": coverage,
        "graphics_path": graphics,
        "title_image": ({"width": image[0], "height": image[1]} if image else
                        ("present but undecoded" if xpr else None)),
        "debug_path": xbe.debug_pathname,
        "problems": problems,
        "warnings": warnings,
    }
    return report, image


def _title_id_str(tid):
    a, b = (tid >> 24) & 0xFF, (tid >> 16) & 0xFF
    if 32 < a < 127 and 32 < b < 127:
        return "%c%c-%03d" % (a, b, tid & 0xFFFF)
    return "%08X" % tid


def to_markdown(r):
    lines = ["# XBE analysis: %s" % r["title"], "",
             "| Field | Value |", "|---|---|"]
    for k, label in (("title_id", "Title ID"), ("title_id_ascii", "Title ID (publisher-number)"),
                     ("version", "Version"), ("entry_point", "Entry point"),
                     ("entry_key", "Entry encoding"), ("kernel_thunk", "Kernel thunk table"),
                     ("base", "Base"), ("image_size", "Image size"),
                     ("graphics_path", "Graphics path"), ("sha256", "default.xbe SHA-256")):
        lines.append("| %s | %s |" % (label, r[k]))
    lines.append("| Regions | %s |" % ", ".join(r["regions"]))
    lines.append("| Media | %s |" % ", ".join(r["media"]))
    lines.append("| Init flags | %s |" % (", ".join(r["init_flags"]) or "none"))
    lines += ["", "## Sections", "", "| Name | VA | Virtual size | Raw size | Flags | Digest |",
              "|---|---|---|---|---|---|"]
    for s in r["sections"]:
        lines.append("| %s | %s | %d | %d | %s | %s |" % (
            s["name"], s["va"], s["virtual_size"], s["raw_size"], ", ".join(s["flags"]),
            "ok" if s["digest_ok"] else "MISMATCH"))
    lines += ["", "## XDK libraries", ""]
    if r["libraries"]:
        lines += ["| Library | Version | Runtime note |", "|---|---|---|"]
        for l in r["libraries"]:
            lines.append("| %s | %s | %s |" % (l["name"], l["version"], l["note"]))
    else:
        lines.append("none (homebrew or stripped)")
    cov = r["kernel_coverage"]
    lines += ["", "## Kernel imports", ""]
    if "error" in cov:
        lines.append("coverage unavailable: %s" % cov["error"])
    else:
        lines.append("%d imported, %d routed by the runtime bridge." % (cov["imported"], cov["routed"]))
        for u in cov["unimplemented"]:
            lines.append("- not implemented: %d %s" % (u["ordinal"], u["name"]))
    if r["problems"] or r["warnings"]:
        lines += ["", "## Findings", ""]
        lines += ["- **problem**: %s" % p for p in r["problems"]]
        lines += ["- warning: %s" % w for w in r["warnings"]]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.android.xbe_info", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--icon", help="write the title image as PNG when decodable")
    args = ap.parse_args(argv)
    report, image = analyze(args.xbe)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(to_markdown(report))
    if args.icon and image:
        write_png(args.icon, *image)
    if not (args.json or args.markdown):
        print(to_markdown(report))
    return 1 if report["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
