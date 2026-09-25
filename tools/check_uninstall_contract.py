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
  CHECK 2  The shared store cleanup (CleanupSharedEngineStore, M7 A-3) is
           reachable only behind a SuppressibleMsgBox confirmation that
           compares against IDYES inside CurUninstallStepChanged. The
           pre-A-3 blanket DelTree(CommonDir, ...) is PROHIBITED (§V2-5.4
           user-model protection) and its return trips this gate.
  CHECK 8  M7 A-3 registry-aware cleanup invariants: the cleanup procedure
           exists and fail-closes (a damaged/unreadable registry preserves
           the ENTIRE store), deletes ONLY origin:"bundled" items' files,
           preserves user items, and guards every files[] name with a bare-
           name check before DeleteFile.
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
  CHECK 7  Call-site visibility (P5 review #5): IsEmebalaProcessRunning() is
           actually invoked inside CurUninstallStepChanged, and BOTH preserve
           branches show the kept-notice box — deleting an else-if branch or
           the probe call must trip this gate even when CHECKs 1-4 pass.

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
        # REQ-048 P6 R4: pin the filter itself — narrowing the LIKE pattern
        # (e.g. to an exact exe name) would blind the in-use detection while
        # the WMI plumbing checks above still passed. Inno Pascal doubles
        # single quotes inside string literals.
        if "LIKE ''Emebala%''" not in probe:
            failures.append(
                "CHECK 1: IsEmebalaProcessRunning narrowed the process filter "
                "(expected Win32_Process Name LIKE ''Emebala%'')")

    # -- CHECK 2: registry-aware cleanup behind an explicit IDYES -----------
    # confirmation, and the blanket DelTree of the shared store is gone.
    uninstall = extract_procedure(text, "CurUninstallStepChanged")
    if not uninstall:
        failures.append("CHECK 2: CurUninstallStepChanged is missing")
    else:
        if re.search(r"DelTree\(\s*CommonDir", uninstall):
            failures.append(
                "CHECK 2: blanket DelTree(CommonDir) is back — M7 A-3 "
                "requires registry-aware CleanupSharedEngineStore (§V2-5.4 "
                "protects origin:\"user\" models; §V2-8.5 cleans bundles only)")
        m_del = re.search(r"CleanupSharedEngineStore\(", uninstall)
        m_prompt = re.search(
            r"SuppressibleMsgBox\(\s*CustomMessage\('SharedEngineDeletePromptTitle'\)",
            uninstall)
        if not m_del:
            failures.append(
                "CHECK 2: no CleanupSharedEngineStore call in "
                "CurUninstallStepChanged")
        elif not m_prompt or m_prompt.start() > m_del.start():
            failures.append(
                "CHECK 2: CleanupSharedEngineStore is not preceded by the "
                "SharedEngineDeletePrompt SuppressibleMsgBox")
        else:
            guard = uninstall[m_prompt.start():m_del.start()]
            if not re.search(r"MB_YESNO,\s*IDNO\)\s*=\s*IDYES\s*then", guard,
                             re.DOTALL):
                failures.append(
                    "CHECK 2: the SharedEngine delete confirmation does not "
                    "require an explicit IDYES before CleanupSharedEngineStore")

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

    # -- CHECK 7: call-site visibility (P5 review #5) ----------------------
    # The detectors must not only EXIST but be WIRED into the uninstall flow:
    # deleting the else-if branch or the probe call entirely must trip this
    # gate even though CHECKs 1-4 still pass.
    if uninstall:
        if not re.search(r"IsEmebalaProcessRunning\(\)", uninstall):
            failures.append(
                "CHECK 7: IsEmebalaProcessRunning() is never called inside "
                "CurUninstallStepChanged (unwired detector)")
        kept_notices = len(re.findall(
            r"(?:Suppressible)?MsgBox\(\s*CustomMessage\('SharedEngineKeptInUseTitle'\)",
            uninstall))
        if kept_notices < 2:
            failures.append(
                f"CHECK 7: expected a kept-notice box in BOTH preserve branches, "
                f"found {kept_notices}")

    # -- CHECK 8: M7 A-3 registry-aware cleanup invariants -------------------
    cleanup = extract_procedure(text, "CleanupSharedEngineStore")
    if not cleanup:
        failures.append("CHECK 8: CleanupSharedEngineStore is missing")
    else:
        if "not a strict v1 document" not in text or \
                "ENTIRE shared store is preserved" not in cleanup:
            failures.append(
                "CHECK 8: CleanupSharedEngineStore lost the fail-closed "
                "preserve-everything path for a damaged/unreadable registry")
        if "DelTree" in cleanup:
            failures.append(
                "CHECK 8: CleanupSharedEngineStore uses DelTree — only "
                "targeted DeleteFile of owned/bundled paths is allowed")
        if "Origin = 'bundled'" not in cleanup:
            failures.append(
                "CHECK 8: cleanup no longer gates model deletion on "
                "origin == 'bundled' (user models must survive)")
        if "IsSafeBareName" not in text or \
                "not a safe bare name" not in text:
            failures.append(
                "CHECK 8: the bare-name tampering guard (IsSafeBareName) is "
                "gone from the cleanup path")

    # -- CHECK 9: REQ-L32 P2-2/P2-3 asr shared-slot contract -----------------
    # The Listener-owned ggml-asr pair must be (a) staged as a NON-owner slot
    # (first-install only, NEVER replace existing — G3 downgrade guard), and
    # (b) enumerated in the M7 A-3 owned-cleanup list so a last-app Chat
    # uninstall does not strand it as an orphan (KNOWN-GAP #1 close).
    if "WORKER_ASR_FILENAME = 'Emebala.Engine.ggml-asr.exe'" not in text:
        failures.append(
            "CHECK 9: WORKER_ASR_FILENAME constant missing "
            "(REQ-L32 P2-2 asr staging)")
    if "WORKER_MANIFEST_ASR_FILENAME = 'worker.ggml-asr.manifest'" not in text:
        failures.append(
            "CHECK 9: WORKER_MANIFEST_ASR_FILENAME constant missing "
            "(REQ-L32 P2-2 asr staging)")
    if "function ShouldInstallEngineWorkerAsr" not in text:
        failures.append(
            "CHECK 9: ShouldInstallEngineWorkerAsr() missing (REQ-L32 P2-2)")
    if cleanup:
        if "DeleteOwnedFile(EngineDir + '\\' + WORKER_ASR_FILENAME)" not in cleanup:
            failures.append(
                "CHECK 9: CleanupSharedEngineStore does not delete "
                "WORKER_ASR_FILENAME (P2-3 orphan close)")
        if "DeleteOwnedFile(EngineDir + '\\' + WORKER_MANIFEST_ASR_FILENAME)" not in cleanup:
            failures.append(
                "CHECK 9: CleanupSharedEngineStore does not delete "
                "WORKER_MANIFEST_ASR_FILENAME (P2-3 orphan close)")
    if "function SharedSlotReplaceDecision" not in text or \
            "NEVER replace (G3)" not in text:
        failures.append(
            "CHECK 9: SharedSlotReplaceDecision G3 guard missing "
            "(REQ-L32 P2-2 downgrade guard)")

    # -- CHECK 10: M7 A-7 family-shared setup-gate mutex wiring ----------------
    # The [Code] gate must exist on BOTH entry points and use the exact
    # family mutex name; the elevated chained child must be exempt so the
    # master's own mutex never aborts the real install. (Structural only —
    # runtime behavior is the physical-install domain.)
    if "FAMILY_SETUP_MUTEX = 'Local\\EmebalaSetup'" not in text:
        failures.append("CHECK 10: family setup mutex constant is not the "
                        "exact Local\\EmebalaSetup")
    gate = extract_procedure(text, "FamilySetupGateAcquire")
    if not gate:
        failures.append("CHECK 10: FamilySetupGateAcquire() is missing")
    else:
        if "CheckForMutexes(FAMILY_SETUP_MUTEX)" not in gate:
            failures.append("CHECK 10: gate does not check the shared mutex")
        if "CreateMutex(FAMILY_SETUP_MUTEX)" not in gate:
            failures.append("CHECK 10: gate does not acquire the shared mutex")
        if "{param:SL5}" not in gate:
            failures.append("CHECK 10: gate lost the chained-mode (SL5) "
                            "elevated-child exemption")
    for ev in ("InitializeSetup", "InitializeUninstall"):
        fn = extract_procedure(text, ev)
        if not fn or "FamilySetupGateAcquire" not in fn:
            failures.append(
                f"CHECK 10: {ev}() is missing or not wired to the gate")

    # -- CHECK 9: M7 A-2 installer-side registry merge invariants ------------
    writer = extract_procedure(text, "WriteRegistryFile")
    if not writer:
        failures.append("CHECK 9: WriteRegistryFile is missing")
    else:
        if "leaving it untouched" not in writer:
            failures.append(
                "CHECK 9: WriteRegistryFile lost the fail-closed preserve "
                "path for a damaged/non-strict registry.json")
        if "REGISTRY_BUNDLED_ID" not in writer or "'bundled'" not in writer:
            failures.append(
                "CHECK 9: WriteRegistryFile no longer merges the single "
                "owned bundled slot (origin-preservation rule)")
        if "WriteTextFileAtomic" not in writer:
            failures.append(
                "CHECK 9: WriteRegistryFile no longer persists through the "
                "atomic tmp+rename writer (multi-writer torn-read protection)")
        # A-2 must not regress to the pre-A-2 create-if-missing model:
        if re.search(r"already exists - leaving it untouched \(idempotent",
                     writer):
            failures.append(
                "CHECK 9: the pre-A-2 create-if-missing early-exit is back; "
                "existing registries must be MERGED, not skipped")
    if "function WriteTextFileAtomic" not in text:
        failures.append("CHECK 9: WriteTextFileAtomic (atomic writer helper) "
                        "is missing")

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
