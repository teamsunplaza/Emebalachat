#pragma once

// ---------------------------------------------------------------------------
// worker_protocol — REQ-043 (M6 T3, design §1.2/§3.4, plan §V2-3): pure
// (Win32-free) parsers + builders for the SECOND frozen contract, the
// orchestrator <-> worker pipe ("난부 계약"). Header-only so unit tests run
// without spawning any process (R-6 adopted; design §9).
//
// Reuses the §4.3 frame/JSON primitives from engine_host_protocol.hpp
// UNMODIFIED: frames stay [u32 LE length][UTF-8 JSON], 1 MiB cap, written as
// ONE pipe message. The v1 client contract in that header is untouched — this
// is a NEW internal contract (design §1.1: "v1 §4 동결 무접촉").
//
// Methods (design §1.2, the second frozen contract):
//   worker -> orchestrator:
//     announce    {op:"announce", token, schema_version, family, engine,
//                  engine_version, ops[], abi_version, protocol_min,
//                  protocol_max}   (worker.manifest content + the boot-scoped
//                  token echoed back — security audit finding 2 (M6 T6): the
//                  orchestrator re-validates the token it issued on the
//                  command line against this field, constant-time; a MISSING
//                  or MISMATCHED token tears the session and the family goes
//                  unavailable. §V2-12-2 unknown-field-ignore means an OLD
//                  worker that omits `token` is rejected here by design.)
//     event       {op:"event","session":N,"kind":"partial|final|token|error|eos",
//                  "seq":M, "text":"...", "code":"...",
//                  "model":"..."}  (REQ-057 OPTIONAL served-model id echo,
//                                  emitted only when the sender populates it;
//                                  "" / omitted = pre-REQ-057 sender)
//     closed      {op:"closed","session":N}
//     heartbeat   {op:"heartbeat","ts":T}             (every 5 s; ReaperLoop
//                                                   15 s timeout judge)
//     shutdown_ack{op:"shutdown_ack"}                 (M-2: LAST frame before
//                                                   exit 0 — the orchestrator
//                                                   reads it BEFORE closing the
//                                                   pipe so no frame is lost;
//                                                   v1 measured DisconnectNamed
//                                                   Pipe frame-purge race,
//                                                   host_main.cpp:363-372)
//   orchestrator -> worker:
//     session_open{op:"session_open","session":N,"capability":"translate",
//                  "model_id":"...","profile":"..."}  -> opened | error
//     opened      {op:"opened","session":N}           (worker ack)
//     job         {op:"job","job":N,"session":N,"kind":"translate","src":..,
//                  "tgt":..,"text":.., "sampling":{temperature,top_p,top_k,
//                  rep_pen}}                          (one-shot shortcut: a
//                                                   session_open is NOT required)
//     close       {op:"close","session":N}
//     abort       {op:"abort","session":N}            -> worker cancel_flag
//                                                   store (D-2 chain b->c)
//     heartbeat   (worker also tolerates a received heartbeat: no-op)
//     shutdown    {op:"shutdown"}                     -> unload + shutdown_ack
//                                                   + exit 0 (M-2 order)
//     error       {op:"error","code":"..."}           (orchestrator-side errors)
//
// Unknown ops converge to an error frame + close (the §V2-12-2 bad_request
// rule generalized); unknown fields are IGNORED everywhere (§V2-12-2); every
// document carries a REQUIRED integer schema_version where applicable
// (worker.manifest); unknown event kinds fail to parse (fail-closed, mirrors
// the §4.4 status rule).
//
// Security invariants carried by this contract (design §10): user-only pipe
// SD (BuildUserOnlySd pattern), boot-scoped token passed on the command line
// (no token file -> nothing to clean up) AND echoed in the announce frame for
// orchestrator re-validation (audit finding 2), shape-only
// ENGINEHOST/worker/NNN logs only — user text never reaches a log from this
// layer.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine_host_protocol.hpp" // frozen §4.3 frame/JSON primitives (unmodified)

