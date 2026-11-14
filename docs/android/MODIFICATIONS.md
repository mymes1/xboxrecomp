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
| `docs/android/*.md` | this documentation |
