# Android port: inspection of the toolkit (Phase 1)

> Part of the [Android port documentation](README.md). Written from reading the
> tree at `343b6d8` (v0.17.0) and running its tests, before any Android code was
> added. Every claim below names the file it comes from.

## 1. What the toolkit is, and what it is not

xboxrecomp is a **static recompiler plus a host runtime**. The Python half
(`tools/`) turns an XBE's x86 code into C; the C half (`src/`) replaces the
Xbox kernel, GPU, APU, USB and so on at link time. The repository builds
libraries only: the executable is produced by a *game project* (copied from
`templates/new-game/`) that links the generated C with the runtime
(`README.md`, "This repo builds libraries only").

No part of it emulates an x86 CPU at run time. Guest code becomes native
code of whatever host compiles the generated C. That is what makes an
ARM64 Android target possible at all: the generated C is portable, so the
question is whether the *runtime* and the *generated C's support header*
build and behave on Android/bionic/arm64.

## 2. The pipeline, stage by stage

| Stage | Tool | Output | Notes for automation |
|---|---|---|---|
| XBE parse | `python3 -m tools.xbe_parser X --json <dir>/<stem>_analysis.json` | header, sections, kernel imports, libraries, certificate | **The JSON must sit beside the XBE with exactly that name**; later stages look for it there. |
| Disassembly | `python3 -m tools.disasm X` (capstone) | `functions.json`, `xrefs.json`, `strings.json`, `labels.json` in `tools/disasm/output/` | `--text-only` limits it to `.text`; without it every executable section is covered. Output dir is fixed under `tools/`; the pipeline must copy it out per game. |
| Function ID | `python3 -m tools.func_id X` | `identified_functions.json` | names CRT/XDK library functions. |
| ABI | `python3 -m tools.abi_analysis X` | `abi_functions.json` | if absent every function is treated as cdecl with 0 params. |
| Codegen | `python3 -m tools.recomp X --all --split N --gen-dir D` (plus `--disasm-dir/--func-id-dir/--abi-dir`) | `recomp_NNNN.c`, `recomp_dispatch.c`, `recomp_funcs.h`, `recomp_stubs.c`, `recomp_types.h` | `recomp_types.h` is copied from `templates/runtime/` only if absent. Untranslatable instructions become a `/* mnemonic */` comment (and, with `RECOMP_UNIMPL_TRAP`, a trap); unresolved call targets become stubs in `recomp_stubs.c`. `--split 1000` exhausted 15 GB on a large title: keep N around 250. |
| Kernel coverage | `tools/kernel_audit` | which imported ordinals the runtime implements | used for the compatibility report. |
| ISO | `tools/xiso` (`ls`, `get`, `unpack`) | files from an XDVDFS image | pure Python; probes five known game-partition offsets including redump full-disc images. |

Measured here: on the synthetic `tools/conformance/test.xbe` the parser runs;
`python3 -m pytest tools/` gives **981 passed, 6 failed, 7 skipped** on this
Debian/GCC 12 host. All six failures are host-toolchain artefacts, not pipeline
defects: five `test_reserved_idents` subtests expect glibc's `<math.h>` to
reject `void isnan(void);` (glibc 2.36 with GCC 12 accepts it), and one
frndint runtime test passes `-lm` *before* the source file
(`tools/recomp/c_fixture.py`), which Debian's `--as-needed` linker drops. See [MODIFICATIONS.md](MODIFICATIONS.md)
for whether they were touched (they were not).

## 3. The generated C and its support header

`templates/runtime/recomp_types.h` is the contract between generated code and
the runtime:

- Guest registers are globals with `RECOMP_TLS` (`__thread`) storage, so every
  guest thread has its own set. **Android consequence:** bionic supports ELF
  TLS only from API 29. Below that the NDK silently switches to *emulated* TLS,
  where every register access is a call to `__emutls_get_address`. A minSdk-26
  APK therefore needs either two builds of the game library (ELF TLS for
  API ≥ 29, emulated TLS for 26–28) or to accept the slow path everywhere. The
  target device (Android 15) takes the fast one.
- Memory access is `MEM32(va)` = `*(volatile uint32_t *)((uint32_t)va + g_xbox_mem_offset)`:
  guest memory is one flat host mapping at a runtime-chosen offset. Nothing
  assumes a fixed host address, which suits Android's ASLR.
- SSE/MMX helpers use x86 intrinsics only under `__i386__/__x86_64__/_M_IX86/_M_X64`
  and have portable C fallbacks otherwise (`XMM_CVT_PS2DQ`, `MMX_CVT_F2I`, the
  `rcp/rsqrt` approximations). The arm64 path is the fallback path; the
  toolkit already runs on Apple Silicon and Linux arm64 with it.
