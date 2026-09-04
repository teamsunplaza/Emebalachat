"""Unit tests for Domain 3: UI, Badge, Tray, and Application Coordinator.

Verifies FloatingBadge lifecycle, TrayManager menu and icon generation,
and master coordinator wiring.
"""

from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch
import tkinter as tk

from src.ui.badge import FloatingBadge, BadgeState, get_lang_code
from src.ui.tray import TrayManager, generate_icon_image
from main import EmebalachatApp


class TestBadgeLanguageCodes(unittest.TestCase):
    """Test short code mapping helper for status badge."""

    def test_known_languages(self):
        self.assertEqual(get_lang_code("Korean"), "KO")
        self.assertEqual(get_lang_code("English"), "EN")
        self.assertEqual(get_lang_code("Japanese"), "JA")
        self.assertEqual(get_lang_code("Vietnamese"), "VI")
        self.assertEqual(get_lang_code("Chinese Simplified"), "ZH-CN")
        self.assertEqual(get_lang_code("Auto Detect"), "Auto")

    def test_short_code_pass_through(self):
        self.assertEqual(get_lang_code("KO"), "KO")
        self.assertEqual(get_lang_code("EN"), "EN")
        self.assertEqual(get_lang_code("ZH-TW"), "ZH-TW")

    def test_none_or_unknown(self):
        self.assertEqual(get_lang_code(None), "Auto")
        self.assertEqual(get_lang_code(""), "Auto")


_shared_test_root: Optional[tk.Tk] = None


def get_shared_test_root() -> tk.Tk:
    """Retrieve or lazily instantiate the shared Tk root instance for tests."""
    global _shared_test_root
    if _shared_test_root is None:
        _shared_test_root = tk.Tk()
        _shared_test_root.withdraw()
    return _shared_test_root


def tearDownModule() -> None:
    """Tear down shared Tk root upon completion of all test cases in this module."""
    global _shared_test_root
    if _shared_test_root is not None:
        try:
            _shared_test_root.destroy()
        except Exception:
            pass
        _shared_test_root = None


class TestFloatingBadgeLifecycle(unittest.TestCase):
    """Test FloatingBadge window instantiation and state transitions."""

    def setUp(self):
        self.root = get_shared_test_root()

    def tearDown(self):
        pass

    def test_badge_creation_and_update(self):
        toggle_called = False

        def on_toggle():
            nonlocal toggle_called
            toggle_called = True

        badge = FloatingBadge(
            root=self.root,
            source_lang="Korean",
            target_lang="English",
            initial_state=BadgeState.ACTIVE,
            on_toggle_callback=on_toggle,
        )

        self.assertEqual(badge.state, BadgeState.ACTIVE)
        self.assertEqual(badge.source_lang, "Korean")
        self.assertEqual(badge.target_lang, "English")
        self.assertTrue(badge.is_visible)

        # Update to translating
        badge.update_status(BadgeState.TRANSLATING)
        self.root.update_idletasks()
        self.assertEqual(badge.state, BadgeState.TRANSLATING)

        # Update to disabled
        badge.update_status(BadgeState.DISABLED)
        self.root.update_idletasks()
        self.assertEqual(badge.state, BadgeState.DISABLED)

        # Update languages
        badge.update_status(BadgeState.ACTIVE, target_lang="Japanese", source_lang="Vietnamese")
        self.root.update_idletasks()
        self.assertEqual(badge.source_lang, "Vietnamese")
        self.assertEqual(badge.target_lang, "Japanese")

        # Test hide/show
        badge.hide()
        self.root.update_idletasks()
        self.assertFalse(badge.is_visible)

        badge.show()
        self.root.update_idletasks()
        self.assertTrue(badge.is_visible)

        # Test double click trigger
        badge._on_double_click(None)
        self.assertTrue(toggle_called)


class TestTrayIconAndManager(unittest.TestCase):
    """Test TrayManager menu construction and icon image generation."""

    def test_icon_generation(self):
        img_active = generate_icon_image("active", 64)
        self.assertEqual(img_active.size, (64, 64))
        self.assertEqual(img_active.mode, "RGBA")

        img_trans = generate_icon_image("translating", 64)
        self.assertEqual(img_trans.size, (64, 64))

        img_disabled = generate_icon_image("disabled", 64)
        self.assertEqual(img_disabled.size, (64, 64))

    def test_tray_manager_init_and_state(self):
        tray = TrayManager(
            initial_enabled=True,
            source_language="Auto Detect",
            target_language="English",
            auto_send=True,
            sound_enabled=True,
            badge_visible=True,
        )

        self.assertTrue(tray.is_enabled)
        self.assertEqual(tray.source_language, "Auto Detect")
        self.assertEqual(tray.target_language, "English")
        self.assertTrue(tray.auto_send)
        self.assertTrue(tray.sound_enabled)
        self.assertTrue(tray.badge_visible)

        # Update state
        tray.update_state(
            is_enabled=False,
            source_language="Vietnamese",
            target_language="Korean",
            auto_send=False,
            sound_enabled=False,
            badge_visible=False,
        )

        self.assertFalse(tray.is_enabled)
        self.assertEqual(tray.source_language, "Vietnamese")
        self.assertEqual(tray.target_language, "Korean")
        self.assertFalse(tray.auto_send)
        self.assertFalse(tray.sound_enabled)
        self.assertFalse(tray.badge_visible)


class TestEmebalachatAppCoordinator(unittest.TestCase):
    """Test master application coordinator initialization and lifecycle."""

    def test_app_coordinator_wiring(self):
        with patch.object(EmebalachatApp, "run", return_value=None):
            app = EmebalachatApp(root=get_shared_test_root())
            app.root.withdraw()

            self.assertIsNotNone(app.config)
            self.assertIsNotNone(app.engine)
            self.assertIsNotNone(app.hook)
            self.assertIsNotNone(app.badge)
            self.assertIsNotNone(app.tray)

            # Test toggling mode
            app.set_translation_mode(False)
            self.assertFalse(app.hook.is_active)

            app.set_translation_mode(True)
            self.assertTrue(app.hook.is_active)

            # Test language selection and swap
            app.set_source_language("Vietnamese")
            app.set_target_language("Korean")
            self.assertEqual(app.config.source_language, "Vietnamese")
            self.assertEqual(app.config.target_language, "Korean")

            app.swap_languages()
            self.assertEqual(app.config.source_language, "Korean")
            self.assertEqual(app.config.target_language, "Vietnamese")

            # Clean shutdown without exiting python process
            app._perform_shutdown(exit_process=False)


if __name__ == "__main__":
    unittest.main()
