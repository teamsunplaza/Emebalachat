// ---------------------------------------------------------------------------
// engine_host_registry.cpp — REQ-043 (M6 T2, design §3.1): registry.json
// parser implementation. See the header for the schema/contract.
// DIAG discipline: shape-only lines (ids, counts, reason codes) — the file
// sink itself stays opt-in (diag_logger REQ-201 default OFF).
// ---------------------------------------------------------------------------

#include "engine_host_registry.hpp"

#include <cstdlib>

#include "diag_logger.hpp"

namespace emebalachat {
namespace engine_host_registry {

namespace {

// REQ-043 (M6 T2): local alias — the /W4 C4456 shadow warnings came from
// redeclaring `const JsonValue* v` inside sibling if-scopes where the
// unqualified name resolved to a shadowed earlier declaration. A local
// using-declaration keeps the call sites readable and shadow-free.
using FieldPtr = const enginehost::JsonValue*;


// Clamp helper (int-safe: out-of-range from_chars already failed before here).
int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Double field inside a raw profile object: the protocol JsonReader keeps
// numbers as RAW text, so parse through strtod with full-consumption check.
bool ParseRawDouble(const enginehost::JsonPairs& pairs, std::string_view key, double& out) {
    FieldPtr v = enginehost::detail::FindField(pairs, key);
    if (!v || v->is_string) return false;
    if (v->text.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(v->text.c_str(), &end);
    if (!end || end != v->text.c_str() + v->text.size()) return false;
    out = parsed;
    return true;
}

bool ParseResourceObject(std::string_view raw, ResourceProfile& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw, p)) return false;

    // vram_mb: optional, integer only, clamped.
    if (FieldPtr v = enginehost::detail::FindField(p, "vram_mb")) {
        if (v->is_string) return false; // wrong-typed value -> item rejected
        int n = 0;
        if (!enginehost::detail::ParseInt(v->text, n)) return false;
        out.vram_mb = ClampInt(n, 0, kClampVramMbMax);
    }
    // ctx: optional, integer only, clamped.
    if (FieldPtr v = enginehost::detail::FindField(p, "ctx")) {
        if (v->is_string) return false;
        int n = 0;
        if (!enginehost::detail::ParseInt(v->text, n)) return false;
        out.ctx = ClampInt(n, 0, kClampCtxMax);
    }
    // max_sessions: optional, integer only, clamped to >= 1.
    if (FieldPtr v = enginehost::detail::FindField(p, "max_sessions")) {
        if (v->is_string) return false;
        int n = 0;
        if (!enginehost::detail::ParseInt(v->text, n)) return false;
        out.max_sessions = ClampInt(n, kClampMaxSessionsMin, kClampMaxSessionsMax);
    }
    // residency/eviction: string enums with safe defaults; unknown VALUES
    // degrade to the safe default (ondemand/evict) — scheduling policy input,
    // not a security boundary. Wrong TYPES still reject the item.
    if (!engine_host_json::GetStringOr(p, "residency", "ondemand", out.residency)) return false;
    if (!engine_host_json::GetStringOr(p, "eviction", "evict", out.eviction)) return false;
    // priority: optional, integer only, clamped 0..9.
    if (FieldPtr v = enginehost::detail::FindField(p, "priority")) {
        if (v->is_string) return false;
        int n = 0;
        if (!enginehost::detail::ParseInt(v->text, n)) return false;
        out.priority = ClampInt(n, kClampPriorityMin, kClampPriorityMax);
    }
    return true;
}

bool ParseProfilesObject(std::string_view raw, std::map<std::string, SamplingProfile>& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw, p)) return false;
    for (const auto& [name, val] : p) {
        if (val.is_string) return false; // profiles values must be objects
        if (val.text.empty() || val.text.front() != '{') return false;
        SamplingProfile prof;
        prof.name = name;
        prof.raw_json = val.text;
        enginehost::JsonPairs inner;
        if (!enginehost::JsonParseObject(val.text, inner)) return false;
        // All numeric keys are OPTIONAL inside a profile (the worker supplies
        // its own defaults); a wrong-typed known key rejects the item. REQ-043
        // (M6 T2) root-cause fix: the checks below were written as "REQUIRED"
        // (ParseRawDouble returns false when the key is ABSENT), which
        // rejected every non-empty profile that carried a subset of the
        // sampling keys — the exact shape the installer writes
        // ({"temperature": 0.5} alone). Absent -> keep the struct default.
        if (FieldPtr v = enginehost::detail::FindField(inner, "temperature")) {
            if (v->is_string || !ParseRawDouble(inner, "temperature", prof.temperature)) {
                return false;
            }
        }
        if (FieldPtr v = enginehost::detail::FindField(inner, "top_p")) {
            if (v->is_string || !ParseRawDouble(inner, "top_p", prof.top_p)) return false;
        }
        if (FieldPtr v = enginehost::detail::FindField(inner, "top_k")) {
            if (v->is_string) return false;
            if (!enginehost::detail::ParseInt(v->text, prof.top_k)) return false;
        }
        if (FieldPtr v = enginehost::detail::FindField(inner, "rep_pen")) {
            if (v->is_string || !ParseRawDouble(inner, "rep_pen", prof.rep_pen)) return false;
        }
        if (!engine_host_json::GetStringOr(inner, "prompt_template_ref", "", prof.prompt_template_ref)) {
            return false;
        }
        out[name] = std::move(prof);
    }
    return true;
}

} // namespace

