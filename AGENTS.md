# AGENTS.md — Emebalachat

Guidance for AI coding agents working in this repository. Read this first; it summarizes what the project is, how to build/test it, and the conventions that keep the codebase consistent.

## Project Overview

**Emebala Chat** is a native Windows 10/11 x64 real-time translation application (v0.10.1, MIT license). The user types in their own language, presses Enter, and the typed text is erased and replaced with its translation in place — no clipboard pollution, no copy-paste workflow. It is written in **pure C++20 against raw Win32 APIs** (no frameworks, no Electron, no Qt): Direct2D/DirectWrite for UI, WinHTTP for networking, WinMM for audio, `WH_KEYBOARD_LL`/`WH_MOUSE_LL` low-level hooks for input interception.

Key product facts:

- **Dual translation engines**: a local engine served by the shared per-user host (`Emebala.Engine.exe` orchestrator + `Emebalachat.Engine.ggml-translate.exe` worker, llama.cpp tag `b6099` loading Tencent Hy-MT2-1.8B, Q8_0 GGUF, ~1.9 GB, CUDA GPU offload with CPU fallback) and a cloud engine (direct async WinHTTP client to Google Translate, no API key). The Chat app itself carries **no embedded inference**: its exe is llama-free, and local serving fails over through one-click repair → consent-gated cloud → explicit unavailability notice. Engine selection is `auto` / `local` / `google` via `config.json`.
- **Privacy-first design**: the app operates no servers; cloud translation is consent-gated; clipboard use is RAII snapshot/restore with Windows Clipboard History exclusion formats; diagnostic logging is OFF by default and shape-only unless `diag_log_content` is explicitly enabled.
- **38 language entries** (37 targets + Auto Detect) for translation; UI localized in 37 languages; installer UI in 32 languages.
- The produced binary is `Emebala_chat.exe` (CMake target name is `Emebalachat`; rename is via `OUTPUT_NAME`).

## Technology Stack & Runtime Architecture

- **Build system**: CMake ≥ 3.24 + Ninja (MSBuild also possible), MSVC v143 (VS 2022), C++20, static CRT (`/MT`), `/W4 /utf-8 /permissive-`, `/Zc:char8_t-`.
- **Dependencies**: llama.cpp is fetched at configure time via `FetchContent` (pinned tag `b6099`, static libs). CUDA backend (sm_75/86/89/120) enabled when the toolkit is present; Vulkan backend auto-enabled when the Vulkan SDK with `glslc` is found (`EMEBALA_VULKAN` option, default ON); otherwise CUDA-only with a warning — never a hard fail. No other third-party libraries.
- **Targets** (all in root `CMakeLists.txt`):
  - `Emebalachat_engine_core` — static library with the llama-coupled inference code (`translation_common.cpp` + engine pure helpers); the only target that links llama.
  - `Emebalachat_core` — static library with all `src/` modules except `main.cpp` and the engine_core files.
  - `Emebalachat` — `WIN32` executable → `Emebala_chat.exe` (links core only; no llama symbol).
  - `EmebalaEngine` — `WIN32` executable → `Emebala.Engine.exe` (orchestrator: dual pipes, sessions, scheduler, worker manager, health).
  - `EmebalaEngineGgmlTranslate` — `WIN32` executable → `Emebalachat.Engine.ggml-translate.exe` (translation worker; also emits its per-family manifest `worker.ggml-translate.manifest` next to it — M7 A-1 scheme `worker.<family>.manifest`; skipped entirely in no-llama builds).
  - `run_tests` — unit-test console runner, registered as CTest `CoreTests`.
