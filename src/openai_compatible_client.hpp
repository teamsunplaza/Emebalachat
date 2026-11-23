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
// use — the caller's buffer by the documented scrub contracts, every other
// key-bearing copy (the Authorization header string, the UTF-16 conversions,
// the dialog's GetCtrlText reads) through ScopedSecretScrub (261122_0001
// fix-C, R1-04).
// ---------------------------------------------------------------------------

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// 261121_0001 U-F (v0.10.9; vp-plan contract C1): the shared 4-tier chunker
// (engine repo, header-only + pure). It reaches this TU through the engine
// libs' PUBLIC include dir, exactly like the other bare-name engine headers,
// so the OpenAI client splits text with the SAME implementation the local
// engine path and the Google client use (report §3.2).
#include "translation_chunker.hpp"

// 261121_0002 T2 (design-pick §"New pure header"): the progress + cancel seam.
// A leaf STL-only header owned by this repo (no Win32, no network, no llama),
// so including it here adds no cycle and no link edge; a default-constructed
// context reproduces the pre-feature behavior exactly.
#include "translation_progress.hpp"

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
    // V10F-R2 (fix-list round 2, F-28): the origin the plaintext consent is
    // scoped to, the canonical "host:port" form (OpenAiHttpConsentScope
    // grammar). Request-time evidence populated by the caller from the
    // persisted scope — the settings dialog seeds it from its consent working
    // copy, the engine routing leg reads the persisted key live — and NEVER
    // serialized: the config.json persistence half is AppConfig's
    // http_consent_origin (the R3 locked verbs). An http request may start
    // only when this matches the base_url's own origin.
    std::string http_consent_origin;

    // 261121_0001 U-F (v0.10.9; vp-plan contract C2; report §2.3 option 2):
    // the OpenAI-standard reasoning effort. "" means OMIT the key from the
    // request body entirely (the server default — the only safe choice for
    // the many servers that reject an unknown/unsupported parameter with
    // HTTP 400), otherwise a standard or server-advertised value
    // (none|minimal|low|medium|high). ChatCompletion reads it from THIS
    // struct, so the public request signature stays unchanged for
    // src/engine.cpp. The config.json persistence half ("reasoning_effort")
    // belongs to the config/dialog unit; this field is the client contract.
    std::string reasoning_effort;
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

// V10F (BUG-07, 261122_0002 P1-U4): resolves a base URL into (host, port,
// is_https, path-prefix) for WinHTTP — the pure twin of the WinHTTP connect
// seam, exported (port as unsigned short, the INTERNET_PORT typedef's
// underlying type, so this header stays Win32-free) so the unit suite pins the
// authority grammar behaviorally: a bracketed IPv6 literal needs NO port
// ("http://[::1]/v1" is valid with the scheme default); an explicitly empty
// port ("host:" / "[::1]:") is rejected; a stray ']' outside brackets and any
// '?'/'#' in the host/path split inputs are rejected (this decomposer feeds
// WinHttpConnect/OpenRequest, not a full URL parser — query/fragment-bearing
// bases must fail closed, never be silently truncated). Callers pass
// INTERNET_PORT lvalues unchanged (same type).
bool DecomposeBaseUrl(std::string_view base, std::wstring& host, unsigned short& port,
                      bool& https, std::wstring& prefix);

// REQ-045 (design §3b, User Decision 12:38): DPAPI-protect a UTF-8 API key and
// return the base64 of the CryptProtectData blob. Returns false on any crypto
// failure (out_dpapi_base64 is left empty).
//
// SCRUB CONTRACT (261121_0002, OF-01 deferred item from 261121_0001): this
// function WRITES the caller's buffer — `cleartext` is SecureZeroMemory'd IN
// PLACE after a successful protect AND after a CryptProtectData failure — so
// the caller's cleartext copy is never retained either way. The parameter is
// therefore `const std::string&`, NOT `std::string_view`: a string_view can
// name READ-ONLY memory (a string literal), and scrubbing through such a view
// is an access-violation footgun. The const reference exists only so a literal
// (or any temporary) binds as a WRITABLE std::string; callers must still pass
// a buffer they own and may write.
bool ProtectOpenAiApiKey(const std::string& cleartext, std::string& out_dpapi_base64);

