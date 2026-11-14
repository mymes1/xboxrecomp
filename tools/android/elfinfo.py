"""Minimal ELF reader for checking built native libraries and executables.

Just enough of the format to answer what the pipeline asks of a binary --
machine, class, segments and their alignment, TLS, dynamic dependencies,
defined/undefined dynamic symbols, the Android API-level note -- without a
third-party dependency. 64- and 32-bit, little-endian (every Android ABI and
every host the pipeline runs on).

    python3 -m tools.android.elfinfo libmain.so
"""

import json
import struct
import sys

EM_NAMES = {3: "x86", 40: "arm", 62: "x86_64", 183: "aarch64", 243: "riscv"}
PT_LOAD, PT_DYNAMIC, PT_NOTE, PT_TLS = 1, 2, 4, 7
PT_GNU_STACK = 0x6474E551
PT_GNU_RELRO = 0x6474E552
SHT_SYMTAB, SHT_DYNSYM, SHT_NOTE = 2, 11, 7
DT_NULL, DT_NEEDED, DT_SONAME, DT_FLAGS, DT_FLAGS_1, DT_RUNPATH = 0, 1, 14, 30, 0x6FFFFFFB, 29
STB_NAMES = {0: "local", 1: "global", 2: "weak", 10: "unique"}
STT_NAMES = {0: "notype", 1: "object", 2: "func", 3: "section", 4: "file", 5: "common", 6: "tls"}


