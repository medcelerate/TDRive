#!/usr/bin/env python3
"""Post the weekly review agent's write-ups as PR comments.

Reads CONTEXT_DIR/index.json (from select_prs.py) and REVIEW_DIR/pr-<n>.md
(written by the agent). Each comment gets a fixed header - the marker
select_prs.py looks for, and the merge state and CI result exactly as GitHub
reported them - so those facts never depend on what the agent wrote.

Runs in a job that holds a pull-requests: write token but no Claude
credentials, and the agent's job holds no write token. Comments are plain
issue comments: nothing here can approve, request changes, or merge.

Usage: post_reviews.py CONTEXT_DIR REVIEW_DIR   (env: GITHUB_REPOSITORY, GH_TOKEN)
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

MAX_BODY = 60_000


def fmt_checks(checks) -> str:
    counts = checks.get("counts") or {}
    if not counts:
        return "no checks reported"
    parts = [f"{v} {k.lower()}" for k, v in sorted(counts.items())]
    if checks.get("failing"):
        parts.append("failing: " + ", ".join(checks["failing"]))
    return ", ".join(parts)


def main() -> int:
    repo = os.environ["GITHUB_REPOSITORY"]
    ctx, reviews = Path(sys.argv[1]), Path(sys.argv[2])
    index = json.loads((ctx / "index.json").read_text())
    posted = 0
    for pr in index:
        n = pr["number"]
        f = reviews / f"pr-{n}.md"
        if not f.is_file() or not f.read_text().strip():
            print(f"#{n}: the agent wrote no review, nothing posted")
            continue
        body = f.read_text().replace("<!--", "&lt;!--")   # no forged markers
        if len(body) > MAX_BODY:
            body = body[:MAX_BODY] + "\n\n_(truncated)_"
        overlaps = ", ".join(f"#{k}" for k in pr["overlaps"]) or "none"
        rows = [
            ("Merge state", f"{pr['mergeable']} / {pr['merge_state']}"),
            ("CI", fmt_checks(pr["checks"])),
            ("Touches the same files as", overlaps),
        ]
        if pr["binary_files"]:
            rows.append(("Binary files (not inspected)",
                         ", ".join(f"`{p}`" for p in pr["binary_files"])))
        if pr["workflow_files"]:
            rows.append(("CI/workflow files changed",
                         ", ".join(f"`{p}`" for p in pr["workflow_files"])))
        table = "| | |\n|---|---|\n" + "\n".join(f"| {k} | {v} |" for k, v in rows)
        comment = (
            f"<!-- weekly-review head={pr['head_sha']} -->\n"
            f"### Weekly review\n\n{table}\n\n{body}\n\n---\n"
            "_Automated weekly review. It reads the diff and discussion only - "
            "it does not run the code or open binary files. Reply here and the "
            "next weekly run will follow up._"
        )
        if os.environ.get("POST_DRY_RUN"):
            print(f"----- #{n} (dry run) -----\n{comment}\n")
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as t:
            t.write(comment)
        subprocess.run(["gh", "pr", "comment", str(n), "--repo", repo,
                        "--body-file", t.name], check=True)
        os.unlink(t.name)
        posted += 1
        print(f"#{n}: review posted")
    print(f"{posted} review(s) posted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
