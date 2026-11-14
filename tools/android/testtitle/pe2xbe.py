"""Convert a fixed-base i386 PE (as lld-link writes it) into an XBE.

Enough of cxbe's job for a self-built test title:

  * the XBE header page at the PE's own base (0x00010000), holding the image
    header, certificate, section headers, section names and page refcounts;
  * one XBE section per PE section, raw data copied, flags mapped
    (execute -> EXECUTABLE, write -> WRITABLE, every section PRELOAD) and
    each section's SHA-1 digest filled in;
  * the entry point and kernel thunk table encoded with the retail XOR keys;
  * the PE import address table for xboxkrnl.exe used as the kernel thunk
    table: PE ordinal imports are already 0x80000000 | ordinal, the encoding
    the XBE loader expects, and the IAT is zero-terminated.

Relocations are not supported (link with -fixed at 0x10000) and only
xboxkrnl.exe may be imported, as on a real Xbox. No signature is produced:
the toolkit's loader does not check one.

    python3 tools/android/testtitle/pe2xbe.py in.exe out.xbe --title "Name" --title-id 0x58425254
"""

import argparse
import hashlib
import os
import struct
import sys

ENTRY_RETAIL_XOR = 0xA8FC57AB
THUNK_RETAIL_XOR = 0x5B6D40B6

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000

XBE_SEC_WRITABLE = 0x01
XBE_SEC_PRELOAD = 0x02
XBE_SEC_EXECUTABLE = 0x04

HEADER_SIZE = 0x1000
IMAGE_HEADER_SIZE = 0x178
CERT_SIZE = 0x1D0
SECTION_HEADER_SIZE = 0x38


class PeError(Exception):
    pass


def parse_pe(data):
    if data[:2] != b"MZ":
        raise PeError("not a PE file (no MZ)")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise PeError("not a PE file (no PE signature)")
    machine, nsec, timedate, _, _, opt_size, _chars = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    if machine != 0x14C:
        raise PeError("not an i386 PE (machine 0x%X)" % machine)
    opt = pe_off + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        raise PeError("not a PE32 optional header")
    entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    stack_commit = struct.unpack_from("<I", data, opt + 76)[0]
    heap_reserve = struct.unpack_from("<I", data, opt + 80)[0]
    heap_commit = struct.unpack_from("<I", data, opt + 84)[0]
    ndirs = struct.unpack_from("<I", data, opt + 92)[0]
    dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i) for i in range(ndirs)]
    sections = []
    sec_off = opt + opt_size
    for i in range(nsec):
        o = sec_off + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        sections.append({"name": name, "vsize": vsize, "rva": rva, "raw_size": raw_size,
                         "raw_ptr": raw_ptr, "chars": chars})
    if len(dirs) > 5 and dirs[5][1]:
        raise PeError("the PE has base relocations; link with -fixed")
    return {"entry_rva": entry_rva, "image_base": image_base, "size_of_image": size_of_image,
            "stack_commit": stack_commit, "heap_reserve": heap_reserve,
            "heap_commit": heap_commit, "dirs": dirs, "sections": sections,
            "timedate": timedate}


def rva_to_off(pe, rva):
    for s in pe["sections"]:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["raw_size"]):
            return s["raw_ptr"] + (rva - s["rva"])
    raise PeError("RVA 0x%X is in no section" % rva)


def find_kernel_iat(data, pe):
    """RVA of the xboxkrnl.exe import address table, and its ordinals."""
    imp_rva, imp_size = pe["dirs"][1] if len(pe["dirs"]) > 1 else (0, 0)
    if not imp_rva:
        return None, []
    off = rva_to_off(pe, imp_rva)
    found = None
    while True:
        oft, _td, _fc, name_rva, ft = struct.unpack_from("<IIIII", data, off)
        if not (oft or name_rva or ft):
            break
        noff = rva_to_off(pe, name_rva)
        name = data[noff:data.index(b"\0", noff)].decode("ascii").lower()
        if name != "xboxkrnl.exe":
            raise PeError("imports from %s; only xboxkrnl.exe exists on an Xbox" % name)
        if found is not None:
            raise PeError("more than one xboxkrnl.exe import descriptor")
        found = ft
        off += 20
    ords = []
    o = rva_to_off(pe, found)
    while True:
        v = struct.unpack_from("<I", data, o)[0]
        if v == 0:
            break
        if not v & 0x80000000:
            raise PeError("import by name (0x%08X); the kernel exports by ordinal only" % v)
        ords.append(v & 0xFFFF)
        o += 4
    return found, ords


