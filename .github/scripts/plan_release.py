#!/usr/bin/env python3
"""Decide whether the weekly run should cut a release, and which version.

Releases when origin/main has commits after the newest plain vX.Y.Z tag
(pre-release tags like v1.2.0-ea are ignored). The bump comes from the PRs
merged since that tag: any `release:major` label -> major, any
`release:minor` -> minor, otherwise patch.

Blocks (release=false) while code scanning has open alerts of high or
critical security severity, and lists them in the job summary.

Writes to GITHUB_OUTPUT: release=true|false, tag=vX.Y.Z, reason=...
Usage: plan_release.py   (env: GITHUB_REPOSITORY, GH_TOKEN; run in a full clone)
"""

import json
import os
import re
import subprocess
import sys

SEMVER = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def run(*args: str) -> str:
    return subprocess.run(list(args), check=True, capture_output=True,
                          text=True).stdout.strip()


def output(**kv) -> None:
    with open(os.environ.get("GITHUB_OUTPUT", os.devnull), "a") as f:
        for k, v in kv.items():
            f.write(f"{k}={v}\n")


def summary(text: str) -> None:
    print(text)
    with open(os.environ.get("GITHUB_STEP_SUMMARY", os.devnull), "a") as f:
        f.write(text + "\n")


def main() -> int:
    repo = os.environ["GITHUB_REPOSITORY"]
    tags = [t for t in run("git", "tag", "--list", "v*").split() if SEMVER.match(t)]
    if not tags:
        summary("No vX.Y.Z tag found; tag the first release by hand.")
        output(release="false", reason="no-base-tag")
        return 0
    last = max(tags, key=lambda t: tuple(int(x) for x in SEMVER.match(t).groups()))
    ahead = int(run("git", "rev-list", "--count", f"{last}..origin/main"))
    if ahead == 0:
        summary(f"main has nothing new since {last}; no release.")
        output(release="false", reason="no-changes")
        return 0

    since = run("git", "log", "-1", "--format=%cI", last)
    merged = json.loads(run(
        "gh", "pr", "list", "--repo", repo, "--state", "merged", "--base", "main",
        "--search", f"merged:>{since}", "--limit", "200",
        "--json", "number,title,labels"))
    labels = {lbl["name"] for pr in merged for lbl in pr["labels"]}
    major, minor, patch = (int(x) for x in SEMVER.match(last).groups())
    if "release:major" in labels:
        nxt, kind = f"v{major + 1}.0.0", "major"
    elif "release:minor" in labels:
        nxt, kind = f"v{major}.{minor + 1}.0", "minor"
    else:
        nxt, kind = f"v{major}.{minor}.{patch + 1}", "patch"

    # Security gate: open high/critical code-scanning alerts block the release.
    try:
        alerts = json.loads(run(
            "gh", "api", f"repos/{repo}/code-scanning/alerts?state=open&per_page=100"))
    except subprocess.CalledProcessError as e:
        err = e.stderr.strip()
        if "no analysis found" not in err:
            # Fail closed: an unreadable security state must not ship.
            summary(f"**Release {nxt} blocked**: could not read code-scanning "
                    f"alerts ({err[:200]}).")
            output(release="false", reason="security-unknown")
            return 0
        alerts = None
        summary("Code scanning has not analysed this repository yet "
                "(CodeQL's first run is pending); not gating on alerts.")
    blocking = [a for a in alerts or []
                if (a.get("rule") or {}).get("security_severity_level")
                in ("high", "critical")]
    if blocking:
        summary(f"**Release {nxt} blocked**: {len(blocking)} open high/critical "
                "code-scanning alert(s):")
        for a in blocking[:20]:
            summary(f"- [{a['rule']['security_severity_level']}] "
                    f"{a['rule'].get('description', a['rule'].get('id'))} - {a['html_url']}")
        output(release="false", reason="security-alerts")
        return 0

    summary(f"Releasing **{nxt}** ({kind} bump from {last}): {ahead} commit(s), "
            f"{len(merged)} merged PR(s).")
    for pr in merged:
        summary(f"- #{pr['number']} {pr['title']}")
    output(release="true", tag=nxt, reason=kind)
    return 0


if __name__ == "__main__":
    sys.exit(main())
