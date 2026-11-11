#pragma once

// ---------------------------------------------------------------------------
// openai_compatible_client — REQ-045 P4-3 (design §3b, item 3b): the OpenAI
// Compatible cloud engine. Two endpoints against a user-supplied base URL:
//   * ListModels(base_url, api_key)            -> GET {base}/v1/models
//   * ChatCompletion(base_url, key, model,...)  -> POST {base}/v1/chat/completions
//
// The WinHTTP layer clones google_translate.cpp's RAII ScopedHInternet +
// synchronous WinHttpOpen/Connect/OpenRequest/SendRequest/ReceiveResponse
// pattern (design §3b "F9"), so it runs on the pipeline worker thread exactly
// like the Google seam (TECH GATE Item 7/8: synchronous WinHTTP on the worker
// thread is UI-safe). REQ-052: the settings dialog's "fetch model list" no
// longer calls this on the GUI thread either — the dialog spawns a detached
// worker and the completion refills the combo on the GUI thread (the old
// synchronous GUI-thread call froze the dialog mid-typing; bounded by a 10 s
// budget). All ListModels callers therefore run off the GUI thread.
//
// This header also carries the SMALL, PURE security helpers (design §3b "API
// Key 보안"): base-URL policy, DPAPI protect/unprotect, SHA-256 integrity
// digest, and display masking. They are pure so the unit suite pins them
// headlessly. NEVER log the key or request/response bodies (shape-only).
//
// Privacy: https-only unless the user explicitly consented to plaintext http
// (http_consent_given). The API key is persisted DPAPI-protected, never in
// cleartext; it is unprotected only transiently in memory and zeroed after
// use (SecureZeroMemory).
// ---------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

namespace emebalachat {

// REQ-045 (design §3b): the persisted "openai" block of config.json. The key
// is stored DPAPI-protected (base64 blob) plus a SHA-256 integrity digest so
// a failed/tampered unprotect is detected at load time. Startup-only write +
// worker-thread read, so (mirroring cloud_fallback_enabled) it needs no mutex
// or Snapshot entry.
struct OpenAiConfig {
    std::string base_url;             // e.g. "https://api.openai.com" or "http://host:port"
    std::string model;                // chosen / directly-typed model id
    std::string api_key_dpapi;        // base64(CryptProtectData(api_key)), never cleartext
    std::string api_key_sha256;       // 64 lowercase hex chars of the cleartext key
    bool http_consent_given = false;  // explicit consent to send the key over plaintext http
};

// ---- Pure policy / security helpers (unit-testable headlessly) ----

// REQ-045 (design §3b): classifies a base URL's transport security.
//   * empty/invalid scheme         -> Invalid (reject)
//   * "https"                      -> Https (allowed unconditionally)
//   * "http"                       -> Http (allowed ONLY with consent)
// Case-insensitive scheme. Trailing path/query on the base is ignored for the
// classification; the client joins "/v1/..." onto the base.
enum class OpenAiUrlSecurity { Invalid, Https, Http };
OpenAiUrlSecurity ClassifyOpenAiBaseUrl(std::string_view base_url);

// REQ-045 (design §3b, User Decision 12:38): DPAPI-protect a UTF-8 API key and
// return the base64 of the CryptProtectData blob. Returns false on any crypto
// failure; on success `cleartext` is SecureZeroMemory'd. The cleartext is thus
// never retained by the caller after a successful protect.
bool ProtectOpenAiApiKey(std::string_view cleartext, std::string& out_dpapi_base64);

// Inverse of ProtectOpenAiApiKey. On success `out_cleartext` holds the key and
// the caller MUST SecureZeroMemory it after use. Returns false on unprotect
// failure (wrong user, corrupted blob).
bool UnprotectOpenAiApiKey(std::string_view dpapi_base64, std::string& out_cleartext);

// REQ-045 (design §3b, User Decision 12:38): SHA-256 of a memory buffer as 64
// lowercase hex chars (integrity digest of the API key). Uses Windows CNG
// (bcrypt) directly — engine_core's ComputeFileSha256 is NOT linked into the
// Chat exe, so a buffer variant lives here in core. Returns false on failure.
bool OpenAiSha256Hex(std::string_view data, std::string& out_hex);

// REQ-045 (design §3b, User Decision 12:38): display mask "abcdef***" for a
// key longer than 6 chars; keys of 6 or fewer chars are FULLY masked ("***")
// so no usable prefix of a short secret leaks. Pure.
std::string MaskOpenAiApiKey(std::string_view cleartext);

// ---- 261111_0001 U6 (v0.10.7, R1 P1-3) + R2-L/R2-M (261111_0001)
// + G-02/G-11 (261111_0001 round 2) --------
// external-provider failure detail --
//
// Sanitizes a provider error message for user-visible display. Pass order
// (G-02, the security invariant — see the .cpp): NORMALIZE first (drop
// Unicode bidi controls whole: U+202A-202E, U+2066-2069, U+200E-200F,
// U+061C, U+2060-2064, U+FEFF — display spoofing vectors, R2-M; plus the
// zero-width / invisible separators added by F-4 (261111_0001 R3):
// U+200B ZWSP, U+00AD SOFT HYPHEN, U+034F CGJ, U+180E MVS, which render as
// nothing and so could hide a key split from the mask pass; ZWNJ/ZWJ
// U+200C/200D stay — legitimate Persian/Indic text; collapse
// control whitespace to single spaces), THEN mask EVERY occurrence of the
// request's cleartext API key as "***" (masking after the only
// text-joining transforms means a control-interleaved key — e.g.
// "sk-sec<U+202E>ret" for key "sk-secret" — can never reassemble past it),
// and finally bound + trim. The bound is applied on WHOLE UTF-8 sequences
// so the cap can never split a multi-byte character into a replacement
// glyph (R2-L). Pure and unit-testable; the masking guarantee is pinned by
// the v107 suite, including the adversarial interleaved-key cases.
inline constexpr std::size_t kMaxOpenAiErrorDetailChars = 160;
std::string SanitizeOpenAiErrorDetail(std::string_view message,
                                      std::string_view api_key);

// Extracts the provider's error.message out of a non-2xx response body
// ({"error":{"message":"...", ...}}), falling back to a bounded raw prefix
// when the body is not that shape. Returns "" for an empty body. G-11
// (261111_0001 round 2): every return path stops on a UTF-8 boundary — an
// incomplete trailing sequence (from the 160-byte fallback cap or a body
// that ends mid-sequence) is dropped, so the extract -> sanitize chain can
// never surface a replacement glyph. Pure; never logs.
std::string ExtractOpenAiErrorMessage(std::string_view body);

// ---- 261111_0001 R3 (F-1/F-7): the /v1/models body walker ----
//
// Parses an OpenAI-style model list ({"object":"list","data":[{"id":"...",
// ...},...]}) into the id list. Exposed as a PURE seam (no WinHTTP involved)
// so the unit suite can pin the malformed-body walk the ListModels worker
// runs:
//   * F-1 (security): the walk must ALWAYS terminate. The array loop had no
//     progress guard, and SkipValue's scalar arm stops AT a close delimiter
//     without consuming it, so a provider 200 body like {"data":[} or
//     {"data":[123}]} left the cursor frozen and the detached ListModels
//     worker spun at 100% CPU forever (each retry leaking another thread).
//   * F-7: the collected list is BOUNDED — at most kMaxOpenAiModelIds ids of
//     at most kMaxOpenAiModelIdChars bytes each; collection stops cleanly at
//     the cap instead of letting a 10 MiB hostile body flood the settings
//     combo. Out-of-range ids are dropped (shape-only DIAG: length only).
// A non-object body, a truncated body and a body without "data" all return
// whatever ids were collected before the walk ended. Never logs content.
inline constexpr std::size_t kMaxOpenAiModelIds = 512;
inline constexpr std::size_t kMaxOpenAiModelIdChars = 256;
std::vector<std::string> ParseOpenAiModelsJson(std::string_view json);

// ---- WinHTTP client (synchronous; worker thread) ----

class OpenAiCompatibleClient {
public:
    // REQ-045 (design §3b): GET {base}/v1/models with Bearer auth. On success
    // returns the parsed model id list (may be empty). On HTTP/transport/
    // parse failure returns an empty vector — the caller (settings dialog)
    // falls back to direct model entry. The 10 s budget bounds the GUI-thread
    // call in the settings dialog.
    static std::vector<std::string> ListModels(const OpenAiConfig& cfg);

