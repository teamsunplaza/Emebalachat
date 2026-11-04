#!/usr/bin/env python3
"""Release-notes SHA256 gate (v0.10.5 lesson, session 261104_0001).

Purpose
-------
The in-app update checker (src/update_checker.hpp, ExtractSha256FromNotes)
is FAIL-CLOSED: it downloads the release's first `.exe` asset, then requires
the release NOTES body to carry a line whose first non-blank content is
`SHA256:` followed by exactly 64 hex chars. When the line is absent or
malformed the update is refused and the user sees "다운로드한 파일이
손상되었습니다" even though the file is intact. v0.10.3 shipped without the
line; v0.10.5 was generated from CHANGELOG.md and repeated the mistake.

This script is the one-command guard so no future release can repeat it:

  Mode A (pre-publish, notes file):
    python tools/check_release_notes.py --tag v0.10.5 \
        --file installer/output/Emebalachat_Setup_0.10.5.exe \
        --notes docs/<session>/release-notes-v0.10.5.md
    Verifies the notes file parses to the installer's real SHA-256.

  Mode B (post-publish, live release):
    python tools/check_release_notes.py --tag v0.10.5 \
        --file installer/output/Emebalachat_Setup_0.10.5.exe
    Fetches https://api.github.com/repos/teamsunplaza/Emebalachat/releases/tags/<tag>
    (auth: `gh api` when available, else GITHUB_TOKEN/GH_TOKEN env) and runs
    the same verification against the live body.

The extraction rules mirror the C++ parser exactly (first matching line
wins, leading blanks allowed, exactly 64 hex chars, case-insensitive,
lowercased result). Exit 0 = pass; exit 1 = fail (line missing/malformed or
hash mismatch); exit 2 = usage/IO error.

Stdlib only. Read-only: never writes to the repo or to GitHub.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

REPO = "teamsunplaza/Emebalachat"


def extract_sha256_line(body: str) -> str | None:
    """Mirror of ExtractSha256FromNotes: first 'SHA256:'-led line with exactly
    64 hex chars (leading blanks tolerated). Returns lowercase or None."""
    for line in body.split("\n"):
        stripped = line.lstrip(" \t")
        if not stripped.startswith("SHA256:"):
            continue
        rest = stripped[len("SHA256:"):].lstrip(" \t\r")
        if not rest.strip():
            return None
        token = rest.split()[0] if rest.split() else ""
        if len(token) != 64 or any(c not in "0123456789abcdefABCDEF" for c in token):
            return None
        return token.lower()
    return None


def file_sha256_hex(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_live_release_body(tag: str) -> str:
    url = f"https://api.github.com/repos/{REPO}/releases/tags/{tag}"
    # Prefer gh (handles auth + protocol) when present.
    try:
        proc = subprocess.run(
            ["gh", "api", f"repos/{REPO}/releases/tags/{tag}", "--jq", ".body"],
            capture_output=True, text=True, timeout=60,
        )
        if proc.returncode == 0:
            return proc.stdout
    except FileNotFoundError:
        pass
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""
    req = urllib.request.Request(url, headers={
        "Authorization": f"token {token}" if token else "",
        "Accept": "application/vnd.github+json",
        "User-Agent": "emebala-release-gate",
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.loads(r.read().decode())
    return data.get("body") or ""


def verify(body: str, installer: str, label: str) -> bool:
    found = extract_sha256_line(body)
    if not found:
        print(f"FAIL [{label}]: no valid 'SHA256: <64hex>' line in the release notes "
              f"(the in-app updater is fail-closed and will refuse the download)", file=sys.stderr)
        return False
    actual = file_sha256_hex(installer)
    if found != actual:
        print(f"FAIL [{label}]: notes hash {found[:16]}... != installer hash {actual[:16]}...",
              file=sys.stderr)
        return False
    print(f"PASS [{label}]: notes SHA256 matches {installer} ({actual[:16]}...)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tag", help="release tag, e.g. v0.10.5")
    ap.add_argument("--file", help="path to the built setup .exe")
    ap.add_argument("--notes", help="local release-notes markdown (pre-publish mode)")
    ap.add_argument("--self-test", action="store_true", help="run the parser unit checks")
    args = ap.parse_args()

    if args.self_test:
        ok = True
        ok &= extract_sha256_line("SHA256: " + "a" * 64) == "a" * 64
        ok &= extract_sha256_line("  \tSHA256:" + "B" * 64 + "\r") == "b" * 64
        ok &= extract_sha256_line("no hash here") is None
        ok &= extract_sha256_line("SHA256: " + "a" * 63) is None
        ok &= extract_sha256_line("SHA256: " + "g" * 64) is None
        ok &= extract_sha256_line("intro\nSHA256: " + "c" * 64 + "\nmore") == "c" * 64
        print("self-test:", "PASS" if ok else "FAIL")
        return 0 if ok else 1

    if not args.tag or not args.file:
        print("usage error: --tag and --file are required (or use --self-test)", file=sys.stderr)
        return 2

    if not os.path.isfile(args.file):
        print(f"usage error: installer not found: {args.file}", file=sys.stderr)
        return 2

    if args.notes:
        if not os.path.isfile(args.notes):
            print(f"usage error: notes file not found: {args.notes}", file=sys.stderr)
            return 2
        with open(args.notes, encoding="utf-8") as f:
            body = f.read()
        if not verify(body, args.file, f"notes file {args.notes}"):
            return 1
        print("pre-publish gate passed; publish, then re-run without --notes for the live check")
        return 0

    body = fetch_live_release_body(args.tag)
    return 0 if verify(body, args.file, f"live release {args.tag}") else 1


if __name__ == "__main__":
    sys.exit(main())
