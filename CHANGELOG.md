# Changelog

All notable changes to Emebala Chat are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.10.6] - 2026-11-08

Feature and fix release. Single-word hallucinations from the local engine are
closed at the prompt, decode, and output-gate layers; consecutive
multi-sentence in-place translation now works after the first translation;
and a floating back-translation card lets you check that the meaning
survived. The installer verifies the engine binaries it copies, repairs
stale engine slots on upgrade, and no longer reports success over a broken
engine. Upgrading from any 0.10.x build is fully supported.

### Added

- **Floating Back-Translation Verification** (역번역 플로팅 확인): an
  optional floating card that shows your translation translated back into
  the language you typed, so you can judge whether the meaning survived the
  round trip. Off by default because it costs a second engine round trip per
  translation; toggle it from the tray **Floating Back-Translation** row or
  the new `back_translation_enabled` config key (`false` by default; configs
  written before 0.10.6 lack the key and read it as off). The card, its tray
  row, and its strings are translated in all 37 interface languages,
  including right-to-left languages.

### Fixed

Translation:

- **Single-word hallucinations** from the built-in local engine and
  user-registered GGUF models no longer produce invented sentences:
  - The "Auto Detect" source prompt no longer wraps your text in brand-name
    fence markers or appends a 46-word meta-instruction, so a one-word input
    is no longer treated as a chat command.
  - A single ASCII-shaped word now resolves to English instead of collapsing
    into "Auto Detect" and taking the two-stage completion path.
  - Decoded output is stripped of role headers (`assistant:`, `model:`,
    `<|assistant|>`, ...); the meta-leakage needle table now covers
    `assistant:`, `user:`, `system:`, `model:`, `<|im_start|>`,
    `<|im_end|>`, `emebala`, `emebala_source` and `이메발라`; and a
    single-word-to-sentence structural gate rejects answers that turn one
    word into a whole sentence.
  - Single-word generation is capped at 16 tokens, and the amplification
    floors are lowered so 14-32 code-point hallucinations are caught.
  - Chat-template models no longer degrade to the untemplated completion form
    when an answer is rejected: the request fails honestly (retry, cloud
    consent rules, or a clear notice) instead of inventing continuations.
- **Consecutive multi-sentence in-place translation** after a first
  translation:
  - Tail slicing no longer feeds the engine a leading space, and the tail
    gets its own language decision instead of inheriting the first
    sentence's.
  - The paste ledger re-anchors on a smart-bypassed Enter, and a
    deterministic same-line splitter separates already-translated sentences
    from newly typed source-language text when the ledger cannot match.
  - The 16-token echo probe no longer false-aborts on mixed-script
    continuations, the leading-colon stripper no longer eats `:shortcode:`
    emoji, and untranslated source-script dominance is rejected.

Installer & upgrade safety:

- The installer now SHA-256-verifies every engine binary after copying it,
  and only verified components advance the recorded version.
- Owned engine slots are replaced whenever the recorded version or the bytes
  on disk differ, so a same-version reinstall can no longer keep stale
  binaries; the worker manifest is an independent repair target.
- Engine processes are terminated with a bounded wait before the store is
  touched, the legacy worker name included.
- When the elevated profile differs from the invoking user's, the engine
  store is also deployed to the user's own profile, so the app running as
  that user finds a working engine.
- The uninstall shared-store cleanup refuses junction/symlink redirection.
- Post-install verification surfaces failures loudly instead of finishing
  "successfully" with a broken engine.
- `ENGINE_REVISION` is bumped to `.r14` (the engine binaries changed in this
  release).

## [0.10.5] - 2026-11-04

Patch release. Single-word translation hallucinations, input-pipeline
defects (stuck Win key, foreign-window key injection, over-selection), and
engine-host reliability (mid-inference abort, orphaned workers, version
comparison) were audited by independent review teams and fixed. Upgrading
from any 0.10.x build is fully supported.

### Added

- Three user-selectable UI design themes: **Fluent Craft** (default),
  **Warm Editorial**, and **Refined Monolith**, switchable live from the
  tray "UI Theme" submenu or the 3-pill switcher at the top of the About
  window; the choice persists in the new `ui_theme` config key
  (`"fluent"` / `"editorial"` / `"monolith"`). The theme UI is translated
  in all 37 interface languages, including right-to-left languages.
- Direct2D vector icons (Copy, Check, Speak, Swap) rendered in the active
  theme, replacing the emoji-style stand-ins.

### Fixed

Translation:

- `PIPE-1`: the Enter pipeline now passes the detected source language to
  the translation engine instead of raw "Auto Detect", and the
  drag-to-translate pipeline now applies the same smart-bypass gate (URLs,
  numbers, emoji, same-language) as Enter.
