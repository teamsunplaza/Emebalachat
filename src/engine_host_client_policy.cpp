// ---------------------------------------------------------------------------
// engine_host_client_policy.cpp — REQ-CP (session 260928_0001, design §4/§5,
// task T1): ClientPolicyResolver pure-core implementation. See the header for
// the schema/contract. DIAG discipline: shape-only lines (ids, reason codes) —
// the file sink itself stays opt-in (diag_logger REQ-201 default OFF).
// ---------------------------------------------------------------------------

#include "engine_host_client_policy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "diag_logger.hpp"
#include "engine_host_json_util.hpp" // REQ-043 frozen json primitives (reused)

namespace emebalachat {
namespace enginehost {
namespace clientpolicy {

namespace {

using FieldPtr = const enginehost::JsonValue*;

// The compiled-in known client ids (design §4 known-client registry). The
// reader only consults the document's sections; this set answers IsKnown-
// ClientId for documents that carry no section at all (a seeded file with two
// sections still leaves the third family KNOWN — its policy is the defaults
// by absence, not by unfamiliarity).
bool IsInKnownSet(std::string_view canonical) {
    return canonical == "emebala-chat" || canonical == "emebala-listner" ||
           canonical == "emebala-reader";
}

// Design §4 alias table: the ids a hello has historically carried -> the
// canonical id. The Listener's live hello id is the misspelled "emebala-
// listner" (engine_host_client.cpp kClientName — the typo is PRESERVED as the
// canonical id); the corrected spellings map onto it.
std::string_view MapLegacyAlias(std::string_view lowered) {
    if (lowered == "emebala-listener") return "emebala-listner"; // the typo-id is canonical
    if (lowered == "emebala-listner")  return "emebala-listner";
    if (lowered == "emebala-chat")     return "emebala-chat";
    if (lowered == "emebala-reader")   return "emebala-reader";
    return {}; // not a known alias
}

// Double field inside a raw object: the protocol JsonReader keeps numbers as
// RAW text, so parse through strtod with a full-consumption check (same
// helper shape as the registry parser's ParseRawDouble).
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

// Per-key tolerant sampling reader: every known key is OPTIONAL; a wrong-typed
// or unparsable key falls back to THAT KEY's default only (the rest of the
// section still applies — REQ-L22 per-key tolerance, design §5 rule 3). The
// output flags tell the caller whether ANY key was usable so a garbage
// sampling object does not light sampling_present.
struct SamplingRead {
    SamplingPolicy values;
    bool any_key_usable = false;
};

SamplingRead ParseSamplingObject(std::string_view raw) {
    SamplingRead out;
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw, p)) return out; // unbalanced/!object -> nothing

    if (FieldPtr v = enginehost::detail::FindField(p, "temperature")) {
        double d = 0.0;
        if (!v->is_string && ParseRawDouble(p, "temperature", d)) {
            out.values.temperature = static_cast<float>(d);
            out.any_key_usable = true;
        } // wrong-typed / unparsable -> that key's shipped default
    }
    if (FieldPtr v = enginehost::detail::FindField(p, "top_p")) {
        double d = 0.0;
        if (!v->is_string && ParseRawDouble(p, "top_p", d)) {
            out.values.top_p = static_cast<float>(d);
            out.any_key_usable = true;
        }
    }
    if (FieldPtr v = enginehost::detail::FindField(p, "top_k")) {
        if (!v->is_string) {
            int n = 0;
            if (enginehost::detail::ParseInt(v->text, n)) {
                out.values.top_k = n;
                out.any_key_usable = true;
            }
        }
    }
    if (FieldPtr v = enginehost::detail::FindField(p, "rep_pen")) {
        double d = 0.0;
        if (!v->is_string && ParseRawDouble(p, "rep_pen", d)) {
            out.values.rep_pen = static_cast<float>(d);
            out.any_key_usable = true;
        }
    }
    return out;
}

// Per-section parser. Unknown FIELDS inside the section are ignored (design §5
// forward-compat rule); every known field is optional and per-key tolerant.
// Returns false only when the section text is not a parseable object (the
// caller then treats the whole section as absent — per-KEY tolerance does not
// extend to a structurally broken section).
bool ParseClientSection(std::string_view raw_section, ClientPolicy& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw_section, p)) return false;

    // model_id: optional string; wrong-typed -> that key's default ("").
    if (FieldPtr v = enginehost::detail::FindField(p, "model_id")) {
        if (v->is_string) out.model_id = v->text;
    }
    // profile_name: optional string; wrong-typed -> that key's default ("").
    if (FieldPtr v = enginehost::detail::FindField(p, "profile_name")) {
        if (v->is_string) out.profile_name = v->text;
    }
    // sampling: optional object; present-but-partial applies per key, a
    // syntactically valid object with at least one usable key lights
    // sampling_present (design §5: a partial section is still a section).
    if (FieldPtr v = enginehost::detail::FindField(p, "sampling")) {
        if (!v->is_string && !v->text.empty() && v->text.front() == '{') {
            SamplingRead sr = ParseSamplingObject(v->text);
            if (sr.any_key_usable) {
                out.sampling = sr.values;
                out.sampling_present = true;
            }
        }
    }
    // prompt_template_ref: optional string; wrong-typed -> the compiled-in
    // default. A present empty string means "the caller's default" — the
    // dispatcher already falls back to kDefaultPromptTemplateRef on empty, so
    // normalize it here to keep ClientPolicy.prompt_template_ref non-empty.
    if (FieldPtr v = enginehost::detail::FindField(p, "prompt_template_ref")) {
        if (v->is_string && !v->text.empty()) out.prompt_template_ref = v->text;
    }
    return true;
}

} // namespace

