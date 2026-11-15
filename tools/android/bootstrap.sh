#!/usr/bin/env bash
# Bootstrap every pinned input of the Android APK pipeline.
#
#   bash tools/android/bootstrap.sh            # all: python, git, archives, gradle, sdk
#   bash tools/android/bootstrap.sh --no-sdk   # skip the Android SDK/NDK (host-only work)
#   bash tools/android/bootstrap.sh --dev      # also the test-only Python packages
#   bash tools/android/bootstrap.sh --vk-test  # also SwiftShader, a CPU Vulkan device for
#                                              # tests/nv2a_vk_backend (implies --dev)
#
# Pins: tools/android/deps.lock.json. Verification rules: tools/android/fetch_deps.py.
# Afterwards:  source build/android-deps/android-env.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

want="python,git,archives,gradle,sdk"
extra=()
for a in "$@"; do
    case "$a" in
        --no-sdk) want="${want/,sdk/}" ;;
        --no-gradle) want="${want/,gradle/}" ;;
        --dev) want="$want,python-dev" ;;
        --vk-test) want="$want,python-dev,test-vk" ;;
        --check) extra+=(--check) ;;
        --deps-dir=*) extra+=(--deps-dir "${a#--deps-dir=}") ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 (>= 3.10) is required" >&2; exit 1; }
python3 - <<'EOF'
import sys
if sys.version_info < (3, 10):
    sys.exit("python3 >= 3.10 is required, found %s" % sys.version.split()[0])
EOF

python3 -m tools.android.fetch_deps --only "$want" "${extra[@]}"