namespace emebalachat {
namespace workerproto {

// ---- frozen contract constants (worker.manifest §3.4 values) ---------------
inline constexpr int kWorkerSchemaVersion = 1;          // §3.4 schema_version
inline constexpr std::string_view kWorkerFamily = "ggml-translate";
inline constexpr std::string_view kWorkerEngine = "llama.cpp";
inline constexpr std::string_view kWorkerEngineVersion = "b6099"; // plan §V2-3 pin
inline constexpr int kWorkerAbiVersion = 1;
inline constexpr int kWorkerProtocolMin = 1;
inline constexpr int kWorkerProtocolMax = 2;

// Lifecycle constants (design §1.1/§1.2; R-4 adopted: hardcoded, not profiled).
inline constexpr int kWorkerHeartbeatIntervalMs = 5000;  // §1.2 heartbeat cadence
inline constexpr int kWorkerHeartbeatTimeoutMs = 15000;  // ReaperLoop judge
inline constexpr int kWorkerBackoffInitialMs = 1000;     // 1s -> 2s -> 4s ...
inline constexpr int kWorkerBackoffMaxMs = 30000;        // R-4 cap, hardcoded
inline constexpr int kWorkerReaperPollMs = 250;          // done_event poll
inline constexpr int kWorkerSpawnConnectTimeoutMs = 15000; // announce handshake

// Single-instance mutex per family (design §4.2 target note).
inline constexpr wchar_t kWorkerSingleInstanceMutexName[] =
    L"Local\\EmebalaEngine_Worker_ggml_translate";

// ---- event kinds (§1.2 event frame; unknown -> parse failure) --------------
enum class EventKind : unsigned char {
    Partial,
    Final,
    Token,
    Error,
    Eos,
};

inline std::string_view EventKindToString(EventKind k) {
    switch (k) {
        case EventKind::Partial: return "partial";
        case EventKind::Final:   return "final";
        case EventKind::Token:   return "token";
        case EventKind::Error:   return "error";
        case EventKind::Eos:     return "eos";
    }
    return {};
}

inline bool EventKindFromString(std::string_view s, EventKind& out) {
    if (s == "partial") { out = EventKind::Partial; return true; }
    if (s == "final")   { out = EventKind::Final;   return true; }
    if (s == "token")   { out = EventKind::Token;   return true; }
    if (s == "error")   { out = EventKind::Error;   return true; }
    if (s == "eos")     { out = EventKind::Eos;     return true; }
    return false; // unknown kind converges to failure (§V2-12-2)
}

// ---- worker.manifest (§3.4) ------------------------------------------------
struct WorkerManifest {
    int schema_version = 0;
    std::string family;
    std::string engine;
    std::string engine_version;
    std::vector<std::string> ops;
    int abi_version = 0;
    int protocol_min = 0;
    int protocol_max = 0;
};

// Load status mirroring the engine_host_manifest convention (T2): NotJson /
// SchemaVersion / Malformed are all fail-closed; Ok is the only pass.
enum class ManifestStatus : unsigned char { Ok, NotJson, SchemaVersion, Malformed };

// Serialize the manifest (identical bytes feed BOTH the deployed
// worker.manifest file content and the announce frame body — one builder, one
// shape; the installer deploys the same content the exe announces).
inline std::string BuildManifestJson(const WorkerManifest& m) {
    std::string ops = "[";
    for (std::size_t i = 0; i < m.ops.size(); ++i) {
        if (i) ops += ',';
        ops += std::string("\"") + enginehost::JsonEscape(m.ops[i]) + "\"";
    }
    ops += ']';
    return std::string("{\"schema_version\":") + std::to_string(m.schema_version) +
           ",\"family\":\"" + enginehost::JsonEscape(m.family) +
           "\",\"engine\":\"" + enginehost::JsonEscape(m.engine) +
           "\",\"engine_version\":\"" + enginehost::JsonEscape(m.engine_version) +
           "\",\"ops\":" + ops +
           ",\"abi_version\":" + std::to_string(m.abi_version) +
           ",\"protocol_min\":" + std::to_string(m.protocol_min) +
           ",\"protocol_max\":" + std::to_string(m.protocol_max) + "}";
}

// Parse a worker.manifest document (§3.4). REQUIRED: schema_version == 1,
// family/engine/engine_version strings, ops string array, the three ints.
// Unknown fields ignored (§V2-12-2). Damaged input never throws.
inline ManifestStatus ParseManifestJson(std::string_view json, WorkerManifest& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return ManifestStatus::NotJson;
    const auto* sv = enginehost::detail::FindField(p, "schema_version");
    if (!sv || !enginehost::detail::ParseInt(sv->text, m.schema_version)) {
        return ManifestStatus::SchemaVersion; // missing OR unparseable -> fail-closed
    }
    if (m.schema_version != kWorkerSchemaVersion) return ManifestStatus::SchemaVersion;
    const auto* fam = enginehost::detail::FindField(p, "family");
    if (!fam || !fam->is_string || fam->text.empty()) return ManifestStatus::Malformed;
    m.family = fam->text;
    const auto* eng = enginehost::detail::FindField(p, "engine");
    if (!eng || !eng->is_string || eng->text.empty()) return ManifestStatus::Malformed;
    m.engine = eng->text;
    const auto* engv = enginehost::detail::FindField(p, "engine_version");
    if (!engv || !engv->is_string || engv->text.empty()) return ManifestStatus::Malformed;
    m.engine_version = engv->text;
    const auto* ops = enginehost::detail::FindField(p, "ops");
    if (!ops || !enginehost::JsonParseStringArray(ops->text, m.ops)) {
        return ManifestStatus::Malformed;
    }
    const auto* abi = enginehost::detail::FindField(p, "abi_version");
    if (!abi || !enginehost::detail::ParseInt(abi->text, m.abi_version)) {
        return ManifestStatus::Malformed;
    }
    const auto* pmin = enginehost::detail::FindField(p, "protocol_min");
    if (!pmin || !enginehost::detail::ParseInt(pmin->text, m.protocol_min)) {
        return ManifestStatus::Malformed;
    }
    const auto* pmax = enginehost::detail::FindField(p, "protocol_max");
    if (!pmax || !enginehost::detail::ParseInt(pmax->text, m.protocol_max)) {
        return ManifestStatus::Malformed;
    }
    if (m.protocol_min > m.protocol_max || m.protocol_min < 1) {
        return ManifestStatus::Malformed; // inverted/absurd range is tampering
    }
    return ManifestStatus::Ok;
}

// The embedded manifest the worker exe announces (§3.4 values; the deployed
// worker.manifest is written from the same constants — keep CMake in sync).
inline WorkerManifest EmbeddedWorkerManifest() {
    WorkerManifest m;
    m.schema_version = kWorkerSchemaVersion;
    m.family = std::string(kWorkerFamily);
    m.engine = std::string(kWorkerEngine);
    m.engine_version = std::string(kWorkerEngineVersion);
    m.ops = {"translate"};
    m.abi_version = kWorkerAbiVersion;
    m.protocol_min = kWorkerProtocolMin;
    m.protocol_max = kWorkerProtocolMax;
    return m;
}

// ---- announce validation (§3.4 + §1.1 handshake rule) ----------------------
// The orchestrator checks the RUNNING worker's announce against (a) its own
// supported abi ceiling and (b) its own supported protocol range. A mismatch
// means a wrong deployment: NO respawn — family goes unavailable (design §1.1
// rule 2 / §3.4 note) and the condition is logged.
enum class AnnounceStatus : unsigned char {
    Ok,
    FamilyMismatch,
    AbiOutOfRange,
    ProtocolNoOverlap,
};

inline std::string_view AnnounceStatusToString(AnnounceStatus s) {
    switch (s) {
        case AnnounceStatus::Ok:                return "ok";
        case AnnounceStatus::FamilyMismatch:    return "family_mismatch";
        case AnnounceStatus::AbiOutOfRange:     return "abi_out_of_range";
        case AnnounceStatus::ProtocolNoOverlap: return "protocol_no_overlap";
    }
    return {};
}

struct AnnounceCheck {
    // orchestrator_abi_max: the highest worker abi this orchestrator speaks.
    // orch_proto_min/max: this orchestrator's supported protocol window.
    static AnnounceStatus Validate(const WorkerManifest& a,
                                   int orchestrator_abi_max,
                                   int orch_proto_min,
                                   int orch_proto_max) {
        if (a.family != std::string(kWorkerFamily)) return AnnounceStatus::FamilyMismatch;
        if (a.abi_version < 1 || a.abi_version > orchestrator_abi_max) {
            return AnnounceStatus::AbiOutOfRange;
        }
        // Ranges must overlap: a.worker_max >= orch_min && a.worker_min <= orch_max
        if (a.protocol_min > a.protocol_max ||
            a.protocol_max < orch_proto_min || a.protocol_min > orch_proto_max) {
            return AnnounceStatus::ProtocolNoOverlap;
        }
        return AnnounceStatus::Ok;
    }
};

// ---- announce token re-validation (security audit finding 2, M6 T6) ---------
// The orchestrator issued a 128-bit boot-scoped token on the worker command
// line. The worker echoes it in the announce frame; the orchestrator compares
// against its own issued value in CONSTANT TIME (audit recommendation). A
// missing or mismatched token means the connected party is not the worker we
// spawned (same-user pipe spoofing) — the caller tears the session and the
// family goes unavailable (no respawn: respawning cannot fix a spoofed peer).
inline bool AnnounceTokenMatches(std::string_view announced_token,
                                 std::string_view issued_token) {
    // Constant-time byte compare (no early exit on length/byte mismatch), so
    // the comparison leaks nothing about how many prefix bytes were right.
    std::size_t diff = announced_token.size() ^ issued_token.size();
    const std::size_t n = announced_token.size() < issued_token.size()
                              ? announced_token.size()
                              : issued_token.size();
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<std::size_t>(
            static_cast<unsigned char>(announced_token[i]) ^
            static_cast<unsigned char>(issued_token[i]));
    }
    return diff == 0;
}

// ---- worker-side message structs -------------------------------------------
struct SamplingParams {
    float temperature = 0.0f; // Hy-MT2 shipped defaults (host_main.py parity)
    float top_p = 0.6f;
    int top_k = 20;
    float rep_pen = 1.05f;
};

struct SessionOpenMsg {
    std::uint64_t session = 0;
    std::string capability;
    std::string model_id;
    std::string profile;
};

// One-shot translate shortcut (design §1.2: "단발 translate은 session_open
// 없이 job 프레임으로 단축 허용"). session may be 0 (sessionless job).
struct JobMsg {
    std::uint64_t job = 0;
    std::uint64_t session = 0;
    std::string src;
    std::string tgt;
    std::string text;
    SamplingParams sampling;
    bool sampling_present = false; // false -> use the shipped defaults
};

struct EventMsg {
    std::uint64_t session = 0;
    EventKind kind = EventKind::Error;
    std::uint64_t seq = 0;
    std::string text;                 // final/partial/token payload
    std::string code;                 // error kind: model_missing|engine_failed
    // REQ-057: OPTIONAL served-model id echo ("" = sender didn't populate —
    // the pre-REQ-057 wire shape). Registry-id metadata only (never user
    // content); carried on terminal events so a wrong-model serving issue is
    // diagnosable from the diagnostic log.
    std::string model;
};

struct AbortMsg { std::uint64_t session = 0; };
struct CloseMsg { std::uint64_t session = 0; };
struct HeartbeatMsg { std::uint64_t ts = 0; };
struct ErrorMsg { std::string code; };

// ---- builders --------------------------------------------------------------
// The token rides the announce frame (audit finding 2): the worker echoes the
// boot-scoped token the orchestrator passed on its command line. Empty token
// is rejected by the orchestrator (AnnounceTokenMatches vs the issued value).
inline std::string BuildAnnounce(const WorkerManifest& m, std::string_view token) {
    std::string body = BuildManifestJson(m);
    // Splice the op + token members in front: manifest bytes stay byte-identical
    // with the deployed file, and the announce frame additionally names its op
    // and echoes the token (the token field is announce-only, never in the
    // deployed worker.manifest document).
    return "{\"op\":\"announce\",\"token\":\"" + enginehost::JsonEscape(token) +
           "\"," + body.substr(1);
}

inline std::string BuildSessionOpen(std::uint64_t session, std::string_view capability,
                                    std::string_view model_id, std::string_view profile) {
    return std::string("{\"op\":\"session_open\",\"session\":") + std::to_string(session) +
           ",\"capability\":\"" + enginehost::JsonEscape(capability) +
           "\",\"model_id\":\"" + enginehost::JsonEscape(model_id) +
           "\",\"profile\":\"" + enginehost::JsonEscape(profile) + "\"}";
}

inline std::string BuildOpened(std::uint64_t session) {
    return std::string("{\"op\":\"opened\",\"session\":") + std::to_string(session) + "}";
}

inline std::string BuildJob(const JobMsg& m) {
    std::string s = std::string("{\"op\":\"job\",\"job\":") + std::to_string(m.job) +
                    ",\"session\":" + std::to_string(m.session) +
                    ",\"kind\":\"translate\",\"src\":\"" + enginehost::JsonEscape(m.src) +
                    "\",\"tgt\":\"" + enginehost::JsonEscape(m.tgt) +
                    "\",\"text\":\"" + enginehost::JsonEscape(m.text) + "\"";
    if (m.sampling_present) {
        s += ",\"sampling\":{\"temperature\":" + std::to_string(m.sampling.temperature) +
             ",\"top_p\":" + std::to_string(m.sampling.top_p) +
             ",\"top_k\":" + std::to_string(m.sampling.top_k) +
             ",\"rep_pen\":" + std::to_string(m.sampling.rep_pen) + "}";
    }
    s += "}";
    return s;
}

inline std::string BuildEvent(const EventMsg& m) {
    std::string s = std::string("{\"op\":\"event\",\"session\":") + std::to_string(m.session) +
                    ",\"kind\":\"" + std::string(EventKindToString(m.kind)) +
                    "\",\"seq\":" + std::to_string(m.seq);
    if (!m.text.empty()) {
        s += ",\"text\":\"" + enginehost::JsonEscape(m.text) + "\"";
    }
    if (!m.code.empty()) {
        s += ",\"code\":\"" + enginehost::JsonEscape(m.code) + "\"";
    }
    // REQ-057: the served-model echo is OPTIONAL — emitted only when the
    // sender populated it (old senders omit the member entirely; receivers
    // ignore unknown fields per §V2-12-2, so old<->new interop is unaffected).
    if (!m.model.empty()) {
        s += ",\"model\":\"" + enginehost::JsonEscape(m.model) + "\"";
    }
    s += "}";
    return s;
}

inline std::string BuildClosed(std::uint64_t session) {
    return std::string("{\"op\":\"closed\",\"session\":") + std::to_string(session) + "}";
}

inline std::string BuildClose(std::uint64_t session) {
    return std::string("{\"op\":\"close\",\"session\":") + std::to_string(session) + "}";
}

inline std::string BuildAbort(std::uint64_t session) {
    return std::string("{\"op\":\"abort\",\"session\":") + std::to_string(session) + "}";
}

inline std::string BuildHeartbeat(std::uint64_t ts) {
    return std::string("{\"op\":\"heartbeat\",\"ts\":") + std::to_string(ts) + "}";
}

inline std::string BuildShutdown() { return "{\"op\":\"shutdown\"}"; }

// M-2: the worker's LAST frame before exit 0 — the orchestrator waits for it
// (or pipe EOF) BEFORE closing, so no event frame can be purged unseen.
inline std::string BuildShutdownAck() { return "{\"op\":\"shutdown_ack\"}"; }

inline std::string BuildError(std::string_view code) {
    return std::string("{\"op\":\"error\",\"code\":\"") + enginehost::JsonEscape(code) + "\"}";
}

// ---- parsers (strict on the frozen shape; extras ignored per §V2-12-2) ------
// Parsed announce = manifest + the echoed token (audit finding 2). The token
// is announce-frame-only (never part of the deployed worker.manifest), so it
// is surfaced separately from the manifest body.
struct AnnounceMsg {
    WorkerManifest manifest;
    std::string token; // empty when the frame omitted the field (old worker)
};

inline bool ParseAnnounce(std::string_view json, AnnounceMsg& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "announce") return false;
    // The announce body IS a manifest document minus the op/token keys. The
    // manifest parser re-reads raw JSON text, so rebuild a manifest document
    // from the parsed pairs (field order irrelevant — key-based, §V2-12-2).
    std::string doc = "{";
    bool first = true;
    for (const auto& [k, v] : p) {
        if (k == "op" || k == "token") continue;
        if (!first) doc += ',';
        first = false;
        doc += "\"" + enginehost::JsonEscape(k) + "\":";
        if (v.is_string) {
            doc += "\"" + enginehost::JsonEscape(v.text) + "\"";
        } else {
            doc += v.text; // numbers/arrays keep raw source text (JsonValue contract)
        }
    }
    doc += "}";
    if (ParseManifestJson(doc, out.manifest) != ManifestStatus::Ok) return false;
    if (const auto* t = enginehost::detail::FindField(p, "token")) {
        if (!t->is_string) return false; // non-string token is malformed
        out.token = t->text;
    } else {
        out.token.clear();
    }
    return true;
}

