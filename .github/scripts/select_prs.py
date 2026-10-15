#!/usr/bin/env python3
"""Pick the open PRs the weekly review should look at and gather their context.

A PR is selected when it has never been reviewed by the weekly job, its head
commit moved since that review, or someone other than the bot commented or
reviewed after it. For each selected PR this writes, under OUT_DIR:

    index.json         every selected PR with its mergeability and CI summary
    pr-<n>/meta.json   metadata, comments, reviews, commits, files, overlaps
    pr-<n>/diff.patch  the PR diff (truncated past MAX_DIFF bytes)

Everything the review agent needs is fetched here, so the agent itself runs
with no network or GitHub tools. Uses only the gh CLI and the standard library.

Usage: select_prs.py OUT_DIR   (env: GITHUB_REPOSITORY, GH_TOKEN)
"""

import json
import os
import subprocess
import sys
import time
from pathlib import Path

MARKER = "<!-- weekly-review head="
BOT_LOGINS = {"github-actions", "github-actions[bot]"}
MAX_DIFF = 400_000
BINARY_SUFFIXES = {".tox", ".toe", ".riv", ".png", ".jpg", ".jpeg", ".gif",
                   ".zip", ".dll", ".dylib", ".so", ".exe", ".bin"}


def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True).stdout


def gh_json(*args: str):
    return json.loads(gh(*args))


def checks_summary(rollup):
    counts = {}
    failing = []
    for c in rollup or []:
        state = (c.get("conclusion") or c.get("status") or c.get("state")
                 or "UNKNOWN").upper()
        counts[state] = counts.get(state, 0) + 1
        if state in ("FAILURE", "ERROR", "CANCELLED", "TIMED_OUT",
                     "ACTION_REQUIRED"):
            failing.append(c.get("name") or c.get("context") or "?")
    return {"counts": counts, "failing": failing}


def last_marker(comments):
    """Latest weekly-review comment by the bot, as (head_sha, createdAt)."""
    found = None
    for c in comments:
        if (c.get("author") or {}).get("login") in BOT_LOGINS and MARKER in c["body"]:
            head = c["body"].split(MARKER, 1)[1].split(" ", 1)[0].split("-->", 1)[0]
            found = (head.strip(), c["createdAt"])
    return found


def main() -> int:
    repo = os.environ["GITHUB_REPOSITORY"]
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    prs = gh_json("pr", "list", "--repo", repo, "--state", "open",
                  "--limit", "100", "--json",
                  "number,title,author,isDraft,headRefOid,baseRefName,"
                  "headRepositoryOwner,labels,url")
    details = {}
    for pr in prs:
        n = pr["number"]
        fields = ("body,mergeable,mergeStateStatus,statusCheckRollup,files,"
                  "commits,reviews,comments,additions,deletions,changedFiles,"
                  "createdAt,updatedAt")
        d = gh_json("pr", "view", str(n), "--repo", repo, "--json", fields)
        # GitHub computes mergeability lazily; the first read is often UNKNOWN.
        for _ in range(5):
            if d.get("mergeable") != "UNKNOWN":
                break
            time.sleep(3)
            d.update(gh_json("pr", "view", str(n), "--repo", repo, "--json",
                             "mergeable,mergeStateStatus"))
        details[n] = d

    files_by_pr = {n: {f["path"] for f in d["files"]} for n, d in details.items()}
    selected = []
    for pr in prs:
        n = pr["number"]
        d = details[n]
        marker = last_marker(d["comments"])
        if marker:
            head, at = marker
            human_after = [
                x for x in d["comments"] + d["reviews"]
                if (x.get("author") or {}).get("login") not in BOT_LOGINS
                and (x.get("createdAt") or x.get("submittedAt") or "") > at
            ]
            if head == pr["headRefOid"] and not human_after:
                print(f"#{n}: no new activity since the last weekly review, skipping")
                continue
            reason = "new commits" if head != pr["headRefOid"] else "new comments"
        else:
            reason = "never reviewed"

        overlaps = {
            other: sorted(files_by_pr[n] & files_by_pr[other])
            for other in files_by_pr
            if other != n and files_by_pr[n] & files_by_pr[other]
        }
        binary = sorted(p for p in files_by_pr[n]
                        if Path(p).suffix.lower() in BINARY_SUFFIXES)
        workflow = sorted(p for p in files_by_pr[n] if p.startswith(".github/"))
        entry = {
            "number": n,
            "title": pr["title"],
            "url": pr["url"],
            "author": (pr.get("author") or {}).get("login"),
            "from_fork": (pr.get("headRepositoryOwner") or {}).get("login")
                         != repo.split("/")[0],
            "draft": pr["isDraft"],
            "head_sha": pr["headRefOid"],
            "base": pr["baseRefName"],
            "labels": [lbl["name"] for lbl in pr.get("labels", [])],
            "reason": reason,
            "mergeable": d.get("mergeable"),
            "merge_state": d.get("mergeStateStatus"),
            "checks": checks_summary(d.get("statusCheckRollup")),
            "overlaps": {str(k): v for k, v in overlaps.items()},
            "binary_files": binary,
            "workflow_files": workflow,
        }
        pdir = out / f"pr-{n}"
        pdir.mkdir(exist_ok=True)
        meta = dict(entry)
        meta.update({
            "body": d.get("body") or "",
            "additions": d["additions"], "deletions": d["deletions"],
            "files": d["files"],
            "commits": [{"oid": c["oid"][:7], "headline": c["messageHeadline"]}
                        for c in d["commits"]],
            "comments": [{"author": (c.get("author") or {}).get("login"),
                          "createdAt": c["createdAt"], "body": c["body"]}
                         for c in d["comments"]],
            "reviews": [{"author": (r.get("author") or {}).get("login"),
                         "state": r.get("state"),
                         "submittedAt": r.get("submittedAt"),
                         "body": r.get("body") or ""}
                        for r in d["reviews"]],
        })
        (pdir / "meta.json").write_text(json.dumps(meta, indent=2))
        diff = gh("pr", "diff", str(n), "--repo", repo)
        if len(diff) > MAX_DIFF:
            diff = diff[:MAX_DIFF] + "\n\n[... diff truncated by select_prs.py ...]\n"
        (pdir / "diff.patch").write_text(diff)
        selected.append(entry)
        print(f"#{n}: selected ({reason})")

    (out / "index.json").write_text(json.dumps(selected, indent=2))
    with open(os.environ.get("GITHUB_OUTPUT", os.devnull), "a") as f:
        f.write(f"count={len(selected)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
