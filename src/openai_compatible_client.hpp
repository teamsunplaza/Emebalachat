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
    static std::wstring ChatCompletion(const OpenAiConfig& cfg,
                                       std::string_view source_lang,
                                       std::string_view target_lang,
                                       std::wstring_view text);
};

} // namespace emebalachat
