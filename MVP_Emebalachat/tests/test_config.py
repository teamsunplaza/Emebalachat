"""Unit tests for Emebalachat configuration, prompt building, and i18n language management."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

# Ensure project root is in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from src.config import (
    DEFAULT_CONFIG,
    DEFAULT_MODEL_PATH,
    LANGUAGE_CODES,
    SOURCE_LANGUAGES,
    SUPPORTED_LANGUAGES,
    Config,
    build_prompt,
    detect_language,
    should_translate,
)


class TestConfigDefaults(unittest.TestCase):
    """Test default values and initialization of Config."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.config_path = Path(self.temp_dir.name) / "config.json"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_default_values(self) -> None:
        """Verify default configuration values meet specifications."""
        cfg = Config(config_path=self.config_path, auto_create=False)
        self.assertIn("Hy-MT2-1.8B-Q8_0.gguf", cfg.model_path)
        self.assertEqual(cfg.target_language, "English")
        self.assertFalse(cfg.auto_send)
        self.assertTrue(cfg.sound_enabled)
        self.assertEqual(cfg.hotkey_toggle, "F9")
        self.assertEqual(cfg.hotkey_lang, "Ctrl+F9")
        self.assertEqual(cfg.hotkey_mode, "Ctrl+Shift+Enter")

    def test_default_dict_keys(self) -> None:
        """Verify DEFAULT_CONFIG contains all expected keys."""
        expected_keys = {
            "model_path",
            "source_language",
            "target_language",
            "auto_send",
            "sound_enabled",
            "hotkey_toggle",
            "hotkey_lang",
            "hotkey_mode",
        }
        self.assertTrue(expected_keys.issubset(set(DEFAULT_CONFIG.keys())))