// Back-compat convenience: parse the manifest only (token ignored). Kept so
// existing manifest-shape tests stay readable; token validation uses the
// AnnounceMsg overload above.
inline bool ParseAnnounce(std::string_view json, WorkerManifest& m) {
    AnnounceMsg msg;
    if (!ParseAnnounce(json, msg)) return false;
    m = msg.manifest;
    return true;
}

inline bool ParseSessionOpen(std::string_view json, SessionOpenMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "session_open") return false;
    const auto* s = enginehost::detail::FindField(p, "session");
    if (!s || !enginehost::detail::ParseUInt64(s->text, m.session)) return false;
    if (const auto* c = enginehost::detail::FindField(p, "capability")) {
        if (!c->is_string) return false;
        m.capability = c->text;
    }
    if (const auto* mi = enginehost::detail::FindField(p, "model_id")) {
        if (!mi->is_string) return false;
        m.model_id = mi->text;
    }
    if (const auto* pr = enginehost::detail::FindField(p, "profile")) {
        if (!pr->is_string) return false;
        m.profile = pr->text;
    }
    return true;
}

inline bool ParseOpened(std::string_view json, std::uint64_t& session) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "opened") return false;
    const auto* s = enginehost::detail::FindField(p, "session");
    if (!s || !enginehost::detail::ParseUInt64(s->text, session)) return false;
    return true;
}