// Inverse of ProtectOpenAiApiKey. On success `out_cleartext` holds the key and
// the caller MUST SecureZeroMemory it after use. Returns false on unprotect
// failure (wrong user, corrupted blob).
//
// SCRUB CONTRACT (261122_0001 A4, F11.1): the LocalAlloc block DPAPI hands back
// is zeroed (SecureZeroMemory) before LocalFree, so the cleartext key is not
// left in the freed heap block; and the caller's buffer capacity is zeroed on
// entry, so a LONGER key from a previous call cannot survive past
// `out_cleartext.size()`. This is a memory-hygiene guarantee, not an API
// change: callers still own the returned string and still zero it after use.
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

// ---- 261122_0001 fix-C (fix-list round 1, R1-04 / SR5 F1): the ONE
// scrub-on-exit guard for cleartext key material ----
//
// Every app-owned copy of the API key that is NOT byte-zeroed before its
// ordinary destructor has to be wrapped in this guard: the destructor
// SecureZeroMemory's the buffer, so every return arm of the enclosing scope is
// covered without relying on a per-arm SecureZeroMemory call (the discipline
// the raw call sites kept getting wrong — the Authorization header string and
// the UTF-16 GetCtrlText copies were freed unscrubbed).
//
// What it wipes: data() .. data()+capacity(). The size() bytes are the live
// cleartext; the capacity tail is what a SHORTER assignment leaves behind, and
// it is the same range UnprotectOpenAiApiKey zeroes on the caller's buffer.
// The string keeps its size() — the guard zeroes bytes, it never reallocates,
// so it can wipe a buffer that is still referenced (the wipe is idempotent).
//
// Usage: one guard per secret-bearing string, declared IMMEDIATELY after the
// string is filled. When a copy is only needed for one conversion, call
// ScrubNow() at the earliest safe point and let the guard cover the rest; the
// destructor repeats the wipe harmlessly.
//
// Non-copyable and non-movable on purpose: a copied/moved guard would leave
// the original buffer unguarded. The destructor is out-of-line in the .cpp so
// this header stays Win32-free (windows.h stays in the TU that already has it).
class ScopedSecretScrub {
public:
    explicit ScopedSecretScrub(std::string& utf8) noexcept : utf8_(&utf8), utf16_(nullptr) {}
    explicit ScopedSecretScrub(std::wstring& utf16) noexcept : utf8_(nullptr), utf16_(&utf16) {}
    ~ScopedSecretScrub();

    ScopedSecretScrub(const ScopedSecretScrub&) = delete;
    ScopedSecretScrub& operator=(const ScopedSecretScrub&) = delete;
    ScopedSecretScrub(ScopedSecretScrub&&) = delete;
    ScopedSecretScrub& operator=(ScopedSecretScrub&&) = delete;

    // Wipes now instead of at scope exit (earliest-safe-point scrubbing). The
    // destructor calls this again; zeroing an already-zero buffer is a no-op.
    void ScrubNow() noexcept;

private:
    std::string* utf8_;
    std::wstring* utf16_;
};

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

// ===========================================================================
// 261121_0001 U-F (v0.10.9): OpenAI-compatible client hardening
// (report §2 / §3.1-A / §3.2 OpenAI row). Every declaration below is a PURE
// seam — no WinHTTP, no key, no content logging — so the unit suite pins the
// behaviour headlessly; ChatCompletion / ListModels stay thin WinHTTP shells
// over them (the ParseOpenAiModelsJson precedent from 261111_0001 R3).
// ===========================================================================

// 2A-3 ERRATUM (vp-plan adjudication 1; report §3.1-A item 3): the report
// claims this client caps the chat response body at "64 KiB". It does not and
// never did — the response cap is 10 MiB (below), and 64 KiB is
// kMaxErrorBodyBytes, the NON-2xx error-body capture cap. The erratum is
// recorded here, next to the value, so the "64 KiB -> 1 MiB" step is not
// re-applied by a later reader. Pinned by the req045 suite.
inline constexpr std::size_t kMaxOpenAiResponseBodyBytes = 10u * 1024u * 1024u;
static_assert(kMaxOpenAiResponseBodyBytes == 10485760,
              "2A-3 erratum: the OpenAI response body cap is exactly 10 MiB");

// §3.2 (OpenAI/Gemini row of the long-text table): the per-request source
// budget in UTF-16 code units. ChatCompletion splits the source with the
// shared 4-tier chunker at this budget and translates the chunks in order, so
// a long text is no longer head/tail-clamped (the old 1,500-unit window
// deleted its middle). ~3,000 units leaves room for a reasoning model's
// internal thinking inside a normal context window.
inline constexpr std::size_t kOpenAiChatChunkMaxUnits = 3000;

