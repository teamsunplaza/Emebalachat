# Emebala Chat E2E Harnesses

These harnesses spawn the REAL `Emebala_chat.exe` GUI application and drive a
real Notepad window against it. They are **OPT-IN, MANUAL-ONLY** tools: they
are NOT part of `ctest` / `run_tests` and are never run by the default build
or CI. Run them only when you are deliberately doing interactive E2E QA.

## Isolation Contract (REQ-B007, Plan-B-minimum §7.3)

**NEVER run these harnesses while any of the following are active:**

- `EmebalaListener.exe` (the Listener app)
- `Emebala.Engine.exe` (the shared engine host)
- Listener repo's `ctest` / test suite

Concurrent access to the shared engine pipes / Common store from two apps is
exactly the window-flicker class of defect this contract exists to prevent
(see `docs/260928_0001_session_translation-fail-fix/234900_architect-planB-design.md`
§7.1 root cause).

### Pre-flight gate (R2)

`AppSession.start()` in every harness below runs the R2 sibling-process gate
BEFORE any `subprocess.Popen`: it probes for `EmebalaListener.exe` and
`Emebala.Engine.exe` via the existing `list_processes_like()` helper and
aborts with an `ISOLATION GATE` RuntimeError (message points here) if either
is live. The gate is implemented once in `isolation_gate.py`
(`check_isolation_gate`) and imported by both harnesses — no copy-paste.

### Escape hatch: `EMEBALA_E2E_FORCE=1`

Setting `EMEBALA_E2E_FORCE=1` in the environment **skips the sibling-process
gate** (the gate logs that it honored the bypass). This is the documented,
explicit opt-in for the rare intentional cross-app scenario — you take
responsibility for the shared-pipe / Common-store corruption risk when you
set it. There is one first-class consumer: the interleave proof (below).

## Running

```bat
:: 1. Close the Listener app, the engine host, and any Listener ctest run.
:: 2. Then:
python tools/e2e/req027_e2e.py
python tools/e2e/req051_drag_e2e.py
```

## Harness inventory

| Harness | Purpose | Gate? |
|---|---|---|
| `req027_e2e.py` | REQ-027 notepad per-line block translation QA | R2 gate enforced |
| `req051_drag_e2e.py` | REQ-051 drag-gesture / selection translation QA | R2 gate enforced |

## req027_e2e.py: what the current expectations are (261121_0002 re-baseline)

The 2026-11-21 QA run flagged 8 of 13 scenarios red. The analysis
(`docs/261121_0002_session_leftover-release-v0109/explore-report-e2e-baseline.md`)
showed 7 of those were STALE HARNESS expectations, not product regressions:
two product changes landed two hours after the harness was written
(REQ-029-B moved the config to `%LOCALAPPDATA%`, REQ-028 gated the editor
newline on `auto_send`). The harness now encodes the current contract:

- **Config fixture (pinned, restored).** Before the first app launch the
  harness backs up the runtime `config.json` (`%LOCALAPPDATA%\Emebalachat`,
  exe-dir copy only when absent), sets `diag_log_enabled=true`,
  `diag_log_content=true`, `auto_send=<scenario mode>` and verifies the app's
  own `SESSION/diag_log_enabled=.. diag_log_content=..` and
  `SESSION/config loaded=.. auto_send=..` lines. A mismatch aborts the
  scenario as ENVIRONMENT (INCONCLUSIVE), never as a product FAIL. The
  original bytes are written back in a `finally` and the SHA-256 is compared;
  a mismatch exits 3. Only harness-owned keys are written - never user data.
  The fixture lifecycle is **refuse-or-restore** (261121_0002 round 2): the
  pin is written atomically (temp file + `os.replace`), a failed write is a
  HARNESS error (restore, then exit 3, never a truncated config), a `Ctrl+C`
  during teardown still restores, and the pin is **refused** - no write at
  all - in two cases: `--no-cleanup` (the still-live app's exit save would
  re-persist the QA flags from memory) and the legacy exe-dir-only layout
  (the app migrates the file to `%LOCALAPPDATA%` at startup, out of the
  restore's reach). A refused pin degrades content checks to BLOCKED and
  `autosend_newline` to INCONCLUSIVE, never PASS.
- **Editor newline = document delta vs the app's own action.** With
  `auto_send=0` (shipped default) the app logs
  `stage=send_enter action=SKIPPED reason=send_gate` and the document must NOT
  grow; with `auto_send=1` it logs `action=synthetic_newline ... auto_send=1`
  and the document must gain exactly one `\n`. No decisive `send_enter` line
  (hold / send-through) means BLOCKED, never PASS. The verdict no longer keys
  on the log-global task index, so the first line of a fresh document is no
  longer required to carry a newline.
- **`autosend_newline` scenario.** The `auto_send=1` leg (3 lines: per-task
  `synthetic_newline` + one newline each, final document exactly `len(lines)`
  newlines, `EditCaretTracker/008` counter 0). Without it the editor-newline
  feature would have zero E2E coverage.
- **Content checks are honest about the privacy gate.** `capture end` and
  `translate end` are parsed in BOTH shapes (content-gated `content="..."`
  / `out="..."`, and the shape-only default). When content is absent the
  content-dependent checks report BLOCKED (with `diag_log_content=false`
  named), never FAIL.
- **`multi_lang` patches the runtime config**, not the exe-dir copy the app
  ignores, and proves the pinned target is live in the file the app loads.
- **`long_text`** keeps its timeout constants and reports the known 1000-char
  capture timeout as INCONCLUSIVE with the capture diagnosis
  (`capture_len`/`capture_result`/`paste`) printed. That is a suspected
  PRODUCT defect and must never be baselined to PASS.

### Exit codes and run manifest

`0` = every chosen scenario PASS; `1` = any FAIL; `2` = no FAIL but at least
one INCONCLUSIVE (includes the two tray scenarios); `3` = harness error or
config-fixture restore mismatch (also: a failed fixture write, which restores
the original bytes first, and a teardown interrupted by `Ctrl+C`, which still
restores). Every run also prints one machine-readable
`E2E_SUMMARY {...}` line and writes `<run-dir>/run-manifest.json` (UTC
timestamps, OS build, app SHA-256 + self-reported SESSION version, config
path + pre/post SHA-256, pinned mode, Notepad frame/edit class, read methods,
tray-UIA outcome, per-scenario verdicts, exit code). No user document content
is ever recorded: environment notes carry lengths only.

The two tray scenarios (`uilang_37`, `reset_defaults`) are environment-bound:
`TrackPopupMenu`'s modal loop can starve the UIA menu provider, which is
INCONCLUSIVE by design and never converted to PASS. Use
`python tools/e2e/req027_e2e.py all --skip-tray-scenarios` when a green gate
is required; their contracts remain covered by the unit suites plus user QA.

`--run-dir <dir>` keeps the manifest in an explicit location;
`--baseline-manifest <saved run-manifest.json>` diffs the environment
fingerprint and prints a drift banner (reporting only - it never changes a
verdict).

## Intentional Cross-App Harnesses (gate-exempt-by-design)

Per Plan-B design v2 amendment A4
(`235700_architect-planB-design-v2-amendments.md`), the following harness is
**implemented** (task B-T9, `planb_interleave_proof.py` exists) and
deliberately runs with both apps live:

- `planb_interleave_proof.py` (REQ-B009): the per-model translate worker pool
  behavioral proof. It DELIBERATELY runs Listener + Chat + engine host
  concurrently. It requires `EMEBALA_E2E_FORCE=1` (the R2 pre-flight gate
  skips its sibling-process check when this is set) and must never be
  executed outside a controlled interleave-proof session.