namespace detail {

// Parse one float from a raw JSON number token (from_chars covers the strict
// subset the builder emits; locale-free by construction).
inline bool ParseFloat(std::string_view s, float& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    // std::from_chars for float is available on MSVC 19.24+ (project pins
    // VS2022 v143) — strict, locale-free, no errno games.
    const auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc() && r.ptr == end;
}

inline bool ParseSamplingObject(std::string_view raw, SamplingParams& out) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(raw, p)) return false;
    // Defaults first: a partial sampling object keeps shipped values for the
    // absent members (the request-override rule, plan §V2-5.2).
    out = SamplingParams{};
    if (const auto* t = enginehost::detail::FindField(p, "temperature")) {
        if (!ParseFloat(t->text, out.temperature)) return false;
    }
    if (const auto* tp = enginehost::detail::FindField(p, "top_p")) {
        if (!ParseFloat(tp->text, out.top_p)) return false;
    }
    if (const auto* tk = enginehost::detail::FindField(p, "top_k")) {
        if (!enginehost::detail::ParseInt(tk->text, out.top_k)) return false;
    }
    if (const auto* rp = enginehost::detail::FindField(p, "rep_pen")) {
        if (!ParseFloat(rp->text, out.rep_pen)) return false;
    }
    return true;
}

} // namespace detail