- **DLL loading**: CUDA (`cublas64_13`, `cublasLt64_13`, `cudart64_13`) and Vulkan (`vulkan-1.dll`, only when ggml-vulkan was built) are **delay-loaded** so machines without GPU drivers fall back to CPU instead of failing at startup. `main.cpp` calls `SetDllDirectoryW(L"")` at startup to close the CWD DLL-hijack vector (do NOT pass NULL — that restores the CWD in the search order; do NOT use `SetDefaultDllDirectories` — it drops PATH and breaks CUDA resolution). `src/vulkan_guard.cpp` adds a delay-load failure hook plus a loader probe that keeps the ggml Vulkan backend out of the registry on driverless machines.
- **Runtime data flow**: keyboard hook (dedicated message-pump thread) → synthetic-event filter (randomized per-process `EXTRA_INFO_MARKER` in `dwExtraInfo`) → pipeline worker thread → IME flush → select line / Ctrl+C → smart-bypass filter (URLs, numbers, emojis, script mismatch, same-language) → engine router → (local) `engine_host_client` over `\\.\pipe\emebala-engine` (canonical) or `\\.\pipe\emebala-engine-v1` (frozen v1 alias), orchestrator spawns the `ggml-translate` worker on demand → or (cloud) WinHTTP Google client → set clipboard + Ctrl+V with RAII clipboard restore → Direct2D badge/tray/tooltip update.
- **Config**: canonical file `%LOCALAPPDATA%\Emebalachat\config.json` (one-shot migration from a legacy config next to the exe). Template in `config.example.json`; full reference in `README.md` §"Configuration Reference".

## Code Organization

```
CMakeLists.txt          # entire build definition (single file, heavily commented)
src/                    # production C++20 sources; one module per .hpp/.cpp pair
  main.cpp              # entry point: single-instance mutex, privacy notice, message loop
  version.hpp           # version string source of truth (stringizes EMEBALACHAT_VERSION_STR)
  hook.cpp / hook.hpp   # low-level keyboard hook, Enter pipeline gating
  worker.cpp / worker.hpp  # background pipeline worker thread + task queue
  engine.cpp            # dual-engine router (shared engine host / Google; no embedded llama)
  google_translate.cpp  # async WinHTTP Google Translate client
  smart_bypass.cpp      # non-translatable content filter
  win32_input.cpp       # key injection + RAII clipboard manager
  unicode_utils.cpp / bidi_utils.cpp  # UTF conversion, script classification, RTL
  i18n.cpp / config.cpp # UI strings (~37 langs) + config serialization / language DB
  diag_logger.cpp       # opt-in per-run diagnostic log (shape-only by default)
  engine_core/          # llama-coupled inference library (translation_common, engine helpers) — links llama
  engine_host_*         # host client, registry/manifest/components parsers, bootstrap client
  host_main.cpp         # Emebala.Engine entry: dual pipes, sessions, scheduler, worker manager, health
  host_v2_*.cpp/.hpp    # session/scheduler/worker-manager/health modules for the orchestrator
  worker_protocol.hpp   # orchestrator↔worker (inner) pipe contract + worker manifest schema (M7 A-1: deployed as worker.<family>.manifest)
  ggml_translate_worker.cpp  # translation worker process entry
  sound.cpp / mouse_hook.cpp / vulkan_guard.cpp / hook_thread.hpp / single_slot_worker.hpp
  ui/                   # Direct2D presentation layer
    badge.cpp  tray.cpp  tooltip.cpp  drag_icon.cpp  about_window.cpp
    layered_renderer.cpp  asset_loader.cpp  dpi.cpp  (Per-Monitor V2 DPI)
tests/run_tests.cpp     # single-file unit test suite (~4,531 checks), links Emebalachat_core (+ m6_engine_host_*.inc, m7_*, req0xx_*.inc suites)
installer/              # Inno Setup 6 packaging (setup.iss, README.md, languages/, assets/)
tools/                  # Python/PowerShell verification & gate scripts (see below)
tools/e2e/              # interactive-desktop E2E harness (Notepad + real app)
docs/                   # session reports, ADRs, release checklist (LOCAL ONLY — gitignored)
assets/                 # logos/icons (PNG/ICO) used by the app and installer
```

## Build & Test Commands

Run from an **x64 Native Tools Command Prompt for VS 2022** on Windows 10/11 x64:

```powershell
# Configure (fetches llama.cpp; requires Git + network on first run)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LLAMA_FETCH=ON

# Build
cmake --build build --config Release
# Outputs: build\Emebala_chat.exe, build\run_tests.exe

# Unit tests (or: ctest --test-dir build)
.\build\run_tests.exe
# Expect "Total Checks: 4755 / Failures: 0 / >>> ALL CORE TESTS PASSED SUCCESSFULLY! <<<"
```

