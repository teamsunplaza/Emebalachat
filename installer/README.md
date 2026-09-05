# Emebala Chat Installer

This directory contains the Inno Setup script and assets for building the Emebala Chat Windows installer.

## Prerequisites

1. **Inno Setup 6.1 or later**
   Download and install from: https://jrsoftware.org/isinfo.php

2. **Build Emebalachat.exe first**
   Use CMake to build the application before compiling the installer. The installer expects the built executable at `../build/Emebalachat.exe` (relative to this directory).

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

Place the following files in the `assets\` subdirectory to customize the installer appearance:

| File | Description | Recommended Size |
|------|-------------|------------------|
| `icon.ico` | Setup icon (shown in taskbar and EXE) | 256×256 multi-resolution |
| `wizard_large.bmp` | Wizard left-side banner image | 164×314 pixels |
| `wizard_small.bmp` | Wizard header small image | 55×58 pixels |

The installer compiles successfully even without these files — they are optional.

## What the Installer Does

1. Installs `Emebalachat.exe` to `Program Files\Emebalachat`
2. Creates Start Menu shortcuts and (optionally) a desktop shortcut
3. Optionally registers the app for auto-start with Windows
4. Downloads the AI translation model (~1.8 GB) from Hugging Face, verifying its
   SHA-256 against the `EXPECTED_MODEL_SHA256` constant in `setup.iss`
5. Generates a `config.json` configuration file
6. If the model download is skipped, the config defaults to Google Translate mode

## Model Integrity Verification (release procedure)

The downloaded model's SHA-256 is checked against the `EXPECTED_MODEL_SHA256`
constant in `setup.iss`:

- **Non-empty** → the download page aborts on any hash mismatch, the temp file is
  deleted, and the user is offered Retry / Skip / Cancel. The hash is re-checked
  (Inno Setup 6.3+) before the file is copied to `{app}\models`.
- **Empty string (default)** → verification is skipped. This is intentional for
  development builds.

Before shipping a release, compute the hash of the exact file hosted at `MODEL_URL`
and paste it (hex only, no separators) into `EXPECTED_MODEL_SHA256`:

```powershell
# PowerShell
(Get-FileHash .\Hy-MT2-1.8B-Q8_0.gguf -Algorithm SHA256).Hash
```

```cmd
:: or certutil
certutil -hashfile "Hy-MT2-1.8B-Q8_0.gguf" SHA256
```

Pre-existing model files left by earlier installs are hash-checked too, but only
logged on mismatch (the installer never deletes user data).

## Supported Languages

- English
- Korean (한국어)
