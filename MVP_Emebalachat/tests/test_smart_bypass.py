"""Unit tests for the Smart Bypass detection and multilingual decision logic."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

# Ensure project root is in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from src.config import contains_korean, detect_language, should_translate


class TestSmartBypassKoreanDetection(unittest.TestCase):
    """Test suite verifying accurate Korean detection for smart Enter bypass."""

    def test_pure_korean_sentences(self) -> None:
        """Pure Korean phrases and sentences must return True."""
        vectors = [
            "안녕하세요",
            "밥 먹었니?",
            "오늘 날씨가 정말 좋습니다.",
            "번역 테스트 중입니다.",
            "감사합니다",
            "수고하셨습니다",
            "반갑습니다",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertTrue(contains_korean(text), f"Failed for: {text!r}")

    def test_korean_mixed_with_english_and_numbers(self) -> None:
        """Mixed Korean with English, alphanumeric, or slang must return True."""
        vectors = [
            "hello 안녕 123",
            "lol ㅋㅋㅋ",
            "discord에서 만나요",
            "Level 99 달성!",
            "GG 수고하셨어요",
            "오후 3시에 call 가능한가요?",
            "API 키를 확인해주세요",
            "Windows 11 환경에서 테스트",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertTrue(contains_korean(text), f"Failed for: {text!r}")

    def test_hangul_jamo_and_slang(self) -> None:
        """Korean Hangul Compatibility Jamo (e.g. ㅋㅋㅋ, ㅎㅎ, ㅠㅠ) must return True."""
        vectors = [
            "ㅋㅋㅋ",
            "ㅎㅎ",
            "ㅠㅠ",
            "ㅜㅜ",
            "ㅅㄱ",
            "ㄱ",
            "ㅎ",
            "ㅏ",
            "ㅣ",
            "ㅇㅋ",
            "ㄷㄷ",
            "ㅂㅂ",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertTrue(contains_korean(text), f"Failed for: {text!r}")

    def test_hangul_unicode_boundary_syllables(self) -> None:
        """Hangul Syllables boundaries (U+AC00 '가' to U+D7A3 '힣') must return True."""
        vectors = [
            "\uAC00",  # '가' (first Hangul syllable)
            "\uD7A3",  # '힣' (last Hangul syllable)
            "가나다라마바사",
            "뷃",
            "뷁",
            "힠",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertTrue(contains_korean(text), f"Failed for: {text!r}")

    def test_pure_english(self) -> None:
        """Pure English phrases must return False."""
        vectors = [
            "hello world",
            "good game",
            "lol",
            "lmao",
            "brb in 5 minutes",
            "How are you doing today?",
            "Let's play another round.",
            "This is purely English text.",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertFalse(contains_korean(text), f"Failed for: {text!r}")

    def test_numbers_symbols_and_urls(self) -> None:
        """Numbers, URLs, markdown, and ASCII punctuation must return False."""
        vectors = [
            "123456",
            "https://discord.com",
            "https://github.com/openai/gpt-4?tab=readme-ov-file#quickstart",
            ":)",
            ":D",
            "¯\\_(ツ)_/¯",
            "!@#$%^&*()_+{}[]:\"<>?,./",
            "1 + 1 = 2",
            "error_code = 0x80004005",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertFalse(contains_korean(text), f"Failed for: {text!r}")

    def test_emojis_and_special_symbols(self) -> None:
        """Unicode emojis and symbols without Korean must return False."""
        vectors = [
            "👍",
            "🚀🔥🎉✨",
            "❤️",
            "🎮🕹️🎲",
            "✔️ ❌ ⚠️",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertFalse(contains_korean(text), f"Failed for: {text!r}")

    def test_empty_and_whitespace(self) -> None:
        """Empty string, whitespace, tabs, and newlines must return False."""
        vectors = [
            "",
            " ",
            "   ",
            "\t",
            "\n",
            "\r\n",
            "  \t \n \r  ",
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertFalse(contains_korean(text), f"Failed for: {text!r}")

    def test_none_or_falsy(self) -> None:
        """None or empty inputs must safely return False without exception."""
        self.assertFalse(contains_korean(None))  # type: ignore

    def test_other_languages_and_scripts(self) -> None:
        """Other non-Korean scripts (Japanese, Chinese, Cyrillic, Latin with diacritics) must return False for contains_korean."""
        vectors = [
            "こんにちは",  # Japanese Hiragana
            "カタカナ",    # Japanese Katakana
            "漢字テスト",  # Japanese Kanji
            "你好，世界",  # Simplified Chinese
            "繁體中文測試", # Traditional Chinese
            "Привет мир", # Russian Cyrillic
            "مرحبا بالعالم", # Arabic
            "Café au lait", # French diacritics
            "naïve façade", # Latin diacritics
            "Guten Tag!",   # German
            "Hola amigo",   # Spanish
        ]
        for text in vectors:
            with self.subTest(text=text):
                self.assertFalse(contains_korean(text), f"Failed for: {text!r}")


class TestSmartBypassMultilingualPairs(unittest.TestCase):
    """Test suite verifying i18n should_translate behavior across multiple language pairs."""

    def test_vietnamese_to_korean(self) -> None:
        """Vietnamese 'Xin chào thế giới' targeting Korean must return True."""
        text = "Xin chào thế giới"
        self.assertEqual(detect_language(text), "Vietnamese")
        self.assertTrue(should_translate(text, target_lang="Korean"))

    def test_chinese_to_vietnamese(self) -> None:
        """Chinese '你好世界' targeting Vietnamese must return True."""
        text = "你好世界"
        self.assertEqual(detect_language(text), "Chinese Simplified")
        self.assertTrue(should_translate(text, target_lang="Vietnamese"))

    def test_english_to_korean_and_spanish(self) -> None:
        """Pure English text targeting Korean or Spanish must return True."""
        text = "How much does this cost?"
        self.assertEqual(detect_language(text), "English")
        self.assertTrue(should_translate(text, target_lang="Korean"))
        self.assertTrue(should_translate(text, target_lang="Spanish"))
        self.assertTrue(should_translate(text, target_lang="Japanese"))

    def test_korean_to_english_and_vietnamese(self) -> None:
        """Korean text targeting English or Vietnamese must return True."""
        text = "도와주셔서 감사합니다!"
        self.assertEqual(detect_language(text), "Korean")
        self.assertTrue(should_translate(text, target_lang="English"))
        self.assertTrue(should_translate(text, target_lang="Vietnamese"))

    def test_bypass_when_text_matches_target_language(self) -> None:
        """Text matching target language should bypass translation."""
        self.assertFalse(should_translate("Hello world, have a good day", target_lang="English"))
        self.assertFalse(should_translate("안녕하세요 만나서 반갑습니다", target_lang="Korean"))
        self.assertFalse(should_translate("Xin chào bạn nhé", target_lang="Vietnamese"))
        self.assertFalse(should_translate("こんにちは、元気ですか？", target_lang="Japanese"))

    def test_bypass_pure_numbers_symbols_and_urls(self) -> None:
        """Non-linguistic inputs must be bypassed regardless of target language."""
        non_linguistic = [
            "123456",
            "999.99",
            "https://discord.com/channels/123/456",
            "http://example.com?query=test",
            ":) ;) :(",
            ":-)",
            "^_^",
            "👍🔥🎉💯",
            "",
            "   \n  \t ",
            "--- ... ---",
        ]
        targets = ["Korean", "English", "Vietnamese", "Spanish", "Japanese"]
        for sample in non_linguistic:
            for target in targets:
                with self.subTest(sample=sample, target=target):
                    self.assertFalse(
                        should_translate(sample, target_lang=target),
                        f"Expected bypass for sample '{sample}' targeting '{target}'",
                    )


if __name__ == "__main__":
    unittest.main()
