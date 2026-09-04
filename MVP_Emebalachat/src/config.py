"""Configuration module for Emebalachat.

Handles loading, saving, and managing configuration settings backed by config.json.
Provides helper utilities for 33+ languages supported by Hy-MT2-1.8B, multi-script
language detection, translation necessity checks, and Korean character detection.
"""

from __future__ import annotations

import json
import logging
import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

# Default project root is the parent directory of 'src'
DEFAULT_PROJECT_ROOT: Path = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG_PATH: Path = DEFAULT_PROJECT_ROOT / "config.json"

DEFAULT_MODEL_PATH: str = r"models/Hy-MT2-1.8B-Q8_0.gguf"

# 33+ languages supported by Tencent Hy-MT2-1.8B
SUPPORTED_LANGUAGES: List[str] = [
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

SOURCE_LANGUAGES: List[str] = ["Auto Detect"] + SUPPORTED_LANGUAGES

LANGUAGE_CODES: Dict[str, str] = {
    "Auto Detect": "AUTO",
    "English": "EN",
    "Korean": "KO",
    "Vietnamese": "VI",
    "Chinese Simplified": "ZH-CN",
    "Chinese Traditional": "ZH-TW",
    "Japanese": "JA",
    "Spanish": "ES",
    "French": "FR",
    "German": "DE",
    "Russian": "RU",
    "Thai": "TH",
    "Arabic": "AR",
    "Portuguese": "PT",
    "Italian": "IT",
    "Indonesian": "ID",
    "Malay": "MS",
    "Filipino": "FIL",
    "Hindi": "HI",
    "Bengali": "BN",
    "Turkish": "TR",
    "Polish": "PL",
    "Dutch": "NL",
    "Ukrainian": "UK",
    "Persian": "FA",
    "Urdu": "UR",
    "Hebrew": "HE",
    "Czech": "CS",
    "Hungarian": "HU",
    "Swedish": "SV",
    "Greek": "EL",
    "Romanian": "RO",
    "Danish": "DA",
    "Finnish": "FI",
    "Norwegian": "NO",
    "Burmese": "MY",
    "Khmer": "KM",
    "Lao": "LO",
}

DEFAULT_CONFIG: Dict[str, Any] = {
    "model_path": DEFAULT_MODEL_PATH,
    "source_language": "Auto Detect",
    "target_language": "English",
    "auto_send": False,
    "sound_enabled": True,
    "hotkey_toggle": "F9",
    "hotkey_lang": "Ctrl+F9",
    "hotkey_mode": "Ctrl+Shift+Enter",
}

# --- Script Detection Regular Expressions ---
# Korean Hangul Compatibility Jamo and Hangul Syllables
KOREAN_REGEX: re.Pattern = re.compile(r"[\u3131-\u318E\uAC00-\uD7A3]")

# Japanese Hiragana and Katakana
JAPANESE_KANA_REGEX: re.Pattern = re.compile(r"[\u3040-\u309F\u30A0-\u30FF]")

# Thai script Unicode block
THAI_REGEX: re.Pattern = re.compile(r"[\u0E00-\u0E7F]")

# Arabic script Unicode blocks
ARABIC_REGEX: re.Pattern = re.compile(r"[\u0600-\u06FF\u0750-\u077F\u08A0-\u08FF]")

# Cyrillic script Unicode block (Russian, Ukrainian, etc.)
CYRILLIC_REGEX: re.Pattern = re.compile(r"[\u0400-\u04FF]")

# Vietnamese diacritics and modified letters
VIETNAMESE_REGEX: re.Pattern = re.compile(
    r"[àáảãạăằắẳẵặâầấẩẫậèéẻẽẹêềếểễệìíỉĩịòóỏõọôồốổỗộơờớởỡợùúủũụưừứửữựỳýỷỹỵđ"
    r"ÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬÈÉẺẼẸÊỀẾỂỄỆÌÍỈĨỊÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢÙÚỦŨỤƯỪỨỬỮỰỲÝỶỸỴĐ]"
)

# Hanzi / CJK Ideographs (Chinese)
HANZI_REGEX: re.Pattern = re.compile(r"[\u4E00-\u9FFF\u3400-\u4DBF]")

# Latin letters (English, Spanish, French, German, etc.)
LATIN_REGEX: re.Pattern = re.compile(r"[a-zA-Z]")

# URLs (http/https/ftp or www. or standalone domains)
URL_REGEX: re.Pattern = re.compile(
    r"^(?:https?://|ftp://|www\.)\S+|^\S+\.(?:com|org|net|edu|gov|io|ai|kr|jp|cn|vn|me|info)(?:/\S*)?$",
    re.IGNORECASE,
)

# Emojis and miscellaneous symbols
EMOJI_REGEX: re.Pattern = re.compile(
    r"[\U00010000-\U0010ffff\u2600-\u26FF\u2700-\u27BF]"
)


def contains_korean(text: str) -> bool:
    """Check if the given text contains any Korean characters.

    Matches Korean Hangul Compatibility Jamo (\\u3131-\\u318E) and
    Hangul Syllables (\\uAC00-\\uD7A3).

    Args:
        text: The string to test.

    Returns:
        True if the text contains at least one Korean character, False otherwise.
    """
    if not text:
        return False
    return bool(KOREAN_REGEX.search(text))


def detect_language(text: str) -> str:
    """Detect language of the given text based on multi-script analysis.

    Distinguishes Hangul, Kana, Hanzi, Vietnamese diacritics, Cyrillic, Thai, Arabic, and Latin.

    Args:
        text: Input string to analyze.

    Returns:
        Detected language name (e.g. 'Korean', 'Japanese', 'Vietnamese', 'Chinese Simplified',
        'Russian', 'Thai', 'Arabic', 'English', or 'Unknown').
    """
    if not text or not text.strip():
        return "Unknown"

    # Korean Hangul
    if KOREAN_REGEX.search(text):
        return "Korean"

    # Japanese Kana (takes priority over Hanzi for Japanese text containing Kanji)
    if JAPANESE_KANA_REGEX.search(text):
        return "Japanese"

    # Thai script
    if THAI_REGEX.search(text):
        return "Thai"

    # Arabic script
    if ARABIC_REGEX.search(text):
        return "Arabic"

    # Cyrillic script (Russian, Ukrainian)
    if CYRILLIC_REGEX.search(text):
        return "Russian"

    # Vietnamese diacritics
    if VIETNAMESE_REGEX.search(text):
        return "Vietnamese"

    # Hanzi without Kana -> Chinese
    if HANZI_REGEX.search(text):
        return "Chinese Simplified"

    # Latin letters -> English
    if LATIN_REGEX.search(text):
        return "English"

    return "Unknown"


def should_translate(
    text: str,
    source_lang: str = "Auto Detect",
    target_lang: str = "English",
) -> bool:
    """Determine whether the input text should undergo translation.

    Returns False if:
    - text is empty or whitespace only
    - text is a URL
    - text contains only numbers, punctuation, or whitespace
    - text contains only emojis or symbols
    - detected source language matches the target language

    Args:
        text: Source text to evaluate.
        source_lang: Expected or configured source language, or 'Auto Detect'.
        target_lang: Configured target language.

    Returns:
        True if valid cross-language translation is needed, False otherwise.
    """
    if not text or not text.strip():
        return False

    trimmed = text.strip()

    # Check if text is a URL
    if URL_REGEX.match(trimmed):
        return False

    # Check if text has linguistic content (remove emojis, digits, whitespace, punctuation)
    no_emojis = EMOJI_REGEX.sub("", trimmed)
    linguistic_chars = re.sub(r"[\d\s\W_]+", "", no_emojis)
    if not linguistic_chars:
        return False

    # Resolve source language
    if source_lang and source_lang != "Auto Detect":
        effective_source = source_lang
    else:
        effective_source = detect_language(trimmed)

    # If source language is known and matches target language, skip translation
    if effective_source != "Unknown":
        if effective_source.lower() == target_lang.lower():
            return False
        # Normalize Chinese Simplified / Chinese Traditional match
        if "chinese" in effective_source.lower() and "chinese" in target_lang.lower():
            if effective_source.lower() == target_lang.lower():
                return False

    return True


def build_prompt(source_text: str, target_lang: str) -> str:
    """Build the translation prompt for the Hy-MT2 model.

    Args:
        source_text: Source text to translate.
        target_lang: Target language name (e.g. 'English', 'Japanese', 'Vietnamese').

    Returns:
        Formatted prompt string.
    """
    return f"Translate the following segment into {target_lang}, without additional explanation. {source_text}"


class Config:
    """JSON-backed configuration manager for Emebalachat.

    Maintains configuration settings, providing persistent storage in config.json.
    """

    def __init__(self, config_path: Optional[str | Path] = None, auto_create: bool = True) -> None:
        """Initialize Config with an optional custom path.

        Args:
            config_path: Path to config.json. Defaults to project root config.json.
            auto_create: Whether to automatically create config.json if not present.
        """
        self.config_path: Path = Path(config_path) if config_path else DEFAULT_CONFIG_PATH
        self._data: Dict[str, Any] = dict(DEFAULT_CONFIG)
        self.load(auto_create=auto_create)

    @property
    def model_path(self) -> str:
        """Absolute or relative path to the GGUF model file."""
        return self._data.get("model_path", DEFAULT_MODEL_PATH)

    @model_path.setter
    def model_path(self, value: str) -> None:
        self._data["model_path"] = str(value)

    @property
    def source_language(self) -> str:
        """Configured source language or 'Auto Detect'."""
        return self._data.get("source_language", "Auto Detect")

    @source_language.setter
    def source_language(self, value: str) -> None:
        self._data["source_language"] = str(value)

    @property
    def target_language(self) -> str:
        """Selected target translation language."""
        return self._data.get("target_language", "English")

    @target_language.setter
    def target_language(self, value: str) -> None:
        self._data["target_language"] = str(value)

    @property
    def auto_send(self) -> bool:
        """Whether to automatically send (press Enter) after pasting translation.

        Defaults to False (Replace Only mode: translation replaces typed text without
        triggering Enter, and subsequent Enter sends normally).
        """
        return bool(self._data.get("auto_send", False))

    @auto_send.setter
    def auto_send(self, value: bool) -> None:
        self._data["auto_send"] = bool(value)

    @property
    def sound_enabled(self) -> bool:
        """Whether audio feedback beeps are enabled."""
        return bool(self._data.get("sound_enabled", True))

    @sound_enabled.setter
    def sound_enabled(self, value: bool) -> None:
        self._data["sound_enabled"] = bool(value)

    @property
    def hotkey_toggle(self) -> str:
        """Hotkey combination for toggling translation mode."""
        return self._data.get("hotkey_toggle", "F9")

    @hotkey_toggle.setter
    def hotkey_toggle(self, value: str) -> None:
        self._data["hotkey_toggle"] = str(value)

    @property
    def hotkey_lang(self) -> str:
        """Hotkey combination for cycling target language."""
        return self._data.get("hotkey_lang", "Ctrl+F9")

    @hotkey_lang.setter
    def hotkey_lang(self, value: str) -> None:
        self._data["hotkey_lang"] = str(value)

    @property
    def hotkey_mode(self) -> str:
        """Hotkey combination for manual translation mode."""
        return self._data.get("hotkey_mode", "Ctrl+Shift+Enter")

    @hotkey_mode.setter
    def hotkey_mode(self, value: str) -> None:
        self._data["hotkey_mode"] = str(value)

    def load(self, auto_create: bool = True) -> None:
        """Load configuration from the JSON file.

        If the file does not exist, defaults are used and optionally saved.
        If the file is invalid JSON, defaults are retained and a warning is logged.

        Args:
            auto_create: If True, creates the config file if missing.
        """
        if self.config_path.exists():
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict):
                        # Merge loaded values over defaults
                        self._data = {**DEFAULT_CONFIG, **loaded}
                        logger.info("Loaded configuration from %s", self.config_path)
                    else:
                        logger.warning(
                            "Invalid configuration format in %s; using defaults.",
                            self.config_path,
                        )
            except Exception as e:
                logger.warning(
                    "Failed to read config file %s: %s; using defaults.",
                    self.config_path,
                    e,
                )
        else:
            logger.info("Config file not found at %s. Using default configuration.", self.config_path)
            if auto_create:
                self.save()

    def save(self) -> None:
        """Save current configuration to config.json."""
        try:
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            with open(self.config_path, "w", encoding="utf-8") as f:
                json.dump(self._data, f, indent=2, ensure_ascii=False)
            logger.info("Saved configuration to %s", self.config_path)
        except Exception as e:
            logger.error("Failed to save config to %s: %s", self.config_path, e)

    def update(self, **kwargs: Any) -> None:
        """Update multiple configuration keys and persist changes.

        Args:
            **kwargs: Key-value pairs to update in configuration.
        """
        for key, value in kwargs.items():
            if key in DEFAULT_CONFIG:
                self._data[key] = value
            else:
                logger.warning("Unrecognized configuration key: %s", key)
                self._data[key] = value
        self.save()

    def cycle_target_language(self) -> str:
        """Cycle target_language to the next one in SUPPORTED_LANGUAGES and persist.

        Returns:
            The newly selected target language name.
        """
        try:
            current_idx = SUPPORTED_LANGUAGES.index(self.target_language)
            next_idx = (current_idx + 1) % len(SUPPORTED_LANGUAGES)
        except ValueError:
            next_idx = 0
        self.target_language = SUPPORTED_LANGUAGES[next_idx]
        self.save()
        return self.target_language

    def to_dict(self) -> Dict[str, Any]:
        """Return a copy of the configuration dictionary."""
        return dict(self._data)

    def __repr__(self) -> str:
        return f"Config(path={self.config_path}, data={self._data})"
