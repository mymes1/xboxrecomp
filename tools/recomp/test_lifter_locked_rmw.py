"""[android-port] The whole lock-prefixed family lowers to atomic operations.

Only lock xadd and lock cmpxchg used to: every other locked read-modify-write
(lock add/sub/inc/dec/and/or/xor/not/neg/adc/sbb/bts/btr/btc, 8- and 16-bit
xadd/cmpxchg, cmpxchg8b) became a RECOMP_UNIMPL call that did nothing at all,
and xchg with memory -- locked by definition, the acquire of every spin lock --
was a plain load and store. A counter bumped with `lock inc` never moved.

These tests translate real machine code, compile it against the real
recomp_types.h, and run it: the memory result and the flag a following setcc
reads must match the x86 definition for every operand pair, the misaligned
fallback (taken on ARM64 hosts) must agree with the aligned path, and four
host threads hammering one counter through `lock inc` / `lock add` / `xchg`
spin locks must lose no update.
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import config
from .c_fixture import compiler, command, is_msvc
from .translator import FunctionTranslator

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "templates" / "runtime"


def translate(hexcode, name):
    image = bytes.fromhex(hexcode)
    base = 0x10000
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="locked-rmw-test")
    db = {base: {"start": hex(base), "end": base + len(image), "_addr": base,
                 "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(base, db[base]).replace(
        "sub_00010000", name)


# setcc bl ; movzx ebx, bl ; ret -- exports one flag in ebx.
def _flag(cc):
    return "0f%02xc3" % cc + "0fb6db" + "c3"


SETZ, SETS, SETB, SETO, SETL, SETBE = 0x94, 0x98, 0x92, 0x90, 0x9C, 0x96
BT_EBX0 = "0fbae300"     # bt ebx, 0: CF = ebx & 1 (carry-in for adc/sbb)

# name -> (machine code, C expression for the expected memory value, C
# expression for the expected flag); m = old memory value, v = edx, c = carry
# in, all already masked to the width W (bits).
CASES = {
    "add_z":   ("f00111" + _flag(SETZ),  32, "m + v", "((m + v) & M) == 0"),
    "add_b":   ("f00111" + _flag(SETB),  32, "m + v", "((uint64_t)m + v) >> W & 1"),
    "add_o":   ("f00111" + _flag(SETO),  32, "m + v", "OF_ADD(m, v, (m + v) & M)"),
    "sub_b":   ("f02911" + _flag(SETB),  32, "m - v", "m < v"),
    "sub_l":   ("f02911" + _flag(SETL),  32, "m - v", "SX(m) < SX(v)"),
    "sub_be":  ("f02911" + _flag(SETBE), 32, "m - v", "m <= v"),
    "and_z":   ("f02111" + _flag(SETZ),  32, "m & v", "(m & v) == 0"),
    "or_s":    ("f00911" + _flag(SETS),  32, "m | v", "SX(m | v) < 0"),
    "xor_z":   ("f03111" + _flag(SETZ),  32, "m ^ v", "(m ^ v) == 0"),
    "inc_z":   ("f0ff01" + _flag(SETZ),  32, "m + 1", "((m + 1) & M) == 0"),
    "inc_o":   ("f0ff01" + _flag(SETO),  32, "m + 1", "m == (M >> 1)"),
    "dec_z":   ("f0ff09" + _flag(SETZ),  32, "m - 1", "m == 1"),
    "dec_l":   ("f0ff09" + _flag(SETL),  32, "m - 1", "SX(m) < 1"),
    "neg_b":   ("f0f719" + _flag(SETB),  32, "0 - m", "m != 0"),
    "neg_s":   ("f0f719" + _flag(SETS),  32, "0 - m", "SX(0 - m) < 0"),
    "not":     ("f0f711" + "c3",         32, "~m", None),
    "adc_b":   (BT_EBX0 + "f01111" + _flag(SETB), 32, "m + v + c",
                "((uint64_t)m + v + c) >> W & 1"),
    "sbb_b":   (BT_EBX0 + "f01911" + _flag(SETB), 32, "m - v - c",
                "(uint64_t)m < (uint64_t)v + c"),
    "bts_b":   ("f00fab11" + _flag(SETB), 32, "BITSTR(m, v, |)", "BIT(m, v)"),
    "btr_b":   ("f00fb311" + _flag(SETB), 32, "BITSTR(m, v, & ~)", "BIT(m, v)"),
    "btc_b":   ("f00fbb11" + _flag(SETB), 32, "BITSTR(m, v, ^)", "BIT(m, v)"),
    "btsi_b":  ("f00fba2905" + _flag(SETB), 32, "m | 0x20", "(m >> 5) & 1"),
    "addi_z":  ("f0810178563412" + _flag(SETZ), 32, "m + 0x12345678",
                "((m + 0x12345678u) & M) == 0"),
    "subi_b":  ("f0832901" + _flag(SETB), 32, "m - 1", "m < 1"),
    "add8_b":  ("f00011" + _flag(SETB),  8, "m + v", "(m + v) >> 8 & 1"),
    "add8_o":  ("f00011" + _flag(SETO),  8, "m + v", "OF_ADD(m, v, (m + v) & M)"),
    "inc8_o":  ("f0fe01" + _flag(SETO),  8, "m + 1", "m == 0x7F"),
    "dec8_z":  ("f0fe09" + _flag(SETZ),  8, "m - 1", "m == 1"),
    "sub16_l": ("66f02911" + _flag(SETL), 16, "m - v", "SX(m) < SX(v)"),
    "inc16_o": ("66f0ff01" + _flag(SETO), 16, "m + 1", "m == 0x7FFF"),
    "or16_z":  ("66f00911" + _flag(SETZ), 16, "m | v", "(m | v) == 0"),
}

# Exchange-type forms check registers as well: (code, width, memory, register
# expression and which register, flag).
XCASES = {
    "xadd":    ("f00fc111" + "c3",   32, "m + v", ("edx", "m")),
    "xadd8":   ("f00fc011" + "c3",    8, "m + v", ("edx", "(v0 & 0xFFFFFF00u) | m")),
    "xadd16":  ("66f00fc111" + "c3", 16, "m + v", ("edx", "(v0 & 0xFFFF0000u) | m")),
    "xchg":    ("8711" + "c3",       32, "v", ("edx", "m")),
    "xchgl":   ("f08711" + "c3",     32, "v", ("edx", "m")),
    "xchg8":   ("8611" + "c3",        8, "v", ("edx", "(v0 & 0xFFFFFF00u) | m")),
}

HARNESS = r"""
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#undef eax
#undef ecx
#undef edx
#undef esp
#undef ebx
#undef esi
#undef edi
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
ptrdiff_t g_xbox_mem_offset;
static uint8_t guest[1 << 16] __attribute__((aligned(64)));
#define M ((uint32_t)(W == 32 ? 0xFFFFFFFFu : ((1u << W) - 1)))
#define SX(x) (W == 32 ? (int64_t)(int32_t)(uint32_t)(x) : W == 16 ? (int64_t)(int16_t)(x) : (int64_t)(int8_t)(x))
#define OF_ADD(a, b, r) (((((a) ^ (r)) & ((b) ^ (r))) >> (W - 1)) & 1)
#define BIT(m, v) (((m) >> ((v) & 31)) & 1)
#define BITSTR(m, v, op) ((m) op (1u << ((v) & 31)))
static uint32_t rd(uint32_t a, int w) {
    uint32_t x = 0; memcpy(&x, guest + a, w / 8); return x;
}
static void wr(uint32_t a, int w, uint32_t x) { memcpy(guest + a, &x, w / 8); }
static const uint32_t VALS[] = {0, 1, 2, 5, 0x1F, 0x20, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000,
    0xFFFF, 0x12345678, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF};
