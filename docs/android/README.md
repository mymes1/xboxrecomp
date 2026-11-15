# xboxrecomp for Android (arm64-v8a)

An automated pipeline from an Original Xbox disc image you are entitled to use
to a standalone Android APK that contains the **statically recompiled** game
and the xboxrecomp runtime, built for ARM64 with the NDK and presenting
through Vulkan:

```
ISO ─▶ XDVDFS extraction ─▶ default.xbe analysis ─▶ disasm / func_id / ABI ─▶ C codegen
    ─▶ NDK arm64 build (generated C + runtime + Android host layer + Vulkan back end)
    ─▶ Gradle APK (SDL2 activity, launcher UI, game files) ─▶ verification reports
```

There is no CPU emulator in the APK. Guest x86 code is compiled to AArch64
ahead of time; the runtime replaces the Xbox kernel and hardware.

Target device for tuning: Samsung Galaxy Tab A9 (Mali-G57 MC2), Android 15.
`minSdk 26`, `targetSdk 35`.

## Status (honest)

Each phase lists what exists and what has been *demonstrated*, and on what.
"Compiled" is never reported as "works". Everything below was shown on the
self-built test title (`tools/android/testtitle`, no game content); no
commercial title and no physical Android device have been run yet.

| Phase | Scope | State |
|---|---|---|
| 1 | Inspection, dependency automation | **Done**: [INSPECTION.md](INSPECTION.md); every input pinned and verified ([DEPENDENCIES.md](DEPENDENCIES.md)) |
| 2 | ISO download/verify/extract, XBE analysis, game project | **Done** as pipeline stages: intake (path or URL, SHA-256 and/or SHA-1, XISO detection, extraction, `default.xbe`), analysis, per-game project. Demonstrated on the test title |
| 3 | ARM64 code generation | generated C plus runtime build with the NDK for arm64-v8a (CI) and run natively on Linux arm64 (CI: test title self-test 8/8). The `lock` family lowers atomically |
| 4 | Android kernel/runtime | test title: **Runtime initialized** and **Game booted** on the Android emulator, whose ARM translation runs the arm64 code (self-test 8/8, the title's own frames on the scanout). Not yet on a physical device |
| 5 | Vulkan back end | in the tree (`src/vk`), built into `libmain.so`, registered at boot, CPU-rasteriser fallback. Translates the executor's draws: register combiners, four texture stages, blending, depth, stencil, alpha test, colour mask, 16-bit and swizzled surfaces, render to texture. **Matches the CPU rasteriser on SwiftShader** (9 scenes; CI job *vulkan-backend*). In the test-title APK on the Android emulator it initialises at boot on the emulator's virtual GPU (Android Vulkan loader, under ARM translation) and the title runs on unchanged (8/8); the test title draws nothing through the NV2A, so no frame there came from it. The driver's pipeline cache is kept across runs (checked before the driver sees it), so pipelines compiled once are not compiled again at the next start; the saving depends on the driver and is unmeasured until the device run. Not yet run on a phone GPU. Frames still reach the screen through the SDL presenter: there is no Vulkan swapchain presenter yet |
| 6 | Audio, input, assets | game files are packaged in the APK and installed to app storage on first start. Audio output, touch controls, controllers and pause/resume: not started |
| 7 | APK packaging, launcher | APK built, signed and verified in CI (arm64-v8a only, 16 KB-aligned libraries, required symbols present, game files complete), with a launcher activity |
| 8 | Device testing, Mali tuning | not started: needs the Galaxy Tab A9 (Mali-G57) |

Compatibility levels used throughout the reports:

| Level | Meaning | Evidence required |
|---|---|---|
| Compiled | the generated C and runtime link into an arm64 library | build log, `readelf` of the library |
| Runtime initialized | memory layout, kernel, bridge, Vulkan device come up on ARM64 | boot log up to `xbe_entry_point` |
| Game booted | the title's own code runs and presents frames | log + captured frames |
| Gameplay tested | a person played it on a device | a play report |
| Full compatibility | the title is complete and correct | out of scope for automation |

## Documents

- [INSPECTION.md](INSPECTION.md) — what the toolkit does, measured, and what Android needs
- [DEPENDENCIES.md](DEPENDENCIES.md) — every pinned input and how it is verified
- [MODIFICATIONS.md](MODIFICATIONS.md) — every change to upstream files

## Running the pipeline on your disc image

```
python3 -m tools.android.pipeline run --work build/games/mygame \
    --source /path/to/game.iso --sha1 <40 hex digits>      # or --sha256 <64 hex digits>
python3 -m tools.android.pipeline status --work build/games/mygame
```

`--source` is a path, a `file://` URL or an `http(s)://` URL (credentials in
the URL are sent as a header and never logged), to an `.iso`/`.xiso` image or
a `.zip` holding exactly one.

The image is verified at intake against the digests you give:

- `--sha256` and `--sha1` are both accepted. SHA-1 is what disc preservation
  databases such as Redump list for each dump, so it is often the only
  reference you have. SHA-256 is the stronger check: use it when you have it.
- Give either one or both. When both are given, both must match.
- A digest may be plain hex or labelled (`sha1:…`, `SHA-1:…`); letter case
  does not matter. A 40-digit value passed as `--sha256`, or a 64-digit value
  passed as `--sha1`, is refused with a hint to use the other option.
- For a `.zip`, the digests may be those of the zip or of the image inside
  it (databases list the image).
- Without any digest the run continues, but the image is reported as
  **unverified**. Both of its digests are still recorded in
  `reports/intake.json` and in the compatibility report.

In GitHub Actions, start *Android pipeline* by hand with `iso_url` plus
`iso_sha256` and/or `iso_sha1`, or set the `XBR_ISO_URL` repository secret
with `XBR_ISO_SHA256` and/or `XBR_ISO_SHA1`. The inputs and the secrets are
each used as a complete set: with `iso_url` given, no digest is taken from
the secrets.

## Install and build

Needs Linux x86-64, Python 3.10 or newer and git. Everything else is pinned
in `tools/android/deps.lock.json` and verified by SHA-256 (or a pinned git
commit) when it is fetched:

```
bash tools/android/bootstrap.sh              # Python packages, sources, glslang, Gradle, SDK + NDK
source build/android-deps/android-env.sh
python3 -m tools.android.pipeline run --work build/games/mygame \
    --source /path/to/game.iso --sha1 <40 hex digits>
adb install build/games/mygame/out/<package>-<version>.apk
```

`reports/compatibility.md` in the work directory says what was demonstrated
for that title and on what. `python3 -m tools.android.device_run --work
build/games/mygame` installs the APK on a connected device, runs it and
records the boot log, the renderer in use and the frames it presented.

## Testing the Vulkan back end

`tests/nv2a_vk_backend` draws eight scenes twice, with the executor's CPU
rasteriser and with the Vulkan back end, and compares the framebuffers; a
ninth (stencil) is checked against the expected pixels. Without a GPU,
SwiftShader (pinned) is the Vulkan device:

```
bash tools/android/bootstrap.sh --no-sdk --no-gradle --vk-test
source build/android-deps/android-env.sh
cmake -S tests/nv2a_vk_backend -B build/tests/nv2a_vk_backend -G Ninja
cmake --build build/tests/nv2a_vk_backend
VK_ICD_FILENAMES=$XBOXRECOMP_SWIFTSHADER_ICD LD_LIBRARY_PATH=$XBOXRECOMP_SWIFTSHADER_DIR \
    ./build/tests/nv2a_vk_backend/nv2a-vk-backend-test
```

SwiftShader compiles each pipeline in software, so the run takes a few
minutes. `RECOMP_VK_TEST_OUT=<dir>` writes both renderings of every scene as
PPM files. After editing `src/vk/shaders/*`, regenerate the embedded SPIR-V
with `python3 -m tools.android.vk_shaders` (CI checks it with `--check`).

On a device, the boot log names the renderer: `[BOOT] render: vulkan (...)`
with the GPU and driver, or `[BOOT] render: cpu (...)` with the reason. Set
`RECOMP_VK=0` to compare against the CPU rasteriser, and `RECOMP_VK_STATS=60`
for counters and per-pipeline build times
([environment variables](../runtime/environment-variables.md#graphics-vulkan-back-end)).

## Legal

The pipeline never fetches game content on its own. You supply the disc image
(a path, or a URL to a copy you are entitled to use, with its SHA-256 or
SHA-1), and the APK it produces contains that title's code and data: do not
distribute it.
