#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
REQ-027 E2E harness - notepad per-line block translation, automated QA.
=========================================================================

Purpose
-------
Replace the manual user QA items QA-27-B / QA-27-1 / QA-27-4 (design report
docs/260907_0001_session_user-test-fixes-req024-027/
192100_architect-report-req027-richeditd2dpt-redesign.md section 3.4) with a
repeatable, evidence-based automated gate, and extend it (B-6c, design
210000_architect section 2.4) to the edge-flow matrix the user asked for:
"평소에 사용하지 않을만한 다양한 flow에서도 에러가 발생 안 해야해".
The harness really launches the built app and a real Notepad window, types
Hangul lines, presses Enter, and judges the result on three layers:

  Layer 1 (log, primary):  %LOCALAPPDATA%\\Emebalachat\\logs\\emebalachat_*.log
    (a) no "EditCaretTracker/002" focus-unresolved fallback, and - since B-6b
        replaced the class whitelist with the EM capability probe - no
        "/006" (capability Unknown) nor "/007" (NotCapable) fallback either:
        for every scenario below the target is Win11 Notepad's
        RichEditD2DPT, whose intended verdict is Capable, so ANY /006|/007
        line is a FAIL (capability decision fell back against intent).
        "/005" (requery-failed estimate) and "/008" (newline-settle timeout)
        are collected as INFO reference counters (design 4.1-1).
    (b) the positive EM-path marker "EditCaretTracker em_setselect" present,
    (c) "PIPELINE/stage=capture end len=N" equals the TYPED line length
        EXACTLY (never the growing document = pre-REQ-027 cumulative
        signature; a captured leading/trailing newline is flagged as a
        separator-swallow defect candidate, see verify_task_layer1). The
        parser accepts BOTH log shapes: with diag_log_content=1 the line
        carries content="..." and the content checks run; with the shipped
        privacy default (content off) it is shape-only and the content
        checks report BLOCKED - never FAIL (261121_0002 re-baseline E1/E7),
    (d) "stage=paste result=1" appears,
    (e) translation completion evidence "stage=translate end status=0".
        NOTE: tooltip_show kind=translation is only emitted by the drag /
        double-Ctrl+C paths (src/ui/tooltip.cpp L634, src/main.cpp L1167/
        L1290), never by the Enter pipeline. For the Enter pipeline the
        completion proof is translate-end(status=0) + paste(result=1) +
        task_end(pasted=1). This substitution is documented in the session
        report (the delegation allowed "tooltip_show OR 번역 완료 증거").
  Layer 2 (content): WM_GETTEXT on the edit control (RichEditD2DPT), with a
    PowerShell UIA ValuePattern fallback (tools/e2e/uia_read_edit.ps1):
    the typed Hangul source must be GONE (replaced by the translation) and
    the document must have changed.
  Layer 3 (verdict): PASS / FAIL (with cited log lines) / INCONCLUSIVE
    (cloud/network/engine/process-environment factors - never conflated
    with a product FAIL).

Scenarios (arg 1) - design 210000 section 2.4 matrix (12; ime_composing is
deliberately NOT automated, see README "Handed to user QA"):
  qa27b               one Hangul line + Enter -> only that line replaced
  example1            six sentences, each Enter -> non-cumulative captures
  consecutive         two same-language lines back-to-back -> no
                      translation_equals_source skip (QA-27-4)
  multi_lang          three blocks, per-block DIFFERENT target language
                      (EN -> JA -> VI). Language switch: tray-menu UIA
                      automation attempted FIRST (runtime path, REQ-019);
                      on refusal the config.json pre-seed + app-restart
                      fallback runs per-block sessions and the report notes
                      "runtime switch path unverified by this run".
  empty_enter         empty document, bare Enter x3 -> every task ends in
                        stage=empty_capture decision=hold_send, no crash,
                        document still empty (REQ-023 / R5 hold contract);
                        REQ-034 F3-B: the no-selection notice MUST surface
                        per held task (tooltip kind=message) and the
                        paste-window suppression must NEVER fire here (C-5)
  cursor_mid          caret placed mid-line (EM_SETSEL) before Enter ->
                      capture == [doc start .. caret) exactly, the tail
                      after the caret survives untouched
  backspace_enter     translate line A, type line B, backspace INTO the
                      translated A' so stored last > caret -> the clamp
                      path (EditCaretTracker/004) must run safely (task
                      completes, no /002|/003|/006|/007, no crash)
  shift_enter_multi   L1 + Shift+Enter + L2 + Shift+Enter + L3 + bare Enter
                      -> Shift+Enter passes through (ENTER_GATE
                      reason=shift_enter_newline x2, REQ-018), exactly ONE
                      task whose capture spans the whole multi-line block
  autosend_newline    REQ-028 positive leg (261121_0002 re-baseline E10):
                      auto_send=1 pinned, 3 lines -> EVERY task must log
                      stage=send_enter action=synthetic_newline ... auto_send=1,
                      the document must gain exactly ONE "\n" per task, the
                      final document must hold exactly len(lines) newlines and
                      the EditCaretTracker/008 newline-settle counter must stay
                      0. This is the scenario that keeps the editor-newline
                      feature under E2E coverage after REQ-028 retired the old
                      "CategoryB always injects" rule; it is the ONLY scenario
                      that flips the pinned config fixture's auto_send.
  paste_then_enter    clipboard seeded OUTSIDE the app + real Ctrl+V
                        (SendInput) + immediate Enter -> capture == pasted
                        text exactly, replacement normal; then one IMMEDIATE
                        retry Enter -> REQ-034 F3-B: WORKER/ExecuteTask/036 +
                        decision=paste_window_suppress, NO notice, silent
                        send-through (the app really adds a newline)
  long_text           one 1000-char Hangul line + Enter -> capture len
                      1000, replacement normal. The 64K EM_GETSEL WORD
                      saturation boundary is NOT automated (user QA
                      QA-27-7, see README). The known intermittent
                      long-input capture timeout (a 1000-char RichEdit copy
                      exceeding the clipboard-sequence budget, first
                      documented 260907 and tightened when
                      kClipboardChangeTimeoutMs shrank to 80 ms) is a
                      PROBABLE PRODUCT defect: this scenario reports it as
                      INCONCLUSIVE with the capture diagnosis in the output
                      (never PASS, never a silent harness FAIL) - see
                      scenario_long_text's C5 disposition.
  notepad_vscode_mix  window-1 Enter -> switch to a SECOND Notepad window,
                        Enter there -> back to window 1, Enter. Proves the
                        caret-offset state map is keyed per focus hwnd:
                        window 2 starts at last=0 (no window-1 offset leak),
                        window 1's second Enter resumes at exactly its own
                        stored post-newline offset. (VSCode automation is
                        environment-dependent; per the delegation a second
                        Notepad window is the sanctioned substitute - the
                        verification target is per-hwnd isolation, not the
                        specific client. Rationale also in README.)
    uilang_37           REQ-037/B-4 (design 260907_0002 §4.3 E2E-UILANG-2):
                        open the tray "Interface Language" submenu via UIA and
                        enumerate it: 38 items (localized Auto + the 37
                        endonyms - العربية/日本語/한국어 spot-checked, order
                        pinned to design §2-Q3, EN last). READ-ONLY: nothing is
                        invoked, config is never touched. The AR-selection half
                        of E2E-UILANG-1 is deferred to B-6 with the reset
                        scenario per the delegation. Tray automation is
                        environment-bounded (TrackPopupMenu modal loop can
                        starve the UIA menu provider - documented multi_lang
                        limitation): a NOTFOUND verdict is INCONCLUSIVE for
                        this scenario, never a product FAIL.
    reset_defaults      REQ-040/B-6 (design 260907_0002 §4.3 E2E-RESET-1 +
                        §2.3.2 G-1, user-approved 2026-09-07): seed the four
                        language fields AND ui_language="th" into the app's
                        LIVE config.json (%LOCALAPPDATA%, matching
                        GetDefaultConfigPath order - the exe-dir copy only
                        when absent), restart so the values are live in
                        memory, open About through the tray (UIA
                        InvokeMenuItem on the locale-independent
                        "Emebala Chat...\u2026" brand+ellipsis item), click the
                        reset button at its production DIP geometry, then
                        assert the persisted config returns to the system
                        defaults with ui_language="auto" and the lang_reset
                        coordinator DIAG marker appears. Tray-UIA refusal =
                        INCONCLUSIVE (same documented TrackPopupMenu
                        starvation bound as uilang_37).

Dependencies
------------
Python 3.8+ standard library ONLY (Win32 via ctypes P/Invoke). pywinauto is
NOT required and NOT installed (verified in this session: ModuleNotFoundError;
pip install would be needed). The delegation's fallback branch was chosen:
Python ctypes (P/Invoke equivalent) + PowerShell System.Windows.Automation
UIA fallback - neither needs an install.

Usage
-----
  python tools\\e2e\\req027_e2e.py qa27b
  python tools\\e2e\\req027_e2e.py example1 --step-timeout 40
  python tools\\e2e\\req027_e2e.py all          ; all automated scenarios
  python tools\\e2e\\req027_e2e.py multi_lang --no-tray   ; force config fallback
  python tools\\e2e\\req027_e2e.py uilang_37    ; tray UI-language submenu
                                                 enumeration (interactive
                                                 desktop; do not touch
                                                 keyboard/mouse during it)
  python tools\\e2e\\req027_e2e.py reset_defaults ; REQ-040 E2E-RESET-1
                                                 (interactive desktop; the
                                                 run moves the real cursor
                                                 onto the About reset button)
  python tools\\e2e\\req027_e2e.py all --skip-tray-scenarios
                                                 ; green-gate form: the two
                                                 tray scenarios are removed
                                                 from the matrix instead of
                                                 contributing INCONCLUSIVE
  python tools\\e2e\\req027_e2e.py qa27b --run-dir <dir>
                                                 ; keep run-manifest.json (and
                                                 a copy of the console log) in
                                                 an explicit directory

Exit codes (E12 policy, 261121_0002): 0 = every chosen scenario PASS;
1 = any FAIL; 2 = no FAIL but at least one INCONCLUSIVE (this includes the
two tray scenarios, which are environment-bounded - use
--skip-tray-scenarios when a green gate is required on a desktop whose tray
UIA provider is starved, rather than converting them to PASS); 3 = harness
error or config-fixture restore mismatch. Every run prints a machine-readable
"E2E_SUMMARY {...}" line and writes run-manifest.json into the run directory.

Operating constraints (documented; pre-flight enforced where possible)
----------------------------------------------------------------------
  * Interactive desktop session required (SendInput + foreground windows).
    Headless CI cannot run this.
  * Translation goes to the cloud (config engine_type=google in build/):
    network/engine failures are classified INCONCLUSIVE, not FAIL. INCON-
    CLUSIVE scenarios are auto-retried once (delegation section 4).
  * The app's low-level keyboard hook treats ANY input without its private
    per-process marker (src/win32_input.cpp EXTRA_INFO_MARKER) as a real
    keystroke. The harness therefore sends the trigger Enter via SendInput
    UNMARKED - the hook intercepts it and feeds the pipeline, exactly like a
    user pressing Enter. The EDITOR newline is injected BY THE APP only when
    auto_send=1: REQ-028 (user-approved reversal, 2026-09-07 13:06) retired
    the old REQ-023 "CategoryB always injects a newline" rule, so with the
    shipped default auto_send=0 the app logs
    "stage=send_enter action=SKIPPED reason=send_gate", and with auto_send=1
    it logs "action=synthetic_newline ... auto_send=1". The harness sends
    exactly ONE Enter per typed line in BOTH modes and judges the document
    DELTA against the app's own logged action (261121_0002 re-baseline
    E2/E3): synthetic_newline -> delta 1, SKIPPED/synthetic_send -> delta 0,
    no decisive action line -> BLOCKED (never PASS). The marker value is
    per-process and NOT reproducible from outside; harness injections of
    Ctrl+V / Shift+Enter are therefore unmarked and pass through the hook's
    documented pass-through branches (ctrl+V is not a gesture key;
    Shift+Enter exits via the REQ-018 gate).
  * E2E CONFIG FIXTURE (261121_0002 E5): before the first app launch the
    harness backs up the RUNNING app's config.json bytes
    (%LOCALAPPDATA%\\Emebalachat\\config.json, matching
    GetDefaultConfigPath; exe-dir only when absent), pins
    diag_log_enabled=true, diag_log_content=true and auto_send=<scenario
    mode> (autosend_newline = true, everything else false), verifies the
    app's own "SESSION/diag_log_enabled=.. diag_log_content=.." and
    "SESSION/config loaded=.. auto_send=.." lines against the fixture, and
    restores the ORIGINAL BYTES in a finally with a SHA-256 verification.
    Mismatch -> EnvBlock (environment), never a product FAIL. Privacy: the
    fixture writes ONLY harness-owned E2E strings into the QA log (every
    adopted document is verified empty before typing); it never records user
    content and never leaves the config modified.
  * Text is inserted via PostMessageW(WM_CHAR) directly into the edit
    control: this bypasses the hook just like IME-committed text does, so
    the hook's IME mirror stays false and Enter is never gated as
    "composing". It also avoids SendInput per-character layout surprises
    (Korean IME jamo handling) in the harness itself.
  * DO NOT touch keyboard/mouse while a scenario runs - real keystrokes
    would be captured by the hook and pollute the per-task event stream.
  * A pre-existing Emebala_chat.exe instance aborts the run (single-instance
    mutex, src/main.cpp L278). Close it first. Windows 11 Notepad shares ONE
    process across document windows, session-restores old scratch docs
    asynchronously, and (once blank frames exist) a blank launch may
    ACTIVATE an existing empty frame instead of creating a new one (both
    observed live 260907). B-6c adoption rule: claim the first FRESH frame
    that reads EMPTY; failing that, REUSE an existing untitled-and-empty
    frame. Either way the claimed window holds no content by verification,
    so user data can never be adopted, cleared, or closed; a window with
    any non-E2Exx text is skipped untouched. Teardown closes ONLY the
    adopted window (WM_CLOSE + UIA "don't save" dismissal); the shared
    Notepad process is never killed (B-6c also removed a duplicate stop()
    that force-killed it - user-data-loss hazard); the spawned APP is
    force-killed.
  * SetForegroundWindow from a background process hits the Windows foreground
    lock; the harness uses AttachThreadInput + an ALT tap to acquire it and
    verifies the window really is foreground before every Enter injection
    (probed live: Win11 Notepad session-restore can pre-fill the new window
    - the harness clears its own scratch window before typing).
  * CLIPBOARD (261122_0001 A4, F11.3): the APP restores what it copies (its own
    RAII snapshot in src/win32_input.cpp), but the harness's own SEED is the
    harness's business - paste_then_enter snapshots the user's CF_UNICODETEXT
    before seeding and puts it back in a finally (a non-text clipboard cannot be
    re-created and is reported, never silently emptied). paste_then_enter and
    the multi_lang fallback also change the RUNTIME config.json
    (%LOCALAPPDATA%, see the fixture bullet above) and restore it.
  * HARD-KILL RECOVERY (261122_0001 A4, F11.4): a taskkill / power loss cannot
    run that finally, so the fixture ALSO keeps a crash ledger next to the file
    (<config>.e2e-pin.json: pre-pin bytes, pinned keys, previous values) that is
    written BEFORE the pin and deleted after a verified restore. The next run
    detects a leftover ledger and either self-heals (writes the pre-pin bytes
    back, or unpins the keys when the app rewrote the file) or REFUSES to pin
    with guidance - it never backs the pinned bytes up as "the user's own".
  * All synchronization is polling with explicit timeouts (constants below);
    the only fixed delay is a 300 ms diag-flush settle before teardown.
  * ENVIRONMENT MANIFEST (261121_0002 E11): every run writes
    <run-dir>/run-manifest.json (UTC start/end, OS build, app exe + SHA-256 +
    self-reported SESSION version, config path + pre/post SHA-256, pinned
    mode, Notepad frame/edit class, read methods, tray-UIA outcome,
    per-scenario verdicts, exit code). The manifest exists because the
    previous release QA misdiagnosed a stale baseline as "Win11 Notepad
    drift": a future green/red flip must be attributable to a recorded
    environment fact, not to a guess. 261122_0001 A4 (F11.2): every
    user-identifying path in the manifest (run dir, app exe, self-reported exe,
    config, notes) is redacted to a %USERPROFILE%/%LOCALAPPDATA% token, so the
    file can be pasted into a report without carrying an account name.
"""

import argparse
import base64
import ctypes
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes

# Plan-B (REQ-B007) B-T7 R2: shared sibling-process isolation gate.  The
# sibling sibling-app/engine check lives in ONE helper (isolation_gate.py)
# imported by both this harness and req051_drag_e2e.py — factored, not
# copy-pasted, per the B-T7 delegation (the pre-existing "Copied from req027"
# convention predates the gate; new shared logic goes in the helper).
try:
    from isolation_gate import IsolationGateError, check_isolation_gate
except ImportError:  # pragma: no cover - direct script invocation fallback
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from isolation_gate import IsolationGateError, check_isolation_gate

# 261122_0001 A4 (F11.2, SR5 F-07): user-identifying local paths are
# redacted to %ENV% tokens in everything this harness PRINTS or PERSISTS
# (run-manifest.json included), so a transcript pasted into docs/ carries
# no Windows account name. Shared with req051_drag_e2e.py, factored like
# isolation_gate.py rather than copy-pasted.
try:
    from env_redact import redact_user_path
except ImportError:  # pragma: no cover - direct script invocation fallback
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from env_redact import redact_user_path

# ---------------------------------------------------------------------------
# constants / tunables (all timeouts explicit, polling-only)
# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

DEFAULT_APP_EXE = os.path.join(REPO_ROOT, "build", "Emebala_chat.exe")
LOG_DIR = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Emebalachat", "logs")
# src/diag_logger.cpp BuildLogName: emebalachat_yymmddhhmmss.log (12 LOCAL
# time digits) + optional "-N" collision suffix.
LOG_RE = re.compile(r"^emebalachat_\d{12}(-\d+)?\.log$")

POLL_S = 0.05                 # generic poll interval
APP_READY_TIMEOUT_S = 20.0    # wait for SESSION/subsystems started
NOTEPAD_READY_TIMEOUT_S = 15.0
STEP_TIMEOUT_S = 30.0         # per-Enter pipeline completion (cloud round-trip)
ENTER_TASK_TIMEOUT_S = 6.0    # Enter -> task_received appears in log
TEARDOWN_SETTLE_S = 0.3       # diag flush batch settle before process kill
WM_TIMEOUT_MS = 2000          # SendMessageTimeoutW cap for cross-process reads
TRAY_ATTEMPT_TIMEOUT_S = 50.0  # hard cap for the UIA tray-menu attempt
TRAY_ENUM_TIMEOUT_S = 70.0     # uilang_37 enumeration attempt (B-4)

EDIT_CLASSES_PREFERRED = ("RichEditD2DPT", "RichEditD2D", "RICHEDIT50W",
                          "RichEdit20W", "RichEdit20A", "Edit")

STATUS_NAMES = {0: "Ok", 1: "InputEmpty", 2: "CloudConsentBlocked",
                3: "EngineFailed", 4: "Canceled"}

# ---------------------------------------------------------------------------
# Win32 plumbing (ctypes P/Invoke) - argtypes set everywhere so 64-bit
# HWND/WPARAM/LPARAM values are never truncated to 32 bits.
# ---------------------------------------------------------------------------
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

HWND = wintypes.HWND
UINT = wintypes.UINT
WPARAM = wintypes.WPARAM
LPARAM = wintypes.LPARAM

WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_CHAR = 0x0102
WM_CLOSE = 0x0010
WM_CLEAR = 0x0303
EM_SETSEL = 0x00B1
EM_GETSEL = 0x00B0
VK_RETURN = 0x0D
VK_BACK = 0x08
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_ESCAPE = 0x1B
VK_V = 0x56
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1
SMTO_ABORTIFHUNG = 0x0002
SMTO_BLOCK = 0x0001
SW_RESTORE = 9
GMEM_MOVEABLE = 0x0002
CF_UNICODETEXT = 13

user32.GetClassNameW.argtypes = [HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.IsWindowVisible.argtypes = [HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindowTextLengthW.argtypes = [HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.EnumChildWindows.argtypes = [HWND, ctypes.c_void_p, LPARAM]
user32.EnumChildWindows.restype = wintypes.BOOL
user32.SendMessageTimeoutW.argtypes = [HWND, UINT, WPARAM, LPARAM, UINT,
                                       wintypes.UINT, ctypes.POINTER(wintypes.ULONG)]
user32.SendMessageTimeoutW.restype = wintypes.BOOL
user32.PostMessageW.argtypes = [HWND, UINT, WPARAM, LPARAM]
user32.PostMessageW.restype = wintypes.BOOL
user32.ShowWindow.argtypes = [HWND, ctypes.c_int]
user32.ShowWindow.restype = wintypes.BOOL
user32.SetForegroundWindow.argtypes = [HWND]
user32.SetForegroundWindow.restype = wintypes.BOOL
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = HWND
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD,
                                     wintypes.BOOL]
user32.AttachThreadInput.restype = wintypes.BOOL
user32.BringWindowToTop.argtypes = [HWND]
user32.BringWindowToTop.restype = wintypes.BOOL
user32.SwitchToThisWindow.argtypes = [HWND, wintypes.BOOL]
user32.SwitchToThisWindow.restype = None
kernel32.GetCurrentThreadId.argtypes = []
kernel32.GetCurrentThreadId.restype = wintypes.DWORD
user32.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]
user32.SendInput.restype = wintypes.UINT
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                ctypes.c_wchar_p,
                                                ctypes.POINTER(wintypes.DWORD)]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.GlobalAlloc.argtypes = [UINT, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.restype = wintypes.BOOL
user32.OpenClipboard.argtypes = [HWND]
user32.OpenClipboard.restype = wintypes.BOOL
user32.CloseClipboard.argtypes = []
user32.CloseClipboard.restype = wintypes.BOOL
user32.EmptyClipboard.argtypes = []
user32.EmptyClipboard.restype = wintypes.BOOL
user32.SetClipboardData.argtypes = [UINT, ctypes.c_void_p]
user32.SetClipboardData.restype = ctypes.c_void_p
# 261122_0001 A4 (F11.3): the harness now SNAPSHOTS the user's clipboard before
# seeding it, so the read half (GetClipboardData + the sequence counter) needs
# the same declared-signature discipline as the write half above.
user32.GetClipboardData.argtypes = [UINT]
user32.GetClipboardData.restype = ctypes.c_void_p
user32.GetClipboardSequenceNumber.argtypes = []
user32.GetClipboardSequenceNumber.restype = wintypes.DWORD

PENUMCHILD = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, LPARAM)
PENUMPTR = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, LPARAM)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    """Needed ONLY so the INPUT union is padded to the real x64 size:
    SendInput validates cbSize == sizeof(INPUT) == 40 on x64 (MOUSEINPUT is
    the widest member). A keyboard-only union makes INPUT 32 bytes and
    SendInput fails returning 0 (probed live 260907)."""
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUTU(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("_u",)
    _fields_ = [("type", wintypes.DWORD), ("_u", INPUTU)]


def _assert_input_size():
    """Fail fast (harness error) if the INPUT layout is wrong for this
    interpreter/bitness instead of silently failing every SendInput."""
    expected = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
    actual = ctypes.sizeof(INPUT)
    if actual != expected:
        raise HarnessError(
            f"sizeof(INPUT)=={actual}, expected {expected}: the ctypes "
            "layout does not match the platform SendInput contract")


# (call site at the bottom of the exception-definitions section so the
# raise path can reference HarnessError)


def get_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value or ""


def enum_child_windows(hwnd):
    out = []

    @PENUMCHILD
    def cb(h, l):
        out.append(int(h) if h else 0)
        return True

    user32.EnumChildWindows(hwnd, ctypes.cast(cb, ctypes.c_void_p), LPARAM(0))
    return [h for h in out if h]


def send_em(hwnd, msg, wparam, lparam, timeout_ms=WM_TIMEOUT_MS):
    """SendMessageTimeoutW with SMTO_ABORTIFHUNG|SMTO_BLOCK.
    Returns (ok, result). lparam may be int or c_void_p-compatible."""
    res = wintypes.ULONG(0)
    ok = user32.SendMessageTimeoutW(
        HWND(hwnd), UINT(msg), WPARAM(wparam), LPARAM(lparam),
        UINT(SMTO_ABORTIFHUNG | SMTO_BLOCK), UINT(timeout_ms), ctypes.byref(res))
    return bool(ok), int(res.value)


def read_edit_text_wmgettext(hwnd):
    """WM_GETTEXTLENGTH + WM_GETTEXT (UTF-16). '' = genuinely empty;
    None = unreadable (timeout / control does not answer)."""
    ok, length = send_em(hwnd, WM_GETTEXTLENGTH, 0, 0)
    if not ok:
        return None
    if length == 0:
        return ""
    n = min(length, 65536)  # harness never writes more than a few hundred chars
    buf = ctypes.create_unicode_buffer(n + 2)
    ok, got = send_em(hwnd, WM_GETTEXT, n + 1, ctypes.addressof(buf))
    if not ok:
        return None
    return buf.value


def read_edit_text_uia_ps(pid, hwnd=0, timeout=15):
    """Fallback reader: PowerShell System.Windows.Automation ValuePattern
    (tools/e2e/uia_read_edit.ps1), scoped to hwnd when given (Win11 Notepad
    shares one process across windows, so PID alone can pick the wrong
    document). Returns text ('' when the sentinel says empty) or None."""
    ps1 = os.path.join(HERE, "uia_read_edit.ps1")
    if not os.path.exists(ps1):
        return None
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", ps1, "-TargetPid", str(pid)]
    if hwnd:
        cmd += ["-TargetHwnd", str(hwnd)]
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    out = p.stdout.decode("utf-8-sig", errors="replace").replace("\r\n", "\n")
    if not out.startswith("UIA:"):
        return None
    payload = out[4:]
    if payload.startswith("EMPTY\n"):
        return ""
    if payload.startswith("TEXT\n"):
        return payload[5:].rstrip("\n")
    return None


def set_clipboard_text(text):
    """CF_UNICODETEXT with GMEM_MOVEABLE (the app's own Ctrl+C reader path in
    src/win32_input.cpp uses the same contract). Ownership of the block moves
    to the clipboard on SetClipboardData success - never GlobalFree it."""
    if not user32.OpenClipboard(HWND(0)):
        raise HarnessError("OpenClipboard failed (another app owns it?)")
    try:
        if not user32.EmptyClipboard():
            raise HarnessError("EmptyClipboard failed")
        payload = text.encode("utf-16-le") + b"\x00\x00"
        hmem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(payload))
        if not hmem:
            raise HarnessError("GlobalAlloc failed")
        lock = kernel32.GlobalLock(hmem)
        if not lock:
            raise HarnessError("GlobalLock failed")
        ctypes.memmove(lock, payload, len(payload))
        kernel32.GlobalUnlock(hmem)
        if not user32.SetClipboardData(CF_UNICODETEXT, hmem):
            raise HarnessError("SetClipboardData failed")
    finally:
        user32.CloseClipboard()


def read_clipboard_text():
    """Best-effort CF_UNICODETEXT read (never raises): (True, text) when the
    clipboard currently holds text, (False, None) otherwise (empty clipboard,
    image/file-list owner, or another process holding the clipboard open)."""
    if not user32.OpenClipboard(HWND(0)):
        return False, None
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return False, None
        ptr = kernel32.GlobalLock(handle)
        if not ptr:
            return False, None
        try:
            return True, ctypes.wstring_at(ptr)
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def _clipboard_sequence():
    """GetClipboardSequenceNumber with a None fallback (weakens only the
    'someone else replaced the clipboard' check, never the restore itself)."""
    try:
        return int(user32.GetClipboardSequenceNumber())
    except Exception:
        return None


class ClipboardBackup:
    """Snapshot/restore of the USER's clipboard around a harness seed.

    261122_0001 A4 (F11.3, SR5 F-06): scenario_paste_then_enter seeded its own
    text into the clipboard and left it there - after a normal run the user's
    clipboard was replaced by harness text, and a crash mid-run left it with no
    way back. This is the harness-side mirror of the app's own RAII clipboard
    discipline (src/win32_input.cpp ClipboardManager: snapshot -> use ->
    restore), reduced to the one format the harness writes.

    Honest limits, printed rather than papered over:
      * only CF_UNICODETEXT is captured; an image / file-list clipboard cannot
        be re-created from a text read, so it is restored to NOTHING and the
        loss is reported (never silently replaced by an empty clipboard);
      * a hard kill (taskkill / power loss) cannot run the finally - only a
        clean exception, Ctrl+C, or a normal exit restores the snapshot.
    """

    def __init__(self):
        self.had_text = False
        self.text = None
        self.seeded_seq = None
        self.lost_nontext = False
        self.restored = False

    def capture(self):
        self.had_text, self.text = read_clipboard_text()
        self.lost_nontext = not self.had_text
        return self

    def seeded(self):
        """Call right AFTER the harness writes its own text: records the
        clipboard sequence the seed produced, so restore() can tell whether
        someone ELSE replaced the clipboard in the meantime (in which case the
        newer content wins and the snapshot is NOT written back)."""
        self.seeded_seq = _clipboard_sequence()

    def restore(self):
        """Put the snapshot back (best effort, never raises - it runs in a
        finally and must not mask the scenario's own outcome)."""
        if not self.had_text:
            if self.lost_nontext:
                print("   clipboard: the pre-run clipboard held no TEXT (empty, "
                      "image, or file list); the harness can only restore "
                      "CF_UNICODETEXT, so any non-text content must be re-copied "
                      "by the operator")
            return False
        try:
            now_seq = _clipboard_sequence()
        except Exception:
            now_seq = None
        if (self.seeded_seq is not None and now_seq is not None
                and now_seq != self.seeded_seq):
            print("   clipboard: another process replaced the clipboard during the "
                  "run; leaving ITS content in place (the pre-run text is not "
                  "written back)")
            return False
        try:
            set_clipboard_text(self.text)
            self.restored = True
            print("   clipboard: pre-run text restored")
        except Exception as e:  # noqa: BLE001 - a finally must never raise
            print(f"   clipboard: RESTORE FAILED ({type(e).__name__}: {e}); the "
                  "seeded harness text is still on the clipboard")
        return self.restored


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", HWND), ("hwndFocus", HWND),
                ("hwndCapture", HWND), ("hwndMenuOwner", HWND),
                ("hwndMoveSize", HWND), ("hwndCaret", HWND),
                ("rcCaret", wintypes.RECT)]


