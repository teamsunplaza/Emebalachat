[![Gumroad Sponsor](https://img.shields.io/badge/Gumroad-Sponsor-FF90A0?style=for-the-badge&logo=gumroad&logoColor=white)](https://teamsunplaza.gumroad.com/l/emebala)

# Emebalachat — Ultra-Fast Native Real-Time Translation for Windows

[![Release](https://img.shields.io/badge/Release-v0.10.0-blue.svg?style=flat-square)](https://github.com/teamsunplaza/Emebalachat/releases)
[![Sponsor](https://img.shields.io/badge/Sponsor-Gumroad-FF90A0.svg?style=flat-square&logo=gumroad&logoColor=white)](https://teamsunplaza.gumroad.com/l/emebala)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20x64-0078D6.svg?style=flat-square&logo=windows)](https://microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg?style=flat-square)](LICENSE)
[![llama.cpp](https://img.shields.io/badge/llama.cpp-b6099-orange.svg?style=flat-square)](https://github.com/ggerganov/llama.cpp)
[![Rendering](https://img.shields.io/badge/GUI-Direct2D%20%2F%20DirectWrite-purple.svg?style=flat-square)](#floating-pill-badge-ui)

> ### **"Never copy-paste again. Type in your language, and let Emebalachat translate and replace your text in real-time anywhere."**
>
> **Type in your language. It replaces your text with the translation in real-time.**  
> *No more copy-paste context switching (복붙 없는 번역).*

### ⚡ The 3-Step Magic: Type ➔ Translate ➔ Replace & Send

- ⌨️ **Type** — Type naturally in your native language (Discord, Slack, in-game chat, browser, Excel, anywhere).
- ⚡ **Translate** — Offline local AI (**Hy-MT2-1.8B** via llama.cpp) or cloud engine translates in sub-100ms.
- 🚀 **Replace & Send** — Your original keystrokes are automatically erased and replaced with the translated text, right where your cursor is.

---

**Emebalachat** (에메발라챗) is an ultra-fast, zero-latency native Windows translation tool engineered in pure modern C++20 and Win32 APIs. It seamlessly intercepts input text across any Windows application (Discord, Slack, KakaoTalk, browsers, terminal, IDEs, games), translates it instantly via local LLM or cloud fallback, and places the translated text into the active input field with zero clipboard pollution.

---

## 📑 Table of Contents

- [Executive Overview](#-executive-overview)
- [Architecture & Tech Stack](#-architecture--tech-stack)
- [Key Features](#-key-features)
  - [Zero-Latency Keyboard Hook & Anti-Reentrancy](#1-zero-latency-keyboard-hook--anti-reentrancy)
  - [Dual Translation Engines (Local AI + Cloud Fallback)](#2-dual-translation-engines)
  - [Floating Pill Badge UI (Direct2D / DirectWrite)](#3-floating-pill-badge-ui)
  - [Zero-Leak Clipboard Safety & Privacy](#4-zero-leak-clipboard-safety--privacy)
  - [Smart Content Bypass](#5-smart-content-bypass)
  - [Supported Languages (38 Languages)](#6-supported-languages-38-languages)
- [Global Hotkeys & Mouse Gestures](#-global-hotkeys--mouse-gestures)
- [Project Structure](#-project-structure)
- [Building from Source](#-building-from-source)
  - [Prerequisites](#prerequisites)
  - [Configure & Build (CMake + Ninja)](#configure--build-cmake--ninja)
  - [Running Unit Tests](#running-unit-tests)
- [Installer Generation (Inno Setup)](#-installer-generation-inno-setup)
- [Configuration Reference (`config.json`)](#-configuration-reference-configjson)
- [Security & Privacy Guarantee](#-security--privacy-guarantee)
- [Support & Sponsorship](#-support--sponsorship)
- [Credits & Acknowledgments](#-credits--acknowledgments)
- [License](#-license)

---

## 🚀 Executive Overview

Traditional desktop translation utilities suffer from clunky Electron wrappers, slow response times (> 500ms), privacy leaks into Windows Clipboard History (`Win+V`), or awkward copy-paste manual workflows.

**Emebalachat** eliminates all of these pain points:
- **Instantaneous Native Performance**: Built in pure C++20 with MSVC static runtime (`/MT`), linking directly against Win32, Direct2D, DirectWrite, and WinHTTP.
- **Dual-Engine Flexibility**:
  - **Local AI Engine**: Powered by [llama.cpp](https://github.com/ggerganov/llama.cpp) tag `b6099` loading Tencent's state-of-the-art **Hy-MT2-1.8B** (Q8_0 quantized model). Utilizes full 33-layer GPU acceleration (CUDA sm_75+) with sub-100ms cached inference, falling back seamlessly to CPU multi-threading if GPU is unavailable.
  - **Cloud Fallback Engine**: Built-in, high-speed asynchronous WinHTTP Google Translate client that requires **zero API keys and zero external runtime DLLs**.
- **Invisible In-Place Translation**: Type naturally in your native language, press <kbd>Enter</kbd> or <kbd>Shift</kbd>+<kbd>Enter</kbd>, and watch the text instantly transform and send—working in any chat, form, or document field.
- **Hardware-Accelerated Minimalist UI**: Non-intrusive floating pill badge rendered via hardware Direct2D displaying real-time translation state and language pairs.

---

## 🏗 Architecture & Tech Stack

```mermaid
flowchart TB
    subgraph OS_Layer ["Windows OS & User Interaction"]
        KBD[User Keystroke: Enter / Shift+Enter / Hotkey]
        HOOK[Low-Level Keyboard Hook: WH_KEYBOARD_LL]
        IME[IME Composition Buffer]
        CLIP[Windows System Clipboard]
    end

    subgraph Core_Pipeline ["Emebalachat C++20 Pipeline Worker"]
        FILTER{"Synthetic Event?\n(dwExtraInfo == 0x1337BEEF)"}
        DISPATCH{"Alt / Win Pressed\nor Disabled?"}
        TASK[Pipeline Task Queue & Worker Thread]
        IME_FLUSH[Flush IME Composition Buffer]
        SELECT[Select Current Line: Shift + Home]
        COPY[Extract Selection: Ctrl + C]
        BYPASS{"Smart Bypass Check\n(URLs, pure numbers, emojis, same lang)"}
        RESTORE[RAII Clipboard State Restoration]
    end

    subgraph Translation_Engines ["Dual Translation Architecture"]
        ROUTER{"Engine Router\n(auto / local / google)"}
        LLM["Local AI: llama.cpp (b6099)\nHy-MT2-1.8B Q8_0\nCUDA 33-Layer Offload / CPU"]
        GTRANS["Cloud Engine: WinHTTP Client\nGoogle Translate API\nZero External DLLs"]
    end

    subgraph Presentation_Layer ["Direct2D Presentation & UI"]
        BADGE["Floating Pill Badge\n(Direct2D / DirectWrite / Per-Monitor DPI)"]
        TRAY["System Notification Tray\n(Shell_NotifyIconW Context Menu)"]
        AUDIO["WinMM Audio Feedback\n(Synthesized Status Tones)"]
    end

    KBD --> HOOK
    HOOK --> FILTER
    FILTER -- Yes --> PASS_THROUGH[Normal OS Pass-through]
    FILTER -- No --> DISPATCH
    DISPATCH -- Yes --> PASS_THROUGH
    DISPATCH -- No --> TASK

    TASK --> IME_FLUSH
    IME_FLUSH --> SELECT
    SELECT --> COPY
    COPY --> CLIP
    CLIP --> BYPASS

    BYPASS -- Bypass Triggered --> RESTORE
    BYPASS -- Needs Translation --> ROUTER

    ROUTER -- Local Model Available --> LLM
    ROUTER -- Local Model Missing or Auto Fallback --> GTRANS

    LLM --> PASTE[Set Text & Paste: Ctrl + V]
    GTRANS --> PASTE
    PASTE --> RESTORE
    RESTORE --> BADGE
    RESTORE --> AUDIO
    TRAY -. Controls & Updates .-> TASK
    BADGE -. Drag / Click Gestures .-> TASK
```

---

## ✨ Key Features

### 1. Zero-Latency Keyboard Hook & Anti-Reentrancy
- Installs an asynchronous low-level keyboard hook (`WH_KEYBOARD_LL`) running on a dedicated message-pump thread.
- **Re-entrancy Protection**: All synthetic key events emitted by Emebalachat embed the proprietary signature marker `EXTRA_INFO_MARKER = 0x1337BEEF` into `KBDLLHOOKSTRUCT::dwExtraInfo`. The hook instantly ignores all flagged synthetic events, preventing recursive keystroke loops.
- **IME Composition Flushing**: Simulates a synthetic right-arrow advance before selection to force Windows CJK/Korean IME composition buffers into committed strings.
- **Pass-through Safety**: Transparently lets <kbd>Alt</kbd>+<kbd>Enter</kbd> (Excel newline / full screen) and <kbd>Win</kbd> combinations pass through unhindered.

### 2. Dual Translation Engines
- **Local AI Engine**:
  - Direct C++ integration with `llama.cpp` (b6099).
  - Employs **Tencent Hy-MT2-1.8B** (Q8_0 quantization, ~1.9 GB), custom-tuned for high-fidelity translation across 33+ language pairs.
  - Automatically loads 33 transformer layers onto NVIDIA GPU via CUDA (Compute Capability 7.5+ / Turing, Ampere, Ada Lovelace, Blackwell).
  - KV-cache reuse with in-process cache clearance (`llama_kv_cache_clear`) yielding inference latencies under **100ms** on modern GPUs.
  - Automatic fallback to high-performance AVX2 CPU threads if CUDA drivers or hardware are absent.
- **Built-in Cloud Fallback**:
  - Native asynchronous HTTP client built on `winhttp.dll`.
  - Communicates directly with Google Translate HTTPS endpoints.
  - Requires **no Google Cloud API keys**, no Python runtimes, and zero third-party dynamic libraries.
  - Seamless auto-fallback: If the local model is not downloaded or GPU memory is exhausted, translation continues uninterrupted via the cloud engine.

### 3. Floating Pill Badge UI
- **Hardware-Accelerated Direct2D / DirectWrite**:
  - Lightweight, semi-transparent rounded pill badge rendered with hardware acceleration.
  - Per-monitor DPI awareness (`PerMonitorV2`), rendering crisp typography on 1080p, 1440p, and 4K displays.
  - Dynamic width calculation based on active language labels and status state.
- **Interactive Mouse Gestures**:
  - **Left Click**: Instantly toggle translation between Active and Paused.
  - **Double Click**: Swap source and target languages immediately.
  - **Right Click**: Open full context menu (Language picker, Auto-Send toggle, Sound toggle, Exit).
  - **Left Drag**: Freely reposition anywhere across your displays; coordinates automatically persist in `config.json`.

### 4. Zero-Leak Clipboard Safety & Privacy
- Standard translation tools overwrite the user's clipboard and expose sensitive text to Windows 10/11 Clipboard History.
- Emebalachat implements comprehensive privacy protections:
  - Registers Windows Clipboard Monitor exclusion format tags:
    - `ExcludeClipboardContentFromMonitorProcessing`
    - `CanIncludeInClipboardHistory` (explicitly set to 0)
    - `CanUploadToCloudClipboard` (explicitly set to 0)
  - **RAII State Backup & Restore**: Completely snapshots existing clipboard contents (including non-text formats while safely skipping volatile GDI handles), performs the paste operation, and restores the original clipboard state within 120ms.

### 5. Smart Content Bypass
Avoids wasting compute or generating corrupted translations on non-translatable text:
- **URLs & Links**: Automatically detects `http://`, `https://`, `ftp://`, `www.`, and root domain URLs.
- **Numbers & Math**: Skips pure digits, formulas, timestamps, and currency amounts.
- **Emojis & Emoticons**: Skips strings consisting solely of Unicode emojis, symbols, and punctuation.
- **Language Equivalence**: Detects if the source text is already written in the target language (e.g. typing English while target is set to English) and skips translation immediately.

### 6. Supported Languages (38 Languages)

Emebalachat supports full bidirectional translation across **38 language entries** with full native and localized name recognition:

| Code | English Name | Native Name | Code | English Name | Native Name |
|:----:|:-------------|:------------|:----:|:-------------|:------------|
| `AUTO` | Auto Detect | 자동 감지 | `ID` | Indonesian | Bahasa Indonesia |
| `KO` | Korean | 한국어 | `MS` | Malay | Bahasa Melayu |
| `EN` | English | English | `FIL`| Filipino | Filipino |
| `VI` | Vietnamese | Tiếng Việt | `KM` | Khmer | ភាសាខ្មែរ |
| `ZH-CN` | Chinese Simplified | 简体中文 | `LO` | Lao | ພາສາລາວ |
| `ZH-TW` | Chinese Traditional | 繁體中文 | `HI` | Hindi | हिन्दी |
| `JA` | Japanese | 日本語 | `BN` | Bengali | বাংলা |
| `ES` | Spanish | Español | `TR` | Turkish | Türkçe |
| `FR` | French | Français | `PL` | Polish | Polski |
| `DE` | German | Deutsch | `NL` | Dutch | Nederlands |
| `RU` | Russian | Русский | `UK` | Ukrainian | Українська |
| `TH` | Thai | ไทย | `FA` | Persian | فارسی |
| `AR` | Arabic | العربية | `UR` | Urdu | اردو |
| `PT` | Portuguese | Português | `HE` | Hebrew | עברית |
| `IT` | Italian | Italiano | `CS` | Czech | Čeština |
| `HU` | Hungarian | Magyar | `SV` | Swedish | Svenska |
| `EL` | Greek | Ελληνικά | `RO` | Romanian | Română |
| `DA` | Danish | Dansk | `FI` | Finnish | Suomi |
| `NO` | Norwegian | Norsk | `MY` | Burmese | မြန်မာစာ |

---

## ⌨ Global Hotkeys & Mouse Gestures

| Trigger | Action | Description |
|:--------|:-------|:------------|
| <kbd>F9</kbd> | **Toggle Active / Paused** | Enables or pauses real-time translation with audio chime |
| <kbd>Ctrl</kbd> + <kbd>F9</kbd> | **Cycle Target Language** | Cycles forward through the 37 target languages |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>Enter</kbd> | **Toggle Auto-Send Mode** | Toggles whether <kbd>Enter</kbd> is automatically sent after translation |
| <kbd>Shift</kbd> + <kbd>Enter</kbd> | **Instant Translate & Send** | Translates input line and sends <kbd>Enter</kbd> immediately |
| <kbd>Alt</kbd> + <kbd>Enter</kbd> | **Bypass Pass-through** | Always passed directly to host application (Excel newline, etc.) |
| **Badge Left-Click** | **Pause / Resume** | Toggles active translation status |
| **Badge Double-Click**| **Swap Languages** | Swaps source and target language pair |
| **Badge Right-Click** | **Context Menu** | Opens popup settings, engine selection, and language menu |
| **Badge Left-Drag** | **Reposition Window** | Moves badge across monitors; saves position persistently |

---

## 📂 Project Structure

```
D:\OneDrive\Projects\Emebalachat\
├── CMakeLists.txt              # Primary CMake build specification (C++20, llama.cpp FetchContent)
├── LICENSE                     # MIT Permissive Open-Source License
├── README.md                   # This documentation
├── config.example.json         # Reference configuration template
├── installer\                  # Inno Setup 6.x packaging scripts
│   ├── README.md               # Installer build guide
│   ├── setup.iss               # Inno Setup installer script (0.10.0)
│   ├── assets\                 # Optional setup icons & wizard graphics
│   └── output\                 # Compiled installer binaries
├── src\                        # Production C++20 source code
│   ├── config.hpp/.cpp         # Configuration serialization & 38-language database
│   ├── engine.hpp/.cpp         # Dual-engine translation manager (llama.cpp + WinHTTP)
│   ├── google_translate.hpp/.cpp # Standalone WinHTTP Google Translate client
│   ├── hook.hpp/.cpp           # Asynchronous low-level keyboard hook (WH_KEYBOARD_LL)
│   ├── i18n.hpp/.cpp           # Internationalization and localization strings
│   ├── main.cpp                # Application entry point, mutex & message loop
│   ├── smart_bypass.hpp/.cpp   # Content filter (URLs, numbers, emojis, language matching)
│   ├── sound.hpp/.cpp          # Synthesized WinMM audio notifications
│   ├── unicode_utils.hpp/.cpp  # UTF-8 / UTF-16 conversion & script classification
│   ├── win32_input.hpp/.cpp    # Simulated keyboard injection & RAII clipboard manager
│   ├── worker.hpp/.cpp         # Background pipeline worker thread
│   └── ui\                     # Hardware-accelerated presentation layer
│       ├── badge.hpp/.cpp      # Direct2D / DirectWrite floating pill badge
│       └── tray.hpp/.cpp       # Shell_NotifyIconW system tray integration
├── tests\                      # Native unit test suite
│   └── run_tests.cpp           # 186 unit tests covering all core modules
└── MVP_Emebalachat\            # Python MVP reference prototype
```

---

## 🔨 Building from Source

### Prerequisites
1. **Windows 10 / 11 64-bit** (Build 19041 or newer)
2. **Visual Studio 2022** (MSVC v143 toolset with C++20 support)
3. **CMake 3.24 or higher**
4. **Ninja Build** (recommended) or MSBuild
5. *(Optional for GPU Acceleration)* **NVIDIA CUDA Toolkit 12.x or 13.x**

### Configure & Build (CMake + Ninja)

Open **x64 Native Tools Command Prompt for VS 2022** and execute:

```powershell
# Navigate to project repository
cd D:\OneDrive\Projects\Emebalachat

# Configure CMake with Release optimization and Ninja generator
cmake -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DENABLE_LLAMA_FETCH=ON

# Compile core library, executable, and test suite
cmake --build build --config Release
```

The output binaries will be placed in `build\`:
- `build\Emebalachat.exe` (Win32 GUI application)
- `build\run_tests.exe` (Unit test console runner)

### Running Unit Tests

Emebalachat includes a self-contained unit test suite verifying all 186 subsystem assertions:

```powershell
.\build\run_tests.exe
```

Expected output:
```text
========================================
  Emebalachat C++20 Core Test Suite     
========================================
[RUN] Testing Config & Languages...
[PASS] Config & Languages tests completed.
[RUN] Testing Unicode & Normalization...
[PASS] Unicode & Normalization tests completed.
[RUN] Testing Smart Bypass...
[PASS] Smart Bypass tests completed.
[RUN] Testing Sound Feedback...
[PASS] Sound Feedback tests completed.
[RUN] Testing Win32 Input & Clipboard Safety...
[PASS] Win32 Input & Clipboard Safety tests completed.
[RUN] Testing Google Translate Engine...
[PASS] Google Translate Engine tests completed.
[RUN] Testing Translation Manager...
[PASS] Translation Manager tests completed.
[RUN] Testing Floating Badge Dynamic Sizing...
[PASS] Floating Badge Dynamic Sizing tests completed.
[RUN] Testing Universal i18n Localization...
[PASS] Universal i18n Localization tests completed.
========================================
Total Checks: 186
Failures:     0
========================================
>>> ALL CORE TESTS PASSED SUCCESSFULLY! <<<
```

---

## 📦 Installer Generation (Inno Setup)

To package Emebalachat into a single, self-extracting Windows installer:

1. Download and install [Inno Setup 6.1+](https://jrsoftware.org/isinfo.php).
2. Ensure `build\Emebalachat.exe` has been compiled.
3. Run the Inno Setup compiler:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\setup.iss
```

The compiled installer will be output to:
```
installer\output\Emebalachat_Setup_0.10.0.exe
```

The installer offers:
- Automatic installation to `%ProgramFiles%\Emebalachat`
- Optional auto-start with Windows login
- Automatic download of the `Hy-MT2-1.8B-Q8_0.gguf` model from Hugging Face
- Fallback config generation if model download is skipped

---

## ⚙ Configuration Reference (`config.json`)

On first launch, Emebalachat generates `config.json` next to the executable. An example template is provided in `config.example.json`:

```json
{
  "source_language": "Auto Detect",
  "target_language": "English",
  "engine_type": "auto",
  "model_path": "models/Hy-MT2-1.8B-Q8_0.gguf",
  "auto_send": false,
  "sound_enabled": true,
  "hotkey_toggle": "F9",
  "hotkey_lang": "Ctrl+F9",
  "hotkey_mode": "Ctrl+Shift+Enter",
  "badge_x": -1,
  "badge_y": -1
}
```

### Parameter Details:
- `source_language`: Source language name or code (e.g. `"Auto Detect"`, `"Korean"`).
- `target_language`: Target language name or code (e.g. `"English"`, `"Vietnamese"`, `"Japanese"`).
- `engine_type`:
  - `"auto"`: Prefers local LLM if the model exists; automatically falls back to Google Translate.
  - `"local"`: Strictly forces local llama.cpp model.
  - `"google"`: Strictly forces Google Translate via WinHTTP.
- `model_path`: Relative or absolute path to the `.gguf` model file.
- `auto_send`: If `true`, automatically simulates an <kbd>Enter</kbd> keypress after replacing text.
- `sound_enabled`: Enables pleasant synthesized audio tones for hotkey actions.
- `badge_x` / `badge_y`: Screen coordinates for floating badge position (`-1` centers at top of screen).

---

## 🔒 Security & Privacy Guarantee

- **No Remote Telemetry**: Emebalachat contains zero tracking, zero telemetry, and zero third-party analytics.
- **Offline Capable**: In `local` engine mode with `Hy-MT2-1.8B`, all translation runs strictly offline on your local CPU/GPU. No text leaves your machine.
- **Clipboard Isolation**: Temporary text placed on the clipboard is explicitly flagged with Windows privacy exclusions (`CanIncludeInClipboardHistory = 0`), preventing your sensitive messages from appearing in Windows Cloud Clipboard or <kbd>Win</kbd>+<kbd>V</kbd> history.

---

## 💖 Support & Sponsorship

If **Emebalachat** enhances your daily workflow, saves you from manual copy-pasting, or helps you communicate seamlessly across languages, consider supporting ongoing development:

[![Gumroad Sponsor](https://img.shields.io/badge/Gumroad-Sponsor%20Emebala-FF90A0?style=for-the-badge&logo=gumroad&logoColor=white)](https://teamsunplaza.gumroad.com/l/emebala)

Your support directly fuels local AI model performance tuning, new language features, and multi-platform expansion. Thank you!

---

## 🤝 Credits & Acknowledgments

- **Team Sunplaza** — Architectural design, native Win32/C++20 development, UI/UX, and maintenance.
- **[llama.cpp](https://github.com/ggerganov/llama.cpp)** by Georgi Gerganov and contributors — High-performance cross-platform LLM inference engine.
- **[Tencent Hunyuan](https://github.com/Tencent/HunyuanTranslation)** — Creator of the superb **Hy-MT2-1.8B** high-speed multilingual translation model.

---

## 📄 License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for complete details.

Copyright (c) 2026 **Team Sunplaza**. All rights reserved.