Useful variants:

- `-DENABLE_LLAMA_FETCH=OFF` — CPU-only build without fetching llama.cpp (defines no `HAVE_LLAMA_CPP`; the app then effectively runs cloud-only).
- `-DEMEBALA_VULKAN=OFF` — force CUDA-only even when the Vulkan SDK is installed.
- Separate build trees exist for special configs (`build_gpuoff`, `build_gputest`, …); `build*/` is gitignored.
- **E2E harness** (requires an interactive desktop session — SendInput + real foreground windows; cannot run headless/CI): `python tools\e2e\req027_e2e.py --all` (12 scenarios; individual: `python tools\e2e\req027_e2e.py <scenario>`). Built app must exist at `build\Emebala_chat.exe`. Python 3.8+, stdlib only.
- **Installer gates (mandatory before any installer build/release):**
  - `python tools/check_installer_encoding.py` — encoding gate for all `.iss`/`.isl` text inputs (UTF-8 rules + Inno ≥ 6.4 version guard). Non-zero exit = do not build.
  - `python tools/check_installer_display_text.py` — display-layer mojibake gate (compiles a probe wizard per language and reads memo text back). Non-zero exit = do not ship.
  - `python tools/check_uninstall_contract.py` — static re-derivation of the shared-engine uninstall contract, including the M7 A-3 registry-aware bundled-only cleanup invariants (the pre-A-3 blanket `DelTree` trips the gate). Non-zero exit = do not ship.
  - Then: `& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\setup.iss` → `installer\output\Emebalachat_Setup_0.10.1.exe`. (ISCC lives wherever Inno Setup 6 was installed — `check_installer_encoding.py` prints the detected Inno dir, e.g. a per-user `AppData\Local\Programs\Inno Setup 6` — use that `ISCC.exe`.) The installer bundles `build\Emebala.Engine.exe` **and** `build\Emebalachat.Engine.ggml-translate.exe` + `build\worker.ggml-translate.manifest` (the mandatory engine bundle, per-family manifest scheme — M7 A-1; a missing worker exe fails the compile even in no-llama builds). REQ-L32 P2-2 (session 260925): the installer ALSO stages the Listener-owned ggml-asr pair + CUDA 13.3 runtime DLLs from `installer\bundled\engine\` (git-ignored, populated at release time per `installer\README.md` §2b; `skipifsourcedoesntexist` + the `SharedSlotReplaceDecision` staged gate) — the Chat installer is a NON-owner of that slot: first-install coverage only, never replaces an existing store file (G3). Inno Setup 6.4+ is a hard requirement (enforced by a compile-time `#error`).

## Development Conventions

- **Language & style**: C++20, RAII throughout, `snake_case` files with paired `.hpp/.cpp`, includes grouped with explanatory trailing comments (`// R6 B1: ...`). No external coding-style file exists — match the surrounding code.
- **Comment annotations**: changes are tagged with requirement/session IDs in comments, e.g. `REQ-006`, `P4-B1`, `R18`, `P5-F1`, and session slugs like `260909_0004`. Keep these tags when editing the code they annotate; add a tag referencing the design doc/decision when making a non-trivial change. Session reports live under `docs/<YYMMDD>_NNNN_session_<slug>/` as `HHMMSS_<type>-<topic>.md` files; each session folder typically has a `decisions.md`.
- **Version bump procedure**: version appears in exactly three places that must stay in sync — `CMakeLists.txt` `project(... VERSION x.y.z)` (single source of truth; `version.hpp` stringizes `EMEBALACHAT_VERSION_STR` and must receive it UNQUOTED), `installer/setup.iss` `AppVersion`/`OutputBaseFilename`, and `README.md` badges/download text.
- **Third-party warning isolation (R18)**: never loosen warning flags on project code to silence llama.cpp/ggml noise. The isolated set (`llama ggml ggml-base ggml-cpu ggml-cuda ggml-vulkan common`) carries `/W0 /wd4244 /wd4267` (MSVC) and `--diag-suppress=177` (nvcc) plus SYSTEM-include remapping, applied in `CMakeLists.txt`. Project TUs keep `/W4`.
- **Installer memo pages**: any text passed to `CreateOutputMsgMemoPage` (About/Guide wizard pages) MUST go through the RTF-safe `MessageLines()`/`ToRtf()` in `setup.iss` `[Code]` — never assign plain non-ASCII text to a memo page (TRichEditViewer mojibake under ANSI code pages). The display-text gate extracts and verifies these functions.
- **Model integrity**: `EXPECTED_MODEL_SHA256` in `installer/setup.iss` pins the Hy-MT2-1.8B-Q8_0.gguf SHA-256 (recompute with `certutil -hashfile <file> SHA256` if the hosted file ever changes); the app re-verifies at load time with a `.sha256ok` size+mtime marker cache.
- **Git**: conventional commits (`feat(ledger):`, `fix(prompt):`, `refactor(capture):`, `test(capture):`). Remote: `github.com/teamsunplaza/Emebalachat`. Releases are GitHub Releases built from the Inno Setup output.