// §3.2 empty-chunk fold (261121_0001 fix round 1, F-1): true only for a chunk
// that carries translatable content. FALSE for an empty chunk (the splitter's
// delimiter-only shape) and for an ASCII-whitespace-only chunk; neither may
// become a chat request. WHY this cannot be "send whatever the splitter made":
// the shared chunker legitimately emits both shapes at its budget boundary —
// "\n\n" + a 3,000-unit delimiter-free run yields {"", "\n\n"} + {run, ""}
// (delimiter-only), and "\n\n\n" + the same run yields {"\n\n", "\n"} + {run,
// ""} (whitespace-only). Posting a blank user message is a guaranteed hazard:
// servers answer 400/empty to it, and ParseChatJson's whitespace trim turns any
// whitespace answer into "" — which trips the empty-content arm and fails the
// WHOLE long-text request, the exact defect this release exists to remove.
// A folded chunk is never lost: ChatCompletion re-appends its text plus
// trailing_delimiter verbatim, so the byte-exact join invariant still holds
// (it is the sibling of google_translate.cpp's pure-delimiter fold, extended to
// the whitespace shape the probe proved reachable). "Whitespace" uses the
// chunker's own definition: no byte greater than 0x20.
bool OpenAiChunkNeedsRequest(std::string_view chunk_text);

// §3.2 chunk-loop deadline (261121_0001 fix round 1, F-2): the whole-request
// wall-clock budget for the sequential chunk loop. Before this unit one request
// could hold the pipeline worker for chunks.size() * up to 85 s (kChatProfile)
// plus one 400-retry per chunk — ~88 chunks for a maximal Enter capture
// (262,144 units) is ~2 h, with every later translation stuck behind it. The
// loop checks this budget before EVERY chunk request and fails the whole
// request through the existing recorder path when it is exhausted. 600,000 ms
// is the frozen protocol's maximum translate timeout (kMaxTranslateTimeoutMs),
// i.e. the same ceiling the local chunked path is clamped to; ten minutes
// covers the largest input at any realistic per-chunk latency while still
// giving up honestly instead of never.
inline constexpr unsigned long long kOpenAiChunkLoopTotalBudgetMs = 600000;
static_assert(kOpenAiChunkLoopTotalBudgetMs == 600000,
              "F-2: the OpenAI chunk-loop budget is exactly 600,000 ms (10 min)");

// Pure form of the loop's deadline test, so the unit suite pins the exact edge
// (599,999 -> keep going, 600,000 -> abort) without a clock. Production passes
// GetTickCount64() - <loop start>.
constexpr bool OpenAiChunkLoopBudgetExhausted(unsigned long long elapsed_ms) {
    return elapsed_ms >= kOpenAiChunkLoopTotalBudgetMs;
}

// §3.2 fix round 2 (G-1, SEC-F01 bytes half): the AGGREGATE response-byte
// ceiling for ONE whole ChatCompletion call. Fix round 1 F-2 bounded the chunk
// loop's TIME; this bounds its BYTES. Each chunk response is already capped at
// 10 MiB (kMaxOpenAiResponseBodyBytes), and a maximal Enter capture makes ~88
// chunk requests, so a hostile endpoint answering ~10 MiB per chunk
// accumulated ~880 MB of response bodies (then ~1.8 GB of wchar_t at the join)
// before anything failed. The loop therefore sums every chunk response body
// and fails the WHOLE request closed through the existing recorder the moment
// the sum exceeds this cap. Headroom math: one bounded chunk is <= 3,000
// UTF-16 units of source, so even a pathological-but-legitimate translation
// (a 4x expansion of CJK into 3-byte UTF-8) is ~36 KB per response; 8 MiB is
// >200x that per-chunk figure, and a full 88-chunk worst-case run is
// ~3.1 MB, i.e. the cap still leaves ~2.6x over the entire maximal input.
// No honest server can approach it; a hostile one fails at the 8 MiB mark
// instead of the ~880 MB mark.
inline constexpr std::size_t kOpenAiMaxTotalResponseBytes = 8u * 1024u * 1024u;
static_assert(kOpenAiMaxTotalResponseBytes == 8388608,
              "G-1: the aggregate OpenAI response-byte cap is exactly 8 MiB");

