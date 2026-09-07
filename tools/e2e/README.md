# `tools/e2e/` — REQ-027 notepad translation E2E harness

Automates the manual user-QA items QA-27-B / QA-27-1 / QA-27-4 (design report
[`docs/260907_0001_session_user-test-fixes-req024-027/192100_architect-report-req027-richeditd2dpt-redesign.md`](../../docs/260907_0001_session_user-test-fixes-req024-027/192100_architect-report-req027-richeditd2dpt-redesign.md) §3.4)
and the B-6c edge-flow matrix (design
[`210000_architect-report-issue1-fix-b5b-e2e-matrix.md`](../../docs/260907_0001_session_user-test-fixes-req024-027/210000_architect-report-issue1-fix-b5b-e2e-matrix.md)
§2.4): "평소에 사용하지 않을만한 다양한 flow에서도 에러가 발생 안
해야 해" is now verified by one command.

## Files

| File | Role |
|---|---|
| [`req027_e2e.py`](req027_e2e.py) | Main harness. Launches `build\Emebala_chat.exe` + a real Notepad scratch window, types/drives input, judges on 3 layers (log / content / verdict). |
| [`app_probe.py`](app_probe.py) | D-4a universal-app matrix probe (design 173700 §2.3): lists edit-control classes of the 8 user-named apps (메모장/카톡/디스코드/Chrome/Firefox/HWP/PPT/Word), replays the app's read-only EM_* probe family via ctypes, ports `ClassifyEmProbe` verbatim, and renders the 앱×컨트롤×EM-능력×예상경로×판정 matrix (markdown, `--json` for machine reading, `--fallback` adds the opt-in keyboard-geometry measurement). Reuses the `req027_e2e` ctypes layer (one source of truth, stdlib only). |
| [`uia_read_edit.ps1`](uia_read_edit.ps1) | Layer-2 fallback reader: UIA ValuePattern via `System.Windows.Automation` (ships with .NET — no install). Used only when `WM_GETTEXT` cannot read the edit control. |
| [`uia_close_window.ps1`](uia_close_window.ps1) | Teardown helper: dismisses the Win11 Notepad "save?" dialog via UIA when closing our dirty scratch window. (B-6c fixed the ko-KR discard-button pattern `저장하지 않음`.) |
| [`uia_tray_menu.ps1`](uia_tray_menu.ps1) | B-6c multi_lang 1st-choice path: right-clicks the app tray icon and walks the type → target-language submenu via UIA. Verdict is parsed from its `TRAY:` output lines. |

## Scenarios (11 automated)

