"""Compatibility report: what the pipeline knows about a title, by stage.

Inputs are the artefacts the stages leave in the work directory; nothing here
re-runs a tool. The report keeps the levels distinct and only claims the ones
there is evidence for:

    Compiled              the generated C and runtime linked into an Android
                          arm64-v8a library (reports/native_build.json)
    Runtime initialized   on an Android device or emulator, the boot log
                          reached the recompiled entry point
                          (reports/runtime_test.json, platform android-*)
    Game booted           ... and the title's own code kept running and drew:
                          pushbuffer flips, or a scanout surface that changed
    Gameplay tested       a person played it (never set by automation)
    Full compatibility    never set by automation

Two kinds of evidence are reported but never raise the level, because they
say nothing about a device: the compile-only check (reports/native_check.json,
nothing linked) and desktop host runs of the same project
(reports/host_build.json, host_runtime.json).

Every unimplemented instruction, unresolved call target and unrouted kernel
import is listed with its address; nothing is summarised away.
"""

import json
import os

LEVELS = ["Analyzed", "Recompiled", "Compiled", "Runtime initialized", "Game booted",
          "Gameplay tested", "Full compatibility"]


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def build(work):
    rep = os.path.join(work, "reports")
    xbe = _load(os.path.join(rep, "xbe_analysis.json"))
    summary = _load(os.path.join(rep, "recomp_summary.json"))
    native = _load(os.path.join(rep, "native_build.json"))
    runtime = _load(os.path.join(rep, "runtime_test.json"))
    apk = _load(os.path.join(rep, "apk_verify.json"))
    check = _load(os.path.join(rep, "native_check.json"))
    host_build = _load(os.path.join(rep, "host_build.json"))
    host_run = _load(os.path.join(rep, "host_runtime.json"))
    intake = _load(os.path.join(rep, "intake.json"))
    evidence = {}
    level = None
    if xbe and not xbe.get("problems"):
        level = "Analyzed"
        evidence["Analyzed"] = "reports/xbe_analysis.json"
    if summary and summary.get("translated") and not summary.get("failed"):
        level = "Recompiled"
        evidence["Recompiled"] = "reports/recomp_summary.json"
    if native and native.get("ok") and str(native.get("target", "")).startswith("android-"):
        level = "Compiled"
        evidence["Compiled"] = "reports/native_build.json"
    on_device = runtime and str(runtime.get("platform", "")).startswith("android-")
    if level == "Compiled" and on_device and runtime.get("entry_reached"):
        # Where it ran is part of the evidence: a device, or an emulator
        # running the arm64 code under ARM translation.
        where = "%s, %s" % ((runtime.get("device") or {}).get("model") or "device",
                            runtime.get("execution") or "?")
        level = "Runtime initialized"
        evidence["Runtime initialized"] = "%s (%s)" % (runtime.get("log", "reports/runtime_test.json"), where)
        drew = runtime.get("flips", 0) > 0 or runtime.get("display_changes", 0) > 0
        if drew and not runtime.get("crash"):
            level = "Game booted"
            evidence["Game booted"] = "%s (%s)" % (runtime.get("display_image") or "reports/runtime_test.json", where)

    unimpl = []
    if summary:
        for mnem, addrs in sorted((summary.get("unimplemented") or {}).items(),
                                  key=lambda kv: -len(kv[1])):
            unimpl.append({"mnemonic": mnem, "count": len(addrs),
                           "addresses": ["0x%08X" % a for a in addrs]})
    report = {
        "title": (xbe or {}).get("title"),
        "title_id": (xbe or {}).get("title_id"),
        "xbe_sha256": (xbe or {}).get("sha256"),
        # The disc image the title came from, and which supplied digest
        # (SHA-256, SHA-1 or none) it was checked against at intake.
        "image": (intake or {}).get("image"),
        "level": level,
        "levels": LEVELS,
        "evidence": evidence,
        "xbe": {
            "problems": (xbe or {}).get("problems", []),
            "warnings": (xbe or {}).get("warnings", []),
            "graphics_path": (xbe or {}).get("graphics_path"),
            "libraries": (xbe or {}).get("libraries", []),
            "kernel_coverage": (xbe or {}).get("kernel_coverage"),
        },
        "recompilation": {
            "functions": (summary or {}).get("total"),
            "translated": (summary or {}).get("translated"),
            "failed": (summary or {}).get("failed"),
            "lines_of_c": (summary or {}).get("total_lines"),
            "unresolved_stubs": (summary or {}).get("unresolved_stubs"),
            "unimplemented_instructions": unimpl,
            "unimplemented_total": sum(u["count"] for u in unimpl),
        },
        "native_build": native,
        "runtime_test": runtime,
        "apk": apk,
        # Reported, never counted towards the level (see the module docstring).
        "compile_check": check,
        "host": {"build": host_build, "runtime": _host_summary(host_run)} if (host_build or host_run) else None,
    }
    return report