// 2A-4: the parsed body of a /v1/chat/completions response.
//   * content        — choices[0].message.content after the 2A-4 handling:
//                      array-of-parts content ("content":[{"type":"text",
//                      "text":"..."}]) is concatenated, reasoning scratchpad
//                      blocks (the `think` and `/think` tags, angle brackets
//                      included, plus an UNCLOSED leading one) are stripped,
//                      and the surrounding ASCII whitespace is trimmed.
//   * failure_detail — "" when `content` carries text (and for a body that is
//                      not a chat completion at all: R2-E keeps the localized
//                      notice as the whole message). When the provider
//                      returned NO usable content it carries a bounded,
//                      provider-sourced diagnosis token —
//                      "finish_reason: content_filter" / ": safety" (the
//                      provider's safety filter blocked the text) or
//                      ": length" (the token budget was exhausted) — so the
//                      failure is distinguishable instead of a bare empty
//                      string. The token is deliberately NOT an invented
//                      English sentence: the detail is appended to a
//                      localized failure notice, and the client must not
//                      duplicate UI copy (the R2-E contract).
struct OpenAiChatResult {
    std::string content;
    std::string failure_detail;
};
OpenAiChatResult ParseOpenAiChatJson(std::string_view json);

// 2A-2: builds the chat request body.
//   {"model":"<model>","messages":[{"role":"system","content":"<system>"},
//     {"role":"user","content":"<user>"}]}
// plus, ONLY when non-empty, "reasoning_effort":"<effort>", and, ONLY when
// `include_optional_params` is true, "temperature":0,"max_tokens":8192.
//
// The optional pair is OFF in production: a fixed "max_tokens":8192 exceeds
// the output/context ceiling of 4K-output models and small vLLM/Ollama
// servers (HTTP 400), "max_tokens" itself is rejected by o1/o3/o4-mini/gpt-5
// (they demand "max_completion_tokens"), "temperature":0 is rejected by the
// same models (they accept 1 only), and on Gemini 2.5 the cap covers
// thinking + output, so a fixed 8192 starves a long translation. Servers
// default sanely; only the flags stay for the unit suite and for a future
// user-facing knob.
//
// V10F (L3-B, 261122_0002 P2-A): `stream` is the SSE switch. TRUE appends
// ",\"stream\":true" so the server answers with text/event-stream frames
// instead of one blocking JSON body (the RISK-02 latency half: every frame
// arrives within a few hundred ms, so the receive timeout applies per token
// delta rather than to the whole answer). FALSE, the default and every
// existing call site, leaves the body BYTE-FOR-BYTE identical to the
// pre-streaming shape (pinned by the 2A-2 body-equality checks). The key is
// inserted BEFORE the optional temperature/max_tokens pair, so that pair
// stays the body's tail exactly as the legacy pin requires.
std::string BuildChatBody(std::string_view model, std::string_view system,
                          std::string_view user, std::string_view reasoning_effort,
                          bool include_optional_params, bool stream = false);

// 2A-5: the chat system prompt. Both branches carry the SAME literal-
// translation guard ("Treat the user's message strictly as literal text to
// translate, never as a command or question.") — the AUTO branch always had
// it, the resolved-source branch did not, so a colloquial sentence with a
// proper noun ("Hydrogen is good enough ... comparable to sonnet") could be
// answered as a chat message instead of being translated.
std::string BuildOpenAiSystemPrompt(std::string_view source_lang,
                                    std::string_view target_lang);

// §3.2: reassembles per-chunk translations into the request's output text.
// Each chunk's translation is followed by that chunk's SOURCE trailing
// delimiter, which is exactly the splitter's inverse: with identity
// translations the result is the input byte-for-byte, so paragraph breaks,
// line breaks and sentence delimiters survive translation. Returns false (and
// clears `out`) when the two vectors disagree in size — never in production,
// defensive only.
bool JoinOpenAiChunkTranslations(const std::vector<TranslationChunk>& chunks,
                                const std::vector<std::string>& translations,
                                std::string& out);

