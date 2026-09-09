# Emebala Chat Installer

This directory contains the Inno Setup script and assets for building the Emebala Chat Windows installer.

## Prerequisites

1. **Inno Setup 6.1 or later**
   Download and install from: https://jrsoftware.org/isinfo.php

2. **Build Emebala_chat.exe first**
   Use CMake to build the application before compiling the installer. The installer expects the built executable at `../build/Emebala_chat.exe` (relative to this directory).

## How to Compile

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

1. Installs `Emebala_chat.exe` to `Program Files\Emebalachat`
2. Creates Start Menu shortcuts and (optionally) a desktop shortcut
3. Optionally registers the app for auto-start with Windows
4. Downloads the AI translation model `Hy-MT2-1.8B-Q8_0.gguf` (~1.9 GB) from
   Hugging Face, verifying its SHA-256 against the pinned `EXPECTED_MODEL_SHA256`
   constant in `setup.iss`
5. Generates a `config.json` in the install folder (the app one-shot migrates it
   to `%LOCALAPPDATA%\Emebalachat\config.json` on first launch)
6. If the model download is skipped, the generated config starts with
   `engine_type: "google"` instead of `"auto"`
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

Configured in `[Languages]` (plus bundled `.isl` files under `languages\`):

- English
- Korean (한국어)
- Japanese (日本語)
- Chinese Simplified (简体中文) — `languages\ChineseSimplified.isl`
- Chinese Traditional (繁體中文) — `languages\ChineseTraditional.isl`

> Note: installer UI languages are separate from the 38 translation languages
> supported by the app itself (see the root README).
