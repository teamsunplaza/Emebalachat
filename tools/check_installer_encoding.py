#!/usr/bin/env python3
"""Installer text-input encoding gate (session 260910_0005).

Parses installer/setup.iss, resolves EVERY text file the Inno Setup compiler
(ISCC) will consume, and enforces the encoding rules proven in
docs/260910_0005_session_installer-mojibake-brand-design/ (official Inno
Setup whatsnew: BOM-less UTF-8 decoding for .iss/.isl exists only since
6.3; the 31 official bundled .isl translations intentionally ship WITHOUT a
BOM since 6.5):

  RULE 1  Every consumed text file must decode as STRICT UTF-8.
  RULE 2  Project-owned files (installer/setup.iss and installer/languages/
          *.isl) must carry a UTF-8 BOM, so they decode as UTF-8 on every
          Inno Setup 6.x compiler (pre-6.3 falls back to system ANSI without
          one).
  RULE 3  installer/setup.iss must contain the compile-time version guard
          `#if VER < 0x06030000` (refuses pre-6.3 compilers, which would
          mis-decode the BOM-less bundled .isl files as system ANSI and
          garble every non-Latin language).
  RULE 4  Files referenced via `compiler:` must exist in the local Inno
          Setup installation (auto-detected; override with --inno <dir>).

Exit code 0 = PASS, non-zero = violation (usable as a pre-build gate).

Usage:
  python tools/check_installer_encoding.py
  python tools/check_installer_encoding.py --inno "C:\\path\\to\\Inno Setup 6"
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

WS = pathlib.Path(__file__).resolve().parent.parent
ISS = WS / "installer" / "setup.iss"
LANG_DIR = WS / "installer" / "languages"

BOM = b"\xef\xbb\xbf"


def find_inno_dir(override: str | None) -> pathlib.Path | None:
    if override:
        p = pathlib.Path(override)
        return p if p.exists() else None
    candidates = [
        pathlib.Path(r"C:\Users\k1yt\AppData\Local\Programs\Inno Setup 6"),
        pathlib.Path(r"C:\Program Files (x86)\Inno Setup 6"),
        pathlib.Path(r"C:\Program Files\Inno Setup 6"),
    ]
    # also try registry (HKLM/HKCU uninstall entries) without external deps
    try:
        import winreg

        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for view in (getattr(winreg, "KEY_WOW64_64KEY", 0), 0):
                try:
                    key = winreg.OpenKey(
                        hive, r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1",
                        0, winreg.KEY_READ | view)
                    loc = winreg.QueryValueEx(key, "InstallLocation")[0]
                    if loc and pathlib.Path(loc).exists():
                        return pathlib.Path(loc)
                except OSError:
                    pass
    except ImportError:  # non-Windows dev box: candidates only
        pass
    for c in candidates:
        if c.exists():
            return c
    return None


def check_file(path: pathlib.Path, must_have_bom: bool, problems: list[str]) -> None:
    try:
        raw = path.read_bytes()
    except OSError as e:
        problems.append(f"READ-ERROR {path}: {e}")
        return
    try:
        raw.decode("utf-8")
    except UnicodeDecodeError as e:
        problems.append(f"RULE1 {path}: not strict UTF-8 ({e})")
        return
    has_bom = raw.startswith(BOM)
    if must_have_bom and not has_bom:
        problems.append(f"RULE2 {path}: project-owned file missing UTF-8 BOM")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inno", help="Inno Setup install dir override", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    problems: list[str] = []
    rows: list[tuple[str, str]] = []

    if not ISS.exists():
        print(f"FATAL: {ISS} not found")
        return 2

    iss_raw = ISS.read_bytes()
    try:
        iss_text = iss_raw.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        print(f"FATAL: installer/setup.iss is not UTF-8: {e}")
        return 2

    # RULE 2 + RULE 3 on setup.iss itself
    check_file(ISS, must_have_bom=True, problems=problems)
    if "#if VER < 0x06030000" not in iss_text:
        problems.append("RULE3 installer/setup.iss: missing `#if VER < 0x06030000` compiler-version guard")

    # Resolve referenced files
    compiler_refs = re.findall(r'MessagesFile:\s*"compiler:([^"]+)"', iss_text)
    local_refs = re.findall(r'MessagesFile:\s*"(?!compiler:)([^"]+)"', iss_text)

    inno = find_inno_dir(args.inno)
    for ref in compiler_refs:
        rel = ref.replace("\\", "/")
        rows.append((f"compiler:{rel}", "bundled"))
        if inno is None:
            problems.append(f"RULE4 cannot locate Inno Setup install dir to resolve compiler:{rel}"
                            " (pass --inno <dir>)")
            continue
        target = inno / rel
        if not target.exists():
            problems.append(f"RULE4 compiler:{rel} not found at {target}")
            continue
        # Bundled vendor files: RULE1 only (they intentionally have NO BOM since 6.5).
        check_file(target, must_have_bom=False, problems=problems)

    for ref in local_refs:
        target = (WS / "installer" / ref.replace("\\", "/")).resolve()
        rows.append((str(target.relative_to(WS)).replace("\\", "/"), "project"))
        if not target.exists():
            problems.append(f"RULE4 local MessagesFile {ref} not found at {target}")
            continue
        check_file(target, must_have_bom=True, problems=problems)

    # Every .isl under installer/languages must also be BOM'd (covers files a
    # future [Languages] edit might reference)
    for isl in sorted(LANG_DIR.glob("*.isl")):
        if not any(str(isl).lower() == str((WS / "installer" / r.replace("\\", "/")).resolve()).lower()
                   for r in local_refs):
            rows.append((str(isl.relative_to(WS)).replace("\\", "/"), "project(unreferenced)"))
            check_file(isl, must_have_bom=True, problems=problems)

    if not args.quiet:
        print(f"setup.iss: {len(rows)} referenced/unreferenced text inputs checked"
              + (f" (Inno dir: {inno})" if inno else ""))
        for name, kind in rows:
            print(f"  [{kind:20s}] {name}")

    if problems:
        print(f"\nFAIL - {len(problems)} encoding violation(s):")
        for p in problems:
            print("  " + p)
        return 1
    print("\nPASS - all installer text inputs satisfy the UTF-8 encoding rules.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
