# Android port: modification log

> Part of the [Android port documentation](README.md).

Every change this port makes to a file that existed upstream (`343b6d8`,
v0.17.0) is listed here with its reason and its effect on existing hosts.
New files are listed separately at the end. In source, changed spots carry an
`[android-port]` marker so `git grep "android-port"` finds them all.

Policy: upstream behaviour on Windows, Linux and macOS is preserved. Android
code paths are added under `#if defined(__ANDROID__)`, and shared fixes are
made only where the old code was wrong or over-constrained on every host.

## Changes to upstream files

| # | File | Change | Why | Effect on existing hosts |
|---|---|---|---|---|
| 1 | `src/kernel/CMakeLists.txt` | `find_package(OpenSSL REQUIRED)` → `QUIET`; link `OpenSSL::Crypto` only when found and not Android | No kernel source uses OpenSSL (`kernel_crypto.c` carries its own SHA-1/RC4/HMAC), and the NDK has no OpenSSL. | None where OpenSSL is installed (still linked). Hosts without OpenSSL now configure instead of failing. |
| 2 | `tools/recomp/lifter.py` | New `Lifter._lift_locked`: every lock-prefixed read-modify-write on memory (add/sub/inc/dec/and/or/xor/not/neg/adc/sbb/bts/btr/btc/xchg, 8- and 16-bit xadd/cmpxchg, cmpxchg8b) lowers to one `RECOMP_ATOMIC_*` operation, flags derived from the returned old value; `xchg` with a memory operand (implicitly locked on x86) is atomic too; flag tracking in `lift_basic_block` covers the whole family | These lifted to `RECOMP_UNIMPL` (a logged no-op): a `lock inc` counter never moved, and an `xchg` spin lock was a plain load+store. With guest threads on host threads both are wrong on every host. | Generated code changes only for these instructions, which were broken before. 32-bit `lock xadd`/`lock cmpxchg` output is byte-identical. |
| 3 | `tools/recomp/translator.py` | `_function_needs_cf`, `_flag_uses`, `_flags_read` and the `_fa/_fb` declaration test strip the `lock ` prefix (and know `cmpxchg8b`) | Flag analysis must see `lock add` as `add`. | None for code without these instructions. |
| 4 | `templates/runtime/recomp_types.h` | 8/16/32-bit `RECOMP_ATOMIC_{ADD,AND,OR,XOR,XCHG,CAS}*` and `RECOMP_ATOMIC_CAS64`; on GCC/Clang through `__atomic` builtins with a misaligned fallback on non-x86 hosts | ARM64 faults on a misaligned atomic where x86 does a split lock. | MSVC: intrinsics as before plus the new ones. GCC/Clang on x86: same instructions as the old `__sync` builtins. |

## Tests deliberately not changed

`python3 -m pytest tools/` on Debian 12 / GCC 12 reports six failures that
predate this port and are host-toolchain specific (see
[INSPECTION.md §2](INSPECTION.md#2-the-pipeline-stage-by-stage)). They are left
as they are because they do not concern the pipeline's behaviour, and the CI
workflow runs the suite on Ubuntu with the same expectations upstream has.

## New files

| Path | Purpose |
|---|---|
| `tools/android/deps.lock.json` | every pinned dependency: Android SDK packages, NDK r27d, JDK, Gradle + AGP, SDL2/Vulkan-Headers/extract-xiso commits, glslangValidator archive |
| `tools/android/requirements-pipeline.txt`, `requirements-dev.txt` | hash-locked Python inputs |
| `tools/android/fetch_deps.py`, `bootstrap.sh` | fetch-and-verify for all of the above; writes `build/android-deps/{deps-manifest.json,android-env.sh}` |
| `tools/android/iso.py` | disc intake: acquire (path/URL, credentials moved to a header, never logged), SHA-256 verification, XDVDFS detection, traversal-safe extraction with a manifest, `default.xbe` lookup |
| `tools/android/xiso_build.py` | XDVDFS writer (balanced directory trees, sector-safe entries) for fixtures and the test title |
| `tools/android/xbe_info.py` | structured XBE analysis on top of `tools.xbe_parser` + `tools.kernel_audit.coverage`; title-image (XPR0: DXT1/3/5, swizzled ARGB) decoding to PNG |
| `tools/android/compat.py` | compatibility report with evidence-gated levels |
| `tools/android/pipeline.py` | the staged ISO -> APK driver |
| `tools/android/testtitle/` | a self-built Xbox test title (C, built with the pinned zig), `pe2xbe.py`, disc packing; original code, no game content |
| `tools/android/test_*.py` | tests for the above |
| `tools/recomp/test_lifter_locked_rmw.py` | translates, compiles and runs every locked form against the x86 definition (memory, flags, misaligned path) plus a 4-thread lost-update test |
| `docs/android/*.md` | this documentation |
