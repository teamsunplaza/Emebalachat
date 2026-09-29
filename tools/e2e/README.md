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

## Intentional Cross-App Harnesses (gate-exempt-by-design)

Per Plan-B design v2 amendment A4
(`235700_architect-planB-design-v2-amendments.md`), the following harness is
**planned** (task B-T9) and deliberately runs with both apps live:

- `planb_interleave_proof.py` (REQ-B009): the per-model translate worker pool
  behavioral proof. It DELIBERATELY runs Listener + Chat + engine host
  concurrently. It requires `EMEBALA_E2E_FORCE=1` (the R2 pre-flight gate
  skips its sibling-process check when this is set) and must never be
  executed outside a controlled interleave-proof session.