## Testing Strategy

1. **Unit tests** — `tests/run_tests.cpp`, one self-contained file linking `Emebalachat_core` (+ `Emebalachat_engine_core` for llama-guarded suites), ~4,531 checks covering every core module (config/languages, unicode/normalization, bidi, smart bypass, engine routing, engine-host protocol/scheduler/worker/registry/manifest/components/bootstrap, UI-audit fixes, clipboard, hooks, worker, UI helpers, vulkan guard). Run after every build; CTest name is `CoreTests`. Add checks there for logic changes.
2. **E2E harness** — `tools/e2e/req027_e2e.py` drives the real app against a real Notepad window (12 scenarios: capture correctness, multi-language blocks, paste-window suppression, caret-mid-line, backspace clamp, per-hwnd offset isolation, tray UI-language enumeration). Verdicts are judged from the diagnostic log; INCONCLUSIVE is auto-retried once. Interactive desktop only.
3. **Gates** — the three installer gates above (encoding, display text, uninstall contract) plus benchmark/verification scripts under `tools/` (e.g. `run_fidelity_experiment.py`, `fidelity_probe`) used during translation-quality tuning sessions.
4. **Manual QA gate** — `docs/RELEASE-CHECKLIST.md` lists user-verified release items (IME composition behavior, hotkeys, privacy-notice flow) that cannot be automated. Note this file and the whole `docs/` tree are gitignored (local-only).

## Security & Privacy Considerations

- Treat any change touching the clipboard, network, logging, or DLL loading as security-relevant; the repo's history includes multiple security audits (see git log / local docs).
- Never log user content by default: `diag_log_*` fields are opt-in (`diag_log_content` default `false` logs shape only — key codes, lengths, timings, window class).
- Never send text to the cloud under `engine_type: "local"` unless `cloud_fallback_enabled` is `true`; the first-run privacy notice must stay blocking before any engine/hook/worker creation.
- Preserve the anti-reentrancy marker (`EXTRA_INFO_MARKER` randomization in `dwExtraInfo`), `SetDllDirectoryW(L"")`, delay-load wiring, and SHA-256 model verification when refactoring.
- Do not commit secrets, `config.json`, models (`*.gguf`), build outputs, or `docs/` (all gitignored by `.gitignore`).

## Repository Hygiene Notes

- The working tree accumulates many session artifacts: hundreds of `*.log` files, `*_base_*.log`, `tools_tmp_*` probe directories, `f1_harness/`, stray `.obj` files at root. These are transient verification leftovers (mostly covered by `.gitignore` patterns like `*.log`, `tools_tmp_*`, `tmp_*`). Do not treat them as source; do not "clean them up" unless asked.
- `docs/` (session reports, ADRs, feedback) is intentionally gitignored and never published; `README.md` + `installer/README.md` are the public documentation and must stay accurate with any behavior change.
- There is no package manager manifest (no `package.json`/`pyproject.toml`/`Cargo.toml`); Python scripts in `tools/` are stdlib-only by design.