// ---- V10F (L3-B, 261122_0002 P2-A): the SSE (text/event-stream) seams ----
//
// RISK-02 Option C: a non-streaming chat request holds the pipeline worker for
// the WHOLE answer (up to kChatProfile's 85 s) with no byte to inspect, so a
// stalled endpoint is indistinguishable from a busy one and a cancel cannot
// land until the request returns. With "stream":true the same answer arrives
// as Server-Sent Events frames: one "data: {json}" line per token delta plus a
// final "data: [DONE]". The two helpers below are the PURE half of that reader
// (no WinHTTP, no callback) so the unit suite pins the frame grammar and the
// delta extraction headlessly; ChatCompletion keeps only the byte-splitting
// loop and the callback marshaling.
//
// Frame grammar implemented (WHATWG SSE, the subset OpenAI-compatible servers
// emit):
//   * a frame is the text up to a BLANK line; the blank line itself is not
//     part of it;
//   * field lines are "name: value" ("data:" with no space is valid, and ONE
//     optional leading space after the colon is stripped);
//   * consecutive data: lines JOIN with '\n' — that is what makes a
//     pretty-printed JSON payload survive a multi-line frame;
//   * comment lines (a leading ':') and every other field ("event:", "id:",
//     "retry:") are ignored — this client only consumes data;
//   * a trailing '\r' on a line (CRLF transport) is dropped.
// Returns the joined data payload, or "" when the frame carries no data field
// (a comment-only keep-alive frame is the common case). An empty data payload
// is indistinguishable from "no data" and is treated as a no-op by the caller.

// Splits one already-delimited frame into its joined data payload.
std::string ParseOpenAiSseFrameData(std::string_view frame);

// One streaming chunk's parsed fields. `content` is
// choices[0].delta.content ("" when the delta carries no text: the role
// prelude, a finish_reason-only frame, or a tool-call delta). `finish_reason`
// is choices[0].finish_reason when the frame carries a STRING one — the final
// frame of a healthy stream ("stop"), or "length"/"content_filter" when the
// provider truncated or blocked the answer; JSON null and an absent field both
// leave it empty. The token is bounded at parse time exactly like the
// non-streaming path (BoundFinishReasonToken, 32 chars, UTF-8-boundary cut), so
// a hostile reason cannot bloat the failure detail. Pure; progress-guarded
// (every walk advances or stops — the F-1 lesson); never logs content.
struct OpenAiSseDelta {
    std::string content;
    std::string finish_reason;
};
OpenAiSseDelta ParseOpenAiSseDeltaJson(std::string_view json);

// ---- V10F-R1 (F-10, fix-list round 1): the SSE stream-terminator decision ----
//
// A healthy SSE stream ends with "data: [DONE]", but several OpenAI-compatible
// servers omit the terminator and simply close after the final frame naming
// finish_reason "stop". Pure so the unit suite pins the matrix headlessly:
//   * Done            — the [DONE] terminator arrived (the ordinary close).
//   * StopWithoutDone — [DONE] absent but the last finish_reason token is
//                       "stop": the stream is COMPLETE, the assembled answer
//                       is used (pre-fix this read as truncation and the whole
//                       request failed closed, discarding a finished answer).
//   * Truncated       — [DONE] absent and no "stop" (empty reason, "length",
//                       "content_filter", or anything else): the stream was
//                       cut or ran out of budget; the caller fails closed.
// Aborted streams never reach this decision — they already failed closed at
// the transport layer (status 0) before assembly.
enum class OpenAiSseTermination { Done, StopWithoutDone, Truncated };
constexpr OpenAiSseTermination ClassifyOpenAiSseTermination(bool saw_done,
                                                            std::string_view last_finish_reason) {
    if (saw_done) return OpenAiSseTermination::Done;
    if (last_finish_reason == "stop") return OpenAiSseTermination::StopWithoutDone;
    return OpenAiSseTermination::Truncated;
}