bool ParseModelItem(std::string_view raw_item, ModelEntry& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw_item, p)) return false;

    // id and family are REQUIRED strings (empty counts as missing — a model
    // without an id/family cannot be referenced by the scheduler or the
    // manifest check).
    if (!engine_host_json::GetString(p, "id", out.id) || out.id.empty()) return false;
    if (!engine_host_json::GetString(p, "family", out.family) || out.family.empty()) {
        return false;
    }

    // files[]: REQUIRED array of strings; every entry must be a bare filename
    // (they resolve against the fixed Common\models dir — a separator/escape
    // in the name is tampering, fail-closed for the item).
    std::string files_raw;
    if (!engine_host_json::GetRawArray(p, "files", files_raw)) return false;
    if (!enginehost::JsonParseStringArray(files_raw, out.files)) return false;
    if (out.files.empty()) return false;
    for (const auto& f : out.files) {
        if (!engine_host_json::IsBareFilename(f)) return false;
    }

    // capabilities[]: optional array of strings (absent -> empty).
    if (FieldPtr v = enginehost::detail::FindField(p, "capabilities")) {
        if (v->is_string) return false;
        if (!enginehost::JsonParseStringArray(v->text, out.capabilities)) return false;
    } else {
        out.capabilities.clear();
    }

    // origin: optional string; unknown values degrade to "user" (treated as
    // manifest-exempt by consumers — safe direction).
    if (!engine_host_json::GetStringOr(p, "origin", "user", out.origin)) return false;

    // resource: optional object (absent -> defaults); present-but-invalid
    // types reject the item.
    if (FieldPtr v = enginehost::detail::FindField(p, "resource")) {
        if (v->is_string) return false;
        if (!ParseResourceObject(v->text, out.resource)) return false;
    } else {
        out.resource = ResourceProfile{}; // defaults
    }

    // profiles: optional object of named sampling profiles.
    if (FieldPtr v = enginehost::detail::FindField(p, "profiles")) {
        if (v->is_string) return false;
        if (!ParseProfilesObject(v->text, out.profiles)) return false;
    } else {
        out.profiles.clear();
    }

    // lang_pairs[]: optional array of strings.
    if (FieldPtr v = enginehost::detail::FindField(p, "lang_pairs")) {
        if (v->is_string) return false;
        if (!enginehost::JsonParseStringArray(v->text, out.lang_pairs)) return false;
    } else {
        out.lang_pairs.clear();
    }

    return true;
}

LoadResult ParseRegistryJson(std::string_view json) {
    LoadResult result;
    enginehost::JsonPairs doc;
    if (!enginehost::JsonParseObject(json, doc)) {
        result.status = LoadStatus::NotJson;
        return result; // registry stays empty (fail-closed)
    }

    // schema_version: REQUIRED, integer, must equal 1 (§V2-12-2). Absence or
    // any other value rejects the whole document.
    int schema = 0;
    if (!engine_host_json::GetInt(doc, "schema_version", schema) ||
        schema != kRegistrySchemaVersion) {
        result.status = LoadStatus::SchemaVersion;
        DIAG_F("ENGINEHOST/Registry/001: registry.json rejected (schema_version "
               "missing or unsupported, got=%d)\n", schema);
        return result;
    }
    result.registry.schema_version = schema;

    // models[]: REQUIRED raw array; elements parsed individually. A single
    // bad item is SKIPPED (with a shape-only note) — one corrupt entry must
    // not take down every model on the machine; structurally valid entries
    // keep serving.
    std::string models_raw;
    if (!engine_host_json::GetRawArray(doc, "models", models_raw)) {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Registry/002: registry.json rejected (models[] missing "
               "or wrong-typed)\n");
        result.registry.models.clear();
        return result;
    }
    std::vector<std::string> items;
    if (!engine_host_json::SplitJsonArray(models_raw, items)) {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Registry/003: registry.json rejected (models[] not "
               "splittable)\n");
        result.registry.models.clear();
        return result;
    }
    result.registry.models.reserve(items.size());
    int rejected = 0;
    for (const auto& item : items) {
        ModelEntry entry;
        if (!ParseModelItem(item, entry)) {
            ++rejected;
            continue;
        }
        result.registry.models.push_back(std::move(entry));
    }
    if (rejected > 0) {
        DIAG_F("ENGINEHOST/Registry/004: %d model item(s) rejected by the "
               "registry parser (shape-only)\n", rejected);
    }
    result.status = LoadStatus::Ok;
    return result;
}

std::filesystem::path DefaultModelsDir() {
    std::filesystem::path dir;
    size_t len = 0;
    if (_wgetenv_s(&len, nullptr, 0, L"LOCALAPPDATA") == 0 && len > 1) {
        std::wstring v(len, L'\0');
        if (_wgetenv_s(&len, &v[0], len, L"LOCALAPPDATA") == 0) {
            while (!v.empty() && v.back() == L'\0') v.pop_back();
            if (!v.empty()) {
                dir = std::filesystem::path(v) / L"Emebala" / L"Common" / L"models";
            }
        }
    }
    return dir;
}

LoadResult LoadDefaultRegistry() {
    LoadResult result;
    const std::filesystem::path dir = DefaultModelsDir();
    if (dir.empty()) {
        result.status = LoadStatus::Missing;
        return result;
    }
    const std::filesystem::path path = dir / L"registry.json";
    std::string text;
    switch (engine_host_json::ReadTextFileUtf8(path, text)) {
        case engine_host_json::FileReadOutcome::Ok:
            break;
        case engine_host_json::FileReadOutcome::Missing:
            result.status = LoadStatus::Missing;
            return result;
        case engine_host_json::FileReadOutcome::ReadError:
            result.status = LoadStatus::ReadError;
            DIAG_F("ENGINEHOST/Registry/005: registry.json exists but could not "
                   "be read (locked/permission); serving an empty registry\n");
            return result;
    }
    return ParseRegistryJson(text);
}

} // namespace engine_host_registry
} // namespace emebalachat
