"""Floating Status Badge overlay for Emebalachat.

Provides a modern, semi-transparent, draggable pill-shaped status badge
that indicates translation state, source language, and target language.
All updates from background threads are safely dispatched via root.after().
"""

from __future__ import annotations

import logging
import sys
import threading
import tkinter as tk
from enum import Enum
from typing import Callable, Optional

logger = logging.getLogger(__name__)

# Standard language code mappings for visual badge display
LANGUAGE_CODES = {
    "Auto Detect": "Auto",
    "Korean": "KO",
    "English": "EN",
    "Japanese": "JA",
    "Chinese Simplified": "ZH-CN",
    "Chinese Traditional": "ZH-TW",
    "Vietnamese": "VI",
    "Spanish": "ES",
    "French": "FR",
    "German": "DE",
    "Russian": "RU",
    "Portuguese": "PT",
    "Italian": "IT",
    "Thai": "TH",
    "Indonesian": "ID",
    "Arabic": "AR",
    "Hindi": "HI",
    "Turkish": "TR",
    "Dutch": "NL",
    "Polish": "PL",
    "Ukrainian": "UK",
    "Czech": "CS",
    "Swedish": "SV",
    "Greek": "EL",
    "Hungarian": "HU",
    "Romanian": "RO",
    "Danish": "DA",
    "Finnish": "FI",
    "Norwegian": "NO",
    "Malay": "MS",
    "Filipino": "TL",
    "Swahili": "SW",
    "Bengali": "BN",
    "Urdu": "UR",
}


def get_lang_code(name_or_code: Optional[str]) -> str:
    """Normalize language name or code into a short badge display code."""
    if not name_or_code:
        return "Auto"
    # Direct match in standard dictionary
    if name_or_code in LANGUAGE_CODES:
        return LANGUAGE_CODES[name_or_code]
    # Check case-insensitive
    name_lower = name_or_code.strip().lower()
    for k, v in LANGUAGE_CODES.items():
        if k.lower() == name_lower:
            return v
    # If already a short code (e.g. "EN", "KO", "ZH-CN")
    if len(name_or_code) <= 5:
        return name_or_code.upper()
    return name_or_code[:3].upper()


class BadgeState(str, Enum):
    """Enumeration of possible badge states."""
    ACTIVE = "active"
    TRANSLATING = "translating"
    DISABLED = "disabled"
    SKIPPED = "skipped"