- `RC-1`: single words and 1-2 Latin-token phrases (for example "hello",
  "good morning") no longer fall through "Auto Detect" into the two-stage
  completion path that produced invented sentences; short inputs now
  resolve to a real language via core vocabulary, distinctive diacritics,
  or a stopword vote.
- `RC-2`: short inputs no longer come back as long invented text: the
  amplification guard now covers 5-15 character words with ratio checks,
  and 2-character AA / 4-character ABAB words (for example "각각",
  "바이바이", "haha") are no longer misread as laughter repetition.
- `RC-3`: prompts for "Auto Detect" sources now wrap your text in explicit
  source markers with a literal-text instruction, so one-word inputs are
  no longer answered as chat commands or questions.
- `RC-4`: short single-line inputs (up to 16 bytes) are now capped at 64
  generation tokens instead of the 256-token floor that left room for
  hallucinated essays.
- `RC-5`: on chat-template models, an engine answer that echoes the input
  is accepted as-is instead of triggering a raw completion retry that
  invented continuations; any remaining completion retry stops at the
  first line break and keeps all output guards.
- `RC-6`: Vietnamese detection no longer hijacks Romanian/Croatian text
  containing Ă/Đ alone (Vietnamese now needs Vietnamese-only letters or a
  Vietnamese stopword), and short pure-kanji Japanese words are no longer
  forced to Chinese Simplified (unambiguous ones resolve correctly,
  ambiguous ones stay on "Auto Detect"). Enter path fixed; the
  drag-to-translate path keeps a residual for ambiguous short pure-kanji
  strings, as documented by the fix team.
- `BUG-7`: a mostly-foreign sentence containing a few target-script
  characters (for example an English sentence with one Korean word) is no
  longer skipped entirely just because the detected language matches the
  target; the skip now requires the target script to be at least 70% of
  the letters.
- `BUG-8`: Google Translate responses whose translated text contains
  square brackets no longer truncate or misalign the response parser.
- `BUG-9`: the OpenAI-compatible engine no longer sends a broken "from
  Auto Detect to ..." system prompt and now caps `max_tokens` at 8192.
- `BUG-10`: multilingual conversational/refusal prefixes (for example
  "번역 결과:", "죄송합니다", "申し訳", "以下是", "翻译结果") are filtered out of
  translation output as meta-leakage, with guards against false positives
  when your source text itself contains the phrase.

Input & UI:

- `Input P0-1`: hotkeys no longer perform disk writes or tray icon updates
  on the low-level keyboard hook thread; those ride a background worker,
  removing the risk of Windows silently unhooking the app.
- `Input P1-1`: a missing worker destructor that could deadlock process
  shutdown was added.
- `Input P1-2`: pressing a Win-key hotkey no longer leaves the Win key
  stuck down OS-wide (the Win key release always reaches Windows; a dummy
  Ctrl tap masks the Start menu).
- `Input P1-3`: diagnostic key logging no longer destroys dead-key state
  on European keyboard layouts.
- `Input P1-4`: switching to another app mid-translation no longer
  injects a right-arrow keypress into the app you switched to (the release
  is gated on the captured window still being foreground and focused).
- `Input P1-5`: pasting a single line no longer arms a 64-line
  over-selection; the internal counter now follows the actual number of
  pasted line breaks, decreases on Backspace, and resets on Ctrl+A/X/Z.
- `Input P1-6`: IME composition state is now reset on mouse click and on
  foreground-window changes (the modern TSF detection gap itself remains
  documented, per the audit scope).
- `Input P2-1`: edit-selection offsets are read as full 32-bit values, so
  texts over 65,535 characters no longer truncate.
- `Input P2-2`: physically held Win keys are now released before synthetic
  key chords, closing the Win+Ctrl+C / Win+Ctrl+V side effects.
- `Input P2-3`: an originally empty clipboard is restored truly empty
  instead of leaving the translated text behind.
- `Input P2-4`: mouse hook click/drag state now resets when the hook
  disables or a drag completes.
- `Input P3-2`: Unicode normalization retries with the API-requested
  buffer size instead of giving up on `ERROR_INSUFFICIENT_BUFFER`.
- Badge single-click and tray left-click now act immediately; the old
  500 ms double-click recognition delay is removed (`UI-BUG-3`).
- The model-download dialog's error text now meets WCAG AA contrast (deep
  crimson on light gray) (`UI-BUG-1`).
- The tray status dot now antialiases at subpixel level instead of showing
  a stairstep edge (`UI-BUG-2`).
- The About window's update-animation timer now runs only during
  indeterminate progress (`UI-BUG-6`).

Backend & Engine Host:

- `Backend P0-1`: the translation worker now responds to abort and
  shutdown while a translation is still running; a dedicated pipe reader
  thread unwinds the in-flight decode within milliseconds instead of after
  up to 30 seconds. (The pipe-settle half of the finding was adjudicated
  and deliberately left unchanged to preserve the settle unit-test
  contract; the reaper covers genuinely wedged workers.)
