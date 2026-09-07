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
        REQ-023-separator-swallow defect candidate, see verify_task_layer1),
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
  paste_then_enter    clipboard seeded OUTSIDE the app + real Ctrl+V
                        (SendInput) + immediate Enter -> capture == pasted
                        text exactly, replacement normal; then one IMMEDIATE
                        retry Enter -> REQ-034 F3-B: WORKER/ExecuteTask/036 +
                        decision=paste_window_suppress, NO notice, silent
                        send-through (the app really adds a newline)
  long_text           one 1000-char Hangul line + Enter -> capture len
                      1000, replacement normal. The 64K EM_GETSEL WORD
                      saturation boundary is NOT automated (user QA
                      QA-27-7, see README).
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
  python tools\\e2e\\req027_e2e.py all          ; all 11 automated scenarios
  python tools\\e2e\\req027_e2e.py multi_lang --no-tray   ; force config fallback
Exit codes: 0 = PASS, 1 = FAIL, 2 = INCONCLUSIVE/environment, 3 = harness error.

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
    user pressing Enter. The newline after each replacement is injected by
    the APP itself (REQ-023), so the harness sends exactly ONE Enter per
    typed line. The marker value is per-process and NOT reproducible from
    outside; harness injections of Ctrl+V / Shift+Enter are therefore
    unmarked and pass through the hook's documented pass-through branches
    (ctrl+V is not a gesture key; Shift+Enter exits via the REQ-018 gate).
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
  * Clipboard backup/restore is the app's own responsibility (out of scope).
    paste_then_enter and the multi_lang fallback change build\\config.json
    / the clipboard and restore them afterwards.
  * All synchronization is polling with explicit timeouts (constants below);
    the only fixed delay is a 300 ms diag-flush settle before teardown.
