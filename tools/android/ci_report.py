"""Summarise a pipeline work directory for CI: job summary and a check run.

    python3 -m tools.android.ci_report --work W --name android-arm64 [--post]

Writes the summary to $GITHUB_STEP_SUMMARY when set and, with --post, also
publishes it as a check run on the commit ($GITHUB_REPOSITORY, $GITHUB_SHA,
$GH_TOKEN), so the result can be read through the checks API by anyone who
cannot download Actions logs. The text has the stage table, the compatibility
report, the native build and host run details, and the tail of the log of
any failed stage.
"""

import argparse
import json
import os
import re
import subprocess
import sys

LIMIT = 60000


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _tail(path, n):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    return lines[-n:]


def summarize(work, name, extra=None):
    st = _load(os.path.join(work, "pipeline_state.json")) or {"stages": {}}
    rep = os.path.join(work, "reports")
    out = ["# %s" % name, ""]
    for line in extra or []:
        out.append(line)
    out += ["| stage | status | seconds | result / error |", "|---|---|---|---|"]
    failed = []
    for stage, e in st["stages"].items():
        detail = e.get("error") or json.dumps(e.get("result", {}))[:300]
        out.append("| %s | %s | %s | %s |" % (stage, e.get("status"), e.get("seconds"),
                                            detail.replace("|", "\\|").replace("\n", " ")[:400]))
        if e.get("status") != "ok":
            failed.append(stage)
    out.append("")
    nb = _load(os.path.join(rep, "native_build.json"))
    if nb:
        out.append("## native_build.json")
        out.append("```json\n%s\n```" % json.dumps(nb, indent=1)[:6000])
    hr = _load(os.path.join(rep, "host_runtime.json"))
    if hr:
        keep = {k: hr.get(k) for k in ("platform", "entry_reached", "exit", "seconds", "flips",
                                       "display_changes", "selftest", "failed_step")}
        keep["boot"] = hr.get("boot", [])[-25:]
        keep["crash"] = hr.get("crash", [])[:30]
        keep["unimpl"] = hr.get("unimpl", [])[:10]
        keep["title_lines"] = hr.get("title_lines", [])[:30]
        keep["status"] = hr.get("status", [])[-4:]
        out.append("## host_runtime.json (excerpt)")
        out.append("```json\n%s\n```" % json.dumps(keep, indent=1)[:9000])
    rt = _load(os.path.join(rep, "runtime_test.json"))
    if rt:
        keep = {k: rt.get(k) for k in ("platform", "execution", "device", "install", "entry_reached",
                                       "exit", "seconds", "flips", "display_changes", "selftest",
                                       "failed_step", "game_process_alive_at_end")}
        keep["boot"] = rt.get("boot", [])[-30:]
        keep["crash"] = rt.get("crash", [])[:40]
        keep["fatal_lines"] = rt.get("fatal_lines", [])[:20]
        keep["title_lines"] = rt.get("title_lines", [])[:30]
        keep["status"] = rt.get("status", [])[-4:]
        for k in ("game_zip", "files_import", "files_install"):
            if k in rt:
                keep[k] = rt[k]
        out.append("## runtime_test.json (excerpt)")
        out.append("```json\n%s\n```" % json.dumps(keep, indent=1)[:12000])
        if rt.get("crash_buffer"):
            out.append("## crash buffer")
            out.append("```\n%s\n```" % "\n".join(rt["crash_buffer"][:80]))
        # Layout and kernel warnings are printed long before the tail; a
        # window that failed to map is the cause of a crash many lines later.
        key = [l for l in _tail(os.path.join(rep, "logs", "device_runtime.log"), 100000)
               if re.search(r"WARNING|ERROR|FAILED|\[CRASH\]|\[FILES\]|XBRTEST|guest window|"
                            r"Contiguous window|aperture:|RAM mirror", l)
               and "Unresolved kernel ordinal" not in l]
        if key:
            out.append("## device runtime key lines")
            out.append("```\n%s\n```" % "\n".join(key[:80]))
        out.append("## device runtime log tail")
        out.append("```\n%s\n```" % "\n".join(_tail(os.path.join(rep, "logs", "device_runtime.log"), 120)))
    compat = os.path.join(rep, "compatibility.md")
    if os.path.exists(compat):
        with open(compat, encoding="utf-8") as f:
            out.append(f.read()[:12000])
    for stage in failed:
        logp = os.path.join(rep, "logs", stage + ".log")
        lines = _tail(logp, 100000)
        # The cause is usually far above the tail (Gradle prints the stack
        # after "What went wrong"); keep those lines and what follows them.
        key = []
        for i, line in enumerate(lines):
            if re.search(r"What went wrong|error:|Error:|FAILED|Caused by|Exception:|undefined reference|fatal", line):
                key.extend(lines[i:i + 6])
                if len(key) > 120:
                    break
        if key:
            out.append("## key lines: %s" % stage)
            out.append("```\n%s\n```" % "\n".join(key[:120]))
        out.append("## log tail: %s" % stage)
        out.append("```\n%s\n```" % "\n".join(lines[-80:]))
    if hr and (hr.get("crash") or not hr.get("entry_reached")):
        out.append("## host run log tail")
        out.append("```\n%s\n```" % "\n".join(_tail(os.path.join(rep, "logs", "host_run.log"), 120)))
    text = "\n".join(out)
    if len(text) > LIMIT:
        text = text[:LIMIT - 200] + "\n\n... (truncated; full reports are in the artifact)\n"
    ok = bool(st["stages"]) and not failed
    return text, ok


