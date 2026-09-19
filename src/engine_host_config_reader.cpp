// ---------------------------------------------------------------------------
// engine_host_config_reader — REQ-046 P4-2 (Rev2 §B-4, Ask Light Gate C1):
// boot-time read of %LOCALAPPDATA%\Emebalachat\config.json's user_model_id,
// gated on engine_type == "user_gguf". See the header for the contract.
// ---------------------------------------------------------------------------

#include "engine_host_config_reader.hpp"

#include "engine_host_json_util.hpp" // REQ-043 frozen json primitives (reused)
#include "engine_host_paths.hpp"     // REQ-044: paths::kModelsDirRel sibling constants

#include <filesystem>
#include <string>

#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath / FOLDERID_LocalAppData

namespace emebalachat {
namespace enginehost {
namespace {

// Mirrors the orchestrator's LocalAppDataDir() (host_main.cpp): the
// per-user known folder; "" when unavailable (the reader then bails to the
// pinned path).
std::wstring LocalAppDataDir() {
    PWSTR known = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        std::wstring out(known);
        ::CoTaskMemFree(known);
        return out;
    }
    return {};
}

} // namespace

// REQ-046 P4-2 (Rev2 §B-4, C1): cache user_model_id ONLY when
// engine_type=="user_gguf". Any other engine_type (including "local" with a
// stale user_model_id still in config.json) keeps the result "" so the
// host's g_user_model_id stays empty -> EnsureWorkerModelRelayed no-ops ->
// the pinned Hy-MT2 path serves (the user's "터치하면 안 됨" contract).
std::string LoadUserModelIdFromConfig(const std::wstring& lad_override) {
    const std::wstring lad = lad_override.empty() ? LocalAppDataDir() : lad_override;
    if (lad.empty()) return {};
    const std::filesystem::path path = std::filesystem::path(lad) / L"Emebalachat" / L"config.json";
    std::string text;
    if (engine_host_json::ReadTextFileUtf8(path, text) != engine_host_json::FileReadOutcome::Ok) {
        return {}; // absent/unreadable -> the pinned default (AC-4)
    }
    enginehost::JsonPairs fields;
    if (!enginehost::JsonParseObject(text, fields)) return {};
    // C1 gate: engine_type must be the exact string "user_gguf".
    const auto* et = enginehost::detail::FindField(fields, "engine_type");
    if (!et || !et->is_string) return {};      // absent -> pinned path
    if (et->text != "user_gguf") return {};    // "local"/"google"/"auto"/unknown -> pinned path
    const auto* v = enginehost::detail::FindField(fields, "user_model_id");
    if (!v || !v->is_string) return {};
    return v->text; // "" (or absent above) keeps the pinned path
}

} // namespace enginehost
} // namespace emebalachat
