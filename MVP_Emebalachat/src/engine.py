"""Translation engine module for Emebalachat.

Wraps llama-cpp-python's Llama interface with GPU acceleration, thread-safe inference,
greedy decoding, and graceful fallback to CPU or mock translation mode when GPU or
model weights are unavailable.
"""

from __future__ import annotations

import logging
import os
import threading
from pathlib import Path
from typing import Any, Optional

from .config import Config, LANGUAGE_CODES, build_prompt

logger = logging.getLogger(__name__)

# Attempt to import llama_cpp gracefully
try:
    from llama_cpp import Llama
    HAS_LLAMA_CPP = True
except ImportError:
    Llama = None
    HAS_LLAMA_CPP = False
    logger.warning(
        "llama-cpp-python is not installed. TranslationEngine will run in mock mode."
    )


class TranslationEngine:
    """Thread-safe translation engine powered by llama-cpp-python or mock fallback."""

    def __init__(
        self,
        model_path: Optional[str | Path] = None,
        config: Optional[Config] = None,
        force_mock: bool = False,
    ) -> None:
        """Initialize the TranslationEngine.

        Args:
            model_path: Path to the GGUF model file. If not specified, uses config.model_path.
            config: Optional Config instance.
            force_mock: If True, bypass model loading and force mock translation mode.
        """
        self._lock: threading.Lock = threading.Lock()
        self.config: Optional[Config] = config
        self.is_mock: bool = False
        self.llm: Any = None

        if model_path is not None:
            self.model_path: str = str(model_path)
        elif self.config is not None:
            self.model_path = self.config.model_path
        else:
            self.model_path = Config(auto_create=False).model_path

        if force_mock:
            logger.info("TranslationEngine forced to mock mode.")
            self.is_mock = True
        else:
            self._initialize_model()

    def _initialize_model(self) -> None:
        """Initialize the Llama model with GPU acceleration and graceful fallback."""
        if not HAS_LLAMA_CPP:
            logger.warning(
                "llama_cpp library unavailable; initializing TranslationEngine in mock mode."
            )
            self.is_mock = True
            return

        if not self.model_path or not os.path.exists(self.model_path):
            logger.warning(
                "Model file '%s' not found; initializing TranslationEngine in mock mode.",
                self.model_path,
            )
            self.is_mock = True
            return

        # Attempt GPU acceleration first (n_gpu_layers=-1, n_ctx=4096, verbose=False)
        try:
            logger.info(
                "Loading model with GPU acceleration (n_gpu_layers=-1, n_ctx=4096): %s",
                self.model_path,
            )
            self.llm = Llama(
                model_path=self.model_path,
                n_gpu_layers=-1,
                n_ctx=4096,
                verbose=False,
            )
            self.is_mock = False
            logger.info("Model loaded successfully with GPU acceleration.")
            return
        except Exception as e_gpu:
            logger.warning(
                "GPU initialization failed (%s). Attempting CPU fallback (n_gpu_layers=0)...",
                e_gpu,
            )

        # Attempt CPU fallback
        try:
            self.llm = Llama(
                model_path=self.model_path,
                n_gpu_layers=0,
                n_ctx=4096,
                verbose=False,
            )
            self.is_mock = False
            logger.info("Model loaded successfully on CPU.")
        except Exception as e_cpu:
            logger.warning(
                "CPU initialization failed (%s). Falling back to mock translation mode.",
                e_cpu,
            )
            self.llm = None
            self.is_mock = True

    def _mock_translate(self, text: str, target_lang: str) -> str:
        """Generate a mock translation response for testing and fallback scenarios.

        Args:
            text: Source text.
            target_lang: Name of target language.

        Returns:
            Mock translated string in the format '[MOCK <LANG_CODE>: <text>]'.
        """
        code = LANGUAGE_CODES.get(target_lang, target_lang.upper()[:2])
        return f"[MOCK {code}: {text}]"

    def translate(self, text: str, target_lang: Optional[str] = None) -> str:
        """Translate text into target_lang in a thread-safe manner.

        Args:
            text: Source text to translate.
            target_lang: Name of target language. If None, uses config or defaults to 'English'.

        Returns:
            The translated text string, or empty string if input is blank.
        """
        if not text or not text.strip():
            return ""

        clean_text = text.strip()

        if target_lang is None:
            if self.config is not None:
                target_lang = self.config.target_language
            else:
                target_lang = "English"

        with self._lock:
            if self.is_mock or self.llm is None:
                return self._mock_translate(clean_text, target_lang)

            prompt = build_prompt(clean_text, target_lang)
            try:
                # Fast greedy decoding: temperature=0.0, max_tokens=2048, repeat_penalty=1.05
                response = self.llm(
                    prompt=prompt,
                    max_tokens=2048,
                    temperature=0.0,
                    repeat_penalty=1.05,
                )

                if isinstance(response, dict):
                    choices = response.get("choices", [])
                    if choices and isinstance(choices[0], dict):
                        output_text = choices[0].get("text", "")
                        return output_text.strip()

                logger.warning("Unexpected response structure from Llama: %s", response)
                return self._mock_translate(clean_text, target_lang)
            except Exception as e:
                logger.error(
                    "Inference error during translation (%s). Falling back to mock translation.",
                    e,
                )
                return self._mock_translate(clean_text, target_lang)

    def reload(self, model_path: Optional[str | Path] = None) -> None:
        """Reload the model, optionally with a new model path.

        Args:
            model_path: Optional new model path.
        """
        with self._lock:
            if model_path is not None:
                self.model_path = str(model_path)
            self._initialize_model()
