// ---------------------------------------------------------------------------
// engine_host_components.cpp — REQ-043 (M6 T2, design §3.3): components.json
// parser + decision-rule-A helpers + partial-update merge. See the header.
// ---------------------------------------------------------------------------

#include "engine_host_components.hpp"

#include "diag_logger.hpp"

#include <cstdlib> // Backend P2-1: std::strtoull (numeric version segments)

namespace emebalachat {
namespace engine_host_components {

LoadResult ParseComponentsJson(std::string_view json) {
    LoadResult result;
    enginehost::JsonPairs doc;
    if (!enginehost::JsonParseObject(json, doc)) {
        result.status = LoadStatus::NotJson;
        return result;
    }

    // schema_version: REQUIRED == 1 (fail-closed, §V2-12-2).
    int schema = 0;
    if (!engine_host_json::GetInt(doc, "schema_version", schema) ||
        schema != kComponentsSchemaVersion) {
        result.status = LoadStatus::SchemaVersion;
        DIAG_F("ENGINEHOST/Components/001: components.json rejected "
               "(schema_version missing or unsupported, got=%d)\n", schema);
        return result;
    }
    result.file.schema_version = schema;

    // components: REQUIRED object of named entries.
    const enginehost::JsonValue* comps = enginehost::detail::FindField(doc, "components");
    if (!comps || comps->is_string || comps->text.empty() ||
        comps->text.front() != '{') {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Components/002: components.json rejected "
               "(components{} missing or wrong-typed)\n");
        return result;
    }
    enginehost::JsonPairs entries;
    if (!enginehost::JsonParseObject(comps->text, entries)) {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Components/003: components.json rejected "
               "(components{} not parseable)\n");
        return result;
    }
    for (const auto& [name, val] : entries) {
        if (val.is_string || val.text.empty() || val.text.front() != '{') {
            result.status = LoadStatus::Malformed;
            DIAG_F("ENGINEHOST/Components/004: components.json rejected (entry "
                   "'%s' is not an object)\n", name.c_str());
            result.file.components.clear();
            return result;
        }
        ComponentEntry entry;
        entry.name = name;
        entry.raw_json = val.text;
        enginehost::JsonPairs inner;
        if (!enginehost::JsonParseObject(val.text, inner)) {
            result.status = LoadStatus::Malformed;
            DIAG_F("ENGINEHOST/Components/005: components.json rejected (entry "
                   "'%s' not parseable)\n", name.c_str());
            result.file.components.clear();
            return result;
        }
        // version: optional string; abi_version: optional integer. An entry
        // with NEITHER cannot feed decision rule A -> reject the document
        // (fail-closed: the installer writes at least one of the two).
        if (const enginehost::JsonValue* v = enginehost::detail::FindField(inner, "version")) {
            if (!v->is_string) {
                result.status = LoadStatus::Malformed;
                DIAG_F("ENGINEHOST/Components/006: components.json rejected "
                       "(entry '%s' version wrong-typed)\n", name.c_str());
                result.file.components.clear();
                return result;
            }
            entry.version = v->text;
            entry.has_version = true;
        }
        if (const enginehost::JsonValue* a = enginehost::detail::FindField(inner, "abi_version")) {
            if (a->is_string || !enginehost::detail::ParseInt(a->text, entry.abi_version)) {
                result.status = LoadStatus::Malformed;
                DIAG_F("ENGINEHOST/Components/007: components.json rejected "
                       "(entry '%s' abi_version wrong-typed)\n", name.c_str());
                result.file.components.clear();
                return result;
            }
            entry.has_abi_version = true;
        }
        if (!entry.has_version && !entry.has_abi_version) {
            result.status = LoadStatus::Malformed;
            DIAG_F("ENGINEHOST/Components/008: components.json rejected (entry "
                   "'%s' has neither version nor abi_version)\n", name.c_str());
            result.file.components.clear();
            return result;
        }
        result.file.components.emplace(name, std::move(entry));
    }
    result.status = LoadStatus::Ok;
    return result;
}

// ---- rule A helpers (plan §V2-8.1) ------------------------------------------

