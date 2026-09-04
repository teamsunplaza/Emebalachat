"""Unit tests for Windows Low-Level Input synthesis, ctypes alignment, and synthetic markers."""

from __future__ import annotations

import ctypes
import sys
import unittest
from pathlib import Path

# Ensure project root is in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from src.win32_input import (
    CF_TEXT,
    CF_UNICODETEXT,
    EXTENDED_KEYS,
    HARDWAREINPUT,
    INPUT,
    INPUT_HARDWARE,
    INPUT_KEYBOARD,
    INPUT_MOUSE,
    KBDLLHOOKSTRUCT,
    KEYBDINPUT,
    KEYEVENTF_EXTENDEDKEY,
    KEYEVENTF_KEYUP,
    KEYEVENTF_UNICODE,
    MOUSEINPUT,
    SYNTHETIC_MARKER,
    ULONG_PTR,
    VK_BACK,
    VK_C,
    VK_CONTROL,
    VK_F9,
    VK_HOME,
    VK_RETURN,
    VK_RIGHT,
    VK_SHIFT,
    VK_V,
    ClipboardBackup,
    _create_keyboard_input,
    restore_clipboard,
)


class TestWin32Constants(unittest.TestCase):
    """Test Win32 API constants and virtual key definitions."""

    def test_synthetic_marker_value(self) -> None:
        """Verify the exact magic value of SYNTHETIC_MARKER."""
        self.assertEqual(SYNTHETIC_MARKER, 0x1337BEEF)

    def test_virtual_key_codes(self) -> None:
        """Verify standard Windows virtual key code mapping."""
        self.assertEqual(VK_BACK, 0x08)
        self.assertEqual(VK_RETURN, 0x0D)
        self.assertEqual(VK_SHIFT, 0x10)
        self.assertEqual(VK_CONTROL, 0x11)
        self.assertEqual(VK_HOME, 0x24)
        self.assertEqual(VK_RIGHT, 0x27)
        self.assertEqual(VK_C, 0x43)
        self.assertEqual(VK_V, 0x56)
        self.assertEqual(VK_F9, 0x78)

    def test_input_type_constants(self) -> None:
        """Verify INPUT type discriminator constants."""
        self.assertEqual(INPUT_MOUSE, 0)
        self.assertEqual(INPUT_KEYBOARD, 1)
        self.assertEqual(INPUT_HARDWARE, 2)

    def test_key_event_flags(self) -> None:
        """Verify key event flag constants."""
        self.assertEqual(KEYEVENTF_EXTENDEDKEY, 0x0001)
        self.assertEqual(KEYEVENTF_KEYUP, 0x0002)
        self.assertEqual(KEYEVENTF_UNICODE, 0x0004)

    def test_clipboard_format_constants(self) -> None:
        """Verify standard clipboard format identifiers."""
        self.assertEqual(CF_TEXT, 1)
        self.assertEqual(CF_UNICODETEXT, 13)

    def test_extended_keys_membership(self) -> None:
        """Verify EXTENDED_KEYS contains cursor navigation and special keys."""
        self.assertIn(VK_HOME, EXTENDED_KEYS)
        self.assertIn(VK_RIGHT, EXTENDED_KEYS)
        self.assertNotIn(VK_RETURN, EXTENDED_KEYS)
        self.assertNotIn(VK_C, EXTENDED_KEYS)
        self.assertNotIn(VK_V, EXTENDED_KEYS)


class TestCtypesStructuresAndAlignment(unittest.TestCase):
    """Test Win32 structure sizes, alignments, and member offsets."""

    def setUp(self) -> None:
        self.is_64bit = ctypes.sizeof(ctypes.c_void_p) == 8

    def test_ulong_ptr_size(self) -> None:
        """ULONG_PTR must match pointer size on architecture."""
        expected_size = 8 if self.is_64bit else 4
        self.assertEqual(ctypes.sizeof(ULONG_PTR), expected_size)

    def test_keybdinput_size_and_offsets(self) -> None:
        """Verify KEYBDINPUT structure alignment and offsets."""
        expected_size = 24 if self.is_64bit else 16
        self.assertEqual(ctypes.sizeof(KEYBDINPUT), expected_size)

        self.assertEqual(KEYBDINPUT.wVk.offset, 0)
        self.assertEqual(KEYBDINPUT.wScan.offset, 2)
        self.assertEqual(KEYBDINPUT.dwFlags.offset, 4)
        self.assertEqual(KEYBDINPUT.time.offset, 8)
        expected_extra_offset = 16 if self.is_64bit else 12
        self.assertEqual(KEYBDINPUT.dwExtraInfo.offset, expected_extra_offset)

    def test_mouseinput_size(self) -> None:
        """Verify MOUSEINPUT structure size."""
        expected_size = 32 if self.is_64bit else 24
        self.assertEqual(ctypes.sizeof(MOUSEINPUT), expected_size)

    def test_input_size_and_offsets(self) -> None:
        """Verify INPUT structure size and union offset."""
        expected_size = 40 if self.is_64bit else 28
        self.assertEqual(ctypes.sizeof(INPUT), expected_size)
        self.assertEqual(INPUT.type.offset, 0)
        expected_union_offset = 8 if self.is_64bit else 4
        self.assertEqual(INPUT.u.offset, expected_union_offset)

    def test_kbdllhookstruct_size_and_offsets(self) -> None:
        """Verify KBDLLHOOKSTRUCT size matching WH_KEYBOARD_LL layout."""
        expected_size = 24 if self.is_64bit else 20
        self.assertEqual(ctypes.sizeof(KBDLLHOOKSTRUCT), expected_size)

        self.assertEqual(KBDLLHOOKSTRUCT.vkCode.offset, 0)
        self.assertEqual(KBDLLHOOKSTRUCT.scanCode.offset, 4)
        self.assertEqual(KBDLLHOOKSTRUCT.flags.offset, 8)
        self.assertEqual(KBDLLHOOKSTRUCT.time.offset, 12)
        expected_extra_offset = 16
        self.assertEqual(KBDLLHOOKSTRUCT.dwExtraInfo.offset, expected_extra_offset)


