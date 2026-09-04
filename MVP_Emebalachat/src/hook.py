"""Global Windows low-level keyboard hook (WH_KEYBOARD_LL) for Emebalachat.

Intercepts Enter key events in active applications, analyzes typing context,
and executes asynchronous multi-language translation using Hy-MT2 models with
safe clipboard swapping, synthetic keystroke injection, and responsive audio cues.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import logging
import re
import threading
import time
from typing import Any, Callable, List, Optional

from .win32_input import (
    HOOKPROC,
    KBDLLHOOKSTRUCT,
    SYNTHETIC_MARKER,
    VK_C,
    VK_CONTROL,
    VK_F9,
    VK_HOME,
    VK_LCONTROL,
    VK_LMENU,
    VK_LSHIFT,
    VK_MENU,
    VK_RCONTROL,
    VK_RETURN,
    VK_RIGHT,
    VK_RMENU,
    VK_RSHIFT,
    VK_SHIFT,
    VK_V,
    WH_KEYBOARD_LL,
    WM_KEYDOWN,
    WM_KEYUP,
    WM_QUIT,
    WM_SYSKEYDOWN,
    WM_SYSKEYUP,
    ClipboardBackup,
    backup_clipboard,
    copy_selection,
    flush_ime,
    get_clipboard_text,
    kernel32,
    paste_text,
    restore_clipboard,
    select_current_line,
    send_enter,
    send_key_down,
    send_key_press,
    send_key_up,
    set_clipboard_text,
    unselect,
    user32,
)

logger = logging.getLogger(__name__)

# Attempt to import optional sound and config utilities gracefully
try:
    from .sound import (
        play_lang_change,
        play_mode_change,
        play_toggle_off,
        play_toggle_on,
    )
except ImportError:
    def play_toggle_on(*args: Any, **kwargs: Any) -> None:
        pass

    def play_toggle_off(*args: Any, **kwargs: Any) -> None:
        pass

    def play_lang_change(*args: Any, **kwargs: Any) -> None:
        pass

    def play_mode_change(*args: Any, **kwargs: Any) -> None:
        pass

try:
    from .config import (
        SUPPORTED_LANGUAGES,
        contains_korean,
        should_translate as _config_should_translate,
    )
except ImportError:
    SUPPORTED_LANGUAGES = [
        "English",
        "Japanese",
        "Chinese Simplified",
        "Chinese Traditional",
        "Spanish",
        "French",
        "German",
        "Russian",
        "Korean",
    ]

    _KOREAN_FALLBACK_REGEX = re.compile(r"[\u3131-\u318E\uAC00-\uD7A3]")

    def contains_korean(text: str) -> bool:
        if not text:
            return False
        return bool(_KOREAN_FALLBACK_REGEX.search(text))

    _config_should_translate = None


# Regex patterns for smart bypass validation across multiple languages
_URL_REGEX = re.compile(
    r"^(?:https?://|www\.|ftp://)\S+$",
    re.IGNORECASE,
)
_SYMBOLS_ONLY_REGEX = re.compile(
    r"^[\s0-9!@#$%^&*()_+\-=\[\]{};':\",.<>/?\\|`~₩€£¥§±×÷°•…—–«»“”‘’]+$"
)
_KOREAN_CHAR_REGEX = re.compile(r"[\u3131-\u318E\uAC00-\uD7A3]")
_JAPANESE_KANA_REGEX = re.compile(r"[\u3040-\u309F\u30A0-\u30FF]")
_CJK_IDEOGRAPH_REGEX = re.compile(r"[\u4E00-\u9FFF]")
_CYRILLIC_REGEX = re.compile(r"[\u0400-\u04FF]")
_ARABIC_REGEX = re.compile(r"[\u0600-\u06FF]")
_VIETNAMESE_REGEX = re.compile(
    r"[àáảãạăằắẳẵặâầấẩẫậèéẻẽẹêềếểễệìíỉĩịòóỏõọôồốổỗộơờớởỡợùúủũụưừứửữựỳýỷỹỵđ]",
    re.IGNORECASE,
)


def default_should_translate(
    text: Optional[str],
    source_language: Optional[str] = None,
    target_language: Optional[str] = "English",
) -> bool:
    """Evaluate whether text needs translation when config does not supply a custom rule.

    Handles i18n support across all 33+ Hy-MT2 languages including Vietnamese,
    Korean, Japanese, Chinese, Russian, Spanish, and English.

    Bypasses translation if:
    - text is empty or blank
    - text is purely a URL
    - text consists only of punctuation, numbers, symbols, or emojis
    - text is already determined to be in the target language

    Args:
        text: Copied input text.
        source_language: Expected or detected source language name.
        target_language: Desired target translation language name.

    Returns:
        True if translation should proceed, False otherwise.
    """
    if not text:
        return False

    clean_text = text.strip()
    if not clean_text:
        return False

    # Skip URLs
    if _URL_REGEX.match(clean_text):
        return False

    # Skip pure numbers, symbols, spaces, or emojis
    if _SYMBOLS_ONLY_REGEX.match(clean_text):
        return False

    # Must contain at least one letter/ideograph
    has_letters = any(c.isalpha() for c in clean_text)
    if not has_letters:
        return False

    target = (target_language or "English").strip().lower()
    source = (source_language or "").strip().lower() if source_language else None

    # If source and target are identical, bypass
    if source and target and source == target:
        return False

    # Korean detection
    if _KOREAN_CHAR_REGEX.search(clean_text):
        return target not in ("korean", "ko")

    # Japanese Kana detection
    if _JAPANESE_KANA_REGEX.search(clean_text):
        return target not in ("japanese", "ja")

    # Vietnamese special diacritics detection
    if _VIETNAMESE_REGEX.search(clean_text):
        return target not in ("vietnamese", "vi")

    # Cyrillic detection
    if _CYRILLIC_REGEX.search(clean_text):
        return target not in ("russian", "ru", "ukrainian", "uk", "belarusian", "be")

    # Arabic detection
    if _ARABIC_REGEX.search(clean_text):
        return target not in ("arabic", "ar")

    # Chinese Hanzi detection (without Japanese Kana or Korean Hangul)
    if _CJK_IDEOGRAPH_REGEX.search(clean_text):
        return target not in (
            "chinese",
            "chinese simplified",
            "chinese traditional",
            "zh",
            "zh-cn",
            "zh-tw",
        )

    # Pure ASCII English text detection
    is_ascii_letters_only = all(
        ord(c) < 128 for c in clean_text if c.isalpha()
    )
    if is_ascii_letters_only:
        # If target is English, bypass translation
        if target in ("english", "en"):
            return False
        # If target is another language (e.g. Korean, Spanish, Japanese), translate
        return True

    # General fallback: if text has alphabetical/ideographic characters and target differs
    return True


class GlobalInputHook:
    """Low-level Windows keyboard hook managing smart translation triggers."""

    def __init__(
        self,
        engine: Optional[Any] = None,
        config: Optional[Any] = None,
        sound_player: Optional[Any] = None,
        target_languages: Optional[List[str]] = None,
        on_status_change: Optional[Callable[[str], Any]] = None,
        on_language_change: Optional[Callable[[str], Any]] = None,
        on_mode_change: Optional[Callable[[bool], Any]] = None,
        on_translation: Optional[Callable[[str, str], Any]] = None,
    ) -> None:
        """Initialize the GlobalInputHook.

        Args:
            engine: TranslationEngine or compatible translation interface.
            config: Config instance managing persistent settings.
            sound_player: SoundPlayer or sound effect handler.
            target_languages: List of language names to cycle through via Ctrl+F9.
            on_status_change: Callback invoked on status updates ('active', 'translating', etc.).
            on_language_change: Callback invoked when target language changes.
            on_mode_change: Callback invoked when auto_send mode toggles.
            on_translation: Callback invoked after successful translation (original, translated).
        """
        self.config = config
        self.engine = engine
        self.sound_player = sound_player

        # Callbacks
        self._on_status_change = on_status_change
        self._on_language_change = on_language_change
        self._on_mode_change = on_mode_change
        self._on_translation = on_translation

        # Language cycle list
        if target_languages:
            self.target_languages: List[str] = list(target_languages)
        elif self.config and hasattr(self.config, "target_languages") and self.config.target_languages:
            self.target_languages = list(self.config.target_languages)
        else:
            self.target_languages = list(SUPPORTED_LANGUAGES)

        # Operational state
        self._is_active: bool = True
        self._auto_send: bool = False
        self._target_language: str = "English"
        self._source_language: Optional[str] = None

        self._sync_from_config()

        # Threading and Hook internals
        self._h_hook: Optional[int] = None
        self._thread: Optional[threading.Thread] = None
        self._thread_id: int = 0
        self._ready_event: threading.Event = threading.Event()
        self._running: bool = False

        # Persistent ctypes callback reference to prevent garbage collection crash
        self._hook_proc: HOOKPROC = HOOKPROC(self._low_level_keyboard_proc)

        # Modifier key state tracking
        self._ctrl_down: bool = False
        self._shift_down: bool = False
        self._alt_down: bool = False
        self._suppressing_enter: bool = False

        # Concurrency management
        self._translation_lock: threading.Lock = threading.Lock()
        self._executor: ThreadPoolExecutor = ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="Emebalachat-Worker"
        )

    def _sync_from_config(self) -> None:
        """Synchronize operational parameters from self.config if present."""
        if self.config is not None:
            if hasattr(self.config, "auto_send"):
                self._auto_send = bool(self.config.auto_send)
            if hasattr(self.config, "target_language"):
                self._target_language = str(self.config.target_language)
            if hasattr(self.config, "source_language"):
                self._source_language = getattr(self.config, "source_language", None)

    @property
    def is_active(self) -> bool:
        """Whether translation interception mode is currently active."""
        return self._is_active

    @is_active.setter
    def is_active(self, value: bool) -> None:
        self._is_active = bool(value)

    @property
    def auto_send(self) -> bool:
        """Whether auto-send (immediate Enter after paste) is enabled."""
        return self._auto_send

    @auto_send.setter
    def auto_send(self, value: bool) -> None:
        self._auto_send = bool(value)
        if self.config is not None and hasattr(self.config, "auto_send"):
            self.config.auto_send = self._auto_send
            if hasattr(self.config, "save"):
                try:
                    self.config.save()
                except Exception as e:
                    logger.debug("Failed saving config auto_send: %s", e)

    @property
    def target_language(self) -> str:
        """Currently active target translation language."""
        if self.config is not None and hasattr(self.config, "target_language"):
            return str(self.config.target_language)
        return self._target_language

    @target_language.setter
    def target_language(self, value: str) -> None:
        self._target_language = str(value)
        if self.config is not None and hasattr(self.config, "target_language"):
            self.config.target_language = self._target_language
            if hasattr(self.config, "save"):
                try:
                    self.config.save()
                except Exception as e:
                    logger.debug("Failed saving config target_language: %s", e)

    @property
    def source_language(self) -> Optional[str]:
        """Currently configured source language, or None for auto-detection."""
        if self.config is not None and hasattr(self.config, "source_language"):
            return getattr(self.config, "source_language", None)
        return self._source_language

    @source_language.setter
    def source_language(self, value: Optional[str]) -> None:
        self._source_language = value
        if self.config is not None and hasattr(self.config, "source_language"):
            setattr(self.config, "source_language", value)
            if hasattr(self.config, "save"):
                try:
                    self.config.save()
                except Exception as e:
                    logger.debug("Failed saving config source_language: %s", e)

    def _notify_status(self, status: str, **kwargs: Any) -> None:
        """Notify status change callback if registered."""
        if self._on_status_change:
            try:
                self._on_status_change(status, **kwargs)
            except TypeError:
                try:
                    self._on_status_change(status)
                except Exception as e:
                    logger.debug("Error in on_status_change callback: %s", e)
            except Exception as e:
                logger.debug("Error in on_status_change callback: %s", e)

    def toggle_translation_mode(self) -> bool:
        """Toggle translation mode ON or OFF.

        Plays audio cues and notifies registered callbacks.

        Returns:
            The new active state (True = active, False = inactive).
        """
        self._is_active = not self._is_active
        logger.info("Translation mode toggled: %s", "ACTIVE" if self._is_active else "INACTIVE")

        if self._is_active:
            play_toggle_on(config=self.config)
            self._notify_status("active")
        else:
            play_toggle_off(config=self.config)
            self._notify_status("inactive")

        return self._is_active

    def cycle_target_language(self) -> str:
        """Cycle to the next supported target language.

        Plays language change sound and notifies registered callbacks.

        Returns:
            The newly selected target language name.
        """
        if not self.target_languages:
            return self.target_language

        current = self.target_language
        try:
            current_index = self.target_languages.index(current)
            next_index = (current_index + 1) % len(self.target_languages)
        except ValueError:
            next_index = 0

        new_lang = self.target_languages[next_index]
        self.target_language = new_lang
        logger.info("Target language cycled to: %s", new_lang)

        play_lang_change(config=self.config)

        if self._on_language_change:
            try:
                self._on_language_change(new_lang)
            except Exception as e:
                logger.debug("Error in on_language_change callback: %s", e)

        return new_lang

    def toggle_auto_send(self) -> bool:
        """Toggle auto-send mode (immediate Enter vs replacement only).

        Plays mode change sound and notifies registered callbacks.

        Returns:
            The new auto_send state (True = immediate send, False = replace only).
        """
        self.auto_send = not self.auto_send
        logger.info("Auto-send mode toggled: %s", "ON" if self.auto_send else "OFF")

        play_mode_change(config=self.config)

        if self._on_mode_change:
            try:
                self._on_mode_change(self.auto_send)
            except Exception as e:
                logger.debug("Error in on_mode_change callback: %s", e)

        return self.auto_send

    def should_translate(
        self,
        text: Optional[str],
        source_language: Optional[str] = None,
        target_language: Optional[str] = None,
    ) -> bool:
        """Determine whether the specified text requires translation.

        Respects config.should_translate(text, source_lang, target_lang) if available,
        falling back to the comprehensive multi-language default_should_translate rule.

        Args:
            text: Input text string.
            source_language: Optional source language override.
            target_language: Optional target language override.

        Returns:
            True if text should be translated, False otherwise.
        """
        if not text or not text.strip():
            return False

        clean_text = text.strip()
        target = target_language or self.target_language
        source = source_language if source_language is not None else self.source_language

        # 1. Delegate to config.should_translate if defined on instance
        if self.config is not None and hasattr(self.config, "should_translate"):
            try:
                return bool(self.config.should_translate(clean_text, source, target))
            except TypeError:
                try:
                    return bool(self.config.should_translate(clean_text, target))
                except Exception as e:
                    logger.debug("config.should_translate fallback error: %s", e)
            except Exception as e:
                logger.debug("config.should_translate execution error: %s", e)

        # 2. Delegate to module-level should_translate from src.config if available
        if _config_should_translate is not None:
            try:
                return bool(
                    _config_should_translate(
                        clean_text,
                        source_lang=source or "Auto Detect",
                        target_lang=target or "English",
                    )
                )
            except Exception as e:
                logger.debug("src.config.should_translate execution error: %s", e)

        # 3. Comprehensive multi-language default bypass
        return default_should_translate(clean_text, source, target)

    def _perform_translation(self, text: str, target_lang: str) -> str:
        """Invoke translation engine safely, extracting the translated string.

        Args:
            text: Text to translate.
            target_lang: Target language name.

        Returns:
            Translated string.
        """
        if self.engine is None:
            # Fallback mock representation if no engine is configured
            return f"[{target_lang}: {text}]"

        # Attempt to call engine.translate
        try:
            result = self.engine.translate(text, target_lang=target_lang)
        except TypeError:
            result = self.engine.translate(text, target_lang)

        if hasattr(result, "text"):
            return str(result.text)
        elif hasattr(result, "translated_text"):
            return str(result.translated_text)
        return str(result)

    def _handle_enter_translation(self, force_send: bool = False) -> None:
        """Background translation pipeline executing on physical Enter intercept.

        Follows the strict multi-language invariant:
        1. flush_ime() + 10ms sleep
        2. Backup clipboard
        3. select_current_line() + 10ms sleep + copy_selection()
        4. Read copied text from clipboard (with 50ms wait)
        5. Check should_translate(text, source_language, target_language):
           - If False: restore clipboard, unselect(), send_enter()
        6. If True:
           - notify UI status="translating"
           - translate via engine.translate(text, target_lang)
           - set clipboard to translated text
           - paste_text()
           - wait 120ms settle delay
           - restore original clipboard
           - if auto_send or force_send is True: send_enter()
           - notify UI status="active"
        """
        if not self._translation_lock.acquire(blocking=False):
            logger.debug("Translation already in progress; forwarding Enter keystroke.")
            send_enter()
            return

        backup: Optional[ClipboardBackup] = None
        try:
            # Step 1: flush IME buffer cleanly (commits ongoing composition)
            flush_ime()
            time.sleep(0.010)

            # Step 2: Backup clipboard
            backup = backup_clipboard()

            # Step 3: Select current line and copy selection
            select_current_line()
            time.sleep(0.010)
            copy_selection()

            # Step 4: Wait for application to process copy and read clipboard
            time.sleep(0.050)
            text = get_clipboard_text(timeout_ms=50.0)

            target_lang = self.target_language
            source_lang = self.source_language

            # Step 5: Smart bypass check
            if not self.should_translate(text, source_lang, target_lang):
                self._notify_status("skipped")
                restore_clipboard(backup)
                unselect()
                send_enter()
                return

            # Step 6: Proceed with translation
            self._notify_status("translating", original_text=text)

            try:
                translated_text = self._perform_translation(text, target_lang)
            except Exception as e_trans:
                logger.error("Translation engine failed: %s", e_trans, exc_info=True)
                restore_clipboard(backup)
                unselect()
                send_enter()
                self._notify_status("error", error=str(e_trans))
                return

            # Place translated text on clipboard
            set_clipboard_text(translated_text)

            # Paste replacement text
            paste_text()

            # Wait 120ms settle delay so the target application completes paste
            time.sleep(0.120)

            # Restore original clipboard
            restore_clipboard(backup)

            # Send Enter if auto_send is enabled or force_send is requested (Shift+Enter)
            if self.auto_send or force_send:
                shift_was_down = bool(user32.GetAsyncKeyState(VK_SHIFT) & 0x8000)
                if shift_was_down:
                    send_key_up(VK_SHIFT)
                send_enter()
                if shift_was_down:
                    send_key_down(VK_SHIFT)

            # Notify translation callback
            if self._on_translation:
                try:
                    self._on_translation(text, translated_text)
                except Exception as e_cb:
                    logger.debug("Error in on_translation callback: %s", e_cb)

            # Restore active UI status
            self._notify_status("active")

        except Exception as e:
            logger.error("Unexpected error in Enter translation handler: %s", e, exc_info=True)
            if backup is not None:
                restore_clipboard(backup)
            unselect()
            send_enter()
            self._notify_status("active")
        finally:
            self._translation_lock.release()

    def _dispatch_background_translation(self, force_send: bool = False) -> None:
        """Submit the Enter translation pipeline to the background executor."""
        try:
            self._executor.submit(self._handle_enter_translation, force_send)
        except Exception as e:
            logger.error("Failed submitting Enter translation task: %s", e)
            send_enter()

    def _low_level_keyboard_proc(
        self, nCode: int, wParam: int, lParam: int
    ) -> int:
        """Low-level keyboard hook callback procedure.

        Must return immediately without blocking to avoid Windows hook timeouts.
        """
        if nCode < 0:
            return user32.CallNextHookEx(self._h_hook, nCode, wParam, lParam)

        hook_struct = KBDLLHOOKSTRUCT.from_address(lParam)

        # Bypass all synthetic keystrokes injected by our own helpers
        if hook_struct.dwExtraInfo == SYNTHETIC_MARKER:
            return user32.CallNextHookEx(self._h_hook, nCode, wParam, lParam)

        vk_code = hook_struct.vkCode
        is_key_down = wParam in (WM_KEYDOWN, WM_SYSKEYDOWN)
        is_key_up = wParam in (WM_KEYUP, WM_SYSKEYUP)

        # Update modifier states
        if vk_code in (VK_CONTROL, VK_LCONTROL, VK_RCONTROL):
            self._ctrl_down = is_key_down
        elif vk_code in (VK_SHIFT, VK_LSHIFT, VK_RSHIFT):
            self._shift_down = is_key_down
        elif vk_code in (VK_MENU, VK_LMENU, VK_RMENU):
            self._alt_down = is_key_down

        # Query physical modifier state via GetAsyncKeyState for absolute precision
        ctrl_pressed = self._ctrl_down or bool(user32.GetAsyncKeyState(VK_CONTROL) & 0x8000)
        shift_pressed = self._shift_down or bool(user32.GetAsyncKeyState(VK_SHIFT) & 0x8000)
        alt_pressed = self._alt_down or bool(user32.GetAsyncKeyState(VK_MENU) & 0x8000)

        # Hotkey: F9 / Ctrl+F9
        if vk_code == VK_F9:
            if is_key_down:
                if ctrl_pressed and not shift_pressed and not alt_pressed:
                    # Ctrl + F9: Cycle target language
                    self.cycle_target_language()
                    return 1
                elif not ctrl_pressed and not shift_pressed and not alt_pressed:
                    # F9: Toggle translation mode ON/OFF
                    self.toggle_translation_mode()
                    return 1
            elif is_key_up:
                if not alt_pressed:
                    # Suppress matching key-up
                    return 1

        # Hotkey: Enter / Shift+Enter / Ctrl+Shift+Enter
        if vk_code == VK_RETURN:
            if is_key_down:
                if ctrl_pressed and shift_pressed and not alt_pressed:
                    # Ctrl + Shift + Enter: Toggle auto-send mode
                    self.toggle_auto_send()
                    self._suppressing_enter = True
                    return 1
                elif not ctrl_pressed and shift_pressed and not alt_pressed:
                    # Shift + Enter: Direct Translate and Send Immediately (even when auto_send is False)
                    if self.is_active:
                        self._suppressing_enter = True
                        self._dispatch_background_translation(force_send=True)
                        return 1
                elif not ctrl_pressed and not shift_pressed and not alt_pressed:
                    # Physical Enter pressed in normal typing
                    if self.is_active:
                        self._suppressing_enter = True
                        self._dispatch_background_translation(force_send=False)
                        return 1
            elif is_key_up:
                if self._suppressing_enter:
                    self._suppressing_enter = False
                    return 1

        return user32.CallNextHookEx(self._h_hook, nCode, wParam, lParam)

    def _run_message_loop(self) -> None:
        """Dedicated message pump thread running the Win32 hook."""
        self._thread_id = kernel32.GetCurrentThreadId()
        self._h_hook = user32.SetWindowsHookExW(
            WH_KEYBOARD_LL,
            self._hook_proc,
            None,
            0,
        )

        if not self._h_hook:
            err = ctypes.get_last_error()
            logger.error("Failed to install WH_KEYBOARD_LL hook. Error code: %d", err)
            self._running = False
            self._ready_event.set()
            return

        self._running = True
        self._ready_event.set()
        logger.info("Global WH_KEYBOARD_LL hook successfully installed (Thread ID: %d)", self._thread_id)

        msg = wintypes.MSG()
        try:
            while self._running:
                # GetMessageW blocks until a message arrives for this thread
                res = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
                if res <= 0:  # 0 indicates WM_QUIT, -1 indicates error
                    break
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
        finally:
            if self._h_hook:
                user32.UnhookWindowsHookEx(self._h_hook)
                self._h_hook = None
            self._running = False
            logger.info("Global WH_KEYBOARD_LL hook uninstalled.")

    def start(self, timeout: float = 3.0) -> bool:
        """Start the hook message pump thread.

        Args:
            timeout: Maximum seconds to wait for hook installation confirmation.

        Returns:
            True if hook was installed successfully, False otherwise.
        """
        if self._running or (self._thread and self._thread.is_alive()):
            logger.warning("GlobalInputHook is already running.")
            return True

        self._ready_event.clear()
        self._thread = threading.Thread(
            target=self._run_message_loop,
            daemon=True,
            name="Emebalachat-HookLoop",
        )
        self._thread.start()

        if not self._ready_event.wait(timeout=timeout):
            logger.error("Timed out waiting for hook thread initialization.")
            return False

        return self._running

    def stop(self, timeout: float = 2.0) -> None:
        """Stop the hook message pump and clean up resources.

        Args:
            timeout: Maximum seconds to wait for message pump thread to exit.
        """
        self._running = False
        if self._thread_id:
            user32.PostThreadMessageW(self._thread_id, WM_QUIT, 0, 0)

        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=timeout)

        # Fallback unhook if thread was killed abnormally
        if self._h_hook:
            user32.UnhookWindowsHookEx(self._h_hook)
            self._h_hook = None

        self._executor.shutdown(wait=False, cancel_futures=True)

    def is_alive(self) -> bool:
        """Check whether the hook message loop thread is currently active."""
        return self._running and (self._thread is not None and self._thread.is_alive())

    def __enter__(self) -> GlobalInputHook:
        """Context manager support to start hook automatically."""
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        """Context manager exit to stop hook automatically."""
        self.stop()
