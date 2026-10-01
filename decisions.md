# decisions.md — Emebalachat (append-only)

## 2026-09-30 (session 260930_0003) — CEO decisions on Listener handoff + user_gguf truncation

- **D1 (user_gguf long-text truncation fix = options 가+다).** Raise llama n_ctx 4096 -> 8192
  and the prompt token budget 2032 -> >=6000 (generation reserve 2048 kept), AND replace the
  over-budget head+tail mid-text discard with paragraph-boundary chunking + stitching
  (a single paragraph that alone exceeds budget falls back to the existing shrink).
  Rationale (CEO): the fix must behave identically for ALL models present and future;
  translation quality outranks speed.
- **D2 (translate worker filename unification = Listener report option 1, fundamental fix).**
  One deployed name everywhere: `Emebala.Engine.ggml-translate.exe` (Listener family
  convention). Covers build output (CMake OUTPUT_NAME), installer [Files]+WORKER_FILENAME,
  install-time legacy absorb (rename `Emebalachat.Engine.ggml-translate.exe`; if both exist,
  legacy deleted), orchestrator spawn path (host_main.cpp), bootstrap required-component
  (engine_host_bootstrap_client), kWorkerExe, e2e scripts, docs (AGENTS.md/README/installer
  README). The REQ-043 name freeze ("installer contract is frozen") is LIFTED by this
  decision. Mutex/pipe/manifest names stay frozen (wire contracts untouched).
- **D3 (bundled small items).** CUDA DLLs (cublas64_13/cublasLt64_13/cudart64_13) added to
  the M7 A-3 owned cleanup + uninstall-contract gate literals; bt8 test fixture ASR id
  refreshed to whisper-large-v3-turbo-q5-0-v1. Badge idle-fade UX improvement deferred
  (optional, not requested).
- Listener-side follow-up owed after D2 lands: reply about re-enabling their `--strict`
  cross-check.

## 2026-09-30 17:53 — Scope change: badge idle-fade item dropped

- CEO: the 5 s badge dimming is INTENTIONAL and fine; only the disappearance was a
  defect (already fixed in the 2026-09-22 rerelease). Cancelled the planned idle-opacity
  raise. No badge UX work in this initiative. Coder C stream dropped.

## 2026-09-30 19:53 — Initiative 260930_0003 COMPLETE (working tree, uncommitted)

- **D1 landed**: llama n_ctx 4096->8192, prompt budget 2032->6128 (8192-2048-16);
  over-budget input now splits at paragraph boundaries and stitches with original
  separators; single over-budget paragraph keeps the per-chunk head+tail fallback;
  within-budget path byte-identical.
- **D2 landed**: ONE worker name `Emebala.Engine.ggml-translate.exe` across CMake
  OUTPUT_NAME, installer (WORKER_FILENAME + [Files] + install-time legacy absorb
  AbsorbLegacyWorkerFileName), host spawn path, kWorkerExe, bootstrap required
  component, e2e scripts, AGENTS.md/README/installer README. Wire names frozen.
- **D3 landed**: CUDA DLL 3 rows in M7 A-3 cleanup + gate CHECK 11; bt8 fixture
  refreshed to whisper-large-v3-turbo-q5-0-v1 (incl. the FindModel literal the
  brief missed). Stale build-tree old-name exe recycled.
- **Verification (all unconditional)**: build green /W4; run_tests 6127 checks 0
  failures; uninstall-contract + encoding + display-text gates PASS; real ISCC
  compile Successful (75 s); code-reviewer PASS after P1(empty-chunk)+P2(headless
  packer tests) remediation; security-reviewer PASS (0 critical/high/medium);
  blind-qa PASS incl. a REAL translation served through the renamed worker.
- **Accepted deferrals**: setup.iss absorb runs before StopRunningEngineHost (P3:
  a running legacy worker can block the rename; log-only, self-heals on next
  reinstall). plans/req051 doc and fidelity_probe 4096/2032 pins left as follow-ups.