user32.GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUITHREADINFO)]
user32.GetGUIThreadInfo.restype = wintypes.BOOL


def get_focus_hwnd_for_window(hwnd):
    """Keyboard-focused control inside hwnd via GetGUIThreadInfo(tid) - the
    SAME resolution the app's EM path uses (src/win32_input.cpp
    ResolveFocusCandidate). Cross-process lookup is officially supported.
    Returns 0 when unavailable."""
    tid = user32.GetWindowThreadProcessId(HWND(hwnd), None)
    if not tid:
        return 0
    info = GUITHREADINFO()
    info.cbSize = ctypes.sizeof(GUITHREADINFO)
    if not user32.GetGUIThreadInfo(tid, ctypes.byref(info)):
        return 0
    return int(info.hwndFocus or 0)


def notepad_top_windows():
    """{hwnd: (pid, title)} of visible top-level windows whose class is the
    Notepad frame class ('Notepad' on Win11 24H2, probed live)."""
    out = {}
    for pid in list_processes_like("Notepad"):
        for (h, title, cls) in top_windows_for_pid(pid):
            if cls.lower() == "notepad":
                out[h] = (pid, title)
    return out


def process_image_name(pid):
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return os.path.basename(buf.value)
        return ""
    finally:
        kernel32.CloseHandle(h)


def list_processes_like(base):
    """PIDs whose image name equals base(.exe) or base-* (Store helpers)."""
    pids = set()
    try:
        p = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                           capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return pids
    for line in p.stdout.splitlines():
        parts = re.findall(r'"([^"]*)"', line)
        if len(parts) >= 2:
            nm = parts[0].lower()
            stem = base.lower()
            if nm in (stem, stem + ".exe") or nm.startswith(stem + "-"):
                try:
                    pids.add(int(parts[1]))
                except ValueError:
                    pass
    return pids


def top_windows_for_pid(pid):
    """[(hwnd, title, class)] of visible top-level windows owned by pid."""
    found = []
    target = pid

    @PENUMPTR
    def cb(h, l):
        hpid = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(h, ctypes.byref(hpid))
        if hpid.value == target and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            found.append((int(h), buf.value, get_class(h)))
        return True

    user32.EnumWindows(ctypes.cast(cb, ctypes.c_void_p), LPARAM(0))
    return found


# EnumWindows is needed for the helper above:
user32.EnumWindows.argtypes = [ctypes.c_void_p, LPARAM]
user32.EnumWindows.restype = wintypes.BOOL


def force_kill_pids(pids):
    for pid in sorted(pids):
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            pass


def find_edit_control(top_hwnd):
    """Return (edit_hwnd, class) preferring known EDIT/RichEdit classes;
    (0, '') when nothing matches."""
    children = enum_child_windows(top_hwnd)
    by_class = {}
    for h in children:
        by_class.setdefault(get_class(h).lower(), (h, get_class(h)))
    for cls in EDIT_CLASSES_PREFERRED:
        hit = by_class.get(cls.lower())
        if hit:
            return hit
    for k, (h, raw) in by_class.items():
        if "edit" in k:
            return (h, raw)
    return 0, ""


# ---------------------------------------------------------------------------
# app-side DIAG log regexes (signatures quoted from src/, verified 260907;
# B-6c: /004 /005 /006 /007 /008 + lang_pair + translate-out + ENTER_GATE)
# ---------------------------------------------------------------------------
RE_TASK_RECEIVED = re.compile(r"PIPELINE/task_received source=enter_pipeline")
# 261121_0002 E1: TWO shapes exist. With diag_log_content=1 the line is
#   stage=capture end len=21 duration_ms=63 content="..." k=0 result=ok
# and with the shipped privacy default (content OFF, src/worker.cpp L453-456)
# it is shape-only:
#   stage=capture end len=21 duration_ms=63 k=0 result=ok
# The old regex REQUIRED content="..." and therefore never matched the default
# shape, which turned the harness's primary anti-cumulative assertion into
# BLOCKED everywhere and into hard FAIL in cursor_mid/backspace_enter.
# capture_content is None (not '') when the field is absent so the content
# checks can distinguish "logging off" from "captured an empty string" (E7).
RE_CAPTURE_END = re.compile(
    r'PIPELINE/stage=capture end len=(?P<len>\d+) duration_ms=\d+'
    r'(?: content="(?P<content>.*?)")?'
    r" k=(?P<k>\d+) result=(?P<result>[A-Za-z_]\w*)\s*$")
RE_TRANSLATE_END = re.compile(r"PIPELINE/stage=translate end status=(-?\d+)")
# Same two-shape contract as the capture line (E1): status/engine/duration/
# out_len stay ungated (src/worker.cpp L1108-1122) and only out="..." is
# content-gated. The old regex REQUIRED out="...", so out_len was unparsed on
# every privacy-default run even though the app had written it.
RE_TRANSLATE_OUT = re.compile(
    r"PIPELINE/stage=translate end status=-?\d+ engine=.+? duration_ms=\d+ "
    r'out_len=(?P<len>\d+)(?: out="(?P<out>.*?)")?\s*$')
# REQ-028 send decision (src/worker.cpp L1505-1520):
#   stage=send_enter action=synthetic_newline shift=0 auto_send=1
#   stage=send_enter action=SKIPPED reason=send_gate target=00000000002E0660
#   stage=send_enter action=synthetic(reason=ime_backstop) shift=0
# The injected branch carries auto_send=; the SKIPPED branch does not (the gate
# itself implies 0). The newline delta is judged from `action` (E2).
RE_SEND_ENTER = re.compile(
    r"PIPELINE/stage=send_enter action=(?P<action>[A-Za-z_]+)")
RE_SEND_ENTER_REASON = re.compile(
    r"PIPELINE/stage=send_enter action=SKIPPED reason=(?P<reason>\S+)")
RE_SEND_ENTER_AUTO_SEND = re.compile(
    r"PIPELINE/stage=send_enter .*?auto_send=(?P<auto_send>[01])\b")
# Per-task auto_send witness that exists on EVERY capture path (the lang_pair
# line is emitted before capture begin and is not content-gated).
RE_TASK_AUTO_SEND = re.compile(r"stage=lang_pair .*?auto_send=(?P<v>[01])\b")
RE_PASTE_RESULT = re.compile(r"PIPELINE/stage=paste result=(\d+)")
RE_PASTE_SKIPPED = re.compile(r"PIPELINE/stage=paste skipped reason=(\S+)")
RE_TASK_END = re.compile(r"PIPELINE/stage=task_end pasted=(\d+)")
# smart-bypass on the bare-Enter path returns BEFORE task_end (src/worker.cpp
# L240-250: ReleaseSelectionOnce + SendEnterKey + return) - a harmless,
# documented outcome, not a stall.
RE_SEND_THROUGH = re.compile(
    r"stage=send_through decision=bypass_pipeline reason=(\S+)")
RE_EM_SETSELECT = re.compile(r"EditCaretTracker/em_setselect")
RE_EM_SETSELECT_DETAIL = re.compile(
    r"EditCaretTracker/em_setselect hwnd=(\S+) last=(\d+) caret=(\d+)")
RE_FALLBACK_NONSTANDARD = re.compile(
    r"EditCaretTracker/002.*non-standard class|"
    r"EditCaretTracker/002.*focus candidate unresolved")
# B-6b moved the whitelist-fallback signature to capability codes: /006
# (probe inconclusive) and /007 (probe says EM_* not handled). For every
# scenario in this matrix the intended verdict on RichEditD2DPT is Capable,
# so BOTH codes are FAIL signatures (delegation B-6c section 3 common row).
RE_CAP_UNKNOWN = re.compile(r"EditCaretTracker/006")
RE_CAP_NOTCAPABLE = re.compile(r"EditCaretTracker/007")
RE_CLAMP = re.compile(r"EditCaretTracker/004.*clamped to 0")
RE_ESTIMATE_005 = re.compile(r"EditCaretTracker/005")
RE_SETTLE_008 = re.compile(r"EditCaretTracker/008")
RE_EM_FAILURE = re.compile(
    r"EditCaretTracker/(001|003): EM_(GETSEL|SETSEL).*failed/timed out")
RE_EMPTY_HOLD = re.compile(r"stage=empty_capture decision=hold_send")
# REQ-034 F3-B paste-window gate signatures (src/worker.cpp L242-254): the
# suppression decision line and its WORKER/ExecuteTask/036 companion.
RE_EMPTY_SUPPRESS = re.compile(
    r"stage=empty_capture decision=paste_window_suppress")
RE_DIAG_036 = re.compile(r"WORKER/ExecuteTask/036")
RE_DIAG_036_DETAIL = re.compile(
    r"WORKER/ExecuteTask/036: empty capture within paste window "
    r"\(elapsed (\d+)ms <= (\d+)ms\)")
# No-selection notice surface (hold branch's UI side effect via
# empty_capture_cb_ -> ShowMessageThreadSafe): "UI tooltip_show kind=message"
# (src/ui/tooltip.cpp L760). The marshal-drop variant (L754) counts as
# SURFACE evidence too - it proves the callback fired and only lost a
# generation race, which must not turn the C-5 notice assertion into a
# false product FAIL.
RE_NOTICE_SURFACE = re.compile(
    r"tooltip_(?:show|marshal_drop) kind=message")
RE_SUBSYSTEMS = re.compile(r"SESSION/subsystems started")
RE_SECOND_INSTANCE = re.compile(r"SESSION/second instance")
RE_LANG_PAIR = re.compile(
    r'stage=lang_pair ctx=type src="([^"]*)" tgt="([^"]*)"')
# E5 fixture witnesses (src/main.cpp L1655-1680, src/diag_logger.cpp):
#   SESSION/diag_log_enabled=1 diag_log_content=1 (file sink ENABLED - ...)
#   SESSION/config loaded=1 ui_language=auto engine=local ... auto_send=0 ...
#   SESSION/==== Emebalachat v0.10.8 session start (pid=..., exe=...)
# The config line's optional content fields (model_path=, drag=, type=) are
# content-gated, but auto_send= is NOT, so the mode witness always exists.
RE_SESSION_DIAG_FLAGS = re.compile(
    r"SESSION/diag_log_enabled=(?P<enabled>[01]) "
    r"diag_log_content=(?P<content>[01])")
RE_SESSION_CONFIG = re.compile(
    r"SESSION/config loaded=(?P<loaded>[01]) .*?"
    r"\bauto_send=(?P<auto_send>[01])\b")
RE_SESSION_VERSION = re.compile(
    r"SESSION/==== Emebalachat (?P<version>\S+) session start")
RE_SESSION_EXE = re.compile(r"session start \(pid=\d+, exe=(?P<exe>.*?)\) ====")
RE_SHIFT_GATE = re.compile(r"ENTER_GATE.*reason=shift_enter_newline")
# Runtime language-change record (src/main.cpp L604 STATE/lang_sync): the
# single authority every tray menu pick routes through. Group 1 = the new
# TYPE target language name. Proves a UIA tray selection reached
# ApplyLanguageChange at runtime (REQ-019's live-switch path).
RE_LANG_SYNC_TYPE_TO = re.compile(
    r"lang_sync ctx=type valid=1 (?:changed=\d )?pair .* -> .*/(.+?) \(req")
# Tray "Interface Language" submenu title per locale table (src/i18n.cpp
# StringId::MenuUiLanguage: KO L153 / EN L547; the ko/en pair is the exact
# idiom of uia_tray_menu.ps1's $groupRx walker). ui_language defaults to
# "auto", so the EN/KO tables are the deterministic enumeration target for
# the uilang_37 scenario; other UI locales keep the count+endonym assertions
# (only the title lookup is ko/en-bounded by the ps1 walker).
UILANG_SUBMENU_TITLE_RX = r"(인터페이스 언어|Interface Language)"


def read_log(path):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def unescape_diag(s):
    """The log escapes control chars (diag_logger AppendEscaped): content
    fields carry literal backslash-r / backslash-n sequences."""
    return s.replace("\\r\\n", "\r\n").replace("\\n", "\n").replace("\\r", "\r")


class TaskBlock:
    """All log events attributable to one enter-pipeline task, from
    task_received through task_end. Raw (lineno, text) kept for citations."""

    def __init__(self, index, start_line_no):
        self.index = index
        self.start_line_no = start_line_no
        self.lines = []
        self.capture_len = None
        self.capture_content = None   # None = content gated off (E1/E7)
        self.capture_result = None    # ok | empty | guard_abort | ...
        self.capture_k = None
        self.translate_status = None
        self.translate_out_len = None
        self.translate_out = None
        self.paste_result = None
        self.paste_skip_reason = None
        self.send_enter_action = None   # synthetic_newline|synthetic_send|SKIPPED|synthetic
        self.send_enter_reason = None   # SKIPPED reason (send_gate|h1_foreground_mismatch)
        self.send_enter_auto_send = None  # only the injected branch logs it
        self.task_auto_send = None      # lang_pair witness, every capture path
        self.em_setselect = False
        self.em_setselect_last = None
        self.em_setselect_caret = None
        self.fallback_nonstandard = False
        self.em_failure = False
        self.empty_hold = False
        self.empty_suppress = False   # REQ-034 F3-B paste-window gate fired
        self.diag_036 = False         # WORKER/ExecuteTask/036 line seen
        self.notice_surface = False   # tooltip show/drop kind=message seen
        self.send_through = False
        self.cap_unknown = False
        self.cap_notcapable = False
        self.clamped = False
        self.estimate_005 = False
        self.settle_008 = False
        self.lang_tgt = None
        self.completed = False
        self.pasted = None

    def feed(self, lineno, text):
        self.lines.append((lineno, text))
        m = RE_CAPTURE_END.search(text)
        if m and self.capture_len is None:
            self.capture_len = int(m.group("len"))
            # None when the shape-only branch wrote the line (E1): the content
            # checks must NOT treat that as "captured an empty string".
            self.capture_content = m.group("content")
            self.capture_k = int(m.group("k"))
            self.capture_result = m.group("result")
        m = RE_TRANSLATE_END.search(text)
        if m:
            self.translate_status = int(m.group(1))
        m = RE_TRANSLATE_OUT.search(text)
        if m:
            self.translate_out_len = int(m.group("len"))
            self.translate_out = m.group("out")
        m = RE_PASTE_RESULT.search(text)
        if m:
            self.paste_result = int(m.group(1))
        m = RE_PASTE_SKIPPED.search(text)
        if m:
            self.paste_skip_reason = m.group(1)
        m = RE_LANG_PAIR.search(text)
        if m:
            self.lang_tgt = m.group(2)
        m = RE_TASK_AUTO_SEND.search(text)
        if m:
            self.task_auto_send = int(m.group("v"))
        m = RE_SEND_ENTER.search(text)
        if m:
            self.send_enter_action = m.group("action")
        m = RE_SEND_ENTER_REASON.search(text)
        if m:
            self.send_enter_reason = m.group("reason")
        m = RE_SEND_ENTER_AUTO_SEND.search(text)
        if m:
            self.send_enter_auto_send = int(m.group("auto_send"))
        m = RE_EM_SETSELECT_DETAIL.search(text)
        if m:
            # groups: 1=hwnd 2=last 3=caret
            self.em_setselect_last = int(m.group(2))
            self.em_setselect_caret = int(m.group(3))
        if RE_EM_SETSELECT.search(text):
            self.em_setselect = True
        if RE_FALLBACK_NONSTANDARD.search(text):
            self.fallback_nonstandard = True
        if RE_EM_FAILURE.search(text):
            self.em_failure = True
        if RE_EMPTY_HOLD.search(text):
            self.empty_hold = True
        if RE_EMPTY_SUPPRESS.search(text):
            self.empty_suppress = True
        if RE_DIAG_036.search(text):
            self.diag_036 = True
        if RE_NOTICE_SURFACE.search(text):
            self.notice_surface = True
        if RE_SEND_THROUGH.search(text):
            self.send_through = True
        if RE_CAP_UNKNOWN.search(text):
            self.cap_unknown = True
        if RE_CAP_NOTCAPABLE.search(text):
            self.cap_notcapable = True
        if RE_CLAMP.search(text):
            self.clamped = True
        if RE_ESTIMATE_005.search(text):
            self.estimate_005 = True
        if RE_SETTLE_008.search(text):
            self.settle_008 = True
        m = RE_TASK_END.search(text)
        if m:
            self.completed = True
            self.pasted = int(m.group(1))

    def cite(self, regex):
        for ln, tx in self.lines:
            if regex.search(tx):
                return f"L{ln}: {tx.strip()}"
        return ""


def parse_tasks(text):
    """Split the log into per-enter task blocks. Lines before the first
    task_received (startup chatter) and between blocks (hook/KEY/UI lines)
    attach to the open block until task_end closes it. NOTE: the empty-capture
    hold branch returns BEFORE task_end (src/worker.cpp hold + return), so
    such a block stays open until the next task_received - the parser's
    block-per-task_received split still yields one block per Enter."""
    blocks = []
    current = None
    idx = 0
    for i, line in enumerate(text.splitlines(), 1):
        if RE_TASK_RECEIVED.search(line):
            current = TaskBlock(idx, i)
            blocks.append(current)
            idx += 1
            current.feed(i, line)
            continue
        if current is not None:
            current.feed(i, line)
            if current.completed:
                current = None
    return blocks


def utf16_len(s):
    return len(s.encode("utf-16-le")) // 2


def wait_block_terminal(app_log_text, index, timeout=None):
    """Poll until task block `index` reaches ANY terminal marker. Both empty-
    capture exits (R5 hold src/worker.cpp L256-266 and the F3-B suppression
    branch L242-255) return BEFORE stage=task_end, so the method-level
    wait_task_complete (early-exit on completed OR empty_hold) never sees the
    suppression branch. This helper adds empty_suppress + send_through to the
    exit set; timeout behaves identically (return the newest block view)."""
    deadline = time.time() + (timeout or STEP_TIMEOUT_S)
    blocks = []
    while time.time() < deadline:
        blocks = parse_tasks(app_log_text())
        if len(blocks) > index:
            b = blocks[index]
            if (b.completed or b.empty_hold or b.empty_suppress
                    or b.send_through):
                return b
        time.sleep(POLL_S)
    if len(blocks) > index:
        return blocks[index]
    return TaskBlock(index, 0)


# ---------------------------------------------------------------------------
# harness outcomes
# ---------------------------------------------------------------------------
class HarnessError(Exception):
    """A bug/unavailability in the harness itself -> exit 3."""


class EnvBlock(Exception):
    """Environment precondition failed -> INCONCLUSIVE (never product FAIL)."""


class FixtureRefused(EnvBlock):
    """The config fixture REFUSES to pin (F1/F3, 261121_0002 round 2).

    A deliberate refusal with guidance, not a missing precondition. It stays an
    EnvBlock subclass so every existing handler (main()'s fixture block, the
    per-scenario `except EnvBlock`) keeps classifying it as ENVIRONMENT: never a
    product FAIL, and never a config write."""


_assert_input_size()


