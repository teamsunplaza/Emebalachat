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
//                                       500 ms -> false, err="connect:<oserr>"
//                                       (R2-10: the ACTUAL token is
//                                       "connect:" + the last Win32 error as
//                                       decimal digits, e.g. "connect:2";
//                                       ClassifyEngineHostFailure reads the
//                                       digit tail as Transient - src/engine.hpp
//                                       R1-25. The pre-R2-10 comment said a
//                                       bare "connect", which no consumer ever
//                                       observed.)
//   * pipe peer is not the host exe  -> false, err="connect:peer" + ONE
//                                       shape-only stderr line (R1-7; nothing
//                                       is written to that peer, fail closed)
//   * token / version / pin mismatch -> false + ONE shape-only stderr line
//                                       (never the user's text)
//   * busy                           -> false immediately, err="busy"
//                                       (queue waiting is FORBIDDEN, UX rule)
//   * host dies mid-request          -> detected within timeout_ms -> false,
//                                       err="io"; the NEXT call respawns
//   * result status != ok            -> false, err=<status> (model_missing,
//                                       engine_failed, timeout, bad_request)
//
// MIRROR DUTY (R2-10): the SELECTION CONTRACT above and this failure contract
// are part of what the sibling workspaces copy verbatim. A change to either
// block must land in every sibling copy (Emebala_Listner, Emebala Reader, ...)
// in the same change - the wire may not drift, and neither may the tokens a
// retry classifier keys on.
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
//   { "enabled": true, "spawn": true, "idle_exit_ms": 600000,
//     "worker_idle_ms": 90000, "warmup": false }
// `idle_exit_ms` is the HOST-side idle-exit rule (plan §4.4); protocol v1
// carries no configuration channel, so the client persists it for
// observability/forward-compat and the host applies its own default (the
// value is read back when a future protocol version can push it).
// REQ-060 adds the translation-worker idle-eviction + warmup knobs, with
// different consumers. worker_idle_ms is a STARTUP-ONLY HOST read: the host
// loads it from config.json directly at boot (no wire channel; clamped
// 5000..86400000 with 0/negative -> 90000), so the client persists it for
// the host and for observability. warmup is NOT a host boot read — it is a
// CLIENT-side decision input: while true (with the block enabled and the
// local route active) the client holds a dedicated pipe connection whose
// hello carries warmup:true, and that hold is what tells the host to skip
// idle eviction for the family and to pre-spawn the translate worker.
// Neither knob is mutated after threads exist.
struct EngineHostConfig {
    bool enabled = true;
    bool spawn = true;
    int idle_exit_ms = 600000;
    // REQ-060: idle budget (ms) before the host evicts the translation worker
    // process (default 90000 = 90 s; parsed clamped 5000..86400000, with
    // 0/negative restoring the 90000 default policy).
    int worker_idle_ms = 90000;
    // REQ-060: client-side warmup request (default false), NOT a host boot
    // read. When true (and the block is enabled with the local route
    // active) the client holds a dedicated pipe connection whose hello
    // carries warmup:true; for as long as that connection lives the host
    // skips idle eviction for the translate family, asynchronously
    // pre-spawns the worker, and relays a background preload session_open
    // that loads the pinned default model before the first job (warmup off
    // keeps the legacy first-job load).
    bool warmup = false;
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

// ---- R1-7 (v0.10.4 sweep, session 261029_0001): pipe-SERVER identity gate ---
//
// Threat: the canonical pipe name is ownerless while the host idle-exits
// (default 10 min), so another local user account can create the name first and
// answer the hello/translate frames. The only pre-R1-7 check was the welcome's
// model_sha256 pin, which is a PUBLIC compiled-in constant the squatter can
// echo. The boot token was therefore disclosed pre-auth and every translated
// message was readable by the squatter.
//
// Rule (no wire change, names frozen): after a successful CreateFile on a pipe
// candidate and BEFORE the token is read or any frame is written, the client
// resolves the pipe server's process image and requires it to be exactly the
// host exe this client would spawn (ExePath(), i.e. the installed
// %LOCALAPPDATA%\Emebala\Common\engine\Emebala.Engine.exe, inside the
// user-only ACL'd profile). Both the one-shot and the REQ-060 warmup connect
// paths run through the same helper, so both inherit the gate. Fail closed: any
// API failure, unreadable peer, or path mismatch is a connect failure.
//
// PeerPathMatches: the pure comparison predicate (no syscalls, unit-testable).
// Both sides are normalized for the Win32 equivalences the OS itself applies:
// case (CompareStringOrdinal ignore-case), the \\?\ / \\?\UNC\ long-path
// prefix, forward slashes, duplicated separators, and trailing dots/spaces
// (which Win32 strips from file names). Nothing else is rewritten, so two
// different files can never compare equal; empty input never matches.
bool PeerPathMatches(std::wstring_view expected, std::wstring_view actual);

// PeerHandleIsExpectedHost: the handle-level gate the connect path applies
// (GetNamedPipeServerProcessId -> OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
// -> QueryFullProcessImageNameW -> PeerPathMatches). `pipe_handle` is a Win32
// HANDLE passed as void* so this self-contained header stays free of
// <windows.h> (widen it back with a static_cast). `expected_host_path` is the
// host exe path (ExePath()). Returns false on ANY failure — invalid handle,
// unreadable peer (e.g. another user's process: OpenProcess is denied), empty
// expected path, or a different image. Callers MUST treat false as "not the
// host" and close the handle without writing a byte to it.
bool PeerHandleIsExpectedHost(void* pipe_handle, const wchar_t* expected_host_path);

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
