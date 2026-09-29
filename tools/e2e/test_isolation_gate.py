#!/usr/bin/env python3
"""Unit test for the R2 sibling-process isolation gate (REQ-B007, B-T7).

Repo convention note: the Chat repo has NO pytest/unittest suite anywhere in
tools/ or tests/ (verified by search at authoring time) — every Python file
under tools/ is a self-contained stdlib-only script.  This file follows that
convention: a lightweight assert-based script runnable with EITHER

    python tools/e2e/test_isolation_gate.py          (exit 0 = pass)
    python -m pytest tools/e2e/test_isolation_gate.py (pytest auto-discovers
                                                       the test_* functions)

The gate function is imported (not subprocessed) and its `list_processes_like`
probe + environment are INJECTED, so no real Listener/engine process is needed
and the test is hermetic (A-7 delegation: stubs, not live apps).
"""

from __future__ import annotations

import os
import sys

# Make the import work both as `python test_isolation_gate.py` (cwd anywhere)
# and as `python -m pytest tools/e2e/test_isolation_gate.py`.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from isolation_gate import (  # noqa: E402
    FORCE_ENV_VAR,
    IsolationGateError,
    SIBLING_IMAGES,
    check_isolation_gate,
)


def _stub(present_images):
    """Return a list_processes_like stub that reports `present_images` as live.

    The stub records every probe base it was asked about (call-sequence spy).
    """
    probed = []

    def fake(base):
        probed.append(base)
        # PID set: non-empty means "running".
        return {1234} if base in present_images else set()

    fake.probed = probed
    return fake


def test_sibling_present_raises():
    """Listener/engine present + no FORCE -> IsolationGateError, message actionable."""
    for image in SIBLING_IMAGES:
        stub = _stub(present_images={image})
        try:
            check_isolation_gate(stub, environ={}, out=lambda _m: None)
        except IsolationGateError as exc:
            msg = str(exc)
            assert "ISOLATION GATE" in msg, msg
            assert f"{image}.exe" in msg, msg
            assert "tools/e2e/README.md" in msg, msg
            assert "EMEBALA_E2E_FORCE=1" in msg, msg
        else:
            raise AssertionError(
                f"expected IsolationGateError when {image}.exe is live")
        # The gate probed at least the one sibling that is live.
        assert image in stub.probed, stub.probed


def test_no_sibling_passes():
    """No sibling live + no FORCE -> returns None; every sibling probed once."""
    stub = _stub(present_images=set())
    notes = []
    result = check_isolation_gate(stub, environ={}, out=notes.append)
    assert result is None
    assert sorted(stub.probed) == sorted(SIBLING_IMAGES), stub.probed
    # No FORCE honored-note when the hatch is not set.
    assert notes == [], notes


def test_force_skips_gate():
    """FORCE=1 + sibling present -> passes; honored-note logged (A4)."""
    stub = _stub(present_images=set(SIBLING_IMAGES))  # EVERYTHING live
    notes = []
    result = check_isolation_gate(
        stub, environ={FORCE_ENV_VAR: "1"}, out=notes.append)
    assert result is None
    # The documented A4 contract: the bypass is LOGGED, and the probe is
    # still executed (we do NOT trust the env var alone to mean "no probe" —
    # the note is emitted regardless of probe outcome).
    assert len(notes) == 1 and "EMEBALA_E2E_FORCE=1 honored" in notes[0], notes
    assert "skipped" in notes[0], notes


def _main() -> int:
    tests = [
        test_sibling_present_raises,
        test_no_sibling_passes,
        test_force_skips_gate,
    ]
    failures = 0
    for t in tests:
        name = t.__name__
        try:
            t()
        except AssertionError as exc:
            print(f"FAIL: {name}: {exc}")
            failures += 1
        else:
            print(f"ok:   {name}")
    if failures:
        print(f"{failures}/{len(tests)} gate unit test(s) FAILED")
        return 1
    print(f"all {len(tests)} isolation-gate unit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
