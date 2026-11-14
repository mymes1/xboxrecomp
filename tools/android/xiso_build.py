"""Write an XDVDFS (XISO) image from a directory tree.

    python3 -m tools.android.xiso_build <dir> <out.iso>

Used for test fixtures and for packing the self-built test title; game images
come from the user. The layout follows the format as tools/xiso reads it and
as extract-xiso walks it:

  sector 32        volume descriptor: "MICROSOFT*XBOX*MEDIA", root directory
                   sector and size, a FILETIME, and the magic again at 0x7EC
  sector 33...     directory tables, then file data, each file sector-aligned

A directory table is a binary search tree of entries ordered by upper-cased
name; each entry is (left u16, right u16, sector u32, size u32, attributes u8,
name length u8, name), padded to 4 bytes, with child offsets counted in dwords
from the start of the table and 0 meaning "no child" (the root of the tree is
always the first entry, so nothing can point back at it). Entries never
straddle a 2048-byte sector; the rest of a sector is filled with 0xFF.
The tree is built balanced from the sorted names.
"""

import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTR_DIRECTORY = 0x10
ATTR_NORMAL = 0x80


class XisoBuildError(Exception):
    pass


def _key(name):
    return name.upper()


def _entry_size(name):
    return (14 + len(name) + 3) & ~3


class _Node:
    __slots__ = ("name", "is_dir", "path", "size", "sector", "children", "table", "table_sector")

    def __init__(self, name, is_dir, path, size=0):
        self.name = name
        self.is_dir = is_dir
        self.path = path
        self.size = size
        self.sector = 0
        self.children = []
        self.table = b""
        self.table_sector = 0


def _scan(path, name=""):
    node = _Node(name, True, path)
    entries = sorted(os.listdir(path), key=_key)
    seen = {}
    for n in entries:
        try:
            n.encode("ascii")
        except UnicodeEncodeError:
            raise XisoBuildError("non-ASCII name not representable in XDVDFS: %r" % n)
        if len(n) > 255:
            raise XisoBuildError("name longer than 255 bytes: %r" % n)
        if _key(n) in seen:
            raise XisoBuildError("names differ only in case: %r and %r" % (seen[_key(n)], n))
        seen[_key(n)] = n
        full = os.path.join(path, n)
        if os.path.isdir(full):
            child = _scan(full, n)
            if not child.children:
                continue          # empty directories are not representable here
        else:
            size = os.path.getsize(full)
            if size >= 1 << 32:
                raise XisoBuildError("file too large for XDVDFS: %s" % full)
            child = _Node(n, False, full, size)
        node.children.append(child)
    return node


def _layout_table(children):
    """Serialise one directory table as a balanced BST. Returns (bytes, order)."""
    # Pre-order of a balanced tree over the sorted list.
    order = []

    def build(lo, hi):
        if lo > hi:
            return None
        mid = (lo + hi) // 2
        idx = len(order)
        order.append([mid, None, None])
        left = build(lo, mid - 1)
        right = build(mid + 1, hi)
        order[idx][1] = left
        order[idx][2] = right
        return idx

    build(0, len(children) - 1)
    # Byte offset of each pre-order slot, never straddling a sector.
    offsets = []
    pos = 0
    for mid, _l, _r in order:
        size = _entry_size(children[mid].name)
        if (pos % SECTOR) + size > SECTOR:
            pos += SECTOR - (pos % SECTOR)
        offsets.append(pos)
        pos += size
    total = (pos + SECTOR - 1) // SECTOR * SECTOR
    buf = bytearray(b"\xFF" * total)
    for slot, (mid, left, right) in enumerate(order):
        c = children[mid]
        lo = offsets[left] // 4 if left is not None else 0
        ro = offsets[right] // 4 if right is not None else 0
        if lo > 0xFFFF or ro > 0xFFFF:
            raise XisoBuildError("directory table too large")
        nb = c.name.encode("ascii")
        attr = ATTR_DIRECTORY if c.is_dir else ATTR_NORMAL
        size = len(c.table) if c.is_dir else c.size
        sector = c.table_sector if c.is_dir else c.sector
        e = struct.pack("<HHIIBB", lo, ro, sector, size, attr, len(nb)) + nb
        e += b"\x00" * ((-len(e)) % 4)
        buf[offsets[slot]:offsets[slot] + len(e)] = e
    return bytes(buf)


def build(src_dir, out_path):
    root = _scan(os.path.abspath(src_dir))
    if not root.children:
        raise XisoBuildError("%s has no files" % src_dir)

    dirs = []

    def collect(n):
        dirs.append(n)
        for c in n.children:
            if c.is_dir:
                collect(c)

    collect(root)
    # Table sizes depend only on names, so size them first with dummy values.
    for d in dirs:
        d.table = _layout_table(d.children)
    sector = 33
    for d in dirs:
        d.table_sector = sector
        sector += len(d.table) // SECTOR
    files = []

    def place(n):
        for c in n.children:
            if c.is_dir:
                place(c)
            else:
                files.append(c)

    place(root)
    for f in files:
        f.sector = sector
        sector += (f.size + SECTOR - 1) // SECTOR
    for d in dirs:
        d.table = _layout_table(d.children)   # now with real sectors and sizes
    total_sectors = sector

    desc = bytearray(SECTOR)
    desc[0:len(MAGIC)] = MAGIC
    struct.pack_into("<II", desc, 0x14, root.table_sector, len(root.table))
    struct.pack_into("<Q", desc, 0x1C, 0x01C2A1E6E9B6C000)      # fixed FILETIME: reproducible
    desc[0x7EC:0x7EC + len(MAGIC)] = MAGIC

    with open(out_path, "wb") as out:
        out.write(b"\0" * (32 * SECTOR))
        out.write(desc)
        for d in dirs:
            out.seek(d.table_sector * SECTOR)
            out.write(d.table)
        for f in files:
            out.seek(f.sector * SECTOR)
            with open(f.path, "rb") as src:
                remaining = f.size
                while remaining:
                    chunk = src.read(min(1 << 20, remaining))
                    if not chunk:
                        raise XisoBuildError("%s shrank while packing" % f.path)
                    out.write(chunk)
                    remaining -= len(chunk)
        out.seek(total_sectors * SECTOR - 1)
        out.write(b"\0")
    return {"files": len(files), "directories": len(dirs), "sectors": total_sectors}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2])
        return 2
    try:
        info = build(argv[0], argv[1])
    except XisoBuildError as e:
        print("xiso_build: %s" % e, file=sys.stderr)
        return 1
    print("xiso_build: %s -> %s (%d files, %d directories, %d sectors)"
          % (argv[0], argv[1], info["files"], info["directories"], info["sectors"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