def _host_summary(r):
    if not r:
        return None
    keep = ("platform", "entry_reached", "title_returned", "exit", "seconds", "flips",
            "display_changes", "display_set", "selftest", "display_bmp", "log", "failed_step", "note",
            "render", "present", "audio", "usb")
    out = {k: r.get(k) for k in keep if k in r}
    out["crash"] = r.get("crash", [])[:20]
    out["unimpl_reached"] = r.get("unimpl", [])[:20]
    out["icall_failures"] = r.get("icall", [])[:20]
    out["title_lines"] = r.get("title_lines", [])[:40]
    return out


def _image_line(img):
    if not img:
        return "Disc image: no intake record."
    files = img.get("kind") == "files"
    line = "%s SHA-256 `%s`, SHA-1 %s: " % (
        "Game files (a zip of the disc's files)" if files else "Disc image",
        img.get("sha256"), "`%s`" % img["sha1"] if img.get("sha1") else "not recorded")
    if not img.get("verified"):
        return line + "**unverified** (no expected digest was given)."
    names = {"sha256": "SHA-256", "sha1": "SHA-1"}
    return line + "verified against the supplied %s of the %s." % (
        " and ".join(names.get(a, a) for a in img.get("verified_with") or []),
        "zip" if files else "zip it came in" if img.get("matched") == "container" else "image")