namespace {

// Backend P2-1 (v0.10.5): dotted-INTEGER segment compare. The old plain
// lexicographic `bundled > installed` mis-orders every mixed-width pair
// ("0.9.0" > "0.10.4" was true, "0.10.10" > "0.10.9" was false — the 0.10.10
// component upgrade would have been refused). Numeric major.minor.patch(.build)
// segments fix both while keeping every equal-shape triple ordered exactly as
// before (the pinned release-train contract in
// m6_engine_host_manifest_components_tests.inc is unchanged). A non-numeric
// segment tail parses as its leading digits (strtoull), so "0.10.1-beta"
// compares as 0.10.1. True when bundled > installed.
bool VersionNewer(const std::string& bundled, const std::string& installed) {
    auto segments = [](const std::string& v, unsigned long long out[4]) {
        for (size_t k = 0; k < 4; ++k) out[k] = 0; // param decays to a pointer
        size_t pos = 0;                             // — index, do not range-for
        for (size_t i = 0; i < 4 && pos <= v.size(); ++i) {
            const size_t dot = v.find('.', pos);
            const std::string part =
                v.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            out[i] = std::strtoull(part.c_str(), nullptr, 10);
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    };
    unsigned long long b[4] = {}, n[4] = {};
    segments(bundled, b);
    segments(installed, n);
    for (size_t i = 0; i < 4; ++i) {
        if (b[i] != n[i]) return b[i] > n[i];
    }
    return false; // every segment equal -> not newer
}

} // namespace

ReplaceDecision NeedsComponentReplace(const ComponentsFile& file,
                                      const std::string& name,
                                      const BundleComponent& bundle) {
    const ComponentEntry* entry = file.Find(name);
    if (!entry) {
        // Rule A-1 (per component): absent data -> replace, no comparison.
        return ReplaceDecision::Replace;
    }
    // abi_version compared first (contract breakage beats cosmetic version).
    if (bundle.abi_version > 0) {
        if (!entry->has_abi_version || entry->abi_version < bundle.abi_version) {
            return ReplaceDecision::Replace;
        }
        if (entry->abi_version > bundle.abi_version) {
            return ReplaceDecision::Keep; // installed is NEWER than the bundle
        }
    }
    // abi equal (or bundle carries none): fall back to the version string.
    if (entry->has_version && !bundle.version.empty()) {
        if (VersionNewer(bundle.version, entry->version)) {
            return ReplaceDecision::Replace;
        }
        return ReplaceDecision::Keep;
    }
    // Nothing left to compare (e.g. entry carries only abi and it matches):
    // the deployment matches the bundle -> keep.
    return ReplaceDecision::Keep;
}

ReplaceDecision NeedsOrchestratorReplace(const ComponentsFile& file,
                                         const BundleComponent& orchestrator_bundle) {
    return NeedsComponentReplace(file, "orchestrator", orchestrator_bundle);
}

// ---- partial-update merge (design §6.3) --------------------------------------

bool SerializeWithComponents(const ComponentsFile& current,
                             const std::map<std::string, std::string>& updates,
                             std::string& out_json) {
    using enginehost::JsonEscape;
    // Validate every update payload up front: a raw '{...}' object only. The
    // caller must not produce a half-merged document on disk.
    for (const auto& [name, raw] : updates) {
        enginehost::JsonPairs probe;
        if (raw.empty() || raw.front() != '{' || !enginehost::JsonParseObject(raw, probe)) {
            DIAG_F("ENGINEHOST/Components/009: merge refused (invalid update "
                   "payload for '%s', len=%zu)\n", name.c_str(), raw.size());
            return false;
        }
    }
    out_json.clear();
    out_json += "{\"schema_version\":";
    out_json += std::to_string(kComponentsSchemaVersion);
    out_json += ",\"components\":{";
    bool first = true;
    auto append_entry = [&](const std::string& name, const std::string& raw) {
        if (!first) out_json += ',';
        first = false;
        out_json += '"';
        out_json += JsonEscape(name);
        out_json += "\":";
        out_json += raw;
    };
    // Pass 1: preserved entries (everything NOT touched by updates).
    for (const auto& [name, entry] : current.components) {
        if (updates.find(name) == updates.end()) {
            append_entry(name, entry.raw_json);
        }
    }
    // Pass 2: updated/new entries in map (alphabetical) order — a stable,
    // deterministic output for diffing and repeated merges.
    for (const auto& [name, raw] : updates) {
        append_entry(name, raw);
    }
    out_json += "}}";
    return true;
}

std::filesystem::path DefaultEngineDir() {
    std::filesystem::path dir;
    size_t len = 0;
    if (_wgetenv_s(&len, nullptr, 0, L"LOCALAPPDATA") == 0 && len > 1) {
        std::wstring v(len, L'\0');
        if (_wgetenv_s(&len, &v[0], len, L"LOCALAPPDATA") == 0) {
            while (!v.empty() && v.back() == L'\0') v.pop_back();
            if (!v.empty()) {
                dir = std::filesystem::path(v) / L"Emebala" / L"Common" / L"engine";
            }
        }
    }
    return dir;
}

LoadResult LoadDefaultComponents() {
    LoadResult result;
    const std::filesystem::path dir = DefaultEngineDir();
    if (dir.empty()) {
        result.status = LoadStatus::Missing;
        return result;
    }
    const std::filesystem::path path = dir / L"components.json";
    std::string text;
    switch (engine_host_json::ReadTextFileUtf8(path, text)) {
        case engine_host_json::FileReadOutcome::Ok:
            break;
        case engine_host_json::FileReadOutcome::Missing:
            result.status = LoadStatus::Missing;
            return result;
        case engine_host_json::FileReadOutcome::ReadError:
            result.status = LoadStatus::ReadError;
            DIAG_F("ENGINEHOST/Components/010: components.json exists but could "
                   "not be read (locked/permission)\n");
            return result;
    }
    return ParseComponentsJson(text);
}

} // namespace engine_host_components
} // namespace emebalachat