class TestSyntheticKeystrokeTagging(unittest.TestCase):
    """Test synthetic marker tagging and input construction."""

    def test_synthetic_marker_check(self) -> None:
        """Verify synthetic flag identification logic."""
        def is_synthetic(extra_info: int) -> bool:
            return bool(extra_info == SYNTHETIC_MARKER)

        self.assertTrue(is_synthetic(0x1337BEEF))
        self.assertFalse(is_synthetic(0))
        self.assertFalse(is_synthetic(0x12345678))
        self.assertFalse(is_synthetic(1))

    def test_create_keyboard_input_key_down(self) -> None:
        """Verify synthetic key-down input creation."""
        inp = _create_keyboard_input(VK_RETURN, is_up=False, extra_info=SYNTHETIC_MARKER)
        self.assertEqual(inp.type, INPUT_KEYBOARD)
        self.assertEqual(inp.u.ki.wVk, VK_RETURN)
        self.assertEqual(inp.u.ki.dwExtraInfo, SYNTHETIC_MARKER)
        self.assertEqual(inp.u.ki.dwFlags & KEYEVENTF_KEYUP, 0)

    def test_create_keyboard_input_key_up(self) -> None:
        """Verify synthetic key-up input creation."""
        inp = _create_keyboard_input(VK_RETURN, is_up=True, extra_info=SYNTHETIC_MARKER)
        self.assertEqual(inp.type, INPUT_KEYBOARD)
        self.assertEqual(inp.u.ki.wVk, VK_RETURN)
        self.assertEqual(inp.u.ki.dwExtraInfo, SYNTHETIC_MARKER)
        self.assertNotEqual(inp.u.ki.dwFlags & KEYEVENTF_KEYUP, 0)

    def test_create_keyboard_input_extended_key(self) -> None:
        """Verify extended key flag applied for VK_HOME and VK_RIGHT."""
        inp_home = _create_keyboard_input(VK_HOME, is_up=False, extra_info=SYNTHETIC_MARKER)
        self.assertNotEqual(inp_home.u.ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0)

        inp_right = _create_keyboard_input(VK_RIGHT, is_up=False, extra_info=SYNTHETIC_MARKER)
        self.assertNotEqual(inp_right.u.ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0)

        inp_return = _create_keyboard_input(VK_RETURN, is_up=False, extra_info=SYNTHETIC_MARKER)
        self.assertEqual(inp_return.u.ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0)

    def test_hook_struct_synthetic_detection(self) -> None:
        """Verify KBDLLHOOKSTRUCT population and synthetic extra info reading."""
        hook_info = KBDLLHOOKSTRUCT(
            vkCode=VK_RETURN,
            scanCode=0x1C,
            flags=0,
            time=12345,
            dwExtraInfo=SYNTHETIC_MARKER,
        )
        self.assertEqual(hook_info.vkCode, VK_RETURN)
        self.assertEqual(hook_info.dwExtraInfo, SYNTHETIC_MARKER)
        is_synthetic = bool(hook_info.dwExtraInfo == SYNTHETIC_MARKER)
        self.assertTrue(is_synthetic)

        # User hardware event
        user_info = KBDLLHOOKSTRUCT(
            vkCode=VK_RETURN,
            scanCode=0x1C,
            flags=0,
            time=12350,
            dwExtraInfo=0,
        )
        self.assertFalse(bool(user_info.dwExtraInfo == SYNTHETIC_MARKER))


class TestClipboardBackupContainer(unittest.TestCase):
    """Test clipboard backup container dataclass and fallback restore."""

    def test_clipboard_backup_defaults(self) -> None:
        """Verify ClipboardBackup default values."""
        backup = ClipboardBackup()
        self.assertIsNone(backup.text)
        self.assertEqual(backup.formats, [])

    def test_clipboard_backup_custom_data(self) -> None:
        """Verify ClipboardBackup stores custom formats and text."""
        raw_bytes = "Test string".encode("utf-16le")
        backup = ClipboardBackup(
            text="Test string",
            formats=[(CF_UNICODETEXT, raw_bytes)],
        )
        self.assertEqual(backup.text, "Test string")
        self.assertEqual(len(backup.formats), 1)
        self.assertEqual(backup.formats[0][0], CF_UNICODETEXT)
        self.assertEqual(backup.formats[0][1], raw_bytes)

    def test_restore_none_backup(self) -> None:
        """Restoring None backup should be a safe no-op returning True."""
        result = restore_clipboard(None)
        self.assertTrue(result)


if __name__ == "__main__":
    unittest.main()
