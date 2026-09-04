"""Unit tests for Domain 2: win32_input and hook modules."""

from __future__ import annotations

import ctypes
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import MagicMock

# Ensure project root is in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import src.win32_input as wi
from src.hook import GlobalInputHook, default_should_translate


class TestWin32InputStructures(unittest.TestCase):
    """Test Win32 ctypes structure alignments and constants."""

    def test_synthetic_marker_constant(self) -> None:
        """Verify SYNTHETIC_MARKER is 0x1337BEEF."""
        self.assertEqual(wi.SYNTHETIC_MARKER, 0x1337BEEF)

    def test_structure_sizes(self) -> None:
        """Verify struct sizes align with 64-bit Windows ABI."""
        is_64bit = ctypes.sizeof(ctypes.c_void_p) == 8
        if is_64bit:
            self.assertEqual(ctypes.sizeof(wi.INPUT), 40)
            self.assertEqual(ctypes.sizeof(wi.KEYBDINPUT), 24)
            self.assertEqual(ctypes.sizeof(wi.KBDLLHOOKSTRUCT), 24)


class TestClipboardSafeSwap(unittest.TestCase):
    """Test clipboard reading, writing, backup, and restore routines."""

    def test_set_and_get_clipboard_text(self) -> None:
        """Verify unicode text can be written and read back faithfully."""
        test_content = "테스트 유니코드 문자열: Hello World 123! ✨"
        self.assertTrue(wi.set_clipboard_text(test_content))
        retrieved = wi.get_clipboard_text()
        self.assertEqual(retrieved, test_content)

    def test_backup_and_restore_clipboard(self) -> None:
        """Verify original clipboard contents are preserved across overwrites."""
        original_text = "Original Clip Content: https://github.com/test"
        self.assertTrue(wi.set_clipboard_text(original_text))

        backup = wi.backup_clipboard()
        self.assertIsNotNone(backup)
        self.assertEqual(backup.text, original_text)

        # Overwrite with temporary translation
        temp_text = "Temporary Replacement Text"
        self.assertTrue(wi.set_clipboard_text(temp_text))
        self.assertEqual(wi.get_clipboard_text(), temp_text)

        # Restore original clipboard
        self.assertTrue(wi.restore_clipboard(backup))
        self.assertEqual(wi.get_clipboard_text(), original_text)


class TestSmartBypassMultiLanguage(unittest.TestCase):
    """Test i18n should_translate evaluation for all supported language flows."""

    def test_korean_text(self) -> None:
        self.assertTrue(default_should_translate("안녕하세요", target_language="English"))
        self.assertTrue(default_should_translate("수고하셨습니다", target_language="Japanese"))
        self.assertTrue(default_should_translate("ㅋㅋㅋ", target_language="English"))
        # If target is Korean, should not translate
        self.assertFalse(default_should_translate("안녕하세요", target_language="Korean"))

    def test_english_text(self) -> None:
        # If target is English, English should not translate
        self.assertFalse(default_should_translate("Hello world!", target_language="English"))
        # If target is Korean or Japanese, English should translate
        self.assertTrue(default_should_translate("Hello world!", target_language="Korean"))
        self.assertTrue(default_should_translate("Good game", target_language="Japanese"))

    def test_vietnamese_text(self) -> None:
        self.assertTrue(default_should_translate("Xin chào bạn", target_language="Korean"))
        self.assertFalse(default_should_translate("Xin chào bạn", target_language="Vietnamese"))

    def test_urls_and_symbols_bypass(self) -> None:
        self.assertFalse(default_should_translate("https://example.com/api"))
        self.assertFalse(default_should_translate("www.google.com"))
        self.assertFalse(default_should_translate("1234567890"))
        self.assertFalse(default_should_translate("!@#$%^&*()_+"))
        self.assertFalse(default_should_translate(""))
        self.assertFalse(default_should_translate("   \t\n  "))
        self.assertFalse(default_should_translate(None))


class TestGlobalInputHook(unittest.TestCase):
    """Test GlobalInputHook state machine, hotkey actions, and lifecycle."""

    def setUp(self) -> None:
        self.status_cb = MagicMock()
        self.lang_cb = MagicMock()
        self.mode_cb = MagicMock()
        self.trans_cb = MagicMock()

        self.mock_engine = MagicMock()
        self.mock_engine.translate.side_effect = lambda t, target_lang="English": f"[Tr: {t}]"

        self.hook = GlobalInputHook(
            engine=self.mock_engine,
            target_languages=["English", "Korean", "Japanese"],
            on_status_change=self.status_cb,
            on_language_change=self.lang_cb,
            on_mode_change=self.mode_cb,
            on_translation=self.trans_cb,
        )

    def tearDown(self) -> None:
        if self.hook.is_alive():
            self.hook.stop()

    def test_initial_state(self) -> None:
        """Verify default state has is_active=True and auto_send=False."""
        self.assertTrue(self.hook.is_active)
        self.assertFalse(self.hook.auto_send)
        self.assertEqual(self.hook.target_language, "English")

    def test_toggle_translation_mode(self) -> None:
        """Verify F9 action toggles mode and fires status callback."""
        self.assertFalse(self.hook.toggle_translation_mode())
        self.assertFalse(self.hook.is_active)
        self.status_cb.assert_called_with("inactive")

        self.assertTrue(self.hook.toggle_translation_mode())
        self.assertTrue(self.hook.is_active)
        self.status_cb.assert_called_with("active")

    def test_cycle_target_language(self) -> None:
        """Verify Ctrl+F9 action cycles through languages and fires callback."""
        new_lang = self.hook.cycle_target_language()
        self.assertEqual(new_lang, "Korean")
        self.assertEqual(self.hook.target_language, "Korean")
        self.lang_cb.assert_called_with("Korean")

        new_lang2 = self.hook.cycle_target_language()
        self.assertEqual(new_lang2, "Japanese")
        self.assertEqual(self.hook.target_language, "Japanese")
        self.lang_cb.assert_called_with("Japanese")

        # Wraps around
        new_lang3 = self.hook.cycle_target_language()
        self.assertEqual(new_lang3, "English")
        self.assertEqual(self.hook.target_language, "English")

    def test_toggle_auto_send(self) -> None:
        """Verify Ctrl+Shift+Enter toggles auto_send and fires callback."""
        self.assertTrue(self.hook.toggle_auto_send())
        self.assertTrue(self.hook.auto_send)
        self.mode_cb.assert_called_with(True)

        self.assertFalse(self.hook.toggle_auto_send())
        self.assertFalse(self.hook.auto_send)
        self.mode_cb.assert_called_with(False)

    def test_hook_thread_lifecycle(self) -> None:
        """Verify low-level hook thread starts and stops cleanly."""
        started = self.hook.start(timeout=2.0)
        self.assertTrue(started)
        self.assertTrue(self.hook.is_alive())

        self.hook.stop(timeout=2.0)
        self.assertFalse(self.hook.is_alive())


if __name__ == "__main__":
    unittest.main()