| Scenario | Flow (design §2.4 #) | What it proves |
|---|---|---|
| `qa27b` | 1 Hangul line + Enter (existing) | only that line replaced, EM path, exact capture |
| `example1` | 6 sentences, Enter each (existing) | non-cumulative captures, 6 replacements, newline structure |
| `consecutive` | 2 same-language lines (existing) | no `translation_equals_source` skip (QA-27-4) |
| `multi_lang` | 3 blocks EN→JA→VI (new, REQ-019) | per-block target language: `lang_pair tgt` + output script witness |
| `empty_enter` | bare Enter ×3 on empty doc | R5 `empty_capture hold_send` on every Enter, no crash, doc stays empty; **D-4b (F3-B/C-5)**: the no-selection notice MUST surface per held task (`tooltip_show kind=message`) and the paste-window suppression must NOT fire (2.6 s settle guards against a previous scenario's paste still inside the 2000 ms window) |
| `cursor_mid` | caret mid-line via EM_SETSEL + Enter (new) | capture = [start..caret) only; text after the caret survives |
| `backspace_enter` | translate → retype → real VK_BACK across the stored offset → Enter (new) | `/004` clamp path fires and completes safely (last>caret → 0) |
| `shift_enter_multi` | Shift+Enter ×2 then bare Enter (new, REQ-018) | 2× `ENTER_GATE reason=shift_enter_newline`, exactly ONE task, capture spans the multi-line block |
| `paste_then_enter` | external clipboard + Ctrl+V + immediate Enter (B-6c phase 1), **then one immediate retry Enter (D-4b phase 2)** | phase 1: capture == pasted text exactly, normal replacement; phase 2: retry Enter inside the 2000 ms paste window must log `WORKER/ExecuteTask/036` + `decision=paste_window_suppress`, surface NO notice, skip translate/paste, and hand the Enter to the app (document grows by the newline); the `/036` elapsed/window numbers are parsed as `kPasteEmptySuppressMs`-tuning evidence |
| `long_text` | one 1000-char Hangul line + Enter (new) | no EM saturation, full-block capture, replacement normal |
| `notepad_vscode_mix` | window-1 → window-2 → window-1 Enters (new) | per-hwnd caret-offset isolation: window-2 starts `last=0`, window-1 resumes its own stored offset, no cross leak, no spurious `/004` |

`--all` runs all 11 (shared app instance, fresh scratch window per
scenario; `multi_lang` uses its own sessions — see below). INCONCLUSIVE
results are auto-retried once (delegation §4). Individual runs:
`python tools\e2e\req027_e2e.py <scenario>`.

## Requirements

- Windows 10/11 with an **interactive desktop session** (SendInput + real
  foreground windows). **Headless CI cannot run this.**
- Python 3.8+ (stdlib only; **pywinauto not required** — verified absent in
  this environment, the ctypes/P-Invoke + PowerShell path was chosen).
- Built app at `build\Emebala_chat.exe` (no rebuild needed; override with
  `--app-exe`).
- Network: the type path translates via the cloud engine
  (`engine_type: google` in `build\config.json`). Cloud failure is judged
  **INCONCLUSIVE**, never FAIL.
- Do not touch the keyboard/mouse during a run; close any pre-existing
  `Emebala_chat.exe` first (single-instance mutex).

## How verdicts are produced (3 layers)

1. **Log layer (primary)** — parses the session's
   `%LOCALAPPDATA%\Emebalachat\logs\emebalachat_*.log` per pipeline task:
   - NO fallback signature: `EditCaretTracker/002` (focus unresolved),
     `/006` (capability Unknown) or `/007` (NotCapable) — every scenario
     targets Win11 Notepad `RichEditD2DPT` whose intended verdict is
     Capable, so **any /006|/007 line is FAIL** (B-6c watch requirement).
     Exception by design: on an EMPTY document the B-6b (0,0)-ambiguity
     probe may conservatively fall back; `empty_enter` records that as
     INFO (harmless outcome, still asserted).
   - positive `EditCaretTracker em_setselect` (EM path taken);
   - `stage=capture end len=N` equals the expected capture EXACTLY
     (typed line / [start..caret) / pasted text — never cumulative;
     leading/trailing-newline shapes are flagged as ISSUE-1/REQ-023
     defect candidates);
   - `stage=paste result=1`; translation completion =
     `stage=translate end status=0` + paste + `task_end`
     (`tooltip_show kind=translation` belongs to the drag paths only);
   - `/004` is the designed clamp safety net (asserted PRESENT by
     `backspace_enter`, never a FAIL by itself); `/005` (estimate
     fallback) and `/008` (newline-settle timeout) are collected as
     **INFO reference counters** — regression watch, not verdict drivers.
2. **Content layer (secondary)** — reads the edit control
   (`RichEditD2DPT`, via `WM_GETTEXT`, UIA fallback): typed source GONE
   where replacement is expected, document changed, REQ-023 separators
   intact (newline count ≥ translation count), scenario-specific
   survival checks (`cursor_mid` tail, `empty_enter` emptiness).
3. **Verdict layer** — FAIL if any check definitively fails; INCONCLUSIVE
   if environment factors blocked it (cloud status 2/3, hook off, unreadable
   control, foreground lock); PASS only when all checks pass.
   Exit codes: `0` PASS · `1` FAIL · `2` INCONCLUSIVE · `3` harness error.

## Known limits & handoffs (documented per delegation)

- **`ime_composing` is NOT automated (user QA QA-27-6).** The harness
  inserts text via `PostMessageW(WM_CHAR)` / `SendInput(UNICODE)` — neither
  can create a real IME *composition* state (WM_CHAR is post-commit text;
  vk=0 unicode events never set the hook's IME mirror). Simulating jamo
  without a way to VERIFY the OS IME state would produce unverified fake
  verdicts (decisions.md 2026-09-07 20:54). Manual QA: compose Hangul with
  the real IME, press Enter mid-composition; the log must show
  `ENTER_GATE ... reason=ime_composing_commit` and no pipeline task.
- **64K offset saturation is NOT automated (user QA QA-27-7).** `EM_GETSEL`
  answers are WORD-packed (saturate at 65535). 65,000-char input via
  65,000 `PostMessage`s risks message-queue overflow and minutes of
  runtime for no added signal beyond 1000-char proof. `long_text` uses
  1000 chars.
- **`multi_lang` runtime-switch limitation when in fallback mode.** The
  tray-menu UIA automation is ATTEMPTED FIRST (design §2.4 option (b));
  live verdict 260907: the app's tray menu is a modal
  `TrackPopupMenuEx` loop that starves the UIA/MSAA menu provider —
  the `#32768` pane enumerates **zero children** for out-of-proct
  UIA (probe evidence in the batch report). So the harness falls back to
  option (a): per-block fresh app session with `type_target_language`
  pre-injected in `build\config.json` (restored afterwards). **This
  verifies ONLY the startup config-load path — the REQ-019 runtime
  switch path (ApplyLanguageChange via the tray) remains unverified by
  this harness**; live switching is user QA QA-27-3R. `--no-tray` forces
  the fallback immediately. The per-block `STATE/lang_sync` marker is
  watched so that IF a future UIA/automation path does succeed, runtime
  mode runs automatically with its own per-block switch assertions.
- **`notepad_vscode_mix` uses two Notepad windows, not VSCode.** The
  delegation sanctions the substitution; the verified property is
  per-focus-hwnd offset isolation. Win11 makes it STRICTER than the
  original design: both scratch windows share ONE process (pid), so the
  key `(focus_hwnd, pid)` is exercised with the pid constant and only
  hwnd differing. Electron launch/focus automation is environment-
  dependent and its intended verdict is the `/007` FALLBACK, which
  collides with the matrix-wide `/006|/007`-FAIL gate; VSCode/Chrome
  fallback behavior belongs to user QA (B-6b report Next-Step 3).
- **Windows 11 Notepad adoption policy (B-6c).** File-open coalesces into
  a TAB of an existing frame and `/m` is ignored (probed live), so the
  harness launches BLANK and claims a frame by CONTENT: the first fresh
  frame that reads empty, else an existing untitled-and-empty frame
  (reuse pool; preflight `preflight_scrub` trims accumulated blank
  residue but keeps one). A window with any non-`E2Exx` text is never
  adopted, cleared, or closed. Teardown closes ONLY the owned window;
  the shared Notepad process is NEVER killed (a duplicate `stop()` that
  did that was removed in B-6c — user-data-loss hazard). If adoption
  fails, look first for foreground-lock wedges (below) and for session-
  restore content in Settings.
- **Foreground-lock wedge (environment, observed live 260907).**
  `GameInputSvc.exe` can hold an invisible phantom
  `GameInputServiceWindow` as the foreground window with
  `ForegroundLockTimeout` forced to 0x7FFFFFFF; every acquisition API
  (`SetForegroundWindow`, ALT-tap, `AttachThreadInput`,
  `SwitchToThisWindow`, `LockSetForegroundWindow(UNLOCK)`, synthetic
  clicks/Alt+Tab, taskbar UIA invoke, hide/minimize/WM_CLOSE on the
  phantom) then fails and the whole matrix turns INCONCLUSIVE. Recovery
  is OUTSIDE user-mode reach from the harness: stop/restart the
  "GameInputService" (admin), log off/on, or reboot — then re-run.
- **`long_text` intermittency is a PRODUCT-side finding, not harness
  flake.** Observed both PASS and FAIL in one session:
  `WIN32_INPUT/CopySelectionWithSequenceWait/002` — for a 1000-char
  selection Win11 Notepad's Ctrl+C does not bump the clipboard sequence
  within the app's 180 ms budget, the app refuses the stale read, capture
  = 0, and the R5 hold surfaces a no-selection notice (harmless, no
  crash, original text intact — the Enter retried later succeeded within
  ~407 ms in the PASS runs). Fixing the timing belongs to src (out of
  this batch's scope); the harness reports it honestly as FAIL and the
  matrix keeps the evidence.
- Clipboard backup/restore is the app's own responsibility — out of scope.
  `paste_then_enter` and the `multi_lang` fallback change
  `build\config.json` / the clipboard and restore afterwards.
- Waits are polling with explicit timeouts; the only fixed delay is a
  300 ms diag-flush settle before teardown.
- Offline parser proof (earlier batches): against the user's pre-fix log
  `emebalachat_260907041018.log` the task parser recovered 9 tasks,
  cumulative capture growth 61→761 chars, 3× `translation_equals_source`.

## Usage

```bat
python tools\e2e\req027_e2e.py all            ; the full 11-scenario matrix
python tools\e2e\req027_e2e.py qa27b          ; single scenarios...
python tools\e2e\req027_e2e.py multi_lang --no-tray   ; force config-seed mode
python tools\e2e\req027_e2e.py long_text --step-timeout 90
python tools\e2e\req027_e2e.py paste_then_enter  ; incl. D-4b F3-B retry-Enter phase
python tools\e2e\req027_e2e.py empty_enter       ; incl. D-4b notice-required + never-suppress

optional: --app-exe <path>  --no-cleanup (debug: leave the app running)
```

### app_probe.py (D-4a, universal-app matrix)

```bat
python tools\e2e\app_probe.py --help                 ; full option list
python tools\e2e\app_probe.py                        ; markdown matrix, all 8 apps (READ-ONLY)
python tools\e2e\app_probe.py --app word --json      ; one app, machine-readable
python tools\e2e\app_probe.py --app all --fallback   ; + Shift+Home/Ctrl+C geometry (mutates selection+clipboard, never text)
python tools\e2e\app_probe.py --probe-hwnd 0x1234    ; arbitrary control (user-QA escape hatch)
python tools\e2e\app_probe.py --out app-matrix.md    ; also write the rendered matrix
```

Default mode sends **read-only** messages only (EM_GETSEL / EM_GETLIMITTEXT /
WM_GETTEXTLENGTH / EM_GETLINECOUNT / EM_LINEFROMCHAR / EM_GETLINE via
`SendMessageTimeoutW` 100 ms, the app's own SendEm budget) plus an EM_SETSEL
no-op re-set of the *current* range whose reply is verified unchanged. It
never activates windows and never types. `--fallback` opts into the design
§2.3 item-5 measurement (Shift+Home span, Ctrl+Shift+Home span, Ctrl+C
clipboard-sequence/length) which does mutate selection and the clipboard and
needs the foreground - interactive-QA scoped. Launching an app requires
`--top` and only for apps with a locatable install path; anything not
measurable stays "확인 필요" by design (no guesses). Per the D-4 delegation,
the 8-app LIVE run (with each app focused and holding a representative
document) is **user QA**; the harness sandbox proof is `--help` + a
read-only default run in `tools_d4_probe.log` (workspace root).
