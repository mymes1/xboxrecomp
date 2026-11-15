"""Native builds of a generated game project, and what each one proves.

    python3 -m tools.android.native --work W --mode host [--run-seconds 20]
    python3 -m tools.android.native --work W --mode ndk
    python3 -m tools.android.native --work W --mode check

Modes (all build <work>/project/native, the project the `project` stage
writes; build trees go to <work>/build/<mode>):

  host   the title as a desktop executable for this machine (Linux or macOS,
         x86-64 or arm64) and a headless run of it. Evidence that the
         generated code and the runtime execute on a POSIX host -- not that
         they run on Android. reports/host_build.json, host_runtime.json.
  ndk    Android arm64-v8a with the pinned NDK: libmain.so (generated code +
         runtime + Android glue) and libSDL2.so, copied to
         <work>/out/lib/arm64-v8a. reports/native_build.json is the
         "Compiled" evidence of the compatibility report.
  check  no NDK here: every source compiled for aarch64-linux-android<min_sdk>
         with zig's clang against the pinned bionic headers, nothing linked.
         Finds bionic API-level gaps and ARM64 breakage before CI does.
         reports/native_check.json; never counted as "Compiled".
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.android import elfinfo         # noqa: E402

LOCK = os.path.join(ROOT, "tools", "android", "deps.lock.json")
DEFAULT_DEPS = os.path.join(ROOT, "build", "android-deps")

# What a built libmain.so must define: the Android entry, the generated
# program, and the runtime's boot path (kernel, memory, presenter, APU, input).
REQUIRED_SYMBOLS = [
    "SDL_main", "xbe_entry_point", "recomp_dispatch_init", "recomp_lookup",
    "xbr_boot_run", "xbox_MemoryLayoutInit", "xbox_kernel_init", "xbox_kernel_bridge_init",
    "xbox_path_init", "xbox_HostWindowMain", "xbox_FramebufferWindowPresent",
    "mcpx_apu_init_standalone", "xbox_InputSdlInit", "xbox_PosixMmioFault",
]


class NativeError(Exception):
    pass


def load_lock():
    with open(LOCK, encoding="utf-8") as f:
        return json.load(f)


def _write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def _default_run(name, cmd, cwd=None, env=None, timeout=None):
    print("$ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env, timeout=timeout)
    if r.returncode:
        raise NativeError("%s failed (exit %d)" % (name, r.returncode))
    return []


def tool(name):
    p = shutil.which(name)
    if not p:
        raise NativeError("%s not found on PATH (bootstrap.sh --dev installs a pinned one)" % name)
    return p


def deps_dir():
    return os.environ.get("XBOXRECOMP_ANDROID_DEPS", DEFAULT_DEPS)


def sdl2_source():
    d = os.environ.get("XBOXRECOMP_SDL2_DIR") or os.path.join(deps_dir(), "src", "SDL2")
    if not os.path.exists(os.path.join(d, "include", "SDL.h")):
        raise NativeError("SDL2 source not found at %s (run tools/android/fetch_deps.py --only git)" % d)
    return d


def vulkan_headers():
    """The pinned Vulkan-Headers include directory, or None (then CMake looks
    in the toolchain: the NDK sysroot has its own)."""
    d = os.environ.get("XBOXRECOMP_VULKAN_HEADERS_DIR") or os.path.join(
        deps_dir(), "src", "Vulkan-Headers", "include")
    return d if os.path.exists(os.path.join(d, "vulkan", "vulkan.h")) else None


def _vulkan_defs(defs):
    vh = vulkan_headers()
    if vh:
        defs["XBOXRECOMP_VULKAN_HEADERS_DIR"] = vh
    return defs


def _props(path):
    out = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def find_ndk(lock, explicit=None, allow_other=False):
    want = lock["android"]["ndk"]["revision"]
    cands = [explicit] if explicit else []
    for var in ("XBOXRECOMP_NDK", "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"):
        if os.environ.get(var):
            cands.append(os.environ[var])
    for var in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        if os.environ.get(var):
            cands.append(os.path.join(os.environ[var], "ndk", want))
    cands.append(os.path.join(deps_dir(), "android-sdk", "ndk", want))
    seen = []
    for c in cands:
        if not c or c in seen:
            continue
        seen.append(c)
        rev = _props(os.path.join(c, "source.properties")).get("Pkg.Revision")
        if not rev:
            continue
        if rev == want or allow_other:
            return c, rev
    return None, None


# ── build helpers ───────────────────────────────────────────────────────────

def _cmake_build(run, src, build, defs, targets=None, env=None, generator="Ninja"):
    os.makedirs(build, exist_ok=True)
    cmd = [tool("cmake"), "-S", src, "-B", build, "-G", generator]
    if generator == "Ninja":
        cmd.append("-DCMAKE_MAKE_PROGRAM=" + tool("ninja"))
    cmd += ["-D%s=%s" % kv for kv in defs.items()]
    run("cmake configure", cmd, env=env)
    bcmd = [tool("cmake"), "--build", build, "--parallel", str(max(1, os.cpu_count() or 1))]
    for t in targets or []:
        bcmd += ["--target", t]
    run("cmake build", bcmd, env=env)


def _project(work):
    native = os.path.join(work, "project", "native")
    if not os.path.exists(os.path.join(native, "CMakeLists.txt")):
        raise NativeError("no generated project at %s (run the project stage)" % native)
    cfg_path = os.path.join(work, "project", "xbr_game.json")
    with open(cfg_path, encoding="utf-8") as f:
        cfg = json.load(f)
    return native, cfg


def _summary_functions(work):
    try:
        with open(os.path.join(work, "reports", "recomp_summary.json"), encoding="utf-8") as f:
            return json.load(f).get("translated")
    except (OSError, ValueError):
        return None


def host_platform():
    m = os.uname().machine
    m = {"amd64": "x86_64", "arm64": "aarch64"}.get(m, m)
    return "%s-%s" % ("macos" if sys.platform == "darwin" else sys.platform.rstrip("0123456789"), m)


# ── host ────────────────────────────────────────────────────────────────────

def build_host(work, run=_default_run, build_type="Release", log=print):
    native, cfg = _project(work)
    build = os.path.join(work, "build", "host")
    name = "xbr_" + cfg["title_id"].lower()
    defs = {"CMAKE_BUILD_TYPE": build_type, "XBR_TOOLKIT_DIR": ROOT, "XBR_GAME_NAME": name}
    host_sdl = os.path.join(deps_dir(), "host-sdl2")
    if os.path.isdir(host_sdl):
        defs["CMAKE_PREFIX_PATH"] = host_sdl
    t0 = time.time()
    _cmake_build(run, native, build, defs, targets=[name])
    exe = os.path.join(build, name)
    info = elfinfo.Elf(exe).summary(REQUIRED_SYMBOLS[1:]) if sys.platform != "darwin" else {}
    rep = {
        "ok": True, "platform": host_platform(), "executable": exe,
        "seconds": round(time.time() - t0, 1), "build_type": build_type,
        "machine": info.get("machine"), "size": info.get("size"), "tls": info.get("tls"),
        "guest_functions": info.get("guest_functions"),
        "expected_guest_functions": _summary_functions(work),
        "symbols_present": info.get("symbols_present"),
    }
    _write_json(os.path.join(work, "reports", "host_build.json"), rep)
    log("host build: %s, %s, %s guest functions linked (of %s)" % (
        exe, rep["machine"], rep["guest_functions"], rep["expected_guest_functions"]))
    return rep


_BOOT = re.compile(r"^\[BOOT\] (.*)$")
_STATUS = re.compile(r"^\[STATUS\] t=(\d+)s flips=(\d+) draws=(\d+) display=0x([0-9A-F]+) "
                     r"pitch=(\d+) fbsum=([0-9A-F]+) fbchanges=(\d+)")


def parse_boot_log(text):
    """The [BOOT]/[STATUS]/[CRASH] protocol of src/host, from any host's log."""
    out = {"boot": [], "entry_reached": False, "title_returned": False, "crash": [],
           "unimpl": [], "icall": [], "status": [], "title_lines": [], "failed_step": None}
    for raw in text.splitlines():
        line = raw.rstrip()
        # logcat prefixes ("I xboxrecomp: ") are stripped by the caller.
        m = _BOOT.match(line)
        if m:
            msg = m.group(1)
            out["boot"].append(msg)
            if msg.startswith("entering xbe_entry_point"):
                out["entry_reached"] = True
            elif msg.startswith("render: "):
                # "render: vulkan (<device>)" or "render: cpu (<why>)"
                rest = msg[len("render: "):]
                name, _, why = rest.partition(" ")
                out["render"] = {"backend": name, "detail": why.strip().strip("()")}
            elif msg.startswith("title returned"):
                out["title_returned"] = True
            elif msg.startswith("FAILED"):
                out["failed_step"] = msg
            continue
        m = _STATUS.match(line)
        if m:
            out["status"].append({"t": int(m.group(1)), "flips": int(m.group(2)),
                                  "draws": int(m.group(3)), "display": "0x" + m.group(4),
                                  "pitch": int(m.group(5)), "fbsum": m.group(6),
                                  "fbchanges": int(m.group(7))})
            continue
        if line.startswith("[CRASH]"):
            out["crash"].append(line)
        elif line.startswith("[UNIMPL]"):
            out["unimpl"].append(line)
        elif line.startswith("[ICALL]"):
            out["icall"].append(line)
        elif line.startswith("XBRTEST ") or line.startswith("[TITLE] "):
            out["title_lines"].append(line)
    last = out["status"][-1] if out["status"] else {}
    out["flips"] = last.get("flips", 0)
    out["display_changes"] = last.get("fbchanges", 0)
    out["display_set"] = bool(last.get("display") and last.get("display") != "0x00000000")
    xt = [l for l in out["title_lines"] if l.startswith("XBRTEST DONE")]
    if xt:
        m = re.search(r"pass=(\d+) fail=(\d+)", xt[-1])
        if m:
            out["selftest"] = {"pass": int(m.group(1)), "fail": int(m.group(2))}
    for k in ("crash", "unimpl", "icall"):
        out[k] = out[k][:200]
    return out


