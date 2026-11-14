"""Compatibility report: what the pipeline knows about a title, by stage.

Inputs are the artefacts the stages leave in the work directory; nothing here
re-runs a tool. The report keeps the levels distinct and only claims the ones
there is evidence for:

    Compiled              the generated C and runtime linked into an arm64 library
    Runtime initialized   boot log reached the recompiled entry point on ARM64
    Game booted           the title's own code ran and frames were presented
    Gameplay tested       a person played it (never set by automation)
    Full compatibility    never set by automation

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
    evidence = {}
    level = None
    if xbe and not xbe.get("problems"):
        level = "Analyzed"
        evidence["Analyzed"] = "reports/xbe_analysis.json"
    if summary and summary.get("translated") and not summary.get("failed"):
        level = "Recompiled"
        evidence["Recompiled"] = "reports/recomp_summary.json"
    if native and native.get("ok"):
        level = "Compiled"
        evidence["Compiled"] = "reports/native_build.json"
    if runtime and runtime.get("entry_reached"):
        level = "Runtime initialized"
        evidence["Runtime initialized"] = runtime.get("log", "reports/runtime_test.json")
        if runtime.get("frames_presented", 0) > 0 and runtime.get("guest_progress"):
            level = "Game booted"
            evidence["Game booted"] = runtime.get("frames", "reports/runtime_test.json")

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
    }
    return report


def to_markdown(r):
    out = ["# Compatibility report: %s" % (r.get("title") or "unknown title"), ""]
    out.append("**Highest level with evidence: %s**" % (r["level"] or "none"))
    out.append("")
    out.append("| Level | Evidence |")
    out.append("|---|---|")
    for lv in r["levels"]:
        out.append("| %s | %s |" % (lv, r["evidence"].get(lv, "—")))
    out += ["", "Title ID `%s`, default.xbe SHA-256 `%s`." % (r.get("title_id"), r.get("xbe_sha256")), ""]
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
        out.append("## Runtime test (%s)" % rt.get("host", "?"))
        for k in ("entry_reached", "frames_presented", "guest_progress", "exit"):
            out.append("- %s: %s" % (k, rt.get(k)))
        for line in rt.get("title_lines", [])[:40]:
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