- **NEW pre-existing defect discovered by blind-qa (NOT caused by this change set,
  out of scope)**: after a FAIL-CLOSED asr session_open, that orchestrator v2
  connection is poisoned — the next translate session_open is swallowed (90 s no
  frame; 0.1 s on a fresh connection). 10/11 m6 smoke probes pass otherwise.
  Needs a dedicated debug session (orchestrator AsrOpenSession/RunSessionV2).
- **Uncommitted by policy** — commit requires user approval; propose 2 atomic
  commits: (1) translation quality, (2) worker-name unification + D3.

## 2026-09-30 21:40 — Committed, Listener follow-up answered, setup regenerated

- **Listener follow-up (chat-followup-reply.md) received**: their `--strict` rerun
  PASSED (made default); they mirrored our request (dropped their rename staging step);
  they independently confirmed the asr-poison root cause matches our fix; they reported
  two more pre-existing defects (3-1 client-write missing terminal, 3-2 v2 answers
  hardcoded id 0). Both verified in our code and FIXED (see commits below).
- **Commits (user-approved, 5 atomic)**:
  - 7d73a7d feat(engine): paragraph chunking (D1)
  - 3a9f687 refactor(engine): worker filename unification (D2 + defect B + fixture)
  - 0133fe7 fix(host): fail-closed asr open keeps the v2 connection
  - 7e18044 fix(host): echo client request id in every v2 answer
  - c0430e6 docs(decisions): this log
- **Answer to Listener's open question**: the 90 s repro does NOT reproduce on the
  fix-inclusive binary (reviewer re-ran the regression sequence: same-connection
  asr reject -> translate open answers in 0.1 s; m6 smoke 11/11).
- Backlog recorded by review: v2 enqueue probes pinned-only model (consistent with
  current v2 semantics; revisit when v2 gains model selection); fidelity_probe
  4096/2032 pins; absorb ordering P3.
- installer/bundled/engine populated from the Listener bundle (asr pair + CUDA 13.3
  x64); setup.exe regenerated via ISCC for the CEO's clean reinstall test.

## 2026-09-30 22:1x — Version promoted to 0.10.2, pushed, released

- CEO: promote this build to 0.10.2, change every related number, commit,
  push, rebuild the setup, and cut a GitHub Release on teamsunplaza.
- Bumped: CMakeLists project(0.10.2), setup.iss AppVersion/OutputBaseFilename
  (Emebalachat_Setup_0.10.2), README badge+download+check-count, AGENTS.md,
  installer/README, .vscode IntelliSense define, REQ-006 version pins,
  CHANGELOG section promoted to "v0.10.2 - 2026-09-30". Historical "(0.10.1+)"
  annotations and illustrative version examples intentionally untouched.
- Pitfall hit and fixed: a ';' typoed as '#' on the AppVersion history note made
  ISPP read a preprocessor directive (compile aborted, line 45); the Edit tool
  also stripped the setup.iss BOM again - both fixed, encoding gate re-passed.
- Commits 233c0a2 (bump) + 6966b23 (marker fix) pushed to teamsunplaza/main.
- Release: gh release create v0.10.2 on teamsunplaza/Emebalachat with the
  setup exe + CHANGELOG notes.

## 2026-09-30 23:0x — Initiative 260930_0004: version-stamp fix + update checker (v0.10.2 kept)

- **D5**: Fix the exe VERSIONINFO stamp bug (RC files hardcode 0,10,1). CMake
  derives MAJOR/MINOR/PATCH from PROJECT_VERSION and passes them to
  app_icon.rc / engine_icon.rc; VERSIONINFO blocks become macro-driven. The
  version.hpp fallback string syncs too. Result: bumping = the canonical 3
  places only.
