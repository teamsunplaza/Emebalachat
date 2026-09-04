"""System Tray integration for Emebalachat.

Provides a persistent Windows notification area icon with dynamic status coloring,
comprehensive context menu for mode toggling, source/target language selection (33+ languages),
auto-send mode toggling, sound feedback toggling, badge visibility control, and graceful shutdown.
"""

from __future__ import annotations

import logging
import threading
from typing import Any, Callable, List, Optional

from PIL import Image, ImageDraw
import pystray

logger = logging.getLogger(__name__)

# Fallback languages if config is unavailable
DEFAULT_LANGUAGES: List[str] = [
    "English",
    "Korean",
    "Vietnamese",
    "Chinese Simplified",
    "Chinese Traditional",
    "Japanese",
    "Spanish",
    "French",
    "German",
    "Russian",
    "Thai",
    "Arabic",
    "Portuguese",
    "Italian",
    "Indonesian",
    "Malay",
    "Filipino",
    "Hindi",
    "Bengali",
    "Turkish",
    "Polish",
    "Dutch",
    "Ukrainian",
    "Persian",
    "Urdu",
    "Hebrew",
    "Czech",
    "Hungarian",
    "Swedish",
    "Greek",
    "Romanian",
    "Danish",
    "Finnish",
    "Norwegian",
    "Burmese",
    "Khmer",
    "Lao",
]

try:
    from ..config import SOURCE_LANGUAGES, SUPPORTED_LANGUAGES
except ImportError:
    SUPPORTED_LANGUAGES = DEFAULT_LANGUAGES
    SOURCE_LANGUAGES = ["Auto Detect"] + DEFAULT_LANGUAGES


def generate_icon_image(status: str = "active", size: int = 64) -> Image.Image:
    """Generate a high-contrast tray icon image dynamically using Pillow.

    Args:
        status: Current state ('active', 'translating', or 'disabled').
        size: Icon square dimensions in pixels.

    Returns:
        Pillow RGBA Image object.
    """
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Color palette based on status
    if status == "active":
        bg_color = (16, 185, 129, 255)  # Emerald green (#10B981)
        ring_color = (5, 150, 105, 255)
    elif status == "translating":
        bg_color = (245, 158, 11, 255)  # Amber (#F59E0B)
        ring_color = (217, 119, 6, 255)
    else:
        bg_color = (107, 114, 128, 255)  # Gray (#6B7280)
        ring_color = (75, 85, 99, 255)

    # Draw outer boundary circle
    padding = 4
    draw.ellipse(
        [padding, padding, size - padding, size - padding],
        fill=bg_color,
        outline=ring_color,
        width=2,
    )

    # Draw centered, font-independent geometric letter 'E' for Emebala
    white = (255, 255, 255, 255)
    # Vertical backbone
    draw.rectangle([20, 18, 27, 46], fill=white)
    # Top horizontal arm
    draw.rectangle([27, 18, 44, 24], fill=white)
    # Middle horizontal arm
    draw.rectangle([27, 29, 41, 35], fill=white)
    # Bottom horizontal arm
    draw.rectangle([27, 40, 44, 46], fill=white)

    return image