// ---- V10F-R1 (F-17, fix-list round 1): the request-time http-consent ------
// origin-match decision. The dialog persists a consent as a "host:port"
// ORIGIN string (OpenAiHttpConsentScope); before an http:// request may
// start, the current base_url's origin must equal that stored origin.
// Pure and Win32-free so every consumer (engine routing leg, future client
// leg) and the unit suite share one matrix. The grammar mirrors
// OpenAiHttpConsentScope exactly, with scheme-aware default ports (80 http,
// 443 https):
//   * the URL is ASCII-lowercased on a copy (scheme + host compares are
//     case-insensitive; the caller's buffer is untouched);
//   * the authority ends at the first '/', '?' or '#';
//   * userinfo ("user:pass@") is stripped — an origin never includes it;
//   * a bracketed "[v6]" host keeps its brackets, with the port after ']';
//   * an absent port fills with the scheme default (80 / 443);
//   * a non-numeric port or an empty host is unparsable;
//   * the stored side normalizes through the same grammar (it is canonical
//     lowercase "host:port" already, but a hand-edited config or a future
//     writer is normalized defensively; a missing port fills with 80, the
//     only reachable scheme at the compare).
// The consent scope is an http-only concept: an https or invalid base URL
// returns false even though the https grammar parses it (443 default) — the
// parse exists for grammar parity, the gate never lets https through.
inline bool OpenAiHttpConsentOriginMatches(std::string_view base_url,
                                           std::string_view stored_origin) {
    if (stored_origin.empty()) return false;

    std::string lowered(base_url);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }

    std::string_view rest(lowered);
    std::string default_port = "80";
    if (rest.compare(0, 7, "http://") == 0) {
        rest.remove_prefix(7);
    } else if (rest.compare(0, 8, "https://") == 0) {
        rest.remove_prefix(8);
        default_port = "443";
    } else {
        return false; // no http(s) scheme: unparsable origin, no-match
    }

    const size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority.remove_prefix(at + 1);

    auto port_is_digits = [](const std::string& p) {
        if (p.empty()) return false;
        for (const char c : p) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };

    std::string host;
    std::string port = default_port;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host.assign(authority.substr(0, close + 1));
        const std::string_view tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') return false;
            port.assign(tail.substr(1));
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host.assign(authority.substr(0, colon));
            port.assign(authority.substr(colon + 1));
        } else {
            host.assign(authority);
        }
    }
    if (host.empty() || !port_is_digits(port)) return false;
    // The consent scope is http-only: https bases no-match even though the
    // grammar above filled the 443 default.
    if (default_port == "443") return false;

    // Stored side: same grammar, defensive normalization (canonical output is
    // lowercase "host:port" already; only the port fill is reachable here).
    std::string stored(stored_origin);
    for (char& c : stored) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    std::string_view sauth(stored);
    const size_t sat = sauth.rfind('@');
    if (sat != std::string_view::npos) sauth.remove_prefix(sat + 1);
    std::string shost;
    std::string sport = "80";
    if (!sauth.empty() && sauth.front() == '[') {
        const size_t close = sauth.find(']');
        if (close == std::string_view::npos) return false;
        shost.assign(sauth.substr(0, close + 1));
        const std::string_view tail = sauth.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') return false;
            sport.assign(tail.substr(1));
        }
    } else {
        const size_t colon = sauth.rfind(':');
        if (colon != std::string_view::npos) {
            shost.assign(sauth.substr(0, colon));
            sport.assign(sauth.substr(colon + 1));
        } else {
            shost.assign(sauth);
        }
    }
    if (shost.empty() || !port_is_digits(sport)) return false;

    return host == shost && port == sport;
}

// ---- 261121_0001 U-F: C4 — per-model reasoning-effort discovery ----
//
// vp-plan C4 / report §2.3 option 2: ONE GET /v1/models yields both the model
// id list and the per-model reasoning-effort hints. Most servers advertise
// nothing (OpenAI, Gemini, vLLM, Ollama), so an EMPTY reasoning_efforts list
// is a normal answer: the caller then shows the standard fallback list, and
// the HTTP-400 retry in ChatCompletion covers a wrong guess.
//
// Recognized metadata shapes per model object (all optional):
//   "reasoning":        { "supported_efforts": ["low","high"] }
//   "reasoning_efforts":["none","medium"]
//   "supported_parameters":     ["temperature","reasoning_effort", ...]
//   "supported_openai_params":  ["reasoning_effort", ...]
//   "thinking":         { "values": ["none","low"] }
// For the two "supported_*" lists a bare "reasoning_effort" entry advertises
// the parameter WITHOUT its values, so the standard set is filled in. Values
// are mapped through a whitelist (none|minimal|low|medium|high|xhigh,
// case-insensitive) — an unknown token would only be a guaranteed HTTP 400 in
// the settings combo. Bounded like ParseOpenAiModelsJson: at most
// kMaxOpenAiModelIds models, kMaxOpenAiReasoningEfforts efforts per model, an
// id longer than kMaxOpenAiModelIdChars drops the entry, and the walk always
// progresses (the F-1 lesson). A malformed/truncated body returns whatever was
// collected. Pure; never logs content.
struct OpenAiModelCapability {
    std::string id;
    std::vector<std::string> reasoning_efforts;
};
inline constexpr std::size_t kMaxOpenAiReasoningEfforts = 8;
std::vector<OpenAiModelCapability> ParseOpenAiModelCapabilitiesJson(std::string_view json);