def build_xbe(pe_data, title="xboxrecomp test", title_id=0x58425254, version=1):
    pe = parse_pe(pe_data)
    base = pe["image_base"]
    if base != 0x00010000:
        raise PeError("image base must be 0x00010000 (got 0x%08X)" % base)
    iat_rva, ordinals = find_kernel_iat(pe_data, pe)
    if iat_rva is None:
        raise PeError("no kernel imports")

    secs = [s for s in pe["sections"] if s["vsize"] or s["raw_size"]]
    hdr = bytearray(HEADER_SIZE)
    cert_off = IMAGE_HEADER_SIZE
    sh_off = cert_off + CERT_SIZE
    names_off = sh_off + SECTION_HEADER_SIZE * len(secs)
    # Section names, page refcounts (two u16 per section) and debug names.
    cursor = names_off
    name_offs = []
    for s in secs:
        nb = s["name"].encode("ascii") + b"\0"
        name_offs.append(cursor)
        hdr[cursor:cursor + len(nb)] = nb
        cursor += len(nb)
    cursor = (cursor + 3) & ~3
    refcount_off = cursor
    cursor += 4 * len(secs)
    dbg_path = b"D:\\xbrtest\\xbrtest.exe\0"
    dbg_path_off = cursor
    hdr[cursor:cursor + len(dbg_path)] = dbg_path
    cursor += len(dbg_path)
    dbg_name_off = dbg_path_off + dbg_path.rindex(b"\\") + 1
    cursor = (cursor + 1) & ~1
    dbg_wname = "xbrtest.exe\0".encode("utf-16-le")
    dbg_wname_off = cursor
    hdr[cursor:cursor + len(dbg_wname)] = dbg_wname
    cursor += len(dbg_wname)
    if cursor > HEADER_SIZE:
        raise PeError("XBE headers overflow one page")

    # Raw data: sections laid out after the header page, 4 KB aligned.
    body = bytearray()
    file_off = HEADER_SIZE
    raw_info = []
    image_end = base + HEADER_SIZE
    for s in secs:
        raw = pe_data[s["raw_ptr"]:s["raw_ptr"] + s["raw_size"]] if s["raw_size"] else b""
        raw = raw[:s["vsize"]] if s["vsize"] else raw
        raw_info.append((file_off, len(raw)))
        body += raw
        pad = (-len(raw)) % 0x1000
        body += b"\0" * pad
        file_off += len(raw) + pad
        image_end = max(image_end, base + s["rva"] + max(s["vsize"], len(raw)))

    # Reproducible: the same input gives the same XBE (SOURCE_DATE_EPOCH honoured).
    now = int(os.environ.get("SOURCE_DATE_EPOCH", "1731542400")) & 0xFFFFFFFF
    entry_va = base + pe["entry_rva"]
    thunk_va = base + iat_rva
    hdr[0:4] = b"XBEH"
    struct.pack_into("<IIIIIIII", hdr, 0x104,
                     base, HEADER_SIZE, image_end - base, IMAGE_HEADER_SIZE, now,
                     base + cert_off, len(secs), base + sh_off)
    struct.pack_into("<I", hdr, 0x124, 0)                                  # init flags
    struct.pack_into("<I", hdr, 0x128, entry_va ^ ENTRY_RETAIL_XOR)
    struct.pack_into("<I", hdr, 0x12C, 0)                                  # no TLS
    struct.pack_into("<IIIIIII", hdr, 0x130,
                     pe["stack_commit"] or 0x10000, pe["heap_reserve"] or 0x100000,
                     pe["heap_commit"] or 0x1000, base, pe["size_of_image"], 0, pe["timedate"])
    struct.pack_into("<III", hdr, 0x14C, base + dbg_path_off, base + dbg_name_off,
                     base + dbg_wname_off)
    struct.pack_into("<I", hdr, 0x158, thunk_va ^ THUNK_RETAIL_XOR)
    struct.pack_into("<IIIIIII", hdr, 0x15C, 0, 0, 0, 0, 0, 0, 0)       # no libraries, no logo

    c = cert_off
    struct.pack_into("<III", hdr, c, CERT_SIZE, now, title_id)
    tn = title.encode("utf-16-le")[:78]
    hdr[c + 0x0C:c + 0x0C + len(tn)] = tn
    struct.pack_into("<I", hdr, c + 0x9C, 0x00000003)                      # HDD | DVD_X2
    struct.pack_into("<I", hdr, c + 0xA0, 0x80000007)                      # all regions
    struct.pack_into("<III", hdr, c + 0xA4, 0, 0, version)                 # ratings, disc, version

    for i, s in enumerate(secs):
        o = sh_off + SECTION_HEADER_SIZE * i
        flags = XBE_SEC_PRELOAD
        if s["chars"] & IMAGE_SCN_MEM_EXECUTE:
            flags |= XBE_SEC_EXECUTABLE
        if s["chars"] & IMAGE_SCN_MEM_WRITE:
            flags |= XBE_SEC_WRITABLE
        foff, rsize = raw_info[i]
        raw = body[foff - HEADER_SIZE:foff - HEADER_SIZE + rsize]
        struct.pack_into("<IIIIIIIII", hdr, o, flags, base + s["rva"], max(s["vsize"], rsize),
                         foff, rsize, base + name_offs[i], 0,
                         base + refcount_off + 4 * i, base + refcount_off + 4 * i + 2)
        hdr[o + 0x24:o + 0x38] = hashlib.sha1(struct.pack("<I", rsize) + raw).digest()

    return bytes(hdr) + bytes(body), {"entry": entry_va, "thunk": thunk_va,
                                      "kernel_ordinals": ordinals,
                                      "sections": [s["name"] for s in secs],
                                      "image_size": image_end - base}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pe")
    ap.add_argument("xbe")
    ap.add_argument("--title", default="xboxrecomp test")
    ap.add_argument("--title-id", default="0x58425254")
    args = ap.parse_args(argv)
    with open(args.pe, "rb") as f:
        data = f.read()
    try:
        xbe, info = build_xbe(data, args.title, int(args.title_id, 0))
    except PeError as e:
        print("pe2xbe: %s" % e, file=sys.stderr)
        return 1
    with open(args.xbe, "wb") as f:
        f.write(xbe)
    print("pe2xbe: %s -> %s (%d bytes) entry=0x%08X thunk=0x%08X ordinals=%s"
          % (args.pe, args.xbe, len(xbe), info["entry"], info["thunk"], info["kernel_ordinals"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
