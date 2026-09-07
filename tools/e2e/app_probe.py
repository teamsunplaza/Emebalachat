#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
app_probe.py - universal app EM-capability matrix probe (design 173700 §2.3,
batch D-4a).
================================================================================

Purpose
-------
The EM capability probe inside the app (src/win32_input.cpp
edit_caret::ProbeEmCapability + the PURE decision core ClassifyEmProbe in
src/win32_input.hpp) is known to answer Capable for RichEdit-family controls
only; for HWP / PowerPoint / Word / browsers / Electron chat apps the verdict
is "확인 필요" until MEASURED (debug 173700 root-cause report). This tool is
that measurement: it lists the edit-control class names of a target app's
window, replays the exact same read-only EM_* signal family through
SendMessageTimeoutW (100 ms, SMTO_ABORTIFHUNG - the app's SendEm budget),
ports ClassifyEmProbe to Python verbatim, and renders the app x control x
EM-capability x expected-pipeline-path matrix (markdown or JSON).

The 8 user-named apps (design §2.3 matrix rows):
    notepad   메모장      Notepad.exe
    kakao     카카오톡    KakaoTalk.exe
    discord   디스코드    Discord.exe
    chrome    Chrome      chrome.exe
    firefox   Firefox     firefox.exe
    hwp       한글(HWP)   hwp.exe
    ppt       PowerPoint  powerpnt.exe
    word      MS Word     winword.exe

Expected pipeline path per verdict (B-6b contract, src/win32_input.cpp
CopySelectedText gate):
    Capable                -> EM 경로: EditCaretTracker EM_SETSEL(last, caret)
    NotCapable / Unknown   -> 폴백: SelectMessageBlock keyboard geometry
    app not running        -> 확인 필요 (never guessed - design §2.3)

Safety contract
---------------
* DEFAULT MODE IS READ-ONLY: EM_GETSEL, EM_GETLIMITTEXT, WM_GETTEXTLENGTH,
  EM_GETLINECOUNT, EM_LINEFROMCHAR, EM_GETLINE are all non-mutating; the only
  EM_SETSEL traffic re-sets the CURRENT selection to itself (a verified no-op)
  so the "does SETSEL answer" signal the delegation asks for is measured
  without touching any user document. Focus resolution uses
  GetGUIThreadInfo - no window activation, no keystrokes.
* --fallback adds the design §2.3 item-5 geometry measurement (Shift+Home ->
  selection span; Ctrl+Shift+Home -> block span; Ctrl+C -> clipboard capture
  length with GetClipboardSequenceNumber freshness). This SELECTS text and
  OVERWRITES the clipboard (content is never modified) and needs the target
  frame to win the foreground - it is opt-in and interactive-QA scoped.
* --top launches an app when it is not already running (notepad is always
  available; Office/HWP/Electron apps depend on the install location).
* Live interactive measurement of the 8-app matrix is USER QA per the D-4
  delegation ("대화형 실측 실행은 유저 QA 이관"); this file only has to be
  correct, runnable and --help-safe in the harness sandbox.

Dependencies
------------
Python 3.8+ standard library ONLY (ctypes P/Invoke), mirroring
req027_e2e.py. The Win32 plumbing (SendMessageTimeoutW wrapper, class
reading, window/process enumeration, SendInput structures) is IMPORTED from
the sibling module req027_e2e.py - one source of truth for the ctypes layer,
no pywinauto anywhere.

Usage
-----
  python tools\\e2e\\app_probe.py --help
  python tools\\e2e\\app_probe.py                      ; markdown matrix, all 8
  python tools\\e2e\\app_probe.py --app word           ; single app row set
  python tools\\e2e\\app_probe.py --json               ; machine-readable
  python tools\\e2e\\app_probe.py --app all --fallback ; + keyboard geometry
  python tools\\e2e\\app_probe.py --probe-hwnd 0x1234  ; arbitrary control
  python tools\\e2e\\app_probe.py --app notepad --top  ; allow launching

Exit codes: 0 = probe completed (rows may still be "확인 필요"), 3 = harness
error. Never 1/2: this tool measures, it does not judge the product.
"""

import argparse
import ctypes
import json
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import req027_e2e as e2e  # noqa: E402  (sibling ctypes layer, imported after sys.path fix)

# ---------------------------------------------------------------------------
# message constants (values verified against the app's own usage: src/
# win32_input.cpp ProbeEmCapability sends exactly this family; EM_GETSEL/
# EM_SETSEL/WM_GETTEXTLENGTH/WM_GETTEXT already pinned in req027_e2e.py)
# ---------------------------------------------------------------------------
EM_GETSEL = e2e.EM_GETSEL              # 0x00B0
EM_SETSEL = e2e.EM_SETSEL              # 0x00B1
EM_GETLIMITTEXT = 0x00D5               # WM_USER + 0x35
EM_GETLINECOUNT = 0x00BA               # WM_USER + 0x1A
EM_LINEFROMCHAR = 0x00C9               # WM_USER + 0x29
EM_GETLINE = 0x00C4                    # WM_USER + 0x24
WM_GETTEXTLENGTH = e2e.WM_GETTEXTLENGTH
VK_HOME = 0x24
VK_SHIFT = e2e.VK_SHIFT
VK_CONTROL = e2e.VK_CONTROL
CF_UNICODETEXT = e2e.CF_UNICODETEXT
PROBE_TIMEOUT_MS = 100                 # the app's SendEm deadlock budget (design §1.3)
WM_TIMEOUT_MS = 2000                   # longer cap only for the informational GETLINE/SETSEL extras

user32 = e2e.user32
user32.GetClipboardSequenceNumber.argtypes = []
user32.GetClipboardSequenceNumber.restype = ctypes.c_uint
user32.IsClipboardFormatAvailable.argtypes = [ctypes.c_uint]
user32.IsClipboardFormatAvailable.restype = e2e.wintypes.BOOL
user32.GetClipboardData.argtypes = [ctypes.c_uint]
user32.GetClipboardData.restype = ctypes.c_void_p

# ---------------------------------------------------------------------------
# target registry (design §2.3 rows). `probe`/`launch` image names were NOT
# invented: tasklist image names for these vendors' desktop apps. Class hints
# are DISCOVERY ACCELERATORS ONLY - like the app's B-6b probe, the verdict
# never consults a class name.
# ---------------------------------------------------------------------------
APP_TARGETS = {
    "notepad": {
        "display": "메모장 (Notepad)",
        "probe": "Notepad",
        "frame_classes": {"notepad"},
        "class_hints": ("richedit", "edit"),
        "launch": ["notepad.exe"],
    },
    "kakao": {
        "display": "카카오톡 (KakaoTalk)",
        "probe": "KakaoTalk",
        "frame_classes": set(),
        "class_hints": ("richedit", "edit"),
        # KakaoTalk does not register a PATH exe by default; only relaunch
        # when the image resolves via shutil.which (usually False ->
        # "확인 필요: not running", which the design explicitly allows).
        "launch": None,
    },
    "discord": {
        "display": "디스코드 (Discord)",
        "probe": "Discord",
        "frame_classes": set(),
        "class_hints": ("chrome_widgetwin",),
        "launch": None,  # per-user %LOCALAPPDATA%\\Discord\\Discord.exe - see try_launch
    },
    "chrome": {
        "display": "Chrome",
        "probe": "chrome",
        "frame_classes": set(),
        "class_hints": ("chrome_widgetwin",),
        "launch": None,  # located via Program Files in try_launch
    },
    "firefox": {
        "display": "Firefox",
        "probe": "firefox",
        "frame_classes": set(),
        "class_hints": ("mozilla",),
        "launch": None,
    },
    "hwp": {
        "display": "한글 (HWP)",
        "probe": "hwp",
        "frame_classes": set(),
        "class_hints": ("edit", "hwp", "writer"),
        "launch": None,  # AhnLab install path is machine-specific - never guessed
    },
    "ppt": {
        "display": "PowerPoint",
        "probe": "powerpnt",
        "frame_classes": set(),
        "class_hints": ("screen", "edit"),
        "launch": None,
    },
    "word": {
        "display": "MS Word",
        "probe": "winword",
        "frame_classes": set(),
        "class_hints": ("_wwg", "richedit", "document"),
        "launch": None,  # Office root path probed in try_launch, never guessed
    },
}
APP_ORDER = ["notepad", "kakao", "discord", "chrome", "firefox", "hwp", "ppt",
             "word"]


class ProbeError(Exception):
    """Unexpected harness-level failure while probing (reported per row, not
    fatal to the matrix)."""


# ---------------------------------------------------------------------------
# EM_* signal collection - the ctypes mirror of ProbeEmCapability (src/
# win32_input.cpp ~L796). Read-only EXCEPT the documented no-op SETSEL re-set
# measured separately (does_not_change_range is asserted).
# ---------------------------------------------------------------------------
def collect_signals(hwnd):
    """Return (EmProbeSignals-equivalent dict, extra measurements dict)."""
    sig = {
        "getsel_handled": False,
        "sel_start": 0,
        "sel_end": 0,
        "limit_ok": False,
        "limittext": 0,
        "len_ok": False,
        "textlen": 0,
        "count_ok": False,
        "linecount": 0,
        "linefromchar_ok": False,
    }
    extras = {}
    ok, res = e2e.send_em(hwnd, EM_GETSEL, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
    sig["getsel_handled"] = ok
    if ok:
        sig["sel_start"] = res & 0xFFFF          # LOWORD (WORD-packed, saturates at 65535)
        sig["sel_end"] = (res >> 16) & 0xFFFF    # HIWORD = caret when no range
    ok, val = e2e.send_em(hwnd, EM_GETLIMITTEXT, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
    sig["limit_ok"], sig["limittext"] = ok, val
    ok, val = e2e.send_em(hwnd, WM_GETTEXTLENGTH, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
    sig["len_ok"], sig["textlen"] = ok, val
    ok, val = e2e.send_em(hwnd, EM_GETLINECOUNT, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
    sig["count_ok"], sig["linecount"] = ok, val
    if sig["len_ok"] and sig["textlen"] > 0:
        # wParam -1 is the caret sentinel. ctypes WPARAM is a SIGNED
        # LONG_PTR, so -1 sign-extends to 0xFFFFFFFFFFFFFFFF - byte-identical
        # to the C++ probe's static_cast<WPARAM>(-1) (src/win32_input.cpp
        # L814-815). A 0xFFFFFFFF literal would send only the low 32 bits.
        ok, val = e2e.send_em(hwnd, EM_LINEFROMCHAR, -1, 0,
                              timeout_ms=PROBE_TIMEOUT_MS)
        sig["linefromchar_ok"] = ok
        extras["caret_line"] = val
    # informational: does EM_GETLINE answer (line 0 copy, max 64 chars)?
    buf = ctypes.create_unicode_buffer(66)
    try:
        struct.pack_into("=H", buf, 0, 64)  # first WORD = buffer capacity, GETLINE contract
        ok, n = e2e.send_em(hwnd, EM_GETLINE, 0, ctypes.addressof(buf),
                            timeout_ms=WM_TIMEOUT_MS)
        preview = ""
        if ok and n:
            raw = bytes(buf)[2:2 + int(n) * 2]
            preview = raw.decode("utf-16-le", "replace")[:24]
        extras["getline"] = {"ok": ok, "chars": int(n) if ok else None,
                             "preview": preview}
    except Exception as exc:  # never let one message kill the row
        extras["getline"] = {"ok": False, "error": str(exc)}
    # informational: EM_SETSEL re-set of the CURRENT range (verified no-op).
    if sig["getsel_handled"]:
        try:
            ok, _ = e2e.send_em(hwnd, EM_SETSEL, sig["sel_start"], sig["sel_end"],
                                timeout_ms=WM_TIMEOUT_MS)
            ok2, res2 = e2e.send_em(hwnd, EM_GETSEL, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
            same = (ok2 and (res2 & 0xFFFF) == sig["sel_start"]
                    and ((res2 >> 16) & 0xFFFF) == sig["sel_end"])
            extras["setsel_noop"] = {"ok": ok, "range_unchanged": same}
        except Exception as exc:
            extras["setsel_noop"] = {"ok": False, "error": str(exc)}
    return sig, extras


def classify_em_probe(s):
    """Python port of ClassifyEmProbe (src/win32_input.hpp L224-246, VP-
    approved DefWindowProc false-positive hardening). Kept LINE-FOR-LINE
    comparable to the C++ constexpr so the matrix and the shipped probe can
    never diverge silently. Returns 'Capable' | 'NotCapable' | 'Unknown'."""
    if not s["getsel_handled"]:
        return "NotCapable"  # EM_GETSEL timed out: EM path unusable
    if s["sel_start"] != 0 or s["sel_end"] != 0:
        return "Capable"     # meaningful selection/caret: EM_GETSEL handled
    em_line_model = s["count_ok"] and s["linecount"] >= 1
    if s["len_ok"] and s["textlen"] > 0:
        # caret at document start in a NON-empty document
        return "Capable" if (em_line_model and s["linefromchar_ok"]) else "Unknown"
    if s["len_ok"]:
        # empty document: (0,0) is the only valid caret; a generic window
        # leaves EM_GETLINECOUNT to DefWindowProc (reply 0) -> NotCapable.
        return "Capable" if em_line_model else "NotCapable"
    return "Unknown" if (em_line_model or s["limit_ok"]) else "NotCapable"


def expected_path(verdict):
    return ("EM 경로 (EditCaretTracker EM_SETSEL(last,caret))"
            if verdict == "Capable" else
            "폴백 (SelectMessageBlock 키보드 지오메트리)")


# ---------------------------------------------------------------------------
# window / control discovery
# ---------------------------------------------------------------------------
def child_edit_inventory(top_hwnd):
    """{class_lower: {'count': n, 'hwnd': first, 'textlen_ok': bool}} for
    child windows whose class looks editable, plus the full distinct class
    list (design D-4a item 1: '편집 컨트롤 클래스명 나열')."""
    children = e2e.enum_child_windows(top_hwnd)
    inventory = {}
    all_classes = {}
    for h in children:
        cls = e2e.get_class(h)
        if not cls:
            continue
        all_classes[cls] = all_classes.get(cls, 0) + 1
        low = cls.lower()
        if "edit" in low or "document" in low or "console" in low:
            entry = inventory.setdefault(low, {"class": cls, "count": 0,
                                               "hwnd": h, "textlen_ok": False})
            entry["count"] += 1
            ok, _ = e2e.send_em(h, WM_GETTEXTLENGTH, 0, 0, timeout_ms=500)
            entry["textlen_ok"] = entry["textlen_ok"] or ok
    return inventory, sorted(all_classes.items(), key=lambda kv: -kv[1])


def pick_candidates(top_hwnd, spec, inventory):
    """Controls worth probing: keyboard-focused first, then up to 3 class-
    distinct children matching the hint order (design: focused control is
    exactly what the app's EM path targets via ResolveFocusCandidate)."""
    cands = []
    focus = e2e.get_focus_hwnd_for_window(top_hwnd)
    if focus:
        cands.append(focus)
    seen = {int(h) for h in cands}
    for hint in spec["class_hints"]:
        for low, info in inventory.items():
            if hint in low and info["hwnd"] not in seen:
                cands.append(info["hwnd"])
                seen.add(int(info["hwnd"]))
                break
        if len(cands) >= 4:
            break
    return cands[:4]


# ---------------------------------------------------------------------------
# clipboard + fallback geometry (design §2.3 item 5) - OPT-IN via --fallback.
# Selection is mutated, content is not; the clipboard is overwritten (same
# side effect the app's own Ctrl+C capture step has).
# ---------------------------------------------------------------------------
def clipboard_state():
    """(seq, utf16_len, preview). CF_UNICODETEXT is a MOVABLE memory BLOCK:
    its handle must be GlobalLock'd before reading (casting the handle to a
    pointer directly is undefined). Never GlobalFree - the clipboard owns
    the block. seq None when GetClipboardSequenceNumber unavailable."""
    seq = user32.GetClipboardSequenceNumber()
    if not user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
        return seq, None, None
    if not user32.OpenClipboard(e2e.HWND(0)):
        return seq, None, None
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return seq, None, None
        ptr = e2e.kernel32.GlobalLock(h)
        if not ptr:
            return seq, None, None
        try:
            s = ctypes.cast(ptr, ctypes.c_wchar_p).value or ""
        finally:
            e2e.kernel32.GlobalUnlock(h)
        return seq, e2e.utf16_len(s), s[:24]
    finally:
        user32.CloseClipboard()


def _send_inputs(seq):
    arr = (e2e.INPUT * len(seq))(*seq)
    n = user32.SendInput(len(seq), ctypes.byref(arr), ctypes.sizeof(e2e.INPUT))
    if n != len(seq):
        raise ProbeError(f"SendInput failed (sent {n} of {len(seq)})")
    time.sleep(0.05)


def _down(vk):
    return e2e.INPUT(type=e2e.INPUT_KEYBOARD, ki=e2e.KEYBDINPUT(vk, 0, 0, 0, None))


def _up(vk):
    return e2e.INPUT(type=e2e.INPUT_KEYBOARD,
                     ki=e2e.KEYBDINPUT(vk, 0, e2e.KEYEVENTF_KEYUP, 0, None))


def send_combo(mod_vk, key_vk):
    _send_inputs([_down(mod_vk), _down(key_vk), _up(key_vk), _up(mod_vk)])


def send_chord2(mod1_vk, mod2_vk, key_vk):
    """Two-modifier chord (Ctrl+Shift+Home): proper nesting of key-ups."""
    _send_inputs([_down(mod1_vk), _down(mod2_vk), _down(key_vk),
                  _up(key_vk), _up(mod2_vk), _up(mod1_vk)])


def tap_alt():
    """Foreground-lock unlock (same recipe as NotepadSession._tap_alt):
    VK_MENU alone never gates the app's Enter path, so this is harmless to
    any app still watching its hook."""
    _send_inputs([_down(e2e.VK_MENU), _up(e2e.VK_MENU)])


def try_foreground(top_hwnd, budget=10):
    """Bounded version of NotepadSession._focus's escalation ladder."""
    cur_tid = e2e.kernel32.GetCurrentThreadId()
    for attempt in range(budget):
        if int(user32.GetForegroundWindow() or 0) == int(top_hwnd):
            return True
        if attempt < 2:
            user32.SetForegroundWindow(e2e.HWND(top_hwnd))
        elif attempt < 5:
            tap_alt()
            user32.SetForegroundWindow(e2e.HWND(top_hwnd))
        elif attempt < 8:
            fg = user32.GetForegroundWindow()
            fg_tid = user32.GetWindowThreadProcessId(fg, None)
            user32.AttachThreadInput(cur_tid, fg_tid, True)
            user32.SetForegroundWindow(e2e.HWND(top_hwnd))
            user32.BringWindowToTop(e2e.HWND(top_hwnd))
            user32.AttachThreadInput(cur_tid, fg_tid, False)
        else:
            user32.SwitchToThisWindow(e2e.HWND(top_hwnd), e2e.wintypes.BOOL(1))
        time.sleep(0.2)
    return int(user32.GetForegroundWindow() or 0) == int(top_hwnd)


def _read_sel(ctrl_hwnd):
    ok, res = e2e.send_em(ctrl_hwnd, EM_GETSEL, 0, 0, timeout_ms=PROBE_TIMEOUT_MS)
    if not ok:
        return None
    return res & 0xFFFF, (res >> 16) & 0xFFFF


def measure_fallback_geometry(frame_hwnd, ctrl_hwnd):
    """Design §2.3 item 5: Shift+Home (line-start) and Ctrl+Shift+Home
    (block-ish) selection spans + Ctrl+C clipboard capture length. The
    original selection is RESTORED between chords and finally collapsed, so
    the user's window ends visually as found (selection only - the caret was
    already at the selection end; content is never touched). Ctrl+C does
    overwrite the clipboard (same side effect the app's own capture has).
    NEVER raises to the caller - errors land in the dict so one uncooperative
    app cannot kill the matrix."""
    out = {"attempted": True}
    try:
        if not try_foreground(frame_hwnd):
            return {"attempted": False,
                    "note": "target never won the foreground; geometry "
                            "measurement skipped (run interactively or "
                            "click the app first)"}
        original = _read_sel(ctrl_hwnd)
        if original is None:
            out["note"] = "baseline EM_GETSEL unreadable; spans unmeasurable"
            return out
        # --- Shift+Home: line-start selection span ---
        send_combo(VK_SHIFT, VK_HOME)
        sh = _read_sel(ctrl_hwnd)
        out["shift_home"] = ({"ok": True, "start": sh[0], "end": sh[1],
                              "span": abs(sh[1] - sh[0])} if sh else {"ok": False})
        e2e.send_em(ctrl_hwnd, EM_SETSEL, original[0], original[1],
                    timeout_ms=WM_TIMEOUT_MS)  # restore
        # --- Ctrl+Shift+Home: block-ish start selection span, then Ctrl+C
        # WHILE THE RANGE IS LIVE (exactly the SelectMessageBlock fallback
        # capture the app performs: grow the selection -> copy) ---
        send_chord2(VK_CONTROL, VK_SHIFT, VK_HOME)
        csh = _read_sel(ctrl_hwnd)
        out["ctrl_shift_home"] = ({"ok": True, "start": csh[0], "end": csh[1],
                                   "span": abs(csh[1] - csh[0])}
                                  if csh else {"ok": False})
        seq_before, _l, _p = clipboard_state()
        send_combo(VK_CONTROL, 0x43)  # VK_C
        deadline = time.time() + 0.6
        seq_after, clen, preview = seq_before, None, None
        while time.time() < deadline:
            seq_after, clen, preview = clipboard_state()
            if seq_after is not None and seq_after != seq_before:
                break
            time.sleep(0.03)
        out["ctrl_c"] = {"seq_before": seq_before, "seq_after": seq_after,
                         "changed": seq_before != seq_after,
                         "clip_utf16_len": clen, "clip_preview": preview}
        # collapse to the caret end: the user's window is left visually clean
        # (a parked caret is the natural resting state after Ctrl+C anyway)
        e2e.send_em(ctrl_hwnd, EM_SETSEL, original[1], original[1],
                    timeout_ms=WM_TIMEOUT_MS)
        return out
    except Exception as exc:
        return {"attempted": True, "error": f"{type(exc).__name__}: {exc}"}


# ---------------------------------------------------------------------------
# app launch (only with --top; paths are probed, never hallucinated)
# ---------------------------------------------------------------------------
def _env_dir(name):
    """Program Files dirs need os.environ (NOT expandvars): the
    'ProgramFiles(x86)' variable name contains parentheses, which
    expandvars' %VAR% scanner does not match - the literal string would
    silently never exist."""
    return os.environ.get(name, "")


def _pf_candidates(subpath):
    dirs = [_env_dir("ProgramFiles"), _env_dir("ProgramFiles(x86)"),
            _env_dir("ProgramW6432")]
    return [os.path.join(d, subpath) for d in dirs if d]


def _localappdata(subpath):
    la = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(la, subpath) if la else None


def _launch_command(key):
    if key == "notepad":
        return ["notepad.exe"]
    if key == "chrome":
        for p in _pf_candidates(r"Google\Chrome\Application\chrome.exe"):
            if os.path.exists(p):
                return [p]
        return None
    if key == "firefox":
        for p in _pf_candidates(r"Mozilla Firefox\firefox.exe"):
            if os.path.exists(p):
                return [p]
        return None
    if key == "discord":
        p = _localappdata(r"Discord\Discord.exe")
        return [p] if p and os.path.exists(p) else None
    if key == "kakao":
        p = _localappdata(r"kakao\KakaoTalk\KakaoTalk.exe")
        return [p] if p and os.path.exists(p) else None
    if key == "word":
        for p in _pf_candidates(
                r"Microsoft Office\root\Office16\WINWORD.EXE"):
            if os.path.exists(p):
                return [p]
        return None
    if key == "ppt":
        for p in _pf_candidates(
                r"Microsoft Office\root\Office16\POWERPNT.EXE"):
            if os.path.exists(p):
                return [p]
        return None
    return None  # hwp: machine-specific AhnLab path - "확인 필요" instead of a guess


def try_launch(key):
    cmd = _launch_command(key)
    if not cmd:
        return False, "no safe launch path known (install location varies)"
    try:
        subprocess.Popen(cmd)
    except OSError as exc:
        return False, f"launch failed: {exc}"
    time.sleep(2.5)
    return True, "launched " + " ".join(cmd)


# ---------------------------------------------------------------------------
# one app row
# ---------------------------------------------------------------------------
def probe_app(key, do_fallback, allow_top):
    spec = APP_TARGETS[key]
    r = {"app": key, "display": spec["display"], "status": "not-running",
         "pids": [], "windows": [], "controls": [],
         "verdict": "확인 필요", "expected_path": "확인 필요",
         "launch_note": None, "fallback": None}
    pids = sorted(e2e.list_processes_like(spec["probe"]))
    if not pids and allow_top:
        ok, note = try_launch(key)
        r["launch_note"] = note
        if ok:
            pids = sorted(e2e.list_processes_like(spec["probe"]))
    if not pids:
        r["status"] = "not-running"
        return r
    r["pids"] = pids
    windows = []
    for pid in pids:
        for (h, title, cls) in e2e.top_windows_for_pid(pid):
            windows.append({"hwnd": h, "pid": pid, "title": title, "class": cls})
    r["windows"] = windows
    if not windows:
        r["status"] = "running-no-visible-window"
        return r

    def frame_score(w):
        s = 0
        c = (w["class"] or "").lower()
        if c in spec["frame_classes"]:
            s += 4
        if w["title"] and not c.startswith("ghost") and "tooltip" not in c:
            s += 1
        return -s

    frame = sorted(windows, key=frame_score)[0]
    r["frame"] = frame
    focus = e2e.get_focus_hwnd_for_window(frame["hwnd"])
    r["focused_hwnd"] = focus or None
    inventory, class_counts = child_edit_inventory(frame["hwnd"])
    r["child_classes"] = [{"class": c, "count": n} for c, n in class_counts]
    cands = pick_candidates(frame["hwnd"], spec, inventory)
    if not cands:
        r["status"] = ("running-no-edit-candidate" if focus
                       else "running-no-focus-control")
        return r
    r["status"] = "probed"
    for h in cands:
        try:
            sig, extras = collect_signals(h)
            verdict = classify_em_probe(sig)
            row = {"hwnd": h, "class": e2e.get_class(h),
                   "focused": h == focus,
                   "signals": sig, "extras": extras, "verdict": verdict,
                   "expected_path": expected_path(verdict)}
        except Exception as exc:
            row = {"hwnd": h, "class": e2e.get_class(h), "focused": False,
                   "error": f"{type(exc).__name__}: {exc}",
                   "verdict": "확인 필요", "expected_path": "확인 필요"}
            verdict = "확인 필요"
        if do_fallback and verdict != "확인 필요":
            row["fallback"] = measure_fallback_geometry(frame["hwnd"], h)
        r["controls"].append(row)
    # headline verdict: the FOCUSED control's, else the first candidate's
    primary = next((c for c in r["controls"] if c.get("focused")), r["controls"][0])
    r["verdict"] = primary["verdict"]
    r["expected_path"] = primary["expected_path"]
    return r


# ---------------------------------------------------------------------------
# matrix rendering
# ---------------------------------------------------------------------------
def _sig_cells(row):
    if "signals" not in row:
        return ["-"] * 5
    s, ex = row["signals"], row.get("extras", {})
    g = ex.get("getline", {})
    cells = [
        "Y" if s["getsel_handled"] else "N",
        "Y" if s["count_ok"] and s["linecount"] >= 1 else
        ("Y?" if s["count_ok"] else "N"),
        "Y" if s["len_ok"] else "N",
        "Y" if s["limit_ok"] else "N",
        ("Y" if g.get("ok") else "N"),
    ]
    return cells


def render_markdown(results):
    lines = [
        "# Universal app matrix (app_probe.py, design 173700 §2.3)",
        "",
        "Columns: GETSEL = EM_GETSEL answered; LCOUNT = EM_GETLINECOUNT>=1;",
        "TLEN = WM_GETTEXTLENGTH; LIMIT = EM_GETLIMITTEXT; GLINE = EM_GETLINE.",
        "Verdict = port of the app's pure ClassifyEmProbe (class names NEVER",
        "feed it - B-6b contract). 폴백 동작 column appears only with --fallback.",
        "",
        "| 앱 | 상태 | 프레임 클래스 | 컨트롤 클래스 | focus | GETSEL | LCOUNT | TLEN | LIMIT | GLINE | SETSEL(no-op) | 판정 | 예상 경로 |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        if not r["controls"]:
            lines.append(
                f"| {r['display']} | {r['status']} | - | - | - | - | - | - | - "
                f"| - | - | {r['verdict']} | {r['expected_path']} |")
            continue
        frame_cls = (r.get("frame") or {}).get("class", "-")
        for i, c in enumerate(r["controls"]):
            ex = c.get("extras", {}) or {}
            sn = ex.get("setsel_noop", {})
            setsel = "-"
            if sn:
                setsel = ("Y" if sn.get("ok") and sn.get("range_unchanged")
                          else "N" if sn.get("ok") else "?")
            cells = _sig_cells(c)
            lines.append(
                "| {} | {} | {} | `{}` | {} | {} | {} | {} | {} | {} | {} "
                "| **{}** | {} |".format(
                    r["display"] if i == 0 else "〃", r["status"], frame_cls,
                    c["class"], "Y" if c.get("focused") else "",
                    *cells, setsel, c["verdict"], c["expected_path"]))
    fb = [r for r in results if r.get("fallback")]
    if fb:
        lines += ["", "## Fallback geometry measurements (--fallback)", ""]
        for r in fb:
            for c in r["controls"]:
                f = c.get("fallback")
                if not f:
                    continue
                if f.get("attempted") and "error" in f:
                    lines.append(f"- {r['display']} `{c['class']}`: ERROR {f['error']}")
                elif not f.get("attempted"):
                    lines.append(f"- {r['display']} `{c['class']}`: skipped ({f.get('note', '')})")
                else:
                    sh = f.get("shift_home", {})
                    cc = f.get("ctrl_c", {})
                    lines.append(
                        f"- {r['display']} `{c['class']}`: Shift+Home span="
                        f"{sh.get('span', '-')} | Ctrl+C seq changed="
                        f"{cc.get('changed', '-')} clip_len={cc.get('clip_utf16_len', '-')} "
                        f"preview={cc.get('clip_preview')!r}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(
        prog="app_probe.py",
        description="Universal app EM-capability matrix probe (design 173700 "
                    "§2.3): class listing + read-only EM_* signal replay + "
                    "ClassifyEmProbe port + optional fallback geometry.",
        epilog="Examples:  python tools\\e2e\\app_probe.py --app all  |  "
               "python tools\\e2e\\app_probe.py --app word --fallback  |  "
               "python tools\\e2e\\app_probe.py --probe-hwnd 0x1234 --json")
    ap.add_argument("--app", default="all", choices=["all"] + APP_ORDER,
                    help="target app (default: all 8 user-named apps)")
    ap.add_argument("--json", action="store_true",
                    help="machine-readable JSON output instead of markdown")
    ap.add_argument("--fallback", action="store_true",
                    help="opt-in interactive geometry measurement "
                         "(Shift+Home span + Ctrl+C capture length; mutates "
                         "selection and clipboard, never document content)")
    ap.add_argument("--top", action="store_true",
                    help="allow launching the app when not running (only for "
                         "apps with a locatable install path)")
    ap.add_argument("--probe-hwnd", default=None, metavar="0xHEX",
                    help="probe one arbitrary control hwnd instead of app "
                         "discovery (user-QA escape hatch)")
    ap.add_argument("--out", default=None, metavar="PATH",
                    help="write the rendered matrix to PATH (utf-8) in "
                         "addition to stdout")
    args = ap.parse_args(argv)

    if args.probe_hwnd:
        try:
            hwnd = int(args.probe_hwnd, 16) if args.probe_hwnd.lower().startswith("0x") \
                else int(args.probe_hwnd)
        except ValueError:
            print(f"[app_probe] bad --probe-hwnd: {args.probe_hwnd!r}",
                  file=sys.stderr)
            return 3
        sig, extras = collect_signals(hwnd)
        verdict = classify_em_probe(sig)
        r = {"app": "custom", "display": f"hwnd 0x{hwnd:X}",
             "status": "probed", "pids": [], "windows": [],
             "controls": [{"hwnd": hwnd, "class": e2e.get_class(hwnd),
                           "focused": False, "signals": sig, "extras": extras,
                           "verdict": verdict,
                           "expected_path": expected_path(verdict)}],
             "verdict": verdict, "expected_path": expected_path(verdict),
             "fallback": None}
        if args.fallback:
            r["fallback"] = measure_fallback_geometry(hwnd, hwnd)
            r["controls"][0]["fallback"] = r["fallback"]
        results = [r]
    else:
        keys = APP_ORDER if args.app == "all" else [args.app]
        results = []
        for k in keys:
            try:
                results.append(probe_app(k, args.fallback, args.top))
            except Exception as exc:
                results.append({"app": k, "display": APP_TARGETS[k]["display"],
                                "status": f"probe-error: {exc}", "pids": [],
                                "windows": [], "controls": [],
                                "verdict": "확인 필요",
                                "expected_path": "확인 필요", "fallback": None})

    if args.json:
        rendered = json.dumps({"tool": "app_probe", "fallback": args.fallback,
                               "results": results}, ensure_ascii=False,
                              indent=2)
    else:
        rendered = render_markdown(results)
    print(rendered)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(rendered)
        print(f"[app_probe] matrix written to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