- **D6**: Update checker v1 = MANUAL only. About window gains a "Check for
  updates" button. No startup/background checks. Flow: check -> newer? ->
  notice + consent (dialog states the GitHub connection) -> download to %TEMP%
  with progress -> SHA-256 verify against the hash published IN the release
  notes (line format `SHA256: <64 lowercase hex>`) -> app exits -> installer
  runs. Failure/silent-rate-limit = quiet no-op, never an error popup.
- **D7**: i18n for ALL 37 UI languages (English + Korean authored; the rest
  translated, short and neutral; existing fallback stays as safety net).
- **D8**: Version stays 0.10.2; the existing GitHub Release asset is REPLACED
  (new setup exe + notes updated with the new changelog and the new SHA256
  line). README updated (full audit, not rewrite-from-scratch unless stale).

## 2026-10-01 08:0x — Initiative 260930_0004 extension: on-demand model download + i18n audit

- **D9 (CEO, user feedback)**: when the bundled Hy-MT2-1.8B model was NOT downloaded
  during setup, clicking Built-in LocalLLM in the app must offer: "not installed -
  download now?" -> consent -> download window with progress -> SHA-256 verify ->
  register/use. All new UI strings in EVERY supported language (full i18n audit:
  enumerate every id x language gap and close them; no silent fallbacks left).
- CEO also flagged (fixed in-flight): About window lost its intro content to the
  update zone (restore intro, move the update affordance to the footer); blind-QA
  found the Checking-state spinner arc transform bug (arc orbits off-window) and
  RTL footer non-mirroring.

## 2026-10-01 10:0x — Initiative 260930_0004 COMPLETE (pending release replace)

- **Shipped in the working tree**: D5 version-stamp macro fix; D6 update checker
  (About footer button, 6 states, GitHub API, pinned redirect/allowlist, 2 GiB cap,
  SHA256 vs release-notes line, quiet failures); CEO-mandated About restoration
  (byte-faithful intro, footer affordance, group-centered states, spinner
  Rotation*Translation fix, RTL footer mirror); D9 on-demand bundled-model
  download (models/-only gate, consent+progress dialog, HF allowlist, 4 GiB cap,
  pinned-hash verify, atomic move + registry entry + .sha256ok marker); i18n
  144 ids x 37 languages = 5328 cells, 0 gaps (mechanical checker).
- **Defects found & fixed by double-blind review**: D-01 std::terminate on Retry
  (thread reassign; join-only discipline), D-02 dead zone retry rect + (0,0)
  hit-test hole (empty-rect rejection at the shared helper). Deferred LOWs
  (self-heal skip, consent consistency, cancel receive bound) documented.
- **Final verdicts**: code-review PASS (3 passes), security PASS (re-audit),
  blind-qa PASS (spinner pixel-proven 0->61 gold px, RTL mirrored, Notepad e2e
  translation intact), run_tests 6321/0, i18n checker 0 gaps, model-sha sync PASS.
- **Release mechanics**: CHANGELOG v0.10.2 section extended (4 new bullets);
  README updated (Download note + Local engine bullet); setup rebuilt; the
  existing v0.10.2 GitHub Release ASSET is replaced and its notes refreshed
  (new changelog + SHA256 line for the app's verifier). Version stays 0.10.2.

## 2026-10-01 17:0x — Model-offer latch failure (CEO live report) root-caused & fixed

- CEO did a clean reinstall and reported the D9 model download "not implemented".
- Root cause (debug-specialist, live): the REQ-047 streak latch set by ANY failure
  modal silently dropped all queued engine-unavailable requests, INCLUDING
  offer_model_download (main.cpp drain). First boot consent decline -> latch ->
  every later Built-in LocalLLM pick produced total silence.
- Fix: drain bypass for offer requests (latch still coalesces strict failures);
  declined offer no longer falls through into the dead-end notice box. Live-proofed
  on the CEO machine (consent delivered despite latch; clean desktop after decline).
  Review: CONDITIONAL -> P2/P3 nits fixed (discriminating decline pin, comment).
- Setup rebuilt (SHA256 88884230...), commit 6b2f8fe pushed, release asset replaced.