class TrayManager:
    """Manages the Windows system tray icon and interactive menu."""

    def __init__(
        self,
        initial_enabled: bool = True,
        source_language: str = "Auto Detect",
        target_language: str = "English",
        auto_send: bool = True,
        sound_enabled: bool = True,
        badge_visible: bool = True,
        on_toggle_mode: Optional[Callable[[bool], None]] = None,
        on_toggle_auto_send: Optional[Callable[[bool], None]] = None,
        on_select_source_lang: Optional[Callable[[str], None]] = None,
        on_select_target_lang: Optional[Callable[[str], None]] = None,
        on_swap_languages: Optional[Callable[[], None]] = None,
        on_toggle_sound: Optional[Callable[[bool], None]] = None,
        on_toggle_badge: Optional[Callable[[bool], None]] = None,
        on_show_hotkeys: Optional[Callable[[], None]] = None,
        on_exit: Optional[Callable[[], None]] = None,
    ) -> None:
        """Initialize the TrayManager.

        Args:
            initial_enabled: Initial translation mode state.
            source_language: Initial source language.
            target_language: Initial target language.
            auto_send: Whether auto-send (Enter on paste) is enabled.
            sound_enabled: Whether audio feedback is enabled.
            badge_visible: Whether floating badge overlay is visible.
            on_toggle_mode: Callback invoked when translation mode is toggled.
            on_toggle_auto_send: Callback invoked when auto-send mode is toggled.
            on_select_source_lang: Callback invoked when source language changes.
            on_select_target_lang: Callback invoked when target language changes.
            on_swap_languages: Callback invoked when swapping languages.
            on_toggle_sound: Callback invoked when sound feedback is toggled.
            on_toggle_badge: Callback invoked when badge visibility is toggled.
            on_show_hotkeys: Callback invoked when hotkeys cheat sheet is requested.
            on_exit: Callback invoked when application exit is requested.
        """
        self._lock = threading.Lock()
        self.is_enabled: bool = initial_enabled
        self.source_language: str = source_language
        self.target_language: str = target_language
        self.auto_send: bool = auto_send
        self.sound_enabled: bool = sound_enabled
        self.badge_visible: bool = badge_visible

        # Registered callbacks
        self.on_toggle_mode = on_toggle_mode
        self.on_toggle_auto_send = on_toggle_auto_send
        self.on_select_source_lang = on_select_source_lang
        self.on_select_target_lang = on_select_target_lang
        self.on_swap_languages = on_swap_languages
        self.on_toggle_sound = on_toggle_sound
        self.on_toggle_badge = on_toggle_badge
        self.on_show_hotkeys = on_show_hotkeys
        self.on_exit = on_exit

        self.icon: Optional[pystray.Icon] = None
        self._setup_tray()

    def _setup_tray(self) -> None:
        """Construct the pystray Icon instance and menu structure."""
        image = generate_icon_image(status="active" if self.is_enabled else "disabled")
        menu = self._build_menu()
        self.icon = pystray.Icon(
            name="Emebalachat",
            icon=image,
            title=self._get_title(),
            menu=menu,
        )

    def _get_title(self) -> str:
        """Return tooltip title reflecting current operational state."""
        state_str = "ON" if self.is_enabled else "OFF"
        return f"Emebalachat ({state_str}) [{self.source_language} ➔ {self.target_language}]"

    def _build_menu(self) -> pystray.Menu:
        """Construct the full interactive context menu."""
        # 1. Source Language Submenu
        source_menu_items = []
        for lang in SOURCE_LANGUAGES:
            source_menu_items.append(
                pystray.MenuItem(
                    text=lang,
                    action=self._handle_select_source_lang(lang),
                    checked=lambda item, l=lang: self.source_language == l,
                    radio=True,
                )
            )

        # 2. Target Language Submenu
        target_menu_items = []
        for lang in SUPPORTED_LANGUAGES:
            target_menu_items.append(
                pystray.MenuItem(
                    text=lang,
                    action=self._handle_select_target_lang(lang),
                    checked=lambda item, l=lang: self.target_language == l,
                    radio=True,
                )
            )

        menu_items = [
            # Toggle Translation Mode (F9)
            pystray.MenuItem(
                text="Translation Mode (F9)",
                action=self._handle_toggle_mode,
                checked=lambda item: self.is_enabled,
            ),
            pystray.Menu.SEPARATOR,
            # Source Language Submenu
            pystray.MenuItem(
                text="Source Language",
                action=pystray.Menu(*source_menu_items),
            ),
            # Target Language Submenu
            pystray.MenuItem(
                text="Target Language",
                action=pystray.Menu(*target_menu_items),
            ),
            # Swap Source ⇄ Target
            pystray.MenuItem(
                text=self._get_swap_text,
                action=self._handle_swap_languages,
                enabled=lambda item: self.source_language != "Auto Detect",
            ),
            pystray.Menu.SEPARATOR,
            # Auto-Send Mode Toggle (Ctrl+Shift+Enter)
            pystray.MenuItem(
                text="Auto-Send (Enter on paste)",
                action=self._handle_toggle_auto_send,
                checked=lambda item: self.auto_send,
            ),
            # Sound Feedback Toggle
            pystray.MenuItem(
                text="Sound Feedback",
                action=self._handle_toggle_sound,
                checked=lambda item: self.sound_enabled,
            ),
            # Show Floating Badge Toggle
            pystray.MenuItem(
                text="Show Floating Badge",
                action=self._handle_toggle_badge,
                checked=lambda item: self.badge_visible,
            ),
            pystray.Menu.SEPARATOR,
            # Hotkeys Cheat Sheet
            pystray.MenuItem(
                text="Hotkeys Cheat Sheet",
                action=self._handle_show_hotkeys,
            ),
            pystray.Menu.SEPARATOR,
            # Exit Application
            pystray.MenuItem(
                text="Exit Emebalachat",
                action=self._handle_exit,
            ),
        ]

        return pystray.Menu(*menu_items)

    def _get_swap_text(self, item: Any) -> str:
        """Return dynamic text for swap action."""
        if self.source_language == "Auto Detect":
            return "Swap Languages (Disabled for Auto)"
        return f"Swap: {self.source_language} ⇄ {self.target_language}"

    # -------------------------------------------------------------------------
    # Menu Action Handlers
    # -------------------------------------------------------------------------

    def _handle_toggle_mode(self, icon: pystray.Icon, item: Any) -> None:
        """Handle user toggling translation mode."""
        with self._lock:
            self.is_enabled = not self.is_enabled
            new_state = self.is_enabled

        self._refresh_icon()
        if self.on_toggle_mode:
            try:
                self.on_toggle_mode(new_state)
            except Exception as e:
                logger.error("Error in on_toggle_mode callback: %s", e)

    def _handle_toggle_auto_send(self, icon: pystray.Icon, item: Any) -> None:
        """Handle user toggling auto send mode."""
        with self._lock:
            self.auto_send = not self.auto_send
            new_state = self.auto_send

        self._refresh_menu()
        if self.on_toggle_auto_send:
            try:
                self.on_toggle_auto_send(new_state)
            except Exception as e:
                logger.error("Error in on_toggle_auto_send callback: %s", e)

    def _handle_select_source_lang(self, lang: str) -> Callable[[pystray.Icon, Any], None]:
        """Factory for source language selection callback."""
        def handler(icon: pystray.Icon, item: Any) -> None:
            with self._lock:
                self.source_language = lang

            self._refresh_icon()
            if self.on_select_source_lang:
                try:
                    self.on_select_source_lang(lang)
                except Exception as e:
                    logger.error("Error in on_select_source_lang callback: %s", e)
        return handler

    def _handle_select_target_lang(self, lang: str) -> Callable[[pystray.Icon, Any], None]:
        """Factory for target language selection callback."""
        def handler(icon: pystray.Icon, item: Any) -> None:
            with self._lock:
                self.target_language = lang

            self._refresh_icon()
            if self.on_select_target_lang:
                try:
                    self.on_select_target_lang(lang)
                except Exception as e:
                    logger.error("Error in on_select_target_lang callback: %s", e)
        return handler

    def _handle_swap_languages(self, icon: pystray.Icon, item: Any) -> None:
        """Handle swapping source and target languages."""
        with self._lock:
            if self.source_language == "Auto Detect":
                return
            src, tgt = self.source_language, self.target_language
            self.source_language, self.target_language = tgt, src

        self._refresh_icon()
        if self.on_swap_languages:
            try:
                self.on_swap_languages()
            except Exception as e:
                logger.error("Error in on_swap_languages callback: %s", e)

    def _handle_toggle_sound(self, icon: pystray.Icon, item: Any) -> None:
        """Handle user toggling sound feedback."""
        with self._lock:
            self.sound_enabled = not self.sound_enabled
            new_val = self.sound_enabled

        self._refresh_menu()
        if self.on_toggle_sound:
            try:
                self.on_toggle_sound(new_val)
            except Exception as e:
                logger.error("Error in on_toggle_sound callback: %s", e)

    def _handle_toggle_badge(self, icon: pystray.Icon, item: Any) -> None:
        """Handle user toggling floating badge visibility."""
        with self._lock:
            self.badge_visible = not self.badge_visible
            new_val = self.badge_visible

        self._refresh_menu()
        if self.on_toggle_badge:
            try:
                self.on_toggle_badge(new_val)
            except Exception as e:
                logger.error("Error in on_toggle_badge callback: %s", e)

    def show_hotkeys_cheat_sheet(self) -> None:
        """Display the Hotkeys Cheat Sheet dialog with the 3 main hotkeys."""
        title = "Emebalachat - Hotkeys Cheat Sheet"
        message = (
            "Emebalachat Hotkeys Cheat Sheet\n\n"
            "• F9: Toggle Translation Mode ON/OFF\n"
            "• Ctrl+F9: Cycle Target Language (37 languages)\n"
            "• Enter: Translate & Review / Shift+Enter: Translate & Send\n\n"
            "(Ctrl+Shift+Enter: Toggle Auto-Send default)"
        )

        def _display():
            import sys
            if sys.platform == "win32":
                try:
                    import ctypes
                    MB_OK = 0x00000000
                    MB_ICONINFORMATION = 0x00000040
                    MB_TOPMOST = 0x00040000
                    MB_SETFOREGROUND = 0x00010000
                    ctypes.windll.user32.MessageBoxW(
                        None,
                        message,
                        title,
                        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND,
                    )
                    return
                except Exception as e:
                    logger.debug("Win32 MessageBoxW failed: %s", e)

            try:
                import tkinter.messagebox as mb
                mb.showinfo(title, message)
            except Exception as e:
                logger.debug("Tkinter messagebox failed: %s", e)

        thread = threading.Thread(
            target=_display, daemon=True, name="Emebalachat-CheatSheet"
        )
        thread.start()

    def _handle_show_hotkeys(self, icon: Optional[pystray.Icon] = None, item: Any = None) -> None:
        """Handle click on Hotkeys Cheat Sheet menu item."""
        if self.on_show_hotkeys:
            try:
                self.on_show_hotkeys()
                return
            except Exception as e:
                logger.debug("Error in on_show_hotkeys callback: %s", e)
        self.show_hotkeys_cheat_sheet()

    def _handle_exit(self, icon: pystray.Icon, item: Any) -> None:
        """Handle application exit request from context menu."""
        logger.info("Exit requested from system tray menu.")
        if self.on_exit:
            try:
                self.on_exit()
            except Exception as e:
                logger.error("Error in on_exit callback: %s", e)
        self.stop()

    # -------------------------------------------------------------------------
    # Visual and State Synchronization
    # -------------------------------------------------------------------------

    def _refresh_icon(self) -> None:
        """Update tray icon image, tooltip title, and context menu."""
        if not self.icon:
            return
        try:
            status = "active" if self.is_enabled else "disabled"
            self.icon.icon = generate_icon_image(status=status)
            self.icon.title = self._get_title()
            self.icon.update_menu()
        except Exception as e:
            logger.debug("Failed to refresh tray icon: %s", e)

    def _refresh_menu(self) -> None:
        """Update menu checkboxes and items without altering the icon image."""
        if not self.icon:
            return
        try:
            self.icon.update_menu()
        except Exception as e:
            logger.debug("Failed to refresh tray menu: %s", e)

    def update_state(
        self,
        is_enabled: Optional[bool] = None,
        source_language: Optional[str] = None,
        target_language: Optional[str] = None,
        auto_send: Optional[bool] = None,
        sound_enabled: Optional[bool] = None,
        badge_visible: Optional[bool] = None,
        status: Optional[str] = None,
    ) -> None:
        """Thread-safe update of tray state from external orchestrator.

        Args:
            is_enabled: Optional new translation mode state.
            source_language: Optional new source language.
            target_language: Optional new target language.
            auto_send: Optional new auto-send mode state.
            sound_enabled: Optional new sound enabled state.
            badge_visible: Optional new badge visibility state.
            status: Optional status string ('active', 'translating', 'disabled').
        """
        with self._lock:
            if is_enabled is not None:
                self.is_enabled = is_enabled
            if source_language is not None:
                self.source_language = source_language
            if target_language is not None:
                self.target_language = target_language
            if auto_send is not None:
                self.auto_send = auto_send
            if sound_enabled is not None:
                self.sound_enabled = sound_enabled
            if badge_visible is not None:
                self.badge_visible = badge_visible

        if not self.icon:
            return

        try:
            if status is None:
                status = "active" if self.is_enabled else "disabled"
            self.icon.icon = generate_icon_image(status=status)
            self.icon.title = self._get_title()
            self.icon.update_menu()
        except Exception as e:
            logger.debug("Failed to update tray state: %s", e)

    # -------------------------------------------------------------------------
    # Lifecycle API
    # -------------------------------------------------------------------------

    def start(self) -> None:
        """Start the system tray icon detached on a background thread."""
        if not self.icon:
            self._setup_tray()
        logger.info("Starting system tray icon detached...")
        self.icon.run_detached()

    def stop(self) -> None:
        """Stop and remove the system tray icon."""
        if self.icon:
            try:
                self.icon.stop()
            except Exception as e:
                logger.debug("Error stopping tray icon: %s", e)
            self.icon = None
        logger.info("System tray icon stopped.")
