# Emebala Chat Installer

This directory contains the Inno Setup script and assets for building the Emebala Chat Windows installer.

## Prerequisites

1. **Inno Setup 6.4 or later** (hard requirement, enforced at compile time)
   Download and install from: https://jrsoftware.org/isinfo.php
   BOM-less UTF-8 decoding of `.iss`/`.isl` files exists only since 6.3; the
   official bundled `.isl` translations intentionally ship without a BOM
   (removed in 6.5), so an older compiler would garble every non-Latin
   language. Since 6.4 the installer also relies on `CustomMessage {cm:}`
   falling back to the first defined language at runtime, so the 21
   intentionally-undefined installer languages show English instead of raising
   a compile error. `setup.iss` aborts the build with `#error` on pre-6.4
   compilers.

2. **Build the binaries first**
   Use CMake to build the application before compiling the installer. The installer expects ALL of these to exist (the compile fails otherwise):
   - `../build/Emebala_chat.exe` (the application)
   - `../build/Emebala.Engine.exe` (the shared inference host, bundled into the per-user common store — REQ-043)
   - `../build/Emebalachat.Engine.ggml-translate.exe` **and** `../build/worker.ggml-translate.manifest` (the translation worker and its per-family manifest, both bundled next to the host — REQ-006/M6, M7 A-1)

