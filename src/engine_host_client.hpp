#pragma once

// ---------------------------------------------------------------------------
// engine_host_client — REQ-043 reference client for the Emebala shared
// inference host (plan §5.1-3, §5.3, §5.4).
//
// SELF-CONTAINMENT CONTRACT (do not break): this header and its .cpp are
// copied VERBATIM into the other Emebala workspaces (Emebala_Listner,
// Emebala Reader, ...) which implement the same frozen protocol
// (plans/emebala-engine-host-shared-inference.md §4). The only permitted
// dependencies are Win32 + the C++ standard library — NEVER include any
// other project header here (no config.hpp / unicode_utils.hpp /
// diag_logger.hpp). JSON escaping/parsing is implemented locally, matching
// the semantics of emebalachat::AppendJsonUnicodeEscape (lone surrogates ->
// U+FFFD, permissive hex-prefix decode); the two implementations must stay
// wire-identical.
//
// Failure contract (REQ-043, plan §V2-8.6 — every failure converges to
// "return false"; the caller degrades to consent-gated cloud or the
// repair/unavailable-notice path — no embedded engine exists):
//   * host binary missing            -> false, err="no_host_binary" (SILENT)
//   * pipe connect refused/timeout   -> spawn (when enabled) -> 3 retries at
//                                       500 ms -> false, err="connect"
//   * token / version / pin mismatch -> false + ONE shape-only stderr line
//                                       (never the user's text)
//   * busy                           -> false immediately, err="busy"
//                                       (queue waiting is FORBIDDEN, UX rule)
//   * host dies mid-request          -> detected within timeout_ms -> false,
//                                       err="io"; the NEXT call respawns
//   * result status != ok            -> false, err=<status> (model_missing,
//                                       engine_failed, timeout, bad_request)
//
// Privacy: the client never logs user text; the single diagnostic line on
// handshake mismatch carries codes/lengths only. Cloud-consent policy stays
// entirely on the caller side (plan §6.5) — this client only ever talks to
// the local named pipe (loopback TCP/HTTP is forbidden, §6.3).
//
// Threading: TryTranslate calls are serialized by the caller's worker, but
// the implementation additionally guards its low-level session state with
// its own mutex, so concurrent callers cannot corrupt the pipe handle.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>

namespace emebalachat {

// REQ-043 persisted config block (config.json "engine_host"):
//   { "enabled": true, "spawn": true, "idle_exit_ms": 600000 }
// `idle_exit_ms` is the HOST-side idle-exit rule (plan §4.4); protocol v1
// carries no configuration channel, so the client persists it for
// observability/forward-compat and the host applies its own default (the
// value is read back when a future protocol version can push it).
struct EngineHostConfig {
    bool enabled = true;
    bool spawn = true;
    int idle_exit_ms = 600000;
};

namespace engine_host {

// REQ-043: the Hy-MT2 content pin the client checks against welcome's
// model_sha256 (plan §4.4: pin mismatch -> give up the service, fall back).
// This literal MUST stay in sync with kExpectedModelSha256 in src/engine.hpp
// (tests pin both to the same 64 lowercase hex chars).
inline constexpr std::string_view kExpectedModelSha256 =
    "5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4";

// The one-shot translation seam. src/tgt/text are UTF-8; text is the user's
// source (treated as opaque bytes — never logged). On success `out` holds
// the UTF-8 translation and true is returned; on ANY failure false is
// returned, `out` is empty, and `err_code` carries a stable machine token
// from the contract list above (callers may log it shape-only).
bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  std::string& out,
                  std::string& err_code);

// REQ-057: served-model-aware overload. Additionally reports the OPTIONAL
// served-model id echo from the v1 result frame: on success `served_model`
// holds the registry id the host's worker actually translated with ("" when
// the host omitted the member — a pre-REQ-057 host — or on failure).
// Diagnostic metadata only (a registry id, never user content); callers may
// compare it against the selected engine's expected model id.
bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  std::string& out,
                  std::string& err_code,
                  std::string& served_model);

// REQ-043 test seam (plan §9.1-2 wants the fallback matrix tested
// headlessly): overrides the three fixed deployment paths for THIS process.
// Any nullptr component keeps its production default. Testing only — the
// production values are the frozen contract:
//   pipe  = "\\.\pipe\emebala-engine-v1"
//   token = %LOCALAPPDATA%\Emebala\Common\engine\token
//   exe   = %LOCALAPPDATA%\Emebala\Common\engine\Emebala.Engine.exe
void SetPathsForTesting(const wchar_t* pipe_name,
                        const wchar_t* token_path,
                        const wchar_t* exe_path);

// REQ-043 (plan §5.1-2): the engine manager's local-availability gate. The
// shared host is the local serving source — Auto must route to it whenever
// the host binary is deployed. Cheap existence check only; every runtime
// failure still converges through the plan §5.4 matrix (spawn -> repair ->
// consent-gated cloud -> feature-unavailable notice, plan §V2-8.6).
bool IsHostBinaryPresent();

// REQ-043 migration helper (plan M1#2 companion): the pinned model's common
// location, UTF-8:
//   %LOCALAPPDATA%\Emebala\Common\models\Hy-MT2-1.8B-Q8_0.gguf
// Returns false only when the common dir cannot be resolved. Callers decide
// what to do with it (the engine follows a legacy-default model_path here
// when the old exe-relative copy no longer exists).
bool TryGetCommonModelPath(std::string& out_utf8);

// REQ-043 test seam companion: overrides the common-model path resolved by
// TryGetCommonModelPath for THIS process (testing only). nullptr/empty
// restores the production default.
void SetCommonModelPathForTesting(const wchar_t* model_path);

} // namespace engine_host
} // namespace emebalachat