class ElfError(Exception):
    pass


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 52 or d[:4] != b"\x7fELF":
            raise ElfError("%s: not an ELF file" % path)
        self.is64 = d[4] == 2
        if d[5] != 1:
            raise ElfError("%s: big-endian ELF is not supported" % path)
        if self.is64:
            (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff, _,
             _, self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
             self.e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        else:
            (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff, _,
             _, self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
             self.e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        self.machine = EM_NAMES.get(self.e_machine, "em_%d" % self.e_machine)
        self.segments = list(self._phdrs())
        self.sections = list(self._shdrs())

    # -- headers
    def _phdrs(self):
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            if self.is64:
                t, fl, off, va, pa, fsz, msz, al = struct.unpack_from("<IIQQQQQQ", self.data, o)
            else:
                t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<IIIIIIII", self.data, o)
            yield {"type": t, "flags": fl, "offset": off, "vaddr": va, "filesz": fsz,
                   "memsz": msz, "align": al}

    def _shdrs(self):
        if not self.e_shoff or not self.e_shnum:
            return []
        out = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            if self.is64:
                nm, t, fl, ad, off, sz, link, info, al, es = struct.unpack_from("<IIQQQQIIQQ", self.data, o)
            else:
                nm, t, fl, ad, off, sz, link, info, al, es = struct.unpack_from("<IIIIIIIIII", self.data, o)
            out.append({"name_off": nm, "type": t, "flags": fl, "addr": ad, "offset": off,
                        "size": sz, "link": link, "info": info, "entsize": es})
        if self.e_shstrndx < len(out):
            strtab = out[self.e_shstrndx]
            for s in out:
                s["name"] = self._cstr(strtab["offset"] + s["name_off"])
        return out

    def _cstr(self, off):
        end = self.data.find(b"\0", off)
        return self.data[off:end if end >= 0 else len(self.data)].decode("utf-8", "replace")

    def section(self, name):
        for s in self.sections:
            if s.get("name") == name:
                return s
        return None

    # -- dynamic section
    def dynamic(self):
        seg = next((s for s in self.segments if s["type"] == PT_DYNAMIC), None)
        if not seg:
            return []
        ent = 16 if self.is64 else 8
        fmt = "<qQ" if self.is64 else "<iI"
        out = []
        for i in range(seg["filesz"] // ent):
            tag, val = struct.unpack_from(fmt, self.data, seg["offset"] + i * ent)
            if tag == DT_NULL:
                break
            out.append((tag, val))
        return out

    def _dynstr(self):
        s = self.section(".dynstr")
        return s["offset"] if s else None

    def needed(self):
        base = self._dynstr()
        if base is None:
            return []
        return [self._cstr(base + v) for t, v in self.dynamic() if t == DT_NEEDED]

    def soname(self):
        base = self._dynstr()
        for t, v in self.dynamic():
            if t == DT_SONAME and base is not None:
                return self._cstr(base + v)
        return None

    # -- symbols
    def symbols(self, table=".dynsym"):
        s = self.section(table)
        if not s or not s["entsize"]:
            return []
        strtab = self.sections[s["link"]]["offset"]
        out = []
        for i in range(1, s["size"] // s["entsize"]):
            o = s["offset"] + i * s["entsize"]
            if self.is64:
                nm, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", self.data, o)
            else:
                nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.data, o)
            out.append({"name": self._cstr(strtab + nm), "bind": STB_NAMES.get(info >> 4, info >> 4),
                        "type": STT_NAMES.get(info & 15, info & 15), "defined": shndx != 0,
                        "value": val, "size": size})
        return out

    # -- notes
    def android_api_level(self):
        """API level from the .note.android.ident note the NDK's crtbegin adds."""
        for s in self.sections:
            if s["type"] != SHT_NOTE:
                continue
            off, end = s["offset"], s["offset"] + s["size"]
            while off + 12 <= end:
                namesz, descsz, ntype = struct.unpack_from("<III", self.data, off)
                name = self.data[off + 12:off + 12 + namesz].rstrip(b"\0")
                doff = off + 12 + ((namesz + 3) & ~3)
                if name == b"Android" and ntype == 1 and descsz >= 4:
                    return struct.unpack_from("<I", self.data, doff)[0]
                off = doff + ((descsz + 3) & ~3)
        return None

    def summary(self, want_symbols=()):
        dyn = self.symbols(".dynsym")
        full = self.symbols(".symtab") or dyn
        defined = {s["name"] for s in dyn if s["defined"]}
        undefined = sorted({s["name"] for s in dyn if not s["defined"] and s["name"]})
        all_defined = {s["name"] for s in full if s["defined"]}
        tls_seg = next((s for s in self.segments if s["type"] == PT_TLS), None)
        emutls = sorted(n for n in all_defined | set(undefined) if n.startswith("__emutls_v."))
        uses_emutls_call = "__emutls_get_address" in undefined or "__emutls_get_address" in all_defined
        if tls_seg:
            tls = "elf (PT_TLS, %d bytes)" % tls_seg["memsz"]
        elif emutls or uses_emutls_call:
            tls = "emulated (%d variables)" % len(emutls)
        else:
            tls = "none"
        loads = [s for s in self.segments if s["type"] == PT_LOAD]
        gnu_stack = next((s for s in self.segments if s["type"] == PT_GNU_STACK), None)
        return {
            "path": self.path,
            "size": len(self.data),
            "class": "ELF64" if self.is64 else "ELF32",
            "machine": self.machine,
            "type": {1: "relocatable", 2: "executable", 3: "shared"}.get(self.e_type, self.e_type),
            "soname": self.soname(),
            "needed": self.needed(),
            "android_api": self.android_api_level(),
            "load_alignment": min((s["align"] for s in loads), default=None),
            "tls": tls,
            "exec_stack": bool(gnu_stack and gnu_stack["flags"] & 1),
            "dynsym_defined": len(defined),
            "undefined": undefined,
            # Lifted functions: sub_XXXXXXXX, plus the entry point, which the
            # recompiler names xbe_entry_point.
            "guest_functions": sum(1 for n in all_defined if n.startswith("sub_") and len(n) == 12)
                               + (1 if "xbe_entry_point" in all_defined else 0),
            "symbols_present": {n: (n in defined or n in all_defined) for n in want_symbols},
        }


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    for p in argv:
        s = Elf(p).summary()
        s["undefined"] = s["undefined"][:50]
        print(json.dumps(s, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
