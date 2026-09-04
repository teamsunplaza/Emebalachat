"""Main Application Coordinator for Emebalachat.

Coordinates configuration, local translation engine (Hy-MT2 / llama-cpp-python),
Windows low-level input hook (WH_KEYBOARD_LL), floating status badge overlay,
and system tray manager. Manages application lifecycle and thread-safe UI updates.
"""

from __future__ import annotations

import logging
import os
import signal
import sys
import tkinter as tk
from typing import Optional

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("Emebalachat")

# Import core domain modules
from src.config import Config, SUPPORTED_LANGUAGES, SOURCE_LANGUAGES
from src.sound import SoundPlayer
from src.engine import TranslationEngine
from src.hook import GlobalInputHook
from src.ui.badge import FloatingBadge, BadgeState
from src.ui.tray import TrayManager


class EmebalachatApp:
    """Master application coordinator managing lifecycle, hooks, and UI layers."""

    def __init__(self, root: Optional[tk.Tk] = None) -> None:
        logger.info("Initializing Emebalachat Assistant...")
        self._is_shutting_down: bool = False
        self._owns_root: bool = root is None

        # 1. Configuration Manager
        self.config: Config = Config()
        logger.info(
            "Config loaded (Source: %s, Target: %s, Auto-Send: %s, Sound: %s)",
            self.config.source_language,
            self.config.target_language,
            self.config.auto_send,
            self.config.sound_enabled,
        )

        # 2. Audio Feedback Manager
        self.sound: SoundPlayer = SoundPlayer(config=self.config)

        # 3. Translation Engine
        self.engine: TranslationEngine = TranslationEngine(config=self.config)
        if self.engine.is_mock:
            logger.info("TranslationEngine running in fallback mock mode.")
        else:
            logger.info("TranslationEngine initialized with neural model.")

        # 4. Tkinter Root and Floating Status Badge
        self.root: tk.Tk = root if root is not None else tk.Tk()
        self.badge: FloatingBadge = FloatingBadge(
            root=self.root,
            source_lang=self.config.source_language,
            target_lang=self.config.target_language,
            initial_state=BadgeState.ACTIVE,
            on_toggle_callback=self.toggle_translation_mode,
        )

        # 5. System Tray Icon and Menu Manager
        self.tray: TrayManager = TrayManager(
            initial_enabled=True,
            source_language=self.config.source_language,
            target_language=self.config.target_language,
            auto_send=self.config.auto_send,
            sound_enabled=self.config.sound_enabled,
            badge_visible=self.badge.is_visible,
            on_toggle_mode=self.set_translation_mode,
            on_toggle_auto_send=self.set_auto_send,
            on_select_source_lang=self.set_source_language,
            on_select_target_lang=self.set_target_language,
            on_swap_languages=self.swap_languages,
            on_toggle_sound=self.set_sound_enabled,
            on_toggle_badge=self.set_badge_visible,
            on_exit=self.request_shutdown,
        )

        # 6. Windows Low-Level Input Hook
        self.hook: GlobalInputHook = GlobalInputHook(
            engine=self.engine,
            config=self.config,
            sound_player=self.sound,
            target_languages=SUPPORTED_LANGUAGES,
            on_status_change=self._handle_hook_status_change,
            on_language_change=self._handle_hook_language_change,
            on_mode_change=self._handle_hook_mode_change,
            on_translation=self._handle_hook_translation,
        )

        # Register window and OS signals
        self.root.protocol("WM_DELETE_WINDOW", self.request_shutdown)
        self._register_signals()

    def _register_signals(self) -> None:
        """Register OS signal handlers for graceful termination."""
        try:
            signal.signal(signal.SIGINT, lambda sig, frame: self.request_shutdown())
            signal.signal(signal.SIGTERM, lambda sig, frame: self.request_shutdown())
        except Exception as e:
            logger.debug("Signal registration skipped: %s", e)

    # -------------------------------------------------------------------------
    # Hook Event Handlers (Dispatched from worker or hook threads)
    # -------------------------------------------------------------------------

    def _handle_hook_status_change(self, status: str, **kwargs) -> None:
        """Handle status transition dispatched from low-level hook."""
        def _update() -> None:
            if status == "translating":
                self.badge.update_status(
                    BadgeState.TRANSLATING,
                    target_lang=self.config.target_language,
                    source_lang=self.config.source_language,
                )
                self.tray.update_state(status="translating")
            elif status in ("active", "idle"):
                self.badge.update_status(
                    BadgeState.ACTIVE,
                    target_lang=self.config.target_language,
                    source_lang=self.config.source_language,
                )
                self.tray.update_state(is_enabled=True, status="active")
            elif status == "inactive":
                self.badge.update_status(
                    BadgeState.DISABLED,
                    target_lang=self.config.target_language,
                    source_lang=self.config.source_language,
                )
                self.tray.update_state(is_enabled=False, status="disabled")

        try:
            self.root.after(0, _update)
        except tk.TclError:
            pass

    def _handle_hook_language_change(self, new_target_lang: str) -> None:
        """Handle target language cycle via Ctrl+F9."""
        def _update() -> None:
            self.config.target_language = new_target_lang
            self.config.save()
            active_state = BadgeState.ACTIVE if self.hook.is_active else BadgeState.DISABLED
            self.badge.update_status(
                active_state,
                target_lang=new_target_lang,
                source_lang=self.config.source_language,
            )
            self.tray.update_state(target_language=new_target_lang)

        try:
            self.root.after(0, _update)
        except tk.TclError:
            pass

    def _handle_hook_mode_change(self, auto_send: bool) -> None:
        """Handle auto-send toggle via Ctrl+Shift+Enter."""
        def _update() -> None:
            self.config.auto_send = auto_send
            self.config.save()
            self.tray.update_state(auto_send=auto_send)

        try:
            self.root.after(0, _update)
        except tk.TclError:
            pass

    def _handle_hook_translation(self, original_text: str, translated_text: str) -> None:
        """Log successful translation event."""
        logger.info(
            "Translated: '%s' ➔ '%s'",
            original_text[:40] + ("..." if len(original_text) > 40 else ""),
            translated_text[:40] + ("..." if len(translated_text) > 40 else ""),
        )

    # -------------------------------------------------------------------------
    # Coordinator Actions (Invoked by Tray, Badge, or Hotkeys)
    # -------------------------------------------------------------------------

    def toggle_translation_mode(self) -> None:
        """Toggle translation mode ON/OFF from badge double-click or external caller."""
        self.hook.toggle_translation_mode()

    def set_translation_mode(self, enabled: bool) -> None:
        """Set translation mode explicitly from system tray menu."""
        self.hook.is_active = enabled
        if enabled:
            self.sound.play_toggle_on()
            status = BadgeState.ACTIVE
        else:
            self.sound.play_toggle_off()
            status = BadgeState.DISABLED

        self.root.after(
            0,
            lambda: self.badge.update_status(
                status,
                target_lang=self.config.target_language,
                source_lang=self.config.source_language,
            ),
        )
        self.tray.update_state(is_enabled=enabled)

    def set_auto_send(self, auto_send: bool) -> None:
        """Toggle or set auto-send mode from system tray."""
        self.hook.auto_send = auto_send
        self.config.auto_send = auto_send
        self.config.save()
        self.sound.play_mode_change()
        self.tray.update_state(auto_send=auto_send)

    def set_source_language(self, lang: str) -> None:
        """Select a new source language from tray menu."""
        self.config.source_language = lang
        self.config.save()
        self.hook.source_language = lang
        self.sound.play_lang_change()

        state = BadgeState.ACTIVE if self.hook.is_active else BadgeState.DISABLED
        self.root.after(
            0,
            lambda: self.badge.update_status(
                state,
                target_lang=self.config.target_language,
                source_lang=lang,
            ),
        )
        self.tray.update_state(source_language=lang)

    def set_target_language(self, lang: str) -> None:
        """Select a new target language from tray menu."""
        self.config.target_language = lang
        self.config.save()
        self.hook.target_language = lang
        self.sound.play_lang_change()

        state = BadgeState.ACTIVE if self.hook.is_active else BadgeState.DISABLED
        self.root.after(
            0,
            lambda: self.badge.update_status(
                state,
                target_lang=lang,
                source_lang=self.config.source_language,
            ),
        )
        self.tray.update_state(target_language=lang)

    def swap_languages(self) -> None:
        """Swap source and target languages if source is not Auto Detect."""
        if self.config.source_language == "Auto Detect":
            return

        src, tgt = self.config.source_language, self.config.target_language
        self.config.source_language, self.config.target_language = tgt, src
        self.config.save()

        self.hook.source_language = tgt
        self.hook.target_language = src
        self.sound.play_lang_change()

        state = BadgeState.ACTIVE if self.hook.is_active else BadgeState.DISABLED
        self.root.after(
            0,
            lambda: self.badge.update_status(
                state,
                target_lang=src,
                source_lang=tgt,
            ),
        )
        self.tray.update_state(source_language=tgt, target_language=src)

    def set_sound_enabled(self, enabled: bool) -> None:
        """Toggle sound feedback from system tray."""
        self.config.sound_enabled = enabled
        self.config.save()
        self.tray.update_state(sound_enabled=enabled)

    def set_badge_visible(self, visible: bool) -> None:
        """Toggle floating status badge overlay visibility."""
        if visible:
            self.badge.show()
        else:
            self.badge.hide()
        self.tray.update_state(badge_visible=visible)

    # -------------------------------------------------------------------------
    # Application Lifecycle
    # -------------------------------------------------------------------------

    def run(self) -> None:
        """Start all services, detached tray, input hook, and enter Tkinter mainloop."""
        logger.info("Starting System Tray Icon...")
        self.tray.start()

        logger.info("Installing Windows Low-Level Keyboard Hook...")
        hook_ok = self.hook.start()
        if not hook_ok:
            logger.warning("Keyboard hook failed to start. Normal typing will not be intercepted.")
        else:
            logger.info("Keyboard hook active. Ready for global translation.")

        logger.info("Entering Tkinter GUI event loop. Emebalachat is running!")
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            self.request_shutdown()

    def request_shutdown(self) -> None:
        """Thread-safe request to shut down the application."""
        if self._is_shutting_down:
            return
        try:
            self.root.after(0, self._perform_shutdown)
        except tk.TclError:
            self._perform_shutdown()

    def _perform_shutdown(self, exit_process: bool = True) -> None:
        """Execute complete resource cleanup and exit.

        Args:
            exit_process: If True, calls sys.exit(0) upon cleanup completion.
        """
        if self._is_shutting_down:
            return
        self._is_shutting_down = True
        logger.info("Shutting down Emebalachat...")

        # 1. Stop keyboard hook
        try:
            self.hook.stop()
            logger.info("Keyboard hook stopped.")
        except Exception as e:
            logger.debug("Error stopping hook: %s", e)

        # 2. Stop system tray
        try:
            self.tray.stop()
            logger.info("System tray stopped.")
        except Exception as e:
            logger.debug("Error stopping tray: %s", e)

        # 3. Destroy badge and Tk root
        try:
            self.badge.destroy()
        except Exception as e:
            logger.debug("Error destroying badge: %s", e)

        try:
            self.root.quit()
            if self._owns_root:
                self.root.destroy()
            logger.info("Tkinter main loop exited.")
        except Exception as e:
            logger.debug("Error destroying Tk root: %s", e)

        logger.info("Emebalachat has terminated cleanly.")
        if exit_process:
            sys.exit(0)


def main() -> None:
    """Entry point for Emebalachat application."""
    app = EmebalachatApp()
    app.run()


if __name__ == "__main__":
    main()