#define NV (sizeof VALS / sizeof VALS[0])
static int failures;
"""


def _case_fn(name, width, mem_expr, flag_expr):
    flag_check = (f"uint32_t ef = (uint32_t)({flag_expr});\n"
                  f"          if (ebx != ef) {{ fprintf(stderr, \"%s m=%x v=%x c=%u: flag %u want %u\\n\","
                  f" \"{name}\", m, v, c, ebx, ef); failures++; }}"
                  if flag_expr else "")
    return f"""
static void run_{name}(uint32_t addr) {{
    enum {{ W = {width} }};
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) for (uint32_t c = 0; c < 2; c++) {{
          uint32_t m = VALS[i] & M, v = VALS[j] & M;
          if (W == 32 && "{name}"[1] == 't' && "{name}"[0] == 'b' && "{name}"[3] != 'i') v &= 31;
          wr(addr, W, m); wr(addr + W / 8, 8, 0xA5);
          ecx = addr; edx = VALS[j] & (W == 32 ? 0xFFFFFFFFu : M); ebx = c; esp = 0x8000;
          if (W != 32) edx |= 0xABCD0000u & ~M;
          v = edx & M; if (W == 32 && "{name}"[0] == 'b' && "{name}"[1] == 't' && "{name}"[3] != 'i') {{ v = edx & 31; edx = v; }}
          f_{name}();
          uint32_t em = (uint32_t)({mem_expr}) & M;
          if (rd(addr, W) != em || guest[addr + W / 8] != 0xA5 || esp != 0x8004) {{
              fprintf(stderr, "{name} m=%x v=%x c=%u: mem %x want %x\\n", m, v, c, rd(addr, W), em); failures++; }}
          {flag_check}
    }}
}}
"""


def _xcase_fn(name, width, mem_expr, reg):
    rname, rexpr = reg
    return f"""
