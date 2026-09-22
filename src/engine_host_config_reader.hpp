// ---------------------------------------------------------------------------
// engine_host_config_reader — REQ-046 P4-2 (Rev2 §B-4, Ask Light Gate C1):
// the engine host's boot-time read of the Chat app's config.json.
//
// Extracted from host_main.cpp (previously the file-local
// LoadUserModelIdFromConfig) so the C1 gate logic is LINKABLE from
// run_tests.exe — host_main.cpp is the orchestrator's entry-point TU and is
// never linked into the test runner (Debug Tech Gate §2 조건-1).
//
// C1 contract: the user's model id is cached ONLY when
// config.json's engine_type == "user_gguf". ANY other value (including
// "local" with a stale user_model_id still persisted in config.json) keeps
// the returned id EMPTY, which keeps g_user_model_id == "" at the host so
// EnsureWorkerModelRelayed no-ops and the pinned Hy-MT2 path serves — the
// user's "로컬LLM Hy-MT2-1.8B는 터치하면 안 됨" contract (동결 계약 #11).
//
// REQ-051 U-1 FIX 3 (session 260922, live-bug U-1 triage): the SAME boot read
// also returns config.json's diag_log_enabled so the orchestrator can opt in
// to the diag FILE sink (%LOCALAPPDATA%\Emebalachat\logs\emebala_engine_
// <timestamp>.log, stem override via diag::SetLogFileStem). Default OFF — the
// privacy contract is unchanged (opt-in, shape-only ENGINEHOST/* lines only).
//
// Parsing reuses the frozen engine_host_json_util helpers (JsonParseObject /
// detail::FindField) — the same minimal-parse primitives the pre-REQ-046
// reader used; no new JSON surface. A leading UTF-8 BOM is skipped via the
// shared engine_host_json::SkipUtf8Bom (REQ-051) — BOM-prefixed config.json
// (Notepad / PowerShell edits) must read identically to the app's own
// BOM-free output. Boot-time only: the host caches the
// result once and never reloads (idle-exit respawn bounds staleness).
//
// Privacy: shape-only. Callers must NEVER log the returned id's content —
// the DIAG_LOG site logs its LENGTH plus a pinned/user_gguf discriminator.
// ---------------------------------------------------------------------------

#pragma once

#include <string>

namespace emebalachat {
namespace enginehost {

// REQ-051 U-1 FIX 3: the orchestrator's boot-time view of config.json — the
// C1-gated user model id (REQ-046 P4-2) plus the opt-in diag file flag.
struct HostBootConfig {
    // REQ-046 P4-2 (Rev2 §B-4, C1): config.json's user_model_id, returned
    // ONLY when engine_type == "user_gguf"; otherwise "" (pinned Hy-MT2).
    std::string user_model_id;
    // REQ-051 U-1 FIX 3: config.json's diag_log_enabled (typed JSON bool;
    // absent / wrong-typed -> false). Drives the orchestrator's diag FILE
    // sink opt-in; the shape-only line discipline is authored at every
    // ENGINEHOST/* call site and is unchanged by this flag.
    bool diag_log_enabled = false;
};

// REQ-051 U-1 FIX 3: combined boot read (ONE parse feeds both fields).
// `lad_override` injects the %LOCALAPPDATA% parent for tests (an empty
// string falls back to the real SHGetKnownFolderPath lookup — the
// production call site passes nothing, so its behavior is unchanged).
HostBootConfig LoadHostBootConfig(const std::wstring& lad_override);

// REQ-046 P4-2 (Rev2 §B-4): returns config.json's user_model_id ONLY when
// engine_type == "user_gguf"; otherwise "" (pinned Hy-MT2 path).
// `lad_override` injects the %LOCALAPPDATA% parent for tests (an empty
// string falls back to the real SHGetKnownFolderPath lookup — the
// production call site passes nothing, so its behavior is unchanged).
// REQ-051 U-1 FIX 3: retained single-field convenience wrapper over
// LoadHostBootConfig — the existing REQ-045/REQ-051 test suites resolve
// through it unchanged.
std::string LoadUserModelIdFromConfig(const std::wstring& lad_override);

// REQ-055: mtime/size-cached LIVE read of the C1-gated user model pin.
// LoadUserModelIdFromConfig re-parses on every call; the host dispachers
// call this per job, so cache on the config file's mtime+size signature
// (one GetFileAttributesEx per job, re-parse only on change). Single-slot
// cache keyed by the RESOLVED lad dir: production has one path (steady
// state = zero re-parses); tests with rotating temp dirs just re-read.
// Thread-safe (dispatchers run on multiple threads).
std::string LoadUserModelIdFromConfigLive(const std::wstring& lad_override);

} // namespace enginehost
} // namespace emebalachat