- `Backend P0-2`: a recycled engine connection can no longer answer a new
  client with the previous client's queued jobs; every session carries a
  generation counter verified under the write lock, and a dead session's
  pending jobs are purged.
- `Backend P0-3`: Protocol v2 translate requests now parse the `session`
  field, so session model binding and teardown work.
- `Backend P1-1`: canceled pipe reads are now drained synchronously at all
  four `CancelIoEx` sites before the stack buffer leaves scope.
- `Backend P1-2`: cancel requests now verify connection ownership, and the
  v2 dispatcher can cancel a job that is still running.
- `Backend P1-3`: multi-paragraph local translations now run under one
  accumulated 28-second whole-request deadline, so per-paragraph decoding
  no longer overruns the 30-second IPC timeout.
- `Backend P1-4`: model paths under non-ASCII Windows user folders (for
  example Korean or Japanese account names) are no longer corrupted.
- `Backend P1-5`: translation worker processes are bound to a Job Object
  with kill-on-close, so a dead engine host no longer strands a worker
  holding ~2 GiB of VRAM.
- `Backend P2-1`: component version comparison is now numeric, so
  "0.10.5" correctly compares newer than "0.9.9" (the lexicographic
  compare got this wrong).
- `Backend P2-2`: a crashed worker's reserved VRAM and process/pipe
  handles are now reclaimed.
- `Backend P2-3`: a successful worker send now refreshes the heartbeat
  timestamp.
- `Backend FIX`: the translation worker is spawned with the correct
  environment flags, so the built-in local engine and user-registered
  custom models translate again (an early 0.10.5 build could refuse to
  start the worker and answer every local request as `model_missing`).
- `Backend P2-6`: the config parser now skips `/* ... */` block comments,
  and config saving retries the atomic replace on transient sharing
  violations.
- `Backend P3-3`: the engine route resolves the config path once per
  process instead of probing the shell folder on every request.
- `Backend P3-4`: the diagnostic stderr mirror now returns early while
  disabled and unattached instead of formatting and flushing every line.

## [0.10.4] - 2026-10-29

Patch release. Translation reliability, clipboard safety, and installation
security were audited end-to-end by independent review teams and hardened.
Upgrading from any 0.10.x build is fully supported.

### Fixed

- Nonsense-output guard: the local engine now rejects assistant-style
  replies, repetition loops, wrong-language answers, and meta text ("I
  can't translate", "Please contact us for assistance") instead of
  shipping them; rejected output follows the existing honest failure path
  (retry / cloud consent rules / clear notice). Works with any model,
  including models you add yourself.
- Repetition-penalty sampling is now actually applied during local
  translation (previously dropped silently when temperature was 0), which
  stops most loops before they are produced.
- Clipboard protection: a copy you make while a translation is in flight
  now survives (the app re-checks the clipboard before restoring and keeps
  your newest content); a failed backup can no longer wipe the clipboard;
  large (over 4 MiB) clipboard content is no longer destroyed.
- The floating badge repositions itself when you unplug a monitor or
  change display scale, so it can no longer be stranded off-screen.
- Engine-host privacy hardening: the app verifies the identity of the
  local translation engine before sending any text or token, closing a
  same-machine interception path.
- Installer hardening: elevated install and uninstall steps refuse to
  write through planted directory links, and the shared-store cleanup
  path is guarded too.
- Model verification now samples actual file content in its cache, raising
  the cost of tampering; the served-model label in diagnostics names the
  correct model (previously mislabeled as the speech model on the default
  route).
- Transient engine connection failures retry once as designed (a label
  mismatch previously disabled the retry entirely).
- Cloud clients now ask WinHTTP to refuse HTTP redirects (with a logged
  fallback when the policy cannot be applied), so request text and tokens
  are never replayed to a redirect target.
- Docs: README corrected on engine idle-exit semantics; the clipboard
  exposure window is now disclosed.

### Changed

- UI refresh: the badge, tooltip, and drag icon adopt the Emebala brand
  navy and gold "signet" design with improved contrast and typography.
- Tray convenience: the hover tip shows the active engine and language
  pair; the tip menu gains a Swap item for the drag pair; Ctrl+F9 shows an
  on-screen bubble naming the new target language; the pause bubble shows
  your actual configured hotkey instead of a hardcoded "F9".

### Known limitations (documented)

- Translated text is momentarily on the system clipboard during paste
  (~120 ms settle); Clipboard-History exclusion tags are advisory.
- The model-verification cache is a tamper-cost measure, not a security
  boundary against a same-user attacker.

[0.10.6]: https://github.com/teamsunplaza/Emebalachat/releases/tag/v0.10.6
[0.10.5]: https://github.com/teamsunplaza/Emebalachat/releases/tag/v0.10.5
[0.10.4]: https://github.com/teamsunplaza/Emebalachat/releases/tag/v0.10.4