- x87 is modelled as a `double` stack with explicit control word
  (`recomp_frndint`, `recomp_fist`, `recomp_f80_load/store`), so precision is
  64-bit, not 80-bit, on every host (a documented, host-independent limitation).

## 4. The runtime libraries

| Library | Dir | POSIX status | Android notes |
|---|---|---|---|
| `platform` | `src/platform` | `win32_compat.c` implements threads, events, waits, file mapping, `VirtualAlloc` over pthreads/mmap | `memfd_create` (bionic: API 30+, not declared at 26), `pthread_cancel` (absent from bionic), `pthread_setaffinity_np` (absent; `sched_setaffinity(gettid)` exists), `explicit_bzero` need Android paths. `mmio_decode_a64.h` already decodes AArch64 loads/stores from a Linux `ucontext`. |
| `xbox_kernel` | `src/kernel` | complete: 371 ordinals routed (`kernel_bridge.c`), memory layout, file I/O with path translation (`kernel_path.c`), threads, sync, HAL, crypto | linked OpenSSL although no source uses it (fixed, see MODIFICATIONS). `kernel_rtl.c` uses `backtrace()` (bionic: API 33). |
| pushbuffer executor | `src/kernel/nv2a_pb_*.c`, `nv2a_vsh_interp.c`, `nv2a_combiner.c` | complete CPU path: DMA walk, vertex programs, fixed-function T&L, combiners, 4 stages, threaded rasteriser | **the universal graphics path**: a title that statically links XDK D3D needs no per-title D3D mapping. `nv2a_backend.h` lets a GPU back end receive decoded batches. |
| `xbox_d3d8` | `src/d3d` | D3D11 on Windows (complete); `d3d8_gl.c` on POSIX (first cut, GL 3.3 + libepoxy) | used only by titles a project maps onto the toolkit's D3D8 by hand. Not needed for the automated path (`XBOXRECOMP_BUILD_D3D8=OFF`). |
| `xbox_nv2a` | `src/nv2a` | register model, MMIO hook, `pgraph_d3d11` (Windows) | register semantics used by the kernel's memory layout. |
| `xbox_apu`, `xbox_audio` | `src/apu`, `src/audio` | xemu MCPX APU (LGPL) with `apu_sdl2.c` output; DirectSound software mixer | SDL2's Android audio (AAudio/OpenSL ES) serves `apu_sdl2.c` unchanged. |
| `xbox_usb` | `src/usb` | OHCI host controllers so XAPI finds a pad | reads `xbox_InputGetState`. |
| `xbox_input` | `src/input` | SDL2 GameController backend + keyboard stand-in, merged on port 0 | Android: Bluetooth/USB pads arrive through SDL; touch needs a provider merged the same way as the keyboard. |
| `xbox_video` | `src/video` | `fb_present_sdl.c`: SDL window on the main thread, guest on a pthread, triple-buffered frame hand-off from the executor's flip | the shape Android needs (SDL owns the UI thread there too), but it presents with `SDL_Renderer` (GLES on Android). Replaced by a Vulkan presenter on Android. |

Host tests run here (Release, GCC 12, x86-64, SDL 2.32.10 built from the pinned
source with dummy drivers): 20 POSIX test projects pass —
`win32_compat_posix win32_handles_posix win32_thread_posix win32_waitable_timer_posix
kernel_memory_posix kernel_memory_bridge_posix kernel_directory_posix kernel_path
kernel_guest_cpu mmio_decode_a64 nv2a_combiner nv2a_ffp_backend nv2a_pb_constants
nv2a_pb_walk nv2a_raster_batch nv2a_tex_filter nv2a_vsh usb_ohci_posix flip_hold`
and `memory_layout_posix` (which crashed once under ctest and then passed 30/30
direct runs: recorded as flaky). `fb_present` is Windows-only by design.

## 5. Memory model on a 64-bit Android process

`xbox_memory_layout.c` reserves one span (POSIX: the 64 MB RAM image plus 28
mirrors, about 1.86 GB; `RECOMP_EXT_VMA` reserves 4 GB), then maps the fixed
windows at guest `0x80000000` (contiguous), `0xF0000000` (tiled alias),
`0xFD000000` (NV2A, 16 MB), `0xFE800000` (MCPX, 8 MB) and `0xFF000000` (flash).
Mirrors are true aliases: one shared-memory object mapped several times
(`CreateFileMappingA` + `MapViewOfFileEx` in `win32_compat.c`). On Android:

- address space: arm64 processes have at least 39 bits (512 GB) of VA, so a
  4 GB reservation is unremarkable;
