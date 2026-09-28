// ---------------------------------------------------------------------------
// engine_host_config_reader — REQ-046 P4-2 (Rev2 §B-4, Ask Light Gate C1):
// boot-time read of %LOCALAPPDATA%\Emebalachat\config.json's user_model_id,
// gated on engine_type == "user_gguf". Plus REQ-CP T2 (design §11 T2): the
// live per-client policy reader for the Common-store engine_client_policy.json.
// See the header for both contracts.
// ---------------------------------------------------------------------------

#include "engine_host_config_reader.hpp"

#include "engine_host_json_util.hpp" // REQ-043 frozen json primitives (reused)
#include "engine_host_paths.hpp"     // REQ-044/REQ-CP T2: paths::* store constants

#include <filesystem>
#include <mutex>
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
// REQ-051 U-1 FIX 3: this is the LEGACY full parse — kept verbatim (the
// req051_json_loader suite pins the BOM skip inside this function). The
// combined reader below delegates the id to it.
std::string LoadUserModelIdFromConfig(const std::wstring& lad_override) {
    const std::wstring lad = lad_override.empty() ? LocalAppDataDir() : lad_override;
    if (lad.empty()) return {};
    const std::filesystem::path path = std::filesystem::path(lad) / L"Emebalachat" / L"config.json";
    std::string text;
    if (engine_host_json::ReadTextFileUtf8(path, text) != engine_host_json::FileReadOutcome::Ok) {
        return {}; // absent/unreadable -> the pinned default (AC-4)
    }
    // REQ-051: defense-in-depth BOM skip on the reader entry point itself.
    // ReadTextFileUtf8 already stripped a file BOM, so this is normally a
    // no-op; it keeps the tolerance invariant local to the parse even if the
    // read layer ever changes (the 9cf0d40 lesson: a load-side rejection is
    // what enabled the config self-destruct loop).
    text = engine_host_json::SkipUtf8Bom(text);
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

// REQ-055: mtime/size-cached LIVE read of the C1-gated user model pin (see
// the header for the contract). The parse itself delegates to the legacy
// reader above, so the C1 gate / BOM skip / AC-4 fallbacks stay byte-identical;
// this wrapper only decides WHEN to re-parse. Attribute failure (absent /
// unreadable config) bypasses the cache with a fresh direct read, so a deleted
// file still answers "" and a later re-created file re-seeds normally.
std::string LoadUserModelIdFromConfigLive(const std::wstring& lad_override) {
    const std::wstring lad = lad_override.empty() ? LocalAppDataDir() : lad_override;
    if (lad.empty()) return {};
    const std::filesystem::path path = std::filesystem::path(lad) / L"Emebalachat" / L"config.json";

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return LoadUserModelIdFromConfig(lad); // absent/unreadable -> fresh "" answer
    }
    ULARGE_INTEGER mtime;
    mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    ULARGE_INTEGER fsize;
    fsize.LowPart = fad.nFileSizeLow;
    fsize.HighPart = fad.nFileSizeHigh;

    // Single-slot cache keyed by the RESOLVED lad: production has exactly one
    // config path, so the steady state is one GetFileAttributesEx per job and
    // zero re-parses; tests rotating temp dirs just re-read on every switch.
    static std::mutex live_mu;
    static std::wstring live_lad;
    static unsigned long long live_mtime = 0;
    static unsigned long long live_size = 0;
    static std::string live_id;

    std::lock_guard<std::mutex> lk(live_mu);
    if (live_lad == lad && live_mtime == mtime.QuadPart && live_size == fsize.QuadPart) {
        return live_id;
    }
    live_id = LoadUserModelIdFromConfig(lad);
    live_lad = lad;
    live_mtime = mtime.QuadPart;
    live_size = fsize.QuadPart;
    return live_id;
}