inline bool ParseJob(std::string_view json, JobMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "job") return false;
    const auto* j = enginehost::detail::FindField(p, "job");
    if (!j || !enginehost::detail::ParseUInt64(j->text, m.job)) return false;
    if (const auto* s = enginehost::detail::FindField(p, "session")) {
        if (!enginehost::detail::ParseUInt64(s->text, m.session)) return false;
    }
    const auto* kind = enginehost::detail::FindField(p, "kind");
    if (!kind || !kind->is_string || kind->text != "translate") return false;
    if (const auto* src = enginehost::detail::FindField(p, "src")) {
        if (!src->is_string) return false;
        m.src = src->text;
    }
    if (const auto* tgt = enginehost::detail::FindField(p, "tgt")) {
        if (!tgt->is_string) return false;
        m.tgt = tgt->text;
    }
    if (const auto* txt = enginehost::detail::FindField(p, "text")) {
        if (!txt->is_string) return false;
        m.text = txt->text;
    }
    if (const auto* sm = enginehost::detail::FindField(p, "sampling")) {
        if (!detail::ParseSamplingObject(sm->text, m.sampling)) return false;
        m.sampling_present = true;
    }
    return true;
}

inline bool ParseEvent(std::string_view json, EventMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "event") return false;
    const auto* s = enginehost::detail::FindField(p, "session");
    if (!s || !enginehost::detail::ParseUInt64(s->text, m.session)) return false;
    const auto* k = enginehost::detail::FindField(p, "kind");
    if (!k || !k->is_string || !EventKindFromString(k->text, m.kind)) return false;
    if (const auto* sq = enginehost::detail::FindField(p, "seq")) {
        if (!enginehost::detail::ParseUInt64(sq->text, m.seq)) return false;
    }
    if (const auto* t = enginehost::detail::FindField(p, "text")) {
        if (!t->is_string) return false;
        m.text = t->text;
    }
    if (const auto* c = enginehost::detail::FindField(p, "code")) {
        if (!c->is_string) return false;
        m.code = c->text;
    }
    if (const auto* md = enginehost::detail::FindField(p, "model")) {
        if (!md->is_string) return false; // REQ-057: mistyped optional member is malformed
        m.model = md->text;
    }
    return true;
}

