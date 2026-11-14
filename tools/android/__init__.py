"""Android (arm64-v8a) APK pipeline for xboxrecomp.

ISO -> extraction -> XBE analysis -> static recompilation (the toolkit's own
tools/ pipeline) -> NDK ARM64 build of the generated C plus the runtime ->
Vulkan presentation -> per-game APK. See docs/android/README.md.
"""