def to_markdown(r):
    out = ["# Compatibility report: %s" % (r.get("title") or "unknown title"), ""]
    out.append("**Highest level with evidence: %s**" % (r["level"] or "none"))
    out.append("")
    out.append("| Level | Evidence |")
    out.append("|---|---|")
    for lv in r["levels"]:
        out.append("| %s | %s |" % (lv, r["evidence"].get(lv, "—")))
    out += ["", "Title ID `%s`, default.xbe SHA-256 `%s`." % (r.get("title_id"), r.get("xbe_sha256"))]
    out += [_image_line(r.get("image")), ""]
    x = r["xbe"]
    if x["problems"] or x["warnings"]:
        out.append("## XBE findings")
        out += ["- **problem**: %s" % p for p in x["problems"]]
        out += ["- warning: %s" % w for w in x["warnings"]]
        out.append("")
    out.append("Graphics path: %s" % x.get("graphics_path"))
    out.append("")
    cov = x.get("kernel_coverage") or {}
    if cov and "error" not in cov:
        out.append("## Kernel imports")
        out.append("%d imported, %d routed by the runtime." % (cov.get("imported", 0), cov.get("routed", 0)))
        for u in cov.get("unimplemented", []):
            out.append("- not implemented: %d %s" % (u["ordinal"], u["name"]))
        if cov.get("needs_wrapper"):
            out.append("- implemented but not routed: %s" % ", ".join(map(str, cov["needs_wrapper"])))
        out.append("")
    rc = r["recompilation"]
    out.append("## Recompilation")
    out.append("%s of %s functions translated (%s failed), %s lines of C, %s unresolved call stubs."
               % (rc["translated"], rc["functions"], rc["failed"], rc["lines_of_c"],
                  rc["unresolved_stubs"]))
    if rc["unimplemented_instructions"]:
        out.append("")
        out.append("%d instruction sites are not translated; each is a RECOMP_UNIMPL call that logs "
                   "(or traps with RECOMP_UNIMPL_TRAP=1) when reached:" % rc["unimplemented_total"])
        out.append("")
        out.append("| Mnemonic | Sites | First addresses |")
        out.append("|---|---|---|")
        for u in rc["unimplemented_instructions"]:
            out.append("| `%s` | %d | %s |" % (u["mnemonic"], u["count"], ", ".join(u["addresses"][:6])))
    else:
        out.append("Every instruction was translated.")
    out.append("")
    nb = r.get("native_build")
    if nb:
        out.append("## Native ARM64 build")
        out.append("- result: %s" % ("ok" if nb.get("ok") else "FAILED"))
        for lib in nb.get("libraries", []):
            out.append("- `%s`: %s, %s bytes, %s" % (lib.get("name"), lib.get("machine"),
                                                     lib.get("size"), lib.get("tls", "")))
        out.append("")
    rt = r.get("runtime_test")
    if rt:
        out.append("## Runtime test on %s (%s)" % (rt.get("platform", "?"), rt.get("execution", "?")))
        for k in ("device", "entry_reached", "flips", "display_changes", "exit", "selftest"):
            if k in rt:
                out.append("- %s: %s" % (k, rt.get(k)))
        if rt.get("render"):
            # Which renderer drew: the Vulkan back end, or the CPU rasteriser
            # (and why). A booted title on the CPU rasteriser is still booted.
            out.append("- renderer: %s (%s)" % (rt["render"].get("backend"),
                                                rt["render"].get("detail")))
        if rt.get("present"):
            # What put the frames on the screen: the Vulkan swapchain
            # presenter, or the SDL renderer (and why).
            out.append("- presenter: %s (%s)" % (rt["present"].get("backend"),
                                                 rt["present"].get("detail")))
        for key, label in (("screen_check", "screen"),
                           ("screen_after_resume", "screen after background/foreground")):
            sc = rt.get(key)
            if sc:
                out.append("- %s: %s -- %s" % (label, "matches" if sc.get("ok") else "NO MATCH",
                                               sc.get("reason")))
        if rt.get("usb"):
            out.append("- controllers (USB model): %s" % rt["usb"])
        if rt.get("audio"):
            out.append("- audio output: %s (%s)" % ("initialised" if rt["audio"].get("ok")
                                                     else "FAILED", rt["audio"].get("detail")))
        if rt.get("touch_test"):
            out.append("- on-screen controller: %s" % rt["touch_test"].get("verdict"))
        if rt.get("lifecycle_test"):
            lt = rt["lifecycle_test"]
            out.append("- background/foreground: %s" % lt.get("verdict"))
        for line in rt.get("crash", [])[:12]:
            out.append("    %s" % line)
        for line in rt.get("title_lines", [])[:40]:
            out.append("    %s" % line)
        out.append("")
    ck = r.get("compile_check")
    if ck:
        out.append("## Compile-only check (not a build; does not count as Compiled)")
        out.append("- %s: %s objects for %s, machines %s" % (
            "ok" if ck.get("ok") else "FAILED", ck.get("objects"), ck.get("target"),
            ", ".join(ck.get("machines", []))))
        out.append("")
    h = r.get("host")
    if h:
        out.append("## Desktop host check (not Android evidence)")
        hb, hr = h.get("build") or {}, h.get("runtime") or {}
        if hb:
            out.append("- build: %s, %s, %s of %s guest functions linked" % (
                hb.get("platform"), hb.get("machine"), hb.get("guest_functions"),
                hb.get("expected_guest_functions")))
        if hr:
            out.append("- run: entry reached %s; %s after %ss; flips %s; scanout changes %s" % (
                hr.get("entry_reached"), hr.get("exit"), hr.get("seconds"), hr.get("flips"),
                hr.get("display_changes")))
            if hr.get("selftest"):
                out.append("- test-title self-test: %(pass)d pass, %(fail)d fail" % hr["selftest"])
            for line in hr.get("crash", [])[:12]:
                out.append("    %s" % line)
            for line in hr.get("title_lines", [])[:20]:
                out.append("    %s" % line)
        out.append("")
    return "\n".join(out) + "\n"


def write(work):
    r = build(work)
    rep = os.path.join(work, "reports")
    os.makedirs(rep, exist_ok=True)
    with open(os.path.join(rep, "compatibility.json"), "w", encoding="utf-8") as f:
        json.dump(r, f, indent=2)
    with open(os.path.join(rep, "compatibility.md"), "w", encoding="utf-8") as f:
        f.write(to_markdown(r))
    return r
