# Emebalachat Installer

This directory contains the Inno Setup script and assets for building the Emebalachat Windows installer.

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
4. Downloads the AI translation model (~1.8 GB) from Hugging Face
5. Generates a `config.json` configuration file
6. If the model download is skipped, the config defaults to Google Translate mode

## Supported Languages

- English
- Korean (한국어)