// REQ-046 P4-2 (Rev2 §B-4, C1) + REQ-051 U-1 FIX 3: the combined boot read —
// the C1-gated user_model_id (delegated to the legacy reader above, so its
// pinned shape stays intact) plus the diag_log_enabled opt-in for the
// orchestrator's diag FILE sink. The extra pass re-reads the same ~2 KB
// config.json once per boot; keeping it separate preserves the legacy reader
// byte-for-byte. The diag flag is read UNCONDITIONALLY (not gated on
// engine_type), mirroring the app's own diag_log_enabled semantics
// (config.cpp). Typed-JSON-bool discipline: a quoted "true" (is_string) is a
// schema error and stays false.
HostBootConfig LoadHostBootConfig(const std::wstring& lad_override) {
    HostBootConfig out;
    out.user_model_id = LoadUserModelIdFromConfig(lad_override);
    const std::wstring lad = lad_override.empty() ? LocalAppDataDir() : lad_override;
    if (lad.empty()) return out;
    const std::filesystem::path path = std::filesystem::path(lad) / L"Emebalachat" / L"config.json";
    std::string text;
    if (engine_host_json::ReadTextFileUtf8(path, text) != engine_host_json::FileReadOutcome::Ok) {
        return out; // absent/unreadable -> the pinned default (AC-4)
    }
    text = engine_host_json::SkipUtf8Bom(text);
    enginehost::JsonPairs fields;
    if (!enginehost::JsonParseObject(text, fields)) return out;
    const auto* dbg = enginehost::detail::FindField(fields, "diag_log_enabled");
    out.diag_log_enabled = dbg && !dbg->is_string && dbg->text == "true";
    return out;
}

// REQ-CP T2 (design §11 T2): mtime/size-cached LIVE read of the family-shared
// per-client engine policy file, resolved through the T1 pure core
// (clientpolicy::ResolvePolicy). Reuses the REQ-055 single-slot mtime/size
// cache pattern EXACTLY (engine_host_config_reader.hpp documents why), but the
// cached value is the raw DOCUMENT TEXT, not a resolution: `client_id` varies
// per job, so the cache must be content-level, not resolution-level — every
// call re-resolves the cached text for its own client id.
//
// Fail-closed, never throws: every failure/absence path answers the compiled-in
// safe defaults via clientpolicy::ResolvePolicy(client_id, "") — the same
// reject semantics the pure core applies to a missing/garbage document. The
// reader never creates the file or the Common dir (creation is T7's writer /
// installer concern); an absent store is a normal first-run state.
//
// Privacy: I/O + cache only, no log line here (shape-only codes are authored at
// the T3 dispatcher call site per the ENGINEHOST/<site>/NNN discipline).
clientpolicy::ClientPolicy LoadClientPolicyLive(const std::string& client_id,
                                                const std::wstring& lad_override) {
    const std::wstring lad = lad_override.empty() ? LocalAppDataDir() : lad_override;
    if (lad.empty()) return clientpolicy::ResolvePolicy(client_id, "").policy;

    const std::filesystem::path path = std::filesystem::path(lad) / L"Emebala" / L"Common" /
                                       paths::kClientPolicyJson;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        // Attribute failure (absent dir / unreadable) bypasses the cache with a
        // fresh fail-closed read, so a deleted file still answers defaults and a
        // later re-created file re-seeds — the REQ-055 live-pin discipline.
        return clientpolicy::ResolvePolicy(client_id, "").policy;
    }
    ULARGE_INTEGER mtime;
    mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    ULARGE_INTEGER fsize;
    fsize.LowPart = fad.nFileSizeLow;
    fsize.HighPart = fad.nFileSizeHigh;

    // CONTENT-level single-slot cache keyed by the RESOLVED lad dir (sibling to
    // the REQ-055 pin cache): production has exactly one policy path, so the
    // steady state is one GetFileAttributesEx per job and zero re-reads; tests
    // rotating temp dirs just re-read on every switch.
    static std::mutex policy_mu;
    static std::wstring policy_lad;
    static unsigned long long policy_mtime = 0;
    static unsigned long long policy_size = 0;
    static std::string policy_text;

    std::string text;
    {
        std::lock_guard<std::mutex> lk(policy_mu);
        if (policy_lad == lad && policy_mtime == mtime.QuadPart &&
            policy_size == fsize.QuadPart) {
            text = policy_text; // cache hit: reuse the cached CONTENT
        } else {
            // A torn write (partial content without the writer's tmp+rename) can
            // land here; ResolvePolicy is fail-closed on any malformed document,
            // so worst case is one job on defaults, never a crash or half-read.
            std::string fresh;
            if (engine_host_json::ReadTextFileUtf8(path, fresh) ==
                engine_host_json::FileReadOutcome::Ok) {
                policy_text = fresh;
                policy_lad = lad;
                policy_mtime = mtime.QuadPart;
                policy_size = fsize.QuadPart;
                text = policy_text;
            } else {
                text.clear(); // unreadable/locked -> fail-closed this call
            }
        }
    }
    // Resolve per request: the client id varies per job, so the cached CONTENT
    // (not a resolution) is what the cache holds. ResolvePolicy never throws.
    return clientpolicy::ResolvePolicy(client_id, text).policy;
}

} // namespace enginehost
} // namespace emebalachat