2b. **Stage the Listener-owned engine bundle (REQ-L32 P2-2, session 260925)**
   The ggml-asr worker and the CUDA runtime redists are LISTENER-owned shared
   slots: this installer stages them **only for first-install coverage** and
   never replaces an existing store file (G3 downgrade guard, see
   `SharedSlotReplaceDecision` in `setup.iss`). Populate the staging dir
   before compiling (the dir is git-ignored, local only):
   - copy `Emebala.Engine.ggml-asr.exe` from the Listener build output
     (Listener `build/Release/` or `build_gpu/`, whichever is the current
     release build — the exact source build is recorded per release in
     `docs/…/engine-bundle-hashes.md`)
   - copy `worker.ggml-asr.manifest` (byte copy of the Listener
     `src/worker/worker_manifest_ggml_asr.json`)
   - copy the CUDA v13.3 runtime redists from the toolkit:
     `copy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\x64\cublas64_13.dll" installer\bundled\engine\`
     (same for `cublasLt64_13.dll`, `cudart64_13.dll`)
   If the staging dir is left empty the installer still compiles
   (`skipifsourcedoesntexist`), and the runtime staged gate makes the setup
   install nothing it does not carry — such a build is for verification
   workflows only; a release MUST ship the staged pair.

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
> language (ko/ja/zh-Hans/zh-Hant/en), and reads the About + SharedEngine +
> Guide `TRichEditViewer` memo content back via `WM_GETTEXT`, asserting clean
> text and no CP949-mojibake signature. The static encoding gate cannot
> see display-layer corruption — this gate closes that hole (session
> 260911_0001). Exits non-zero on any failure; do not ship past a FAIL.

> **Uninstall-contract gate — run before every release build (mandatory):**
> `python tools/check_uninstall_contract.py`
> Statically re-derives the shared-engine uninstall contract from `setup.iss`:
> REQ-048 F3 (triple detection, the `SuppressibleMsgBox` confirmation behind
> IDYES with an IDNO preserve default, fail-closed WMI probe, kept notice,
> 11-language message coverage, frozen anchors) **plus the M7 A-3
> registry-aware cleanup invariants** (bundled-only file deletion, user-item
> preservation, bare-name guard, fail-closed preserve of a damaged registry;
> the pre-A-3 blanket `DelTree` is prohibited and trips the gate). Uninstall
> behavior cannot be exercised on a build machine, so do not ship past a FAIL.
> Exits non-zero on any violation.

## Memo page text MUST go through the RTF-safe `MessageLines()`

**Rule:** Any text passed to `CreateOutputMsgMemoPage` (the About,
SharedEngine and Guide wizard pages) MUST be produced by `MessageLines()` in
the `[Code]` section.
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
output\Emebalachat_Setup_0.10.1.exe
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
4. Bundles the shared inference host `Emebala.Engine.exe` into the per-user
   common store `%LOCALAPPDATA%\Emebala\Common\engine\` (REQ-043, plan
   `emebala-engine-host-shared-inference` §7.1). The `[Files]` entry carries a
   `Check: ShouldInstallEngineHost` gate: it installs/replaces the host only
   when `engine.version` next to it is missing, unreadable, or **older** than
   this installer's `AppVersion` (the stamp records the bundled host version =
   the app version that wrote it); an equal or **newer** stamp (another Emebala
   app already placed a same/newer host) skips the copy. The flag
   `uninsneveruninstall` keeps Inno's own uninstaller away from the shared
   file. An informational Shared Engine wizard page (shown right after About,
   REQ-048 F1) explains this install-or-reuse behavior; it adds no user
   choice. A running host is stopped best-effort (`taskkill`) before the copy
   so the exe is not locked.
5. Downloads the AI translation model `Hy-MT2-1.8B-Q8_0.gguf` (file size 1.9 GB;
   the wizard UI rounds it to "about 2 GB" and declares `ExtraDiskSpaceRequired`
   = 2.1 GB) from Hugging Face into the **shared common store**
   `%LOCALAPPDATA%\Emebala\Common\models\`, verifying its SHA-256 against the
   pinned `EXPECTED_MODEL_SHA256` constant in `setup.iss`. An existing
   common-store file with a **matching** pin skips the download. The legacy
   per-app copy `{app}\models\Hy-MT2-1.8B-Q8_0.gguf` from pre-0.10.1 installs
   is **deleted at install time, never migrated** (M1 decision #2): the delete
   runs before the download and is independent of its outcome, so the shared
   store is pin-checked and the model re-downloaded when needed.
6. Generates a `config.json` in the install folder during post-install
   (`CreateConfigFile`); its `model_path` points at the shared common store so
   the embedded fallback engine finds the same model. On first launch the app
   one-shot migrates the config to `%LOCALAPPDATA%\Emebalachat\config.json`,
   which is the canonical location from then on
7. If the model download is skipped or declined, the generated config starts with
   `engine_type: "google"` instead of `"auto"`. This is safe by construction: the
   app's blocking first-run privacy notice discloses the Google transmission
   before the translation engine, hooks, or worker threads are created, so no
   text can reach Google until the user acknowledges the notice (SEC-1
   resolution, design 144800 §2.6 option (i); see `{app}\README.md`
   "Privacy & Data Handling" §4). The dependency is documented in
   `CreateConfigFile` in `setup.iss`.
8. On uninstall, removes the auto-start registry entry and offers to delete the
   user data folder (`%LOCALAPPDATA%\Emebalachat`, settings + diagnostic logs).
   The shared common store (`%LOCALAPPDATA%\Emebala\Common\` — engine + model)
   is handled per REQ-043 (plan §7.3) + REQ-048 F3 (architect 052600 §F3):
   - **Kept with notice** when another Emebala-family app remains installed —
     `IsOtherEmebalaAppInstalled()` scans the HKLM and HKCU uninstall registry
     for a different `Emebala*` DisplayName (own AppId excluded); when a sibling
     app (Emebala_Listner, Emebala Reader, ...) is present the shared engine and
     model stay, and the uninstaller tells the user why
     (`SharedEngineKeptInUse*` messages). Add future family apps to the
     detection simply by their `Emebala` DisplayName prefix.
   - **Kept with notice** when no sibling app is installed but an `Emebala*`
     process is still running: `IsEmebalaProcessRunning()` probes WMI
     (`WbemScripting.SWbemLocator` → `root\cimv2` → `Win32_Process` query; the
     `winmgmts:` moniker form is unavailable in Inno Pascal Script). The probe
     is **fail-closed** — any WMI error preserves the engine.
   - **Deleted only on explicit confirmation** when neither holds: a
     `SuppressibleMsgBox` asks before the registry-aware cleanup and defaults to
     **IDNO (preserve)**, so silent uninstalls and plain "No" answers keep the
     engine. Reinstalling any Emebala product re-downloads the engine + model.
   - **Registry-aware, bundled-only cleanup (M7 A-3)** — what a confirmed
     deletion actually removes changed in 0.10.1: the uninstaller now reads
     `registry.json` in the shared store and deletes **only** the files listed
     by `origin: "bundled"` items (with every `files[]` entry re-checked as a
     bare filename first). Models you registered yourself (`origin: "user"`,
     e.g. your own `.gguf`), unknown-origin or unregistered files, and other
     families' engine binaries are **preserved** — the registry entry is
     dropped from the rewritten document only for bundled items, and the
     rewrite happens before any file deletion so the listing can never point at
     a deleted model. If `registry.json` is unreadable or damaged, the ENTIRE
     shared store is preserved (fail-closed: a destructive fallback is
     forbidden). Engine-dir removal is limited to the files this installer owns
     (orchestrator, worker, manifests, version/components metadata); other
     Emebala products keep their own files.
   - **Model registration is merged, never overwritten (M7 A-2)** — at install
     time the bundled model's entry is merged into the existing `registry.json`
     atomically (write-temp-then-rename), so registrations written by other
     Emebala products or by you are never lost, and two installers running at
     the same time cannot corrupt the file. This protects your registered
     models across reinstall and uninstall.
   The behavior is statically gated by `tools/check_uninstall_contract.py`
   (CHECK 2/8 cover the M7 A-3 invariants); real uninstall verification is a
   manual QA step.

## Model Integrity Verification (release procedure)

The downloaded model's SHA-256 is checked against the `EXPECTED_MODEL_SHA256`
constant in `setup.iss`:

- **Non-empty (current release state)** — pinned to
  `5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4`: the download
  page aborts on any hash mismatch, the temp file is deleted, and the user is
  offered Retry / Skip / Cancel. The hash is re-checked (Inno Setup 6.3+) before
  the file is moved into `%LOCALAPPDATA%\Emebala\Common\models` (REQ-043; the
  destination was `{app}\models` before 0.10.1).
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
