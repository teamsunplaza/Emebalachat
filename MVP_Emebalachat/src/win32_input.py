"""Pure ctypes Windows Low-Level Input synthesis and Clipboard safe swap.

Provides Win32 API bindings for user32.dll and kernel32.dll without external
dependencies. Implements synthetic keystroke injection tagged with SYNTHETIC_MARKER,
IME flush routines, text selection/copy/paste automation, and robust clipboard
backup and restore mechanisms.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
import logging
import time
from typing import List, Optional, Tuple

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# DLL Loading
# ---------------------------------------------------------------------------
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ---------------------------------------------------------------------------
# Types and Constants
# ---------------------------------------------------------------------------
ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

# Synthetic marker placed in dwExtraInfo to prevent hook re-entry loops
SYNTHETIC_MARKER: int = 0x1337BEEF

# Input Types
INPUT_MOUSE: int = 0
INPUT_KEYBOARD: int = 1
INPUT_HARDWARE: int = 2

# Key Event Flags
KEYEVENTF_EXTENDEDKEY: int = 0x0001
KEYEVENTF_KEYUP: int = 0x0002
KEYEVENTF_UNICODE: int = 0x0004
KEYEVENTF_SCANCODE: int = 0x0008

# Windows Hook Types & Codes
WH_KEYBOARD_LL: int = 13
WM_KEYDOWN: int = 0x0100
WM_KEYUP: int = 0x0101
WM_SYSKEYDOWN: int = 0x0104
WM_SYSKEYUP: int = 0x0105
WM_QUIT: int = 0x0012

# Virtual Key Codes
VK_BACK: int = 0x08
VK_TAB: int = 0x09
VK_RETURN: int = 0x0D
VK_SHIFT: int = 0x10
VK_CONTROL: int = 0x11
VK_MENU: int = 0x12  # Alt
VK_PAUSE: int = 0x13
VK_CAPITAL: int = 0x14
VK_ESCAPE: int = 0x1B
VK_SPACE: int = 0x20
VK_PRIOR: int = 0x21  # Page Up
VK_NEXT: int = 0x22   # Page Down
VK_END: int = 0x23
VK_HOME: int = 0x24
VK_LEFT: int = 0x25
VK_UP: int = 0x26
VK_RIGHT: int = 0x27
VK_DOWN: int = 0x28
VK_SNAPSHOT: int = 0x2C  # Print Screen
VK_INSERT: int = 0x2D
VK_DELETE: int = 0x2E
VK_C: int = 0x43
VK_V: int = 0x56
VK_F9: int = 0x78
VK_LSHIFT: int = 0xA0
VK_RSHIFT: int = 0xA1
VK_LCONTROL: int = 0xA2
VK_RCONTROL: int = 0xA3
VK_LMENU: int = 0xA4
VK_RMENU: int = 0xA5

# Extended keys that require KEYEVENTF_EXTENDEDKEY
EXTENDED_KEYS = {
    VK_HOME,
    VK_END,
    VK_PRIOR,
    VK_NEXT,
    VK_LEFT,
    VK_UP,
    VK_RIGHT,
    VK_DOWN,
    VK_INSERT,
    VK_DELETE,
    VK_RCONTROL,
    VK_RMENU,
}

# Clipboard Formats & Flags
CF_TEXT: int = 1
CF_BITMAP: int = 2
CF_METAFILEPICT: int = 3
CF_OEMTEXT: int = 7
CF_PALETTE: int = 9
CF_UNICODETEXT: int = 13
CF_ENHMETAFILE: int = 14
CF_HDROP: int = 15
CF_LOCALE: int = 16

# GDI handle formats that must NOT be backed up or restored as HGLOBAL
GDI_CLIPBOARD_FORMATS = {
    CF_BITMAP,        # 2
    CF_METAFILEPICT,  # 3
    CF_PALETTE,       # 9
    CF_ENHMETAFILE,   # 14
}

GMEM_MOVEABLE: int = 0x0002
GMEM_ZEROINIT: int = 0x0040
GHND: int = GMEM_MOVEABLE | GMEM_ZEROINIT


# ---------------------------------------------------------------------------
# Structures
# ---------------------------------------------------------------------------
class KEYBDINPUT(ctypes.Structure):
    """Low-level keyboard input event data."""

    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    """Low-level mouse input event data."""

    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    """Hardware input event data."""

    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class _INPUT_UNION(ctypes.Union):
    """Union containing event data for INPUT."""

    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
        ("hi", HARDWAREINPUT),
    ]


class INPUT(ctypes.Structure):
    """Generic input structure passed to SendInput."""

    _fields_ = [
        ("type", wintypes.DWORD),
        ("u", _INPUT_UNION),
    ]


class KBDLLHOOKSTRUCT(ctypes.Structure):
    """Low-level keyboard hook information structure."""

    _fields_ = [
        ("vkCode", wintypes.DWORD),
        ("scanCode", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


# ---------------------------------------------------------------------------
# Function Prototypes
# ---------------------------------------------------------------------------
HOOKPROC = ctypes.WINFUNCTYPE(
    ctypes.c_longlong, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM
)

# user32 function signatures
user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
user32.SendInput.restype = wintypes.UINT

user32.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
user32.MapVirtualKeyW.restype = wintypes.UINT

user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = wintypes.SHORT

user32.GetKeyState.argtypes = [ctypes.c_int]
user32.GetKeyState.restype = wintypes.SHORT

user32.OpenClipboard.argtypes = [wintypes.HWND]
user32.OpenClipboard.restype = wintypes.BOOL

user32.CloseClipboard.argtypes = []
user32.CloseClipboard.restype = wintypes.BOOL

user32.EmptyClipboard.argtypes = []
user32.EmptyClipboard.restype = wintypes.BOOL

user32.GetClipboardData.argtypes = [wintypes.UINT]
user32.GetClipboardData.restype = wintypes.HANDLE

user32.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
user32.SetClipboardData.restype = wintypes.HANDLE

user32.IsClipboardFormatAvailable.argtypes = [wintypes.UINT]
user32.IsClipboardFormatAvailable.restype = wintypes.BOOL

user32.GetClipboardSequenceNumber.argtypes = []
user32.GetClipboardSequenceNumber.restype = wintypes.DWORD

user32.EnumClipboardFormats.argtypes = [wintypes.UINT]
user32.EnumClipboardFormats.restype = wintypes.UINT

user32.SetWindowsHookExW.argtypes = [
    ctypes.c_int,
    HOOKPROC,
    wintypes.HINSTANCE,
    wintypes.DWORD,
]
user32.SetWindowsHookExW.restype = wintypes.HHOOK

user32.UnhookWindowsHookEx.argtypes = [wintypes.HHOOK]
user32.UnhookWindowsHookEx.restype = wintypes.BOOL

user32.CallNextHookEx.argtypes = [
    wintypes.HHOOK,
    ctypes.c_int,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.CallNextHookEx.restype = ctypes.c_longlong

user32.GetMessageW.argtypes = [
    ctypes.POINTER(wintypes.MSG),
    wintypes.HWND,
    wintypes.UINT,
    wintypes.UINT,
]
user32.GetMessageW.restype = wintypes.BOOL

user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.TranslateMessage.restype = wintypes.BOOL

user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.restype = wintypes.LPARAM

user32.PostThreadMessageW.argtypes = [
    wintypes.DWORD,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.PostThreadMessageW.restype = wintypes.BOOL

# kernel32 function signatures
kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = wintypes.HGLOBAL

kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalLock.restype = ctypes.c_void_p

kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalUnlock.restype = wintypes.BOOL

kernel32.GlobalSize.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalSize.restype = ctypes.c_size_t

kernel32.GlobalFree.argtypes = [wintypes.HGLOBAL]
kernel32.GlobalFree.restype = wintypes.HGLOBAL

kernel32.GetCurrentThreadId.argtypes = []
kernel32.GetCurrentThreadId.restype = wintypes.DWORD


# ---------------------------------------------------------------------------
# Keystroke Helpers
# ---------------------------------------------------------------------------
def _create_keyboard_input(
    vk_code: int,
    is_up: bool = False,
    extra_info: int = SYNTHETIC_MARKER,
) -> INPUT:
    """Create a single INPUT struct for a keyboard event."""
    flags = 0
    if is_up:
        flags |= KEYEVENTF_KEYUP
    if vk_code in EXTENDED_KEYS:
        flags |= KEYEVENTF_EXTENDEDKEY

    scan_code = user32.MapVirtualKeyW(vk_code, 0)

    ki = KEYBDINPUT(
        wVk=vk_code,
        wScan=scan_code,
        dwFlags=flags,
        time=0,
        dwExtraInfo=extra_info,
    )
    inp = INPUT(type=INPUT_KEYBOARD)
    inp.u.ki = ki
    return inp


def send_inputs(inputs: List[INPUT]) -> bool:
    """Send an array of INPUT structures via user32.SendInput.

    Args:
        inputs: List of INPUT instances.

    Returns:
        True if all inputs were successfully inserted into the input stream.
    """
    if not inputs:
        return True
    n_inputs = len(inputs)
    input_array = (INPUT * n_inputs)(*inputs)
    sent = user32.SendInput(n_inputs, input_array, ctypes.sizeof(INPUT))
    return sent == n_inputs


def send_key_down(vk_code: int, extra_info: int = SYNTHETIC_MARKER) -> bool:
    """Synthesize a key-down event tagged with extra_info.

    Args:
        vk_code: Virtual key code.
        extra_info: Value placed into dwExtraInfo (default: SYNTHETIC_MARKER).

    Returns:
        True on success.
    """
    inp = _create_keyboard_input(vk_code, is_up=False, extra_info=extra_info)
    return send_inputs([inp])


def send_key_up(vk_code: int, extra_info: int = SYNTHETIC_MARKER) -> bool:
    """Synthesize a key-up event tagged with extra_info.

    Args:
        vk_code: Virtual key code.
        extra_info: Value placed into dwExtraInfo (default: SYNTHETIC_MARKER).

    Returns:
        True on success.
    """
    inp = _create_keyboard_input(vk_code, is_up=True, extra_info=extra_info)
    return send_inputs([inp])


def send_key_press(vk_code: int, extra_info: int = SYNTHETIC_MARKER) -> bool:
    """Synthesize an atomic key press (down then up) tagged with extra_info.

    Args:
        vk_code: Virtual key code.
        extra_info: Value placed into dwExtraInfo (default: SYNTHETIC_MARKER).

    Returns:
        True on success.
    """
    down_inp = _create_keyboard_input(vk_code, is_up=False, extra_info=extra_info)
    up_inp = _create_keyboard_input(vk_code, is_up=True, extra_info=extra_info)
    return send_inputs([down_inp, up_inp])


def flush_ime() -> bool:
    """Send VK_RIGHT (0x27) with SYNTHETIC_MARKER to commit Korean composition buffer.

    When typing Korean Hangul, the last syllable often sits in the IME composition
    buffer. Sending an arrow right keystroke cleanly commits it to the document
    without creating spurious newlines or modifying selection.

    Returns:
        True on success.
    """
    return send_key_press(VK_RIGHT, extra_info=SYNTHETIC_MARKER)


def select_current_line() -> bool:
    """Select from current cursor position back to the beginning of the line.

    Synthesizes: VK_SHIFT down -> VK_HOME press -> VK_SHIFT up.
    Tagged with SYNTHETIC_MARKER.

    Returns:
        True on success.
    """
    shift_down = _create_keyboard_input(VK_SHIFT, is_up=False, extra_info=SYNTHETIC_MARKER)
    home_down = _create_keyboard_input(VK_HOME, is_up=False, extra_info=SYNTHETIC_MARKER)
    home_up = _create_keyboard_input(VK_HOME, is_up=True, extra_info=SYNTHETIC_MARKER)
    shift_up = _create_keyboard_input(VK_SHIFT, is_up=True, extra_info=SYNTHETIC_MARKER)
    return send_inputs([shift_down, home_down, home_up, shift_up])


def copy_selection() -> bool:
    """Synthesize Ctrl + C (0x43) to copy the current selection to clipboard.

    Synthesizes: VK_CONTROL down -> 'C' press -> VK_CONTROL up.
    Tagged with SYNTHETIC_MARKER.

    Returns:
        True on success.
    """
    ctrl_down = _create_keyboard_input(VK_CONTROL, is_up=False, extra_info=SYNTHETIC_MARKER)
    c_down = _create_keyboard_input(VK_C, is_up=False, extra_info=SYNTHETIC_MARKER)
    c_up = _create_keyboard_input(VK_C, is_up=True, extra_info=SYNTHETIC_MARKER)
    ctrl_up = _create_keyboard_input(VK_CONTROL, is_up=True, extra_info=SYNTHETIC_MARKER)
    return send_inputs([ctrl_down, c_down, c_up, ctrl_up])


def paste_text() -> bool:
    """Synthesize Ctrl + V (0x56) to paste clipboard contents into active focus.

    Synthesizes: VK_CONTROL down -> 'V' press -> VK_CONTROL up.
    Tagged with SYNTHETIC_MARKER.

    Returns:
        True on success.
    """
    ctrl_down = _create_keyboard_input(VK_CONTROL, is_up=False, extra_info=SYNTHETIC_MARKER)
    v_down = _create_keyboard_input(VK_V, is_up=False, extra_info=SYNTHETIC_MARKER)
    v_up = _create_keyboard_input(VK_V, is_up=True, extra_info=SYNTHETIC_MARKER)
    ctrl_up = _create_keyboard_input(VK_CONTROL, is_up=True, extra_info=SYNTHETIC_MARKER)
    return send_inputs([ctrl_down, v_down, v_up, ctrl_up])


def unselect() -> bool:
    """Send VK_RIGHT press to clear active text selection and restore cursor.

    Returns:
        True on success.
    """
    return send_key_press(VK_RIGHT, extra_info=SYNTHETIC_MARKER)


def send_enter() -> bool:
    """Send VK_RETURN (0x0D) press tagged with SYNTHETIC_MARKER.

    Returns:
        True on success.
    """
    return send_key_press(VK_RETURN, extra_info=SYNTHETIC_MARKER)


# ---------------------------------------------------------------------------
# Win32 Clipboard Manager
# ---------------------------------------------------------------------------
@dataclass
class ClipboardBackup:
    """Data container preserving clipboard state across translation swaps."""

    text: Optional[str] = None
    formats: List[Tuple[int, bytes]] = field(default_factory=list)


def get_clipboard_sequence_number() -> int:
    """Get the current clipboard sequence number from user32.

    Returns:
        The current sequence number, incremented on every clipboard change.
    """
    return int(user32.GetClipboardSequenceNumber())


def _open_clipboard_with_retry(
    hwnd: Optional[int] = None,
    timeout_ms: float = 100.0,
    retry_interval: float = 0.005,
) -> bool:
    """Attempt to open clipboard with exponential/polling retry up to timeout_ms."""
    start = time.perf_counter()
    timeout_sec = timeout_ms / 1000.0
    while (time.perf_counter() - start) <= timeout_sec:
        if user32.OpenClipboard(hwnd):
            return True
        time.sleep(retry_interval)
    return False


def get_clipboard_text(
    timeout_ms: float = 100.0,
    retry_interval: float = 0.005,
) -> str:
    """Retrieve unicode text from Windows clipboard with retry loop up to timeout_ms.

    Args:
        timeout_ms: Maximum time in milliseconds to retry opening clipboard.
        retry_interval: Seconds to wait between open attempts.

    Returns:
        Unicode string from clipboard, or empty string if empty or unavailable.
    """
    if not _open_clipboard_with_retry(None, timeout_ms=timeout_ms, retry_interval=retry_interval):
        logger.debug("Failed to open clipboard within %.1f ms", timeout_ms)
        return ""

    try:
        if not user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
            return ""

        h_data = user32.GetClipboardData(CF_UNICODETEXT)
        if not h_data:
            return ""

        sz = kernel32.GlobalSize(h_data)
        if sz <= 0:
            return ""

        ptr = kernel32.GlobalLock(h_data)
        if not ptr:
            return ""

        try:
            raw_bytes = ctypes.string_at(ptr, sz)
            # Ensure length aligns to 2-byte UTF-16LE code units
            if len(raw_bytes) % 2 != 0:
                raw_bytes = raw_bytes[: len(raw_bytes) - 1]
            text = raw_bytes.decode("utf-16le", errors="replace")
            # Strip trailing null characters safely
            return text.rstrip("\x00")
        finally:
            kernel32.GlobalUnlock(h_data)
    except Exception as e:
        logger.debug("Exception reading clipboard text: %s", e)
        return ""
    finally:
        user32.CloseClipboard()


def set_clipboard_text(text: str, timeout_ms: float = 100.0) -> bool:
    """Set unicode text onto Windows clipboard with retry loop up to timeout_ms.

    Args:
        text: String to place onto clipboard.
        timeout_ms: Maximum time in milliseconds to retry opening clipboard.

    Returns:
        True if text was successfully set, False otherwise.
    """
    text_data = (text + "\0").encode("utf-16le")
    h_mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(text_data))
    if not h_mem:
        logger.debug("Failed to allocate global memory for clipboard text")
        return False

    ptr = kernel32.GlobalLock(h_mem)
    if not ptr:
        kernel32.GlobalFree(h_mem)
        return False

    try:
        ctypes.memmove(ptr, text_data, len(text_data))
    finally:
        kernel32.GlobalUnlock(h_mem)

    if not _open_clipboard_with_retry(None, timeout_ms=timeout_ms):
        kernel32.GlobalFree(h_mem)
        logger.debug("Failed to open clipboard for writing within %.1f ms", timeout_ms)
        return False

    try:
        user32.EmptyClipboard()
        h_res = user32.SetClipboardData(CF_UNICODETEXT, h_mem)
        if h_res:
            # System now owns h_mem; do NOT free it.
            return True
        else:
            kernel32.GlobalFree(h_mem)
            return False
    finally:
        user32.CloseClipboard()


def backup_clipboard(timeout_ms: float = 100.0) -> Optional[ClipboardBackup]:
    """Backup current clipboard contents (CF_UNICODETEXT and standard global memory formats).

    Explicitly skips GDI handles (CF_BITMAP, CF_PALETTE, CF_METAFILEPICT, CF_ENHMETAFILE)
    to prevent memory corruption or process crashes.

    Args:
        timeout_ms: Maximum time to wait for clipboard lock.

    Returns:
        ClipboardBackup containing saved data, or None if clipboard could not be opened.
    """
    if not _open_clipboard_with_retry(None, timeout_ms=timeout_ms):
        return None

    try:
        current_text: Optional[str] = None
        if user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
            h_text = user32.GetClipboardData(CF_UNICODETEXT)
            if h_text:
                sz_text = kernel32.GlobalSize(h_text)
                if sz_text > 0:
                    ptr = kernel32.GlobalLock(h_text)
                    if ptr:
                        try:
                            raw = ctypes.string_at(ptr, sz_text)
                            if len(raw) % 2 != 0:
                                raw = raw[: len(raw) - 1]
                            current_text = raw.decode("utf-16le", errors="replace").rstrip("\x00")
                        finally:
                            kernel32.GlobalUnlock(h_text)

        formats: List[Tuple[int, bytes]] = []
        fmt = 0
        while True:
            fmt = user32.EnumClipboardFormats(fmt)
            if fmt == 0:
                break

            # Explicitly skip GDI handles that are not global memory (HGLOBAL)
            if fmt in GDI_CLIPBOARD_FORMATS:
                continue

            h_data = user32.GetClipboardData(fmt)
            if not h_data:
                continue

            sz = kernel32.GlobalSize(h_data)
            if sz > 0:
                ptr = kernel32.GlobalLock(h_data)
                if ptr:
                    try:
                        formats.append((fmt, ctypes.string_at(ptr, sz)))
                    finally:
                        kernel32.GlobalUnlock(h_data)

        return ClipboardBackup(text=current_text, formats=formats)
    except Exception as e:
        logger.debug("Exception during clipboard backup: %s", e)
        return None
    finally:
        user32.CloseClipboard()


def restore_clipboard(
    backup: Optional[ClipboardBackup],
    timeout_ms: float = 100.0,
) -> bool:
    """Restore previously backed-up clipboard content.

    Explicitly skips GDI handles to prevent memory crashes.

    Args:
        backup: ClipboardBackup instance or None.
        timeout_ms: Maximum time to wait for clipboard lock.

    Returns:
        True on successful restoration or if backup is None.
    """
    if backup is None:
        return True

    if not _open_clipboard_with_retry(None, timeout_ms=timeout_ms):
        logger.debug("Failed to open clipboard for restore within %.1f ms", timeout_ms)
        return False

    try:
        user32.EmptyClipboard()

        if backup.formats:
            for fmt, data in backup.formats:
                # Explicitly skip GDI handles
                if fmt in GDI_CLIPBOARD_FORMATS:
                    continue

                h_mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
                if not h_mem:
                    continue
                ptr = kernel32.GlobalLock(h_mem)
                if not ptr:
                    kernel32.GlobalFree(h_mem)
                    continue
                try:
                    ctypes.memmove(ptr, data, len(data))
                finally:
                    kernel32.GlobalUnlock(h_mem)

                h_res = user32.SetClipboardData(fmt, h_mem)
                if not h_res:
                    kernel32.GlobalFree(h_mem)
            return True
        elif backup.text is not None:
            # Fallback text restore if format list was empty
            text_data = (backup.text + "\0").encode("utf-16le")
            h_mem = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(text_data))
            if h_mem:
                ptr = kernel32.GlobalLock(h_mem)
                if ptr:
                    try:
                        ctypes.memmove(ptr, text_data, len(text_data))
                    finally:
                        kernel32.GlobalUnlock(h_mem)
                    h_res = user32.SetClipboardData(CF_UNICODETEXT, h_mem)
                    if not h_res:
                        kernel32.GlobalFree(h_mem)
            return True

        return True
    except Exception as e:
        logger.debug("Exception during clipboard restore: %s", e)
        return False
    finally:
        user32.CloseClipboard()


def paste_and_restore_clipboard(
    backup: Optional[ClipboardBackup],
    settle_delay: float = 0.12,
) -> bool:
    """Paste translated text and restore the original clipboard after settle delay.

    Sends Ctrl+V paste, sleeps settle_delay (120ms default) so the target application
    can process the WM_PASTE message, and then cleanly restores the original clipboard.

    Args:
        backup: Original clipboard backup.
        settle_delay: Delay in seconds after paste before restoring clipboard.

    Returns:
        True if paste and restore succeeded.
    """
    paste_success = paste_text()
    if settle_delay > 0:
        time.sleep(settle_delay)
    restore_success = restore_clipboard(backup)
    return paste_success and restore_success
