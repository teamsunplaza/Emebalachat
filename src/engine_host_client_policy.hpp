#pragma once

// ---------------------------------------------------------------------------
// engine_host_client_policy — REQ-CP (session 260928_0001, design §4/§5,
// task T1): pure-logic resolver for the family-shared per-client engine
// policy file %LOCALAPPDATA%\Emebala\Common\engine_client_policy.json.
//
// Schema (design §5):
//   { "schema_version": 1,
//     "clients": { <client id>: { model_id, profile_name, sampling
//                   {temperature, top_p, top_k, rep_pen}, prompt_template_ref,
//                   ...unknown fields preserved-ignored... } } }
//
// Contract (all rules mirror the registry.json parser conventions):
//   * schema_version is REQUIRED and must equal 1 — a missing/other version
//     is DOCUMENT REJECTION (fail-closed, whole file ignored, compiled-in
//     defaults for every client id).
//   * clients is REQUIRED; each key is a client id. Unknown keys are IGNORED
//     by the resolver (forward compat — the writer layer preserves them).
//   * Per-section per-KEY tolerance (REQ-L22 style, NOT whole-section
//     rejection): a wrong-typed known key falls back to THAT KEY's compiled-in
//     default; the rest of the section still applies. Unknown fields inside a
//     section are ignored.
//   * sampling is OPTIONAL and PARTIAL: absent keys keep the shipped
//     defaults; sampling_present is true only when the section carried a
//     syntactically valid sampling object (even a partial one).
//   * prompt_template_ref is OPTIONAL; absent -> the compiled-in default.
//   * Identity canonicalization (design §4): trim + lowercase + the legacy
//     alias table (emebala-listener -> emebala-listner, the typo-id the
//     Listener actually sends). UNKNOWN client ids (after canonicalization)
//     resolve to the compiled-in safe defaults — fail-closed, never throws.
//   * NO file I/O in this module: the caller (T2 live reader) injects the
//     document text. Pure decision core, unit-testable, same discipline as
//     SchedQueue / the registry parser.
//
// Privacy: the policy file carries ids, numbers, and template refs only —
// NEVER user text. DIAG lines are shape-only (ids, reason codes).
// ---------------------------------------------------------------------------

#include <string>
#include <string_view>

namespace emebalachat {
namespace enginehost {
namespace clientpolicy {

// The one schema_version this resolver accepts (design §5 strict-equality
// check, same rule as registry.json §V2-12-2).
inline constexpr int kClientPolicySchemaVersion = 1;

// ---------------------------------------------------------------------------
// Compiled-in safe defaults (design §5 + §6 precedence tier 3).
//
// These are the shipped worker defaults the codebase already converges on:
//   * sampling 0.0 / 0.6 / 20 / 1.05 == worker_protocol.hpp SamplingParams{}
//     member initializers (the greedy Hy-MT2 shipped profile).
//   * prompt_template_ref "hymt2-official" == the existing Tencent-style
//     template every current golden test is pinned against (design §8.1).
//   * model_id "" == the pinned bundled default (REQ-057 ResolveBundledModelId;
//     the dispatcher's existing "" semantics — item-D behavior verbatim).
// ---------------------------------------------------------------------------
inline constexpr const char* kDefaultPromptTemplateRef = "hymt2-official";
inline constexpr float kDefaultTemperature = 0.0f;
inline constexpr float kDefaultTopP = 0.6f;
inline constexpr int kDefaultTopK = 20;
inline constexpr float kDefaultRepPen = 1.05f;

// Sampling policy for one translate job — field-for-field the same semantics
// as worker_protocol.hpp SamplingParams (host stamps it into JobMsg.sampling).
struct SamplingPolicy {
    float temperature = kDefaultTemperature;
    float top_p = kDefaultTopP;
    int top_k = kDefaultTopK;
    float rep_pen = kDefaultRepPen;
};

// One resolved client policy (design §4 ClientPolicy minus the raw passthrough
// — the raw section text is a writer-layer concern, not a resolver output).
struct ClientPolicy {
    std::string client_id;            // CANONICAL id ("" when unknown)
    std::string model_id;             // "" = pinned bundled default
    std::string profile_name;         // registry profile ref, "" = model default
    SamplingPolicy sampling;
    bool sampling_present = false;    // the section carried a valid sampling object
    std::string prompt_template_ref;  // resolved (never empty)
};

// What the dispatcher actually applies per translate job (design §4
// ResolvedPolicy): the caller layers its request/session overrides on top of
// this — precedence request > client profile > compiled-in defaults.
struct ResolvedPolicy {
    std::string model_id;             // "" = pinned bundled default
    SamplingPolicy sampling;
    bool sampling_present = false;
    std::string prompt_template_ref;
};

// Resolve result: status != Ok => policy is the compiled-in defaults for the
// requested id (fail-closed). A status is returned even on success so callers
// can distinguish "no section" from "section applied".
enum class ResolveStatus {
    Ok,               // the client's own section was found and applied
    Defaults,         // the file is unusable (any reject reason) — defaults
    UnknownClient,    // file valid, but no section for this client id
};

struct ResolveResult {
    ResolveStatus status = ResolveStatus::Defaults;
    ClientPolicy policy;
};

// ---- pure API (no I/O, never throws) --------------------------------------

// Canonicalize a hello client id: trim surrounding whitespace, lowercase, map
// legacy aliases (design §4 alias table). Returns the canonical id, or "" for
// an input that canonicalizes to nothing (empty/whitespace/garbage).
std::string CanonicalizeClientId(std::string_view raw_id);

// True when CanonicalizeClientId(raw_id) names a client the resolver knows
// (its own section exists in the document, OR the canonical id is in the
// compiled-in known set: emebala-chat / emebala-listner / emebala-reader).
// Unknown ids are NOT an error — they resolve to the safe defaults.
bool IsKnownClientId(std::string_view raw_id);

// Parse a policy document from an in-memory UTF-8 JSON string (a leading BOM
// is tolerated, mirroring every other reader entry point) and resolve the
// section for client_id. On ANY document-level reject (garbage, non-object,
// missing/!=1 schema_version) the result is Defaults with the compiled-in
// policy; on a valid document with no matching section the result is
// UnknownClient with the compiled-in policy. Never throws.
ResolveResult ResolvePolicy(std::string_view client_id, std::string_view json_text);

// Convenience: the compiled-in safe defaults under a canonical id (what every
// reject path returns). Exposed for the T3 wrapper and for tests.
ClientPolicy CompiledInDefaults(std::string_view canonical_id);

// Merge a caller-side request override on top of a resolved client policy —
// precedence request > client profile > compiled-in defaults (design §6).
// An empty override_model_id keeps the policy value; an absent sampling
// (override_sampling_present == false) keeps the policy sampling; an empty
// override_template keeps the policy ref. Pure, never throws.
ResolvedPolicy MergeForRequest(const ClientPolicy& policy,
                               std::string_view override_model_id,
                               const SamplingPolicy* override_sampling,
                               bool override_sampling_present,
                               std::string_view override_template_ref);

} // namespace clientpolicy
} // namespace enginehost
} // namespace emebalachat