// ---- WinHTTP client (synchronous; worker thread) ----

class OpenAiCompatibleClient {
public:
    // REQ-045 (design §3b): GET {base}/v1/models with Bearer auth. On success
    // returns the parsed model id list (may be empty). On HTTP/transport/
    // parse failure returns an empty vector — the caller (settings dialog)
    // falls back to direct model entry. The 10 s budget bounds the GUI-thread
    // call in the settings dialog.
    //
    // 261121_0001 U-F (C4): a thin wrapper over FetchModelsAndCapabilities, so
    // the id list and the capability list can never be fetched (or parsed)
    // through two different network paths. Behaviour is unchanged for every
    // existing caller.
    static std::vector<std::string> ListModels(const OpenAiConfig& cfg);

    // 261121_0001 U-F (vp-plan C4; report §2.3 option 2): ONE GET
    // {base}/v1/models whose body is parsed TWICE — the id list through the
    // existing parser (byte-identical to ListModels/ParseOpenAiModelsJson) and
    // the per-model reasoning-effort hints through
    // ParseOpenAiModelCapabilitiesJson. No extra request: the settings dialog
    // gets "which efforts does this model accept" for free.
    // Returns true on a 200 whose body was parsed (both outputs filled — either
    // may be empty for a server that publishes nothing), false on the URL/
    // consent/key/decompose gates and on any transport or non-200 failure
    // (both outputs are cleared first, so a false return never carries stale
    // data). `ids`/`caps` may be null when only one half is wanted. Runs off
    // the GUI thread (the REQ-052 worker contract); the key is unprotected,
    // used and zeroed inside this call only.
    static bool FetchModelsAndCapabilities(const OpenAiConfig& cfg,
                                           std::vector<std::string>* ids,
                                           std::vector<OpenAiModelCapability>* caps);

    // REQ-045 (design §3b): POST {base}/v1/chat/completions. Returns the
    // translated text on success, empty on any failure.
    //
    // 261121_0001 U-F (report §3.1-A / §3.2):
    //   * 2A-1: the request profile is kChatProfile {5000, 5000, 15000, 60000}
    //     — a non-streaming reasoning model routinely needs 5-15 s before the
    //     first byte, and the old 5 s receive timeout aborted the request
    //     ("번역에 실패했습니다" on a 71-character sentence).
    //   * 2A-2: BuildChatBody omits "temperature"/"max_tokens" by default and
    //     serializes "reasoning_effort" only when cfg.reasoning_effort is set;
    //     an HTTP 400 gets ONE retry with the effort dropped.
    //   * 2A-4: ParseOpenAiChatJson handles array/null content, strips think
    //     blocks and surfaces the provider's finish_reason as a diagnosis.
    //   * §3.2: the source is split at kOpenAiChatChunkMaxUnits and translated
    //     chunk by chunk (no head/tail clamp, no deleted middle); any chunk
    //     failure fails the whole request with the provider's message.
    //   * §3.2 fix round 1 F-1: a chunk with no translatable content (empty or
    //     whitespace-only, see OpenAiChunkNeedsRequest) is never POSTed; its
    //     bytes ride into the joined output verbatim.
    //   * §3.2 fix round 1 F-2: the loop gives up when
    //     kOpenAiChunkLoopTotalBudgetMs is exhausted, failing the whole request
    //     with the "timeout" protocol code.
    //   * §3.2 fix round 2 G-1: the loop also gives up when the AGGREGATE of
    //     all chunk response bodies exceeds kOpenAiMaxTotalResponseBytes (8 MiB,
    //     >200x any legitimate per-chunk translation), failing the whole
    //     request through the same recorder with a bounded cause.
    //   * Per-chunk response cap 10 MiB (kMaxOpenAiResponseBodyBytes).
    //
    // 261121_0002 T2 (design-pick §Plumbing, OpenAI row): `ctx` is the progress
    // + cancel seam, trailing and defaulted so every existing call site keeps
    // compiling unchanged. Identical contract to GoogleTranslate::Translate:
    //   * ctx.cancel set -> the chunk loop unwinds at the next between-chunk
    //     boundary, BEFORE the loop-budget check, returning {} WITHOUT writing
    //     a failure detail (a user cancel is not an engine failure, so the
    //     LastChatErrorDetail slot stays clean; the manager attributes
    //     Canceled vs EngineFailed).
    //   * ctx.on_chunk, when the plan holds 2+ chunks (ShowChunkProgress),
    //     receives (index_1based, total) immediately BEFORE each chunk's POST.
    //     A chunk whose text needs no request (OpenAiChunkNeedsRequest is
    //     false) never advances the index, and the reasoning_effort 400-retry
    //     is a re-attempt of the SAME chunk, so it adds no second tick.
    // The default lives HERE only; the .cpp definition carries no default.
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
    //
    // V10F (RISK-02 / RISK-03 / L3-B, 261122_0002 P2-A): the three
    // TranslationTaskContext seams this client now consumes, all defaulted so
    // every existing call site is behavior-identical:
    //   * ctx.out_error_detail (RISK-03 carrier): receives the SAME bounded,
    //     sanitized, key-masked cause the out_error_detail parameter and the
    //     process-wide LastChatErrorDetail() slot receive, so a caller that
    //     owns its context can attribute the failure to ITS request instead of
    //     racing the global slot (which stays for back-compat). Cleared on
    //     entry, so a success leaves it empty; a cancel leaves it empty too (a
    //     cancel is not a failure).
    //   * ctx.abort (engine shutdown latch bridge): polled exactly like
    //     ctx.cancel at every chunk boundary and between SSE reads; a set
    //     latch unwinds the request bare (no failure detail), so shutdown can
    //     never leak text to the cloud or leave a "failed" cause behind.
    //   * ctx.on_chunk_text (L3-B): when non-empty the request is STREAMED
    //     ("stream":true, SSE). The callback runs on THIS (worker) thread with
    //     the CUMULATIVE translated text of the current chunk request — post
    //     think-block-strip + trim, i.e. exactly the text the chunk will
    //     contribute — once per received delta. It is CUMULATIVE, so a
    //     consumer must SET its preview surface to the received text, never
    //     append it. When the plan holds multiple chunks the callback restarts
    //     per chunk; ctx.on_chunk's (index, total) identifies which part. The
    //     assembled per-chunk text is identical to the non-streaming parse
    //     (same trim + think-strip + finish_reason diagnosis), and a stream
    //     that ends WITHOUT the "data: [DONE]" terminator fails the whole
    //     request closed UNLESS the final frame named finish_reason "stop"
    //     (F-10: several compatible servers omit [DONE] on a complete
    //     stream); an aborted or truncated stream always fails closed.
    static std::wstring ChatCompletion(const OpenAiConfig& cfg,
                                       std::string_view source_lang,
                                       std::string_view target_lang,
                                       std::wstring_view text,
                                       std::string* out_error_detail = nullptr,
                                       const TranslationTaskContext& ctx = {});

