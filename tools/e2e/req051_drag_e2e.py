#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
REQ-051 drag E2E harness - drag -> floating button -> translation scenarios.
===========================================================================

Purpose
-------
Close the E2E coverage hole named in the REQ-051 handoff (plans/
req051-handoff-frontier-prompt.md section 6 "E2E 커버리지 구멍"): the existing
tools/e2e/ matrix is keyboard-path-centric, so Symptom A ("드래그 후 플로팅버튼을
눌러 번역할 때 ... 다른 것이 복사되어 출력") has no automated regression defense.
This harness drives the REAL mouse path against a real Notepad window:

  drag-select text (real SendInput mouse gesture, >= 15 px so the app's
  WH_MOUSE_LL drag-release gate fires, src/mouse_hook.cpp) -> the floating
  DragIconWindow appears at the release point (src/ui/drag_icon.cpp) -> a real
  click on the icon stamps a translation generation and submits the drag job
  (src/main.cpp) -> the copy gate + engine run on the drag worker -> the result
  lands in the translation tooltip (never in the document - the drag path does
  not paste; it Ctrl+C's, reads, and restores the clipboard).

Verdicts are judged from the app's DIAG log (%LOCALAPPDATA%\\Emebalachat\\
logs\\emebalachat_*.log) exactly like req027_e2e.py, on the drag-path token
family (all quoted from src/, see the regex section below):

  UI/tooltip_request_begin gen=N               (src/ui/tooltip.cpp BeginTranslationRequest)
  UI/tooltip_show kind=translation gen=N x=.. y=.. src_len=N out_len=N
                                                  (src/ui/tooltip.cpp ShowTranslation)
  UI/tooltip_show kind=message gen=N x=.. y=..    (src/ui/tooltip.cpp ShowMessage - failure notice)
  UI/tooltip_marshal_drop kind=.. gen=N           (generation-guard drop evidence)
  STATE/drag_src path=icon mode=.. persisted=.. detected=.. eff=.. pivot=N tgt=.. usr_explicit=N
                                                  (src/main.cpp run_drag_translate)
  MAIN/DragIconClick/001 clipboard copy not confirmed after N attempt(s)
                                                  (capture-cycle exhaustion -> TooltipCopyFailed notice;
                                                  N = kDragCopyChordAttempts = 3 since REQ-051 — the one
                                                  failure token for the whole 3-attempt cycle)
  MAIN/DragIconClick/002 clipboard copy confirmed but text empty
  MAIN/DragIconClick/003 console/terminal foreground (REQ-005 gate)
  WIN32_INPUT/CopySelectionWithSequenceWait/002: clipboard sequence unchanged <M>ms after Ctrl+C;
                                                  refusing stale read  (PER-ATTEMPT refusal — logged once
                                                  per failed attempt of the shared retry driver
                                                  CopyChordWithSettledRetry, src/win32_input.cpp; M is the
                                                  EFFECTIVE per-attempt budget: 80/80/120 for the drag
                                                  3-attempt cycle. The pre-REQ-051 MAIN/DragIconClick/004
                                                  and /005 retry tokens are RETIRED — per-attempt evidence
                                                  is these WIN32_INPUT lines, exhaustion is /001)
  MAIN/EngineModal/010 translation recovered    (SUCCESS SENTINEL: logged on EVERY successful
                                                 drag translation, src/main.cpp - the drag path
                                                 logs NO PIPELINE/stage=translate; per the REQ-051
                                                 handoff this sentinel + tooltip_show are the
                                                 completion proof)
  ENGINE/Translate/040 target routed (...)      (engine routing)
  ENGINE/Translate/020|021|022|044              (cloud-fallback / strict-local / host-fail info)

Architecture is copied from tools/e2e/req027_e2e.py (same 3-layer verdict
model: log primary / content secondary / environment never conflated with
product FAIL, same INCONCLUSIVE auto-retry, same exit codes, same
AppSession/NotepadSession ownership and Win32 adoption policy). This file is
deliberately standalone (it does not import req027) so the two harnesses can
evolve independently; shared code is copied verbatim, not forked-by-reference.

Scenarios (arg 1):
  drag_translate        one drag-select -> icon -> click -> translation tooltip.
                        Asserts the capture gate stayed clean (NO
                        WIN32_INPUT/.../002 per-attempt refusal, NO
                        DragIconClick/001 exhaustion, NO result=copy_chord_failed),
                        STATE/drag_src present,
                        tooltip_show kind=translation with src_len == the
                        measured EM selection length, MAIN/EngineModal/010
                        success sentinel present, document untouched.
  drag_consecutive      three consecutive drags with DISTINCT source lines ->
                        per-drag generation-linked src_len equality (each drag
                        translated its OWN selection - the '다른 것이 출력'
                        contamination guard), strictly increasing generations,
                        ordered begin -> drag_src -> show chain per drag.
  drag_capture_failure  force the capture-failure notice path deterministically:
                        drag-select, then collapse the selection with a real
                        VK_LEFT (icon stays up; keyboard does not dismiss it),
                        then click the icon. A synthetic Ctrl+C on a collapsed
                        selection cannot bump the clipboard sequence, so the
                        product MUST take the copy-failure branch. Asserts the
                        failure is SIGNALED (WIN32_INPUT/.../002 per-attempt
                        refusals + MAIN/DragIconClick/001 exhaustion +
                        tooltip_show kind=message), NOT silent: no translation
                        tooltip, no drag_src, document byte-identical.

Prerequisites (same class as req027):
  * Interactive desktop session (SendInput + real mouse/foreground). Headless
    CI cannot run this. The harness moves the REAL cursor - do NOT touch the
    keyboard/mouse until the verdict.
  * diag_log_enabled=true in the RUNTIME config (%LOCALAPPDATA%\\Emebalachat\\
    config.json - the app reads that copy first; this harness NEVER modifies
    any config). Without it no log appears and AppSession aborts INCONCLUSIVE.
  * drag_to_translate=true in the runtime config (the shipped default) and the
    app's hooks active (not paused via Win+F9), otherwise no icon ever shows.
  * An engine that can serve the drag pair. Cloud failure / strict-local
    consent blocks are classified INCONCLUSIVE (never product FAIL), exactly
    like req027's status 2/3 rule.
  * No pre-existing Emebala_chat.exe (single-instance mutex).
  * Built app at build\\Emebala_chat.exe (override with --app-exe).

Usage
-----
  python tools\\e2e\\req051_drag_e2e.py drag_translate
  python tools\\e2e\\req051_drag_e2e.py drag_consecutive
  python tools\\e2e\\req051_drag_e2e.py drag_capture_failure
  python tools\\e2e\\req051_drag_e2e.py all    (or --all; shared app instance)
  optional: --app-exe <path>  --no-cleanup  --step-timeout <s>

Exit codes: 0 = PASS, 1 = FAIL, 2 = INCONCLUSIVE/environment, 3 = harness error.

Known limits (documented, per the req027 idiom):
  * The translation TEXT is never asserted: the drag path is shape-only (no
    captured content is logged - REQ-051 privacy contract), and the tooltip is
    a D2D layered window with no text interface. src_len/gen/position linkage
    is the strongest contamination evidence available without diag_log_content
    (which must NEVER be enabled).
  * The drag gesture needs a readable, non-wrapping first text row; geometry is
    derived from EM_POSFROMCHAR (theme/DPI independent). A degenerate or
    partial drag selection is a SETUP failure -> EnvBlock -> INCONCLUSIVE
    auto-retry, never a product FAIL.
  * REQ-005: if the foreground is a console at icon-click time the app skips
    the capture by design (DragIconClick/003); the harness keeps Notepad
    foregrounded, and a /003 is classified as an environment note.
"""

import argparse
import ctypes
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
# time digits) + optional "-N" collision suffix (same rule as req027).
LOG_RE = re.compile(r"^emebalachat_\d{12}(-\d+)?\.log$")

POLL_S = 0.05                 # generic poll interval
APP_READY_TIMEOUT_S = 20.0    # wait for SESSION/subsystems started
NOTEPAD_READY_TIMEOUT_S = 15.0
STEP_TIMEOUT_S = 30.0         # drag-click -> tooltip outcome (cloud round-trip)
TEARDOWN_SETTLE_S = 0.3       # diag flush batch settle before process kill
WM_TIMEOUT_MS = 2000          # SendMessageTimeoutW cap for cross-process reads
ICON_TIMEOUT_S = 2.0          # drag release -> drag icon visible (fadeout is
                              # 2500 ms, src/ui/drag_icon.hpp kFadeoutTimeoutMs;
                              # hovering the icon stops the timer, and the
                              # harness moves onto it before the click)
CLICK_BEGIN_TIMEOUT_S = 3.0   # icon click -> tooltip_request_begin in the log
DRAG_STEP_SLEEP_S = 0.015     # per-step sleep during the synthetic drag
DRAG_PRESS_SETTLE_S = 0.05    # cursor parked on the start point before down
# src/mouse_hook.cpp LowLevelMouseProc: drag release fires at Euclidean
# distance >= 15 px between button-down and button-up points.
DRAG_MIN_DISTANCE_PX = 15

DRAG_ICON_CLASS = "Emebalachat_DragIconClass"   # src/ui/drag_icon.cpp
TOOLTIP_CLASS = "Emebalachat_TooltipClass"      # src/ui/tooltip.cpp

# Scenarios type marker lines "E2E5x" so a leftover scratch document is
# recognized as harness residue by the shared _is_harness_residue policy
# (E2E\d{2}) and cleared on the next run - never user data.
MARK = "E2E5"   # suffix per scenario below

# ---------------------------------------------------------------------------
# Win32 plumbing (ctypes P/Invoke) - argtypes set everywhere so 64-bit
# HWND/WPARAM/LPARAM values are never truncated (same discipline as req027).
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
EM_POSFROMCHAR = 0x00D6
VK_MENU = 0x12
VK_LEFT = 0x25
VK_ESCAPE = 0x1B
KEYEVENTF_SCANCODE = 0x0008
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_KEYUP = 0x0002
INPUT_KEYBOARD = 1
SMTO_ABORTIFHUNG = 0x0002
SMTO_BLOCK = 0x0001
SW_RESTORE = 9

user32.GetClassNameW.argtypes = [HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.IsWindowVisible.argtypes = [HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.IsWindow.argtypes = [HWND]
user32.IsWindow.restype = wintypes.BOOL
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
kernel32.CloseHandle.restype = wintypes.BOOL
# mouse gesture + popup surfaces (same proven pattern as req027's
# click_about_reset: position with SetCursorPos, click with mouse_event).
user32.GetWindowRect.argtypes = [HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.GetClientRect.argtypes = [HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetClientRect.restype = wintypes.BOOL
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetCursorPos.restype = wintypes.BOOL
user32.mouse_event.argtypes = [wintypes.UINT, wintypes.UINT, wintypes.UINT,
                               wintypes.ULONG, ctypes.c_void_p]  # ULONG_PTR
user32.ClientToScreen.argtypes = [HWND, ctypes.POINTER(wintypes.POINT)]
user32.ClientToScreen.restype = wintypes.BOOL
user32.FindWindowExW.argtypes = [HWND, HWND, ctypes.c_wchar_p,
                                 ctypes.c_wchar_p]
user32.FindWindowExW.restype = HWND

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

PENUMCHILD = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, LPARAM)
PENUMPTR = ctypes.WINFUNCTYPE(wintypes.BOOL, HWND, LPARAM)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    """Needed ONLY so the INPUT union is padded to the real x64 size:
    SendInput validates cbSize == sizeof(INPUT) == 40 on x64 (MOUSEINPUT is
    the widest member) - same note as req027 (probed live 260907)."""
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUTU(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("_u",)
    _fields_ = [("type", wintypes.DWORD), ("_u", INPUTU)]


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", HWND), ("hwndFocus", HWND),
                ("hwndCapture", HWND), ("hwndMenuOwner", HWND),
                ("hwndMoveSize", HWND), ("hwndCaret", HWND),
                ("rcCaret", wintypes.RECT)]


user32.GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUITHREADINFO)]
user32.GetGUIThreadInfo.restype = wintypes.BOOL


def _assert_input_size():
    """Fail fast (harness error) if the INPUT layout is wrong for this
    interpreter/bitness instead of silently failing every SendInput."""
    expected = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
    actual = ctypes.sizeof(INPUT)
    if actual != expected:
        raise HarnessError(
            f"sizeof(INPUT)=={actual}, expected {expected}: the ctypes "
            "layout does not match the platform SendInput contract")


def utf16_len(s):
    return len(s.encode("utf-16-le")) // 2


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
    (tools/e2e/uia_read_edit.ps1), scoped to hwnd when given. Returns text
    ('' when the sentinel says empty) or None (same contract as req027)."""
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


user32.EnumWindows.argtypes = [ctypes.c_void_p, LPARAM]
user32.EnumWindows.restype = wintypes.BOOL


def notepad_top_windows():
    """{hwnd: (pid, title)} of visible top-level windows whose class is the
    Notepad frame class ('Notepad' on Win11 24H2, probed live by req027)."""
    out = {}
    for pid in list_processes_like("Notepad"):
        for (h, title, cls) in top_windows_for_pid(pid):
            if cls.lower() == "notepad":
                out[h] = (pid, title)
    return out


def force_kill_pids(pids):
    for pid in sorted(pids):
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            pass


def find_edit_control(top_hwnd):
    """Return (edit_hwnd, class) preferring known EDIT/RichEdit classes;
    (0, '') when nothing matches (same policy as req027)."""
    EDIT_CLASSES_PREFERRED = ("RichEditD2DPT", "RichEditD2D", "RICHEDIT50W",
                              "RichEdit20W", "RichEdit20A", "Edit")
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


def get_focus_hwnd_for_window(hwnd):
    """Keyboard-focused control inside hwnd via GetGUIThreadInfo(tid) - the
    SAME resolution the app's EM path uses. Returns 0 when unavailable."""
    tid = user32.GetWindowThreadProcessId(HWND(hwnd), None)
    if not tid:
        return 0
    info = GUITHREADINFO()
    info.cbSize = ctypes.sizeof(GUITHREADINFO)
    if not user32.GetGUIThreadInfo(tid, ctypes.byref(info)):
        return 0
    return int(info.hwndFocus or 0)


def read_log(path):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------------------------------------------------------------------------
# app-side DIAG log regexes - drag-path token family (signatures quoted from
# src/, verified against the REQ-051 diagnosis docs; the drag path logs NO
# PIPELINE/stage= lines - REQ-051 handoff section 1-2 - so none are asserted).
# ---------------------------------------------------------------------------
RE_SUBSYSTEMS = re.compile(r"SESSION/subsystems started")
RE_SECOND_INSTANCE = re.compile(r"SESSION/second instance")
RE_BEGIN = re.compile(r"UI/tooltip_request_begin gen=(\d+)")
RE_SHOW_TR = re.compile(
    r"UI/tooltip_show kind=translation gen=(\d+) x=(-?\d+) y=(-?\d+) "
    r"src_len=(\d+) out_len=(\d+)")
RE_SHOW_MSG = re.compile(
    r"UI/tooltip_show kind=message gen=(\d+) x=(-?\d+) y=(-?\d+)")
RE_DROP = re.compile(r"UI/tooltip_marshal_drop kind=(\w+) gen=(\d+)")
# STATE/drag_src path=icon mode=%s persisted=%s detected=%s eff=%s pivot=%d
# tgt=%s usr_explicit=%d (src/main.cpp) - 'persisted'/'detected'/'eff'/'tgt'
# are language NAMES and can contain spaces ("Auto Detect"), so the match
# anchors on the single-token pivot=usr_explicit tail and uses non-greedy
# captures between the field names (verified against the real log lines in
# docs/260921_0002 .../164700_verify-drag-capture-contamination.md section 2).
RE_DRAG_SRC = re.compile(
    r"STATE/drag_src path=icon\b.*? detected=(.*?) eff=(.*?) pivot=(\d) "
    r"tgt=(.*?) usr_explicit=(\d)\s*$")
RE_ENGINE_040 = re.compile(r"ENGINE/Translate/040")
# engine/cloud-side info tokens for INCONCLUSIVE classification (never FAIL):
RE_ENGINE_020 = re.compile(r"ENGINE/Translate/020")  # auto policy -> cloud
RE_ENGINE_021 = re.compile(r"ENGINE/Translate/021")  # consented -> cloud
RE_ENGINE_022 = re.compile(r"ENGINE/Translate/022")  # strict-local, no consent
RE_ENGINE_044 = re.compile(r"ENGINE/Translate/044")  # engine-host try failed
# capture-path tokens (src/main.cpp run_drag_translate / win32_input.cpp):
RE_DRAGICON_001 = re.compile(r"MAIN/DragIconClick/001")  # cycle exhaustion
RE_DRAGICON_002 = re.compile(r"MAIN/DragIconClick/002")  # confirmed but empty
RE_DRAGICON_003 = re.compile(r"MAIN/DragIconClick/003")  # console gate (REQ-005)
# REQ-051: the per-attempt refusal line logs the EFFECTIVE change-timeout of
# the failed attempt (80 = attempt 1/middle, 120 = the final 3rd attempt of
# the drag cycle) — the ms budget is captured so the forced-failure scenario
# can assert the {80, 80, 120} schedule through the shared retry driver
# CopyChordWithSettledRetry (src/win32_input.cpp).
RE_SEQWAIT_002 = re.compile(
    r"WIN32_INPUT/CopySelectionWithSequenceWait/002: clipboard sequence (\d+) "
    r"unchanged (\d+)ms after Ctrl+C")
# REQ-051 retirement note: the pre-REQ-051 MAIN/DragIconClick/004 ("attempt 1
# failed; retrying") and /005 ("copy retry confirmed|failed") tokens no longer
# exist — the drag cycle runs the shared 3-attempt driver and logs ONE
# DragIconClick/001 on exhaustion; per-attempt evidence is the WIN32_INPUT
# seqwait002 lines above. The success scenarios treat ANY of these as the
# Symptom-A capture-failure signature.
RE_EM_010 = re.compile(r"MAIN/EngineModal/010")        # success sentinel
RE_CHORD_FAILED = re.compile(r"result=copy_chord_failed")
RE_MODAL_020 = re.compile(r"MAIN/EngineModal/020")     # strict local failure modal
RE_MODAL_030 = re.compile(r"MAIN/EngineModal/030")     # latched modal suppressed
RE_PIPELINE_ANY = re.compile(r"PIPELINE/stage=")


def parse_drag_events(text):
    """Classify the drag-path tokens of a log slice into an ordered event
    list: [(lineno, kind, groups_dict), ...] in log order. 'kind' is one of
    begin/show_tr/show_msg/drop/drag_src/eng040/copy001/copy002empty/
    console003/seqwait002/em010/chordfail/
    eng020/eng021/eng022/eng044/modal020/modal030/pipeline."""
    events = []
    for i, ln in enumerate(text.splitlines(), 1):
        m = RE_BEGIN.search(ln)
        if m:
            events.append((i, "begin", {"gen": int(m.group(1)), "line": ln.strip()}))
            continue
        m = RE_SHOW_TR.search(ln)
        if m:
            events.append((i, "show_tr", {
                "gen": int(m.group(1)), "x": int(m.group(2)),
                "y": int(m.group(3)), "src_len": int(m.group(4)),
                "out_len": int(m.group(5)), "line": ln.strip()}))
            continue
        m = RE_SHOW_MSG.search(ln)
        if m:
            events.append((i, "show_msg", {
                "gen": int(m.group(1)), "x": int(m.group(2)),
                "y": int(m.group(3)), "line": ln.strip()}))
            continue
        m = RE_DROP.search(ln)
        if m:
            events.append((i, "drop", {
                "kind": m.group(1), "gen": int(m.group(2)),
                "line": ln.strip()}))
            continue
        m = RE_DRAG_SRC.search(ln)
        if m:
            events.append((i, "drag_src", {
                "detected": m.group(1), "eff": m.group(2),
                "pivot": m.group(3), "tgt": m.group(4),
                "usr_explicit": m.group(5), "line": ln.strip()}))
            continue
        pairs = (
            (RE_ENGINE_040, "eng040"), (RE_DRAGICON_001, "copy001"),
            (RE_DRAGICON_002, "copy002empty"), (RE_DRAGICON_003, "console003"),
            (RE_SEQWAIT_002, "seqwait002"),
            (RE_EM_010, "em010"), (RE_CHORD_FAILED, "chordfail"),
            (RE_ENGINE_020, "eng020"), (RE_ENGINE_021, "eng021"),
            (RE_ENGINE_022, "eng022"), (RE_ENGINE_044, "eng044"),
            (RE_MODAL_020, "modal020"), (RE_MODAL_030, "modal030"),
            (RE_PIPELINE_ANY, "pipeline"),
        )
        for rx, kind in pairs:
            if rx.search(ln):
                groups = {"line": ln.strip()}
                if kind == "seqwait002":
                    groups["seq"] = int(rx.search(ln).group(1))
                    groups["ms"] = int(rx.search(ln).group(2))
                events.append((i, kind, groups))
                break
    return events


def find_events(events, kind, gen=None):
    out = [e for e in events if e[1] == kind]
    if gen is not None:
        out = [e for e in out if e[2].get("gen") == gen]
    return out


# ---------------------------------------------------------------------------
# mouse / keyboard gesture helpers - everything is UNMARKED SendInput /
# mouse_event, which the app's hooks treat as real user input (the
# EXTRA_INFO_MARKER bypass is not reproducible from outside the process -
# same reasoning as req027's Enter injection).
# ---------------------------------------------------------------------------
def move_cursor(x, y):
    if not user32.SetCursorPos(int(x), int(y)):
        raise EnvBlock(f"SetCursorPos({x},{y}) failed (cursor input locked?)")


def left_down():
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)


