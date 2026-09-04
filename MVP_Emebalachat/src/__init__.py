"""Emebalachat core package.

Provides configuration management, multi-language detection, asynchronous audio feedback,
and translation engine supporting 33+ languages.
"""

from .config import (
    Config,
    DEFAULT_CONFIG,
    DEFAULT_CONFIG_PATH,
    DEFAULT_MODEL_PATH,
    LANGUAGE_CODES,
    SOURCE_LANGUAGES,
    SUPPORTED_LANGUAGES,
    build_prompt,
    contains_korean,
    detect_language,
    should_translate,
)
from .sound import (
    SoundPlayer,
    is_sound_enabled,
    play_lang_change,
    play_mode_change,
    play_toggle_off,
    play_toggle_on,
    set_sound_config,
)
from .engine import TranslationEngine

__all__ = [
    "Config",
    "DEFAULT_CONFIG",
    "DEFAULT_CONFIG_PATH",
    "DEFAULT_MODEL_PATH",
    "SUPPORTED_LANGUAGES",
    "SOURCE_LANGUAGES",
    "LANGUAGE_CODES",
    "build_prompt",
    "contains_korean",
    "detect_language",
    "should_translate",
    "SoundPlayer",
    "is_sound_enabled",
    "set_sound_config",
    "play_toggle_on",
    "play_toggle_off",
    "play_lang_change",
    "play_mode_change",
    "TranslationEngine",
]
