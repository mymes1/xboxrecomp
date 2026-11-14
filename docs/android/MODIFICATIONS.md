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
| 5 | `src/platform/win32_compat.c` | Under `__ANDROID__`: thread affinity through `sched_{get,set}affinity` on the thread's kernel id (bionic has no `pthread_*affinity_np`); `TerminateThread` signals the handle and says once that the host thread cannot be cancelled (no `pthread_cancel`; nothing in the runtime calls it); `SecureZeroMemory` as `memset` + compiler barrier (no `explicit_bzero`); the file-mapping descriptor from the `memfd_create` system call (bionic wraps it only from API 30) with an `ASharedMemory_create` fallback (API 26), sized at creation since ashmem refuses `ftruncate`. On every POSIX host: `VirtualAlloc` with an address now refuses a different result even where `MAP_FIXED_NOREPLACE` is defined, and checks before `view_register` | Found by compiling the runtime against bionic's API-26 headers (`native.py --mode check`). Kernels before 4.17 (Android 8-10 devices) ignore `MAP_FIXED_NOREPLACE` and return another address, which `VirtualAlloc` handed back as if it were the requested one; the old fallback path also left a stale view registration behind | Linux/macOS: identical except the address check, which cannot trigger on a kernel that honours `MAP_FIXED_NOREPLACE`. Windows: file not compiled |
| 6 | `src/platform/CMakeLists.txt` | On Android, `platform` links `android` and `log` | `ASharedMemory_create` (libandroid); liblog for the host layer | None elsewhere (`if(ANDROID)`) |

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
| `tools/android/project.py` | `project` stage: `<work>/project/xbr_game.json` (per-game configuration: package, RAM, title env defaults, Android SDK levels; profile + saved edits merged) and the native CMake project with `game/xbr_game_config.c` |
| `tools/android/native.py` | `host` and `native` stages: desktop build + headless run of the title (`host_build.json`, `host_runtime.json`, scanout BMP), NDK arm64-v8a build with ELF checks (`native_build.json`), compile-only bionic check (`native_check.json`) |
| `tools/android/elfinfo.py` | dependency-free ELF reader: machine, TLS kind, alignment, NEEDED, dynamic symbols, Android API note |
| `tools/android/ci_report.py` | job summary + check run from a work directory |
| `src/host/xbr_host.h`, `xbr_boot.c` | the POSIX boot sequence every title shares (crash handler with MMIO traps first, XBE load, memory layout, APU, kernel, paths, bridge, init flags, dispatch, watchdog, entry), each step a `[BOOT]` line; title env defaults |
| `src/host/xbr_game_glue.c` | weak defaults for the hooks a game supplies (`recomp_lookup_manual`, ICALL/UNIMPL loggers, `xbr_game_config`) |
| `src/host/xbr_stats.c` | flips and scanout surface seen through `ld --wrap` of two presenter entry points; `[STATUS]` line, scanout checksum and BMP |
| `src/host/xbr_main_posix.c` | desktop entry (CI runs titles on Linux x86-64 and arm64 with it) |
| `src/host/android/` | `SDL_main` (paths from the activity's arguments, landscape, fullscreen presenter), stdout/stderr to logcat + a log file |
| `templates/xbr-game/native/CMakeLists.txt` | one project for NDK (`libmain.so` + SDL2 from source), desktop executable, and the compile-only check |
| `cmake/sdl2-subproject/SDL2Config.cmake` | lets the toolkit's `find_package(SDL2)` resolve to an SDL2 built in the same tree |
| `.github/workflows/android.yml` | test title -> NDK arm64-v8a build + Linux x86-64 run; Linux arm64 run; manual job for your own image. Replaces the temporary `android-env-probe.yml` |
| `tools/android/test_*.py` | tests for the above |
| `tools/recomp/test_lifter_locked_rmw.py` | translates, compiles and runs every locked form against the x86 definition (memory, flags, misaligned path) plus a 4-thread lost-update test |
| `docs/android/*.md` | this documentation |