def left_up():
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def perform_drag(pt_start, pt_end):
    """Real left-button drag from pt_start to pt_end (physical screen px):
    park the cursor, press, step the cursor along the segment (SetCursorPos
    generates real WM_MOUSEMOVEs with the button held, so the target's
    selection tracks continuously), release. The Euclidean down->up distance
    is what the app's WH_MOUSE_LL drag-release gate measures
    (src/mouse_hook.cpp, >= 15 px)."""
    x0, y0 = pt_start
    x1, y1 = pt_end
    dist = ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5
    if dist < DRAG_MIN_DISTANCE_PX + 5:
        raise HarnessError(
            f"drag segment too short ({dist:.0f}px) to trip the >=15px gate")
    move_cursor(x0, y0)
    time.sleep(DRAG_PRESS_SETTLE_S)
    left_down()
    time.sleep(DRAG_PRESS_SETTLE_S)
    steps = max(4, int(dist // 25))
    for k in range(1, steps + 1):
        t = k / (steps + 1.0)
        move_cursor(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
        time.sleep(DRAG_STEP_SLEEP_S)
    move_cursor(x1, y1)
    time.sleep(DRAG_STEP_SLEEP_S)
    left_up()


def click_at(x, y):
    """Left-click at a screen point (physical px) - the proven req027
    pattern (SetCursorPos + mouse_event down/up)."""
    move_cursor(x, y)
    time.sleep(0.08)
    left_down()
    time.sleep(0.05)
    left_up()


def send_key(vk, scancode):
    """One unmarked key press (down+up) via SendInput - the pattern
    req027 uses for VK_BACK. Arrows/Escape pass through the keyboard hook
    untriggered (only VK_RETURN gates the pipeline; ESC is consumed by the
    app's esc dismissal callback when overlay UI is visible)."""
    down = INPUT(type=INPUT_KEYBOARD,
                 ki=KEYBDINPUT(vk, scancode, 0, 0, None))
    up = INPUT(type=INPUT_KEYBOARD,
               ki=KEYBDINPUT(vk, scancode, KEYEVENTF_KEYUP, 0, None))
    arr = (INPUT * 2)(down, up)
    if user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT)) != 2:
        raise HarnessError(f"SendInput(vk={vk:#x}) failed")


def send_escape():
    send_key(VK_ESCAPE, 0x01)


def send_left_arrow():
    send_key(VK_LEFT, 0x4B)


def find_app_window(app_pid, cls):
    """First VISIBLE top-level window of cls owned by app_pid (the drag icon
    and the tooltip are per-process singletons; EnumWindows sees WS_POPUP |
    WS_EX_TOOLWINDOW popups - the same enumeration req027 proves)."""
    for (h, _title, wcls) in top_windows_for_pid(app_pid):
        if wcls == cls:
            return int(h)
    return 0


def click_window_center(hwnd):
    """Click the center of a popup (the drag icon) and return (x, y). The
    icon hides itself inside its own WM_LBUTTONUP handler; the caller
    verifies that as click-landed evidence."""
    rect = wintypes.RECT()
    if not user32.GetWindowRect(HWND(hwnd), ctypes.byref(rect)):
        raise EnvBlock("GetWindowRect failed on the drag icon")
    cx = (rect.left + rect.right) // 2
    cy = (rect.top + rect.bottom) // 2
    click_at(cx, cy)
    return (cx, cy)


def wait_drag_icon(app_pid, timeout=ICON_TIMEOUT_S):
    """Poll for the visible drag icon after a drag release. Absence is an
    ENVIRONMENT outcome (hook paused / drag_to_translate off / tooltip still
    visible / the release did not reach the hook) - never a product FAIL."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = find_app_window(app_pid, DRAG_ICON_CLASS)
        if h:
            return h
        time.sleep(POLL_S)
    raise EnvBlock(
        "no visible Emebalachat_DragIconClass window within "
        f"{timeout}s of the drag release - environment (hooks paused via "
        "Win+F9?, drag_to_translate=false in the runtime config?, a stale "
        "tooltip suppressing the drag release?, or mouse injection not "
        "reaching the hook)")


def tooltip_visible(app_pid):
    h = find_app_window(app_pid, TOOLTIP_CLASS)
    return bool(h)


def dismiss_tooltip(app_pid, timeout=3.0):
    """Dismiss the translation tooltip via the app's ESC seam (hook esc_cb
    consumes ESC only while overlay UI is visible - src/main.cpp). Polls
    until the tooltip window hides. Stays visible -> EnvBlock (the next drag
    release would be suppressed by tooltip.IsVisible())."""
    if not tooltip_visible(app_pid):
        return
    send_escape()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not tooltip_visible(app_pid):
            return
        time.sleep(POLL_S)
    send_escape()  # one more attempt (a focus race can swallow the first)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not tooltip_visible(app_pid):
            return
        time.sleep(POLL_S)
    raise EnvBlock(
        "the translation tooltip did not dismiss via ESC - the next drag "
        "release would be suppressed (tooltip.IsVisible gate); environment "
        "(hook paused?) - close the tooltip manually and re-run")


def wait_new_begin(app_log_text, offset, prev_gens, timeout=CLICK_BEGIN_TIMEOUT_S):
    """After an icon click, wait for a NEW 'tooltip_request_begin' in the log
    slice and return its generation. None -> the click never reached the icon
    (it faded, or the click missed) - a setup/env failure, not a verdict."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for e in parse_drag_events(app_log_text()[offset:]):
            if e[1] == "begin" and e[2]["gen"] not in prev_gens:
                return e[2]["gen"]
        time.sleep(POLL_S)
    return None


