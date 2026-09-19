#!/usr/bin/env python3
"""REQ-048 F3 uninstall-contract gate (session 260920).

Static verifier for the shared-engine uninstall handling in
installer/setup.iss (architect 052600 §F3). Uninstall behavior on a real
machine is proven by the user only, so this gate re-derives the contract
from the script source on every run:

  CHECK 1  Triple detection exists: IsOtherEmebalaAppInstalled (ARP scan),
           IsEmebalaProcessRunning (WMI process probe), and the WMI plumbing
           itself (SWbemLocator against root\\cimv2 — Inno Pascal Script
           registers no GetObject, so the 'winmgmts:' moniker form is not
           usable; the locator binds the same namespace).
  CHECK 2  The shared store DelTree({localappdata}\\Emebala\\Common) is
           reachable only behind a SuppressibleMsgBox confirmation that
           compares against IDYES inside CurUninstallStepChanged.
  CHECK 3  Preserve is the default: the delete confirmation passes IDNO as
           the SuppressibleMsgBox default (silent/Enter answers keep the
           engine), and the process probe is fail-closed (except -> keep).
  CHECK 4  The kept-notice marker: both preserve branches show the
           SharedEngineKeptInUse* MsgBox instead of silently keeping.
  CHECK 5  The four F3 CustomMessage keys exist for exactly the 11
           F1-policy languages (english + 10 more); the remaining
           installer languages intentionally fall back to english.
  CHECK 6  Frozen anchors are still present (AppId GUID, pinned model
           SHA-256, unins000 fixed names, F1 SharedEngine page wiring).

Exit code 0 = PASS, non-zero = violation (usable as a pre-build gate).

Usage:
  python tools/check_uninstall_contract.py
"""
from __future__ import annotations

import pathlib
import re
import sys

WS = pathlib.Path(__file__).resolve().parent.parent
ISS = WS / "installer" / "setup.iss"

F3_KEYS = (
    "SharedEngineKeptInUseTitle",
    "SharedEngineKeptInUseBody",
    "SharedEngineDeletePromptTitle",
    "SharedEngineDeletePromptBody",
)

# Same 11-language policy as the F1 SharedEngine pages / B-3 About+Guide.
F3_LANGS = (
    "english",
    "korean",
    "japanese",
    "chinesesimplified",
    "chinesetraditional",
    "spanish",
    "portuguese",
    "brazilianportuguese",
    "french",
    "german",
    "italian",
)

# Frozen contracts that must survive any F3-area edit (REQ-043/REQ-045/
# REQ-047, F1). Values/patterns, not just names, so a silent mutation is
# caught here as well.
FROZEN = (
    ("AppId GUID", r"AppId=\{\{E3B7A1C4-8D2F-4A6E-9C1B-5F0D3E8A7B2C\}"),
    ("pinned model SHA-256",
     r"EXPECTED_MODEL_SHA256 = '5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4'"),
    ("unins000 fixed name (install-time guard)",
     r"unins000\.exe"),
    ("own-AppId exclusion in the family ARP scan",
     r"\{E3B7A1C4-8D2F-4A6E-9C1B-5F0D3E8A7B2C\}_is1"),
    ("F1 SharedEngine page wiring (creation order untouched)",
     r"SharedEnginePage := CreateOutputMsgMemoPage\(AboutPage\.ID,"),
)


def extract_procedure(text: str, name: str) -> str:
    """Return the body of `function|procedure <name>` up to its closing
    top-level `end;` (column 0), or '' when not found."""
    m = re.search(
        r"^(?:function|procedure)\s+" + re.escape(name) + r"\b.*?\n(.*?)^end;",
        text, re.MULTILINE | re.DOTALL)
    return m.group(0) if m else ""


