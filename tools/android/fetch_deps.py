"""Fetch and verify every pinned input of the Android pipeline.

    python3 -m tools.android.fetch_deps                 # everything
    python3 -m tools.android.fetch_deps --only git      # git sources only
    python3 -m tools.android.fetch_deps --check         # report, change nothing

Pins live in tools/android/deps.lock.json. Nothing is fetched by "latest":

  git      checked out by commit id, and the checkout's HEAD is compared with
           the pinned id before it is used
  archive  downloaded to a temporary name, SHA-256 compared, renamed only on a
           match (a mismatch deletes the file and fails the run)
  sdk      Android SDK packages installed by exact revision; sdkmanager checks
           each archive against the checksum in Google's repository manifest,
           and the installed revision is read back from source.properties
  python   pip --require-hashes against the lock files

Results are recorded in <deps-dir>/deps-manifest.json and the environment the
build needs is written to <deps-dir>/android-env.sh.
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCK = os.path.join(ROOT, "tools", "android", "deps.lock.json")
DEFAULT_DEPS = os.path.join(ROOT, "build", "android-deps")


class DepError(Exception):
    pass


def log(msg):
    print("[deps] " + msg, flush=True)


def load_lock(path=LOCK):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# --------------------------------------------------------------- downloads

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download_verified(url, dest, sha256, retries=3):
    """Download url to dest, verifying SHA-256. Existing verified files are kept."""
    if os.path.exists(dest):
        got = sha256_file(dest)
        if got == sha256:
            log("cached and verified: %s" % os.path.basename(dest))
            return dest
        log("cached file has the wrong hash (%s), refetching" % got)
        os.remove(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    last = None
    for attempt in range(1, retries + 1):
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(dest), suffix=".part")
        os.close(fd)
        try:
            log("downloading %s (attempt %d)" % (url, attempt))
            h = hashlib.sha256()
            req = urllib.request.Request(url, headers={"User-Agent": "xboxrecomp-fetch-deps"})
            with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as out:
                for chunk in iter(lambda: r.read(1 << 20), b""):
                    h.update(chunk)
                    out.write(chunk)
            got = h.hexdigest()
            if got != sha256:
                raise DepError("SHA-256 mismatch for %s: expected %s, got %s"
                               % (url, sha256, got))
            os.replace(tmp, dest)
            log("verified %s sha256=%s" % (os.path.basename(dest), got))
            return dest
        except DepError:
            os.remove(tmp)
            raise
        except Exception as e:  # network errors: retry
            last = e
            if os.path.exists(tmp):
                os.remove(tmp)
            time.sleep(2 * attempt)
    raise DepError("download failed for %s: %s" % (url, last))


# --------------------------------------------------------------------- git

def run(cmd, cwd=None, env=None, check=True, capture=False, input_text=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, text=True, input=input_text,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None)
    if check and r.returncode != 0:
        raise DepError("command failed (%d): %s%s" % (
            r.returncode, " ".join(cmd),
            ("\n" + r.stdout[-4000:]) if capture and r.stdout else ""))
    return r


def git_head(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return r.stdout.strip() if r.returncode == 0 else None


def fetch_git(name, spec, deps_dir, check_only=False):
    dest = os.path.join(deps_dir, "src", name)
    want = spec["commit"]
    head = git_head(dest) if os.path.isdir(os.path.join(dest, ".git")) else None
    if head == want:
        log("%s: %s at pinned commit %s" % (name, spec.get("tag", ""), want))
        return {"path": dest, "commit": want, "tag": spec.get("tag")}
    if check_only:
        raise DepError("%s: not present at the pinned commit (have %s)" % (name, head))
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    log("%s: fetching %s @ %s" % (name, spec["url"], want))
    run(["git", "init", "-q", dest])
    run(["git", "-C", dest, "remote", "add", "origin", spec["url"]])
    fetch = ["git", "-C", dest, "fetch", "-q", "--depth", "1"]
    if spec.get("sparse"):
        # Large repositories of which a few directories are needed (bionic's
        # headers): a blobless fetch plus a sparse checkout downloads only the
        # files under those paths. The commit id is still verified below.
        run(["git", "-C", dest, "config", "remote.origin.promisor", "true"])
        run(["git", "-C", dest, "config", "remote.origin.partialclonefilter", "blob:none"])
        run(["git", "-C", dest, "sparse-checkout", "set", "--no-cone"] + list(spec["sparse"]))
        fetch.append("--filter=blob:none")
    # GitHub serves any reachable commit by id; a shallow fetch of exactly the
    # pinned object keeps this fast and makes the tag irrelevant to what lands.
    run(fetch + ["origin", want])
    run(["git", "-C", dest, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"])
    head = git_head(dest)
    if head != want:
        raise DepError("%s: checked out %s, expected %s" % (name, head, want))
    log("%s: verified commit %s" % (name, head))
    return {"path": dest, "commit": head, "tag": spec.get("tag")}


# --------------------------------------------------------------------- sdk

def read_props(path):
    props = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" in line:
                    k, v = line.split("=", 1)
                    props[k.strip()] = v.strip()
    except OSError:
        pass
    return props


def package_dir(sdk, package):
    return os.path.join(sdk, *package.split(";"))


def package_revision(sdk, package):
    props = read_props(os.path.join(package_dir(sdk, package), "source.properties"))
    return props.get("Pkg.Revision")


def expected_revision(package):
    """The revision a package name pins, where the name carries one."""
    kind, _, ver = package.partition(";")
    if kind in ("ndk", "cmake", "build-tools"):
        return ver
    return None   # platforms;android-N: the directory name is the pin


def find_sdk(deps_dir):
    for var in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        p = os.environ.get(var)
        if p and os.path.isdir(p):
            return p
    local = os.path.join(deps_dir, "android-sdk")
    return local


def sdkmanager_path(sdk):
    for rel in (("cmdline-tools", "latest", "bin", "sdkmanager"),
                ("cmdline-tools", "bin", "sdkmanager")):
        p = os.path.join(sdk, *rel)
        if os.path.exists(p):
            return p
    return None


def ensure_cmdline_tools(sdk, lock, deps_dir):
    sm = sdkmanager_path(sdk)
    if sm:
        return sm
    ct = lock["android"]["cmdline_tools"]
    zpath = download_verified(ct["url"], os.path.join(deps_dir, "downloads",
                              os.path.basename(ct["url"])), ct["sha256"])
    dest = os.path.join(sdk, "cmdline-tools")
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(dest)
    # The zip holds cmdline-tools/; sdkmanager wants cmdline-tools/latest/.
    src = os.path.join(dest, "cmdline-tools")
    latest = os.path.join(dest, "latest")
    if os.path.exists(latest):
        shutil.rmtree(latest)
    os.rename(src, latest)
    for root, _dirs, files in os.walk(os.path.join(latest, "bin")):
        for f in files:
            os.chmod(os.path.join(root, f), 0o755)
    return sdkmanager_path(sdk)


def ensure_sdk(lock, deps_dir, check_only=False):
    sdk = find_sdk(deps_dir)
    pkgs = lock["android"]["sdk_packages"]
    missing = []
    for p in pkgs:
        want = expected_revision(p)
        have = package_revision(sdk, p)
        ok = os.path.isdir(package_dir(sdk, p)) and (want is None or (have or "").startswith(want))
        if not ok:
            missing.append(p)
        log("sdk %-24s %s" % (p, "ok (%s)" % have if ok else "missing (have %s)" % have))
    if missing and check_only:
        raise DepError("Android SDK packages missing: %s" % ", ".join(missing))
    if missing:
        os.makedirs(sdk, exist_ok=True)
        sm = ensure_cmdline_tools(sdk, lock, deps_dir)
        env = dict(os.environ, ANDROID_SDK_ROOT=sdk, ANDROID_HOME=sdk)
        run([sm, "--sdk_root=" + sdk, "--licenses"], env=env, check=False,
            input_text="y\n" * 50, capture=True)
        run([sm, "--sdk_root=" + sdk, "--install"] + missing, env=env)
        for p in missing:
            want = expected_revision(p)
            have = package_revision(sdk, p)
            if want and not (have or "").startswith(want):
                raise DepError("sdkmanager installed %s at %s, expected %s" % (p, have, want))
    ndk = package_dir(sdk, lock["android"]["ndk"]["package"])
    props = read_props(os.path.join(ndk, "source.properties"))
    if props.get("Pkg.Revision") != lock["android"]["ndk"]["revision"]:
        raise DepError("NDK at %s is %s, expected %s" % (
            ndk, props.get("Pkg.Revision"), lock["android"]["ndk"]["revision"]))
    glslc = os.path.join(ndk, "shader-tools", "linux-x86_64", "glslc")
    return {"sdk": sdk, "ndk": ndk, "ndk_revision": props.get("Pkg.Revision"),
            "glslc": glslc if os.path.exists(glslc) else None,
            "build_tools": os.path.join(sdk, "build-tools", lock["android"]["build_tools"]),
            "platform": os.path.join(sdk, "platforms", "android-%d" % lock["android"]["compile_sdk"])}


# ------------------------------------------------------------------ python

def ensure_python(lock, which=("pipeline",), check_only=False):
    done = {}
    for key in which:
        req = os.path.join(ROOT, lock["python"][key])
        if check_only:
            done[key] = req
            continue
        cmd = [sys.executable, "-m", "pip", "install", "--quiet", "--require-hashes",
               "--only-binary", ":all:", "-r", req]
        r = run(cmd, check=False, capture=True)
        if r.returncode != 0 and "externally-managed" in (r.stdout or ""):
            r = run(cmd + ["--break-system-packages"], check=False, capture=True)
        if r.returncode != 0:
            raise DepError("pip install -r %s failed:\n%s" % (req, (r.stdout or "")[-3000:]))
        log("python %s: installed with verified hashes" % key)
        done[key] = req
    return done


# ------------------------------------------------------------------ gradle

def ensure_gradle(lock, deps_dir, check_only=False):
    g = lock["gradle"]
    dest = os.path.join(deps_dir, "downloads", os.path.basename(g["url"]))
    home = os.path.join(deps_dir, "gradle-%s" % g["version"])
    exe = os.path.join(home, "bin", "gradle")
    if os.path.exists(exe):
        log("gradle %s present" % g["version"])
        return {"home": home, "gradle": exe, "version": g["version"]}
    if check_only:
        raise DepError("gradle %s not fetched" % g["version"])
    download_verified(g["url"], dest, g["sha256"])
    with zipfile.ZipFile(dest) as z:
        z.extractall(deps_dir)
    os.chmod(exe, 0o755)
    return {"home": home, "gradle": exe, "version": g["version"]}


# ---------------------------------------------------------------- archives

def ensure_archive(name, spec, deps_dir, check_only=False):
    """A verified archive, with one member extracted when the spec names one."""
    dl = os.path.join(deps_dir, "downloads", os.path.basename(spec["url"]))
    out_dir = os.path.join(deps_dir, "tools", name)
    member = spec.get("member")
    target = os.path.join(out_dir, os.path.basename(member)) if member else dl
    if os.path.exists(target) and os.path.exists(dl) and sha256_file(dl) == spec["sha256"]:
        log("%s %s present and verified" % (name, spec.get("version", "")))
        return {"path": target, "version": spec.get("version"), "sha256": spec["sha256"]}
    if check_only:
        raise DepError("%s not fetched" % name)
    download_verified(spec["url"], dl, spec["sha256"])
    if member:
        os.makedirs(out_dir, exist_ok=True)
        with tarfile.open(dl, "r:*") as t:
            m = t.getmember(member)
            src = t.extractfile(m)
            with open(target, "wb") as out:
                shutil.copyfileobj(src, out)
        os.chmod(target, 0o755)
    return {"path": target, "version": spec.get("version"), "sha256": spec["sha256"]}


# -------------------------------------------------------------- test tools

def _unbrotli(data):
    """brotli.decompress, from a fresh interpreter when this one cannot import
    the module yet (the python-dev group installed it earlier in this run)."""
    try:
        import brotli
        return brotli.decompress(data)
    except ImportError:
        pass
    r = subprocess.run([sys.executable, "-c", "import brotli, sys; "
                        "sys.stdout.buffer.write(brotli.decompress(sys.stdin.buffer.read()))"],
                       input=data, capture_output=True)
    if r.returncode != 0:
        raise DepError("swiftshader needs the brotli module: python3 -m pip install "
                       "--require-hashes -r tools/android/requirements-dev.txt")
    return r.stdout


def ensure_swiftshader(spec, deps_dir, check_only=False):
    """SwiftShader (a CPU Vulkan device) for tests/nv2a_vk_backend: the npm
    tarball verified by SHA-256, the brotli'd tar inside it verified, and the
    two libraries unpacked from that verified again."""
    out_dir = os.path.join(deps_dir, "tools", "swiftshader")
    icd = os.path.join(out_dir, spec["icd"])

    def unpacked_ok():
        return os.path.exists(icd) and all(
            os.path.exists(os.path.join(out_dir, f)) and sha256_file(os.path.join(out_dir, f)) == h
            for f, h in spec["files_sha256"].items())

    if unpacked_ok():
        log("swiftshader present and verified")
        return {"path": icd, "dir": out_dir, "version": spec["version"]}
    if check_only:
        raise DepError("swiftshader not fetched")
    dl = os.path.join(deps_dir, "downloads", os.path.basename(spec["url"]))
    download_verified(spec["url"], dl, spec["sha256"])
    with tarfile.open(dl, "r:gz") as t:
        packed = t.extractfile(t.getmember(spec["member"])).read()
    if hashlib.sha256(packed).hexdigest() != spec["member_sha256"]:
        raise DepError("swiftshader: %s does not match its pinned SHA-256" % spec["member"])
    os.makedirs(out_dir, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(_unbrotli(packed))) as t:
        for m in t.getmembers():
            name = os.path.basename(m.name)
            if m.isfile() and (name in spec["files_sha256"] or name == spec["icd"]):
                with open(os.path.join(out_dir, name), "wb") as f:
                    f.write(t.extractfile(m).read())
    if not unpacked_ok():
        raise DepError("swiftshader: unpacked libraries do not match their pinned SHA-256")
    log("swiftshader unpacked and verified in %s" % out_dir)
    return {"path": icd, "dir": out_dir, "version": spec["version"]}


# -------------------------------------------------------------------- main

def write_env(deps_dir, manifest):
    lines = ["# generated by tools/android/fetch_deps.py -- source it"]
    sdk = manifest.get("sdk") or {}
    if sdk:
        lines += ["export ANDROID_SDK_ROOT=%s" % sdk["sdk"],
                  "export ANDROID_HOME=%s" % sdk["sdk"],
                  "export ANDROID_NDK_ROOT=%s" % sdk["ndk"],
                  "export ANDROID_NDK_HOME=%s" % sdk["ndk"]]
        if sdk.get("glslc"):
            lines.append("export XBOXRECOMP_GLSLC=%s" % sdk["glslc"])
    for name, info in (manifest.get("git") or {}).items():
        var = "XBOXRECOMP_%s_DIR" % name.upper().replace("-", "_")
        lines.append("export %s=%s" % (var, info["path"]))
    for name, info in (manifest.get("archives") or {}).items():
        var = "XBOXRECOMP_%s" % name.upper().replace("-", "_")
        lines.append("export %s=%s" % (var, info["path"]))
    gr = manifest.get("gradle")
    if gr:
        lines.append("export XBOXRECOMP_GRADLE=%s" % gr["gradle"])
    ss = (manifest.get("test_tools") or {}).get("swiftshader")
    if ss:
        # Not exported as VK_ICD_FILENAMES: only the Vulkan test opts in.
        lines.append("export XBOXRECOMP_SWIFTSHADER_ICD=%s" % ss["path"])
        lines.append("export XBOXRECOMP_SWIFTSHADER_DIR=%s" % ss["dir"])
    path = os.path.join(deps_dir, "android-env.sh")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.android.fetch_deps", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deps-dir", default=os.environ.get("XBOXRECOMP_ANDROID_DEPS", DEFAULT_DEPS))
    ap.add_argument("--only", default="git,sdk,gradle,python",
                    help="comma list of: git, git-dev, sdk, gradle, python, python-dev, "
                         "archives, test-vk")
    ap.add_argument("--check", action="store_true", help="verify only; fetch nothing")
    ap.add_argument("--lock", default=LOCK)
    args = ap.parse_args(argv)

    lock = load_lock(args.lock)
    only = set(s.strip() for s in args.only.split(",") if s.strip())
    deps_dir = os.path.abspath(args.deps_dir)
    os.makedirs(deps_dir, exist_ok=True)
    manifest_path = os.path.join(deps_dir, "deps-manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, encoding="utf-8") as f:
                manifest = json.load(f)
        except ValueError:
            manifest = {}
    manifest["lock_sha256"] = sha256_file(args.lock)
    errors = []

    if "python" in only or "python-dev" in only:
        which = (["pipeline"] if "python" in only else []) + (["dev"] if "python-dev" in only else [])
        try:
            manifest["python"] = ensure_python(lock, which, args.check)
        except DepError as e:
            errors.append(str(e))
    if "git" in only:
        manifest.setdefault("git", {})
        for name, spec in lock["git"].items():
            try:
                manifest["git"][name] = fetch_git(name, spec, deps_dir, args.check)
            except DepError as e:
                errors.append(str(e))
    if "git-dev" in only:
        # Development-only sources (bionic headers for the compile-only ARM64
        # check where no NDK can be installed). CI builds never need them.
        manifest.setdefault("git", {})
        for name, spec in (lock.get("git_dev") or {}).items():
            try:
                manifest["git"][name] = fetch_git(name, spec, deps_dir, args.check)
            except DepError as e:
                errors.append(str(e))
    if "archives" in only:
        manifest.setdefault("archives", {})
        for name, spec in (lock.get("archives") or {}).items():
            host = spec.get("host")
            if host == "linux-x86_64" and not (sys.platform.startswith("linux")
                                               and os.uname().machine == "x86_64"):
                log("%s: skipped (only for %s)" % (name, host))
                continue
            try:
                manifest["archives"][name] = ensure_archive(name, spec, deps_dir, args.check)
            except DepError as e:
                errors.append(str(e))
    if "test-vk" in only:
        spec = (lock.get("test_tools") or {}).get("swiftshader")
        if spec and not (sys.platform.startswith("linux") and os.uname().machine == "x86_64"):
            log("swiftshader: skipped (only for %s)" % spec.get("host"))
        elif spec:
            try:
                manifest.setdefault("test_tools", {})["swiftshader"] = \
                    ensure_swiftshader(spec, deps_dir, args.check)
            except DepError as e:
                errors.append(str(e))
    if "gradle" in only:
        try:
            manifest["gradle"] = ensure_gradle(lock, deps_dir, args.check)
        except DepError as e:
            errors.append(str(e))
    if "sdk" in only:
        try:
            manifest["sdk"] = ensure_sdk(lock, deps_dir, args.check)
        except DepError as e:
            errors.append(str(e))

    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    env_path = write_env(deps_dir, manifest)
    log("manifest: %s" % manifest_path)
    log("environment: source %s" % env_path)
    if errors:
        for e in errors:
            log("ERROR: " + e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