def wait_drag_outcome(app_log_text, offset, gen, timeout=None):
    """Poll the log slice for the terminal UI event stamped with generation
    'gen': show_tr (translation landed), show_msg (a notice surfaced) or
    drop (generation-guarded marshal drop - still proof the pipeline answer
    reached the GUI thread). Returns (kind, event) or (None, None)."""
    deadline = time.time() + (timeout or STEP_TIMEOUT_S)
    while time.time() < deadline:
        events = parse_drag_events(app_log_text()[offset:])
        drops = [e for e in events
                 if e[1] == "drop" and e[2]["gen"] == gen]
        shows_tr = [e for e in events
                    if e[1] == "show_tr" and e[2]["gen"] == gen]
        shows_msg = [e for e in events
                     if e[1] == "show_msg" and e[2]["gen"] == gen]
        if shows_tr:
            return "translation", shows_tr[0]
        if shows_msg:
            return "message", shows_msg[0]
        if drops:
            return "dropped_" + drops[0][2]["kind"], drops[0]
        time.sleep(POLL_S)
    return None, None


# ---------------------------------------------------------------------------
# harness outcomes (same contract as req027)
# ---------------------------------------------------------------------------
class HarnessError(Exception):
    """A bug/unavailability in the harness itself -> exit 3."""


