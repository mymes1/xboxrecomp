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
    compat = os.path.join(rep, "compatibility.md")
    if os.path.exists(compat):
        with open(compat, encoding="utf-8") as f:
            out.append(f.read()[:12000])
    for stage in failed:
        out.append("## log tail: %s" % stage)
        out.append("```\n%s\n```" % "\n".join(_tail(os.path.join(rep, "logs", stage + ".log"), 150)))
    if hr and (hr.get("crash") or not hr.get("entry_reached")):
        out.append("## host run log tail")
        out.append("```\n%s\n```" % "\n".join(_tail(os.path.join(rep, "logs", "host_run.log"), 120)))
    text = "\n".join(out)
    if len(text) > LIMIT:
        text = text[:LIMIT - 200] + "\n\n... (truncated; full reports are in the artifact)\n"
    ok = bool(st["stages"]) and not failed
    return text, ok


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
    ap.add_argument("--work", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--post", action="store_true")
    ap.add_argument("--note", action="append", default=[], help="extra line(s) at the top")
    ap.add_argument("--note-file", action="append", default=[],
                    help="file(s) whose contents go at the top, in a code block")
    a = ap.parse_args(argv)
    extra = list(a.note)
    for p in a.note_file:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                extra.append("```\n%s\n```" % f.read()[-8000:])
        except OSError:
            extra.append("(missing: %s)" % p)
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