static void run_{name}(uint32_t addr) {{
    enum {{ W = {width} }};
    for (unsigned i = 0; i < NV; i++) for (unsigned j = 0; j < NV; j++) {{
        uint32_t m = VALS[i] & M, v0 = VALS[j] ^ 0x5A5A0000u, v = v0 & M;
        wr(addr, W, m); ecx = addr; edx = v0; esp = 0x8000;
        f_{name}();
        uint32_t em = (uint32_t)({mem_expr}) & M, er = (uint32_t)({rexpr});
        if (rd(addr, W) != em || {rname} != er) {{
            fprintf(stderr, "{name} m=%x v=%x: mem %x/%x reg %x/%x\\n", m, v, rd(addr, W), em, {rname}, er);
            failures++; }}
    }}
}}
"""


CMPXCHG_TESTS = r"""
static void run_cmpxchg_family(uint32_t addr) {
    /* 32/16/8-bit cmpxchg: match stores edx, mismatch loads the accumulator. */
    for (int k = 0; k < 2; k++) {
        wr(addr, 32, 0x11223344); eax = k ? 0x11223344 : 0x55; edx = 0xCAFEBABE; ecx = addr; esp = 0x8000;
        f_cmpxchg_z();
        if (k ? (rd(addr, 32) != 0xCAFEBABE || ebx != 1 || eax != 0x11223344)
              : (rd(addr, 32) != 0x11223344 || ebx != 0 || eax != 0x11223344)) {
            fprintf(stderr, "cmpxchg32 k=%d mem=%x eax=%x zf=%u\n", k, rd(addr, 32), eax, ebx); failures++; }
        wr(addr, 16, 0x3344); eax = k ? 0xFFFF3344u : 0xFFFF0055u; edx = 0xBABE; ecx = addr; esp = 0x8000;
        f_cmpxchg16_z();
        if (k ? (rd(addr, 16) != 0xBABE || ebx != 1 || eax != 0xFFFF3344u)
              : (rd(addr, 16) != 0x3344 || ebx != 0 || eax != 0xFFFF3344u)) {
            fprintf(stderr, "cmpxchg16 k=%d mem=%x eax=%x zf=%u\n", k, rd(addr, 16), eax, ebx); failures++; }
        wr(addr, 8, 0x44); eax = k ? 0xFFFFFF44u : 0xFFFFFF55u; edx = 0xBE; ecx = addr; esp = 0x8000;
        f_cmpxchg8_z();
        if (k ? (rd(addr, 8) != 0xBE || ebx != 1 || eax != 0xFFFFFF44u)
              : (rd(addr, 8) != 0x44 || ebx != 0 || eax != 0xFFFFFF44u)) {
            fprintf(stderr, "cmpxchg8 k=%d mem=%x eax=%x zf=%u\n", k, rd(addr, 8), eax, ebx); failures++; }
        /* cmpxchg8b [esi]: edx:eax vs m64, ecx:ebx stored on a match. */
        wr(addr, 32, 0x89ABCDEF); wr(addr + 4, 32, 0x01234567);
        edx = k ? 0x01234567 : 0; eax = k ? 0x89ABCDEF : 0; ecx = 0xDEADBEEF; ebx = 0xFEEDFACE;
        esi = addr; esp = 0x8000;
        f_cmpxchg8b_z();
        if (k ? (rd(addr, 32) != 0xFEEDFACE || rd(addr + 4, 32) != 0xDEADBEEF || ebx != 1)
              : (rd(addr, 32) != 0x89ABCDEF || eax != 0x89ABCDEF || edx != 0x01234567 || ebx != 0)) {
            fprintf(stderr, "cmpxchg8b k=%d lo=%x hi=%x eax=%x edx=%x zf=%u\n", k,
                    rd(addr, 32), rd(addr + 4, 32), eax, edx, ebx); failures++; }
    }
}
"""

CMPXCHG_CODE = {
    "cmpxchg_z":   "f00fb111" + _flag(SETZ),
    "cmpxchg16_z": "66f00fb111" + _flag(SETZ),
    "cmpxchg8_z":  "f00fb011" + _flag(SETZ),
    "cmpxchg8b_z": "f00fc70e" + _flag(SETZ),
}

# Thread test: inc_loop runs `lock inc [ecx]` edx times; add_loop `lock add
# [ecx], 3`; spin_loop takes an xchg spin lock at [ecx+4] around a plain
# read-modify-write of [ecx+8].
#   inc_loop:  f0 ff 01 / 4a / 75 fa / c3          (lock inc [ecx]; dec edx; jnz)
#   add_loop:  f0 83 01 03 / 4a / 75 f9 / c3       (lock add [ecx],3; dec edx; jnz)
#   spin_loop: b8 01 00 00 00 / 87 41 04 / 85 c0 / 75 f4 /   (mov eax,1; xchg [ecx+4],eax; test; jnz)
#              8b 41 08 / 40 / 89 41 08 /                    (mov eax,[ecx+8]; inc eax; mov [ecx+8],eax)
#              c7 41 04 00 00 00 00 / 4a / 75 e3 / c3        (mov [ecx+4],0; dec edx; jnz)
THREAD_CODE = {
    "inc_loop": "f0ff01" "4a" "75fa" "c3",
    "add_loop": "f0830103" "4a" "75f9" "c3",
    "spin_loop": "b801000000" "874104" "85c0" "75f4" "8b4108" "40" "894108"
                 "c7410400000000" "4a" "75e3" "c3",
}

THREAD_TEST = r"""
#include <pthread.h>
#define ITER 200000
static void *worker(void *arg) {
    uintptr_t k = (uintptr_t)arg;
    esp = 0x9000 + (uint32_t)k * 0x400;
    ecx = 0x100; edx = ITER; f_inc_loop();
    esp = 0x9000 + (uint32_t)k * 0x400;
    ecx = 0x200; edx = ITER; f_add_loop();
    esp = 0x9000 + (uint32_t)k * 0x400;
    ecx = 0x300; edx = ITER / 4; f_spin_loop();
    return NULL;
}
static void run_threads(void) {
    pthread_t t[4];
    wr(0x100, 32, 0); wr(0x200, 32, 0); wr(0x304, 32, 0); wr(0x308, 32, 0);
    for (uintptr_t k = 0; k < 4; k++) pthread_create(&t[k], NULL, worker, (void *)k);
    for (int k = 0; k < 4; k++) pthread_join(t[k], NULL);
    if (rd(0x100, 32) != 4 * ITER || rd(0x200, 32) != 12 * ITER || rd(0x308, 32) != ITER) {
        fprintf(stderr, "threads: inc %u/%u add %u/%u spin %u/%u\n", rd(0x100, 32), 4 * ITER,
                rd(0x200, 32), 12 * ITER, rd(0x308, 32), ITER);
        failures++;
    }
}
"""


def _source(threads):
    parts = [HARNESS]
    for name, (code, *_rest) in CASES.items():
        parts.append(translate(code, "f_" + name))
    for name, (code, *_rest) in XCASES.items():
        parts.append(translate(code, "f_" + name))
    for name, code in CMPXCHG_CODE.items():
        parts.append(translate(code, "f_" + name))
    for name, (code, width, mem, flag) in CASES.items():
        parts.append(_case_fn(name, width, mem, flag))
    for name, (code, width, mem, reg) in XCASES.items():
        parts.append(_xcase_fn(name, width, mem, reg))
    parts.append(CMPXCHG_TESTS)
    if threads:
        for name, code in THREAD_CODE.items():
            parts.append(translate(code, "f_" + name))
        parts.append(THREAD_TEST)
    calls = []
    for addr in ("0x1000", "0x2001", "0x3003", "0x4006"):
        calls += ["run_%s(%s);" % (n, addr) for n in list(CASES) + list(XCASES)]
        calls.append("run_cmpxchg_family(%s);" % addr)
    main = ("int main(void) { g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;\n  "
            + "\n  ".join(calls)
            + ("\n  run_threads();" if threads else "")
            + '\n  if (failures) fprintf(stderr, "%d failures\\n", failures);'
            + "\n  return failures != 0; }\n")
    parts.append(main)
    return "\n".join(parts)


class LockedLiftText(unittest.TestCase):
    def test_no_locked_form_is_left_unimplemented(self):
        for name, (code, *_r) in list(CASES.items()) + list(XCASES.items()):
            with self.subTest(name=name):
                c = translate(code, "f")
                self.assertNotIn("RECOMP_UNIMPL", c)
                self.assertIn("RECOMP_ATOMIC_", c)
        for name, code in CMPXCHG_CODE.items():
            with self.subTest(name=name):
                c = translate(code, "f")
                self.assertNotIn("RECOMP_UNIMPL", c)
                self.assertIn("RECOMP_ATOMIC_CAS", c)

    def test_flags_come_from_the_old_value_not_a_reread(self):
        c = translate(CASES["dec_z"][0], "f")
        atomic = c.index("RECOMP_ATOMIC_ADD32")
        self.assertIn("_new = (uint32_t)(_old - 1)", c)
        self.assertNotIn("MEM32(ecx)", c[atomic:])


@unittest.skipUnless(compiler(), "C compiler unavailable")
class LockedLiftCompiled(unittest.TestCase):
    def _build_and_run(self, defines=(), threads=True):
        cc = compiler()
        if is_msvc(cc):
            self.skipTest("the harness uses GNU C (aligned attribute, pthreads)")
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "locked.c"
            src.write_text(_source(threads))
            exe = Path(tmp) / "locked.exe"
            cmd = command(cc, [src], exe, include_dirs=[TEMPLATE], defines=defines,
                          gnu_flags=["-pthread"])
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-4000:])
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
            self.assertEqual(run.returncode, 0, run.stderr[-4000:])

    def test_semantics_and_threads(self):
        self._build_and_run()

    def test_misaligned_fallback_matches(self):
        # Force the alignment test ARM64 hosts use, so the plain
        # read-modify-write fallback runs for the odd addresses.
        self._build_and_run(
            defines=["RECOMP_ATOMIC_ALIGNED_(p,n)=((((uintptr_t)(p))&((uintptr_t)(n)-1u))==0)"],
            threads=False)


if __name__ == "__main__":
    unittest.main()