def run_host(work, exe=None, seconds=20, log=print, extra_env=None):
    """Runs the title headless for `seconds` and reports what happened."""
    if not exe:
        with open(os.path.join(work, "reports", "host_build.json"), encoding="utf-8") as f:
            exe = json.load(f)["executable"]
    with open(os.path.join(work, "game_manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    game = os.path.join(work, "game")
    rundir = os.path.join(work, "run", "host")
    if os.path.exists(rundir):
        shutil.rmtree(rundir)
    saves = os.path.join(rundir, "saves")
    os.makedirs(saves)
    bmp = os.path.join(work, "reports", "host_display.bmp")
    if os.path.exists(bmp):
        os.remove(bmp)
    env = dict(os.environ)
    env.update({"RECOMP_HEADLESS": "1", "RECOMP_STATUS_SECS": "2", "RECOMP_STATUS_BMP": bmp,
                "RECOMP_KERNEL_LOG_BUDGET": env.get("RECOMP_KERNEL_LOG_BUDGET", "400")})
    env.update(extra_env or {})
    logp = os.path.join(work, "reports", "logs", "host_run.log")
    os.makedirs(os.path.dirname(logp), exist_ok=True)
    t0 = time.time()
    with open(logp, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen([exe, game, saves], cwd=rundir, env=env, stdout=lf,
                             stderr=subprocess.STDOUT)
        try:
            rc = p.wait(timeout=seconds)
            timed_out = False
        except subprocess.TimeoutExpired:
            p.kill()
            rc = p.wait()
            timed_out = True
    dur = time.time() - t0
    with open(logp, encoding="utf-8", errors="replace") as f:
        text = f.read()
    rep = parse_boot_log(text)
    rep.update({
        "platform": host_platform(), "executable": exe, "seconds": round(dur, 1),
        "run_limit_seconds": seconds, "log": "reports/logs/host_run.log",
        "exit": ("still running at the limit (killed)" if timed_out else
                 ("signal %d" % -rc if rc < 0 else "exit %d" % rc)),
        "display_bmp": "reports/host_display.bmp" if os.path.exists(bmp) else None,
        "default_xbe": man.get("default_xbe"),
    })
    # Kept apart from the Android evidence on purpose: a desktop run says the
    # generated code and runtime work on this host, nothing about a device.
    rep["note"] = ("desktop POSIX host run; not evidence for Android (see "
                   "reports/runtime_test.json for device/emulator runs)")
    _write_json(os.path.join(work, "reports", "host_runtime.json"), rep)
    log("host run (%s): entry reached=%s, %s, flips=%d, display changes=%d, crash lines=%d%s" % (
        rep["platform"], rep["entry_reached"], rep["exit"], rep["flips"], rep["display_changes"],
        len(rep["crash"]), (", self-test %(pass)d pass / %(fail)d fail" % rep["selftest"])
        if rep.get("selftest") else ""))
    return rep


# ── ndk ─────────────────────────────────────────────────────────────────────

def build_ndk(work, run=_default_run, ndk=None, allow_other_ndk=False, build_type="Release",
              log=print):
    lock = load_lock()
    native, cfg = _project(work)
    ndk_dir, rev = find_ndk(lock, ndk, allow_other_ndk)
    if not ndk_dir:
        raise NativeError("no NDK %s found (set ANDROID_NDK_HOME, or run fetch_deps --only sdk); "
                          "use --mode check for a compile-only check" % lock["android"]["ndk"]["revision"])
    api = cfg["android"]["min_sdk"]
    build = os.path.join(work, "build", "ndk-arm64-v8a")
    defs = {
        "CMAKE_TOOLCHAIN_FILE": os.path.join(ndk_dir, "build", "cmake", "android.toolchain.cmake"),
        "ANDROID_ABI": "arm64-v8a",
        "ANDROID_PLATFORM": "android-%d" % api,
        # 16 KB ELF alignment for every library (SDL2 too): loads on 4 KB-
        # and 16 KB-page devices, as Google Play requires for Android 15+.
        "ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES": "ON",
        "CMAKE_BUILD_TYPE": build_type,
        "XBR_TOOLKIT_DIR": ROOT,
        "XBR_SDL2_SOURCE_DIR": sdl2_source(),
    }
    _vulkan_defs(defs)
    t0 = time.time()
    _cmake_build(run, native, build, defs, targets=["main", "SDL2"])
    out = os.path.join(work, "out", "lib", "arm64-v8a")
    os.makedirs(out, exist_ok=True)
    libs = []
    problems = []
    found = {}
    for root, _d, files in os.walk(build):
        for n in files:
            if n in ("libmain.so", "libSDL2.so") and n not in found:
                found[n] = os.path.join(root, n)
    for n in ("libmain.so", "libSDL2.so"):
        if n not in found:
            problems.append("%s was not produced" % n)
            continue
        dst = os.path.join(out, n)
        shutil.copyfile(found[n], dst)
        s = elfinfo.Elf(dst).summary(REQUIRED_SYMBOLS if n == "libmain.so" else ["JNI_OnLoad"])
        if s["machine"] != "aarch64" or s["class"] != "ELF64":
            problems.append("%s is %s %s, not ELF64 aarch64" % (n, s["class"], s["machine"]))
        if s["exec_stack"]:
            problems.append("%s asks for an executable stack" % n)
        if (s["load_alignment"] or 0) < 16384:
            problems.append("%s segments are %s-aligned; 16 KB-page devices need 16384"
                            % (n, s["load_alignment"]))
        missing = [k for k, v in s["symbols_present"].items() if not v]
        if missing:
            problems.append("%s lacks %s" % (n, ", ".join(missing)))
        libs.append({"name": n, "machine": s["machine"], "class": s["class"], "size": s["size"],
                     "tls": s["tls"], "android_api": s["android_api"], "needed": s["needed"],
                     "load_alignment": s["load_alignment"], "undefined_count": len(s["undefined"]),
                     "guest_functions": s["guest_functions"], "symbols_present": s["symbols_present"]})
    expect = _summary_functions(work)
    main_lib = next((l for l in libs if l["name"] == "libmain.so"), None)
    if main_lib and expect is not None and main_lib["guest_functions"] < expect:
        problems.append("libmain.so has %d guest functions, the recompiler produced %d"
                        % (main_lib["guest_functions"], expect))
    rep = {"ok": not problems, "target": "android-arm64-v8a", "api": api, "ndk": rev,
           "ndk_dir": ndk_dir, "build_type": build_type, "seconds": round(time.time() - t0, 1),
           "libraries": libs, "problems": problems, "out": os.path.relpath(out, work)}
    _write_json(os.path.join(work, "reports", "native_build.json"), rep)
    for l in libs:
        log("%s: %s %s, %d bytes, API %s, TLS %s, align %s, needs %s" % (
            l["name"], l["class"], l["machine"], l["size"], l["android_api"], l["tls"],
            l["load_alignment"], ", ".join(l["needed"])))
    if problems:
        raise NativeError("NDK build checks failed: " + "; ".join(problems))
    return rep


# ── check ───────────────────────────────────────────────────────────────────

def _zig_toolchain(build, api):
    try:
        import ziglang
    except ImportError:
        raise NativeError("the compile check needs the ziglang package (requirements-dev.txt)")
    zig_dir = os.path.dirname(ziglang.__file__)
    bionic = os.environ.get("XBOXRECOMP_BIONIC_HEADERS_DIR") or os.path.join(
        deps_dir(), "src", "bionic-headers")
    libc = os.path.join(bionic, "libc")
    if not os.path.isdir(os.path.join(libc, "include")):
        raise NativeError("bionic headers not found at %s (fetch_deps.py --only git-dev)" % bionic)
    liblog = os.environ.get("XBOXRECOMP_LIBLOG_HEADERS_DIR") or os.path.join(
        deps_dir(), "src", "liblog-headers")
    if not os.path.exists(os.path.join(liblog, "liblog", "include", "android", "log.h")):
        raise NativeError("liblog headers not found at %s (fetch_deps.py --only git-dev)" % liblog)
    vk = os.environ.get("XBOXRECOMP_VULKAN_HEADERS_DIR") or os.path.join(
        deps_dir(), "src", "Vulkan-Headers")
    os.makedirs(build, exist_ok=True)
    flags = ["-target", "aarch64-linux-android", "-nostdinc",
             "-isystem", os.path.join(zig_dir, "lib", "include"),
             "-isystem", os.path.join(libc, "include"),
             "-isystem", os.path.join(libc, "kernel", "uapi"),
             "-isystem", os.path.join(libc, "kernel", "uapi", "asm-arm64"),
             "-isystem", os.path.join(libc, "kernel", "android", "uapi"),
             "-isystem", os.path.join(liblog, "liblog", "include"),
             "-isystem", os.path.join(vk, "include"),
             "-U__ANDROID_MIN_SDK_VERSION__", "-D__ANDROID_MIN_SDK_VERSION__=%d" % api,
             "-U__ANDROID_API__", "-D__ANDROID_API__=%d" % api,
             # What the NDK's clang makes errors by default, so the check
             # fails where the real build would.
             "-Werror=implicit-function-declaration", "-Werror=int-conversion",
             "-Werror=incompatible-pointer-types", "-Werror=return-type"]
    cc = os.path.join(build, "zcc")
    with open(cc, "w") as f:
        f.write("#!/bin/sh\nexec %s -m ziglang cc %s \"$@\"\n" % (
            sys.executable, " ".join("'%s'" % a for a in flags)))
    ar = os.path.join(build, "zar")
    with open(ar, "w") as f:
        f.write("#!/bin/sh\nexec %s -m ziglang ar \"$@\"\n" % sys.executable)
    rl = os.path.join(build, "zranlib")
    with open(rl, "w") as f:
        f.write("#!/bin/sh\nexec %s -m ziglang ranlib \"$@\"\n" % sys.executable)
    for p in (cc, ar, rl):
        os.chmod(p, 0o755)
    tc = os.path.join(build, "zig-android-check.cmake")
    with open(tc, "w") as f:
        f.write("\n".join([
            "# generated by tools/android/native.py: compile-only Android check",
            "set(CMAKE_SYSTEM_NAME Linux)",
            "set(CMAKE_SYSTEM_PROCESSOR aarch64)",
            "set(CMAKE_C_COMPILER \"%s\")" % cc,
            "set(CMAKE_AR \"%s\" CACHE FILEPATH \"\" FORCE)" % ar,
            "set(CMAKE_RANLIB \"%s\" CACHE FILEPATH \"\" FORCE)" % rl,
            "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)",
            "set(ANDROID TRUE)",
            "set(ANDROID_PLATFORM_LEVEL %d)" % api,
            ""]))
    return tc, {"zig": zig_dir, "bionic": bionic}


def compile_check(work, run=_default_run, log=print):
    native, cfg = _project(work)
    api = cfg["android"]["min_sdk"]
    build = os.path.join(work, "build", "check-android-arm64")
    if os.path.exists(build):
        shutil.rmtree(build)
    tc, where = _zig_toolchain(build, api)
    defs = {"CMAKE_TOOLCHAIN_FILE": tc, "CMAKE_BUILD_TYPE": "Release", "XBR_TOOLKIT_DIR": ROOT,
            "XBR_SDL2_SOURCE_DIR": sdl2_source(), "XBR_COMPILE_CHECK": "ON"}
    _vulkan_defs(defs)
    t0 = time.time()
    _cmake_build(run, native, build, defs)
    objs = []
    machines = set()
    for root, _d, files in os.walk(build):
        for n in files:
            if n.endswith(".o"):
                p = os.path.join(root, n)
                objs.append(p)
                try:
                    machines.add(elfinfo.Elf(p).machine)
                except elfinfo.ElfError:
                    machines.add("not-elf")
    rep = {"ok": bool(objs) and machines == {"aarch64"}, "mode": "compile-check",
           "target": "aarch64-linux-android%d" % api, "objects": len(objs),
           "machines": sorted(machines), "seconds": round(time.time() - t0, 1),
           "compiler": "zig cc (clang) " + where["zig"], "bionic_headers": where["bionic"],
           "note": "compiled only, nothing linked: not a build, never reported as Compiled"}
    _write_json(os.path.join(work, "reports", "native_check.json"), rep)
    log("compile check: %d objects for %s (%s)" % (len(objs), rep["target"],
                                                  ", ".join(sorted(machines))))
    if not rep["ok"]:
        raise NativeError("compile check produced no aarch64 objects")
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.android.native", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--mode", choices=["host", "ndk", "check"], required=True)
    ap.add_argument("--run-seconds", type=int, default=20, help="host: run the title this long (0 = build only)")
    ap.add_argument("--ndk", help="ndk: NDK directory (default: ANDROID_NDK_HOME etc.)")
    ap.add_argument("--allow-other-ndk", action="store_true")
    a = ap.parse_args(argv)
    work = os.path.abspath(a.work)
    try:
        if a.mode == "host":
            build_host(work)
            if a.run_seconds:
                run_host(work, seconds=a.run_seconds)
        elif a.mode == "ndk":
            build_ndk(work, ndk=a.ndk, allow_other_ndk=a.allow_other_ndk)
        else:
            compile_check(work)
    except NativeError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
