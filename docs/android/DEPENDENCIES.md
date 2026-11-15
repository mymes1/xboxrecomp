# Android port: dependencies

> Part of the [Android port documentation](README.md).

All pins live in [`tools/android/deps.lock.json`](../../tools/android/deps.lock.json);
`tools/android/fetch_deps.py` is the only code that downloads anything.

```bash
bash tools/android/bootstrap.sh          # everything (needs ~4 GB for the SDK/NDK)
bash tools/android/bootstrap.sh --no-sdk # host-side work only
source build/android-deps/android-env.sh
```

| Input | Pin | Verification | Used for |
|---|---|---|---|
| Android NDK | r27d `27.3.13750724` (LTS) | sdkmanager checks Google's manifest checksum; `source.properties` revision read back | arm64 compiler, sysroot, `glslc` |
| Android SDK | `platforms;android-35`, `build-tools;35.0.0`, `cmake;3.31.5` | same | compileSdk/targetSdk 35, aapt2, apksigner, zipalign |
| SDK command-line tools | `15859902` | SHA-256 of the zip as published on developer.android.com/studio (only fetched when no SDK has the pinned packages) | sdkmanager |
| JDK | Temurin 17 | `actions/setup-java` / host package | Gradle |
| Gradle | 8.11.1 | SHA-256 of the distribution, equal to Gradle's published `.sha256` (also in the generated `gradle-wrapper.properties`) | APK build |
| Android Gradle Plugin | 8.7.3 | Gradle resolves it from Google Maven by exact version | APK build |
| SDL2 | `release-2.32.10` = `5d249570393f…` | git commit id compared after checkout | window, Vulkan surface, input, audio, `SDLActivity` |
| Vulkan-Headers | `v1.3.296` = `29f979ee5aa5…` | commit id | the Vulkan back end (`src/vk`): passed to every build by `native.py`; the NDK's own headers are the fallback |
| extract-xiso | `build-202609111233` = `3f5b62cfe68f…` | commit id | optional second extractor, test-image creation |
| bionic headers (dev only) | aosp-mirror/platform_bionic `731631f30009…`, sparse `libc/include` + kernel uapi | commit id | compile-only API-26 check where no NDK can be installed (`native.py --mode check`); never linked |
| liblog headers (dev only) | LineageOS/android_system_logging `336e1707c825…`, sparse `liblog/include` | commit id | `<android/log.h>` for the same check |
| glslangValidator | 11.7.0 (npm `glslang-validator-prebuilt-predownloaded@0.0.2`) | SHA-256 + npm sha512 integrity | regenerating the committed SPIR-V `src/vk/nv2a_vk_spv.h` on x86-64 Linux (`tools.android.vk_shaders`; builds do not need it) |
| SwiftShader (test only) | 5.0.0 with the Khronos loader, from npm `@sparticuz/chromium@153.0.0` | SHA-256 of the tarball (equal to npm's sha512 integrity), of the brotli'd tar inside, and of both unpacked libraries | a Vulkan device on the CPU for `tests/nv2a_vk_backend` where there is no GPU (CI, development); never in a build or an APK |
| brotli (test only) | 1.2.1 | `pip --require-hashes` | unpacks the SwiftShader archive |
| capstone, cmake, ninja | 5.0.9, 3.31.6, 1.13.0 | `pip --require-hashes` | disassembler, build tools |
| pytest & co, pefile, ziglang | see `requirements-dev.txt` | `pip --require-hashes` | tests, ARM64 cross-compile checks |

Outputs: `build/android-deps/deps-manifest.json` records what was resolved
(paths, commits, hashes, the lock file's own hash) and is uploaded with every
CI run's logs.

The hosted GitHub runner (`ubuntu-24.04`) already carries NDK 27.3.13750724,
build-tools 35.0.0 and android-35; `fetch_deps` verifies and uses them rather
than downloading again.
