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

// REQ-046 P4-2 (Rev2 §B-4): returns config.json's user_model_id ONLY when
// engine_type == "user_gguf"; otherwise "" (pinned Hy-MT2 path).
// `lad_override` injects the %LOCALAPPDATA% parent for tests (an empty
// string falls back to the real SHGetKnownFolderPath lookup — the
// production call site passes nothing, so its behavior is unchanged).
std::string LoadUserModelIdFromConfig(const std::wstring& lad_override);

} // namespace enginehost
} // namespace emebalachat