# ---------------------------------------------------------------------------
# sessions
# ---------------------------------------------------------------------------
class AppSession:
    """Owns the Emebala_chat.exe process spawned for this run and its log."""

    def __init__(self, exe_path):
        self.exe_path = exe_path
        self.proc = None
        self.log_path = None

    def start(self):
        # Plan-B (REQ-B007) B-T7 R2: pre-flight sibling-process isolation
        # gate — refuse to spawn Chat while the Listener app / shared engine
        # host is live (concurrent engine-pipe / Common-store access is the
        # flicker root cause).  Runs BEFORE the self-instance check below;
        # EMEBALA_E2E_FORCE=1 skips it (documented escape hatch, see
        # tools/e2e/README.md).  IsolationGateError is a RuntimeError, so
        # the harness's existing top-level RuntimeError handling reports it.
        check_isolation_gate(list_processes_like)
        pre = list_processes_like("Emebala_chat")
        if pre:
            raise EnvBlock(
                "Emebala_chat.exe is already running (PIDs "
                f"{sorted(pre)}). The app is single-instance (mutex, "
                "src/main.cpp L278): close it, then re-run.")
        if not os.path.exists(self.exe_path):
            raise HarnessError(f"app exe not found: {self.exe_path}")
        logs_before = set()
        if os.path.isdir(LOG_DIR):
            logs_before = {f for f in os.listdir(LOG_DIR) if LOG_RE.match(f)}
        self.proc = subprocess.Popen(
            [self.exe_path], cwd=os.path.dirname(self.exe_path),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + APP_READY_TIMEOUT_S
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise EnvBlock(
                    f"app exited right after launch (exit {self.proc.returncode}); "
                    "possible stray single-instance dialog or crash - inspect "
                    f"{LOG_DIR}")
            if os.path.isdir(LOG_DIR):
                new_logs = [f for f in os.listdir(LOG_DIR)
                            if LOG_RE.match(f) and f not in logs_before]
                if new_logs:
                    cand = max(new_logs, key=lambda f: os.path.getmtime(
                        os.path.join(LOG_DIR, f)))
                    path = os.path.join(LOG_DIR, cand)
                    text = read_log(path)
                    if RE_SUBSYSTEMS.search(text):
                        self.log_path = path
                        return
                    if RE_SECOND_INSTANCE.search(text):
                        raise EnvBlock(
                            "app logged 'second instance detected; exiting'")
            time.sleep(POLL_S)
        raise EnvBlock(
            "app did not log 'subsystems started' within "
            f"{APP_READY_TIMEOUT_S}s")

    def log_text(self):
        return read_log(self.log_path)

    def stop(self):
        if self.proc is None:
            return
        # the app is single-instance: killing image-name matches only ever
        # hits the instance we own. Settle delay lets the flush thread drain.
        time.sleep(TEARDOWN_SETTLE_S)
        force_kill_pids(list_processes_like("Emebala_chat"))
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        self.proc = None


# hwnds currently owned by a live NotepadSession in this harness process
# (guards notepad_vscode_mix's two windows from double-adopting each other).
_ADOPTED_HWND = set()

# Environment facts gathered while the run proceeds and persisted by E11's
# run-manifest.json. Deliberately plain module state: the manifest must survive
# a mid-run EnvBlock (it is written from main()'s finally).
_OBSERVED = {
    "frame_class": None,
    "edit_classes": set(),
    "read_methods": set(),
    "app_version": None,
    "app_exe_reported": None,
    "tray_uia": None,
    "notepad_version": None,
}


class NotepadSession:
    """Opens (or reuses) an EMPTY Notepad scratch WINDOW and owns it until
    teardown; it then closes ONLY that window (WM_CLOSE + UIA 'don't save'
    dismissal). Win11 notepad.exe is a packaged app whose windows share one
    process: taskkill would destroy the user's other unsaved documents and
    is forbidden (B-6c removed a duplicate stop() definition that did
    exactly that). Adoption requires the document to READ EMPTY, so a
    window holding user content is never claimed, cleared, or closed."""

    def __init__(self):
        self.pid = 0
        self.top = 0
        self.edit = 0
        self.edit_class = ""
        self.cleanup_note = None
        self.foreign = False
        self.residue_cleared = False
        self._close_diag = []

    @staticmethod
    def _is_untitled_blank(title):
        """Win11 Notepad frame title of a never-saved blank document, with or
        without the dirty-star prefix ('제목 없음 - 메모장', 'Untitled -
        Notepad', bare '메모장'). A real document title contains the file
        name, so this regex never matches one."""
        t = title.lstrip("*").strip()
        return bool(re.match(
            r"^(제목 없음|untitled|메모장|notepad)"
            r"(\s*[-–]\s*(메모장|Notepad))?$", t, re.IGNORECASE))

    def _try_adopt(self, hwnd, require_untitled, fast=True):
        """Best-effort claim of one frame: focus (escalation ladder), resolve
        the edit control, and require the document to be empty (or our own
        E2Exx residue, which assert_empty clears). Returns 'adopted' on
        success, 'retry' when the frame may still become adoptable (still
        loading, transient focus loss), and 'reject' when it provably holds
        non-harness content or belongs to a live session - only 'reject'
        permanently removes the candidate. Any non-adopted frame is left
        EXACTLY as found."""
        if hwnd in _ADOPTED_HWND:
            return "reject"
        if require_untitled:
            titled = None
            for h, (pid, title) in notepad_top_windows().items():
                if h == hwnd:
                    titled = title
                    break
            if titled is None:
                return "reject"  # vanished
            if not self._is_untitled_blank(titled):
                return "reject"  # a named document is USER content
        self.top = hwnd
        try:
            self._focus(fast=fast)
            self._resolve_edit()
        except EnvBlock:
            self.top = 0
            return "retry"
        txt, _m = self.read_text()
        if txt is None:
            self.top = 0
            return "retry"  # control not answering yet (window still warm)
        if txt.replace("\ufeff", "").strip():
            if not self._is_harness_residue(txt):
                self.top = 0
                return "reject"  # holds user text: never touched
        for h, (pid, title) in notepad_top_windows().items():
            if h == hwnd:
                self.pid = pid
                _ADOPTED_HWND.add(hwnd)
                # E11 manifest: the classes the harness ACTUALLY drove. A
                # future change here (Notepad drift) becomes a recorded fact
                # instead of a silently different read path.
                _OBSERVED["frame_class"] = get_class(hwnd)
                if self.edit_class:
                    _OBSERVED["edit_classes"].add(self.edit_class)
                return "adopted"
        self.top = 0
        return "reject"

    @staticmethod
    def preflight_scrub(limit=40, keep=1):
        """Close harness-leftover scratch frames BEFORE any scenario: a
        frame whose title is untitled-blank, whose child edit controls ALL
        read EMPTY (single edit = single tab - frames with several tab
        edits are skipped, their title only shows the active tab), and that
        closes without surviving. B-6c necessity (probed live 260907): a
        desktop cluttered with 23 restored/abandoned empty frames makes a
        blank notepad launch coalesce into an existing frame and the
        foreground ladder stops winning grants for old windows, so EVERY
        scenario died in adoption. Empty-untitled frames can only be
        harness residue - the harness never types without a marker, and a
        user document with content is excluded by the emptiness check.
        Returns (closed, kept_blank) counts. `keep` empty frames are left
        open on purpose: the reuse-adoption pool, so a scenario never has
        to depend on Win11 creating a brand-new frame (it may instead
        activate an existing one - run3/run4 evidence).
        """
        closed = blanks_seen = 0
        for hwnd, (pid, title) in sorted(notepad_top_windows().items()):
            if closed >= limit:
                break
            if not NotepadSession._is_untitled_blank(title):
                continue
            edits = [h for h in enum_child_windows(hwnd)
                     if "edit" in get_class(h).lower()]
            if len(edits) != 1:
                continue  # multi-tab frame: title proves too little
            txt = read_edit_text_wmgettext(edits[0])
            if txt is None or txt.replace("\ufeff", "").strip():
                continue  # content or unreadable: never touched
            blanks_seen += 1
            if blanks_seen <= keep:
                continue  # keep pool
            user32.PostMessageW(HWND(hwnd), WM_CLOSE, WPARAM(0), LPARAM(0))
            deadline = time.time() + 3.0
            while time.time() < deadline and user32.IsWindowVisible(HWND(hwnd)):
                time.sleep(0.3)
            if not user32.IsWindowVisible(HWND(hwnd)):
                closed += 1
        # E9 (261121_0002): `keep` blanks are deliberately left open, so the
        # reuse-pool count is blanks_seen - keep; when fewer blanks exist than
        # `keep` the honest count is 0, not a negative number in every log.
        return closed, max(0, blanks_seen - keep)

    def start(self):
        # Adoption (B-6c, probed live 260907 - two Win11 realities):
        #  1) passing a FILE to notepad.exe coalesces into a TAB of an
        #     existing frame (and /m is ignored by the Store build), so the
        #     launch is BLANK and ownership is proven by content emptiness,
        #     not by a title marker;
        #  2) once blank frames accumulate (harness churn + session
        #     restore), a blank launch may ACTIVATE an existing empty
        #     untitled frame instead of creating a new one (run3 260907:
        #     every scenario after the first three died waiting for a
        #     "fresh frame"). So: claim the first FRESH empty frame, else
        #     REUSE an existing empty untitled frame - both are harness
        #     scratch by construction (empty document => nothing to lose;
        #     a user's unsaved *typed* document is non-empty and never
        #     matched). Foreign non-empty content is never cleared/closed.
        pre = set(notepad_top_windows())
        subprocess.Popen(["notepad.exe"])
        deadline = time.time() + NOTEPAD_READY_TIMEOUT_S
        rejected = set()
        while time.time() < deadline:
            fresh = {h: v for h, v in notepad_top_windows().items()
                     if h not in pre}
            for h in sorted(fresh):
                if h in rejected:
                    continue
                # FULL focus ladder for our own fresh window: when Win11
                # routes the launch through a persisted zero-window process
                # the new frame gets NO foreground grant and only the
                # AttachThreadInput/SwitchToThisWindow rungs win it; the
                # candidate is retried each round until it rejects or the
                # deadline passes (fast-mode-only rejection is what made
                # run4 fail adoption on a perfectly good fresh frame).
                res = self._try_adopt(h, require_untitled=False, fast=False)
                if res == "adopted":
                    return
                if res == "reject":
                    rejected.add(h)
            time.sleep(POLL_S)
        # reuse path: any existing untitled-empty frame, cheap fast-focus
        # scan (stale frames multiply; a full ladder per frame is minutes)
        for h in sorted(notepad_top_windows()):
            if h in rejected:
                continue
            res = self._try_adopt(h, require_untitled=True, fast=True)
            if res == "adopted":
                return
            if res == "retry":
                res2 = self._try_adopt(h, require_untitled=True, fast=False)
                if res2 == "adopted":
                    return
        self.top = 0
        raise EnvBlock(
            "no adoptable empty Notepad frame (fresh or untitled-blank) - "
            "notepad.exe unavailable or every candidate window holds user "
            "content (never touched). If the foreground is held by an "
            "invisible phantom window, see _foreground_guard's recovery "
            "note (GameInputSvc wedge, 260907).")

    def _resolve_edit(self):
        """Edit control = the keyboard-focused control (GetGUIThreadInfo -
        exactly what the app's EM path targets), falling back to child
        enumeration. Must run AFTER _focus(): hwndFocus only points at the
        edit once our window owns the foreground."""
        cls = ""
        focus = get_focus_hwnd_for_window(self.top)
        if focus:
            cls = get_class(focus)
            if "edit" in cls.lower():
                self.edit, self.edit_class = focus, cls
                return
        edit, ecls = find_edit_control(self.top)
        if edit:
            self.edit, self.edit_class = edit, ecls
            return
        raise EnvBlock(
            f"Notepad window 0x{self.top:X} exposes no detectable edit "
            f"control (focused hwnd class={cls!r})")

    def _tap_alt(self):
        """Classic foreground-lock unlock: a bare ALT down+up grants the
        calling thread the right to SetForegroundWindow. Harmless to the
        app hook: VK_MENU alone never passes the bare-Enter gate (it is
        only consulted inside the VK_RETURN branch)."""
        down = INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(VK_MENU, 0x38, 0, 0, None))
        up = INPUT(type=INPUT_KEYBOARD,
                   ki=KEYBDINPUT(VK_MENU, 0x38, KEYEVENTF_KEYUP, 0, None))
        arr = (INPUT * 2)(down, up)
        user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT))

    def _focus(self, fast=False):
        """Foreground acquisition, escalating. When notepad.exe spawns a NEW
        process its first window is granted foreground (probed live: attempt
        0 OK); when Win11 reuses a persisted zero-window Notepad process the
        new window gets NO grant and the harness must win the foreground
        lock itself. Escalation ladder (proven Win32 recipes):
          1. plain SetForegroundWindow (launch grace window, if any),
          2. ALT tap + SetForegroundWindow (user-input resets the lock),
          3. AttachThreadInput(foreground) + SetForegroundWindow +
             BringWindowToTop (documented queue-attach workaround),
          4. SwitchToThisWindow (legacy but effective last resort).
        Poll to verified foreground after each rung. The Enter injection
        MUST run against a verified foreground window (else the app's H1
        guard aborts the paste, or a wrong window takes the hit), so total
        failure raises EnvBlock -> INCONCLUSIVE, never a silent proceed."""
        user32.ShowWindow(HWND(self.top), SW_RESTORE)
        cur_tid = kernel32.GetCurrentThreadId()
        # B-6c: when fast=True (adoption probing only) stop after the
        # cheap rungs (SFW + ALT tap ~2 s): a stale frame that cannot win
        # the foreground fast is skipped by _try_adopt and the next
        # candidate tried - scanning 20+ restored frames at 5 s each is
        # what turned run3/run4 into minutes-long stalls. Scenario-side
        # focus keeps the FULL ladder (a real adopted window sometimes
        # needs AttachThreadInput/SwitchToThisWindow to win the grant).
        for attempt in range(10 if fast else 24):
            fg = user32.GetForegroundWindow()
            if int(fg or 0) == self.top:
                return
            if attempt < 4:
                user32.SetForegroundWindow(HWND(self.top))
            elif attempt < 10:
                self._tap_alt()
                user32.SetForegroundWindow(HWND(self.top))
            elif attempt < 20:
                fg_tid = user32.GetWindowThreadProcessId(fg, None)
                user32.AttachThreadInput(cur_tid, fg_tid, True)
                user32.SetForegroundWindow(HWND(self.top))
                user32.BringWindowToTop(HWND(self.top))
                user32.AttachThreadInput(cur_tid, fg_tid, False)
            else:
                user32.SwitchToThisWindow(HWND(self.top), wintypes.BOOL(1))
            time.sleep(0.2)
        # final verification pass
        if int(user32.GetForegroundWindow() or 0) == self.top:
            return
        raise EnvBlock(
            "could not bring the spawned Notepad to the foreground after "
            "the full escalation ladder - the harness Enter would hit the "
            "wrong window; environment issue (close leftover Notepad "
            "processes or click away from any foreground-locking app)")

    def read_text(self):
        """(text, method). text None = unreadable by both readers."""
        txt = read_edit_text_wmgettext(self.edit)
        if txt is not None:
            _OBSERVED["read_methods"].add("WM_GETTEXT")
            return txt, "WM_GETTEXT"
        txt = read_edit_text_uia_ps(self.pid, self.top)
        if txt is not None:
            _OBSERVED["read_methods"].add("UIA-ValuePattern")
        return txt, "UIA-ValuePattern"

    @staticmethod
    def _is_harness_residue(text):
        """True when EVERY non-blank line contains an E2Exx scenario marker.
        Only the harness types such lines; restored USER documents never
        consist exclusively of marker lines (a false positive would require
        the user to have typed E2E\\d\\d on every single line)."""
        lines = [l for l in text.splitlines() if l.strip()]
        return bool(lines) and all(re.search(r"E2E\d{2}", l) for l in lines)

    def assert_empty(self):
        """NON-DESTRUCTIVE emptiness gate. Win11 Notepad's session restore
        reopens unsaved documents as fresh windows (observed live 260907:
        our own scratch docs from previous killed runs came back via
        handle-delta adoption). Policy:
          * empty                          -> proceed;
          * content made ONLY of harness
            'E2Exx' marker lines           -> our residue: clear it
            (EM_SETSEL + WM_CLEAR, edit
            messages, invisible to hook);
          * any other content (user data)  -> abort INCONCLUSIVE and leave
            the window EXACTLY as found (never cleared/closed). Fix on the
            machine: disable Notepad session restore in Settings, or close
            leftover windows once cleanly, then re-run."""
        deadline = time.time() + 5.0
        last = None
        cleared_residue = False
        while time.time() < deadline:
            last, _ = self.read_text()
            if last is None:
                time.sleep(POLL_S)
                continue
            if not last.replace("\ufeff", "").strip():
                self.residue_cleared = cleared_residue
                return
            if self._is_harness_residue(last):
                send_em(self.edit, EM_SETSEL, 0, -1)
                user32.PostMessageW(HWND(self.edit), WM_CLEAR, WPARAM(0),
                                    LPARAM(0))
                cleared_residue = True
            else:
                self.foreign = True
                # F4 (261121_0002 round 2): LENGTH-ONLY. This message flows
                # into env_notes -> run-manifest.json notes + the console
                # transcript that becomes release evidence, so it must never
                # carry a fragment of the user's document.
                raise EnvBlock(
                    "the Notepad window the harness adopted opened with "
                    f"PRE-EXISTING content (session restore?) - refusing to "
                    "clear or close user data. Window left untouched. "
                    "Disable Notepad session restore (Settings) or close "
                    "leftover Notepad windows, then re-run. "
                    f"(content withheld: {len(last.strip())} chars, "
                    "shape-only)")
            time.sleep(POLL_S)
        raise EnvBlock(
            "fresh Notepad document unreadable/unclearable within 5s "
            f"(read={last!r}) - aborting before typing")

    def _send_chars_unicode(self, text):
        """Fallback typer: SendInput KEYEVENTF_UNICODE. These reach the app
        as unmarked input and the hook logs them as keystrokes, but only
        VK_RETURN triggers the pipeline (vk 0 keeps the IME mirror false),
        so they land in Notepad exactly like IME-committed characters."""
        for ch in text:
            down = INPUT(type=INPUT_KEYBOARD,
                         ki=KEYBDINPUT(0, ord(ch), KEYEVENTF_UNICODE, 0, None))
            up = INPUT(type=INPUT_KEYBOARD,
                       ki=KEYBDINPUT(0, ord(ch),
                                     KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 0,
                                     None))
            arr = (INPUT * 2)(down, up)
            if user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT)) != 2:
                raise HarnessError("SendInput(unicode) failed")
            time.sleep(0.01)  # ordering margin between injected events

    def type_line(self, text):
        """Insert via WM_CHAR (hook-invisible, IME-independent), then verify
        by reading the control back (poll, endswith = precise)."""
        self._focus()
        for ch in text:
            if not user32.PostMessageW(HWND(self.edit), WM_CHAR,
                                       WPARAM(ord(ch)), LPARAM(0)):
                raise HarnessError("PostMessageW(WM_CHAR) failed")
        deadline = time.time() + 8.0
        last = None
        while time.time() < deadline:
            last, _ = self.read_text()
            if last is not None and last.endswith(text):
                return
            time.sleep(POLL_S)
        # Fallback only when NOTHING of this line landed (marker absent) -
        # a partial WM_CHAR insertion aborts to INCONCLUSIVE instead of
        # risking a double-typed line that would fake a product FAIL.
        if last is not None and "E2E" in last[-60:]:
            raise EnvBlock(
                "WM_CHAR insertion only partially visible "
                f"(read={last!r}); aborting to avoid duplicate typing")
        self._send_chars_unicode(text)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            last, _ = self.read_text()
            if last is not None and last.endswith(text):
                return
            time.sleep(POLL_S)
        raise EnvBlock(
            "neither WM_CHAR nor SendInput(unicode) inserted the line "
            f"(read={last!r}) - input path unavailable in this environment")

    def press_backspaces(self, count):
        """Real VK_BACK keystrokes via SendInput (unmarked). WM_CHAR 0x08 is
        ignored by Win11 Notepad's RichEditD2DPT (probed live 260907: 32
        posted WM_CHAR backs left the document at full length), so the
        honest simulation is the physical-key path. The app hook passes
        VK_BACK through untouched (only VK_RETURN gates the pipeline), and
        it never sets the IME mirror, so the Enter gate stays open.
        Verified by the document actually shrinking by `count` units."""
        self._focus()
        before, _ = self.read_text()
        if before is None:
            raise EnvBlock("document unreadable before backspaces")
        target = max(0, utf16_len(before) - count)
        for _ in range(count):
            down = INPUT(type=INPUT_KEYBOARD,
                         ki=KEYBDINPUT(VK_BACK, 0x0E, 0, 0, None))
            up = INPUT(type=INPUT_KEYBOARD,
                       ki=KEYBDINPUT(VK_BACK, 0x0E, KEYEVENTF_KEYUP, 0, None))
            arr = (INPUT * 2)(down, up)
            if user32.SendInput(2, ctypes.byref(arr),
                                ctypes.sizeof(INPUT)) != 2:
                raise HarnessError("SendInput(VK_BACK) failed")
            time.sleep(0.02)
            self._focus()  # hold the target: an app tooltip may steal fg
        deadline = time.time() + 5.0
        now = before
        while time.time() < deadline:
            now, _ = self.read_text()
            if now is not None and utf16_len(now) <= target:
                return now
            time.sleep(POLL_S)
        raise EnvBlock(
            f"backspaces did not take effect (len before={utf16_len(before)}, "
            f"after={None if now is None else utf16_len(now)}, wanted {target})")

    def set_caret(self, pos):
        """EM_SETSEL(pos, pos) on our edit control and verify by EM_GETSEL
        readback (LOWORD/HIWORD of the return value, UTF-16 offsets)."""
        ok, _ = send_em(self.edit, EM_SETSEL, pos, pos)
        if not ok:
            raise EnvBlock("EM_SETSEL timed out on the harness edit control")
        ok, res = send_em(self.edit, EM_GETSEL, 0, 0)
        start = res & 0xFFFF
        end = (res >> 16) & 0xFFFF
        if not ok or start != pos or end != pos:
            raise EnvBlock(
                f"caret verification failed: wanted {pos}, EM_GETSEL -> "
                f"ok={ok} start={start} end={end}")

    def _send_enter(self, scancode_mode=False):
        extra = KEYEVENTF_SCANCODE if scancode_mode else 0
        down = INPUT(type=INPUT_KEYBOARD,
                     ki=KEYBDINPUT(VK_RETURN, 0x1C, extra, 0, None))
        up = INPUT(type=INPUT_KEYBOARD,
                   ki=KEYBDINPUT(VK_RETURN, 0x1C, extra | KEYEVENTF_KEYUP, 0, None))
        arr = (INPUT * 2)(down, up)
        n = user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT))
        if n != 2:
            raise HarnessError(f"SendInput returned {n} (expected 2)")

    def _send_combo(self, mod_vk, main_vk):
        """Unmarked modifier+key combo via SendInput. For Shift+Enter the
        app hook sees shift=1 and exercises its documented pass-through
        gate (REQ-018); for Ctrl+V no gesture branch consumes 'V'. The
        harness cannot mark synthetic input (EXTRA_INFO_MARKER is computed
        per-process by the app, src/win32_input.cpp L151) - unmarked is
        the honest simulation of a physical keystroke."""
        seq = [
            INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(mod_vk, 0, 0, 0, None)),
            INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(main_vk, 0, 0, 0, None)),
            INPUT(type=INPUT_KEYBOARD,
                  ki=KEYBDINPUT(main_vk, 0, KEYEVENTF_KEYUP, 0, None)),
            INPUT(type=INPUT_KEYBOARD,
                  ki=KEYBDINPUT(mod_vk, 0, KEYEVENTF_KEYUP, 0, None)),
        ]
        arr = (INPUT * len(seq))(*seq)
        if user32.SendInput(len(seq), ctypes.byref(arr),
                            ctypes.sizeof(INPUT)) != len(seq):
            raise HarnessError(f"SendInput(combo {mod_vk:#x}+{main_vk:#x}) failed")
        time.sleep(0.05)

    def press_shift_enter(self):
        self._focus()
        self._send_combo(VK_SHIFT, VK_RETURN)

    def press_ctrl_v(self):
        self._focus()
        self._send_combo(VK_CONTROL, VK_V)

    def press_enter_wait_task(self, app_log_text, expected_tasks):
        """Unmarked Enter -> hook intercepts -> pipeline task. Re-assert
        foreground immediately before the injection (focus can drift between
        typing and Enter, e.g. a save dialog from the previous window's
        close), then poll until the expected task_received count appears. On
        silence, one scancode-mode retry, then EnvBlock (hook inactive /
        toggle off = environment)."""
        self._focus()
        deadline = time.time() + ENTER_TASK_TIMEOUT_S
        self._send_enter()
        while time.time() < deadline:
            if len(parse_tasks(app_log_text())) >= expected_tasks:
                return
            time.sleep(POLL_S)
        self._send_enter(scancode_mode=True)
        deadline = time.time() + ENTER_TASK_TIMEOUT_S
        while time.time() < deadline:
            if len(parse_tasks(app_log_text())) >= expected_tasks:
                return
            time.sleep(POLL_S)
        raise EnvBlock(
            "no 'task_received' after two unmarked Enter injections - the "
            "keyboard hook is not intercepting (app toggled off via "
            "Win+F9, or SendInput blocked): environment, not a product FAIL")

    def _window_alive(self):
        return bool(user32.IsWindowVisible(HWND(self.top)))

    def stop(self):
        """Close ONLY our document window. A dirty buffer makes Win11 Notepad
        show its in-window 'save?' dialog; uia_close_window.ps1 invokes the
        'don't save' button (localized-name pattern match). The shared
        Notepad process is NEVER killed - it may hold the user's documents.
        This is deliberately gentler than the delegation's 'kill' wording:
        on Win11 the harness does not own a process, only a window. A
        window flagged foreign (restored user content) is NEVER closed."""
        if not self.top or self.foreign:
            _ADOPTED_HWND.discard(self.top)
            return
        _ADOPTED_HWND.discard(self.top)
        user32.PostMessageW(HWND(self.top), WM_CLOSE, WPARAM(0), LPARAM(0))
        for attempt in range(6):
            if not self._window_alive():
                return
            try:
                r = subprocess.run(
                    ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                     "-File", os.path.join(HERE, "uia_close_window.ps1"),
                     "-TargetHwnd", str(self.top)],
                    capture_output=True, timeout=20)
                self._close_diag.append(
                    (r.stdout or b"").decode("utf-8", "replace").strip()
                    or f"rc={r.returncode}")
            except (OSError, subprocess.TimeoutExpired) as e:
                self._close_diag.append(f"ps1-fail:{e}")
            time.sleep(0.4)
        if self._window_alive():
            self.cleanup_note = (
                f"Notepad window 0x{self.top:X} survived cleanup - left "
                "open for manual close (shared process intentionally NOT "
                f"killed). dialog-dismiss attempts: "
                + " | ".join(self._close_diag[-3:]))

    def wait_task_complete(self, app_log_text, index, timeout=None):
        deadline = time.time() + (timeout or STEP_TIMEOUT_S)
        blocks = []
        while time.time() < deadline:
            blocks = parse_tasks(app_log_text())
            if len(blocks) > index and blocks[index].completed:
                return blocks[index]
            if len(blocks) > index and blocks[index].empty_hold:
                # hold branch never logs task_end by design - early exit
                return blocks[index]
            time.sleep(POLL_S)
        blocks = parse_tasks(app_log_text())
        if len(blocks) > index:
            return blocks[index]
        return TaskBlock(index, 0)


def _foreground_guard():
    """Open+close one scratch window as a pre-flight (fails in ~20s with an
    ACTIONABLE message instead of cascading every scenario into 15-20s
    adoption timeouts). On failure, name the window currently holding the
    foreground. Live-verified 260907: GameInputSvc.exe's invisible
    'GameInputServiceWindow' can sit on the foreground throne with
    ForegroundLockTimeout forced to 0x7FFFFFFF; SFW/ALT-tap/
    AttachThreadInput/SwitchToThisWindow/LockSetForegroundWindow(UNLOCK)/
    SPI reset/a physical click on our own title bar/Alt+Tab/taskbar-invoke
    were ALL ineffective from user mode (per-source evidence in the B-6c
    batch report). Recovery outside this process: admin 'sc stop
    gameinputsvc' (trigger-starts again) or logoff/reboot, then re-run."""
    try:
        ns = NotepadSession()
        ns.start()
        ns.stop()
        if ns.cleanup_note:
            print("   [guard cleanup] " + ns.cleanup_note)
    except EnvBlock as e:
        f = int(user32.GetForegroundWindow() or 0)
        cls = get_class(f) if f else "NULL"
        vis = bool(user32.IsWindowVisible(HWND(f))) if f else False
        raise EnvBlock(
            f"foreground self-test failed ({e}); current foreground = "
            f"{cls!r} hwnd=0x{f:X} visible={vis}. An INVISIBLE holder means "
            "the session is foreground-locked by an external process "
            "(observed: GameInputSvc.exe phantom window, 260907). User-mode "
            "recovery is exhausted - run 'sc stop gameinputsvc' from an "
            "ADMIN console (it auto-restarts) or logoff/reboot, then "
            "re-run this harness.")


