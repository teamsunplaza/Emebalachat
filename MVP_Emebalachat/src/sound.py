"""Sound module for Emebalachat.

Provides asynchronous audio feedback using Windows built-in winsound.Beep.
All beeps run on daemon threads and are guarded by configuration settings.
Exception-safe: will not crash even if audio hardware is unavailable.
"""

from __future__ import annotations

import logging
import threading
from typing import Any, Optional

logger = logging.getLogger(__name__)

try:
    import winsound
except ImportError:
    winsound = None
    logger.warning("winsound module is not available on this platform. Sound effects disabled.")

# Optional global config reference
_global_sound_config: Optional[Any] = None


def set_sound_config(config: Any) -> None:
    """Set the module-level configuration instance to guard sound playback.

    Args:
        config: Config instance with a `sound_enabled` attribute.
    """
    global _global_sound_config
    _global_sound_config = config


def is_sound_enabled(config: Optional[Any] = None, sound_enabled: Optional[bool] = None) -> bool:
    """Check if sound playback is currently enabled.

    Evaluation precedence:
    1. Explicit `sound_enabled` parameter (if not None).
    2. Explicit `config.sound_enabled` (if config is provided).
    3. Global config `sound_enabled` (if set).
    4. Default True.

    Args:
        config: Optional Config instance.
        sound_enabled: Optional explicit boolean flag.

    Returns:
        True if sound is enabled, False otherwise.
    """
    if sound_enabled is not None:
        return bool(sound_enabled)
    if config is not None:
        return bool(getattr(config, "sound_enabled", True))
    if _global_sound_config is not None:
        return bool(getattr(_global_sound_config, "sound_enabled", True))
    return True


def _beep_worker(frequency: int, duration_ms: int) -> None:
    """Worker function executed inside daemon thread to invoke winsound.Beep."""
    if winsound is None:
        logger.debug("Sound skipped: winsound not available.")
        return
    try:
        winsound.Beep(int(frequency), int(duration_ms))
    except Exception as e:
        logger.debug("Beep execution failed (%dHz, %dms): %s", frequency, duration_ms, e)


def _play_beep_async(
    frequency: int,
    duration_ms: int,
    config: Optional[Any] = None,
    sound_enabled: Optional[bool] = None,
) -> None:
    """Asynchronously play a beep sound if sound is enabled.

    Args:
        frequency: Beep frequency in Hertz.
        duration_ms: Beep duration in milliseconds.
        config: Optional Config instance to check sound_enabled.
        sound_enabled: Optional explicit boolean flag.
    """
    if not is_sound_enabled(config=config, sound_enabled=sound_enabled):
        return

    try:
        thread = threading.Thread(
            target=_beep_worker,
            args=(frequency, duration_ms),
            daemon=True,
            name=f"Beep-{frequency}Hz-{duration_ms}ms",
        )
        thread.start()
    except Exception as e:
        logger.debug("Failed to spawn sound daemon thread: %s", e)


def play_toggle_on(config: Optional[Any] = None, sound_enabled: Optional[bool] = None) -> None:
    """Play toggle ON sound (1000Hz, 100ms) asynchronously.

    Args:
        config: Optional Config instance.
        sound_enabled: Optional explicit boolean flag.
    """
    _play_beep_async(1000, 100, config=config, sound_enabled=sound_enabled)


def play_toggle_off(config: Optional[Any] = None, sound_enabled: Optional[bool] = None) -> None:
    """Play toggle OFF sound (400Hz, 120ms) asynchronously.

    Args:
        config: Optional Config instance.
        sound_enabled: Optional explicit boolean flag.
    """
    _play_beep_async(400, 120, config=config, sound_enabled=sound_enabled)


def play_lang_change(config: Optional[Any] = None, sound_enabled: Optional[bool] = None) -> None:
    """Play language change sound (800Hz, 80ms) asynchronously.

    Args:
        config: Optional Config instance.
        sound_enabled: Optional explicit boolean flag.
    """
    _play_beep_async(800, 80, config=config, sound_enabled=sound_enabled)


def play_mode_change(config: Optional[Any] = None, sound_enabled: Optional[bool] = None) -> None:
    """Play mode change sound (1200Hz, 80ms) asynchronously.

    Args:
        config: Optional Config instance.
        sound_enabled: Optional explicit boolean flag.
    """
    _play_beep_async(1200, 80, config=config, sound_enabled=sound_enabled)


class SoundPlayer:
    """Helper class to trigger sound effects bound to a specific Config instance."""

    def __init__(self, config: Optional[Any] = None) -> None:
        """Initialize SoundPlayer.

        Args:
            config: Optional Config instance containing sound_enabled setting.
        """
        self.config = config

    def play_toggle_on(self) -> None:
        """Play toggle ON sound."""
        play_toggle_on(config=self.config)

    def play_toggle_off(self) -> None:
        """Play toggle OFF sound."""
        play_toggle_off(config=self.config)

    def play_lang_change(self) -> None:
        """Play language change sound."""
        play_lang_change(config=self.config)

    def play_mode_change(self) -> None:
        """Play mode change sound."""
        play_mode_change(config=self.config)