def check_contract(text: str) -> list[str]:
    failures: list[str] = []

    # -- CHECK 1: triple detection ----------------------------------------
    if "function IsOtherEmebalaAppInstalled" not in text:
        failures.append("CHECK 1: IsOtherEmebalaAppInstalled() is missing")
    probe = extract_procedure(text, "IsEmebalaProcessRunning")
    if not probe:
        failures.append("CHECK 1: IsEmebalaProcessRunning() is missing")
    else:
        if "SWbemLocator" not in probe and "winmgmts" not in probe:
            failures.append(
                "CHECK 1: IsEmebalaProcessRunning uses no WMI plumbing "
                "(expected WbemScripting.SWbemLocator against root\\cimv2)")
        if "Win32_Process" not in probe or "ExecQuery" not in probe:
            failures.append(
                "CHECK 1: IsEmebalaProcessRunning issues no Win32_Process "
                "ExecQuery")

    # -- CHECK 2: DelTree behind an explicit IDYES confirmation ------------
    uninstall = extract_procedure(text, "CurUninstallStepChanged")
    if not uninstall:
        failures.append("CHECK 2: CurUninstallStepChanged is missing")
    else:
        m_del = re.search(r"DelTree\(CommonDir", uninstall)
        m_prompt = re.search(
            r"SuppressibleMsgBox\(\s*CustomMessage\('SharedEngineDeletePromptTitle'\)",
            uninstall)
        if not m_del:
            failures.append(
                "CHECK 2: no DelTree(CommonDir) in CurUninstallStepChanged")
        elif not m_prompt or m_prompt.start() > m_del.start():
            failures.append(
                "CHECK 2: DelTree(CommonDir) is not preceded by the "
                "SharedEngineDeletePrompt SuppressibleMsgBox")
        else:
            guard = uninstall[m_prompt.start():m_del.start()]
            if not re.search(r"MB_YESNO,\s*IDNO\)\s*=\s*IDYES\s*then", guard,
                             re.DOTALL):
                failures.append(
                    "CHECK 2: the SharedEngine delete confirmation does not "
                    "require an explicit IDYES before DelTree(CommonDir)")

    # -- CHECK 3: preserve defaults ----------------------------------------
    if uninstall:
        if not re.search(
                r"SuppressibleMsgBox\(\s*CustomMessage\('SharedEngineDeletePromptTitle'\)"
                r".*?MB_YESNO,\s*IDNO\)", uninstall, re.DOTALL):
            failures.append(
                "CHECK 3: the SharedEngine delete confirmation does not "
                "default to IDNO (preserve)")
    if probe:
        except_branch = re.search(r"except(.*?)end;", probe, re.DOTALL)
        if not (except_branch and "Result := True" in except_branch.group(1)):
            failures.append(
                "CHECK 3: IsEmebalaProcessRunning is not fail-closed "
                "(except branch must keep Result := True)")
        if "Result := True" not in probe.split("begin", 1)[-1].split("try", 1)[0]:
            failures.append(
                "CHECK 3: IsEmebalaProcessRunning does not initialize "
                "Result := True before the WMI call")

    # -- CHECK 4: kept-notice MsgBox ---------------------------------------
    if uninstall and "MsgBox(CustomMessage('SharedEngineKeptInUseTitle')" not in uninstall:
        failures.append(
            "CHECK 4: no SharedEngineKeptInUseTitle MsgBox in the preserve "
            "branches")

    # -- CHECK 5: 11-language CustomMessages coverage ----------------------
    for key in F3_KEYS:
        for lang in F3_LANGS:
            if not re.search(rf"^{lang}\.{key}=.+$", text, re.MULTILINE):
                failures.append(
                    f"CHECK 5: missing CustomMessage {lang}.{key}")

    # -- CHECK 6: frozen anchors -------------------------------------------
    for label, pattern in FROZEN:
        if not re.search(pattern, text):
            failures.append(f"CHECK 6: frozen anchor lost: {label}")

    return failures


def main() -> int:
    text = ISS.read_text(encoding="utf-8-sig")
    failures = check_contract(text)
    if failures:
        print("FAIL: uninstall contract violations in installer/setup.iss:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: REQ-048 F3 uninstall contract holds in installer/setup.iss")
    return 0


if __name__ == "__main__":
    sys.exit(main())