class TestConfigPersistence(unittest.TestCase):
    """Test JSON serialization, save/load, and corruption recovery."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.config_path = Path(self.temp_dir.name) / "custom_config.json"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_auto_create_creates_file(self) -> None:
        """Verify auto_create=True creates the config file on disk."""
        self.assertFalse(self.config_path.exists())
        cfg = Config(config_path=self.config_path, auto_create=True)
        self.assertTrue(self.config_path.exists())
        self.assertEqual(cfg.target_language, "English")

    def test_save_and_reload(self) -> None:
        """Verify modified configuration values persist after reload."""
        cfg1 = Config(config_path=self.config_path, auto_create=True)
        cfg1.target_language = "Japanese"
        cfg1.auto_send = False
        cfg1.sound_enabled = False
        cfg1.hotkey_toggle = "F10"
        cfg1.hotkey_lang = "Alt+F9"
        cfg1.hotkey_mode = "Ctrl+Enter"
        cfg1.model_path = r"C:\custom\path\model.gguf"
        cfg1.save()

        # Check raw JSON on disk
        with open(self.config_path, "r", encoding="utf-8") as f:
            raw_data = json.load(f)
        self.assertEqual(raw_data["target_language"], "Japanese")
        self.assertFalse(raw_data["auto_send"])
        self.assertFalse(raw_data["sound_enabled"])
        self.assertEqual(raw_data["hotkey_toggle"], "F10")
        self.assertEqual(raw_data["hotkey_lang"], "Alt+F9")
        self.assertEqual(raw_data["hotkey_mode"], "Ctrl+Enter")
        self.assertEqual(raw_data["model_path"], r"C:\custom\path\model.gguf")

        # Load into new Config instance
        cfg2 = Config(config_path=self.config_path, auto_create=False)
        self.assertEqual(cfg2.target_language, "Japanese")
        self.assertFalse(cfg2.auto_send)
        self.assertFalse(cfg2.sound_enabled)
        self.assertEqual(cfg2.hotkey_toggle, "F10")
        self.assertEqual(cfg2.hotkey_lang, "Alt+F9")
        self.assertEqual(cfg2.hotkey_mode, "Ctrl+Enter")
        self.assertEqual(cfg2.model_path, r"C:\custom\path\model.gguf")

    def test_update_method(self) -> None:
        """Verify Config.update persists multiple values."""
        cfg = Config(config_path=self.config_path, auto_create=True)
        cfg.update(target_language="Spanish", auto_send=False)
        self.assertEqual(cfg.target_language, "Spanish")
        self.assertFalse(cfg.auto_send)

        cfg_reloaded = Config(config_path=self.config_path, auto_create=False)
        self.assertEqual(cfg_reloaded.target_language, "Spanish")
        self.assertFalse(cfg_reloaded.auto_send)

    def test_corrupted_json_fallback(self) -> None:
        """Verify corrupted JSON gracefully falls back to default values."""
        self.config_path.write_text("{ broken: json content ...", encoding="utf-8")
        cfg = Config(config_path=self.config_path, auto_create=False)
        self.assertEqual(cfg.target_language, "English")
        self.assertFalse(cfg.auto_send)
        self.assertTrue(cfg.sound_enabled)

    def test_to_dict(self) -> None:
        """Verify to_dict returns a copy of configuration values."""
        cfg = Config(config_path=self.config_path, auto_create=False)
        data = cfg.to_dict()
        self.assertIsInstance(data, dict)
        self.assertEqual(data["target_language"], "English")
        data["target_language"] = "Mutated"
        self.assertEqual(cfg.target_language, "English")


class TestPromptBuilder(unittest.TestCase):
    """Test zero-shot prompt formatting for the translation engine across diverse languages."""

    def test_build_prompt_english(self) -> None:
        source = "안녕하세요, 반갑습니다."
        target = "English"
        expected = "Translate the following segment into English, without additional explanation. 안녕하세요, 반갑습니다."
        self.assertEqual(build_prompt(source, target), expected)

    def test_build_prompt_japanese(self) -> None:
        source = "오늘 저녁에 시간 괜찮으신가요?"
        target = "Japanese"
        expected = "Translate the following segment into Japanese, without additional explanation. 오늘 저녁에 시간 괜찮으신가요?"
        self.assertEqual(build_prompt(source, target), expected)

    def test_build_prompt_spanish(self) -> None:
        source = "좋은 하루 되세요!"
        target = "Spanish"
        expected = "Translate the following segment into Spanish, without additional explanation. 좋은 하루 되세요!"
        self.assertEqual(build_prompt(source, target), expected)

    def test_build_prompt_vietnamese(self) -> None:
        source = "오늘 회의는 몇 시에 시작합니까?"
        target = "Vietnamese"
        expected = "Translate the following segment into Vietnamese, without additional explanation. 오늘 회의는 몇 시에 시작합니까?"
        self.assertEqual(build_prompt(source, target), expected)

    def test_build_prompt_empty_source(self) -> None:
        source = ""
        target = "French"
        expected = "Translate the following segment into French, without additional explanation. "
        self.assertEqual(build_prompt(source, target), expected)


class TestLanguageSupportAndCycling(unittest.TestCase):
    """Test language definitions, code mapping, and cycling invariants."""

    def test_supported_languages_coverage(self) -> None:
        """Verify SUPPORTED_LANGUAGES contains major Hy-MT supported languages."""
        self.assertGreaterEqual(len(SUPPORTED_LANGUAGES), 30)
        expected_langs = [
            "English",
            "Korean",
            "Vietnamese",
            "Chinese Simplified",
            "Japanese",
            "Spanish",
            "French",
            "German",
            "Russian",
            "Thai",
            "Arabic",
        ]
        for lang in expected_langs:
            self.assertIn(lang, SUPPORTED_LANGUAGES)

    def test_language_codes_resolution(self) -> None:
        """Verify each supported language maps to a valid uppercase language code."""
        for lang in SUPPORTED_LANGUAGES:
            code = LANGUAGE_CODES.get(lang)
            self.assertIsNotNone(code, f"Language '{lang}' missing from LANGUAGE_CODES")
            self.assertIsInstance(code, str)
            self.assertGreater(len(code), 0)
            self.assertEqual(code, code.upper())

    def test_source_languages_contains_auto_detect(self) -> None:
        """Verify SOURCE_LANGUAGES includes 'Auto Detect'."""
        self.assertIn("Auto Detect", SOURCE_LANGUAGES)
        self.assertEqual(LANGUAGE_CODES.get("Auto Detect"), "AUTO")

    def test_language_cycling_logic(self) -> None:
        """Verify language cycling wraps around properly through SUPPORTED_LANGUAGES."""
        total = len(SUPPORTED_LANGUAGES)
        for i, current in enumerate(SUPPORTED_LANGUAGES):
            next_idx = (i + 1) % total
            expected_next = SUPPORTED_LANGUAGES[next_idx]
            curr_idx = SUPPORTED_LANGUAGES.index(current)
            cycled = SUPPORTED_LANGUAGES[(curr_idx + 1) % total]
            self.assertEqual(cycled, expected_next)

        # Full cycle wraps back to starting language
        current = SUPPORTED_LANGUAGES[0]
        for _ in range(total):
            idx = SUPPORTED_LANGUAGES.index(current)
            current = SUPPORTED_LANGUAGES[(idx + 1) % total]
        self.assertEqual(current, SUPPORTED_LANGUAGES[0])


class TestLanguageDetectionAndShouldTranslate(unittest.TestCase):
    """Test multi-language script detection and should_translate decision logic."""

    def test_detect_language_multilingual_vectors(self) -> None:
        """Test script detection across multiple language families."""
        self.assertEqual(detect_language("안녕하세요"), "Korean")
        self.assertEqual(detect_language("こんにちは"), "Japanese")
        self.assertEqual(detect_language("你好世界"), "Chinese Simplified")
        self.assertEqual(detect_language("Xin chào thế giới"), "Vietnamese")
        self.assertEqual(detect_language("Привет мир"), "Russian")
        self.assertEqual(detect_language("สวัสดีชาวโลก"), "Thai")
        self.assertEqual(detect_language("مرحبا بالعالم"), "Arabic")
        self.assertEqual(detect_language("Hello world"), "English")

    def test_should_translate_vietnamese_to_korean(self) -> None:
        """Vietnamese input with target Korean must trigger translation."""
        self.assertTrue(should_translate("Xin chào thế giới", target_lang="Korean"))

    def test_should_translate_chinese_to_vietnamese(self) -> None:
        """Chinese input with target Vietnamese must trigger translation."""
        self.assertTrue(should_translate("你好世界", target_lang="Vietnamese"))

    def test_should_translate_english_to_korean_and_spanish(self) -> None:
        """Pure English input with non-English targets must trigger translation."""
        self.assertTrue(should_translate("Hello world, how are you?", target_lang="Korean"))
        self.assertTrue(should_translate("Hello world, how are you?", target_lang="Spanish"))

    def test_should_translate_bypass_same_language(self) -> None:
        """Input matching target language must be bypassed (return False)."""
        self.assertFalse(should_translate("Hello world", target_lang="English"))
        self.assertFalse(should_translate("안녕하세요", target_lang="Korean"))

    def test_should_translate_bypass_non_linguistic_inputs(self) -> None:
        """Pure digits, URLs, symbols, and emojis must be bypassed."""
        self.assertFalse(should_translate("123456", target_lang="Korean"))
        self.assertFalse(should_translate("https://discord.com", target_lang="Spanish"))
        self.assertFalse(should_translate("https://github.com/project", target_lang="English"))
        self.assertFalse(should_translate(":)", target_lang="Japanese"))
        self.assertFalse(should_translate("👍🚀🔥", target_lang="Vietnamese"))
        self.assertFalse(should_translate("", target_lang="Korean"))
        self.assertFalse(should_translate("   \n\t  ", target_lang="English"))


if __name__ == "__main__":
    unittest.main()