- aliasing: `memfd_create` is API 30 in bionic, but **`ASharedMemory_create`
  (libandroid, API 26)** returns an fd that `mmap(MAP_SHARED)` can map any
  number of times — the same aliasing, available at minSdk 26. Its size is
  fixed at creation (no `ftruncate`);
- 16 KB pages: the layout code already handles non-4 KB host pages (Apple
  Silicon), and Android 15 devices may run 16 KB kernels; the TIB sits at
  guest `0x4000` for exactly this reason;
- faults: hardware registers are trapped by leaving pages inaccessible and
  servicing `SIGSEGV`/`SIGBUS` through `xbox_PosixMmioFault(ucontext, addr)`.
  ART installs its own `SIGSEGV` handler; app handlers installed with
  `sigaction` are chained by libsigchain, so this works inside an APK.

## 6. Graphics: what to reuse for Vulkan

Two paths exist and the automated pipeline can rely on only one of them:

1. **Toolkit D3D8 layer** (`src/d3d`): a project replaces the title's D3D calls
   with the toolkit's COM implementation. Needs per-title function mapping;
   the POSIX version is a GL first cut.
2. **Pushbuffer executor** (`RECOMP_PB_EXEC`): the title's own statically
   linked XDK D3D runs as recompiled code and writes NV2A methods; the
   executor decodes them. This works for any title without per-title work,
   so it is the path the Android pipeline uses.

The executor already does the hard decoding on the CPU: vertex programs,
fixed-function transform and lighting, near-plane clipping, texture formats
(swizzled, linear, DXT: `nv2a_backend_decode_texture`). Through
`nv2a_backend.h` a back end receives clears, triangle lists in surface pixels
with `1/w`, diffuse colour and texel UVs, stage-0 texture state, and blend,
alpha-test, depth, stencil, cull and colour-mask state in NV2A enums, plus a
`present_sync` hook for readback before the window snapshot. Register
combiners and texture stages 1–3 are **not** passed on (the header's own
"ponytail" note); the CPU rasteriser has them.

Decision: the Vulkan renderer is an `Nv2aBackend` (clear/draw/flip + present
sync) plus a Vulkan swapchain presenter that also shows CPU-rasterised frames.
That translates the title's actual NV2A work into Vulkan draws, reuses the
executor's decoding instead of duplicating it, and leaves the CPU rasteriser
as a reference/fallback path selectable at run time.

## 7. Build system facts the Android build depends on

- Top-level `CMakeLists.txt`: C11; `XBOXRECOMP_BUILD_D3D8` (default ON) — a
  parent may define `xbox_d3d8` itself; tests only in a top-level build; the
  umbrella `xboxrecomp` INTERFACE target wraps the cyclic static libraries in
  `-Wl,--start-group` on non-Apple UNIX, which also covers Android's lld.
- POSIX dependencies: SDL2 (apu, input, video, d3d), libepoxy (d3d GL only),
  Threads, libm; OpenSSL no longer required.
- Boot order (from `templates/new-game/src/main.c` and the Linux/macOS port
  apexJCL/blinx2-recomp): load XBE → `xbox_MemoryLayoutInit` →
  `g_xbox_mem_offset = xbox_GetMemoryOffset()` → optional
  `mcpx_apu_init_standalone` → `xbox_kernel_init` → `xbox_path_init(game, save)`
  → `xbox_kernel_bridge_init` → `g_esp = XBOX_STACK_TOP` →
  `recomp_dispatch_init` → `xbox_WatchdogStart` → `xbe_entry_point()`. On POSIX
  the process main thread keeps the window (`xbox_HostWindowMain`) and the
  guest runs on a large-stack pthread.
- The game provides `recomp_lookup`/`recomp_lookup_manual`; the runtime owns
  `g_icall_trace*`/`g_icall_count`.

## 8. Gaps this inspection found (and where they are handled)

| Gap | Effect on Android | Handled in |
|---|---|---|
| No `__ANDROID__` anywhere | first NDK build fails in `win32_compat.c` | Phase 4 (runtime port) |
| ELF TLS needs API 29 | minSdk 26 build would use emulated TLS for every register access | Phase 3/7: dual ABI-identical libraries, chosen at load time |
| `fb_present_sdl.c` presents with `SDL_Renderer` | not Vulkan | Phase 5: Vulkan presenter |
| Back end sees stage 0 only | multi-texture/combiner effects need the CPU path | Phase 5: documented limitation; extend `Nv2aBatch` incrementally |
| Game files read as host files | APK assets are zip entries | Phase 6/7: verified first-run extraction to app storage |
| `templates/new-game` is Windows-only | no Android game project | Phase 2/7: generated Android project |
| Output dirs of disasm/func_id/abi are fixed under `tools/` | one game at a time per checkout | pipeline copies them into the per-game work dir |
