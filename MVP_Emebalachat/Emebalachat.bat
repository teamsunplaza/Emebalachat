@echo off
setlocal enabledelayedexpansion

title Emebalachat Assistant
cd /d "%~dp0"

echo ================================================================
echo           Emebalachat: Global Real-Time Translation
echo ================================================================
echo.

:: 1. Check if Python is installed and accessible in PATH
python --version >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Python was not found in your system PATH!
    echo Please install Python 3.10 or higher and ensure 'Add Python to PATH' is checked.
    echo.
    pause
    exit /b 1
)

:: 2. Check virtual environment integrity
set VENV_READY=0

if exist ".venv\Scripts\python.exe" (
    .venv\Scripts\python.exe -c "import sys" >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        set VENV_READY=1
    ) else (
        echo [INFO] Existing local .venv is invalid or points to an obsolete Python interpreter.
        echo [INFO] Removing corrupted .venv...
        rmdir /s /q .venv >nul 2>&1
    )
)

:: Try linking to Translatingchat .venv if present, valid, and sharing packages
if !VENV_READY! equ 0 (
    if exist "D:\OneDrive\Projects\Translatingchat\.venv\Scripts\python.exe" (
        "D:\OneDrive\Projects\Translatingchat\.venv\Scripts\python.exe" -c "import sys" >nul 2>&1
        if !ERRORLEVEL! equ 0 (
            echo [INFO] Detected compatible shared virtual environment at Translatingchat.
            echo [INFO] Creating directory link to share packages...
            mklink /J .venv "D:\OneDrive\Projects\Translatingchat\.venv" >nul 2>&1
            if exist ".venv\Scripts\python.exe" (
                .venv\Scripts\python.exe -c "import sys" >nul 2>&1
                if !ERRORLEVEL! equ 0 (
                    set VENV_READY=1
                    echo [INFO] Successfully linked shared virtual environment.
                )
            )
        )
    )
)

:: If still not ready, create a fresh local virtual environment
if !VENV_READY! equ 0 (
    echo [INFO] Creating new virtual environment at .venv...
    python -m venv .venv
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed to create virtual environment!
        pause
        exit /b 1
    )
    set VENV_READY=1
)

:: 3. Activate the virtual environment
call .venv\Scripts\activate.bat

:: 4. Install dependencies if not previously installed
if not exist ".venv\.installed" (
    echo [INFO] Checking and installing required dependencies...
    set CMAKE_ARGS="-DGGML_CUDA=on"
    set FORCE_CMAKE=1
    pip install --prefer-binary -r requirements.txt
    if !ERRORLEVEL! equ 0 (
        echo. > .venv\.installed
        echo [INFO] All dependencies installed successfully.
    ) else (
        echo [WARNING] Encountered warnings during dependency installation.
        echo [INFO] Installing essential UI and automation packages...
        pip install Pillow pystray pytest
        echo. > .venv\.installed
    )
)

:: 5. Launch Emebalachat
echo.
echo [INFO] Starting Emebalachat background assistant...
echo [INFO] Check the floating badge on your screen or the system tray icon!
echo [INFO] Hotkeys:
echo         F9                 : Toggle Translation ON/OFF
echo         Ctrl + F9          : Cycle Target Language
echo         Ctrl + Shift + Enter: Toggle Auto-Send Mode
echo.

python main.py

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Emebalachat exited with error code %ERRORLEVEL%.
    pause
)