    // REQ-045 (design §3b): POST {base}/v1/chat/completions. Body:
    //   {"model":..,"messages":[{"role":"system","content":"Translate from X to Y."},
    //                           {"role":"user","content":text}],"temperature":0}
    // Returns choices[0].message.content on success, empty on any failure.
    // 1500-unit input cap + 10 MiB response cap mirror the Google policy.
    //
    // 261111_0001 U6 (v0.10.7, R1 P1-3) + R2-E/R2-P (261111_0001): MINIMAL
    // failure-detail extension — when the request fails (non-2xx HTTP
    // status, transport error, or an unparseable 200 body) and
    // out_error_detail is non-null, it receives a bounded, sanitized,
    // key-masked single-line cause ("HTTP 503: <provider message>", or
    // "HTTP 503" when the body carried no message, or "" on transport
    // failure / unparseable 200 — R2-E: no unlocalized English literals,
    // the localized notice already carries the message). The default
    // nullptr keeps every existing call site source-compatible. The same
    // sanitized cause is recorded in the process-wide
    // LastChatErrorDetail() slot, so UI surfaces can read it WITHOUT
    // growing the Translate() signature. R2-P: the masking happens inside
    // the single failure recorder, so every recorded detail is guaranteed
    // masked and bounded.
    static std::wstring ChatCompletion(const OpenAiConfig& cfg,
                                       std::string_view source_lang,
                                       std::string_view target_lang,
                                       std::wstring_view text,
                                       std::string* out_error_detail = nullptr);

    // 261111_0001 U6: the most recent ChatCompletion FAILURE cause (the
    // sanitized detail above), or "" when the last chat request succeeded
    // or none ran. Process-wide, mutex-guarded; written only by
    // ChatCompletion (Translate serializes requests on translate_mutex_,
    // so "last" is unambiguous for consumers). The API key can never
    // appear — the failure recorder masks it via SanitizeOpenAiErrorDetail
    // before recording.
    static std::string LastChatErrorDetail();
};

} // namespace emebalachat