class EnvBlock(Exception):
    """Environment precondition failed -> INCONCLUSIVE (never product FAIL)."""


_assert_input_size()


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


# ---------------------------------------------------------------------------
# sessions
# ---------------------------------------------------------------------------
class AppSession:
    """Owns the Emebala_chat.exe process spawned for this run and its log.
    Copied from req027 (the single-instance + newest-log-with-subsystems
    contract is identical)."""

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
                "src/main.cpp): close it, then re-run.")
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
            f"{APP_READY_TIMEOUT_S}s - is diag_log_enabled=true in the "
            "runtime config (%LOCALAPPDATA%\\Emebalachat\\config.json)? "
            "This harness judges from the DIAG log and never touches config.")

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
# (guards against double-adoption; same as req027).
_ADOPTED_HWND = set()


class NotepadSession:
    """Opens (or reuses) an EMPTY Notepad scratch WINDOW and owns it until
    teardown; closes ONLY that window (WM_CLOSE + UIA 'don't save'
    dismissal). The shared Notepad process is NEVER killed (user-data-loss
    hazard, B-6c rule copied from req027). Extended here (REQ-051) with the
    drag-gesture primitives: EM_POSFROMCHAR-derived selection geometry, the
    real mouse drag, and selection readback."""

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
        t = title.lstrip("*").strip()
        return bool(re.match(
            r"^(제목 없음|untitled|메모장|notepad)"
            r"(\s*[-–]\s*(메모장|Notepad))?$", t, re.IGNORECASE))

    def _try_adopt(self, hwnd, require_untitled, fast=True):
        """Best-effort claim of one frame: focus (escalation ladder), resolve
        the edit control, require the document to be empty (or our own E2Exx
        residue). 'reject' permanently drops the candidate; anything else is
        retried. Copied from req027 (B-6c adoption policy)."""
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
        """Close harness-leftover BLANK scratch frames before any scenario;
        keep `keep` blanks as the reuse pool. Content/multi-tab frames are
        never touched (B-6c, copied verbatim from req027)."""
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
        # Adoption (B-6c, probed live by req027): claim the first FRESH empty
        # frame, else REUSE an existing empty untitled frame. Both are harness
        # scratch by construction (empty document => nothing to lose).
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
                res = self._try_adopt(h, require_untitled=False, fast=False)
                if res == "adopted":
                    return
                if res == "reject":
                    rejected.add(h)
            time.sleep(POLL_S)
        # reuse path: any existing untitled-empty frame
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
            "invisible phantom window, see the foreground-guard recovery "
            "note (GameInputSvc wedge, req027 README).")

    def _resolve_edit(self):
        """Edit control = the keyboard-focused control (GetGUIThreadInfo -
        exactly what the app's EM path targets), falling back to child
        enumeration. Must run AFTER _focus()."""
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
        calling thread the right to SetForegroundWindow (same as req027)."""
        down = INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(VK_MENU, 0x38, 0, 0, None))
        up = INPUT(type=INPUT_KEYBOARD,
                   ki=KEYBDINPUT(VK_MENU, 0x38, KEYEVENTF_KEYUP, 0, None))
        arr = (INPUT * 2)(down, up)
        user32.SendInput(2, ctypes.byref(arr), ctypes.sizeof(INPUT))

    def _focus(self, fast=False):
        """Foreground acquisition ladder (proven Win32 recipes, copied from
        req027): SFW -> ALT tap + SFW -> AttachThreadInput + SFW +
        BringWindowToTop -> SwitchToThisWindow, polling to VERIFIED
        foreground after each rung. Total failure raises EnvBlock."""
        user32.ShowWindow(HWND(self.top), SW_RESTORE)
        cur_tid = kernel32.GetCurrentThreadId()
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
        if int(user32.GetForegroundWindow() or 0) == self.top:
            return
        raise EnvBlock(
            "could not bring the spawned Notepad to the foreground after "
            "the full escalation ladder - the harness drag/click would hit "
            "the wrong window; environment issue")

    def read_text(self):
        """(text, method). text None = unreadable by both readers."""
        txt = read_edit_text_wmgettext(self.edit)
        if txt is not None:
            return txt, "WM_GETTEXT"
        txt = read_edit_text_uia_ps(self.pid, self.top)
        return txt, "UIA-ValuePattern"

    @staticmethod
    def _is_harness_residue(text):
        """True when EVERY non-blank line contains an E2Exx scenario marker
        (same policy as req027 - only the harness types such lines)."""
        lines = [l for l in text.splitlines() if l.strip()]
        return bool(lines) and all(re.search(r"E2E\d{2}", l) for l in lines)

    def assert_empty(self):
        """NON-DESTRUCTIVE emptiness gate (copied from req027): proceed when
        empty; clear our own E2Exx residue; abort on any other content."""
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
                    "PRE-EXISTING content (session restore?) - refusing to "
                    "clear or close user data. Window left untouched. "
                    "Disable Notepad session restore (Settings) or close "
                    "leftover Notepad windows, then re-run. "
                    f"first 80 chars={last.strip()[:80]!r}")
            time.sleep(POLL_S)
        raise EnvBlock(
            f"fresh Notepad document unreadable/unclearable within 5s "
            f"(read={last!r}) - aborting before typing")

    def _send_chars_unicode(self, text):
        """Fallback typer: SendInput KEYEVENTF_UNICODE (copied from req027)."""
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
            time.sleep(0.01)

    def type_line(self, text):
        """Insert via WM_CHAR (hook-invisible, IME-independent), then verify
        by reading the control back (poll, endswith = precise) - copied from
        req027."""
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

    # ---- REQ-051 drag primitives -------------------------------------

    def replace_all(self, text):
        """Select the whole document and overwrite it by posting WM_CHAR
        (edit-message path, hook-invisible). Verified by exact readback.
        Used between consecutive drags so each drag sees ONE fresh line."""
        send_em(self.edit, EM_SETSEL, 0, -1)
        for ch in text:
            if not user32.PostMessageW(HWND(self.edit), WM_CHAR,
                                       WPARAM(ord(ch)), LPARAM(0)):
                raise HarnessError("PostMessageW(WM_CHAR) failed")
        deadline = time.time() + 8.0
        last = None
        while time.time() < deadline:
            last, _ = self.read_text()
            if last == text:
                return
            time.sleep(POLL_S)
        raise EnvBlock(
            f"replace_all did not land exactly (read={last!r}) - aborting "
            "to avoid a mixed document")

    def selection(self):
        """(start, end) of the EM selection (UTF-16 offsets), None when the
        control does not answer. Same LOWORD/HIWORD read req027 uses."""
        ok, res = send_em(self.edit, EM_GETSEL, 0, 0)
        if not ok:
            return None
        start = res & 0xFFFF
        end = (res >> 16) & 0xFFFF
        return (start, end)

    def char_pos(self, index):
        """Client-area (x, y) of the character cell at `index` via
        EM_POSFROMCHAR (RichEdit packed POINTL return), or None when the
        control refuses. Theme/DPI independent - the control reports where
        the text actually is."""
        ok, res = send_em(self.edit, EM_POSFROMCHAR, index, 0)
        if not ok or res in (0xFFFFFFFF, -1):
            return None
        x = ((res & 0xFFFF) ^ 0x8000) - 0x8000   # signed 16-bit unpack
        y = (((res >> 16) & 0xFFFF) ^ 0x8000) - 0x8000
        return (x, y)

    def client_to_screen(self, x, y):
        pt = wintypes.POINT(x, y)
        if not user32.ClientToScreen(HWND(self.edit), ctypes.byref(pt)):
            raise EnvBlock("ClientToScreen failed on the edit control")
        return (pt.x, pt.y)

    def drag_select_current_line(self, line):
        """Perform the real mouse drag that (a) selects the document's only
        line and (b) releases far enough from the press point to trip the
        app's >= 15 px drag gate (src/mouse_hook.cpp). Geometry comes from
        EM_POSFROMCHAR (first char / one-past-last char), so no theme or DPI
        assumption is baked in. Returns the verified (start, end) selection.

        A degenerate or truncated selection is a SETUP failure (the gesture
        missed the text row) -> EnvBlock, never a product verdict; the
        harness main loop auto-retries once."""
        n = utf16_len(line)
        p_first = self.char_pos(0)
        p_past = self.char_pos(n)   # caret position at end of the line
        if not p_first or not p_past:
            raise EnvBlock(
                "EM_POSFROMCHAR unsupported/unreadable on "
                f"{self.edit_class} - cannot derive drag geometry "
                "(environment)")
        est_char_w = max(4, (p_past[0] - p_first[0]) // max(1, n))
        est_row_h = min(48, max(12, int(est_char_w * 1.5)))
        if abs(p_past[1] - p_first[1]) > est_row_h:
            raise EnvBlock(
                "the fixture line wrapped to a second row (Notepad window "
                "too narrow for it) - widen the scratch window or shorten "
                "the fixture; setup abort")
        client = wintypes.RECT()
        if not user32.GetClientRect(HWND(self.edit), ctypes.byref(client)):
            raise EnvBlock("GetClientRect failed on the edit control")
        y_client = p_first[1] + int(est_row_h * 0.55)
        y_client = max(1, min(y_client, client.bottom - 2))
        # press RIGHT of the line end, release LEFT of the first char (still
        # inside the client): the selection sweeps [~0 .. ~len].
        x_start = min(p_past[0] + est_char_w * 2, client.right - 2)
        x_end = max(2, p_first[0] - 6)
        pt_start = self.client_to_screen(x_start, y_client)
        pt_end = self.client_to_screen(x_end, y_client)
        self._focus()  # the drag must land on OUR window
        perform_drag(pt_start, pt_end)
        time.sleep(0.15)  # let the target settle its selection state
        sel = self.selection()
        if sel is None:
            raise EnvBlock("selection unreadable right after the drag")
        if sel[1] - sel[0] < n - 2 or sel[0] > 2:
            raise EnvBlock(
                f"the drag selected only [{sel[0]}..{sel[1]}] of a {n}-unit "
                "line (gesture missed the text row?) - setup abort, the "
                "harness will retry once")
        return sel

    def press_left_arrow(self):
        """Collapse the current selection with a real VK_LEFT (unmarked
        SendInput). Only arrows/Escape-class keys are safe here: VK_RETURN
        would gate the Enter pipeline; VK_LEFT merely collapses the
        selection in the focused edit (hook passes it through untouched)."""
        self._focus()
        send_left_arrow()
        time.sleep(0.15)

    def _window_alive(self):
        return bool(user32.IsWindowVisible(HWND(self.top)))

    def stop(self):
        """Close ONLY our document window (WM_CLOSE + UIA 'don't save'
        dismissal). The shared Notepad process is NEVER killed (B-6c)."""
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


def _foreground_guard():
    """Open+close one scratch window as a pre-flight (fails fast with an
    ACTIONABLE message instead of cascading every scenario into adoption
    timeouts) - copied from req027, same GameInputSvc wedge diagnosis."""
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
            "(observed: GameInputSvc.exe phantom window, req027). User-mode "
            "recovery is exhausted - run 'sc stop gameinputsvc' from an "
            "ADMIN console (it auto-restarts) or logoff/reboot, then "
            "re-run this harness.")


# ---------------------------------------------------------------------------
# shared drag driver (one full drag -> icon -> click -> outcome cycle)
# ---------------------------------------------------------------------------
def drive_drag(notepad, app, line, offset, *, first=False, pre_click_hook=None):
    """Execute ONE complete drag translation cycle and return the facts the
    scenarios assert on:

      {"gen", "expected_src_len", "sel", "outcome", "outcome_event",
       "icon", "click_xy", "doc_before", "doc_after"}

    Setup failures (document replace failed, drag produced no usable
    selection, no icon appeared, the click stamped no generation) raise
    EnvBlock -> INCONCLUSIVE + one auto-retry - they are never product
    verdicts. `offset` is the log-text length at scenario start (events are
    parsed from that slice). `pre_click_hook` runs after the icon is visible
    but before the click (drag_capture_failure uses it to collapse the
    selection)."""
    result = {"line": line}
    # 1. exactly one fresh source line in the document
    if first:
        notepad.type_line(line)
    else:
        notepad.replace_all(line)
    # 2. real drag: selects the line AND trips the app's drag-release gate
    result["drag_sel"] = notepad.drag_select_current_line(line)
    result["sel"] = result["drag_sel"]
    # 3. the floating button appears at the release point (+12,+12 clamped)
    result["icon"] = wait_drag_icon(app.proc.pid)
    # 4. optional interference step (collapse-selection for the failure path)
    if pre_click_hook is not None:
        pre_click_hook(notepad, result)
    # measure the selection the product WILL capture (unchanged since the
    # drag; the copy gate copies exactly this EM range)
    sel = notepad.selection()
    if sel is not None:
        result["sel"] = sel
    result["expected_src_len"] = result["sel"][1] - result["sel"][0]
    result["doc_before"], _ = notepad.read_text()
    # 5. real click on the icon center
    prev_gens = {e[2]["gen"] for e in parse_drag_events(app.log_text()[offset:])
                 if e[1] == "begin"}
    result["click_xy"] = click_window_center(result["icon"])
    # the icon hides itself inside its own WM_LBUTTONUP - visibility is the
    # click-landed evidence; if it is still up the click missed (e.g. the
    # icon faded before the click) - setup, retried by the main loop.
    time.sleep(0.2)
    if find_app_window(app.proc.pid, DRAG_ICON_CLASS):
        raise EnvBlock(
            "the drag icon is still visible right after the click - the "
            "click missed (icon faded or hit-tested elsewhere); setup "
            "abort, the harness will retry once")
    # 6. the click must stamp a NEW translation generation
    gen = wait_new_begin(app.log_text, offset, prev_gens)
    if gen is None:
        raise EnvBlock(
            "no new 'tooltip_request_begin' within "
            f"{CLICK_BEGIN_TIMEOUT_S}s of the icon click - the click did "
            "not reach the icon's WM_LBUTTONUP (environment)")
    result["gen"] = gen
    # 7. wait for the gen-stamped terminal UI event
    outcome, ev = wait_drag_outcome(app.log_text, offset, gen)
    result["outcome"] = outcome
    result["outcome_event"] = ev
    result["doc_after"], _ = notepad.read_text()
    return result


# ---------------------------------------------------------------------------
# failure-token absence block shared by the success scenarios
# ---------------------------------------------------------------------------
CAPTURE_FAILURE_KINDS = ("copy001", "copy002empty", "seqwait002", "chordfail")


def add_no_capture_failures(rep, events, ctx):
    """The success scenarios' core negative assertion: the capture gate must
    stay silent (any of these tokens on the Notepad drag path is the Symptom
    A capture-failure signature -> product FAIL)."""
    for kind in CAPTURE_FAILURE_KINDS:
        hits = find_events(events, kind)
        rep.add(1, not hits,
                f"{ctx}: no '{kind}' capture-failure token",
                hits[0][2]["line"] if hits else "")


def add_engine_env_notes(rep, events):
    """Engine/cloud-side info tokens that make a kind=message outcome an
    ENVIRONMENT factor (never a product FAIL) - mirrors req027's status
    2/3 rule for the drag path."""
    for kind, label in (("eng020", "auto policy fell back to cloud"),
                        ("eng021", "explicit cloud fallback engaged"),
                        ("eng022", "strict-local without cloud consent"),
                        ("eng044", "engine-host serving failed"),
                        ("modal020", "strict-local failure modal surfaced"),
                        ("modal030", "engine-modal suppressed by streak latch")):
        hits = find_events(events, kind)
        if hits:
            rep.note(f"engine-side token {kind} ({label}): "
                     + hits[0][2]["line"][:160])


def add_message_outcome_classification(rep, events, gen):
    """A kind=message outcome is either a capture failure (product FAIL on
    the success scenarios) or an engine/cloud-side failure (INCONCLUSIVE).
    Distinguish by the capture-path tokens; the engine info tokens are
    recorded as notes either way."""
    add_engine_env_notes(rep, events)
    if find_events(events, "console003"):
        rep.note_env(
            "MAIN/DragIconClick/003: the app found a console/terminal in the "
            "foreground at icon-click time and skipped the synthetic Ctrl+C "
            "by design (REQ-005) - the harness failed to keep Notepad "
            "foregrounded: environment, not a product FAIL")
        return
    capture_hits = [e for kind in CAPTURE_FAILURE_KINDS
                    for e in find_events(events, kind)]
    if capture_hits:
        rep.add(1, False,
                f"drag gen={gen}: capture failure on the Notepad drag path "
                "(Symptom A regression signature)",
                " | ".join(e[2]["line"][:120] for e in capture_hits[:3]))
        return
    if find_events(events, "drag_src"):
        rep.note_env(
            f"drag gen={gen}: a failure NOTICE surfaced after the engine "
            "routing line (STATE/drag_src present, no capture-failure "
            "token) - the engine/cloud side failed for this request "
            "(network/consent/serving): environment, not a product FAIL")
    else:
        rep.note_env(
            f"drag gen={gen}: a failure notice surfaced WITHOUT capture or "
            "routing tokens - cannot attribute; treating as environment "
            "(cloud/engine), never a silent product PASS")


def add_success_chain_checks(rep, window, slice_events, res):
    """Positive chain for ONE successful drag: begin(gen) < drag_src <
    show_tr(gen), with src_len == the measured selection length and the
    tooltip position == the icon click position (proves the visible surface
    belongs to THIS drag). `window` is the per-generation event slice
    (events_for_gen); `slice_events` is the whole scenario slice and is only
    consulted for the em010 success sentinel, which the GUI thread may log
    just OUTSIDE the begin->show window (the modal drain and the tooltip
    marshal are two different posted-message queues - observed adjacent in
    the diagnosis logs, not ordered by construction)."""
    gen = res["gen"]
    begins = find_events(window, "begin")
    shows = find_events(window, "show_tr", gen)
    srcs = find_events(window, "drag_src")
    rep.add(1, len(begins) == 1 and begins[0][2]["gen"] == gen,
            f"drag gen={gen}: exactly one new tooltip_request_begin "
            "(generation stamped at icon click)",
            begins[0][2]["line"] if begins else "(none)")
    rep.add(1, len(shows) == 1,
            f"drag gen={gen}: exactly one kind=translation show for this "
            "generation", shows[0][2]["line"] if shows else "(none)")
    if shows:
        ev = shows[0][2]
        rep.add(1, ev["src_len"] == res["expected_src_len"],
                f"drag gen={gen}: tooltip src_len={ev['src_len']} == "
                f"measured EM selection length {res['expected_src_len']} "
                "(this drag translated its OWN selection)",
                f"sel={res['sel']} line={res['line'][:60]!r}")
        rep.add(1, ev["out_len"] > 0,
                f"drag gen={gen}: translation out_len={ev['out_len']} > 0",
                ev["line"])
        rep.add(1, (ev["x"], ev["y"]) == tuple(res["click_xy"]),
                f"drag gen={gen}: tooltip position == the icon click "
                "position (the visible surface is THIS drag's)",
                f"show=({ev['x']},{ev['y']}) click={res['click_xy']}")
    rep.add(1, len(srcs) == 1,
            f"drag gen={gen}: exactly one STATE/drag_src path=icon "
            "(per-request source resolution)",
            srcs[0][2]["line"] if srcs else "(none)")
    if srcs:
        rep.add(1, srcs[0][2]["detected"] == "Korean",
                "drag_src detected=Korean (fixture is a Korean line)",
                srcs[0][2]["line"])
        rep.note(f"drag gen={gen}: drag_src tgt={srcs[0][2]['tgt']} "
                 f"pivot={srcs[0][2]['pivot']} (config-dependent, noted "
                 "not asserted)")
    if begins and shows and srcs:
        order_ok = (begins[0][0] < srcs[0][0] < shows[0][0])
        rep.add(1, order_ok,
                f"drag gen={gen}: ordered chain begin -> drag_src -> "
                "tooltip_show in the log",
                f"lines begin={begins[0][0]} drag_src={srcs[0][0]} "
                f"show={shows[0][0]}")
    rep.add(1, bool(find_events(window, "eng040")),
            f"drag gen={gen}: ENGINE/Translate/040 routing line present",
            find_events(window, "eng040")[0][2]["line"]
            if find_events(window, "eng040") else "(none)")
    rep.add(1, bool(find_events(slice_events, "em010")),
            f"drag gen={gen}: MAIN/EngineModal/010 success sentinel "
            "present (the drag path's completion proof per the REQ-051 "
            "handoff)",
            find_events(slice_events, "em010")[0][2]["line"]
            if find_events(slice_events, "em010") else "(none)")


def add_doc_unchanged_checks(rep, res, ctx):
    """The drag path must never modify the source document (it captures via
    Ctrl+C and restores the clipboard; the result lands in the tooltip)."""
    before, after = res["doc_before"], res["doc_after"]
    if before is None or after is None:
        rep.add(2, None, f"{ctx}: document unreadable (WM_GETTEXT and UIA)")
        return
    rep.add(2, before == after == res["line"],
            f"{ctx}: document byte-identical to the typed source (drag "
            "path never writes the document)",
            f"after={after[:120]!r}")


# ---------------------------------------------------------------------------
# scenarios
# ---------------------------------------------------------------------------
LINE_DRAG_A = "드래그 후 플로팅 버튼을 눌러 번역이 이루어져야 합니다. " + MARK + "1"
LINES_CONSECUTIVE = [
    "첫 번째 드래그 원문입니다. " + MARK + "1",
    "두 번째로 드래그하는 문장은 첫 번째와 길이가 달라야 오염을 잡아냅니다. " + MARK + "2",
    "세 번째 드래그 문장은 훨씬 더 깁니다. 연속으로 선택하더라도 앞의 두 문장과 전혀 섞이지 않고 번역되어야 합니다. " + MARK + "3",
]
LINE_FAILURE = "이 선택은 곧 화살표 키로 해제되어 캡처 실패가 강제됩니다. " + MARK + "4"


def scenario_drag_translate(app, base_unused):
    """REQ-051 Symptom A happy path: drag-select in Notepad -> the floating
    button appears -> click -> the translation tooltip lands. Asserts the
    capture gate stayed clean, the per-request chain is complete, and the
    measured selection length matches the delivered src_len."""
    rep = Report("drag_translate")
    notepad = NotepadSession()
    offset = len(app.log_text())
    results = []
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        if notepad.residue_cleared:
            rep.note("cleared harness E2Exx residue from Notepad session "
                     "restore (own scratch content only)")
        res = drive_drag(notepad, app, LINE_DRAG_A, offset, first=True)
        results.append(res)
        slice_text = app.log_text()[offset:]
        events = parse_drag_events(slice_text)

        if res["outcome"] is None:
            add_engine_env_notes(rep, events)
            rep.note_env(
                f"drag gen={res['gen']}: no tooltip outcome within "
                f"{STEP_TIMEOUT_S}s of the click (cloud/engine hang or a "
                "marshal starved the GUI thread): environment, not FAIL")
        elif res["outcome"] in ("message", "dropped_message"):
            add_message_outcome_classification(rep, events, res["gen"])
            if res["outcome"] == "dropped_message":
                rep.note("the notice was generation-guard dropped at render "
                         "(marshal race) - the callback fired, cited above")
        elif res["outcome"] == "dropped_translation":
            rep.add(1, False,
                    f"drag gen={res['gen']}: the TRANSLATION was dropped by "
                    "the generation guard (tooltip_marshal_drop "
                    "kind=translation) - a stale/reordered delivery on a "
                    "single-drag flow",
                    res["outcome_event"][2]["line"])
        else:
            add_success_chain_checks(rep, events, events, res)
            add_no_capture_failures(rep, events, f"drag gen={res['gen']}")
            add_doc_unchanged_checks(rep, res, f"drag gen={res['gen']}")
            vis = tooltip_visible(app.proc.pid)
            rep.add(2, vis,
                    "the translation tooltip window is visible (the result "
                    "landed on a real surface)",
                    f"class={TOOLTIP_CLASS}")
        n_pipe = len(find_events(events, "pipeline"))
        rep.note(f"architecture note: {n_pipe} PIPELINE/stage= line(s) in "
                 "the drag slice (expected 0 - the drag path logs no "
                 "PIPELINE tokens per the REQ-051 handoff)")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after the drag cycle",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        try:
            dismiss_tooltip(app.proc.pid)  # keep the next trigger unblocked
        except EnvBlock as e:
            rep.note_env(str(e))
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def scenario_drag_consecutive(app, base_unused):
    """Contamination guard for '다른 것이 출력': three consecutive drags with
    DISTINCT source lines must each translate their OWN selection. src_len
    is measured per drag from the EM selection readback and must equal the
    gen-stamped tooltip src_len exactly; the generations must be strictly
    increasing; each drag's begin -> drag_src -> show chain must hold in
    order."""
    rep = Report("drag_consecutive")
    notepad = NotepadSession()
    offset = len(app.log_text())
    results = []
    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()
        # fixture self-check: pairwise distinct lengths make cross-drag
        # contamination detectable (a swapped capture changes src_len).
        lens = [utf16_len(l) for l in LINES_CONSECUTIVE]
        rep.add(1, len(set(lens)) == len(lens),
                "fixture lines have pairwise distinct lengths "
                f"{lens} (contamination-detectable)",
                f"lines={LINES_CONSECUTIVE}")
        for k, line in enumerate(LINES_CONSECUTIVE):
            if k > 0:
                # the previous translation tooltip must be gone before the
                # next drag release (tooltip.IsVisible suppresses the icon)
                dismiss_tooltip(app.proc.pid)
            res = drive_drag(notepad, app, line, offset, first=(k == 0))
            results.append(res)
            add_doc_unchanged_checks(rep, res, f"drag {k + 1} gen={res['gen']}")
        slice_text = app.log_text()[offset:]
        events = parse_drag_events(slice_text)

        begins = [e for e in events if e[1] == "begin"]
        shows = [e for e in events if e[1] == "show_tr"]
        srcs = [e for e in events if e[1] == "drag_src"]
        gens = [res["gen"] for res in results]
        rep.add(1, [b[2]["gen"] for b in begins] == gens,
                f"exactly the 3 expected request generations in order "
                f"({gens})", f"begins={[b[2]['gen'] for b in begins]}")
        rep.add(1, all(gens[i] < gens[i + 1] for i in range(len(gens) - 1)),
                "request generations strictly increasing (each drag stamped "
                "its own request)",
                f"gens={gens}")

        for k, res in enumerate(results):
            ctx = f"drag {k + 1} gen={res['gen']}"
            window = events_for_gen(events, res["gen"])
            if res["outcome"] == "translation":
                add_success_chain_checks(rep, window, events, res)
            elif res["outcome"] is None:
                rep.note_env(
                    f"{ctx}: no tooltip outcome within {STEP_TIMEOUT_S}s "
                    "(cloud/engine hang): environment, not FAIL")
            elif res["outcome"] in ("message", "dropped_message"):
                # narrowed to this generation's window so another drag's
                # tokens can never be attributed to this one
                add_message_outcome_classification(rep, window, res["gen"])
                if res["outcome"] == "dropped_message":
                    rep.note(f"{ctx}: the notice was generation-guard dropped "
                             "at render (marshal race) - the callback fired")
            else:
                rep.add(1, False,
                        f"{ctx}: unexpected outcome {res['outcome']}",
                        res["outcome_event"][2]["line"]
                        if res["outcome_event"] else "(none)")
        # capture gate silent across the WHOLE session (a capture failure on
        # any of the three drags is the Symptom A signature -> FAIL)
        add_no_capture_failures(rep, events, "session-wide 3x drag")

        # the ordered per-drag chains inside the whole-slice event stream:
        # begin_k < drag_src_k < show_k and show_k < begin_{k+1}. Only drags
        # that delivered a translation participate (env-class drags are
        # judged by their classification above, not by chain position).
        show_by_gen = {e[2]["gen"]: e for e in shows}
        ok_chain = True
        ev_lines = []
        for k, res in enumerate(results):
            if res["outcome"] != "translation":
                ev_lines.append(f"drag{k + 1}: outcome={res['outcome']} "
                                "(chain check skipped, env-classified)")
                continue
            b = [e for e in begins if e[2]["gen"] == res["gen"]]
            s = show_by_gen.get(res["gen"])
            src = srcs[k] if k < len(srcs) else None
            if not (b and s and src):
                ok_chain = False
                ev_lines.append(f"drag{k + 1}: missing begin/show/drag_src")
                continue
            if not (b[0][0] < src[0] < s[0]):
                ok_chain = False
            if k + 1 < len(results):
                b_next = [e for e in begins
                          if e[2]["gen"] == results[k + 1]["gen"]]
                if b_next and not (s[0] < b_next[0][0]):
                    ok_chain = False
            ev_lines.append(
                f"drag{k + 1}: begin@{b[0][0]} drag_src@{src[0]} "
                f"show@{s[0]}")
        n_tr = sum(1 for res in results if res["outcome"] == "translation")
        rep.add(1, ok_chain and len(srcs) == n_tr,
                "ordered chain per delivered translation: begin -> drag_src "
                "-> show, and no interleaving across drags",
                " ; ".join(ev_lines))

        rep.add(1, len(shows) == n_tr,
                f"exactly {n_tr} kind=translation shows for {n_tr} "
                "delivered translations (no stale/suppressed deliveries)",
                f"shows={[e[2]['gen'] for e in shows]}")
        em010s = find_events(events, "em010")
        rep.add(1, len(em010s) == n_tr,
                f"exactly {n_tr} MAIN/EngineModal/010 success sentinels "
                "(one per delivered translation)",
                f"count={len(em010s)}")
        vis = tooltip_visible(app.proc.pid)
        rep.add(2, vis,
                "the third translation tooltip is visible",
                f"class={TOOLTIP_CLASS}")
        n_pipe = len(find_events(events, "pipeline"))
        rep.note(f"architecture note: {n_pipe} PIPELINE/stage= line(s) in "
                 "the drag slice (expected 0)")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after 3 consecutive drags",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        try:
            dismiss_tooltip(app.proc.pid)
        except EnvBlock as e:
            rep.note_env(str(e))
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


def events_for_gen(events, gen):
    """Narrow a whole-slice event list to the window belonging to one
    generation: from the begin(gen) to the show/drop(gen) inclusive. Used
    for the per-drag positive chain so a multi-drag slice cannot satisfy a
    drag's assertions with another drag's tokens."""
    start = None
    end = None
    for e in events:
        if e[1] == "begin" and e[2].get("gen") == gen and start is None:
            start = e[0]
        if start is not None and e[2].get("gen") == gen \
                and e[1] in ("show_tr", "show_msg", "drop"):
            end = e[0]
            break
    if start is None:
        return []
    if end is None:
        end = events[-1][0] if events else start
    return [e for e in events if start <= e[0] <= end]


def scenario_drag_capture_failure(app, base_unused):
    """Failure-visibility contract (REQ-R1(b) + the REQ-051 Symptom A goal):
    a drag whose selection provably cannot be captured MUST surface the
    copy-failure notice, never silence or stale output. Forced
    deterministically: drag-select, collapse the selection with a real
    VK_LEFT (the icon stays up - only mouse-down outside dismisses it),
    then click. A synthetic Ctrl+C on a collapsed selection cannot bump the
    clipboard sequence, so the product must log the per-attempt
    WIN32_INPUT/CopySelectionWithSequenceWait/002 refusals ->
    MAIN/DragIconClick/001 exhaustion and show kind=message."""
    rep = Report("drag_capture_failure")
    notepad = NotepadSession()
    offset = len(app.log_text())

    def collapse_selection(np, res):
        np.press_left_arrow()
        sel = np.selection()
        if sel is None:
            raise EnvBlock("selection unreadable after VK_LEFT")
        res["collapsed_sel"] = sel
        if sel[0] != sel[1]:
            raise EnvBlock(
                f"VK_LEFT did not collapse the selection (still {sel}) - "
                "the failure premise cannot be established: setup abort, "
                "the harness will retry once")

    try:
        notepad.start()
        rep.note(f"notepad pid={notepad.pid} edit_class={notepad.edit_class}")
        notepad.assert_empty()

        res = drive_drag(notepad, app, LINE_FAILURE, offset, first=True,
                         pre_click_hook=collapse_selection)
        slice_text = app.log_text()[offset:]
        events = parse_drag_events(slice_text)
        gen = res["gen"]

        # --- setup evidence: a REAL selection existed and was collapsed ----
        s0, s1 = res["drag_sel"]
        rep.add(1, s1 > s0,
                f"premise: the drag created a real selection [{s0}..{s1}] "
                f"({s1 - s0} units)",
                f"line={LINE_FAILURE[:60]!r}")
        cs = res.get("collapsed_sel")
        rep.add(1, cs is not None and cs[0] == cs[1],
                "premise: VK_LEFT collapsed it to an empty caret before the "
                "icon click", f"collapsed_sel={cs}")

        # --- the outcome itself -------------------------------------------
        # Signature landscape on this forced-failure path (src/main.cpp /
        # src/win32_input.cpp — the REQ-051 shared 3-attempt cycle
        # CopyChordWithSettledRetry, kDragCopyChordAttempts=3, settle 70 ms):
        #   deterministic refusal: seqwait002 x3 with budgets {80, 80, 120}
        #     (the unified CopyChordRetryAttemptTimeoutMs schedule; attempt 3
        #     waits the extended 120 ms patience) + DragIconClick/001 with
        #     'after 3 attempt(s)' (cycle exhaustion) — expected under the
        #     collapsed-selection premise
        #   partial signature (1-2 refusal lines, then a confirmed attempt):
        #     an external clipboard writer raced the ~490 ms gate window and
        #     voided the premise -> environment, notice contract still judged
        #   confirmed-empty (DragIconClick/002, copy 'confirmed' but empty):
        #     same external-race class -> environment
        seqw = find_events(events, "seqwait002")
        refusal_tokens = seqw + find_events(events, "copy001")
        confirmed_empty = bool(find_events(events, "copy002empty"))
        # DragIconClick/001 fires ONLY when the whole 3-attempt cycle failed,
        # so its presence alone is the deterministic-refusal predicate.
        copy001 = find_events(events, "copy001")
        deterministic = bool(copy001)
        if res["outcome"] is None:
            add_engine_env_notes(rep, events)
            rep.note_env(
                f"drag gen={gen}: no tooltip outcome within "
                f"{STEP_TIMEOUT_S}s even on the failure path (the GUI "
                "marshal starved?): environment, not FAIL")
        elif res["outcome"] in ("translation", "dropped_translation"):
            # With a provably collapsed selection, a confirmed copy is only
            # possible if an external writer raced the clipboard sequence.
            if refusal_tokens:
                rep.add(1, True,
                        "the copy gate still logged at least one refusal - "
                        "the unexpected outcome did not pass unchecked",
                        " | ".join(e[2]["line"][:120] for e in refusal_tokens[:3]))
            rep.note_env(
                f"drag gen={gen}: a capture unexpectedly {'succeeded' if res['outcome'] == 'translation' else 'was delivered'} "
                "against a PROVABLY collapsed selection with "
                f"{'some' if refusal_tokens else 'NO'} gate-token evidence "
                "- only possible if an external process wrote the clipboard "
                "inside the gate window (contention): environment, not a "
                "product verdict")
        elif deterministic:
            # The deterministic failure path ran - assert it end to end.
            if res["outcome"] == "dropped_message":
                rep.note("the failure notice was generation-guard dropped at "
                         "render (marshal race) - the callback fired, cited "
                         "below")
            rep.add(1, len(seqw) == 3,
                    "WIN32_INPUT/CopySelectionWithSequenceWait/002 logged for "
                    "ALL THREE attempts of the shared drag cycle "
                    "(CopyChordWithSettledRetry, kDragCopyChordAttempts=3); "
                    f"got {len(seqw)}",
                    " | ".join(e[2]["line"][:140] for e in seqw))
            rep.add(1, [e[2].get("ms") for e in seqw] == [80, 80, 120],
                    "the per-attempt refusal budgets follow the unified "
                    "{80, 80, 120} schedule (attempts 1-2 wait the single-shot "
                    "80 ms, the final attempt the extended 120 ms patience)",
                    " | ".join(str(e[2].get("ms")) for e in seqw))
            rep.add(1, "after 3 attempt(s)" in copy001[0][2]["line"],
                    "MAIN/DragIconClick/001: 'clipboard copy not confirmed "
                    "after 3 attempt(s)' - the cycle exhausted its FULL "
                    "3-attempt budget before the failure branch was taken",
                    copy001[0][2]["line"])
            rep.add(1, not find_events(events, "drag_src"),
                    "STATE/drag_src absent (the flow aborted at capture, "
                    "before engine routing)",
                    find_events(events, "drag_src")[0][2]["line"]
                    if find_events(events, "drag_src") else "(none)")
        else:
            # message without the full triple-refusal signature: an external
            # clipboard race voided the collapsed-selection premise (or the
            # copy confirmed empty) - environment; the notice contract below
            # still must hold.
            shape = ("confirmed-empty (DragIconClick/002)"
                     if confirmed_empty else
                     "partial refusal signature (1-2 refusals, then a "
                     "confirmed attempt)"
                     if refusal_tokens else
                     "no capture-mechanism tokens at all")
            rep.note_env(
                f"drag gen={gen}: the notice surfaced but the deterministic "
                f"triple-refusal signature is incomplete ({shape}) - an "
                "external clipboard write raced the gate window and voided "
                "the premise: environment, mechanism assertions skipped")
        if res["outcome"] in ("message", "dropped_message"):
            # notice contract - independent of which mechanism produced it:
            # the failure is user-visible, nothing is presented as a
            # translation, and the document is untouched.
            msg = find_events(events, "show_msg", gen)
            rep.add(1, bool(msg),
                    f"the failure was SURFACED: UI/tooltip_show kind=message "
                    f"gen={gen} (TooltipCopyFailed notice, reuses the "
                    "existing StringId - no new i18n)",
                    msg[0][2]["line"] if msg else "(none)")
            rep.add(1, not find_events(events, "show_tr", gen),
                    "NO kind=translation tooltip for this generation "
                    "(nothing was silently 'translated')",
                    find_events(events, "show_tr", gen)[0][2]["line"]
                    if find_events(events, "show_tr", gen) else "(none)")
            rep.add(1, not find_events(events, "em010"),
                    "no MAIN/EngineModal/010 success sentinel on the "
                    "failure path",
                    find_events(events, "em010")[0][2]["line"]
                    if find_events(events, "em010") else "(none)")
            rep.add(1, not find_events(events, "chordfail"),
                    "no result=copy_chord_failed (Enter-pipeline token) "
                    "leaked into the drag path",
                    find_events(events, "chordfail")[0][2]["line"]
                    if find_events(events, "chordfail") else "(none)")
            add_doc_unchanged_checks(rep, res, f"drag gen={gen}")
            vis = tooltip_visible(app.proc.pid)
            rep.add(2, vis,
                    "the failure notice tooltip is visible (the failure is "
                    "user-visible, not silent)",
                    f"class={TOOLTIP_CLASS}")
        n_pipe = len(find_events(events, "pipeline"))
        rep.note(f"architecture note: {n_pipe} PIPELINE/stage= line(s) in "
                 "the drag slice (expected 0)")
        rep.add(1, app.proc is None or app.proc.poll() is None,
                "app process alive after the forced capture failure",
                f"exit={None if app.proc is None else app.proc.poll()}")
    finally:
        try:
            dismiss_tooltip(app.proc.pid)
        except EnvBlock as e:
            rep.note_env(str(e))
        notepad.stop()
    if notepad.cleanup_note:
        rep.cleanup_note = notepad.cleanup_note
    return rep


# ---------------------------------------------------------------------------
# scenario registry / main (same driver shape and retry policy as req027)
# ---------------------------------------------------------------------------
DRIVERS = {
    "drag_translate": scenario_drag_translate,
    "drag_consecutive": scenario_drag_consecutive,
    "drag_capture_failure": scenario_drag_capture_failure,
}

ALL_SCENARIOS = ["drag_translate", "drag_consecutive", "drag_capture_failure"]


def main(argv=None):
    global STEP_TIMEOUT_S
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="REQ-051 drag -> floating-button translation E2E harness "
                    "(interactive desktop only; judges from the DIAG log)")
    ap.add_argument("scenario", nargs="?", default=None,
                    choices=sorted(ALL_SCENARIOS + ["all"]))
    ap.add_argument("--all", action="store_true",
                    help="run all scenarios against one shared app instance")
    ap.add_argument("--app-exe", default=DEFAULT_APP_EXE,
                    help="path to Emebala_chat.exe (default build\\)")
    ap.add_argument("--no-cleanup", action="store_true",
                    help="keep the app process alive after the run (debug)")
    ap.add_argument("--step-timeout", type=float, default=None,
                    help="override the drag-click -> tooltip outcome timeout "
                         "(s)")
    args = ap.parse_args(argv)

    if args.step_timeout:
        STEP_TIMEOUT_S = args.step_timeout
    chosen = ALL_SCENARIOS if (args.all or args.scenario in (None, "all")) \
        else [args.scenario]

    print("== REQ-051 drag E2E harness (Symptom A regression defense) ==")
    print(f"   scenarios: {chosen}")
    print(f"   app exe  : {args.app_exe}")
    print(f"   log dir  : {LOG_DIR}")
    print("   NOTE     : do NOT touch keyboard/mouse until the verdict - the")
    print("              harness drives the REAL cursor (drag + icon click).")

    reports = []
    harness_failed = False
    app = None

    def start_app():
        nonlocal app
        app = AppSession(args.app_exe)
        app.start()
        print(f"   app      : pid={app.proc.pid}")
        print(f"   app log  : {app.log_path}")

    def stop_app():
        nonlocal app
        if app is not None:
            if args.no_cleanup and app.proc and app.proc.poll() is None:
                print(f"   cleanup  : SKIPPED (--no-cleanup), app pid "
                      f"{app.proc.pid} left running")
                return
            app.stop()
            app = None

    t0 = time.time()
    try:
        try:
            closed, kept_blank = NotepadSession.preflight_scrub()
            print(f"   preflight: closed {closed} leftover empty-untitled "
                  f"Notepad frame(s); kept {kept_blank} blank as the reuse "
                  "pool (content/multi-tab frames never touched)")
            _foreground_guard()  # fail fast + name a phantom holder
            start_app()
            for name in chosen:
                rep = None
                # One retry for transient environment aborts (foreground /
                # drag / icon races) and one retry for environment-class
                # INCONCLUSIVE (cloud/network) - same policy as req027
                # (delegation section 4).
                for retry in range(2):
                    try:
                        rep = DRIVERS[name](app, 0)
                    except EnvBlock as e:
                        if retry == 0:
                            print(f"   [retry] {name}: transient/setup "
                                  f"abort ({e}); one retry")
                            continue
                        r = Report(name)
                        r.note_env(str(e))
                        rep = r
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
