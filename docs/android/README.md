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

Each phase lists what exists and what has been *demonstrated*. "Compiled" is
never reported as "works".

| Phase | Scope | State |
|---|---|---|
| 1 | Inspection, dependency automation | **Done**: [INSPECTION.md](INSPECTION.md); pinned and verified inputs ([DEPENDENCIES.md](DEPENDENCIES.md)) |
| 2 | ISO download/verify/extract, XBE analysis, game project | intake/analyze/recompile stages work on the self-built test title (`python3 -m pytest tools/android`); the game-project stage arrives with Phase 4 |
| 3 | ARM64 code generation | not started |
| 4 | Android kernel/runtime | not started |
| 5 | Vulkan back end | not started |
| 6 | Audio, input, assets | not started |
| 7 | APK packaging, launcher | not started |
| 8 | Device testing, Mali tuning | not started |

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

## Legal

The pipeline never fetches game content on its own. You supply the disc image
(a path, or a URL to a copy you are entitled to use, with its SHA-256), and
the APK it produces contains that title's code and data: do not distribute it.