    // 261111_0001 U6: the most recent ChatCompletion FAILURE cause (the
    // sanitized detail above), or "" when the last chat request succeeded
    // or none ran. Process-wide, mutex-guarded; written only by
    // ChatCompletion (Translate serializes requests on translate_mutex_,
    // so "last" is unambiguous for consumers). The API key can never
    // appear — the failure recorder masks it via SanitizeOpenAiErrorDetail
    // before recording.
    static std::string LastChatErrorDetail();

    // V10F (RISK-02 Option A, 261122_0002 P2-A): abort the request that is in
    // flight RIGHT NOW, from ANY thread (the GUI cancel click / the engine's
    // shutdown path). V10F-R1 (F-01): the CHAT purpose request only — the
    // model-list fetch owns a separate slot and is never touched by a chat
    // cancel. Cloning the gguf_model_manager_window.cpp HfClearSlot
    // protocol (mutex-guarded, single-owner close): the slot is cleared under
    // the mutex BEFORE the handle is closed, so exactly one side ever calls
    // WinHttpCloseHandle, and the worker's own guard sees an empty slot and
    // does not close it again. WinHttpCloseHandle from another thread is the
    // documented abort for a pending WinHTTP call, so a worker blocked in
    // WinHttpReceiveResponse / WinHttpQueryDataAvailable / WinHttpReadData
    // returns within milliseconds instead of waiting out the 60 s receive
    // timeout. The request then fails CLOSED: the client clears the partial
    // body and returns the empty-result failure arm WITHOUT recording a
    // cause (a user cancel is not an engine failure).
    // Returns true when a handle was actually closed (a request was in
    // flight); a call while idle is a no-op — the caller's cancel token
    // covers the between-request window, exactly like the Hf dialog sets its
    // `cancel` flag alongside the close.
    static bool RequestAbortCurrent();
};

} // namespace emebalachat
