#!/usr/bin/env python3
"""R2 pre-flight sibling-process isolation gate (REQ-B007, Plan-B-minimum B-T7).

Authority: docs/260928_0001_session_translation-fail-fix/
    234900_architect-planB-design.md   §7.3 (R2) + §11 B-T7
    235700_architect-planB-design-v2-amendments.md  A4 (EMEBALA_E2E_FORCE=1
        semantics — the gate MUST honor the env var and skip its check)
    091500_debug-p3-technical-gate-planb.md  amendment A-7 (the env var does
        NOT exist anywhere today — implemented here from scratch).

The Chat E2E harnesses (req027_e2e.py, req051_drag_e2e.py) spawn the real
Emebala_chat.exe GUI.  Spawning it while the sibling app or the shared engine
host is already running causes concurrent access to the shared engine pipes /
Common store — exactly the window-flicker class of defect the user observed
while Listener tests were active.  This module is the ONE shared helper both
harnesses call from AppSession.start() BEFORE any subprocess.Popen.

Contract (enforced by tests/test_isolation_gate.py):
    * EMEBALA_E2E_FORCE=1 in the environment -> the gate is SKIPPED (the
      documented, explicit opt-in escape hatch; a one-line note is printed
      so transcripts show the bypass was honored — A4).
    * Otherwise, for each sibling image in SIBLING_IMAGES, the caller-supplied
      list_processes_like(base) probe is consulted.  Any hit -> IsolationGateError
      with a clear, actionable message pointing at tools/e2e/README.md.
    * No sibling running -> returns None; the caller proceeds to spawn.

The probe callable is INJECTED (not imported) so the gate is unit-testable
with a stubbed list_processes_like — no real process is needed.
"""

from __future__ import annotations

import os
from typing import Callable, Iterable, Optional, Set

#: Escape-hatch environment variable (design §7.3 + v2 amendment A4).  Any
#: non-empty value other than "0" is treated as set ("1" is the documented
#: spelling).
FORCE_ENV_VAR = "EMEBALA_E2E_FORCE"

#: Sibling images that must NOT be running when a Chat E2E harness spawns the
#: app (design §7.3 R2).  Probe bases are given WITHOUT the .exe suffix —
#: list_processes_like() matches base, base + ".exe", and base + "-*"
#: (Store-helper) spellings.
SIBLING_IMAGES = ("EmebalaListener", "Emebala.Engine")

#: Doc pointer appended to the abort message (kept as a literal the README
#: path is part of the actionable contract).
_README_REF = "tools/e2e/README.md"


class IsolationGateError(RuntimeError):
    """ISOLATION GATE abort — a sibling app/engine process is live.

    Subclasses RuntimeError so existing harness error plumbing that catches
    RuntimeError (or the harnesses' own EnvBlock/HarnessError wrappers around
    a RuntimeError message) keeps working unchanged.
    """


def _force_honored(environ: Optional[dict]) -> bool:
    """True when the EMEBALA_E2E_FORCE escape hatch is set (A4 semantics).

    Reads `environ` when injected (tests), else the real process env.
    The documented spelling is "1"; any non-empty, non-"0" value is honored
    so that FORCE=yes / FORCE=true also work, matching how harnesses treat
    other boolean env flags.
    """
    env = os.environ if environ is None else environ
    val = env.get(FORCE_ENV_VAR, "")
    return bool(val) and val != "0"


def check_isolation_gate(
    list_processes_like: Callable[[str], Set[int]],
    environ: Optional[dict] = None,
    out=print,
) -> None:
    """Run the R2 sibling-process gate; raise IsolationGateError on a hit.

    Parameters
    ----------
    list_processes_like:
        The harness's existing PID probe (tasklist-based).  Injected so unit
        tests can stub it — never imported here.
    environ:
        Optional environment dict (tests inject {"EMEBALA_E2E_FORCE": "1"}).
        None -> the real process environment.
    out:
        One-line sink for the FORCE-honored note (A4: "log that it was
        honored").  Defaults to builtins.print; tests capture via a list.

    Raises
    ------
    IsolationGateError
        With the design's actionable message when any sibling image is live
        and the escape hatch is NOT set.
    """
    if _force_honored(environ):
        out(
            "ISOLATION GATE: EMEBALA_E2E_FORCE=1 honored — sibling-process "
            "check skipped (documented escape hatch; see tools/e2e/README.md)."
        )
        return

    for image in SIBLING_IMAGES:
        pids = list_processes_like(image)
        if pids:
            raise IsolationGateError(
                f"ISOLATION GATE: {image}.exe is running (PIDs "
                f"{sorted(pids)}). Stop the Listener app / engine host "
                f"before running this E2E harness — concurrent access to "
                f"the shared engine pipes / Common store corrupts both "
                f"apps' sessions. See {_README_REF} for the isolation "
                f"contract and the EMEBALA_E2E_FORCE=1 escape hatch."
            )