class FloatingBadge:
    """Semi-transparent floating status badge window built with Tkinter.

    Features:
    - Borderless window with rounded pill appearance.
    - Always-on-top overlay with 85% normal opacity and 20% idle fading.
    - Click-and-drag movement anywhere on the badge.
    - Dynamic status transitions: Active, Translating, Disabled, Skipped.
    - Thread-safe updates dispatched via Tkinter event loop.
    - Tool window styling on Windows to prevent taskbar clutter.
    """

    WIDTH = 210
    HEIGHT = 38
    CORNER_RADIUS = 18

    COLOR_BG = "#121316"
    COLOR_PILL_BG = "#1e2025"
    COLOR_BORDER_DEFAULT = "#2c3038"

    OPACITY_NORMAL: float = 0.85
    OPACITY_IDLE: float = 0.20
    IDLE_TIMEOUT_MS: int = 5000
    FLASH_DURATION_MS: int = 500

    # State-specific accent and text colors
    PALETTE = {
        BadgeState.ACTIVE: {
            "border": "#10b981",
            "text": "#10b981",
            "dot": "#10b981",
        },
        BadgeState.TRANSLATING: {
            "border": "#f59e0b",
            "text": "#f59e0b",
            "dot": "#f59e0b",
        },
        BadgeState.DISABLED: {
            "border": "#4b5563",
            "text": "#9ca3af",
            "dot": "#6b7280",
        },
        BadgeState.SKIPPED: {
            "border": "#4b5563",
            "text": "#9ca3af",
            "dot": "#6b7280",
        },
    }

    def __init__(
        self,
        root: Optional[tk.Tk] = None,
        source_lang: str = "Korean",
        target_lang: str = "English",
        initial_state: BadgeState = BadgeState.ACTIVE,
        on_toggle_callback: Optional[Callable[[], None]] = None,
    ) -> None:
        """Initialize the FloatingBadge.

        Args:
            root: Optional parent Tk root. If None, a new Tk instance is created.
            source_lang: Initial source language.
            target_lang: Initial target language.
            initial_state: Initial state of the badge.
            on_toggle_callback: Optional callback invoked on double-clicking the badge.
        """
        self._owns_root: bool = root is None
        self.root: tk.Tk = root if root is not None else tk.Tk()
        self.on_toggle_callback: Optional[Callable[[], None]] = on_toggle_callback

        self.source_lang: str = source_lang
        self.target_lang: str = target_lang
        self.state: BadgeState = initial_state
        self._is_visible: bool = True

        # Mouse dragging state variables
        self._drag_offset_x: int = 0
        self._drag_offset_y: int = 0

        # Opacity & idle fading management
        self._current_opacity: float = self.OPACITY_NORMAL
        self._mouse_hovered: bool = False
        self._idle_timer_id: Optional[str] = None
        self._fade_timer_id: Optional[str] = None
        self._flash_timer_id: Optional[str] = None

        self._init_window()
        self._create_widgets()
        self._apply_status(self.state, self.target_lang, self.source_lang)

    def _init_window(self) -> None:
        """Configure Tkinter window attributes, geometry, and Windows toolwindow flags."""
        self.root.title("Emebalachat Status")
        self.root.overrideredirect(True)
        self.root.resizable(False, False)

        # Set always on top
        try:
            self.root.attributes("-topmost", True)
        except Exception as e:
            logger.debug("Failed to set -topmost: %s", e)

        # Set initial normal opacity
        self._set_opacity(self.OPACITY_NORMAL)

        # Background color
        self.root.configure(bg=self.COLOR_BG)

        # Initial screen positioning: top-right corner with 30px margin
        self.root.update_idletasks()
        screen_w = self.root.winfo_screenwidth()
        init_x = max(20, screen_w - self.WIDTH - 30)
        init_y = 30
        self.root.geometry(f"{self.WIDTH}x{self.HEIGHT}+{init_x}+{init_y}")

        # Set Windows WS_EX_TOOLWINDOW style so it doesn't show in Alt+Tab
        self._apply_toolwindow_style()

    def _apply_toolwindow_style(self) -> None:
        """Apply Windows tool-window extended style if running on Windows."""
        if sys.platform != "win32":
            return
        try:
            import ctypes
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            if not hwnd:
                hwnd = self.root.winfo_id()
            GWL_EXSTYLE = -20
            WS_EX_TOOLWINDOW = 0x00000080
            WS_EX_TOPMOST = 0x00000008

            current_style = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            new_style = current_style | WS_EX_TOOLWINDOW | WS_EX_TOPMOST
            ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, new_style)
        except Exception as e:
            logger.debug("Could not apply toolwindow style: %s", e)

    def _create_widgets(self) -> None:
        """Create canvas and graphical elements for the pill badge."""
        self.canvas = tk.Canvas(
            self.root,
            width=self.WIDTH,
            height=self.HEIGHT,
            bg=self.COLOR_BG,
            highlightthickness=0,
            cursor="fleur",
        )
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Bind mouse events for drag, hover, and double click on both canvas and root
        for widget in (self.root, self.canvas):
            widget.bind("<Button-1>", self._on_drag_start)
            widget.bind("<B1-Motion>", self._on_drag_motion)
            widget.bind("<Double-Button-1>", self._on_double_click)
            widget.bind("<Enter>", self._on_mouse_enter)
            widget.bind("<Leave>", self._on_mouse_leave)

    # -------------------------------------------------------------------------
    # Opacity & Idle Fade Control
    # -------------------------------------------------------------------------

    def _set_opacity(self, alpha: float) -> None:
        """Set window opacity safely."""
        self._current_opacity = max(0.0, min(1.0, alpha))
        try:
            self.root.attributes("-alpha", self._current_opacity)
        except tk.TclError:
            pass
        except Exception as e:
            logger.debug("Failed to set -alpha: %s", e)

    def _restore_opacity(self) -> None:
        """Restore opacity immediately to 0.85."""
        self._cancel_fade_step()
        self._set_opacity(self.OPACITY_NORMAL)

    def _cancel_fade_step(self) -> None:
        """Cancel any ongoing fade transition timer."""
        if self._fade_timer_id is not None:
            try:
                self.root.after_cancel(self._fade_timer_id)
            except Exception:
                pass
            self._fade_timer_id = None

    def _cancel_idle_timer(self) -> None:
        """Cancel scheduled idle fade timer."""
        if self._idle_timer_id is not None:
            try:
                self.root.after_cancel(self._idle_timer_id)
            except Exception:
                pass
            self._idle_timer_id = None

    def _schedule_idle_fade(self) -> None:
        """Schedule idle fading after 5 seconds of inactivity."""
        self._cancel_idle_timer()
        if self.state == BadgeState.ACTIVE and not self._mouse_hovered:
            try:
                self._idle_timer_id = self.root.after(
                    self.IDLE_TIMEOUT_MS, self._fade_to_idle
                )
            except Exception:
                pass

    def _fade_to_idle(self) -> None:
        """Fade opacity smoothly to 0.20 when idle."""
        self._idle_timer_id = None
        if self.state != BadgeState.ACTIVE or self._mouse_hovered:
            return

        def _step(current_alpha: float) -> None:
            target_alpha = self.OPACITY_IDLE
            step_size = 0.05
            if current_alpha > target_alpha:
                next_alpha = max(target_alpha, round(current_alpha - step_size, 2))
                self._set_opacity(next_alpha)
                if next_alpha > target_alpha:
                    try:
                        self._fade_timer_id = self.root.after(30, lambda: _step(next_alpha))
                    except Exception:
                        self._fade_timer_id = None
                else:
                    self._fade_timer_id = None

        self._cancel_fade_step()
        _step(self._current_opacity)

    def _draw_pill(self, border_color: str) -> None:
        """Draw rounded pill background and border on the canvas."""
        self.canvas.delete("pill")

        # Dimensions
        x1, y1 = 2, 2
        x2, y2 = self.WIDTH - 2, self.HEIGHT - 2
        r = self.CORNER_RADIUS

        # Construct smooth polygon points for rounded pill
        points = [
            x1 + r, y1,
            x2 - r, y1,
            x2, y1,
            x2, y1 + r,
            x2, y2 - r,
            x2, y2,
            x2 - r, y2,
            x1 + r, y2,
            x1, y2,
            x1, y2 - r,
            x1, y1 + r,
            x1, y1,
        ]

        # Draw filled pill background with accent border
        self.canvas.create_polygon(
            points,
            smooth=True,
            fill=self.COLOR_PILL_BG,
            outline=border_color,
            width=1.5,
            tags="pill",
        )

    def _render_content(self, text: str, text_color: str, dot_color: str) -> None:
        """Render status text and glowing dot indicator."""
        self.canvas.delete("content")

        # Draw status text centered
        self.canvas.create_text(
            self.WIDTH // 2,
            self.HEIGHT // 2,
            text=text,
            fill=text_color,
            font=("Segoe UI", 10, "bold"),
            justify=tk.CENTER,
            tags="content",
        )

    def _apply_status(
        self,
        state: BadgeState | str,
        target_lang: Optional[str] = None,
        source_lang: Optional[str] = None,
    ) -> None:
        """Internal helper to apply visual changes on the main thread."""
        try:
            if isinstance(state, str):
                try:
                    self.state = BadgeState(state.lower())
                except ValueError:
                    self.state = BadgeState.ACTIVE
            else:
                self.state = state

            if target_lang is not None:
                self.target_lang = target_lang
            if source_lang is not None:
                self.source_lang = source_lang

            # Cancel any active flash revert timer when new status arrives
            if self._flash_timer_id is not None:
                try:
                    self.root.after_cancel(self._flash_timer_id)
                except Exception:
                    pass
                self._flash_timer_id = None

            palette = self.PALETTE.get(self.state, self.PALETTE[BadgeState.ACTIVE])

            if self.state == BadgeState.ACTIVE:
                self._restore_opacity()
                self._schedule_idle_fade()
                src_code = get_lang_code(self.source_lang)
                tgt_code = get_lang_code(self.target_lang)
                display_text = f"🟢 TR: {src_code} ➔ {tgt_code}"
            elif self.state == BadgeState.TRANSLATING:
                # Restore opacity to 0.85 immediately when state changes to 'translating'
                self._cancel_idle_timer()
                self._restore_opacity()
                display_text = "🟡 TR: Translating..."
            elif self.state == BadgeState.SKIPPED:
                self._cancel_idle_timer()
                self._restore_opacity()
                display_text = "⚪ TR: Skipped"
            else:
                self._cancel_idle_timer()
                self._restore_opacity()
                display_text = "⚪ TR: OFF"

            self._draw_pill(palette["border"])
            self._render_content(display_text, palette["text"], palette["dot"])
        except tk.TclError:
            # Window was already destroyed
            pass
        except Exception as e:
            logger.error("Error updating badge visuals: %s", e)

    # -------------------------------------------------------------------------
    # Public Thread-Safe API
    # -------------------------------------------------------------------------

    def update_status(
        self,
        state: BadgeState | str,
        target_lang: Optional[str] = None,
        source_lang: Optional[str] = None,
    ) -> None:
        """Thread-safe update of the badge state, target language, and source language.

        Args:
            state: New state ('active', 'translating', 'disabled', or 'skipped').
            target_lang: Optional new target language name.
            source_lang: Optional new source language name.
        """
        if str(state).lower() in ("skipped", "bypass"):
            self.flash_skipped()
            return

        try:
            if threading.current_thread() is threading.main_thread():
                self._apply_status(state, target_lang, source_lang)
            else:
                self.root.after(0, self._apply_status, state, target_lang, source_lang)
        except tk.TclError:
            pass
        except Exception as e:
            logger.debug("Failed to schedule badge update: %s", e)

    def flash_skipped(self) -> None:
        """Briefly show (500ms) '⚪ TR: Skipped' state then revert to previous state."""
        def _do_flash():
            if self._flash_timer_id is not None:
                try:
                    self.root.after_cancel(self._flash_timer_id)
                except Exception:
                    pass
                self._flash_timer_id = None

            previous_state = self.state
            previous_target = self.target_lang
            previous_source = self.source_lang

            palette = self.PALETTE.get(BadgeState.SKIPPED, self.PALETTE[BadgeState.DISABLED])
            self._draw_pill(palette["border"])
            self._render_content("⚪ TR: Skipped", palette["text"], palette["dot"])
            self._restore_opacity()
            self._cancel_idle_timer()

            def _revert():
                self._flash_timer_id = None
                self._apply_status(previous_state, previous_target, previous_source)

            try:
                self._flash_timer_id = self.root.after(self.FLASH_DURATION_MS, _revert)
            except Exception:
                pass

        if threading.current_thread() is threading.main_thread():
            _do_flash()
        else:
            try:
                self.root.after(0, _do_flash)
            except tk.TclError:
                pass

    def show(self) -> None:
        """Thread-safe show badge."""
        def _do_show():
            try:
                self.root.deiconify()
                self._is_visible = True
            except tk.TclError:
                pass

        if threading.current_thread() is threading.main_thread():
            _do_show()
        else:
            self.root.after(0, _do_show)

    def hide(self) -> None:
        """Thread-safe hide badge."""
        def _do_hide():
            try:
                self.root.withdraw()
                self._is_visible = False
            except tk.TclError:
                pass

        if threading.current_thread() is threading.main_thread():
            _do_hide()
        else:
            self.root.after(0, _do_hide)

    def toggle_visibility(self) -> None:
        """Toggle badge visibility between shown and hidden."""
        if self._is_visible:
            self.hide()
        else:
            self.show()

    @property
    def is_visible(self) -> bool:
        """Return True if badge is currently visible."""
        return self._is_visible

    def set_position(self, x: int, y: int) -> None:
        """Set position of badge window."""
        def _do_set():
            try:
                self.root.geometry(f"+{x}+{y}")
            except tk.TclError:
                pass
        self.root.after(0, _do_set)

    def get_position(self) -> tuple[int, int]:
        """Return (x, y) coordinates of the badge window."""
        return self.root.winfo_x(), self.root.winfo_y()

    def destroy(self) -> None:
        """Cleanly destroy badge window and root if owned."""
        self._cancel_idle_timer()
        self._cancel_fade_step()
        if self._flash_timer_id is not None:
            try:
                self.root.after_cancel(self._flash_timer_id)
            except Exception:
                pass
            self._flash_timer_id = None

        try:
            if self._owns_root:
                self.root.quit()
                self.root.destroy()
            else:
                self.canvas.destroy()
        except Exception as e:
            logger.debug("Badge destroy exception: %s", e)

    # -------------------------------------------------------------------------
    # Drag & Event Handlers
    # -------------------------------------------------------------------------

    def _on_drag_start(self, event: tk.Event) -> None:
        """Record initial mouse click offset relative to the badge window."""
        self._drag_offset_x = event.x_root - self.root.winfo_x()
        self._drag_offset_y = event.y_root - self.root.winfo_y()

    def _on_drag_motion(self, event: tk.Event) -> None:
        """Move the badge window smoothly following the mouse cursor."""
        new_x = event.x_root - self._drag_offset_x
        new_y = event.y_root - self._drag_offset_y
        self.root.geometry(f"+{new_x}+{new_y}")

    def _on_double_click(self, event: Optional[tk.Event] = None) -> None:
        """Trigger toggle callback on double click if registered."""
        if self.on_toggle_callback:
            try:
                self.on_toggle_callback()
            except Exception as e:
                logger.error("Error executing double-click toggle callback: %s", e)

    def _on_mouse_enter(self, event: Optional[tk.Event] = None) -> None:
        """Restore full opacity (0.85) immediately on mouse hover."""
        self._mouse_hovered = True
        self._cancel_idle_timer()
        self._restore_opacity()

    def _on_mouse_leave(self, event: Optional[tk.Event] = None) -> None:
        """Resume idle fade timer when mouse leaves badge window."""
        self._mouse_hovered = False
        if self.state == BadgeState.ACTIVE:
            self._schedule_idle_fade()