inline bool ParseClosed(std::string_view json, std::uint64_t& session) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "closed") return false;
    const auto* s = enginehost::detail::FindField(p, "session");
    if (!s || !enginehost::detail::ParseUInt64(s->text, session)) return false;
    return true;
}

inline bool ParseClose(std::string_view json, std::uint64_t& session) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "close") return false;
    const auto* s = enginehost::detail::FindField(p, "session");
    if (!s || !enginehost::detail::ParseUInt64(s->text, session)) return false;
    return true;
}

inline bool ParseAbort(std::string_view json, AbortMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "abort") return false;
    if (const auto* s = enginehost::detail::FindField(p, "session")) {
        if (!enginehost::detail::ParseUInt64(s->text, m.session)) return false;
    }
    return true;
}

inline bool ParseHeartbeat(std::string_view json, HeartbeatMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "heartbeat") return false;
    if (const auto* t = enginehost::detail::FindField(p, "ts")) {
        if (!enginehost::detail::ParseUInt64(t->text, m.ts)) return false;
    }
    return true;
}

inline bool ParseShutdown(std::string_view json) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    return op && op->is_string && op->text == "shutdown";
}

inline bool ParseShutdownAck(std::string_view json) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    return op && op->is_string && op->text == "shutdown_ack";
}

inline bool ParseError(std::string_view json, ErrorMsg& m) {
    enginehost::JsonPairs p;
    if (!enginehost::JsonParseObject(json, p)) return false;
    const auto* op = enginehost::detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "error") return false;
    const auto* c = enginehost::detail::FindField(p, "code");
    if (!c || !c->is_string) return false;
    m.code = c->text;
    return true;
}

} // namespace workerproto
} // namespace emebalachat