def summarize_text(path, name, outcome, extra=None):
    """A test's own output as the report; ok only when the step succeeded."""
    ok = outcome == "success"
    lines = ["## %s: %s" % (name, "ok" if ok else "FAILED (step outcome: %s)" % (outcome or "unknown")), ""]
    lines += list(extra or [])
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            body = f.read()
    except OSError:
        body = "(no output: %s was not written)" % path
    if len(body) > LIMIT - 2000:
        body = "... (start truncated)\n" + body[-(LIMIT - 2000):]
    lines += ["", "```", body.rstrip("\n"), "```"]
    return "\n".join(lines), ok


def post_check(name, text, ok):
    repo, sha = os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GITHUB_SHA")
    if not repo or not sha:
        print("not in GitHub Actions; check run not posted", file=sys.stderr)
        return
    body = {"name": name, "head_sha": sha, "status": "completed",
            "conclusion": "success" if ok else "failure",
            "output": {"title": "%s: %s" % (name, "ok" if ok else "FAILED"),
                       "summary": "xboxrecomp Android pipeline (%s)" % name,
                       "text": text}}
    path = os.path.join(os.environ.get("RUNNER_TEMP", "/tmp"), "check-%s.json" % name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(body, f)
    subprocess.run(["gh", "api", "-X", "POST", "repos/%s/check-runs" % repo, "--input", path,
                    "--jq", ".html_url"], check=False)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", help="pipeline work directory to summarise")
    ap.add_argument("--text-file",
                    help="instead of --work: report this file (a test's output) as it is")
    ap.add_argument("--outcome", default="",
                    help="with --text-file: the step outcome; only 'success' is ok")
    ap.add_argument("--name", required=True)
    ap.add_argument("--post", action="store_true")
    ap.add_argument("--note", action="append", default=[], help="extra line(s) at the top")
    ap.add_argument("--note-file", action="append", default=[],
                    help="file(s) whose contents go at the top, in a code block")
    a = ap.parse_args(argv)
    if bool(a.work) == bool(a.text_file):
        ap.error("give exactly one of --work and --text-file")
    extra = list(a.note)
    for p in a.note_file:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                extra.append("```\n%s\n```" % f.read()[-8000:])
        except OSError:
            extra.append("(missing: %s)" % p)
    if a.text_file:
        text, ok = summarize_text(a.text_file, a.name, a.outcome, extra)
    else:
        text, ok = summarize(a.work, a.name, extra)
    summ = os.environ.get("GITHUB_STEP_SUMMARY")
    if summ:
        with open(summ, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    else:
        print(text)
    if a.post:
        post_check(a.name, text, ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
