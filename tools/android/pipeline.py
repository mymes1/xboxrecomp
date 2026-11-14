"""Original Xbox disc image -> Android APK, one stage at a time.

    python3 -m tools.android.pipeline run --source game.iso --sha256 <hex> --work build/games/x
    python3 -m tools.android.pipeline run --source https://... --sha256 <hex> --work ...
    python3 -m tools.android.pipeline run --work build/games/x --from recompile
    python3 -m tools.android.pipeline status --work build/games/x

Stages, in order (each records its result in <work>/pipeline_state.json and
its log in <work>/reports/logs/<stage>.log; a failed stage stops the run and
says why):

  intake     acquire the user's image, verify SHA-256, detect XDVDFS, extract
             every file into <work>/game with a manifest, locate default.xbe
  analyze    toolkit XBE parser + structured analysis (sections, imports,
             libraries, certificate, title image) -> reports/xbe_analysis.*
  recompile  the toolkit's disasm -> func_id -> abi_analysis -> recomp, into
             <work>/project/native/gen; recompilation summary + compatibility
  project    the per-game project: xbr_game.json (configuration) and the
             native CMake project around the generated C (Gradle app: phase 7)
  host       desktop build of the same project for this machine, then a
             headless run of the title: generated code + runtime on a POSIX
             host (reports/host_*.json; says nothing about Android)
  native     Android arm64-v8a: the NDK build (libmain.so + libSDL2.so,
             reports/native_build.json) or, with --native-mode check and no
             NDK, a compile-only check against bionic headers
  apk        Gradle release build with the game files as assets
  verify     APK structure, ABI, ELF machine, required symbols, signature

Work directory layout:

  input/disc.iso         the image (a symlink for local files)
  game/                  extracted disc, as the runtime reads it
  analysis/              default.xbe + the toolkit's JSON outputs
  project/               generated Android project (native/ and app/)
  reports/               every report and log; uploaded by CI
  out/                   the APK and unstripped libraries
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.android import apk, compat, iso, native, project, xbe_info      # noqa: E402

STAGES = ["intake", "analyze", "recompile", "project", "host", "native", "apk", "verify"]


class StageError(Exception):
    pass


class Context:
    def __init__(self, args):
        self.args = args
        self.work = os.path.abspath(args.work)
        self.reports = os.path.join(self.work, "reports")
        self.logs = os.path.join(self.reports, "logs")
        os.makedirs(self.logs, exist_ok=True)
        self.state_path = os.path.join(self.work, "pipeline_state.json")
        self.state = self._load_state()
        self._logf = None

    def _load_state(self):
        try:
            with open(self.state_path, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {"stages": {}}

    def save_state(self):
        iso.write_json(self.state_path, self.state)

    def path(self, *p):
        return os.path.join(self.work, *p)

    def log(self, msg):
        line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        if self._logf:
            self._logf.write(line + "\n")
            self._logf.flush()

    def run_tool(self, name, cmd, cwd=ROOT, env=None, timeout=None):
        """Run a subprocess, teeing its output into the stage log."""
        self.log("$ " + " ".join(cmd))
        t0 = time.time()
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace")
        tail = []
        for line in p.stdout:
            if self._logf:
                self._logf.write(line)
            tail.append(line.rstrip("\n"))
            if len(tail) > 60:
                tail.pop(0)
        rc = p.wait(timeout=timeout)
        self.log("%s exited %d after %.1fs" % (name, rc, time.time() - t0))
        if rc != 0:
            raise StageError("%s failed (exit %d); last output:\n  %s"
                             % (name, rc, "\n  ".join(tail[-25:])))
        return tail


# ── stages ──────────────────────────────────────────────────────────────────

def stage_intake(ctx):
    a = ctx.args
    if not a.source:
        raise StageError("--source is required for intake (a path or URL to your disc image)")
    info = iso.acquire(a.source, ctx.path("input"), a.sha256, log=ctx.log)
    ctx.log("image: %s, %d bytes, sha256 %s (%s)" % (
        info["source"], info["size"], info["sha256"],
        "verified" if info["verified"] else "UNVERIFIED: no expected hash given"))
    det = iso.detect(info["path"])
    ctx.log("format: %s at partition base 0x%X" % (det["layout"], det["partition_base"]))
    game = ctx.path("game")
    if os.path.exists(game):
        shutil.rmtree(game)
    manifest = iso.extract(info["path"], game, log=ctx.log)
    xbe = iso.find_default_xbe(game)
    manifest["default_xbe"] = os.path.relpath(xbe, game)
    manifest["image"] = {k: info[k] for k in ("sha256", "size", "source", "verified")}
    manifest["detect"] = det
    iso.write_json(ctx.path("game_manifest.json"), manifest)
    iso.write_json(os.path.join(ctx.reports, "intake.json"), {
        "image": manifest["image"], "detect": det, "file_count": manifest["file_count"],
        "total_bytes": manifest["total_bytes"], "default_xbe": manifest["default_xbe"]})
    return {"default_xbe": manifest["default_xbe"], "files": manifest["file_count"],
            "bytes": manifest["total_bytes"], "image_sha256": info["sha256"],
            "verified": info["verified"]}


def _game_xbe(ctx):
    m = ctx.path("game_manifest.json")
    if not os.path.exists(m):
        raise StageError("run intake first (no game_manifest.json)")
    with open(m, encoding="utf-8") as f:
        rel = json.load(f)["default_xbe"]
    return ctx.path("game", rel)


def stage_analyze(ctx):
    src = _game_xbe(ctx)
    adir = ctx.path("analysis")
    os.makedirs(adir, exist_ok=True)
    xbe = os.path.join(adir, "default.xbe")
    shutil.copyfile(src, xbe)
    # The toolkit's own parser; its JSON must sit beside the XBE as
    # <stem>_analysis.json, where disasm and recomp look for it.
    ctx.run_tool("xbe_parser", [sys.executable, "-m", "tools.xbe_parser", xbe,
                                "--json", os.path.join(adir, "default_analysis.json")])
    report, image = xbe_info.analyze(xbe)
    iso.write_json(os.path.join(ctx.reports, "xbe_analysis.json"), report)
    with open(os.path.join(ctx.reports, "xbe_analysis.md"), "w", encoding="utf-8") as f:
        f.write(xbe_info.to_markdown(report))
    if image:
        xbe_info.write_png(os.path.join(adir, "title_image.png"), *image)
    for p in report["problems"]:
        ctx.log("PROBLEM: " + p)
    for w in report["warnings"]:
        ctx.log("warning: " + w)
    if report["problems"]:
        raise StageError("the XBE failed validation: %s" % "; ".join(report["problems"]))
    cov = report["kernel_coverage"]
    ctx.log("title %r (%s), entry %s, %d sections, %d kernel imports (%s routed)" % (
        report["title"], report["title_id"], report["entry_point"], len(report["sections"]),
        len(report["kernel_imports"]), cov.get("routed", "?")))
    return {"title": report["title"], "title_id": report["title_id"],
            "entry_point": report["entry_point"], "kernel_imports": len(report["kernel_imports"]),
            "title_image": bool(image)}


def stage_recompile(ctx):
    adir = ctx.path("analysis")
    xbe = os.path.join(adir, "default.xbe")
    if not os.path.exists(os.path.join(adir, "default_analysis.json")):
        raise StageError("run analyze first")
    py = sys.executable
    disasm = os.path.join(adir, "disasm")
    func_id = os.path.join(adir, "func_id")
    abi = os.path.join(adir, "abi")
    gen = ctx.path("project", "native", "gen")
    if os.path.exists(gen):
        shutil.rmtree(gen)
    os.makedirs(gen)
    ctx.run_tool("disasm", [py, "-m", "tools.disasm", xbe, "-o", disasm, "--force", "-v"])
    ctx.run_tool("func_id", [py, "-m", "tools.func_id", xbe,
                             "--functions", os.path.join(disasm, "functions.json"),
                             "--strings", os.path.join(disasm, "strings.json"),
                             "--xrefs", os.path.join(disasm, "xrefs.json"), "-o", func_id])
    ctx.run_tool("abi_analysis", [py, "-m", "tools.abi_analysis", xbe, "--disasm-dir", disasm,
                                  "--func-id-dir", func_id, "--output-dir", abi])
    title = "game"
    rep = os.path.join(ctx.reports, "xbe_analysis.json")
    if os.path.exists(rep):
        with open(rep, encoding="utf-8") as f:
            title = json.load(f).get("title") or title
    out = os.path.join(adir, "recomp")
    ctx.run_tool("recomp", [py, "-m", "tools.recomp", xbe, "--all", "--split", str(ctx.args.split),
                            "--gen-dir", gen, "--disasm-dir", disasm, "--func-id-dir", func_id,
                            "--abi-dir", abi, "--game-name", title, "-o", out])
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        summary = json.load(f)
    iso.write_json(os.path.join(ctx.reports, "recomp_summary.json"), summary)
    r = compat.write(ctx.work)
    rc = r["recompilation"]
    ctx.log("translated %s/%s functions, %s lines of C, %d untranslated instruction sites, "
            "%s unresolved stubs" % (rc["translated"], rc["functions"], rc["lines_of_c"],
                                     rc["unimplemented_total"], rc["unresolved_stubs"]))
    for u in rc["unimplemented_instructions"]:
        ctx.log("  untranslated: %-24s x%d (first at %s)" % (u["mnemonic"], u["count"],
                                                              u["addresses"][0]))
    if summary.get("failed"):
        raise StageError("%d functions failed to translate" % summary["failed"])
    return {"functions": rc["functions"], "lines": rc["lines_of_c"],
            "untranslated_sites": rc["unimplemented_total"]}


def _xbe_report(ctx):
    rep = os.path.join(ctx.reports, "xbe_analysis.json")
    if not os.path.exists(rep):
        raise StageError("run analyze first (no reports/xbe_analysis.json)")
    with open(rep, encoding="utf-8") as f:
        return json.load(f)


def stage_project(ctx):
    try:
        cfg = project.generate(ctx.work, _xbe_report(ctx), ctx.args.game_config, log=ctx.log)
    except project.ProjectError as e:
        raise StageError(str(e))
    return {"package": cfg["package"], "total_ram_mb": cfg["total_ram_mb"],
            "env": cfg["env"], "app": "not generated yet (phase 7)"}


def _native_run(ctx):
    def run(name, cmd, cwd=None, env=None, timeout=None):
        return ctx.run_tool(name, cmd, cwd=cwd or ROOT, env=env, timeout=timeout)
    return run


def stage_host(ctx):
    try:
        b = native.build_host(ctx.work, run=_native_run(ctx), log=ctx.log)
        result = {"platform": b["platform"], "machine": b["machine"],
                  "guest_functions": b["guest_functions"]}
        if ctx.args.host_seconds:
            r = native.run_host(ctx.work, seconds=ctx.args.host_seconds, log=ctx.log)
            result.update(entry_reached=r["entry_reached"], exit=r["exit"], flips=r["flips"],
                          display_changes=r["display_changes"], crash=bool(r["crash"]))
            if r.get("selftest"):
                result["selftest"] = r["selftest"]
            if r["crash"]:
                for line in r["crash"][:12]:
                    ctx.log(line)
    except native.NativeError as e:
        raise StageError(str(e))
    finally:
        compat.write(ctx.work)
    return result


def stage_native(ctx):
    mode = ctx.args.native_mode
    run = _native_run(ctx)
    try:
        if mode == "auto":
            ndk_dir, _rev = native.find_ndk(native.load_lock(), ctx.args.ndk)
            mode = "ndk" if ndk_dir else "check"
            ctx.log("native mode: %s (%s)" % (mode, ndk_dir or "no pinned NDK found"))
        if mode == "ndk":
            r = native.build_ndk(ctx.work, run=run, ndk=ctx.args.ndk, log=ctx.log)
            result = {"mode": "ndk", "libraries": [l["name"] for l in r["libraries"]]}
        else:
            r = native.compile_check(ctx.work, run=run, log=ctx.log)
            result = {"mode": "check", "objects": r["objects"],
                      "note": "compile-only check; no Android library was built"}
    except native.NativeError as e:
        raise StageError(str(e))
    finally:
        compat.write(ctx.work)
    return result


def stage_apk(ctx):
    try:
        r = apk.build(ctx.work, run=_native_run(ctx), log=ctx.log)
    except (apk.ApkError, native.NativeError) as e:
        raise StageError(str(e))
    return {"apk": r["apk"], "size": r["size"], "sha256": r["sha256"],
            "generated_key": r["signing"].get("generated")}


def stage_verify(ctx):
    try:
        r = apk.verify(ctx.work, log=ctx.log)
    except apk.ApkError as e:
        raise StageError(str(e))
    finally:
        compat.write(ctx.work)
    return {"abis": r["abis"], "game_files": r["game_files"],
            "signature": (r.get("signature") or {}).get("schemes")}


def _not_yet(name):
    def stage(ctx):
        raise StageError("stage %r is not implemented yet in this build of the pipeline" % name)
    return stage


STAGE_FUNCS = {
    "intake": stage_intake,
    "analyze": stage_analyze,
    "recompile": stage_recompile,
    "project": stage_project,
    "host": stage_host,
    "native": stage_native,
    "apk": stage_apk,
    "verify": stage_verify,
}


def run_stages(ctx, stages):
    for name in stages:
        ctx._logf = open(os.path.join(ctx.logs, name + ".log"), "w", encoding="utf-8")
        t0 = time.time()
        ctx.log("=== stage %s ===" % name)
        entry = {"started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        try:
            result = STAGE_FUNCS[name](ctx) or {}
            entry.update(status="ok", result=result)
        except (StageError, iso.IsoError, project.ProjectError, native.NativeError,
                apk.ApkError) as e:
            entry.update(status="failed", error=str(e))
            ctx.log("stage %s FAILED: %s" % (name, e))
        except Exception as e:      # a bug: keep the traceback in the log
            entry.update(status="failed", error="%s: %s" % (type(e).__name__, e))
            ctx.log(traceback.format_exc())
        entry["seconds"] = round(time.time() - t0, 2)
        ctx.state["stages"][name] = entry
        ctx.save_state()
        ctx._logf.close()
        ctx._logf = None
        if entry["status"] != "ok":
            return False
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.android.pipeline", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run stages")
    r.add_argument("--work", required=True, help="per-game work directory")
    r.add_argument("--source", help="disc image: a path, file:// or http(s):// URL")
    r.add_argument("--sha256", help="expected SHA-256 of the image (strongly recommended)")
    r.add_argument("--from", dest="start", choices=STAGES, default="intake")
    r.add_argument("--to", dest="stop", choices=STAGES, default="verify")
    r.add_argument("--only", choices=STAGES)
    r.add_argument("--split", type=int, default=250, help="functions per generated C file")
    r.add_argument("--game-config", help="project: a profile JSON merged into xbr_game.json")
    r.add_argument("--host-seconds", type=int, default=20,
                   help="host: run the title headless this long (0 = build only)")
    r.add_argument("--native-mode", choices=["auto", "ndk", "check"], default="auto",
                   help="native: NDK build, compile-only check, or whichever is possible")
    r.add_argument("--ndk", help="native: NDK directory (default: ANDROID_NDK_HOME etc.)")
    r.add_argument("--skip", action="append", default=[], choices=STAGES,
                   help="leave a stage out of the run (repeatable), e.g. --skip host")
    s = sub.add_parser("status", help="show stage results")
    s.add_argument("--work", required=True)
    args = ap.parse_args(argv)

    if args.cmd == "status":
        try:
            with open(os.path.join(args.work, "pipeline_state.json"), encoding="utf-8") as f:
                st = json.load(f)
        except OSError:
            print("no pipeline state in %s" % args.work)
            return 1
        for name in STAGES:
            e = st["stages"].get(name)
            print("%-10s %s" % (name, "-" if not e else "%s (%.1fs)%s" % (
                e["status"], e.get("seconds", 0), (": " + e["error"]) if e.get("error") else "")))
        return 0

    ctx = Context(args)
    if args.only:
        stages = [args.only]
    else:
        stages = STAGES[STAGES.index(args.start):STAGES.index(args.stop) + 1]
        stages = [st for st in stages if st not in args.skip]
    ok = run_stages(ctx, stages)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
