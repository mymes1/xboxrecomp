"""Run a built APK on a connected Android device or emulator and record what
happened: reports/runtime_test.json, the evidence for "Runtime initialized"
and "Game booted" in the compatibility report.

    python3 -m tools.android.device_run --work W [--seconds 60] [--serial S]
                                        [--lifecycle]

Installs <work>/out/<package>-<version>.apk, clears logcat, starts the
launcher with AUTOSTART (it installs the game files and starts the game in
its own process), waits, then collects the runtime's log (logcat tag
"xboxrecomp", the same [BOOT]/[STATUS]/[CRASH] protocol as every host),
the crash buffer, the device's properties and the last scanout snapshot
(RECOMP_STATUS_BMP). Whether the ARM64 code ran natively or under the
emulator's ARM translation is recorded from the device's ABI list.

For the self-test title (tools/android/testtitle) the screen itself is
checked too: a raw screenshot must show the title's picture -- its colour
bars in order, the band beneath, black beside it (screen_check.py) -- which
is what the presenter (the Vulkan swapchain, or the SDL renderer) actually
put on the display. --lifecycle then sends the app to the background (HOME),
checks the game process survives it and the runtime saw it, brings the task
back the way the launcher icon does, and checks the screen again.
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

from tools.android import native      # noqa: E402
from tools.android import screen_check  # noqa: E402

PKG_ACTIVITY = "org.xboxrecomp.app.LauncherActivity"


class DeviceError(Exception):
    pass


def adb_path():
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        d = os.environ.get(var)
        if d and os.path.exists(os.path.join(d, "platform-tools", "adb")):
            return os.path.join(d, "platform-tools", "adb")
    p = shutil.which("adb")
    if not p:
        raise DeviceError("adb not found (Android SDK platform-tools)")
    return p


class Adb:
    def __init__(self, serial=None):
        self.exe = adb_path()
        self.serial = serial

    def cmd(self, *args, check=True, timeout=120, text=True):
        full = [self.exe] + (["-s", self.serial] if self.serial else []) + list(args)
        r = subprocess.run(full, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=text,
                           timeout=timeout, errors="replace" if text else None)
        if check and r.returncode:
            raise DeviceError("%s failed (%d): %s" % (" ".join(full), r.returncode,
                                                      (r.stdout or "")[-1500:]))
        return r.stdout

    def prop(self, name):
        return self.cmd("shell", "getprop", name, check=False).strip()


def _raw_screen(adb, serial):
    """`screencap` without -p: a header and the pixels, for screen_check."""
    try:
        return subprocess.run([adb.exe] + (["-s", serial] if serial else []) +
                              ["exec-out", "screencap"], stdout=subprocess.PIPE,
                              timeout=60).stdout
    except (subprocess.SubprocessError, OSError):
        return b""


def _screen(adb, serial, log, what):
    r = screen_check.check(_raw_screen(adb, serial))
    log("screen (%s): %s -- %s" % (what, "matches" if r.get("ok") else "NO MATCH", r.get("reason")))
    return r


def _game_log(adb):
    return adb.cmd("logcat", "-d", "-v", "raw", "-s", "xboxrecomp:*", check=False)


def _lifecycle(adb, serial, pkg, log, selftest):
    """HOME, then back the way the launcher icon brings a task back."""
    out = {"steps": []}
    before = _game_log(adb).count("[HOST] background")
    adb.cmd("shell", "input", "keyevent", "KEYCODE_HOME", check=False)
    time.sleep(6)
    alive_bg = bool(adb.cmd("shell", "pidof", pkg + ":game", check=False).strip())
    text = _game_log(adb)
    saw_bg = text.count("[HOST] background") > before
    out["steps"].append({"step": "home", "game_alive": alive_bg, "runtime_saw_background": saw_bg})
    # NEW_TASK to the task's root activity: "brought to the front" with the
    # game still on top, as tapping the icon does (LauncherActivity is
    # singleTop for this; singleTask would finish the game).
    front = adb.cmd("shell", "am", "start", "-W", "-a", "android.intent.action.MAIN",
                    "-c", "android.intent.category.LAUNCHER", "-f", "0x10200000",
                    "-n", "%s/%s" % (pkg, PKG_ACTIVITY), check=False, timeout=120)
    time.sleep(8)
    alive_fg = bool(adb.cmd("shell", "pidof", pkg + ":game", check=False).strip())
    text = _game_log(adb)
    saw_fg = text.count("[HOST] foreground") >= 1 and saw_bg
    top = adb.cmd("shell", "dumpsys", "activity", "activities", check=False)
    m = re.search(r"(?:mResumedActivity|ResumedActivity|topResumedActivity)[^\n]*?\{[^}]*\s(\S+/\S+)",
                  top)
    resumed = m.group(1) if m else ""
    out["steps"].append({"step": "back", "am": front.strip()[-300:], "game_alive": alive_fg,
                         "runtime_saw_foreground": saw_fg, "resumed": resumed})
    ok = alive_bg and saw_bg and alive_fg and saw_fg and "XbrGameActivity" in resumed
    if selftest:
        out["screen_after_resume"] = _screen(adb, serial, log, "after background/foreground")
        ok = ok and out["screen_after_resume"].get("ok")
    out["verdict"] = ("passed: survived the background, the runtime suspended and resumed, the"
                      " game is in front again" + (" and on the screen" if selftest else "")
                      if ok else "FAILED: %s" % json.dumps(out["steps"]))
    log("background/foreground: %s" % out["verdict"])
    return out


def run(work, seconds=60, serial=None, extra_env=None, log=print, lifecycle=False):
    with open(os.path.join(work, "project", "xbr_game.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    apk = os.path.join(work, "out", "%s-%s.apk" % (cfg["package"], cfg["version_name"]))
    if not os.path.exists(apk):
        raise DeviceError("no APK at %s" % apk)
    pkg = cfg["package"]
    adb = Adb(serial)
    adb.cmd("wait-for-device", timeout=300)
    device = {k: adb.prop(p) for k, p in (
        ("model", "ro.product.model"), ("manufacturer", "ro.product.manufacturer"),
        ("sdk", "ro.build.version.sdk"), ("release", "ro.build.version.release"),
        ("abilist", "ro.product.cpu.abilist"), ("native_bridge", "ro.dalvik.vm.native.bridge"),
        ("hardware", "ro.hardware"), ("emulator", "ro.kernel.qemu"),
        ("vulkan", "ro.hardware.vulkan"), ("egl", "ro.hardware.egl"))}
    device["page_size"] = adb.cmd("shell", "getconf", "PAGESIZE", check=False).strip()
    primary = (device["abilist"] or "").split(",")[0]
    if primary == "arm64-v8a":
        execution = "native arm64"
    elif "arm64-v8a" in (device["abilist"] or ""):
        execution = "ARM translation (%s) on %s" % (device["native_bridge"] or "native bridge",
                                                    primary)
    else:
        execution = "unsupported: no arm64-v8a in %s" % device["abilist"]
    log("device: %s %s, Android %s (API %s), ABIs %s -> %s" % (
        device["manufacturer"], device["model"], device["release"], device["sdk"],
        device["abilist"], execution))

    t0 = time.time()
    install = adb.cmd("install", "-r", "-g", apk, check=False, timeout=600)
    installed = "Success" in install
    rep = {"platform": "android-arm64-v8a", "execution": execution, "device": device,
           "apk": os.path.relpath(apk, work), "install": install.strip()[-500:],
           "entry_reached": False, "crash": [], "flips": 0, "display_changes": 0}
    if not installed:
        rep["exit"] = "install failed"
        _finish(work, rep, log)
        raise DeviceError("install failed: %s" % install.strip()[-500:])

    adb.cmd("logcat", "-c", check=False)
    env = ["RECOMP_STATUS_SECS=2", "RECOMP_STATUS_BMP=display.bmp",
           "RECOMP_KERNEL_LOG_BUDGET=400"] + list(extra_env or [])
    adb.cmd("shell", "am", "start", "-W", "-n", "%s/%s" % (pkg, PKG_ACTIVITY),
            "--ez", "org.xboxrecomp.app.AUTOSTART", "true",
            "--esa", "org.xboxrecomp.app.ENV", ",".join(env), timeout=180)
    deadline = time.time() + seconds
    pid_game = ""
    while time.time() < deadline:
        time.sleep(5)
        pid_game = adb.cmd("shell", "pidof", pkg + ":game", check=False).strip()
        text = adb.cmd("logcat", "-d", "-v", "raw", "-s", "xboxrecomp:*", check=False)
        if "[BOOT] SDL_main returning" in text or "[CRASH]" in text:
            break
    # The screen, while the game is running: for the self-test title the
    # picture can be recognised (screen_check.py); for any title a lifecycle
    # round trip can be checked.
    early = native.parse_boot_log(_game_log(adb))
    selftest = bool(early.get("selftest")) and not early["crash"]
    screen = _screen(adb, serial, log, "running") if selftest else None
    life = None
    if lifecycle and pid_game and not early["crash"]:
        life = _lifecycle(adb, serial, pkg, log, selftest)
    text = adb.cmd("logcat", "-d", "-v", "raw", "-s", "xboxrecomp:*", check=False)
    full = adb.cmd("logcat", "-d", "-v", "threadtime", check=False)
    crash_buf = adb.cmd("logcat", "-d", "-b", "crash", check=False)
    rep_dir = os.path.join(work, "reports")
    os.makedirs(os.path.join(rep_dir, "logs"), exist_ok=True)
    with open(os.path.join(rep_dir, "logs", "device_runtime.log"), "w", encoding="utf-8") as f:
        f.write(text)
    with open(os.path.join(rep_dir, "logs", "device_logcat.txt"), "w", encoding="utf-8") as f:
        f.write(full[-4_000_000:])
    parsed = native.parse_boot_log(text)
    rep.update(parsed)
    if screen is not None:
        rep["screen_check"] = screen
    if life is not None:
        rep["lifecycle_test"] = {k: v for k, v in life.items() if k != "screen_after_resume"}
        if "screen_after_resume" in life:
            rep["screen_after_resume"] = life["screen_after_resume"]
    rep["seconds"] = round(time.time() - t0, 1)
    rep["log"] = "reports/logs/device_runtime.log"
    rep["game_process_alive_at_end"] = bool(pid_game)
    rep["exit"] = ("still running at the limit" if pid_game else
                   ("returned" if parsed["title_returned"] else "process ended"))
    rep["crash_buffer"] = crash_buf.strip().splitlines()[-60:] if crash_buf.strip() else []
    fatal = [l for l in full.splitlines() if "Fatal signal" in l or "FATAL EXCEPTION" in l
             or "UnsatisfiedLinkError" in l or "dlopen failed" in l]
    rep["fatal_lines"] = fatal[-30:]
    if fatal and not rep["crash"]:
        rep["crash"] = fatal[-10:]
    ext = "/sdcard/Android/data/%s/files/logs/display.bmp" % pkg
    bmp = os.path.join(rep_dir, "device_display.bmp")
    if os.path.exists(bmp):
        os.remove(bmp)
    adb.cmd("pull", ext, bmp, check=False)
    rep["display_image"] = "reports/device_display.bmp" if os.path.exists(bmp) else None
    shot = os.path.join(rep_dir, "device_screen.png")
    try:
        png = subprocess.run([adb.exe] + (["-s", serial] if serial else []) +
                             ["exec-out", "screencap", "-p"], stdout=subprocess.PIPE,
                             timeout=60).stdout
        if png[:8] == b"\x89PNG\r\n\x1a\n":
            with open(shot, "wb") as f:
                f.write(png)
            rep["screenshot"] = "reports/device_screen.png"
    except (subprocess.SubprocessError, OSError):
        pass
    _finish(work, rep, log)
    return rep


def _finish(work, rep, log):
    native._write_json(os.path.join(work, "reports", "runtime_test.json"), rep)
    from tools.android import compat
    compat.write(work)
    log("device run (%s): entry reached=%s, %s, flips=%d, scanout changes=%d, crash lines=%d%s" % (
        rep.get("execution"), rep.get("entry_reached"), rep.get("exit"), rep.get("flips", 0),
        rep.get("display_changes", 0), len(rep.get("crash", [])),
        (", self-test %(pass)d pass / %(fail)d fail" % rep["selftest"]) if rep.get("selftest") else ""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--serial")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the runtime")
    ap.add_argument("--lifecycle", action="store_true",
                    help="send the app to the background and back, and check it survives")
    a = ap.parse_args(argv)
    try:
        run(os.path.abspath(a.work), a.seconds, a.serial, a.env, lifecycle=a.lifecycle)
    except DeviceError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