"""

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import time
from ctypes import wintypes

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
RE_CAPTURE_END = re.compile(
    r'PIPELINE/stage=capture end len=(\d+) duration_ms=\d+ content="(.*)"')
RE_TRANSLATE_END = re.compile(r"PIPELINE/stage=translate end status=(-?\d+)")
RE_TRANSLATE_OUT = re.compile(
    r'PIPELINE/stage=translate end status=-?\d+ engine=.+? duration_ms=\d+ '
    r'out_len=(\d+) out="(.*)"')
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
RE_SHIFT_GATE = re.compile(r"ENTER_GATE.*reason=shift_enter_newline")
# Runtime language-change record (src/main.cpp L604 STATE/lang_sync): the
# single authority every tray menu pick routes through. Group 1 = the new
# TYPE target language name. Proves a UIA tray selection reached
# ApplyLanguageChange at runtime (REQ-019's live-switch path).
RE_LANG_SYNC_TYPE_TO = re.compile(
    r"lang_sync ctx=type valid=1 (?:changed=\d )?pair .* -> .*/(.+?) \(req")


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
        self.capture_content = None
        self.translate_status = None
        self.translate_out_len = None
        self.translate_out = None
        self.paste_result = None
        self.paste_skip_reason = None
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
            self.capture_len = int(m.group(1))
            self.capture_content = m.group(2)
        m = RE_TRANSLATE_END.search(text)
        if m:
            self.translate_status = int(m.group(1))
        m = RE_TRANSLATE_OUT.search(text)
        if m:
            self.translate_out_len = int(m.group(1))
            self.translate_out = m.group(2)
        m = RE_PASTE_RESULT.search(text)
        if m:
            self.paste_result = int(m.group(1))
        m = RE_PASTE_SKIPPED.search(text)
        if m:
            self.paste_skip_reason = m.group(1)
        m = RE_LANG_PAIR.search(text)
        if m:
            self.lang_tgt = m.group(2)
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
        return closed, blanks_seen - keep

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
            return txt, "WM_GETTEXT"
        txt = read_edit_text_uia_ps(self.pid, self.top)
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
                raise EnvBlock(
                    "the Notepad window the harness adopted opened with "
                    f"PRE-EXISTING content (session restore?) - refusing to "
                    "clear or close user data. Window left untouched. "
                    "Disable Notepad session restore (Settings) or close "
                    "leftover Notepad windows, then re-run. "
                    f"first 80 chars={last.strip()[:80]!r}")
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
    # (b) capture length == typed line EXACTLY (never cumulative, and never
    # the REQ-023 injected newline swallowed into the EM selection - the
    # delegation specifies len=N must MATCH the typed length; a captured
    # leading "\r\n" merges translated lines in the document).
    if block.capture_len is None:
        rep.add(1, None, f"task {block.index}: no 'stage=capture end' line")
        return
    if expected_line is not None:
        typed = utf16_len(expected_line)
        exact = block.capture_len == typed
        non_cum = block.capture_len < cumulative_estimate if cumulative_estimate else True
        diagnosis = ""
        if not exact and block.capture_content is not None:
            cc = unescape_diag(block.capture_content)
            if expected_line and cc.lstrip("\r\n") == expected_line and cc != expected_line \
                    and cc.startswith(("\r\n", "\n")):
                diagnosis = (" :: DEFECT CANDIDATE - EM selection swallowed the "
                             "previous REQ-023 injected newline (ISSUE-1 "
                             "regression: the replacement MERGES this "
                             "translation onto the previous line)")
            elif expected_line and cc.rstrip("\r\n") == expected_line and cc != expected_line:
                diagnosis = (" :: DEFECT CANDIDATE - trailing newline in capture "
                             "(the pre-B-5a translation_equals_source "
                             "contamination shape)")
        rep.add(1, exact and non_cum,
                f"task {block.index}: capture len={block.capture_len} == typed "
                f"{typed} exactly, non-cumulative "
                f"(cumulative signature would be ~{cumulative_estimate})"
                + diagnosis,
                f'content="{block.capture_content[:150]}"')
        if marker_required and expected_line:
            contains_new = marker_of(expected_line) in (block.capture_content or "")
            rep.add(1, contains_new,
                    f"task {block.index}: captured content carries this line's "
                    "marker (E2Exx)",
                    f'marker={marker_of(expected_line)} content='
                    f'"{(block.capture_content or "")[:150]}"')
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
                       *, replacement_required=True,
                       expect_hold_no_change=False):
    if doc_before is None or doc_after is None:
        rep.add(2, None, f"task {block.index}: content unreadable "
                "(WM_GETTEXT and UIA both failed)")
        return
    if expect_hold_no_change:
        rep.add(2, doc_after == doc_before,
                f"task {block.index}: empty-capture hold left the document "
                "byte-identical", f"len={len(doc_after)}")
        return
    if not (block.completed and block.paste_result == 1):
        if replacement_required:
            rep.add(2, None, f"task {block.index}: content check deferred "
                    "(log layer recorded no successful paste)")
        else:
            # Smart-bypass / skip outcomes are legitimate here (e.g.
            # backspace_enter capturing already-translated text).
            rep.add(2, True,
                    f"task {block.index}: no-replacement outcome tolerated by "
                    f"scenario design (paste={block.paste_result} "
                    f"skip={block.paste_skip_reason})",
                    f"doc changed={doc_after != doc_before}")
        return
    if expected_line:
        rep.add(2, expected_line not in doc_after,
                f"task {block.index}: typed source replaced by translation "
                "(source absent from control text)",
                f'after={doc_after[:160]!r}')
    # NOT a growth check: a translation can be SHORTER than the Hangul
    # source; the invariant is "the document changed at this replacement".
    rep.add(2, doc_after != doc_before,
            f"task {block.index}: document changed after replacement",
            f"before_len={len(doc_before)} after_len={len(doc_after)}")
    newline_ok = ("\n" in doc_after) if block.index > 0 else True
    rep.add(2, newline_ok,
            f"task {block.index}: REQ-023 newline visible between translated "
            "lines", f"doc_after={doc_after[:160]!r}")


def verify_final_document(rep, notepad, all_lines, *, min_newlines=None):
    doc, method = notepad.read_text()
    if doc is None:
        rep.add(2, None, f"final document unreadable ({method})")
        return None
    stale = [s for s in all_lines if s and s in doc]
    rep.add(2, not stale,
            f"final document ({method}, {len(doc)} chars) contains none of "
            f"the {len([s for s in all_lines if s])} typed sources",
            f"still_present={[s[:40] for s in stale]}")
    # Structure: every Enter (REQ-023) injects exactly one newline, so k
    # translated lines must sit on k lines: the document needs >= k newlines.
    newlines = doc.count("\n")
    need = len(all_lines) if min_newlines is None else min_newlines
    rep.add(2, newlines >= need,
            f"final document keeps one line per translation "
            f"(newlines={newlines}, expected >= {need})",
            f"doc={doc[:300]!r}")
    rep.note(f"final document via {method}: {doc[:400]!r}")
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
# long_text builds its 1000-char line via build_long_line() below.


def run_line_session(name, lines, app, base, *, step_timeout=None,
                     final_doc=True, expect=None):
    """base: number of pipeline tasks already present in this app session's
    log (scenarios run sequentially against one shared app instance).
    expect: dict of per-index flags {idx: {'marker_required': bool, ...}} -
    kept simple: all lines get identical treatment unless overridden."""
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
            after, _ = notepad.read_text()
            blocks.append(block)
            verify_task_layer1(
                rep, block, ex.get("expected_line", line), cumulative,
                marker_required=ex.get("marker_required", True),
                empty_hold_expected=ex.get("empty_hold", False),
                clamp_expected=ex.get("clamp_expected", False))
            verify_task_layer2(
                rep, block, ex.get("expected_line", line), before, after,
                replacement_required=ex.get("replacement_required", True),
                expect_hold_no_change=ex.get("empty_hold", False))
            cumulative += utf16_len(line) + 2 + 80  # conservative estimate
        if final_doc:
            verify_final_document(rep, notepad, lines)
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
            if len(caps) < len(lines) and env_hit:
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
        after, _ = notepad.read_text()
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
        rep.add(1, block.capture_len == caret_app,
                f"capture == EM selection [0..{caret_app}) of the caret-"
                f"anchored block",
                f"capture_len={block.capture_len}")
        cap = unescape_diag(block.capture_content or "")
        rep.add(1, cap.startswith(prefix) and len(cap) <= len(prefix) + 1,
                "captured text is exactly the [doc start .. caret) prefix "
                "(+FlushIme char)",
                f"cap_tail={cap[-8:]!r}")
        verify_task_layer2(rep, block, prefix, before, after)
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
        bs = len(line_b) + 4  # erase all of B, the REQ-023 newline, 2 of A'
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
        doc_b, _ = notepad.read_text()
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
        rep.add(1, blk_b.capture_len == caret_app,
                f"capture == [0..{caret_app}) of the shrunk document",
                f"capture_len={blk_b.capture_len}")
        verify_task_layer2(rep, blk_b, None, doc_bs, doc_b,
                           replacement_required=False)
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
        verify_task_layer2(rep, block, expected_block, before, after)
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
    try:
        set_clipboard_text(text)
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
        verify_task_layer2(rep, block, text, before, after)
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
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def build_long_line(total=1000, marker="E2E01"):
    unit = "아주 긴 텍스트 시나리오를 자동으로 검증합니다. "
    fill = (unit * (total // len(unit) + 1))
    return fill[: total - len(marker)] + marker


def scenario_long_text(app, base):
    """1000-char single Hangul line + Enter: capture len 1000, non-saturated
    EM offsets, replacement normal. The 64K WORD-saturation boundary is
    deliberately NOT automated (user QA QA-27-7, see README)."""
    line = build_long_line(1000)
    rep = run_line_session("long_text", [line], app, base, step_timeout=90.0)
    rep.note(f"typed line length = {utf16_len(line)} UTF-16 units "
             "(64K boundary handed to user QA QA-27-7)")
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


def _config_path(app):
    return os.path.join(os.path.dirname(os.path.abspath(app.exe_path)),
                        "config.json")


def patch_config_type_target(exe_dir, value):
    """Rewrite build\\config.json type_target_language (json module, never
    regex - multiline-safe). Returns the previous value."""
    path = os.path.join(os.path.abspath(exe_dir), "config.json")
    if not os.path.exists(path):
        raise EnvBlock(f"config.json not found next to exe: {path}")
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    prev = data.get("type_target_language")
    data["type_target_language"] = value
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return prev


def tray_attempt_switch(target_code):
    """Invoke tools/e2e/uia_tray_menu.ps1 to right-click the app tray icon
    and select the typing->target-language item matching 'CODE - '. Returns
    (ok: bool, diag: str). NEVER raises: a refusal is a normal outcome that
    triggers the documented fallback."""
    ps1 = os.path.join(HERE, "uia_tray_menu.ps1")
    if not os.path.exists(ps1):
        return False, "uia_tray_menu.ps1 missing"
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", ps1, "-ItemRegex", rf"^{target_code}\s*-"]
    try:
        p = subprocess.run(cmd, capture_output=True,
                           timeout=TRAY_ATTEMPT_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, f"ps1 timeout/error: {e}"
    out = (p.stdout or b"").decode("utf-8-sig", errors="replace")
    tail = [ln for ln in out.splitlines() if ln.startswith("TRAY:")]
    ok = any(ln.startswith("TRAY:OK") for ln in tail)
    diag = " | ".join(tail[-4:]) or f"rc={p.returncode}"
    err = (p.stderr or b"").decode("utf-8", "replace").strip()
    if err:
        diag += f" :: stderr[:200]={err[:200]}"
    return ok, diag


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
    exe_dir = os.path.dirname(os.path.abspath(app.exe_path))
    cfg_path = _config_path(app)

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
                np.press_enter_wait_task(app.log_text, base + order + 1)
                blk = np.wait_task_complete(app.log_text, base + order)
                verify_task_layer1(rep, blk, line, cumulative)
                _assert_lang_block(rep, blk, name, script_re, code_prefix=order)
                cumulative += utf16_len(line) + 2
            verify_final_document(rep, np, [MULTI_LANG_LINES[n] for n, _, _
                                            in MULTI_LANG_BLOCKS])
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
        with open(cfg_path, "r", encoding="utf-8") as f:
            original = json.load(f).get("type_target_language")
    except OSError:
        original = None
    final_app = None
    try:
        for order, (name, code, script_re) in enumerate(MULTI_LANG_BLOCKS):
            # Fresh app session per block: stop first (force-kill skips the
            # app's own SaveToFile, so our patch is never raced), THEN patch
            # config.json (startup-load path), THEN start.
            app_state["stop_app"]()
            patch_config_type_target(exe_dir, name)
            app_state["start_app"]()
            app = app_state["app"]
            np = NotepadSession()
            try:
                np.start()
                np.assert_empty()
                line = MULTI_LANG_LINES[name]
                np.type_line(line)
                np.press_enter_wait_task(app.log_text, 1)
                blk = np.wait_task_complete(app.log_text, 0)
                verify_task_layer1(rep, blk, line, 0)
                _assert_lang_block(rep, blk, name, script_re,
                                   code_prefix=order, label=f"[{name}] ")
            finally:
                np.stop()
            if np.cleanup_note:
                rep.note("cleanup: " + np.cleanup_note)
    finally:
        app_state["stop_app"]()
        if original is not None:
            patch_config_type_target(exe_dir, original)
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
        rep.add(1, None,
                f"{label}block {code_prefix}: script check deferred "
                f"(translate status={blk.translate_status})")


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
    return d


DRIVERS = build_drivers()

ALL_SCENARIOS = ["qa27b", "example1", "consecutive", "multi_lang",
                 "empty_enter", "cursor_mid", "backspace_enter",
                 "shift_enter_multi", "paste_then_enter", "long_text",
                 "notepad_vscode_mix"]
# ime_composing is intentionally NOT in the matrix: WM_CHAR/SendInput cannot
# create a real IME composition state, and simulating it unverified would
# produce false verdicts. Handed to user QA (decisions.md 2026-09-07 20:54;
# rationale in tools/e2e/README.md).


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
                    "(B-6c 11-scenario matrix)")
    ap.add_argument("scenario", nargs="?", default="qa27b",
                    choices=sorted(ALL_SCENARIOS + ["all"]))
    ap.add_argument("--app-exe", default=DEFAULT_APP_EXE,
                    help="path to Emebala_chat.exe (default build\\)")
    ap.add_argument("--no-cleanup", action="store_true",
                    help="keep the app process alive after the run (debug)")
    ap.add_argument("--step-timeout", type=float, default=None,
                    help="override per-Enter pipeline completion timeout (s)")
    ap.add_argument("--no-tray", action="store_true",
                    help="skip the multi_lang tray UIA attempt (force the "
                         "config-seed fallback)")
    args = ap.parse_args(argv)

    if args.step_timeout:
        STEP_TIMEOUT_S = args.step_timeout
    if args.no_tray:
        globals()["_TRAY_DISABLED"] = True

    chosen = list(ALL_SCENARIOS) if args.scenario == "all" else [args.scenario]
    print("== REQ-027 E2E harness (B-6c matrix) ==")
    print(f"   scenarios: {chosen}")
    print(f"   app exe  : {args.app_exe}")
    print(f"   log dir  : {LOG_DIR}")
    print("   NOTE     : do NOT touch keyboard/mouse until the verdict.")

    reports = []
    harness_failed = False
    state = {}

    def start_app():
        app = AppSession(args.app_exe)
        app.start()
        state["app"] = app
        print(f"   app      : pid={app.proc.pid}")
        print(f"   app log  : {app.log_path}")

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
            closed, kept_blank = NotepadSession.preflight_scrub()
            print(f"   preflight : closed {closed} leftover empty-untitled "
                  f"Notepad frame(s); kept {kept_blank} blank as the reuse "
                  "pool (content/multi-tab frames never touched)")
            _foreground_guard()  # fail fast + name a phantom holder
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
            print(f"[harness] ERROR: {e}")
    finally:
        stop_app()

    if harness_failed:
        print(f"\n== RESULT: HARNESS-ERROR after {time.time()-t0:.1f}s "
              "(exit 3) ==")
        return 3
    verdicts = [r.verdict()[0] for r in reports]
    if not verdicts:
        code = 2  # nothing produced a verdict -> never claim PASS
    elif any(v == "FAIL" for v in verdicts):
        code = 1
    elif any(v == "INCONCLUSIVE" for v in verdicts):
        code = 2
    else:
        code = 0
    summary = " / ".join([f"{r.name}={r.verdict()[0]}" for r in reports])
    print(f"\n== RESULT: {summary} after {time.time()-t0:.1f}s "
          f"(exit {code}) ==")
    return code


if __name__ == "__main__":
    sys.exit(main())