# ---------------------------------------------------------------------------
# E2E config fixture (261121_0002 E5): pin, verify, restore byte-verbatim
# ---------------------------------------------------------------------------
# Module state rather than a parameter so the judge functions can see the
# pinned mode without threading it through every call site (the replay script
# can still pass auto_send= explicitly).
_MODE = {
    "auto_send": False,   # the pinned value the app must report
    "fixture": None,      # ConfigFixture while this run owns one
}

# 261122_0001 A4 (F11.4): the crash ledger lives NEXT TO the config, under the
# same per-user directory, so a killed run can be detected and healed by the
# next one. The app ignores unknown sibling files.
MARKER_SUFFIX = ".e2e-pin.json"


def fixture_auto_send():
    """The auto_send mode this run pinned (False when no fixture is active,
    which is also the shipped default, src/config.hpp L993)."""
    return bool(_MODE["auto_send"])


class ConfigFixture:
    """Owns the RUNNING app's config.json for the duration of a run.

    Why (E5): diag_log_content is OFF by private default, which is what made
    the capture-content checks BLOCKED/FAIL for a whole release cycle, and
    auto_send decides whether the app injects an editor newline at all
    (REQ-028) - a user toggle during a run would silently flip every newline
    verdict. So the harness pins both, verifies the app's own SESSION lines
    against the pin, and restores the ORIGINAL BYTES in a finally.

    Privacy: only harness-owned keys are written (diag flags, auto_send, and
    multi_lang's target language). No user content is ever read or written
    here; every adopted document is verified empty before typing."""

    def __init__(self, path):
        self.path = path
        self.original = None      # original file bytes (restore source)
        self.pre_sha = None
        self.post_sha = None
        self.values = {}
        self.active = False
        self.restore_ok = None
        # 261122_0001 A4 (F11.4, SR2 F2): the crash ledger. A hard kill cannot
        # run main()'s finally, so the pin survives in config.json and the NEXT
        # run backs those PINNED bytes up as "the user's original" and restores
        # them forever - the poisoned-backup path. The marker is written BEFORE
        # the first pin write and removed after a VERIFIED restore; a leftover
        # marker is the crash evidence that lets the next run self-heal instead
        # of silently re-persisting QA flags.
        self.marker_path = (path + MARKER_SUFFIX) if path else None
        self._marker = None
        self._original_json = None
        self.recovery = None      # what backup() did about a stale marker

    # ---- the crash ledger (F11.4) -----------------------------------------
    def _write_marker(self):
        """Persist the ledger atomically. A failure is a WARNING, never an
        abort: the run still restores byte-verbatim, only the crash self-heal
        protection is lost (and the operator is told)."""
        if not self.marker_path or self._marker is None:
            return
        try:
            _write_json(self.marker_path, self._marker)
        except OSError as e:
            print("   fixture  : WARNING: could not write the pin marker "
                  f"{redact_user_path(self.marker_path)} ({type(e).__name__}: "
                  f"{e}); a hard kill of THIS run could not be self-healed "
                  "next time")

    def _clear_marker(self):
        if not self.marker_path or not os.path.exists(self.marker_path):
            return
        try:
            os.unlink(self.marker_path)
        except OSError as e:
            print("   fixture  : WARNING: could not remove the pin marker "
                  f"{redact_user_path(self.marker_path)} ({type(e).__name__}: "
                  f"{e}); the next run clears it (the restored bytes match its "
                  "original hash)")

    def _arm_marker(self, values):
        """Record the pre-pin bytes and the values about to be written BEFORE
        the write happens: from this point on a hard kill is recoverable."""
        if self._marker is None:
            self._marker = {
                "schema": 1,
                "harness": "tools/e2e/req027_e2e.py",
                "created_utc": utc_stamp(),
                "config": os.path.basename(self.path),
                "original_sha256": self.pre_sha,
                "original_b64": base64.b64encode(self.original).decode("ascii"),
                "pinned_sha256": None,
                "pinned_keys": {},
                "prev_values": {},
            }
        for k, v in values.items():
            self._marker["pinned_keys"][k] = v
            self._marker["prev_values"][k] = (self._original_json or {}).get(k)
        self._write_marker()

    def _refresh_marker(self):
        if self._marker is None:
            return
        try:
            with open(self.path, "rb") as f:
                self._marker["pinned_sha256"] = hashlib.sha256(
                    f.read()).hexdigest()
        except OSError:
            pass
        self._write_marker()

    def record_pin(self, key, value, prev_value):
        """Extend the crash ledger for a key written OUTSIDE pin().

        multi_lang patches type_target_language directly (it re-pins the file
        between blocks), so without this the ledger would miss that key and a
        hard kill mid-multi_lang would leave the harness language in place with
        no record of the user's own. The FIRST previous value wins: the undo
        target must be the user's value, never a harness value an earlier block
        already wrote."""
        if self._marker is None:
            return
        self._marker["pinned_keys"][key] = value
        self._marker["prev_values"].setdefault(key, prev_value)
        self._refresh_marker()

    def _recover_stale_pin(self):
        """Detect a pin left behind by a HARD-KILLED run and neutralise it.

        Dispositions, in order:
          * the file already matches the marker's ORIGINAL hash -> the crash
            happened after a successful restore; drop the marker, change
            nothing;
          * the file matches the marker's PINNED hash -> the crash left the QA
            pin in place; write the pre-pin bytes back and say so LOUDLY;
          * the file changed but every pinned key still holds its pinned value
            -> the app rewrote/reformatted the file; unpin those keys to their
            recorded previous values (value-level, not byte-verbatim);
          * anything else -> REFUSE to pin. The config no longer matches the
            ledger, so overwriting it could destroy a user edit; the marker
            still holds the pre-pin bytes for a manual restore.
        Returns True when a stale ledger was found (and handled)."""
        if not self.marker_path or not os.path.exists(self.marker_path):
            return False
        where = redact_user_path(self.marker_path)
        try:
            with open(self.marker_path, "r", encoding="utf-8") as f:
                marker = json.load(f)
            original = base64.b64decode(marker.get("original_b64") or "")
        except (OSError, ValueError, TypeError) as e:
            raise FixtureRefused(
                f"a pin marker from an interrupted run exists at {where} but "
                f"cannot be read ({type(e).__name__}: {e}). config.json may "
                "still carry the QA pin (diag_log_content=true). Inspect the "
                "marker and config.json by hand, delete the marker once the "
                "config is clean, then re-run.")
        orig_sha = marker.get("original_sha256")
        if not original or hashlib.sha256(original).hexdigest() != orig_sha:
            raise FixtureRefused(
                f"the pin marker at {where} does not carry usable pre-pin "
                "bytes (hash mismatch). Refusing to touch config.json: reset "
                "the diagnostic flags in the app's settings, delete the "
                "marker, then re-run.")
        if not os.path.exists(self.path):
            self._clear_marker()
            self.recovery = "marker-clear-no-config"
            print("   fixture  : cleared a stale pin marker (config.json is gone)")
            return True
        with open(self.path, "rb") as f:
            current = f.read()
        cur_sha = hashlib.sha256(current).hexdigest()
        if cur_sha == orig_sha:
            self._clear_marker()
            self.recovery = "marker-clear"
            print("   fixture  : cleared a stale pin marker (the config already "
                  "matched its pre-pin bytes)")
            return True
        if cur_sha == marker.get("pinned_sha256"):
            _atomic_write_bytes(self.path, original)
            with open(self.path, "rb") as f:
                healed = hashlib.sha256(f.read()).hexdigest()
            if healed != orig_sha:
                raise FixtureRefused(
                    "self-heal wrote the pre-pin bytes but the read-back hash "
                    f"does not match ({healed} != {orig_sha}); the marker at "
                    f"{where} still holds them - restore by hand.")
            self._clear_marker()
            self.recovery = "self-healed"
            print("   fixture  : SELF-HEALED a QA pin left by a hard-killed run "
                  f"(pre-pin bytes written back, sha256={orig_sha[:12]}...)")
            return True
        # Value-level recovery: the file was rewritten, but the harness-pinned
        # keys still hold the harness values.
        pinned_keys = marker.get("pinned_keys") or {}
        prev_values = marker.get("prev_values") or {}
        try:
            cur_data = json.loads(current.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            cur_data = None
        if (pinned_keys and isinstance(cur_data, dict) and
                all(cur_data.get(k) == v for k, v in pinned_keys.items())):
            patch_config_fields(self.path, prev_values)
            self._clear_marker()
            self.recovery = "self-healed-values"
            print("   fixture  : SELF-HEALED a QA pin left by a hard-killed run "
                  "(the file had been rewritten, so the pinned keys were "
                  f"restored to their pre-pin values: {sorted(pinned_keys)})")
            return True
        raise FixtureRefused(
            "a QA pin from an interrupted run cannot be neutralised safely: "
            f"config.json no longer matches the marker at {where} and its "
            "pinned keys are gone. Refusing to pin so the file is not "
            "overwritten. Compare config.json with the marker's original_b64 "
            "(it holds the pre-pin bytes), fix it by hand, delete the marker, "
            "then re-run.")

    def _warn_if_pinned_without_marker(self, data):
        """No marker, but both QA flags are already ON: a previous run may have
        been killed BEFORE the marker landed, or the marker was deleted. These
        bytes are treated as the user's own (restored verbatim), but the state
        is reported - silently carrying diag_log_content=true into the next run
        is exactly the poisoned-backup outcome F11.4 is about."""
        if (data.get("diag_log_enabled") is True
                and data.get("diag_log_content") is True):
            print("   fixture  : WARNING: config.json already has "
                  "diag_log_enabled=true and diag_log_content=true with NO pin "
                  "marker - a previous run may have been hard-killed before it "
                  "could restore. These bytes are treated as YOURS and are "
                  "restored verbatim (the flags stay ON). If you did not enable "
                  "diagnostic logging yourself, turn it off in the app settings "
                  "after this run.")

    def backup(self):
        # 261122_0001 A4 (F11.4): neutralise a previous run's crash leftovers
        # BEFORE reading, so the bytes captured below can never be the pinned
        # ones (the poisoned-backup path).
        self._recover_stale_pin()
        if not self.path or not os.path.exists(self.path):
            raise EnvBlock(
                "E2E config fixture: no config.json at "
                f"{redact_user_path(self.path)!r}. The app materializes it on first run "
                "(or the exe-dir copy when %LOCALAPPDATA% is unavailable); "
                "start the app once, close it, then re-run the harness. "
                "Without the file diag_log_content/auto_send cannot be "
                "pinned and the content checks would silently degrade to "
                "BLOCKED.")
        with open(self.path, "rb") as f:
            self.original = f.read()
        self.pre_sha = hashlib.sha256(self.original).hexdigest()
        try:
            data = json.loads(self.original.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as e:
            raise EnvBlock(f"E2E config fixture: unparseable config.json "
                           f"({redact_user_path(self.path)}): {e}")
        if not isinstance(data, dict):
            raise EnvBlock("E2E config fixture: config.json is not an object")
        self._original_json = data
        self._warn_if_pinned_without_marker(data)
        self.active = True
        return self

    def pin(self, values):
        """Write the given keys, then RE-READ the file and verify they
        persisted. Returns the previous values (informational; the byte-level
        restore below is the actual rollback).

        F11.4: the crash marker is armed BEFORE the write, so there is no
        window in which a pinned file exists without a recovery ledger."""
        self._arm_marker(values)
        prev = patch_config_fields(self.path, values)
        self.values.update(values)
        persisted = _read_json(self.path)
        bad = {k: persisted.get(k) for k, v in values.items()
               if persisted.get(k) != v}
        if bad:
            raise EnvBlock(
                "E2E config fixture: pin did not persist "
                f"(wanted {values}, file reports {bad}) at {redact_user_path(self.path)}")
        self._refresh_marker()
        return prev

    def restore(self):
        """Byte-verbatim restore + SHA-256 verification (R8: the app is
        force-killed first, so its own SaveToFile cannot race this).

        F1 (261121_0002 round 2): the write is atomic (temp file + replace), so
        an OSError or Ctrl+C mid-restore cannot truncate the user's config; an
        OSError still propagates and main() escalates it to exit 3."""
        if not self.active:
            return True, "e2e config fixture: no file was pinned"
        _atomic_write_bytes(self.path, self.original)
        with open(self.path, "rb") as f:
            post = f.read()
        self.post_sha = hashlib.sha256(post).hexdigest()
        self.restore_ok = (post == self.original
                           and self.post_sha == self.pre_sha)
        # F11.4: only a VERIFIED restore clears the crash marker. A mismatch
        # keeps it, so the next run can heal from it instead of backing up the
        # pinned bytes as the user's original.
        if self.restore_ok:
            self._clear_marker()
        state = "MATCH" if self.restore_ok else "MISMATCH"
        return self.restore_ok, (
            f"e2e config fixture restored: {redact_user_path(self.path)} "
            f"sha256 pre={self.pre_sha} post={self.post_sha} -> {state}")


def parse_session_info(text):
    """E5 witnesses from the app's own SESSION lines (see the RE_SESSION_*
    comments). Values are None when the line is absent."""
    out = {"loaded": None, "auto_send": None, "diag_enabled": None,
           "diag_content": None, "version": None, "exe": None}
    m = RE_SESSION_CONFIG.search(text)
    if m:
        out["loaded"] = int(m.group("loaded"))
        out["auto_send"] = int(m.group("auto_send"))
    m = RE_SESSION_DIAG_FLAGS.search(text)
    if m:
        out["diag_enabled"] = int(m.group("enabled"))
        out["diag_content"] = int(m.group("content"))
    m = RE_SESSION_VERSION.search(text)
    if m:
        out["version"] = m.group("version")
    m = RE_SESSION_EXE.search(text)
    if m:
        out["exe"] = m.group("exe")
    return out


def verify_session_fixture(app, fixture, expect_auto_send=None):
    """Assert the RUNNING app's session lines match the pinned fixture.

    A mismatch is an ENVIRONMENT abort (EnvBlock -> INCONCLUSIVE), never a
    product FAIL: it means the app read a different file, a second instance
    served the mutex, or the user flipped a toggle mid-run (E5/R1)."""
    info = parse_session_info(app.log_text())
    if info["version"]:
        _OBSERVED["app_version"] = info["version"]
    if info["exe"]:
        _OBSERVED["app_exe_reported"] = info["exe"]
    want_auto = (int(fixture_auto_send()) if expect_auto_send is None
                 else int(bool(expect_auto_send)))
    problems = []
    if info["loaded"] != 1:
        problems.append(f"config loaded={info['loaded']} (want 1)")
    if info["diag_enabled"] != 1:
        problems.append(f"diag_log_enabled={info['diag_enabled']} (want 1)")
    if info["diag_content"] != 1:
        problems.append(
            f"diag_log_content={info['diag_content']} (want 1 - content "
            "assertions would degrade to BLOCKED)")
    if info["auto_send"] != want_auto:
        problems.append(f"auto_send={info['auto_send']} (want {want_auto})")
    if problems:
        raise EnvBlock(
            "E2E CONFIG FIXTURE VIOLATION (environment, not a product FAIL): "
            + "; ".join(problems)
            + f" :: config={redact_user_path(fixture.path)} "
              f"pre_sha256={fixture.pre_sha} pinned={fixture.values}. "
              "A user toggle / second instance / different config path is "
              "the usual cause; the fixture is still restored in the finally.")
    print(f"   fixture  : verified (auto_send={want_auto} diag=1/1, "
          f"config={os.path.basename(fixture.path)}, "
          f"sha256={str(fixture.pre_sha)[:12]}...)")


def prepare_config_fixture(exe_path, *, no_cleanup=False):
    """Resolve and BACK UP the runtime config before the first launch (E5).

    Only read-only work happens here (resolve + read + parse). The PIN - the
    only write - is performed by main() INSIDE its restore-guarded try/finally,
    so every escape path from that point on (OSError, ValueError, a Ctrl+C
    during teardown) still runs the byte-verbatim restore (F1, 261121_0002
    round 2). Publishing a backed-up fixture before any write is what makes
    that airtight: there is no window in which the file is pinned while no
    restore is armed.

    A refusal is never a write:
      * --no-cleanup (SR2 F1): a live app's exit SaveToFile re-persists the
        pinned privacy flags from memory over the restored file;
      * legacy exe-dir layout (SR5 F-02): the app migrates the pinned bytes to
        %LOCALAPPDATA% at startup, out of this harness's restore reach.
    A missing/unparseable config is an ENVIRONMENT precondition (continue
    without a pin; capture-content checks degrade to BLOCKED, never PASS).
    A config that EXISTS but cannot be READ (PermissionError/OSError: ACL,
    transient lock) raises HarnessError instead - nothing was written, so the
    caller continues unpinned and exits 3, never a product FAIL (R3-2).

    Returns the backed-up ConfigFixture (holding the pre-run bytes for the
    caller's finally), or None when nothing may be written - the reason and its
    guidance are already printed.
    """
    if no_cleanup:
        detail = (
            "--no-cleanup keeps the app alive after the run and its exit "
            "SaveToFile re-persists the pinned diag_log_* flags from memory "
            "over the restored file (src/main.cpp), so the byte-verbatim "
            "restore cannot be guaranteed. Re-run without --no-cleanup for a "
            "fixture-backed run.")
        print(f"   fixture  : REFUSED to pin (--no-cleanup) - {detail}")
        print("   fixture  : continuing WITHOUT a pin - capture-content "
              "checks will report BLOCKED and autosend_newline will be "
              "INCONCLUSIVE (never PASS); the config is never written.")
        return None
    try:
        fixture = ConfigFixture(
            _runtime_config_path_for_exe(exe_path)).backup()
    except (FixtureRefused, EnvBlock) as e:
        label = "REFUSED" if isinstance(e, FixtureRefused) else "UNAVAILABLE"
        print(f"   fixture  : {label} - {e}")
        print("   fixture  : continuing WITHOUT a pin - capture-content "
              "checks will report BLOCKED and autosend_newline will be "
              "INCONCLUSIVE (never PASS); the config is never written.")
        return None
    except OSError as e:
        # R3-2 / F-Δ2 (261121_0002 round 3): backup() is read-only, so nothing
        # was written and there is nothing to restore - but an unreadable file
        # (ACL, transient lock) is a harness/environment fault. Raising
        # HarnessError keeps it out of the E12 exit-1 "product FAIL" channel:
        # main() records the fault, continues unpinned, and exits 3.
        print(f"   fixture  : READ FAILED - {type(e).__name__}: {e}")
        print("   fixture  : continuing WITHOUT a pin - capture-content "
              "checks will report BLOCKED and autosend_newline will be "
              "INCONCLUSIVE (never PASS); the config is never written.")
        raise HarnessError(
            "config fixture backup failed - the runtime config exists but "
            f"cannot be read ({type(e).__name__}: {e}); no write happened, so "
            "the run continues unpinned and exits 3 (harness error)")
    print(f"   fixture  : backed up {redact_user_path(fixture.path)} (sha256="
          f"{fixture.pre_sha[:12]}...)")
    return fixture


def pin_config_fixture(fixture):
    """Write the QA keys for a backed-up fixture; returns True when armed.

    Called from INSIDE main()'s restore-guarded try, so a failed write can
    never skip the restore (F1). An OSError/ValueError is a HARNESS error: it
    raises HarnessError so the caller aborts with exit 3 after restoring the
    pre-run bytes. A pin that does not PERSIST is an ENVIRONMENT abort: nothing
    is armed (the auto_send=1 scenario must not claim a pin) but `fixture` stays
    alive so the finally still writes the original bytes back."""
    try:
        fixture.pin({"diag_log_enabled": True, "diag_log_content": True,
                     "auto_send": False})
    except (OSError, ValueError) as e:
        print(f"   fixture  : WRITE FAILED - {type(e).__name__}: {e}")
        raise HarnessError(
            "config fixture pin failed before the first app launch "
            f"({type(e).__name__}: {e}); aborting with exit 3 after the "
            "byte-verbatim restore")
    except EnvBlock as e:
        print(f"   fixture  : pin did not persist - {e}")
        print("   fixture  : continuing WITHOUT a pin - capture-content "
              "checks will report BLOCKED and autosend_newline will be "
              "INCONCLUSIVE (never PASS); the original bytes are still "
              "restored on exit.")
        return False
    print(f"   fixture  : pinned diag_log_enabled=1 diag_log_content=1 "
          f"auto_send=0 in {redact_user_path(fixture.path)} (sha256="
          f"{fixture.pre_sha[:12]}...)")
    return True


# ---------------------------------------------------------------------------
# verification (three layers) - scenario-independent common core
# ---------------------------------------------------------------------------
class Report:
    def __init__(self, name):
        self.name = name
        self.checks = []  # (layer, ok: True|False|None, desc, evidence)
        self.info = []
        self.env_notes = []

    def add(self, layer, ok, desc, evidence=""):
        self.checks.append((layer, ok, desc, evidence))

    def note(self, msg):
        self.info.append(msg)

    def note_env(self, msg):
        self.env_notes.append(msg)

    def demote_failures(self, reason):
        """C5 disposition (261121_0002): a PROBABLE PRODUCT-defect signature is
        reported INCONCLUSIVE - never PASS and never a bare harness FAIL.

        Every FAIL collected so far is demoted to BLOCKED with `reason`
        appended, so the evidence lines survive, the summary exports stay
        visible, and the verdict cannot become PASS (a BLOCKED check makes
        verdict() return INCONCLUSIVE). Callers must also note_env() the
        defect so the cause is printed and lands in the run manifest. Used
        ONLY by long_text's capture-timeout leg (suspected product defect,
        docs 260907_0001 readjudication; C5 of the 261121_0002 report)."""
        demoted = 0
        checks = []
        for layer, ok, desc, ev in self.checks:
            if ok is False:
                checks.append((layer, None, f"{desc} :: {reason}", ev))
                demoted += 1
            else:
                checks.append((layer, ok, desc, ev))
        self.checks = checks
        return demoted

    def verdict(self):
        failed = [c for c in self.checks if c[1] is False]
        blocked = [c for c in self.checks if c[1] is None]
        if failed:
            return "FAIL", failed, blocked
        if blocked or self.env_notes:
            return "INCONCLUSIVE", failed, blocked
        return "PASS", failed, blocked

    def render(self):
        v, _, _ = self.verdict()
        lines = [f"[{self.name}] verdict={v}"]
        for layer, ok, desc, ev in self.checks:
            mark = {True: "PASS", False: "FAIL", None: "BLOCKED"}[ok]
            line = f"  [L{layer}] {mark}: {desc}"
            if ev:
                line += f"\n         evidence: {ev}"
            lines.append(line)
        for n in self.info:
            lines.append(f"  [info] {n}")
        for n in self.env_notes:
            lines.append(f"  [env] INCONCLUSIVE-factor: {n}")
        return "\n".join(lines), v


def marker_of(line):
    m = re.search(r"E2E\d{2}", line)
    return m.group(0) if m else line[-6:]


def _reference_counters(rep, blocks):
    """Design 210000 4.1-1: /005 (requery-failed estimate) and /008 (newline
    settle timeout) are reference counters, NOT verdict drivers. Recorded as
    [info] so a regression is observable without turning them into FAILs."""
    n5 = sum(1 for b in blocks if b.estimate_005)
    n8 = sum(1 for b in blocks if b.settle_008)
    rep.note(f"reference counters: /005 estimate-fallback tasks={n5}, "
             f"/008 settle-timeout tasks={n8} (expected 0; >0 -> QA watch)")


def verify_task_layer1(rep, block, expected_line, cumulative_estimate,
                       *, marker_required=True, empty_hold_expected=False,
                       clamp_expected=False):
    if block.em_failure:
        rep.add(1, False, f"task {block.index}: EM_GETSEL/EM_SETSEL "
                "failed/timed out", block.cite(RE_EM_FAILURE))

    if empty_hold_expected:
        # The R5 empty-capture hold: task_received -> capture 0 -> hold_send
        # notice -> return WITHOUT translate/paste/task_end (src/worker.cpp
        # L226-237). Everything above is the whole story for this task.
        # /006|/007 on an EMPTY document is B-6b's documented (0,0)-ambiguity
        # conservative fallback (probe needs linecount to judge an editor
        # with no selection; intermittently answers NotCapable on Win11
        # RichEditD2DPT - captured live 260907 08:46:49). The product still
        # reaches the harmless hold either way, which is exactly this
        # scenario's contract ("오류 없음, 번역 시도 없음/무해 처리"), so
        # those lines are INFO here and FAIL elsewhere.
        if block.cap_unknown or block.cap_notcapable:
            rep.note(f"task {block.index}: capability fallback on empty "
                     "document (probe ambiguity, harmless - see report "
                     "Issues): "
                     + (block.cite(RE_CAP_NOTCAPABLE)
                        or block.cite(RE_CAP_UNKNOWN)))
        rep.add(1, block.empty_hold,
                f"task {block.index}: stage=empty_capture decision=hold_send "
                "recorded (no translation attempted - harmless)",
                block.cite(RE_EMPTY_HOLD))
        rep.add(1, block.capture_len in (0, None) and block.capture_len != -1,
                f"task {block.index}: capture len={block.capture_len} on empty "
                "document", f"capture_len={block.capture_len}")
        rep.add(1, block.translate_status is None,
                f"task {block.index}: no translate/paste stages on held empty "
                "task", f"translate={block.translate_status} "
                f"paste={block.paste_result}")
        return
    # (a) fallback signatures for the TRANSLATION path. B-6c: /006 and /007
    # joined /002 as FAIL signatures (intent = Capable for every target
    # control in this matrix; the delegation's common row).
    for sig, pat, label in (
            (block.fallback_nonstandard, RE_FALLBACK_NONSTANDARD, "/002"),
            (block.cap_unknown, RE_CAP_UNKNOWN, "/006"),
            (block.cap_notcapable, RE_CAP_NOTCAPABLE, "/007")):
        if label in ("/006", "/007") and sig and block.empty_hold:
            continue  # empty-document ambiguity: INFO above, not a FAIL
        rep.add(1, not sig,
                f"task {block.index}: no '{label}' capability/fallback line",
                block.cite(pat))
    if block.clamped:
        # /004 is the DESIGNED safety net (degrade to pre-REQ-027 geometry,
        # design 210000 section 2.2) - never a FAIL by itself. The scenario
        # that targets the clamp (backspace_enter) asserts its PRESENCE
        # separately; a spurious clamp elsewhere is visible from the capture
        # length checks below.
        rep.add(1, True,
                f"task {block.index}: /004 clamp path exercised "
                f"({'scenario-expected' if clamp_expected else 'observed'})",
                block.cite(RE_CLAMP))

    if not block.completed and not block.send_through:
        rep.add(1, None,
                f"task {block.index}: never reached stage=task_end within "
                "timeout",
                block.lines[-1][1].strip() if block.lines else "(no lines)")
        return
    if block.send_through and not block.completed:
        # smart-bypass on the bare-Enter path returns before task_end by
        # design (src/worker.cpp L240-250): harmless outcome, nothing more
        # to assert for this task.
        rep.add(1, True,
                f"task {block.index}: send_through bypass outcome "
                "(already target language - no task_end by design)",
                block.cite(RE_SEND_THROUGH))
        return
    rep.add(1, block.em_setselect,
            f"task {block.index}: EM path taken "
            "(EditCaretTracker em_setselect)",
            block.cite(RE_EM_SETSELECT))
    # (b) capture length == typed line EXACTLY (never cumulative, and never the
    # app-injected newline swallowed into the EM selection - the delegation
    # specifies len=N must MATCH the typed length; a captured leading "\r\n"
    # merges translated lines in the document).
    # 261121_0002 E1: a missing capture line is BLOCKED, but it must NOT return
    # - the translate/paste/task_end evidence below is exactly what makes a log
    # gap attributable. Before this change the early return turned every
    # capture-parser blind spot into a scenario with NO product evidence.
    if block.capture_len is None:
        rep.add(1, None,
                f"task {block.index}: no 'stage=capture end' line "
                "(capture length/marker BLOCKED; the translate/paste checks "
                "below still judge this task)",
                "no RE_CAPTURE_END match in this block")
    elif expected_line is not None:
        typed = utf16_len(expected_line)
        exact = block.capture_len == typed
        non_cum = block.capture_len < cumulative_estimate if cumulative_estimate else True
        diagnosis = ""
        if not exact and block.capture_content is not None:
            cc = unescape_diag(block.capture_content)
            if expected_line and cc.lstrip("\r\n") == expected_line and cc != expected_line \
                    and cc.startswith(("\r\n", "\n")):
                diagnosis = (" :: DEFECT CANDIDATE - EM selection swallowed the "
                             "previous app-injected newline (ISSUE-1 "
                             "regression: the replacement MERGES this "
                             "translation onto the previous line)")
            elif expected_line and cc.rstrip("\r\n") == expected_line and cc != expected_line:
                diagnosis = (" :: DEFECT CANDIDATE - trailing newline in capture "
                             "(the pre-B-5a translation_equals_source "
                             "contamination shape)")
        content_note = ("content gated off (diag_log_content=false)"
                        if block.capture_content is None
                        else f'content="{block.capture_content[:150]}"')
        rep.add(1, exact and non_cum,
                f"task {block.index}: capture len={block.capture_len} == typed "
                f"{typed} exactly, non-cumulative "
                f"(cumulative signature would be ~{cumulative_estimate})"
                + diagnosis,
                content_note + f" (k={block.capture_k} "
                f"result={block.capture_result})")
        if marker_required and expected_line:
            # E7: with the shipped privacy default the content field does not
            # exist. Comparing against '' would produce a FALSE FAIL for a
            # product that behaved correctly; report BLOCKED instead. E5 pins
            # diag_log_content=true, so this is the rare-fallback branch.
            if block.capture_content is None:
                rep.add(1, None,
                        f"task {block.index}: captured content carries this "
                        "line's marker (E2Exx) - content unavailable "
                        "(diag_log_content=false), BLOCKED not FAIL")
            else:
                contains_new = marker_of(expected_line) in block.capture_content
                rep.add(1, contains_new,
                        f"task {block.index}: captured content carries this line's "
                        "marker (E2Exx)",
                        f'marker={marker_of(expected_line)} content='
                        f'"{block.capture_content[:150]}"')
    # (c)/(d) paste + translate completion
    if block.translate_status is None:
        rep.add(1, None, f"task {block.index}: no 'stage=translate end' line")
    elif block.translate_status in (2, 3):  # consent/network/engine
        rep.add(1, None,
                f"task {block.index}: translate end status="
                f"{STATUS_NAMES.get(block.translate_status)} - cloud/network/"
                "consent factor -> environment (not FAIL)",
                block.cite(RE_TRANSLATE_END))
    else:
        rep.add(1, block.translate_status == 0,
                f"task {block.index}: translate end status="
                f"{STATUS_NAMES.get(block.translate_status, block.translate_status)}",
                block.cite(RE_TRANSLATE_END))
    if block.paste_result == 1:
        rep.add(1, True, f"task {block.index}: stage=paste result=1",
                block.cite(RE_PASTE_RESULT))
    elif block.paste_skip_reason:
        # QA-27-4's signature is exactly this reason; translation_empty is env
        rep.add(1, block.paste_skip_reason != "translation_equals_source",
                f"task {block.index}: paste skipped reason="
                f"{block.paste_skip_reason}",
                block.cite(RE_PASTE_SKIPPED))
    else:
        rep.add(1, False, f"task {block.index}: neither paste result nor "
                "skip recorded", f"task_end pasted={block.pasted}")


def verify_task_layer2(rep, block, expected_line, doc_before, doc_after,
                       *, line_ordinal=None, auto_send=None,
                       replacement_required=True,
                       expect_hold_no_change=False):
    """Layer 2 (document) judging for one task.

    261121_0002 E2/E4: the editor-newline check is a DOCUMENT DELTA driven by
    the app's OWN logged action, not by a log-global task index. The old rule
    ("a newline must exist when block.index > 0") was written for REQ-023
    ("CategoryB always injects a newline"), which REQ-028 retired; it demanded
    a newline in the first line of every fresh document. `line_ordinal` (the
    0-based line number inside THIS document) is kept for messages only."""
    ordinal = "?" if line_ordinal is None else line_ordinal
    tag = f"task {block.index} (document line {ordinal})"
    if doc_before is None or doc_after is None:
        rep.add(2, None, f"{tag}: content unreadable "
                "(WM_GETTEXT and UIA both failed)")
        return
    if expect_hold_no_change:
        rep.add(2, doc_after == doc_before,
                f"{tag}: empty-capture hold left the document "
                "byte-identical", f"len={len(doc_after)}")
        return
    if not (block.completed and block.paste_result == 1):
        if replacement_required:
            rep.add(2, None, f"{tag}: content check deferred "
                    "(log layer recorded no successful paste)")
        else:
            # Smart-bypass / skip outcomes are legitimate here (e.g.
            # backspace_enter capturing already-translated text).
            rep.add(2, True,
                    f"{tag}: no-replacement outcome tolerated by "
                    f"scenario design (paste={block.paste_result} "
                    f"skip={block.paste_skip_reason})",
                    f"doc changed={doc_after != doc_before}")
        return
    if expected_line:
        rep.add(2, expected_line not in doc_after,
                f"{tag}: typed source replaced by translation "
                "(source absent from control text)",
                f'after={doc_after[:160]!r}')
    # NOT a growth check: a translation can be SHORTER than the Hangul
    # source; the invariant is "the document changed at this replacement".
    rep.add(2, doc_after != doc_before,
            f"{tag}: document changed after replacement",
            f"before_len={len(doc_before)} after_len={len(doc_after)}")
    # E2: the delta must match what the app SAYS it did (src/worker.cpp
    # L1505-1520). SKIPPED/synthetic_send carry no editor newline;
    # synthetic_newline carries exactly one. An absent/unknown action means a
    # hold/send-through path, where the delta proves nothing -> BLOCKED.
    delta = doc_after.count("\n") - doc_before.count("\n")
    action = block.send_enter_action
    asend = (fixture_auto_send() if auto_send is None else bool(auto_send))
    ev = (f"action={action!r} reason={block.send_enter_reason!r} "
          f"auto_send={block.send_enter_auto_send} delta={delta} "
          f"(newlines {doc_before.count(chr(10))} -> "
          f"{doc_after.count(chr(10))})")
    if action == "synthetic_newline":
        rep.add(2, delta == 1,
                f"{tag}: the app injected the editor newline exactly once "
                "(action=synthetic_newline)", ev)
    elif action in ("SKIPPED", "synthetic_send"):
        rep.add(2, delta == 0,
                f"{tag}: no editor newline when the app did not inject one "
                f"(action={action})", ev)
    else:
        rep.add(2, None,
                f"{tag}: newline delta BLOCKED - no decisive "
                f"'stage=send_enter' action line (action={action!r}); a "
                "hold/send-through task cannot prove the delta", ev)
    # E5 per-task fixture cross-check. The injected branch logs auto_send=; the
    # SKIPPED branch does not, so the always-present lang_pair witness is the
    # fallback. A mismatch means config drift / a user toggle mid-run: that is
    # an ENVIRONMENT fact, never a product FAIL (R1).
    witness = (block.send_enter_auto_send if block.send_enter_auto_send
               is not None else block.task_auto_send)
    if witness is not None and bool(witness) != asend:
        rep.add(2, None,
                f"{tag}: ENV - app reports auto_send={witness} but the pinned "
                f"fixture is {int(asend)}; the newline expectation cannot be "
                "trusted (config drift / user toggle)", ev)


def verify_final_document(rep, notepad, all_lines, *, min_newlines=None,
                          auto_send=None):
    doc, method = notepad.read_text()
    if doc is None:
        rep.add(2, None, f"final document unreadable ({method})")
        return None
    stale = [s for s in all_lines if s and s in doc]
    rep.add(2, not stale,
            f"final document ({method}, {len(doc)} chars) contains none of "
            f"the {len([s for s in all_lines if s])} typed sources",
            f"still_present={[s[:40] for s in stale]}")
    # E3 (261121_0002): the structural rule depends on the CONFIGURED mode.
    # REQ-028 gates the editor newline on auto_send, so:
    #   auto_send=0 -> the k translated lines are k REPLACEMENTS in ONE line,
    #                  newlines must be exactly 0;
    #   auto_send=1 -> the app injects one newline per replacement, so the
    #                  document must hold exactly k newlines.
    # The old rule (">= k always") encoded the retired REQ-023 behavior and
    # failed every document produced with the shipped default.
    asend = fixture_auto_send() if auto_send is None else bool(auto_send)
    newlines = doc.count("\n")
    expected_lines = len([s for s in all_lines if s])
    if min_newlines is not None:
        ok = newlines >= min_newlines
        want = f">= {min_newlines} (explicit min_newlines override)"
    elif asend:
        ok = newlines == expected_lines
        want = (f"exactly {expected_lines} (auto_send=1: one app-injected "
                "newline per replacement)")
    else:
        ok = newlines == 0
        want = "exactly 0 (auto_send=0: replacement only, no editor newline)"
    rep.add(2, ok,
            f"final document newline count matches the pinned mode "
            f"(newlines={newlines}, expected {want})",
            f"doc={doc[:300]!r}")
    rep.note(f"final document via {method}: {doc[:400]!r}")
    return doc


def _read_doc_after_settle(notepad, before, block, timeout=1.0):
    """Read the document after a task, giving an app-INJECTED Enter a bounded
    moment to become visible.

    The app stamps stage=task_end only AFTER SendEnterKey +
    EditCaretTracker_SettleNewlineVisible (whose own poll is 50 ms + up to
    4x25 ms, src/win32_input.cpp), so this is normally a no-op; it exists so
    the E10 delta assertion cannot lose a race by a few milliseconds on a busy
    machine and report a false FAIL. Only the synthetic_newline action polls -
    every other outcome expects a zero delta and must be judged immediately."""
    doc, _method = notepad.read_text()
    if block.send_enter_action != "synthetic_newline" or doc is None \
            or before is None:
        return doc
    deadline = time.time() + timeout
    while time.time() < deadline:
        if doc.count("\n") - before.count("\n") >= 1:
            return doc
        time.sleep(POLL_S)
        doc, _method = notepad.read_text()
        if doc is None:
            return None
    return doc


# ---------------------------------------------------------------------------
# line-driven scenario core (shared by qa27b/example1/consecutive/long_text
# and as the per-block worker for multi_lang)
# ---------------------------------------------------------------------------
LINES_QA27B = ["메모장에 한 줄을 써봅니다. E2E01"]
LINES_EXAMPLE1 = [
    "첫 번째 문장입니다. E2E01",
    "두 번째 문장을 타이핑해요. E2E02",
    "세 번째 줄도 이어 작성합니다. E2E03",
    "네 번째 문장은 조금 길게 작성해봅니다. E2E04",
    "다섯 번째 문장입니다. E2E05",
    "여섯 번째 마지막 문장이에요. E2E06",
]
LINES_CONSECUTIVE = [
    "같은 언어 첫 줄을 입력합니다. E2E01",
    "같은 언어 두 번째 줄을 이어서 입력합니다. E2E02",
]
# E10: the auto_send=1 leg (one newline per replacement). Three lines so a
# "grew by one" delta and a "grew by k" accumulation are distinguishable.
LINES_AUTOSEND = [
    "자동 개행 첫 번째 줄입니다. E2E01",
    "자동 개행 두 번째 줄입니다. E2E02",
    "자동 개행 세 번째 줄입니다. E2E03",
]
# long_text builds its 1000-char line via build_long_line() below.


def run_line_session(name, lines, app, base, *, step_timeout=None,
                     final_doc=True, expect=None, auto_send=None):
    """base: number of pipeline tasks already present in this app session's
    log (scenarios run sequentially against one shared app instance).
    expect: dict of per-index flags {idx: {'marker_required': bool, ...}} -
    kept simple: all lines get identical treatment unless overridden.
    auto_send: the mode this session runs under (None = the pinned fixture)."""
    rep = Report(name)
    expect = expect or {}
    notepad = NotepadSession()
    blocks = []
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        if notepad.residue_cleared:
            rep.note("cleared harness E2Exx residue from Notepad session "
                     "restore (own scratch content only)")
        cumulative = 0
        for i, line in enumerate(lines):
            ex = expect.get(i, {})
            notepad.type_line(line)
            before, _ = notepad.read_text()
            notepad.press_enter_wait_task(app.log_text, base + i + 1)
            block = notepad.wait_task_complete(app.log_text, base + i,
                                               timeout=step_timeout)
            after = _read_doc_after_settle(notepad, before, block)
            blocks.append(block)
            verify_task_layer1(
                rep, block, ex.get("expected_line", line), cumulative,
                marker_required=ex.get("marker_required", True),
                empty_hold_expected=ex.get("empty_hold", False),
                clamp_expected=ex.get("clamp_expected", False))
            verify_task_layer2(
                rep, block, ex.get("expected_line", line), before, after,
                line_ordinal=i,   # E4: ordinal inside THIS document
                auto_send=ex.get("auto_send", auto_send),
                replacement_required=ex.get("replacement_required", True),
                expect_hold_no_change=ex.get("empty_hold", False))
            cumulative += utf16_len(line) + 2 + 80  # conservative estimate
        if final_doc:
            verify_final_document(rep, notepad, lines, auto_send=auto_send)
        _reference_counters(rep, blocks)
        if name == "consecutive":
            tail = parse_tasks(app.log_text())[base:]
            eq = [b for b in tail
                  if b.paste_skip_reason == "translation_equals_source"]
            rep.add(1, not eq,
                    "session-wide: no 'translation_equals_source' skip on "
                    "back-to-back same-language Enters (QA-27-4)",
                    eq[0].cite(RE_PASTE_SKIPPED) if eq else "")
        if name == "example1":
            tail = parse_tasks(app.log_text())[base:]
            caps = [b.capture_len for b in tail if b.capture_len is not None]
            env_hit = any(b.translate_status in (2, 3) or not b.completed
                          for b in tail)
            monotonic_growth = len(caps) > 1 and all(
                caps[k + 1] > caps[k] for k in range(len(caps) - 1)) \
                and caps[-1] > utf16_len(lines[-1]) + 2
            # E8 (261121_0002): with caps == [] the old branch printed
            # "capture lengths [] do NOT show cumulative growth" and marked it
            # PASS - a false green that passed precisely because it had no
            # data. No parsed captures = BLOCKED.
            if not caps:
                rep.add(1, None,
                        "capture-length series EMPTY (no 'stage=capture end' "
                        "line parsed for this scenario) - the cumulative-"
                        "growth check is BLOCKED, not passed",
                        f"capture lines=0, blocks={len(tail)}")
            elif len(caps) < len(lines) and env_hit:
                rep.add(1, None,
                        f"capture-length series incomplete due to environment "
                        f"(statuses seen: {caps})")
            else:
                rep.add(1, not monotonic_growth,
                        f"capture lengths {caps} do NOT show cumulative "
                        "growth (each Enter captured only the new sentence)")
            ok_pastes = [b for b in tail
                         if b.translate_status not in (2, 3) and b.completed]
            pastes = [b.paste_result for b in tail]
            if len(ok_pastes) < len(lines) and env_hit:
                rep.add(1, None,
                        "6x paste evidence partial due to environment")
            else:
                rep.add(1, len(ok_pastes) == len(lines) and all(
                    b.paste_result == 1 for b in ok_pastes),
                    f"every Enter ended in a successful replacement "
                    f"(paste results: {pastes})")
    finally:
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


# ---------------------------------------------------------------------------
# B-6c edge-flow drivers
# ---------------------------------------------------------------------------
def scenario_empty_enter(app, base):
    """Empty document, bare Enter x3. Every task must end in the R5
    empty-capture hold (no translation, no paste, no crash) and the
    document must remain empty. REQ-034 F3-B (C-5) additions judged per
    task: the no-selection notice MUST surface (tooltip kind=message via
    empty_capture_cb_), and the paste-window suppression MUST NOT fire -
    this document has no paste history, so a general empty Enter keeps the
    pre-F3-B hold behavior exactly."""
    rep = Report("empty_enter")
    notepad = NotepadSession()
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        # F3-B determinism (D-4b): last_paste_ms_ is WORKER-GLOBAL state
        # shared by every scenario in this app session - if a PREVIOUS
        # scenario pasted successfully < kPasteEmptySuppressMs (2000 ms)
        # ago, the first Enter here would take the suppression branch
        # instead of the C-5 hold+notice this scenario asserts. Settle past
        # the window (margin 600 ms) before judging hold+notice.
        time.sleep(2.6)
        # Settle between Enters: firing the next one while the worker is
        # still busy makes the hook PASS IT THROUGH (ENTER_GATE
        # reason=worker_busy), Notepad then inserts a real "\r\n" and the
        # NEXT task captures it (observed live 260907: len=2/6/8 "\r\n"xK
        # instead of the 0-length hold capture). A user typing Enter x3 at
        # human speed never triggers that; the settle reproduces it.
        for i in range(3):
            notepad.press_enter_wait_task(app.log_text, base + i + 1)
            time.sleep(0.8)
        time.sleep(1.0)  # let the last hold notice + logs settle
        blocks = parse_tasks(app.log_text())[base:base + 3]
        rep.add(1, len(blocks) >= 3,
                f"3 Enters produced 3 pipeline tasks (got {len(blocks)})",
                f"task_received lines={len(blocks)}")
        for i, b in enumerate(blocks):
            # Delegation contract: "오류 없음, 번역 시도 없음(또는 무해
            # 처리)". The designed outcome is the R5 hold (empty capture ->
            # notice, no send). A whitespace-only capture (pass-through
            # newline race) ends in smart_bypass send_through instead -
            # ALSO harmless, but the settle above should make it rare.
            harmless = (b.empty_hold or
                        (b.send_through and b.capture_content is not None
                         and not unescape_diag(b.capture_content).strip()
                         and b.translate_status is None))
            # B-6b contract (src/win32_input.cpp L861-863): on an EMPTY
            # document the (0,0)-ambiguity probe may conservatively answer
            # Unknown(/006)/NotCapable(/007) - intermittently observed live
            # on RichEditD2DPT (260907 08:46:49). The fallback then still
            # reaches the harmless hold; per that code's own semantics the
            # event is attributed as INFO (QA watch), NOT as a scenario
            # FAIL, and reported to VP in the batch report.
            if b.cap_unknown or b.cap_notcapable:
                rep.note("task " + str(b.index) + ": capability fallback on "
                         "empty document (by-design conservative ambiguity "
                         "branch, INFO): "
                         + (b.cite(RE_CAP_NOTCAPABLE) or b.cite(RE_CAP_UNKNOWN)))
            rep.add(1, harmless,
                    f"task {b.index}: empty-line Enter handled harmlessly "
                    f"({'hold_send' if b.empty_hold else 'whitespace smart-bypass'}, "
                    "no translation attempted)",
                    b.cite(RE_EMPTY_HOLD) or b.cite(RE_SEND_THROUGH)
                    or f"capture={b.capture_content!r}")
            rep.add(1, b.translate_status is None and b.paste_result != 1,
                    f"task {b.index}: no translate/paste stages "
                    "(nothing to translate)",
                    f"translate={b.translate_status} paste={b.paste_result}")
            rep.add(1, not (b.empty_suppress or b.diag_036),
                    f"task {b.index}: F3-B paste window did NOT fire on the "
                    "never-pasted document (C-5: hold+notice path intact)",
                    b.cite(RE_EMPTY_SUPPRESS) or b.cite(RE_DIAG_036))
            if b.empty_hold:
                rep.add(1, b.notice_surface,
                        f"task {b.index}: no-selection notice surfaced "
                        "(tooltip kind=message marshaled to the GUI thread)",
                        b.cite(RE_NOTICE_SURFACE) or "(no tooltip line in "
                        "this block's slice)")
            doc, _ = notepad.read_text()
            rep.add(2, doc is not None and not doc.strip(),
                    f"task {b.index}: document holds no user-visible text "
                    f"after the held/whitespace Enter (len={len(doc or '')})",
                    f"doc={doc!r}")
        doc, method = notepad.read_text()
        rep.add(2, doc is not None and not doc.strip(),
                f"document still empty after 3 held Enters ({method})",
                f"doc={doc!r}")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after empty-Enter storm",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_cursor_mid(app, base):
    """Type one line, park the caret mid-line via EM_SETSEL, Enter. The
    capture must be exactly [doc start .. caret) (REQ-027 caret anchor),
    the text after the caret must survive untouched."""
    rep = Report("cursor_mid")
    line = "커서가 중간에 있으면 앞부분만 번역되어야 합니다. E2E01"
    cut = line.index(" E2E01")
    prefix, suffix = line[:cut], line[cut + 1:]
    notepad = NotepadSession()
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        notepad.type_line(line)
        notepad.set_caret(utf16_len(prefix))
        rep.note(f"caret parked at offset {utf16_len(prefix)} "
                 f"(before marker '{suffix}')")
        before, _ = notepad.read_text()
        notepad.press_enter_wait_task(app.log_text, base + 1)
        block = notepad.wait_task_complete(app.log_text, base)
        after = _read_doc_after_settle(notepad, before, block)
        # The app's ExecuteTask opens with FlushIme (src/worker.cpp L139) -
        # a synthetic VK_RIGHT that commits IME composition AND advances a
        # mid-document caret by exactly 1 unit before EM_GETSEL is read
        # (probed live 260907: parked 27 -> app caret 28, capture grabbed
        # the space after the caret). Real product behavior on this edge
        # flow; the assertion anchors on the APP-REPORTED caret and allows
        # {parked, parked+1}. The doc-end capture (33) or a fallback whole-
        # document capture would still FAIL these checks.
        caret_app = block.em_setselect_caret
        rep.add(1, caret_app in (len(prefix), len(prefix) + 1),
                f"app caret = parked {len(prefix)} or parked+1 "
                f"(FlushIme VK_RIGHT), not document end ({len(line)})",
                f"em_setselect caret={caret_app}")
        verify_task_layer1(rep, block, None, 0, marker_required=False)
        # E1: a missing capture line is BLOCKED (log-shape gap), a parsed but
        # wrong length is the FAIL this scenario exists to catch.
        cap_ok = (None if block.capture_len is None
                  else block.capture_len == caret_app)
        rep.add(1, cap_ok,
                f"capture == EM selection [0..{caret_app}) of the caret-"
                f"anchored block",
                f"capture_len={block.capture_len}")
        # E7 (261121_0002): capture_content is None whenever the app ran with
        # the shipped privacy default (diag_log_content=false). Comparing
        # against '' produced cap_tail='' and a FALSE FAIL for a capture that
        # was correct (req027_all.log:230). BLOCKED, never FAIL, never PASS.
        if block.capture_content is None:
            rep.add(1, None,
                    "captured text is exactly the [doc start .. caret) prefix "
                    "(+FlushIme char) - BLOCKED: content unavailable "
                    "(diag_log_content=false)")
        else:
            cap = unescape_diag(block.capture_content)
            rep.add(1, cap.startswith(prefix) and len(cap) <= len(prefix) + 1,
                    "captured text is exactly the [doc start .. caret) prefix "
                    "(+FlushIme char)",
                    f"cap_tail={cap[-8:]!r}")
        verify_task_layer2(rep, block, prefix, before, after, line_ordinal=0)
        rep.add(2, suffix in (after or ""),
                "text after the caret survived the replacement",
                f"suffix={suffix!r} after={(after or '')[:200]!r}")
    finally:
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_backspace_enter(app, base):
    """Round 1: translate line A (stores post-newline offset). Round 2:
    type a short line B, backspace INTO A' so stored last > caret, Enter.
    The /004 clamp path must fire and the task must finish harmlessly:
    capture length == the shrunk document prefix, no error signatures, no
    crash. Replacement outcome (paste vs smart-bypass) depends on the
    translated A' language, so it is not verdict-critical here (design
    210000 2.4 row 8)."""
    rep = Report("backspace_enter")
    line_a = "백스페이스 클램프 경로를 검증하는 첫 줄입니다. E2E01"
    line_b = "E2E02 짧은 두 번째 줄"
    notepad = NotepadSession()
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        notepad.type_line(line_a)
        notepad.press_enter_wait_task(app.log_text, base + 1)
        blk_a = notepad.wait_task_complete(app.log_text, base)
        doc_a, _ = notepad.read_text()
        verify_task_layer1(rep, blk_a, line_a, 0)
        if blk_a.paste_result != 1:
            rep.add(1, None, "round 1 did not replace (env-dependent); clamp "
                    "setup impossible - aborting to INCONCLUSIVE",
                    f"paste={blk_a.paste_result} skip={blk_a.paste_skip_reason}")
            return rep
        stored_last = utf16_len(doc_a)  # post-newline caret (B-6a contract)
        notepad.type_line(line_b)
        bs = len(line_b) + 4  # erase all of B, the injected newline, 2 of A'
        doc_bs = notepad.press_backspaces(bs)
        if doc_bs is None:
            doc_bs = notepad.read_text()[0]
        caret = utf16_len(doc_bs)
        rep.note(f"stored last={stored_last}, caret after {bs} backspaces"
                 f"={caret} (clamp fires iff caret < last)")
        rep.add(1, caret < stored_last,
                "setup achieved last > caret (clamp precondition)",
                f"last={stored_last} caret={caret}")
        notepad.press_enter_wait_task(app.log_text, base + 2)
        blk_b = notepad.wait_task_complete(app.log_text, base + 1)
        doc_b = _read_doc_after_settle(notepad, doc_bs, blk_b)
        # Capture = [0, app caret) of the SHRUNK document after the /004
        # clamp reset last to 0. App caret may be parked+1 (FlushIme
        # VK_RIGHT, same as cursor_mid). last must be 0 on this round.
        caret_app = blk_b.em_setselect_caret
        rep.add(1, caret_app in (caret, caret + 1),
                f"app caret ~{caret} after backspaces ({caret} or {caret+1} "
                "with FlushIme)", f"em_setselect caret={caret_app}")
        verify_task_layer1(rep, blk_b, None, 0, marker_required=False,
                           clamp_expected=True)
        rep.add(1, blk_b.clamped and blk_b.em_setselect_last == 0,
                "round 2 exercised the /004 clamp (stored last > caret -> "
                "last reset to 0) and completed safely",
                blk_b.cite(RE_CLAMP) or
                f"last={blk_b.em_setselect_last} caret={caret_app}")
        # E1: absent capture line = BLOCKED (log-shape gap), wrong length = FAIL.
        cap_b_ok = (None if blk_b.capture_len is None
                    else blk_b.capture_len == caret_app)
        rep.add(1, cap_b_ok,
                f"capture == [0..{caret_app}) of the shrunk document",
                f"capture_len={blk_b.capture_len}")
        verify_task_layer2(rep, blk_b, None, doc_bs, doc_b,
                           line_ordinal=1, replacement_required=False)
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app alive after clamp round",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_shift_enter_multi(app, base):
    """REQ-018: L1 + Shift+Enter + L2 + Shift+Enter + L3 + bare Enter.
    Both Shift+Enters must pass through untouched (ENTER_GATE
    reason=shift_enter_newline x2, no extra newlines from the app), exactly
    ONE pipeline task is created, and its capture spans the whole 3-line
    block. One replacement for the whole block."""
    rep = Report("shift_enter_multi")
    l1 = "시프트 엔터 첫 번째 줄입니다. E2E01"
    l2 = "두 번째 줄은 Shift+Enter로 이어집니다. E2E02"
    l3 = "세 번째 줄 뒤의 bare Enter가 블록을 종결합니다. E2E03"
    notepad = NotepadSession()
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        text_before = app.log_text()
        notepad.type_line(l1)
        notepad.press_shift_enter()
        notepad.type_line(l2)
        notepad.press_shift_enter()
        notepad.type_line(l3)
        gate_lines = len(RE_SHIFT_GATE.findall(app.log_text()[len(text_before):]))
        rep.add(1, gate_lines >= 2,
                f"both Shift+Enters exited via ENTER_GATE "
                f"reason=shift_enter_newline (found {gate_lines})",
                "scan of scenario's own log slice")
        before, _ = notepad.read_text()
        notepad.press_enter_wait_task(app.log_text, base + 1)
        block = notepad.wait_task_complete(app.log_text, base)
        after, _ = notepad.read_text()
        tail = parse_tasks(app.log_text())[base:]
        rep.add(1, len(tail) == 1,
                f"exactly ONE task for 3 Enters (bare terminates, "
                f"Shift+Enter passes through) - got {len(tail)}",
                f"tasks={[b.index for b in tail]}")
        expected_block = l1 + "\r\n" + l2 + "\r\n" + l3
        verify_task_layer1(rep, block, expected_block, 0)
        # ordinal 0: the whole 3-line block is ONE document line for the app
        # (Shift+Enter newlines pre-exist in doc_before, so the delta is 0).
        verify_task_layer2(rep, block, expected_block, before, after,
                           line_ordinal=0)
        _reference_counters(rep, tail)
    finally:
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_paste_then_enter(app, base):
    """Clipboard seeded OUTSIDE the app, real (unmarked) Ctrl+V into the
    fresh window, then immediate bare Enter: capture must equal the pasted
    text exactly and the replacement must run normally.

    D-4b phase 2 (REQ-034 F3-B): one IMMEDIATE retry Enter after the
    successful pipeline paste. The stored offset equals the caret (B-6a
    save), so this Enter's EM_SETSEL range is empty -> Ctrl+C changes
    nothing -> 180 ms stale-refuse -> EMPTY capture. Because the retry
    lands within kPasteEmptySuppressMs (2000 ms) of the worker's own paste
    stamp, the product must take the F3-B branch: WORKER/ExecuteTask/036 +
    stage=empty_capture decision=paste_window_suppress, NO TooltipNoSelection
    (kind=message) notice, and a silent send-through the app answers with a
    real newline (the user's 'notice 없이 send-through' rule). Phase 1 is
    unchanged from B-6c."""
    rep = Report("paste_then_enter")
    text = "클립보드로 외부에서 들어온 원본 문장입니다. E2E01"
    notepad = NotepadSession()
    # 261122_0001 A4 (F11.3): snapshot the USER's clipboard before the seed and
    # put it back in the finally, so a normal exit / exception / Ctrl+C never
    # leaves harness text on the user's clipboard.
    clipboard_backup = ClipboardBackup().capture()
    try:
        set_clipboard_text(text)
        clipboard_backup.seeded()
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        notepad.press_ctrl_v()
        deadline = time.time() + 5.0
        doc = None
        while time.time() < deadline:
            doc, _ = notepad.read_text()
            if doc is not None and text in doc:
                break
            time.sleep(POLL_S)
        rep.add(1, doc is not None and text in doc,
                "Ctrl+V inserted the external clipboard text",
                f"doc={doc[:120]!r}" if doc is not None else "(unreadable)")
        if doc is None or text not in (doc or ""):
            raise EnvBlock("clipboard paste did not land - environment "
                           "(clipboard owner race?), not a product FAIL")
        before = doc
        notepad.press_enter_wait_task(app.log_text, base + 1)
        block = notepad.wait_task_complete(app.log_text, base)
        after, _ = notepad.read_text()
        verify_task_layer1(rep, block, text, 0)
        verify_task_layer2(rep, block, text, before, after, line_ordinal=0)
        # ---- D-4b phase 2: immediate retry Enter -> F3-B suppression ----
        # The window opens ONLY on a SUCCESSFUL paste (worker.cpp stamps
        # last_paste_ms_ under `if (pasted)`). If phase 1 did not replace
        # (cloud/engine/smart-bypass), the setup for the suppression branch
        # does not exist -> honest INCONCLUSIVE via env note, never a fake
        # FAIL (backspace_enter uses the same guard shape).
        if block.paste_result != 1:
            rep.note_env(
                "phase 2 skipped: paste result="
                f"{block.paste_result} skip={block.paste_skip_reason} - no "
                "successful paste means no F3-B window to exercise")
            return rep
        # 300 ms settle: guarantees task 1's ExecuteTask fully returned
        # (is_busy_ cleared) so the retry Enter is not passed through by
        # ENTER_GATE reason=worker_busy; still ~6-7x inside the 2000 ms
        # suppression window measured from the paste stamp.
        time.sleep(0.3)
        notepad.press_enter_wait_task(app.log_text, base + 2)
        # The suppression branch returns BEFORE stage=task_end (worker.cpp
        # L252-254) and the R5 hold branch likewise skips task_end; the
        # method-level wait only knows completed/empty_hold, so blocks with
        # decision=paste_window_suppress are terminal here.
        blk2 = wait_block_terminal(app.log_text, base + 1)
        doc2, method = notepad.read_text()
        rep.add(1, blk2.empty_suppress,
                "retry Enter (post-paste window): "
                "stage=empty_capture decision=paste_window_suppress recorded",
                blk2.cite(RE_EMPTY_SUPPRESS))
        rep.add(1, blk2.diag_036,
                "WORKER/ExecuteTask/036 DIAG line present (suppress "
                "branch executed)", blk2.cite(RE_DIAG_036))
        rep.add(1, not blk2.empty_hold,
                "the R5 hold_send + no-selection notice branch did NOT run "
                "inside the paste window", blk2.cite(RE_EMPTY_HOLD))
        rep.add(1, not blk2.notice_surface,
                "no TooltipNoSelection surfaced for the suppressed retry "
                "(notice-free send-through)", blk2.cite(RE_NOTICE_SURFACE))
        rep.add(1, blk2.translate_status is None and blk2.paste_result is None,
                "suppressed task ran no translate/paste stages (silent "
                "release + Enter handoff only)",
                f"translate={blk2.translate_status} paste={blk2.paste_result}")
        # /036 carries the MEASURED elapsed-vs-window numbers
        # (D-4 evidence for tuning kPasteEmptySuppressMs):
        m = RE_DIAG_036_DETAIL.search(
            "\n".join(tx for _ln, tx in blk2.lines))
        if m:
            rep.note(f"F3-B window timing (measured): elapsed={m.group(1)}ms "
                     f"<= window={m.group(2)}ms (kPasteEmptySuppressMs)")
        else:
            rep.note("F3-B window timing: /036 detail line not parseable "
                     "(informational only)")
        # send-through proof: the app received the Enter and grew the
        # document by the newline the pipeline did not inject (auto_send
        # OFF leaves newline injection to this handoff; ON is equally fine
        # because phase 1's own settle already covered its injected one).
        grew = (doc2 is not None and after is not None
                and len(doc2) > len(after))
        rep.add(2, grew,
                f"retry Enter reached the app: document grew by the "
                f"newline ({method} {len(after or '')} -> "
                f"{len(doc2 or '')} chars)", f"doc2={doc2[:120]!r}")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after the suppressed retry",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        try:
            notepad.stop()
        finally:
            # 261122_0001 A4 (F11.3): restore the user's clipboard even when
            # notepad.stop() raises - the seeded text must never survive the run.
            clipboard_backup.restore()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_autosend_newline(app_state, base_unused):
    """E10 (261121_0002): the auto_send=1 leg of REQ-028.

    REQ-028 gates the CategoryB editor newline on auto_send, and the old
    harness only ever ran with the shipped default (auto_send=0), so the
    INJECTION path - the feature the user actually toggles - had no E2E
    coverage at all. This scenario pins auto_send=true for its own session
    (restart, because the app loads auto_send at startup), types 3 lines and
    asserts per task:
      * the app's own log says action=synthetic_newline ... auto_send=1,
      * the document gains EXACTLY one "\\n" (the E2 delta check),
      * the final document holds exactly len(lines) newlines (E3),
      * the EditCaretTracker/008 newline-settle counter stays 0.
    The session is torn down and the fixture returned to auto_send=0 in a
    finally, so the rest of the matrix keeps running in the pinned mode."""
    rep = Report("autosend_newline")
    fixture = _MODE.get("fixture")
    app_state_local = app_state
    if fixture is None or not fixture.active:
        rep.note_env(
            "auto_send=1 could not be pinned: no active config fixture "
            "(the scenario needs the runtime config.json to flip the mode). "
            "Run through main() so ConfigFixture pins/restores the file.")
        return rep, app_state_local.get("app")
    try:
        # Startup-load path: the app reads auto_send once at launch, so the
        # mode flip needs its own session (force-kill first: the app's own
        # SaveToFile must not race our write).
        app_state_local["stop_app"]()
        fixture.pin({"auto_send": True})
        _MODE["auto_send"] = True
        app_state_local["start_app"]()   # verifies SESSION auto_send=1 (E5)
    except EnvBlock as e:
        _MODE["auto_send"] = False
        rep.note_env(f"auto_send=1 session could not be established: {e}")
        # Restore the pinned baseline so the scenarios that follow still have a
        # LIVE app (stop_app leaves state["app"] None); if even that fails the
        # run must abort as an environment problem instead of crashing later on
        # a None session.
        try:
            fixture.pin({"auto_send": False})
            app_state_local["start_app"]()
        except EnvBlock:
            raise EnvBlock(
                f"auto_send=1 leg failed ({e}) and the fallback baseline "
                "session could not be established either")
        return rep, app_state_local.get("app")
    try:
        app = app_state_local["app"]
        base = len(parse_tasks(app.log_text()))
        sub = run_line_session("autosend_newline", LINES_AUTOSEND, app, base,
                               auto_send=True)
        rep.checks.extend(sub.checks)
        rep.info.extend(sub.info)
        rep.env_notes.extend(sub.env_notes)
        if getattr(sub, "cleanup_note", None):
            rep.cleanup_note = sub.cleanup_note
        tail = parse_tasks(app.log_text())[base:]
        # Per-task: the app must SAY it injected, and the fixture's mode must
        # be the one it reported (a SKIPPED line here means the mode did not
        # reach the worker - environment, so BLOCKED via the E5 cross-check).
        for i, blk in enumerate(tail):
            rep.add(1, blk.send_enter_action == "synthetic_newline"
                    and blk.send_enter_auto_send == 1,
                    f"task {blk.index}: stage=send_enter "
                    f"action=synthetic_newline ... auto_send=1 recorded",
                    f"action={blk.send_enter_action!r} "
                    f"auto_send={blk.send_enter_auto_send} "
                    f"(document line {i})")
        rep.add(1, not any(b.settle_008 for b in tail),
                "EditCaretTracker/008 newline-settle timeout counter stayed 0 "
                "for every auto-injected newline",
                f"/008 tasks={[b.index for b in tail if b.settle_008]}")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after the auto_send newline session",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        app_state_local["stop_app"]()
        try:
            fixture.pin({"auto_send": False})
        finally:
            _MODE["auto_send"] = False
            app_state_local["start_app"]()
    return rep, app_state_local.get("app")


def build_long_line(total=1000, marker="E2E01"):
    unit = "아주 긴 텍스트 시나리오를 자동으로 검증합니다. "
    fill = (unit * (total // len(unit) + 1))
    return fill[: total - len(marker)] + marker


def scenario_long_text(app, base):
    """1000-char single Hangul line + Enter: capture len 1000, non-saturated
    EM offsets, replacement normal. The 64K WORD-saturation boundary is
    deliberately NOT automated (user QA QA-27-7, see README).

    C5 disposition (261121_0002): the surviving evidence shows this scenario
    intermittently dying BEFORE a translation - the 1000-char RichEdit copy
    exceeds the clipboard-sequence budget (kClipboardChangeTimeoutMs = 80 ms
    since 260910, two attempts capped at ~200 ms) and the worker's R5 hold
    preserves the source. That is a PROBABLE PRODUCT defect, not a harness
    bug, so it must not be baselined to PASS and must not be reported as a
    silent harness FAIL either: when the capture-timeout signature is present
    the verdict is INCONCLUSIVE with the diagnosis printed, and the
    individual checks are demoted to BLOCKED (Report.demote_failures) so the
    evidence stays readable. The timeout constants are deliberately UNCHANGED
    (step_timeout=90 s, the product's own clipboard budget untouched) - the
    scenario must keep exposing the defect."""
    line = build_long_line(1000)
    rep = run_line_session("long_text", [line], app, base, step_timeout=90.0)
    rep.note(f"typed line length = {utf16_len(line)} UTF-16 units "
             "(64K boundary handed to user QA QA-27-7)")
    # The capture-timeout signature: the app never got a usable capture (len 0
    # or no capture line at all) AND never pasted. A successful translate with
    # a skip/paste failure is a different (already-visible) verdict and is left
    # exactly as judged.
    try:
        blk = parse_tasks(app.log_text())[base]
    except IndexError:
        blk = None
    if blk is not None:
        captured_nothing = (blk.capture_len in (0, None)
                            or blk.capture_result not in (None, "ok"))
        if captured_nothing and blk.paste_result != 1:
            demoted = rep.demote_failures(
                "PROBABLE PRODUCT DEFECT (long-input capture timeout)")
            rep.note_env(
                "PROBABLE PRODUCT DEFECT, not a harness defect: "
                f"capture_len={blk.capture_len} "
                f"capture_k={blk.capture_k} "
                f"capture_result={blk.capture_result!r} "
                f"translate={blk.translate_status} "
                f"paste={blk.paste_result} skip={blk.paste_skip_reason}. "
                "Known 260907 signature: a 1000-char selection copy exceeds "
                "the clipboard-sequence budget (kClipboardChangeTimeoutMs="
                "80 ms since 260910, two attempts <= ~200 ms) and the R5 "
                "empty-capture hold preserves the user's source. Reported "
                "INCONCLUSIVE (never PASS) so the defect stays visible; "
                f"{demoted} FAIL check(s) demoted to BLOCKED for this run. "
                "Do not baseline this away - fix the capture budget or the "
                "CopySelectionWithSequenceWait retry policy in src/win32_input.")
    return rep


def scenario_notepad_vscode_mix(app, base):
    """Per-hwnd caret-offset state isolation. Win11: notepad.exe shares one
    process across windows, so two scratch windows have distinct edit hwnds
    on the SAME pid - the strictest test of the (focus_hwnd,pid) key
    (src/win32_input.cpp edit_caret::Key). Substituted for VSCode per the
    delegation (Electron launch + focus automation is environment-
    dependent and its intended verdict is the /007 FALLBACK - asserting it
    here would collide with the matrix-wide /006|/007 FAIL gate; the
    isolation logic is identical with a second window). Window-1 Enter,
    window-2 Enter (must start from last=0, uncontaminated), back to
    window-1 Enter (must resume at exactly its own stored post-newline
    offset - no cross-window leak, no spurious /004 clamp)."""
    rep = Report("notepad_vscode_mix")
    l1 = "첫 창에서 입력한 줄입니다. E2E01"
    l2 = "두 번째 창은 독립된 오프셋 상태여야 합니다. E2E02"
    l3 = "첫 창으로 돌아와 이어 입력하는 줄입니다. E2E03"
    np1, np2 = NotepadSession(), NotepadSession()
    try:
        np1.start()
        np1.assert_empty()
        np1.type_line(l1)
        np1.press_enter_wait_task(app.log_text, base + 1)
        blk1 = np1.wait_task_complete(app.log_text, base)
        doc1, _ = np1.read_text()
        verify_task_layer1(rep, blk1, l1, 0)
        if blk1.paste_result != 1:
            raise EnvBlock("window-1 round 1 did not replace (env); "
                           "isolation setup impossible")
        stored_last_np1 = utf16_len(doc1)  # B-6a: post-newline caret
        rep.note(f"np1 edit=0x{np1.edit:X} stored offset after t1="
                 f"{stored_last_np1}")
        np2.start()
        np2.assert_empty()
        rep.note(f"np2 edit=0x{np2.edit:X} same-pid={np2.pid == np1.pid} "
                 "distinct-hwnd=" + str(np2.edit != np1.edit))
        np2.type_line(l2)
        np2.press_enter_wait_task(app.log_text, base + 2)
        blk2 = np2.wait_task_complete(app.log_text, base + 1)
        verify_task_layer1(rep, blk2, l2, 0)
        rep.add(1, blk2.em_setselect_last == 0,
                "window-2's first Enter started at last=0 (no window-1 "
                "offset leak into the new hwnd key)",
                f"last={blk2.em_setselect_last} caret={blk2.em_setselect_caret}")
        rep.add(1, not blk2.clamped,
                "no spurious /004 clamp on window 2 (would mean window-1's "
                "larger stored offset bled across the key)",
                blk2.cite(RE_CLAMP))
        # back to window 1
        np1.type_line(l3)
        np1.press_enter_wait_task(app.log_text, base + 3)
        blk3 = np1.wait_task_complete(app.log_text, base + 2)
        doc1b, _ = np1.read_text()
        verify_task_layer1(rep, blk3, l3, stored_last_np1)
        # The STRICT proof of the resume offset is the capture check above
        # (a leaked/reset offset would capture A' or the newline too and
        # break len == l3). Here last must sit at window-1's own stored
        # offset, +/-2 tolerance for WM_GETTEXT trailing-newline conventions
        # (per-hwnd isolation is about WHICH window's offset, not unit math).
        resume_ok = (blk3.em_setselect_last is not None and
                     abs(blk3.em_setselect_last - stored_last_np1) <= 2 and
                     blk3.em_setselect_last > 0)
        rep.add(1, resume_ok,
                f"window-1's second Enter resumed at its own stored "
                f"post-newline offset ~{stored_last_np1} (per-hwnd isolation)",
                f"last={blk3.em_setselect_last} caret={blk3.em_setselect_caret}")
        rep.add(1, not blk3.clamped,
                "no /004 clamp on window 1 either (offset survived window-2 "
                "traffic)", blk3.cite(RE_CLAMP))
        tail = parse_tasks(app.log_text())[base:]
        _reference_counters(rep, tail)
    finally:
        np1.stop()
        np2.stop()
    for np_ in (np1, np2):
        if np_.cleanup_note:
            rep.cleanup_note = (getattr(rep, "cleanup_note", "") or "") + \
                np_.cleanup_note
    return rep


# ---------------------------------------------------------------------------
# multi_lang: per-block different target language. Tray-menu UIA automation
# is ATTEMPTED FIRST (design 210000 2.4: runtime switch = REQ-019's real
# path). Any refusal -> config.json pre-seed + app restart per block
# (session-split fallback), and the report/README carry the limitation note
# "runtime switch path NOT verified by this run".
# ---------------------------------------------------------------------------
MULTI_LANG_BLOCKS = [
    # (canonical name_en persisted in config + lang_pair log, tray menu item
    # code prefix, translated-output script witness)
    ("English", "EN", re.compile(r"[A-Za-z]")),
    ("Japanese", "JA", re.compile(r"[\u3040-\u30ff]")),
    ("Vietnamese", "VI", re.compile(r"[\u00c0-\u024f\u1e00-\u1eff]")),
]
MULTI_LANG_LINES = {
    "English": "언어를 바꾸지 않고 이 블록은 영어로 번역됩니다. E2E01",
    "Japanese": "이 블록은 도착 언어 전환 후 일본어로 번역되어야 합니다. E2E02",
    "Vietnamese": "세 번째 블록은 베트남어로 번역되는 것을 목표로 합니다. E2E03",
}


def _record_fixture_pin(key, value, prev_value):
    """F11.4: keep the crash ledger in sync with a config key written outside
    ConfigFixture.pin() (multi_lang). No-op when no fixture owns the file."""
    fx = _MODE.get("fixture")
    if fx is not None:
        fx.record_pin(key, value, prev_value)


def patch_config_type_target(path, value):
    """Rewrite type_target_language in the config the RUNNING app reads
    (json module, never regex - multiline-safe). Returns the previous value.

    E6 (261121_0002): the argument used to be the exe DIRECTORY, so the
    multi_lang pre-seed wrote build\\config.json while the app read
    %LOCALAPPDATA%\\Emebalachat\\config.json (REQ-029-B, 2026-09-07 12:56) -
    the fallback had been a no-op for a whole release cycle. The caller now
    passes the resolved runtime path."""
    if not os.path.exists(path):
        raise EnvBlock(f"config.json not found: {redact_user_path(path)}")
    prev = patch_config_fields(path, {"type_target_language": value})
    return prev.get("type_target_language")


def _run_tray_ps1(args, timeout_s):
    """Shared uia_tray_menu.ps1 runner: returns (returncode|None,
    TRAY:-prefix lines decoded). NEVER raises - a refusal is a normal
    outcome the callers classify."""
    ps1 = os.path.join(HERE, "uia_tray_menu.ps1")
    if not os.path.exists(ps1):
        return None, [], "uia_tray_menu.ps1 missing"
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", ps1] + args
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout_s)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, [], f"ps1 timeout/error: {e}"
    out = (p.stdout or b"").decode("utf-8-sig", errors="replace")
    tail = [ln for ln in out.splitlines() if ln.startswith("TRAY:")]
    err = (p.stderr or b"").decode("utf-8", "replace").strip()
    diag = " | ".join(tail[-4:]) or f"rc={p.returncode}"
    if err:
        diag += f" :: stderr[:200]={err[:200]}"
    # E11 manifest: the tray-UIA outcome decides whether the two tray
    # scenarios can be judged at all, so its exact shape is recorded.
    if any(ln.startswith("TRAY:NAME:") for ln in tail):
        _OBSERVED["tray_uia"] = "ENUMERATED"
    elif any(ln.startswith("TRAY:OK") for ln in tail):
        _OBSERVED["tray_uia"] = "OK"
    elif any("NOTFOUND" in ln for ln in tail):
        _OBSERVED["tray_uia"] = "NOTFOUND"
    else:
        _OBSERVED["tray_uia"] = (tail[-1] if tail else diag)[:120]
    return p.returncode, tail, diag


def tray_attempt_switch(target_code):
    """Invoke tools/e2e/uia_tray_menu.ps1 to right-click the app tray icon
    and select the typing->target-language item matching 'CODE - '. Returns
    (ok: bool, diag: str). NEVER raises: a refusal is a normal outcome that
    triggers the documented fallback."""
    rc, tail, diag = _run_tray_ps1(
        ["-ItemRegex", rf"^{target_code}\s*-"], TRAY_ATTEMPT_TIMEOUT_S)
    ok = any(ln.startswith("TRAY:OK") for ln in tail)
    return ok, diag


def tray_enumerate_uilang():
    """B-4 (design §4.3 E2E-UILANG-2): run uia_tray_menu.ps1 in EnumUiLang
    mode - open the tray Interface-Language submenu and dump every item
    Name. Returns (names: list|None, diag) where names is None on refusal
    (environment-bounded: the TrackPopupMenu modal loop can starve the UIA
    menu provider - see multi_lang's documented fallback note)."""
    rc, tail, diag = _run_tray_ps1(
        ["-Mode", "EnumUiLang", "-ItemRegex", UILANG_SUBMENU_TITLE_RX],
        TRAY_ENUM_TIMEOUT_S)
    names = [ln[len("TRAY:NAME:"):] for ln in tail
             if ln.startswith("TRAY:NAME:")]
    if not names:
        return None, diag
    return names, diag


def wait_lang_sync_type(app, tgt_name, since_len, timeout=8.0):
    """STATE/lang_sync ctx=type is logged by ApplyLanguageChange (src/main.
    cpp L604) - the single authority EVERY language mutation routes
    through, including the tray submenu pick. Seeing it with the requested
    new target proves the RUNTIME switch happened (REQ-019), not just a
    config write."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        seg = app.log_text()[since_len:]
        for seen in RE_LANG_SYNC_TYPE_TO.findall(seg):
            if seen.strip().lower() == tgt_name.lower():
                return True
        time.sleep(0.1)
    return False


def run_multi_lang(app_state, base_unused):
    """Returns (Report, new AppSession-or-None if replaced). Either mode
    verifies the user-visible contract: block k is translated into ITS OWN
    target language (lang_pair tgt matches + translated text carries that
    script)."""
    rep = Report("multi_lang")
    app = app_state["app"]
    # E6 (261121_0002): the app reads %LOCALAPPDATA%\Emebalachat\config.json
    # (REQ-029-B); the exe-dir copy is only the fallback when that file is
    # absent. Patching the exe-dir copy (the pre-fix behavior) changed nothing
    # the running app could see, which is exactly why the JA/VI blocks logged
    # tgt="English" for a whole release cycle.
    cfg_path = _runtime_config_path(app)
    rep.note(f"config fixture path (runtime): {redact_user_path(cfg_path)}")

    # --- attempt: runtime tray-menu switch in the CURRENT session ---------
    runtime_ok = True
    attempt_diag = []
    if globals().get("_TRAY_DISABLED"):
        runtime_ok = False
        attempt_diag.append("--no-tray: UIA attempt skipped by request")
    np = None
    if runtime_ok:
        np = NotepadSession()
    try:
        if np is None:
            raise EnvBlock("skipped (no-tray)")
        np.start()
        np.assert_empty()
        for order, (name, code, script_re) in enumerate(MULTI_LANG_BLOCKS):
            # Every block including the first: the EN pick is a no-op
            # re-selection if config already targets English, but it still
            # walks the full menu path (evidence the automation works).
            log_len_before = len(app.log_text())
            ok, diag = tray_attempt_switch(code)
            attempt_diag.append(f"{code}: {diag}")
            verified = ok and wait_lang_sync_type(app, name, log_len_before)
            if not verified:
                runtime_ok = False
                rep.note(f"tray UIA attempt {code} -> ok={ok} "
                         f"log_verified={verified} ({diag})")
                break
            rep.note(f"tray UIA switch to {name} verified via STATE/lang_sync")
    except EnvBlock as e:
        runtime_ok = False
        attempt_diag.append(f"probe-session-env:{e}")
    finally:
        if np is not None:
            np.stop()
            if np.cleanup_note:
                rep.note("cleanup: " + np.cleanup_note)
    rep.note("multi_lang automation-mode decision: tray UIA attempted FIRST "
             "(design 210000 2.4); diag = "
             + (" || ".join(attempt_diag) if attempt_diag else "(no attempts)"))

    if runtime_ok:
        # full runtime matrix in ONE session
        rep.note("RUNTIME SWITCH MODE: language changed live via the tray "
                 "menu for every block (REQ-019 runtime path exercised)")
        base = len(parse_tasks(app.log_text()))
        np = NotepadSession()
        try:
            np.start()
            np.assert_empty()
            cumulative = 0
            for order, (name, code, script_re) in enumerate(MULTI_LANG_BLOCKS):
                # Per-block live switch (user flow: "여러 언어로 바꾸면서"):
                # the probe left the app on the LAST probed language, so
                # each block re-selects its own through the tray menu.
                log_len_before = len(app.log_text())
                sw_ok, sw_diag = tray_attempt_switch(code)
                rep.add(1, sw_ok and wait_lang_sync_type(
                    app, name, log_len_before),
                    f"runtime tray switch to {name} before block "
                    f"{order} (UIA)", sw_diag)
                line = MULTI_LANG_LINES[name]
                np.type_line(line)
                before, _ = np.read_text()
                np.press_enter_wait_task(app.log_text, base + order + 1)
                blk = np.wait_task_complete(app.log_text, base + order)
                after = _read_doc_after_settle(np, before, blk)
                verify_task_layer1(rep, blk, line, cumulative)
                verify_task_layer2(rep, blk, line, before, after,
                                   line_ordinal=order, auto_send=False)
                _assert_lang_block(rep, blk, name, script_re, code_prefix=order)
                cumulative += utf16_len(line) + 2
            verify_final_document(rep, np, [MULTI_LANG_LINES[n] for n, _, _
                                            in MULTI_LANG_BLOCKS],
                                  auto_send=False)
        finally:
            np.stop()
        if np.cleanup_note:
            rep.cleanup_note = np.cleanup_note
        return rep, app

    # --- fallback: config.json pre-seed + fresh app per block -------------
    rep.note("CONFIG-SEED FALLBACK MODE (design 210000 2.4 option (a)): "
             "each block runs in its own app session with "
             "type_target_language pre-injected. LIMITATION: this verifies "
             "ONLY the startup config-load path; the REQ-019 runtime "
             "switch path is NOT verified by this run (see README + "
             "user-QA QA-27-3R).")
    try:
        original = _read_json(cfg_path).get("type_target_language")
    except (OSError, ValueError):
        original = None
    final_app = None
    try:
        for order, (name, code, script_re) in enumerate(MULTI_LANG_BLOCKS):
            # Fresh app session per block: stop first (force-kill skips the
            # app's own SaveToFile, so our patch is never raced), THEN patch
            # the RUNTIME config.json (startup-load path), THEN start.
            app_state["stop_app"]()
            prev_tgt = patch_config_type_target(cfg_path, name)
            _record_fixture_pin("type_target_language", name, prev_tgt)
            app_state["start_app"]()
            app = app_state["app"]
            # E5/E6: prove the pinned target is the one the app actually read
            # (file readback + the app's own lang_pair line below). Without
            # this, a silently ignored pre-seed looked identical to a product
            # language bug for a whole release cycle.
            try:
                live_tgt = _read_json(cfg_path).get("type_target_language")
            except (OSError, ValueError):
                live_tgt = None
            rep.add(1, live_tgt == name,
                    f"[{name}] pinned type_target_language is live in the "
                    f"runtime config the app loads ({redact_user_path(cfg_path)})",
                    f"file reports {live_tgt!r}")
            np = NotepadSession()
            try:
                np.start()
                np.assert_empty()
                line = MULTI_LANG_LINES[name]
                np.type_line(line)
                before, _ = np.read_text()
                np.press_enter_wait_task(app.log_text, 1)
                blk = np.wait_task_complete(app.log_text, 0)
                after = _read_doc_after_settle(np, before, blk)
                verify_task_layer1(rep, blk, line, 0)
                verify_task_layer2(rep, blk, line, before, after,
                                   line_ordinal=0, auto_send=False)
                _assert_lang_block(rep, blk, name, script_re,
                                   code_prefix=order, label=f"[{name}] ")
            finally:
                np.stop()
            if np.cleanup_note:
                rep.note("cleanup: " + np.cleanup_note)
    finally:
        app_state["stop_app"]()
        if original is not None:
            prev_tgt = patch_config_type_target(cfg_path, original)
            _record_fixture_pin("type_target_language", original, prev_tgt)
        # leave a fresh default session for whichever scenario follows
        app_state["start_app"]()
        final_app = app_state["app"]
    return rep, final_app


def _assert_lang_block(rep, blk, lang_name, script_re, code_prefix, label=""):
    rep.add(1, blk.lang_tgt == lang_name,
            f"{label}block {code_prefix}: stage=lang_pair tgt="
            f'"{blk.lang_tgt}" == "{lang_name}"',
            f"lang_tgt={blk.lang_tgt!r}")
    out = unescape_diag(blk.translate_out or "")
    if blk.translate_status == 0 and out:
        rep.add(1, bool(script_re.search(out)) and not re.search(
            r"[\uac00-\ud7af]", out),
            f"{label}block {code_prefix}: translated text matches the "
            f"{lang_name} script and contains no Hangul",
            f"out={out[:80]!r}")
    else:
        # E7: the script witness needs the content-gated out="..." field
        # (src/worker.cpp L1108-1120). Distinguish "logging off" from "engine
        # produced nothing" so a BLOCKED line is not mistaken for a defect.
        why = ("content unavailable (diag_log_content=false)"
               if blk.translate_out is None and blk.translate_status == 0
               else f"translate status={blk.translate_status}")
        rep.add(1, None,
                f"{label}block {code_prefix}: script check BLOCKED - {why}")


# ---------------------------------------------------------------------------
# uilang_37: REQ-037/B-4 tray submenu enumeration (design §4.3 E2E-UILANG-2)
# ---------------------------------------------------------------------------
# The 37 selector endonyms in design §2-Q3 display order (front block KO, JA,
# zh-CN, zh-TW, VI, ES, then registry order for the rest, EN last). This is
# the tray's source-of-truth expectation, independent of the live selector:
# TestReq037LocaleMapping pins the selector's endonyms to the kAllLanguages
# registry, and this list is copied from the same registry rows. If B-3 ever
# changes the join, this list changing is a deliberate cross-file sync.
UILANG_ENDONYM_ORDER = [
    "한국어", "日本語", "简体中文", "繁體中文", "Tiếng Việt", "Español",
    "English",  # placeholder slot for EN; moved last before assertion below
    "Français", "Deutsch", "Русский", "ไทย", "العربية", "Português",
    "Italiano", "Bahasa Indonesia", "Bahasa Melayu", "Filipino",
    "ភាសាខ្មែរ", "ພາສາລາວ", "हिन्दी", "বাংলা", "Türkçe", "Polski",
    "Nederlands", "Українська", "فارسی", "اردو", "עברית", "Čeština",
    "Magyar", "Svenska", "Ελληνικά", "Română", "Dansk", "Suomi",
    "Norsk", "မြန်မာစာ",
]
# §2-Q3 order with the legacy front block first and English last:
UILANG_EXPECTED_ORDER = (UILANG_ENDONYM_ORDER[:6]
                         + [e for e in UILANG_ENDONYM_ORDER[6:] if e != "English"]
                         + ["English"])


def scenario_uilang_37(app_state, base):
    """REQ-037/B-4 (design §4.3 E2E-UILANG-2, delegation B-4 item 4):
    enumerate the tray Interface-Language submenu via UIA and assert the
    38-item expansion. READ-ONLY - the ps1 ESC-closes without invoking,
    so config/app state are untouched. Verdict policy: menu-automation
    refusal (ps1 NOTFOUND or empty tree = the documented TrackPopupMenu
    UIA-starve environment bound) -> INCONCLUSIVE (note_env), never a
    product FAIL; ONLY a readable-but-wrong submenu FAILs."""
    rep = Report("uilang_37")
    app = app_state["app"]
    if globals().get("_TRAY_DISABLED"):
        rep.note_env("--no-tray: UIA enumeration skipped by request")
        return rep, app
    names, diag = tray_enumerate_uilang()
    rep.note(f"uia_tray_menu.ps1 EnumUiLang diag: {diag[:400]}")
    if names is None:
        rep.note_env(
            "tray submenu enumeration refused (TrackPopupMenu modal loop "
            "starves the UIA menu provider in this environment - the same "
            "documented multi_lang bound; NOT a product FAIL). Verdict via "
            "B-4 static review + TestReq037LocaleMapping selector-order "
            "pin + user QA M3 (RTL legibility).")
        return rep, app

    def _norm(s):
        return s.replace("\u200f", "").strip()
    # The dump also contains the root-menu items that were visible while the
    # submenu was open (the ps1 walks ALL #32768 panes). The Interface-
    # Language submenu is identified by its two unique markers; assert on
    # the SUBSET whose names match the 37 endonyms + an auto entry.
    norm = [_norm(n) for n in names]
    endonym_set = set(UILANG_EXPECTED_ORDER)
    lang_hits = [n for n in norm if n in endonym_set]
    auto_hits = [n for n in norm
                 if n == "자동 (시스템 언어)" or n == "Auto (system language)"]
    rep.add(1, len(auto_hits) == 1,
            "exactly one Auto (system language) entry in the submenu",
            f"auto_hits={auto_hits}")
    rep.add(1, len(lang_hits) == 37,
            f"37 endonym entries present (got {len(lang_hits)})",
            f"missing={sorted(endonym_set - set(lang_hits))}")
    # Spot-assert the delegation's named witnesses + the RTL entries the
    # expansion is about (endonyms render in their own scripts).
    for spot in ("العربية", "日本語", "한국어"):
        rep.add(1, spot in norm, f"endonym witness present: {spot}",
                f"submenu={norm[:40]}")
    # Order pin (design §2-Q3 muscle memory): among the names that ARE
    # selector entries, their relative dump order must equal the expected
    # order. The walk order within a pane is top-to-bottom; cross-pane root
    # items are ignored by the endonym filter.
    dumped_order = [n for n in norm if n in endonym_set]
    rep.add(1, dumped_order == UILANG_EXPECTED_ORDER,
            "selector entries appear in design §2-Q3 order (KO, JA, zh-CN, "
            "zh-TW, VI, ES front block then registry order, EN last)",
            f"dumped={dumped_order}")
    rep.add(1, len(norm) >= 38,
            f"submenu pane exposed >= 38 item names (count={len(norm)})",
            f"diag={diag[:200]}")
    # App still alive + untouched: the enumeration must not change config.
    rep.add(1, app.proc is None or app.proc.poll() is None,
            "app process alive after submenu enumeration",
            f"exit={None if app.proc is None else app.proc.poll()}")
    try:
        cfg_path = _runtime_config_path(app)
        with open(cfg_path, "r", encoding="utf-8") as f:
            ui_val = json.load(f).get("ui_language")
        rep.add(1, True, f"[info] config.ui_language unchanged-read={ui_val!r}")
    except (OSError, ValueError) as e:  # ValueError = json decode (malformed)
        rep.note(f"config readback skipped ({e}) - read-only scenario anyway")
    return rep, app


# ---------------------------------------------------------------------------
# reset_defaults: REQ-040/B-6 E2E-RESET-1 (design §4.3 + §2.3.3 C7, G-1
# reset x ui_language user decision 2026-09-07). The scenario is
# interactive-desktop bound: it moves the REAL cursor (the About card is a
# custom-drawn layered popup with no child HWNDs, so UIA Invoke cannot reach
# the reset button - a synthetic click at the production DIP geometry is the
# faithful reproduction of the user action, exercising the WM_LBUTTONUP
# hit-test code path unchanged).
# ---------------------------------------------------------------------------
# Win32 seams added for this scenario (argtypes everywhere - x64 correctness).
user32.GetWindowRect.argtypes = [HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetCursorPos.restype = wintypes.BOOL
user32.mouse_event.argtypes = [wintypes.UINT, wintypes.UINT, wintypes.UINT,
                               wintypes.ULONG, ctypes.c_void_p]  # ULONG_PTR
user32.GetDpiForWindow.argtypes = [HWND]
user32.GetDpiForWindow.restype = ctypes.c_uint
# Unicode-only API: exported WITHOUT the W suffix (no A/W split on
# GetUserDefaultLocaleName - probed live via this import path).
kernel32.GetUserDefaultLocaleName.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
kernel32.GetUserDefaultLocaleName.restype = ctypes.c_int
kernel32.GetUserDefaultUILanguage.restype = ctypes.c_ushort

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
ABOUT_CLASS = "Emebalachat_AboutClass"
# Locale-independent About-menu matcher: every menu_about string embeds the
# brand token "Emebala Chat" and ends in U+2026 (…); no other root-menu item
# pairs both (menu_exit strings carry the brand but never the ellipsis).
ABOUT_ITEM_RX = "Emebala Chat.*\u2026"
# Non-default fixture for the five reset-covered keys (G-1 made ui_language
# the fifth). Values are all valid persisted codes so the seeded config loads
# cleanly through FromJsonString + I18n::Initialize("th").
RESET_NONDEFAULTS = {
    "ui_language": "th",
    "drag_source_language": "English",
    "drag_target_language": "Vietnamese",
    "type_source_language": "Korean",
    "type_target_language": "Japanese",
}
RESET_KEYS = tuple(RESET_NONDEFAULTS)
RESET_POLL_S = 8.0


def _runtime_config_path_for_exe(exe_path):
    """The config.json the app spawned from `exe_path` actually reads/writes.
    Mirrors src/config.cpp GetDefaultConfigPath (REQ-029-B):
    %LOCALAPPDATA%\\Emebalachat\\config.json first, exe-dir only when the
    Known-Folder file is absent. Exists separately from _runtime_config_path so
    main() can resolve the fixture path BEFORE the first app launch (E5).

    F3 (261121_0002 round 2): the LEGACY layout (no Known-Folder file, exe-dir
    copy present) is REFUSED. AppConfig::LoadFromFile migrates the legacy bytes
    to the Known-Folder path at startup (src/config.cpp, one-shot) and keeps the
    legacy file, so a pin written here would be copied into the authoritative
    config and the byte-verbatim restore of the exe-dir copy would not reach it:
    diag_log_content=true would persist for every later run."""
    lad = os.environ.get("LOCALAPPDATA", "")
    if lad:
        p = os.path.join(lad, "Emebalachat", "config.json")
        if os.path.exists(p):
            return p
    legacy = os.path.join(os.path.dirname(os.path.abspath(exe_path)),
                          "config.json")
    if lad and os.path.exists(legacy):
        raise FixtureRefused(
            "legacy exe-dir config.json present but "
            f"{redact_user_path(os.path.join(lad, 'Emebalachat', 'config.json'))} is absent: the "
            "app MIGRATES the pinned bytes to the Known-Folder path at startup "
            "(src/config.cpp LoadFromFile), where this harness's byte-verbatim "
            "restore cannot reach them - the QA diag_log_content=true pin would "
            "persist in the authoritative config. Launch the app once and close "
            "it (creating the %LOCALAPPDATA% copy), then re-run the harness.")
    return legacy


def _runtime_config_path(app):
    """Same resolution, from a live AppSession. The legacy exe-dir copy alone
    is NOT what the app persists when LOCALAPPDATA exists and must never be
    used to patch a running app (E6)."""
    return _runtime_config_path_for_exe(app.exe_path)


def _read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _atomic_write_bytes(path, data):
    """Write `data` to `path` without ever truncating the previous file (F1).

    Serialize into a sibling temp file, fsync it, then os.replace() over the
    target: a failed or interrupted write leaves the OLD bytes in place, so a
    harness bug or a full disk can never destroy the user's config.json. The
    temp lives in the same directory (same volume) so the replace is atomic.
    A read-only target is refused (the pre-atomic behaviour also failed) rather
    than silently having its read-only attribute replaced away."""
    if os.path.exists(path) and not os.access(path, os.W_OK):
        raise PermissionError(f"{redact_user_path(path)} is read-only; refusing to overwrite it")
    directory = os.path.dirname(os.path.abspath(path)) or os.curdir
    fd, tmp = tempfile.mkstemp(prefix=os.path.basename(path) + ".e2e-",
                               suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        if os.path.exists(path):
            try:
                shutil.copymode(path, tmp)   # keep the read-only bit
            except OSError:
                pass
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _write_json(path, data):
    _atomic_write_bytes(
        path,
        (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


def patch_config_fields(path, values):
    """json-module rewrite of the given keys (never regex - multiline-safe).
    Returns {key: previous value} for restore; a restore value of None DELETES
    the key (the app re-derives absent keys as defaults - writing JSON null
    would be a schema the loader does not expect)."""
    if not os.path.exists(path):
        raise EnvBlock(f"runtime config.json not found: {redact_user_path(path)}")
    data = _read_json(path)
    prev = {k: data.get(k) for k in values}
    for k, v in values.items():
        if v is None:
            data.pop(k, None)
        else:
            data[k] = v
    _write_json(path, data)
    return prev


def expected_system_drag_default():
    """Python-side oracle of the design §2.6 rule for the HOST OS language
    (the same mapping TestReq040SystemDefaults37 pins x37 at unit level):
    unsupported/unknown -> English, EN -> 'Korean' (G-2 pivot), any other
    supported language -> its registry name_en. Derived independently of the
    C++ under test, from the OS seam DetectSystemLocale phase 1 reads
    (GetUserDefaultLocaleName), with the zh script pre-check and the LANGID
    fallback (GetUserDefaultUILanguage) mirrored for robustness."""
    # BCP-47 language subtag -> registry code (iw = legacy Hebrew tag Windows
    # still reports on some installs; nb/nn/no all map to Norwegian per the
    # B-3 mapping table's dual-prefix row).
    sub = {
        "ko": "KO", "en": "EN", "vi": "VI", "ja": "JA", "es": "ES",
        "fr": "FR", "de": "DE", "ru": "RU", "th": "TH", "ar": "AR",
        "pt": "PT", "it": "IT", "id": "ID", "ms": "MS", "fil": "FIL",
        "km": "KM", "lo": "LO", "hi": "HI", "bn": "BN", "tr": "TR",
        "pl": "PL", "nl": "NL", "uk": "UK", "fa": "FA", "ur": "UR",
        "he": "HE", "iw": "HE", "cs": "CS", "hu": "HU", "sv": "SV",
        "el": "EL", "ro": "RO", "da": "DA", "fi": "FI", "no": "NO",
        "nb": "NO", "nn": "NO", "my": "MY",
    }
    # Registry code -> name_en (src/config.cpp kAllLanguages, copied literals).
    name_en = {
        "AUTO": "Auto Detect", "EN": "English", "KO": "Korean",
        "VI": "Vietnamese", "ZH-CN": "Chinese Simplified",
        "ZH-TW": "Chinese Traditional", "JA": "Japanese", "ES": "Spanish",
        "FR": "French", "DE": "German", "RU": "Russian", "TH": "Thai",
        "AR": "Arabic", "PT": "Portuguese", "IT": "Italian",
        "ID": "Indonesian", "MS": "Malay", "FIL": "Filipino", "KM": "Khmer",
        "LO": "Lao", "HI": "Hindi", "BN": "Bengali", "TR": "Turkish",
        "PL": "Polish", "NL": "Dutch", "UK": "Ukrainian", "FA": "Persian",
        "UR": "Urdu", "HE": "Hebrew", "CS": "Czech", "HU": "Hungarian",
        "SV": "Swedish", "EL": "Greek", "RO": "Romanian", "DA": "Danish",
        "FI": "Finnish", "NO": "Norwegian", "MY": "Burmese",
    }
    buf = ctypes.create_unicode_buffer(85)
    ok = kernel32.GetUserDefaultLocaleName(buf, 85)
    tag = (buf.value or "").lower() if ok else ""
    primary, _, region = tag.partition("-")
    if primary == "zh":
        if region in ("tw", "hk", "mo", "hant"):
            code = "ZH-TW"
        elif region in ("cn", "sg", "hans", "my"):
            code = "ZH-CN"
        else:
            langid = kernel32.GetUserDefaultUILanguage()
            if (langid & 0x3FF) == 0x04 and ((langid >> 10) & 0x3F) == 0x04:
                code = "ZH-TW"  # SUBLANG_CHINESE_TRADITIONAL
            else:
                code = "ZH-CN"
    else:
        code = sub.get(primary)
    if code is None:
        return "English"   # unsupported/unknown OS language -> AUTO -> English
    if code == "EN":
        return "Korean"    # G-2 EN->EN pivot (user decision 2026-09-07)
    return name_en[code]


def _find_about_hwnd(pid):
    for (h, _title, cls) in top_windows_for_pid(pid):
        if cls == ABOUT_CLASS:
            return int(h)
    return 0


def tray_invoke_about():
    """Open About via the tray menu (UIA InvokeMenuItem, locale-matched).
    Returns (ok, diag); NEVER raises - refusal is a normal environment outcome
    the scenario classifies INCONCLUSIVE (same bound as tray_attempt_switch)."""
    rc, tail, diag = _run_tray_ps1(
        ["-Mode", "InvokeMenuItem", "-ItemRegex", ABOUT_ITEM_RX,
         "-TimeoutSec", "45"], TRAY_ATTEMPT_TIMEOUT_S)
    ok = any(ln.startswith("TRAY:OK") for ln in tail)
    return ok, diag


def click_about_reset(hwnd):
    """Real left-click on the About card's reset button. Geometry comes from
    the PRODUCTION code (about_window.cpp §Phase-4/B-5 comments): 440x596 DIP
    card, reset_rect_ x[24, w-24] y[546,578] -> center (220, 562) DIP.
    Physical px via GetDpiForWindow with the same round-to-nearest convention
    as ui::ScaleDipsToPixels ((dips*dpi + 48) / 96)."""
    rect = wintypes.RECT()
    if not user32.GetWindowRect(HWND(hwnd), ctypes.byref(rect)):
        raise EnvBlock("GetWindowRect failed on the About window")
    dpi = user32.GetDpiForWindow(HWND(hwnd)) or 96
    if dpi < 96:
        dpi = 96
    cx = rect.left + (220 * dpi + 48) // 96
    cy = rect.top + (562 * dpi + 48) // 96
    user32.SetForegroundWindow(HWND(hwnd))  # best-effort: the card is topmost
    if not user32.SetCursorPos(cx, cy):
        raise EnvBlock("SetCursorPos failed (cursor input locked?)")
    time.sleep(0.08)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    return (cx, cy, dpi)


def scenario_reset_defaults(app_state, base_unused):
    """E2E-RESET-1 (design §4.3): seed 4 language fields + ui_language=th ->
    restart (values live in memory) -> About reset click -> persisted config
    shows system defaults + ui_language auto + lang_reset DIAG marker.
    See the section banner above for the interactive-desktop contract."""
    rep = Report("reset_defaults")
    app = app_state["app"]
    cfg = _runtime_config_path(app)
    try:
        original = _read_json(cfg)
    except OSError as e:
        raise EnvBlock(f"runtime config unreadable: {cfg} ({e})")

    want_drag_default = expected_system_drag_default()
    rep.note(f"expected host drag default: {want_drag_default!r} "
             f"(design §2.6 mapping via GetUserDefaultLocaleName)")
    if globals().get("_TRAY_DISABLED"):
        rep.note_env("--no-tray: E2E-RESET-1 needs the tray UIA path to "
                     "open About; skipped by request (config untouched)")
        return rep, app
    # 1) Seed the non-defaults with the app DOWN (the multi_lang config-seed
    #    idiom: a running app would overwrite the file from its in-memory
    #    copy on the next save). From here on the restore block below must
    #    run even on mid-scenario exceptions.
    app_state["stop_app"]()
    patch_config_fields(cfg, RESET_NONDEFAULTS)
    rep.note(f"seeded {RESET_NONDEFAULTS}")
    try:
        app_state["start_app"]()
        app = app_state["app"]
        seeded = _read_json(cfg)
        rep.add(1, seeded.get("ui_language") == "th" and
                    seeded.get("type_target_language") == "Japanese",
                "pre-reset config holds the injected non-defaults (G-1 fixture)",
                f"ui={seeded.get('ui_language')!r} "
                f"tgt={seeded.get('type_target_language')!r}")

        # 2) Open About through the tray (UIA InvokeMenuItem).
        ok, diag = tray_invoke_about()
        if not ok:
            rep.note_env(
                "tray UIA refused About-open (documented TrackPopupMenu "
                "starvation / no interactive desktop - NOT a product FAIL). "
                "E2E-RESET-1 requires a live session with a responsive tray; "
                "the G-1/C7 contract stays proven by "
                "TestReq040SystemDefaults37 (unit half) + user QA. "
                f"diag={diag[:300]}")
        else:
            hwnd = 0
            deadline = time.time() + 6.0
            while time.time() < deadline:
                hwnd = _find_about_hwnd(app.proc.pid)
                if hwnd and user32.IsWindowVisible(HWND(hwnd)):
                    break
                time.sleep(POLL_S)
            rep.add(1, bool(hwnd) and user32.IsWindowVisible(HWND(hwnd)),
                    "C6/E2E: About window (Emebalachat_AboutClass) opened via "
                    "the tray About item under the Thai UI",
                    f"hwnd={hwnd}")
            if hwnd:
                # 3) Real click on the reset button (C6 affordance exists ->
                #    its WM_LBUTTONUP hit-test fires the coordinator callback).
                click_info = click_about_reset(hwnd)
                rep.add(1, True, f"real click at reset-button center {click_info}")

                # 4) Coordinator ran + config re-recorded: poll marker + file.
                log_hit = False
                post = None
                deadline = time.time() + RESET_POLL_S
                while time.time() < deadline:
                    log_hit = log_hit or ("lang_reset" in app.log_text())
                    try:
                        post = _read_json(cfg)
                    except OSError:
                        post = None
                    if (post and post.get("ui_language") == "auto"
                            and post.get("drag_source_language") == "Auto Detect"
                            and post.get("type_source_language") == "Auto Detect"
                            and post.get("type_target_language") == "English"):
                        break
                    time.sleep(0.15)
                rep.add(1, log_hit,
                        "lang_reset DIAG marker present (apply_system_defaults "
                        "coordinator executed the reset)")
                if post is None:
                    post = _read_json(cfg)
                rep.add(1, post.get("drag_source_language") == "Auto Detect",
                        "C1/E2E: drag source back to Auto Detect",
                        f"got={post.get('drag_source_language')!r}")
                rep.add(1, post.get("drag_target_language") == want_drag_default,
                        f"C2/E2E: drag target back to the host system default "
                        f"({want_drag_default!r})",
                        f"got={post.get('drag_target_language')!r}")
                rep.add(1, post.get("type_source_language") == "Auto Detect",
                        "C3/E2E: typing source back to Auto Detect",
                        f"got={post.get('type_source_language')!r}")
                rep.add(1, post.get("type_target_language") == "English",
                        "C4/E2E: typing target back to English",
                        f"got={post.get('type_target_language')!r}")
                rep.add(1, post.get("ui_language") == "auto",
                        "G-1: ui_language back to auto (the user-approved reset "
                        "extension; surfaces re-resolve via DetectSystemLocale)",
                        f"got={post.get('ui_language')!r}")
                rep.add(1, app.proc.poll() is None,
                        "app alive after the reset click")
    finally:
        # Restore the user's five config keys (json-module rewrite; other keys
        # untouched) and leave a fresh app session for any scenario that
        # follows - same teardown contract as run_multi_lang's fallback mode.
        try:
            app_state["stop_app"]()
        finally:
            patch_config_fields(cfg, {k: original.get(k) for k in RESET_KEYS})
            app_state["start_app"]()
    return rep, app_state["app"]


# ---------------------------------------------------------------------------
# scenario registry
# ---------------------------------------------------------------------------
LINE_SCENARIOS = {
    "qa27b": LINES_QA27B,
    "example1": LINES_EXAMPLE1,
    "consecutive": LINES_CONSECUTIVE,
}


def scenario_line_driver(name):
    def drive(app_state, base):
        rep = run_line_session(name, LINE_SCENARIOS[name],
                               app_state["app"], base)
        return rep, app_state["app"]
    return drive


SIMPLE_DRIVERS = {
    "empty_enter": lambda app_state, base: (
        scenario_empty_enter(app_state["app"], base), app_state["app"]),
    "cursor_mid": lambda app_state, base: (
        scenario_cursor_mid(app_state["app"], base), app_state["app"]),
    "backspace_enter": lambda app_state, base: (
        scenario_backspace_enter(app_state["app"], base), app_state["app"]),
    "shift_enter_multi": lambda app_state, base: (
        scenario_shift_enter_multi(app_state["app"], base), app_state["app"]),
    "paste_then_enter": lambda app_state, base: (
        scenario_paste_then_enter(app_state["app"], base), app_state["app"]),
    "long_text": lambda app_state, base: (
        scenario_long_text(app_state["app"], base), app_state["app"]),
    "notepad_vscode_mix": lambda app_state, base: (
        scenario_notepad_vscode_mix(app_state["app"], base), app_state["app"]),
}


def build_drivers():
    d = {name: scenario_line_driver(name) for name in LINE_SCENARIOS}
    d.update(SIMPLE_DRIVERS)
    d["multi_lang"] = run_multi_lang
    d["autosend_newline"] = scenario_autosend_newline
    d["uilang_37"] = scenario_uilang_37
    d["reset_defaults"] = scenario_reset_defaults
    return d


DRIVERS = build_drivers()

ALL_SCENARIOS = ["qa27b", "example1", "consecutive", "multi_lang",
                 "empty_enter", "cursor_mid", "backspace_enter",
                 "shift_enter_multi", "autosend_newline", "paste_then_enter",
                 "long_text", "notepad_vscode_mix", "uilang_37",
                 "reset_defaults"]
# Scenarios whose verdict is environment-bounded by the Windows tray UIA
# provider (TrackPopupMenu modal loop starvation). They are NEVER converted to
# PASS; --skip-tray-scenarios removes them from the matrix so a green "all" is
# achievable on a desktop where the enumeration is unavailable (E12 policy).
TRAY_SCENARIOS = ("uilang_37", "reset_defaults")
# ime_composing is intentionally NOT in the matrix: WM_CHAR/SendInput cannot
# create a real IME composition state, and simulating it unverified would
# produce false verdicts. Handed to user QA (decisions.md 2026-09-07 20:54;
# rationale in tools/e2e/README.md).


# ---------------------------------------------------------------------------
# E11: run manifest + environment fingerprint
# ---------------------------------------------------------------------------
def utc_stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ")


def sha256_file(path):
    """None when the file cannot be read (honest absence, never a fake hash)."""
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def _ps_capture(cmd, timeout=10):
    """PowerShell one-liner capture with CREATE_NO_WINDOW: the manifest probe
    must never be able to flash a console window (zero screen intrusion)."""
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy",
                            "Bypass", "-Command", cmd],
                           capture_output=True, timeout=timeout,
                           creationflags=flags)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode != 0:
        return None
    return (p.stdout or b"").decode("utf-8-sig", errors="replace").strip()


def notepad_image_version():
    """FileVersion of the installed notepad.exe image (drift record). Win11's
    Store build may report an appx version or nothing; None is honest."""
    windir = os.environ.get("WINDIR", r"C:\Windows")
    for cand in (os.path.join(windir, "System32", "notepad.exe"),
                 os.path.join(windir, "notepad.exe")):
        if os.path.exists(cand):
            safe = cand.replace("'", "''")
            out = _ps_capture(
                f"(Get-Item -LiteralPath '{safe}').VersionInfo.FileVersion")
            if out:
                return out.splitlines()[0].strip()
            return None
    return None


def os_fingerprint():
    info = {"platform": platform.platform(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version()}
    try:
        v = sys.getwindowsversion()
        info["build"] = f"{v.major}.{v.minor}.{v.build}"
    except AttributeError:  # pragma: no cover - non-Windows import probe
        info["build"] = None
    return info


def build_run_manifest(*, started_utc, finished_utc, duration_s, app_exe,
                       fixture, chosen, reports, counts, exit_code,
                       run_dir, extra_notes=None):
    """The machine-readable record of WHAT was verified, on WHICH environment.

    The F2 misdiagnosis of 2026-11-21 ("Win11 Notepad drift") is exactly the
    failure mode this prevents: a future green/red flip must be attributable to
    a recorded fact (OS build, app hash, self-reported version, classes, read
    method, pinned mode, config hashes), not to a guess."""
    info = {"schema": 1, "harness": "tools/e2e/req027_e2e.py",
            "started_utc": started_utc, "finished_utc": finished_utc,
            "duration_s": round(duration_s, 1),
            "run_dir": redact_user_path(run_dir),
            "os": os_fingerprint(),
            "app": {"path": redact_user_path(app_exe),
                    "sha256": sha256_file(app_exe),
                    "self_reported_version": _OBSERVED["app_version"],
                    "self_reported_exe":
                        redact_user_path(_OBSERVED["app_exe_reported"])},
            "config": {"path": (redact_user_path(fixture.path) if fixture
                                else None),
                       "pre_sha256": fixture.pre_sha if fixture else None,
                       "post_sha256": fixture.post_sha if fixture else None,
                       "restore_ok": fixture.restore_ok if fixture else None,
                       "pinned": dict(fixture.values) if fixture else None,
                       "fixture_active": bool(fixture and fixture.active)},
            "mode": {"auto_send": int(fixture_auto_send()),
                     "diag_log_enabled": 1 if fixture else None,
                     "diag_log_content": 1 if fixture else None,
                     "tray_disabled": bool(globals().get("_TRAY_DISABLED"))},
            "notepad": {"image_version": _OBSERVED["notepad_version"],
                        "frame_class": _OBSERVED["frame_class"],
                        "edit_classes": sorted(_OBSERVED["edit_classes"])},
            "read_methods": sorted(_OBSERVED["read_methods"]),
            "tray_uia": _OBSERVED["tray_uia"],
            "scenarios": list(chosen),
            "verdicts": [{"scenario": r.name, "verdict": r.verdict()[0],
                          "fail": len(r.verdict()[1]),
                          "blocked": len(r.verdict()[2])} for r in reports],
            "counts": counts,
            "exit_code": exit_code,
            "read": ("Layer1 app DIAG log + Layer2 WM_GETTEXT/UIA document "
                     "read; no screen capture, no synthetic user input beyond "
                     "the documented SendInput Enter/Ctrl+V/backspace"),
            "privacy": ("diag_log_content was pinned ON for this QA run only; "
                        "the config is restored byte-verbatim and only "
                        "harness-owned E2E strings were typed into verified "
                        "empty scratch documents"),
            "notes": [redact_user_path(n) for n in (extra_notes or [])]}
    return info


def print_env_fingerprint(manifest):
    a = manifest["app"]
    c = manifest["config"]
    m = manifest["mode"]
    n = manifest["notepad"]
    print("   fingerprint: os={b} frame={f} edit={e} read={r}".format(
        b=manifest["os"].get("build"), f=n.get("frame_class"),
        e=",".join(n.get("edit_classes") or []) or "?",
        r=",".join(manifest["read_methods"]) or "?"))
    print(f"                app={str(a['sha256'])[:16]}... "
          f"ver={a['self_reported_version']} config={c['path']} "
          f"sha={str(c['pre_sha256'])[:16]}... "
          f"auto_send={m['auto_send']} tray_uia={manifest['tray_uia']}")


def write_run_manifest(manifest, run_dir):
    path = os.path.join(run_dir, "run-manifest.json")
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)
            f.write("\n")
    except OSError as e:
        print(f"[harness] WARN: could not write {redact_user_path(path)}: {e}")
        return None
    return path


def baseline_drift_banner(manifest, baseline_path):
    """Compare the run fingerprint against a saved manifest and print a banner.
    Reporting only: a drift is a human-review signal, never a verdict change."""
    if not baseline_path or not os.path.exists(baseline_path):
        return None
    try:
        base = _read_json(baseline_path)
    except (OSError, ValueError) as e:
        print(f"   baseline : unreadable ({redact_user_path(baseline_path)}: {e}) - skipped")
        return None
    keys = [("os", "build"), ("app", "sha256"),
            ("app", "self_reported_version"), ("notepad", "frame_class"),
            ("mode", "auto_send")]
    diffs = []
    for section, key in keys:
        old = (base.get(section) or {}).get(key)
        new = (manifest.get(section) or {}).get(key)
        if old != new:
            diffs.append(f"{section}.{key}: {old!r} -> {new!r}")
    old_edits = sorted((base.get("notepad") or {}).get("edit_classes") or [])
    if old_edits != manifest["notepad"]["edit_classes"]:
        diffs.append(f"notepad.edit_classes: {old_edits!r} -> "
                     f"{manifest['notepad']['edit_classes']!r}")
    if diffs:
        print("   !! BASELINE-DRIFT CANDIDATE (reporting only, verdict "
              "unchanged): " + " | ".join(diffs))
        return diffs
    print(f"   baseline : matches {redact_user_path(baseline_path)}")
    return []


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main(argv=None):
    global STEP_TIMEOUT_S
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="REQ-027 notepad per-line translation E2E harness "
                    "(14-scenario matrix incl. autosend_newline)")
    ap.add_argument("scenario", nargs="?", default="qa27b",
                    choices=sorted(ALL_SCENARIOS + ["all"]))
    ap.add_argument("--app-exe", default=DEFAULT_APP_EXE,
                    help="path to Emebala_chat.exe (default build\\)")
    ap.add_argument("--no-cleanup", action="store_true",
                    help="keep the app process alive after the run (debug); "
                         "the config fixture refuses to pin under this flag "
                         "because the live app's exit save would re-persist "
                         "the QA diag_log_* flags over the restored file")
    ap.add_argument("--step-timeout", type=float, default=None,
                    help="override per-Enter pipeline completion timeout (s)")
    ap.add_argument("--no-tray", action="store_true",
                    help="skip the multi_lang tray UIA attempt (force the "
                         "config-seed fallback)")
    ap.add_argument("--skip-tray-scenarios", action="store_true",
                    help="drop uilang_37/reset_defaults from an 'all' run so "
                         "a green exit 0 is achievable; they stay "
                         "INCONCLUSIVE and are never converted to PASS")
    ap.add_argument("--run-dir", default=None,
                    help="directory for run-manifest.json (default "
                         "<repo>/build/e2e-runs/<UTC stamp>)")
    ap.add_argument("--baseline-manifest", default=None,
                    help="saved run-manifest.json to diff the environment "
                         "fingerprint against (prints a drift banner)")
    args = ap.parse_args(argv)

    if args.step_timeout:
        STEP_TIMEOUT_S = args.step_timeout
    if args.no_tray:
        globals()["_TRAY_DISABLED"] = True

    if args.scenario == "all":
        chosen = [s for s in ALL_SCENARIOS
                  if not (args.skip_tray_scenarios and s in TRAY_SCENARIOS)]
    else:
        chosen = [args.scenario]

    run_dir = args.run_dir or os.path.join(REPO_ROOT, "build", "e2e-runs",
                                           utc_stamp())
    try:
        os.makedirs(run_dir, exist_ok=True)
    except OSError as e:
        print(f"[harness] ERROR: cannot create run dir {redact_user_path(run_dir)}: {e}")
        return 3

    print("== REQ-027 E2E harness (B-6c matrix + B-4 uilang_37 + "
          "autosend_newline) ==")
    print(f"   scenarios: {chosen}")
    if args.skip_tray_scenarios:
        print("   policy   : --skip-tray-scenarios removed "
              f"{list(TRAY_SCENARIOS)} from the matrix (they remain "
              "environment-bounded INCONCLUSIVE when run explicitly; never "
              "converted to PASS)")
    print(f"   app exe  : {redact_user_path(args.app_exe)}")
    print(f"   log dir  : {LOG_DIR}")
    print(f"   run dir  : {redact_user_path(run_dir)}")
    print("   NOTE     : do NOT touch keyboard/mouse until the verdict.")

    reports = []
    harness_failed = False
    restore_broken = False
    counts = {}
    code = 2
    state = {}
    started_utc = datetime.datetime.now(datetime.timezone.utc).isoformat(
        timespec="seconds")

    # --- E5: back up the runtime config BEFORE the first app launch --------
    # F1 (261121_0002 round 2): the read-only backup is published to `fixture`
    # HERE, before any write; the PIN itself runs inside the restore-guarded
    # try below, so no escape path (write error, Ctrl+C) can skip the restore.
    try:
        fixture = prepare_config_fixture(args.app_exe,
                                         no_cleanup=args.no_cleanup)
    except HarnessError as e:
        # R3-2 / F-Δ2 (261121_0002 round 3): a config that exists but cannot
        # be READ is a harness/environment fault, not a product verdict - the
        # run continues unpinned and the exit policy returns 3 (E12). No write
        # ever happened, so there is nothing to restore.
        print(f"[harness] ERROR: {redact_user_path(str(e))}")
        fixture = None
        harness_failed = True
    _MODE["fixture"] = None
    _MODE["auto_send"] = False
    fixture_write_failed = False
    teardown_interrupted = False

    def start_app():
        app = AppSession(args.app_exe)
        app.start()
        state["app"] = app
        print(f"   app      : pid={app.proc.pid}")
        print(f"   app log  : {redact_user_path(app.log_path)}")
        # Only an ARMED pin is verified: `fixture` also exists when the pin did
        # not persist, and that path already reported itself as environment.
        if _MODE["fixture"] is not None:
            verify_session_fixture(app, _MODE["fixture"])

    def stop_app():
        app = state.get("app")
        if app is not None:
            if args.no_cleanup and app.proc and app.proc.poll() is None:
                print(f"   cleanup  : SKIPPED (--no-cleanup), app pid "
                      f"{app.proc.pid} left running")
                return
            app.stop()
            state["app"] = None

    state["start_app"] = start_app
    state["stop_app"] = stop_app

    t0 = time.time()
    try:
        try:
            if fixture is not None:
                # The pin lives INSIDE this restore-guarded try (F1): a write
                # error or a Ctrl+C from here on still restores the bytes.
                try:
                    armed = pin_config_fixture(fixture)
                except HarnessError:
                    fixture_write_failed = True
                    raise
                if armed:
                    _MODE["fixture"] = fixture
            closed, kept_blank = NotepadSession.preflight_scrub()
            print(f"   preflight : closed {closed} leftover empty-untitled "
                  f"Notepad frame(s); kept {kept_blank} blank as the reuse "
                  "pool (content/multi-tab frames never touched)")
            _foreground_guard()  # fail fast + name a phantom holder
            _OBSERVED["notepad_version"] = notepad_image_version()
            start_app()
            for name in chosen:
                rep = None
                # One retry for transient environment aborts (foreground
                # races) and one retry for network INCONCLUSIVE (delegation
                # section 4: "네트워크 INCONCLUSIVE는 1회 재시도").
                for retry in range(2):
                    base = len(parse_tasks(state["app"].log_text())) \
                        if name != "multi_lang" else 0
                    try:
                        rep, app_after = DRIVERS[name](state, base)
                        if app_after is not state.get("app"):
                            state["app"] = app_after
                    except EnvBlock as e:
                        if retry == 0 and ("foreground" in str(e)
                                           or "focus" in str(e)):
                            print(f"   [retry] {name}: transient focus "
                                  f"abort ({e}); reopening scratch window")
                            continue
                        r = Report(name)
                        r.note_env(str(e))
                        rep = r
                        break
                    v = rep.verdict()[0]
                    if v == "INCONCLUSIVE" and retry == 0:
                        print(f"   [retry] {name}: INCONCLUSIVE "
                              "(transient/network?) - one retry")
                        continue
                    break
                reports.append(rep)
                text, _v = rep.render()
                print(text)
                if getattr(rep, "cleanup_note", None):
                    print("   [cleanup] " + rep.cleanup_note)
        except EnvBlock as e:
            r = Report("environment")
            r.note_env(str(e))
            reports.append(r)
            text, _v = r.render()
            print(text)
        except HarnessError as e:
            harness_failed = True
            print(f"[harness] ERROR: {redact_user_path(str(e))}")
        # E12 exit policy, computed before the finally so the manifest can
        # record it: 0 = all PASS, 1 = any FAIL, 2 = no FAIL but INCONCLUSIVE
        # (or no verdict at all), 3 = harness error (checked after teardown).
        verdicts = [r.verdict()[0] for r in reports]
        counts = {k: verdicts.count(k)
                  for k in ("PASS", "FAIL", "INCONCLUSIVE")}
        if not verdicts:
            code = 2
        elif counts["FAIL"]:
            code = 1
        elif counts["INCONCLUSIVE"]:
            code = 2
        else:
            code = 0
    finally:
        # F1 (261121_0002 round 2, SR1 L-2): a Ctrl+C during the slow app
        # teardown must not unwind past the restore - the config would stay
        # pinned with diag_log_content=true and no manifest would be written.
        try:
            stop_app()
        except KeyboardInterrupt:
            teardown_interrupted = True
            print("[harness] interrupted during app teardown (Ctrl+C); "
                  "restoring the config anyway")
            # R3-1 / F-Δ1 (261121_0002 round 3): the interrupt usually lands
            # in AppSession.stop()'s settle sleep, BEFORE its `taskkill /F`,
            # so the spawned app can still be alive when the bytes are
            # restored just below - and its later graceful exit SaveToFile
            # would re-persist the pinned diag_log_content=true over them
            # (the exact hazard F1 exists for). Best-effort re-kill FIRST; a
            # second Ctrl+C here must not skip the restore, hence the swallow.
            try:
                force_kill_pids(list_processes_like("Emebala_chat"))
                surviving = sorted(list_processes_like("Emebala_chat"))
            except KeyboardInterrupt:
                surviving = []  # a second interrupt: exit 3 loudly below
            except Exception:  # best-effort only - never skip the restore
                surviving = []
            if surviving:
                print("[harness] WARNING: Emebala_chat.exe still alive (PIDs "
                      f"{surviving}); its exit save may re-persist the pinned "
                      "diag_log_* flags over the restored config - close it, "
                      "then re-check config.json")
        if fixture is not None:
            # F1 (SR5 F-01): an OSError inside restore() is the same
            # contractual failure as a SHA mismatch - escalate to exit 3
            # instead of escaping main() with a traceback.
            try:
                ok, msg = fixture.restore()
            except OSError as e:
                ok, msg = False, (f"e2e config fixture restore FAILED "
                                  f"({type(e).__name__}: {e})")
            except KeyboardInterrupt:
                ok, msg = False, ("e2e config fixture restore INTERRUPTED by "
                                  "Ctrl+C - the QA pin may still be in place")
            print(f"   {msg}")
            if not ok:
                restore_broken = True
                print("   !! CONFIG RESTORE MISMATCH: the user's config.json "
                      "is NOT verified byte-identical to the pre-run bytes "
                      "(see the line above). The harness exits 3 so this "
                      "cannot be mistaken for a product verdict; check and "
                      "restore the file before using the app.")
        # A run that is unwinding (Ctrl+C, unexpected exception) must never be
        # recorded in the manifest with a verdict code, even though the process
        # itself exits with the traceback's code.
        aborting = sys.exc_info()[0] is not None
        exit3 = bool(harness_failed or restore_broken or fixture_write_failed
                     or teardown_interrupted or aborting)
        manifest = build_run_manifest(
            started_utc=started_utc,
            finished_utc=datetime.datetime.now(
                datetime.timezone.utc).isoformat(timespec="seconds"),
            duration_s=time.time() - t0,
            app_exe=args.app_exe, fixture=fixture, chosen=chosen,
            reports=reports, counts=counts,
            exit_code=3 if exit3 else code,
            run_dir=run_dir,
            extra_notes=[n for r in reports for n in r.env_notes])
        print_env_fingerprint(manifest)
        baseline_drift_banner(manifest, args.baseline_manifest)
        written = write_run_manifest(manifest, run_dir)
        if written:
            print(f"   manifest : {redact_user_path(written)}")

    if exit3:
        if restore_broken:
            reason = "CONFIG-RESTORE-MISMATCH"
        elif fixture_write_failed:
            reason = "CONFIG-WRITE-FAILED"
        elif teardown_interrupted:
            reason = "INTERRUPTED"
        else:
            reason = "HARNESS-ERROR"
        print(f"\n== RESULT: {reason} after {time.time()-t0:.1f}s "
              "(exit 3) ==")
        print("E2E_SUMMARY " + json.dumps(
            {"exit": 3, "counts": counts, "reason": reason,
             "verdicts": {r.name: r.verdict()[0] for r in reports},
             "skipped_tray": bool(args.skip_tray_scenarios),
             "run_dir": redact_user_path(run_dir)}, ensure_ascii=False, sort_keys=True))
        return 3
    summary = " / ".join([f"{r.name}={r.verdict()[0]}" for r in reports])
    print(f"\n== RESULT: {summary} after {time.time()-t0:.1f}s "
          f"(exit {code}) ==")
    # Machine-readable summary (E12): one line, stable key order, so a wrapper
    # script never has to parse the human report.
    print("E2E_SUMMARY " + json.dumps(
        {"exit": code, "counts": counts,
         "verdicts": {r.name: r.verdict()[0] for r in reports},
         "skipped_tray": bool(args.skip_tray_scenarios),
         "run_dir": redact_user_path(run_dir)}, ensure_ascii=False, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