std::string CanonicalizeClientId(std::string_view raw_id) {
    std::string lowered(raw_id);
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    // Trim surrounding whitespace first so " emebala-listner " canonicalizes.
    const auto first = std::find_if(lowered.begin(), lowered.end(), not_space);
    const auto last = std::find_if(lowered.rbegin(), lowered.rend(), not_space).base();
    if (first >= last) return {}; // empty / whitespace-only / garbage
    std::string trimmed(first, last);
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string_view alias = MapLegacyAlias(trimmed);
    return std::string(alias.empty() ? std::string_view(trimmed) : alias);
}

bool IsKnownClientId(std::string_view raw_id) {
    return IsInKnownSet(CanonicalizeClientId(raw_id));
}

ClientPolicy CompiledInDefaults(std::string_view canonical_id) {
    ClientPolicy out;
    out.client_id = std::string(canonical_id);
    out.model_id.clear();                      // "" = pinned bundled default
    out.profile_name.clear();                  // "" = model default
    out.sampling = SamplingPolicy{};           // 0.0 / 0.6 / 20 / 1.05
    out.sampling_present = false;              // the compiled-in tier is not a section
    out.prompt_template_ref = kDefaultPromptTemplateRef;
    return out;
}

ResolveResult ResolvePolicy(std::string_view client_id, std::string_view json_text) {
    ResolveResult result;
    const std::string canonical = CanonicalizeClientId(client_id);

    // Tolerate a leading UTF-8 BOM on the in-memory text too — every reader
    // entry point in the codebase routes through the shared SkipUtf8Bom so the
    // tolerance lives in exactly one place (REQ-051 lesson).
    json_text = engine_host_json::SkipUtf8Bom(json_text);

    enginehost::JsonPairs doc;
    if (!enginehost::JsonParseObject(json_text, doc)) {
        result.status = ResolveStatus::Defaults; // garbage / blank / non-object
        result.policy = CompiledInDefaults(canonical);
        return result;
    }

    // schema_version: REQUIRED, integer, must equal 1 (design §5 strict-
    // equality rule). Missing/!=1/mistyped rejects the WHOLE document —
    // fail-closed, every client gets the compiled-in defaults.
    int schema = 0;
    if (!engine_host_json::GetInt(doc, "schema_version", schema) ||
        schema != kClientPolicySchemaVersion) {
        result.status = ResolveStatus::Defaults;
        result.policy = CompiledInDefaults(canonical);
        DIAG_F("ENGINEHOST/ClientPolicy/001: engine_client_policy.json rejected "
               "(schema_version missing or unsupported, got=%d)\n", schema);
        return result;
    }

    // clients: REQUIRED object. Missing or wrong-typed is a document reject
    // (a version-1 file with no clients map carries no usable policy).
    std::string clients_raw;
    if (!engine_host_json::GetRawObject(doc, "clients", clients_raw)) {
        result.status = ResolveStatus::Defaults;
        result.policy = CompiledInDefaults(canonical);
        DIAG_F("ENGINEHOST/ClientPolicy/002: engine_client_policy.json rejected "
               "(clients object missing or wrong-typed)\n");
        return result;
    }

    // Locate the requested client's section. The section VALUE keeps its raw
    // balanced '{...}' text from JsonParseObject — the unknown fields inside
    // stay ignored here (the writer layer preserves them verbatim).
    enginehost::JsonPairs clients;
    if (!enginehost::JsonParseObject(clients_raw, clients)) {
        result.status = ResolveStatus::Defaults;
        result.policy = CompiledInDefaults(canonical);
        return result;
    }
    FieldPtr section = enginehost::detail::FindField(clients, canonical);
    if (!section || section->is_string || section->text.empty() ||
        section->text.front() != '{') {
        // A valid document without THIS client's section: the policy is the
        // compiled-in defaults, but the file itself is healthy — the status
        // distinguishes "no section" from "file unusable" for diagnostics.
        result.status = ResolveStatus::UnknownClient;
        result.policy = CompiledInDefaults(canonical);
        return result;
    }

    ClientPolicy policy = CompiledInDefaults(canonical);
    if (!ParseClientSection(section->text, policy)) {
        // Structurally broken section: per-key tolerance does not rescue an
        // unparseable object — treat the section as absent (defaults).
        result.status = ResolveStatus::UnknownClient;
        result.policy = CompiledInDefaults(canonical);
        DIAG_F("ENGINEHOST/ClientPolicy/003: client section '%s' structurally "
               "broken; serving compiled-in defaults (shape-only)\n",
               canonical.c_str());
        return result;
    }
    result.status = ResolveStatus::Ok;
    result.policy = std::move(policy);
    return result;
}

ResolvedPolicy MergeForRequest(const ClientPolicy& policy,
                               std::string_view override_model_id,
                               const SamplingPolicy* override_sampling,
                               bool override_sampling_present,
                               std::string_view override_template_ref) {
    ResolvedPolicy out;
    // Precedence (design §6): request > client profile > compiled-in defaults.
    // The ClientPolicy arriving here already carries tier-2/3 values, so a
    // non-empty request override wins field-by-field.
    out.model_id = override_model_id.empty() ? policy.model_id
                                              : std::string(override_model_id);
    if (override_sampling_present && override_sampling) {
        out.sampling = *override_sampling;
        out.sampling_present = true;
    } else {
        out.sampling = policy.sampling;
        out.sampling_present = policy.sampling_present;
    }
    out.prompt_template_ref = override_template_ref.empty()
                                  ? policy.prompt_template_ref
                                  : std::string(override_template_ref);
    return out;
}

} // namespace clientpolicy
} // namespace enginehost
} // namespace emebalachat
