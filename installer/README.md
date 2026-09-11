# Emebala Chat Installer

This directory contains the Inno Setup script and assets for building the Emebala Chat Windows installer.

## Prerequisites

1. **Inno Setup 6.3 or later** (hard requirement, enforced at compile time)
   Download and install from: https://jrsoftware.org/isinfo.php
   BOM-less UTF-8 decoding of `.iss`/`.isl` files exists only since 6.3; the
   official bundled `.isl` translations intentionally ship without a BOM
   (removed in 6.5), so an older compiler would garble every non-Latin
   language. `setup.iss` aborts the build with `#error` on pre-6.3 compilers.

2. **Build Emebala_chat.exe first**
   Use CMake to build the application before compiling the installer. The installer expects the built executable at `../build/Emebala_chat.exe` (relative to this directory).

## How to Compile

> **Encoding gate — run before every installer build (mandatory):**
> `python tools/check_installer_encoding.py`
> Verifies every `.iss`/`.isl` text input ISCC consumes is UTF-8-clean
> (project files BOM'd, bundled translations valid UTF-8, compiler-version
> guard present). Exits non-zero on any violation; do not build past a FAIL.

> **Display-text gate — run before every release build (mandatory):**
> `python tools/check_installer_display_text.py`
> Compiles a lowest-privilege probe wizard carrying the REAL
> `[CustomMessages]` + `MessageLines()` from `setup.iss`, launches it per
> language (ko/ja/zh-Hans/zh-Hant/en), and reads the About + Guide
> `TRichEditViewer` memo content back via `WM_GETTEXT`, asserting clean
> text and no CP949-mojibake signature. The static encoding gate cannot
> see display-layer corruption — this gate closes that hole (session
> 260911_0001). Exits non-zero on any failure; do not ship past a FAIL.

## Memo page text MUST go through the RTF-safe `MessageLines()`

**Rule:** Any text passed to `CreateOutputMsgMemoPage` (the About and Guide
wizard pages) MUST be produced by `MessageLines()` in the `[Code]` section.
Never assign plain non-ASCII text to a memo page.

**Why:** `CreateOutputMsgMemoPage` renders through `TRichEditViewer`. When
given a PLAIN string, Inno's internal plain→RTF conversion writes non-ASCII
characters as ANSI hex escapes under the active language code page (CP949 for
Korean), so every non-ASCII character is mangled at display time — Korean,
Japanese, Chinese, accented Latin, and even English em-dashes/bullets garble
(e.g. `에메발라 챗` rendered as `?먮찓諛쒕씪`), while the underlying compiled
message data stays clean (root cause documented in
`docs/260911_0001_session_installer-mojibake-regression/115200_debug-rootcause-installer-mojibake.md`).
`MessageLines()` expands `%n` line breaks and then wraps the body in
hand-built RTF (`ToRtf()`) using `\uN` unicode escapes, which
`TRichEditViewer` consumes verbatim — bypassing the lossy conversion and
keeping the memo scrollable.

**Regression protection:** `tools/check_installer_display_text.py` extracts
`ToRtf()`/`MessageLines()` from this file at run time and refuses to run if
they are missing; remove the RTF wrap and the gate fails on every non-ASCII
language (verified by negative control `tools_tmp_mb26_negative.py`).

### Option 1: GUI (Inno Setup Compiler)

1. Open `setup.iss` in **Inno Setup Compiler**
2. Go to **Build** → **Compile** (or press `Ctrl+F9`)

### Option 2: Command Line

```powershell
iscc.exe setup.iss
```

> **Tip:** Add the Inno Setup installation directory to your `PATH`, or use the full path:
> ```powershell
> "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" setup.iss
> ```

## Output

After a successful compile, the installer will be created at:

```
output\Emebalachat_Setup_0.10.0.exe
```

## Optional: Custom Icons and Images

| File | Description | Status |
|------|-------------|--------|
| `..\assets\Emebala_Chat_Appicon.ico` | Setup icon (`SetupIconFile`, shown in taskbar and EXE) | Committed in the repository `assets\` folder |
| `assets\wizard_large.bmp` | Wizard left-side banner image (164×314 pixels) | Optional — `#ifexist` guarded, compile succeeds without it |
| `assets\wizard_small.bmp` | Wizard header small image (55×58 pixels) | Optional — `#ifexist` guarded, compile succeeds without it |

## What the Installer Does

1. Installs `Emebala_chat.exe` to `Program Files\Emebalachat`, together with
   `LICENSE` and this project's `README.md` (installed as `{app}\README.md` so the
   app's first-run privacy notice "re-read this anytime in the README file"
   guidance resolves on a clean machine — REQ-207/208, design 144800 §2.3)
