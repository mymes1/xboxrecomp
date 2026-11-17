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
self-built test title (`tools/android/testtitle`, no game content). One
commercial title has been through the pipeline, in a user's manual CI run.
It recompiled and built for arm64-v8a (Compiled). Its desktop host run
reached the entry point and crashed 4 s later, so it has not been run on
Android. No physical Android device has been used yet.

| Phase | Scope | State |
|---|---|---|
| 1 | Inspection, dependency automation | **Done**: [INSPECTION.md](INSPECTION.md); every input pinned and verified ([DEPENDENCIES.md](DEPENDENCIES.md)) |
| 2 | ISO download/verify/extract, XBE analysis, game project | **Done** as pipeline stages: intake (path or URL, SHA-256 and/or SHA-1, XISO detection, extraction, `default.xbe`), analysis, per-game project. Demonstrated on the test title |
| 3 | ARM64 code generation | generated C plus runtime build with the NDK for arm64-v8a (CI) and run natively on Linux arm64 (CI: test title self-test 8/8). The `lock` family lowers atomically |
| 4 | Android kernel/runtime | test title: **Runtime initialized** and **Game booted** on the Android emulator, whose ARM translation runs the arm64 code (self-test 8/8, the title's own frames on the scanout). Not yet on a physical device |
| 5 | Vulkan back end | in the tree (`src/vk`), built into `libmain.so`, registered at boot, CPU-rasteriser fallback. Translates the executor's draws: register combiners, four texture stages, blending, depth, stencil, alpha test, colour mask, 16-bit and swizzled surfaces, render to texture. **Matches the CPU rasteriser on SwiftShader** (9 scenes; CI job *vulkan-backend*). In the test-title APK on the Android emulator it initialises at boot on the emulator's virtual GPU (Android Vulkan loader, under ARM translation) and the title runs on unchanged (8/8); the test title draws nothing through the NV2A, so no frame there came from it. The driver's pipeline cache is kept across runs (checked before the driver sees it), so pipelines compiled once are not compiled again at the next start; the saving depends on the driver and is unmeasured until the device run. Not yet run on a phone GPU. **Frames reach the screen through a Vulkan swapchain** on the same device (`VK_KHR_surface`/`VK_KHR_android_surface`/`VK_KHR_swapchain`, each tested before use; the SDL renderer is only the fallback): on the Android emulator the swapchain is made on the virtual GPU and a screenshot shows the title's picture, letterboxed 4:3, colours and orientation right, before and after the app went to the background (CI screen check); on SwiftShader a headless-surface test checks 1:1 output in both pixel formats, letterboxing, resize, suspend/resume and the renderer drawing on the presenter's device. The window thread's upload/blit/present overlaps the game thread. Not done: the renderer still writes each frame back to guest memory and waits for the GPU at the flip (no CPU/GPU overlap inside the renderer), and frames are not pre-rotated for a quarter-turn display (the compositor rotates them) |
| 6 | Audio, input, assets | game files are packaged in the APK and installed to app storage on first start. A game over 3.75 GiB (an APK cannot exceed 4 GiB) is built without them. Its files are imported on first start from the user's zip of the game's files, through the system file picker or over adb (`content write`), and each file is checked against its SHA-256. On the emulator the test title's files were sent over adb and imported, and the title booted from them (CI). Neither the picker nor a multi-GB import has been exercised yet. **Input**: titles read pads through the emulated USB host controller, which no POSIX host started before: the boot now does (with `RECOMP_USB`, which the launcher sets by default). An **on-screen controller** (sticks, D-pad, face buttons, triggers, Back/Start; hidden while a gamepad is connected) is merged into controller port 0 beside any real pad: on the emulator, presses on A, the left stick and Start sent with `input swipe` reach port 0 (CI). Bluetooth/USB controllers go through SDL's game controllers (the upstream path); not yet tried with a physical controller, and no real game has yet been seen reading a pad through the USB model on Android. **Pause/resume**: in the background the presenter lets go of the surface and the guest is held at its next flip; on the emulator the game survives HOME and returns, on screen, when the icon is tapped (CI). Back asks before quitting. **Audio**: the emulated APU's SDL output (upstream) opens on Android (emulator: OpenSL ES, 48 kHz stereo); it stays opt-in (launcher: Sound) because without the DSP handshake some titles stall in audio init. No title has yet played sound on Android |
| 7 | APK packaging, launcher | APK built, signed and verified in CI (arm64-v8a only, 16 KB-aligned libraries, required symbols present, game files complete, or listed with their SHA-256 for the import), with a launcher activity |
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
the URL are sent as a header and never logged), to one of:

- an `.iso`/`.xiso` disc image;
- a `.zip` holding exactly one such image;
- a `.zip` of the disc's files, the way extract-xiso (or any XISO extractor)
  leaves them, with `default.xbe` at the zip's root. That is what you get by
  zipping the *contents* of the extracted folder:

  ```
  extract-xiso -x game.iso                 # makes game/ with default.xbe in it
  cd game && zip -r ../game-files.zip .    # default.xbe at the zip's root
  python3 -m tools.android.pipeline run --work build/games/mygame \
      --source ../game-files.zip --sha1 "$(sha1sum ../game-files.zip | cut -c1-40)"
  ```

  A zip of the folder itself (`game/default.xbe`, one folder at the root) is
  also accepted. Archiver leftovers (`__MACOSX/`, `._*`, `.DS_Store`,
  `Thumbs.db`, `desktop.ini`) are ignored, and `\` separators written by some
  Windows archivers are read as `/`. Every entry is checked before anything is
  written: no absolute paths, no `..`, no names that differ only in letter
  case, no symbolic links or encrypted entries. Each file's size and CRC are
  checked, and free disk space is checked first. The game files and the
  manifest are the same as from the image of the same disc, so every later
  stage works unchanged. CI checks this on the test title: it unpacks the
  image with the pinned extract-xiso, zips the result, and compares the
  extracted files, the generated C and `xbr_game.json` with those from the
  image.

  The zip only needs to be readable by Python's `zipfile`, which means Store
  or Deflate (most archivers' default), plus bzip2 and LZMA. Deflate64
  (compression method 9, which some Windows tools produce for large files)
  cannot be read, and intake says so.

The image is verified at intake against the digests you give:

- `--sha256` and `--sha1` are both accepted. SHA-1 is what disc preservation
  databases such as Redump list for each dump, so it is often the only
  reference you have. SHA-256 is the stronger check: use it when you have it.
- Give either one or both. When both are given, both must match.
- A digest may be plain hex or labelled (`sha1:…`, `SHA-1:…`); letter case
  does not matter. A 40-digit value passed as `--sha256`, or a 64-digit value
  passed as `--sha1`, is refused with a hint to use the other option.
- For a `.zip` holding an image, the digests may be those of the zip or of
  the image inside it (databases list the image). A zip of the disc's files
  has no image, so its digests are the zip's own. Disc databases list the
  image's digest, so for a zip you made yourself, take the zip's digest when
  you make it (as above) and keep the image's digest as the record of what
  you dumped.
- Without any digest the run continues, but the image is reported as
  **unverified**. Both of its digests are still recorded in
  `reports/intake.json` and in the compatibility report.

In GitHub Actions, start *Android pipeline* by hand with `iso_url` plus
`iso_sha256` and/or `iso_sha1`, or set the `XBR_ISO_URL` repository secret
with `XBR_ISO_SHA256` and/or `XBR_ISO_SHA1`. The inputs and the secrets are
each used as a complete set: with `iso_url` given, no digest is taken from
the secrets. `iso_url` may point to any of the three kinds of input above,
including a zip of the disc's files. The URL need not end in `.zip`: a zip is
recognised by its content.

Large games: the game's files normally ride inside the APK, and an APK must
stay under 4 GiB because apksigner cannot sign ZIP64 archives. A game with
more than 3.75 GiB of files (the extracted files, not the image, which
includes padding) is therefore built **without** its files
(`--game-files auto`, the default; `bundle` and `external` force either).
The APK then carries the recompiled code, the runtime and a list of every
file with its size and SHA-256. On the first Play, the launcher imports the
files from your zip of the game's files (see *Playing on the device*), and
refuses any file that differs from the one the code was recompiled from.
Intake already notes this (`fits_in_apk` in `reports/intake.json`), and
`apk_verify.json` reports `game_files.mode`.

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

The same project builds `nv2a-vk-present-test`: the window presenter on a
headless surface (`VK_EXT_headless_surface`, which SwiftShader offers), run
the same way.

SwiftShader compiles each pipeline in software, so the run takes a few
minutes. `RECOMP_VK_TEST_OUT=<dir>` writes both renderings of every scene as
PPM files. After editing `src/vk/shaders/*`, regenerate the embedded SPIR-V
with `python3 -m tools.android.vk_shaders` (CI checks it with `--check`).

On a device, the boot log names the renderer: `[BOOT] render: vulkan (...)`
with the GPU and driver, or `[BOOT] render: cpu (...)` with the reason; and
the presenter: `[BOOT] present: vulkan (swapchain ...)` or `[BOOT] present:
sdl (...)` with the reason. Set
`RECOMP_VK=0` to compare against the CPU rasteriser, and `RECOMP_VK_STATS=60`
for counters and per-pipeline build times
([environment variables](../runtime/environment-variables.md#graphics-vulkan-back-end)).

## Playing on the device

- **Game files for a large game** (APK built without them; the launcher says
  so): tap Play, then *Choose zip*, and pick the zip of the game's files.
  This is the same kind of zip the pipeline takes: `default.xbe` at its root
  or in its one folder. The system file picker can open it from Downloads, a
  memory card or a cloud drive. The import needs room for the extracted
  files (as much again as the game's size). It checks every file against
  the SHA-256 recorded at build time, so a different version or region of
  the game is refused with the file that differs. From a computer instead,
  with USB debugging on:
  `adb exec-in content write --uri content://<package>.gamezip/game.zip < game.zip`
  (the app accepts this from adb only, and keeps the zip in its private
  storage). Then tap Play: the launcher imports the zip and deletes it. Up to
  Android 10, `adb push game.zip /sdcard/Android/obb/<package>/` also works,
  after the app has been started once; since Android 11 adb cannot reach
  that folder.
- **Controls**: with no controller connected, an on-screen controller is
  drawn over the game (Options -> On-screen controller: when no controller
  is connected / always / never). A tap on a stick without moving clicks it.
  Bluetooth and USB controllers are read through SDL; the on-screen one
  hides while one is connected. Options -> Controllers (emulated USB pad)
  must stay on for a game to see any controller.
- **Picture**: 4:3 with black bars by default; 16:9 for games set to
  widescreen, or fill the screen (Options -> Picture).
- **Leaving**: Back asks before it ends the game. Home keeps it: the game
  is held while in the background and continues when you return (from
  Recents or the icon).
- **Sound**: Options -> Sound turns on the emulated audio processor. Some
  games stall in their audio start-up with it; if a game stops at a black
  screen, turn it off.
- **Logs**: the launcher's "Last log" shows the runtime's log of the last
  run (`Android/data/<package>/files/logs/last_run.log`).

## Legal

The pipeline never fetches game content on its own. You supply the disc image
(a path, or a URL to a copy you are entitled to use, with its SHA-256 or
SHA-1), and the APK it produces contains that title's code (recompiled) and,
when the files are bundled, its data: do not distribute it.