2. Creates Start Menu shortcuts and (optionally) a desktop shortcut
3. Optionally registers the app for auto-start with Windows
4. Downloads the AI translation model `Hy-MT2-1.8B-Q8_0.gguf` (file size 1.9 GB;
   the wizard UI rounds it to "about 2 GB" and declares `ExtraDiskSpaceRequired`
   = 2.1 GB) from Hugging Face, verifying its SHA-256 against the pinned
   `EXPECTED_MODEL_SHA256` constant in `setup.iss`
5. Generates a `config.json` in the install folder during post-install
   (`CreateConfigFile`); on first launch the app one-shot migrates it to
   `%LOCALAPPDATA%\Emebalachat\config.json`, which is the canonical location from
   then on
6. If the model download is skipped or declined, the generated config starts with
   `engine_type: "google"` instead of `"auto"`. This is safe by construction: the
   app's blocking first-run privacy notice discloses the Google transmission
   before the translation engine, hooks, or worker threads are created, so no
   text can reach Google until the user acknowledges the notice (SEC-1
   resolution, design 144800 §2.6 option (i); see `{app}\README.md`
   "Privacy & Data Handling" §4). The dependency is documented in
   `CreateConfigFile` in `setup.iss`.
7. On uninstall, removes the auto-start registry entry and offers to delete the
   user data folder (`%LOCALAPPDATA%\Emebalachat`, settings + diagnostic logs)

## Model Integrity Verification (release procedure)

The downloaded model's SHA-256 is checked against the `EXPECTED_MODEL_SHA256`
constant in `setup.iss`:

- **Non-empty (current release state)** — pinned to
  `5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4`: the download
  page aborts on any hash mismatch, the temp file is deleted, and the user is
  offered Retry / Skip / Cancel. The hash is re-checked (Inno Setup 6.3+) before
  the file is copied to `{app}\models`.
- **Empty string** → verification is skipped. Intended only for development builds.

If the exact file hosted at `MODEL_URL` ever changes, recompute the hash and
update the constant:

```powershell
# PowerShell
(Get-FileHash .\Hy-MT2-1.8B-Q8_0.gguf -Algorithm SHA256).Hash
```

```cmd
:: or certutil
certutil -hashfile "Hy-MT2-1.8B-Q8_0.gguf" SHA256
```

Pre-existing model files left by earlier installs are hash-checked too. On
mismatch (or an unreadable file) the user is asked an explicit Yes/No: **Yes**
deletes the unverified file and re-downloads the verified model; **No** keeps the
file untouched and skips the download. The app additionally re-verifies the model
SHA-256 at load time (with a `.sha256ok` marker cache) and refuses tampered files.

## Inno Setup License Notes (for distribution)

Inno Setup is used to build this installer. Per the official Inno Setup license
(https://jrsoftware.org/files/is/license.txt), the software may be used
"for any purpose, including commercial applications" — commercial distribution
of our installer is permitted without purchasing a license. The vendor
*requests* (but does not legally require) that commercial users purchase a
voluntary commercial license.

Conditions we comply with (we use Inno Setup unmodified):

1. Binary redistribution of Inno Setup components inside our installer keeps
   the original copyright notices and web-site references intact.
2. We do not misrepresent the origin of Inno Setup.
3. We do not ship modified Inno Setup sources.

If you ever modify Inno Setup itself and ship the modified version, those
changes must be plainly marked as such per license condition 4.

## Installer UI Languages

**32 languages** are registered in `[Languages]` (expanded from 5 in session
260910_0002, B-1). English, Korean, Japanese and 27 additional official Inno
Setup translations resolve from `compiler:Languages\`; the two Chinese variants
use the local files under `languages\`:

- English (`compiler:Default.isl`)
- Korean (한국어), Japanese (日本語) — official compiler ISL
- Chinese Simplified (简体中文) — `languages\ChineseSimplified.isl`
- Chinese Traditional (繁體中文) — `languages\ChineseTraditional.isl`
- Spanish, Portuguese, Brazilian Portuguese, French, German, Italian, Dutch,
  Russian, Turkish, Polish, Ukrainian, Arabic, Hebrew, Swedish, Norwegian,
  Danish, Finnish, Thai, Czech, Hungarian, Catalan, Bulgarian, Slovak,
  Slovenian, Corsican, Armenian, Tamil — official compiler ISL

Custom (project-authored) wizard messages are translated for 11 Latin-alphabet
languages; the rest fall back to English by design to avoid mojibake, while the
standard wizard chrome (Next/Cancel/etc.) is fully translated by the 32 official
ISL files.

> Note: installer UI languages are separate from the 37/38 in-app languages
> (translation targets and UI locales) supported by the app itself (see the root
> README).
