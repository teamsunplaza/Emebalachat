#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// REQ-043: the persisted engine_host block type (self-contained client
// header — see engine_host_client.hpp for the copy-into-other-apps contract).
#include "engine_host_client.hpp"

// REQ-045 P4-3 (design §3b, item 3b): the persisted OpenAI Compatible block
// (base_url / model / DPAPI key blob / integrity digest / http consent).
#include "openai_compatible_client.hpp"

namespace emebalachat {

// Represents a supported translation language with ISO code and localized names.
struct LanguageInfo {
    std::string code;           // e.g. "AUTO", "KO", "EN", "VI", "ZH-CN"
    std::string name_en;        // e.g. "Auto Detect", "Korean", "English", "Vietnamese"
    std::string name_native;    // e.g. "자동 감지", "한국어", "English", "Tiếng Việt"
    std::string display_short;  // e.g. "AUTO", "KO", "EN", "VI", "ZH-CN"
    // P4 Batch B-2 (design §3 B-2 note, §2-Q5 verdict A): canonical BCP-47 tag
    // fed to DWrite CreateTextFormat(localeName) so MapCharacters picks
    // script-appropriate fallback fonts (Myanmar Text, Leelawadee UI, Nirmala
    // UI...). "AUTO" carries the "en" pivot tag (auto-detected content is
    // overwhelmingly Latin; an empty localeName would freeze the fallback on
    // the user default locale instead). Deliberately distinct from B-3's
    // LocaleMapping.bcp47_full: that table is UiLocale-keyed (UI chrome),
    // this field is translation-language-keyed (content). Two owners, two
    // purposes — cross-reference prevents drift. Non-empty for all 90 rows
    // (pinned by TestReq038B2RegistryBcp47). const char* per the design's
    // B-2 row: static-literal storage, no allocation, copy-safe.
    const char* bcp47;          // e.g. "en", "ko", "zh-CN", "ar", "fil", "my"

    // 261116_0003 P2 (language-menu redesign, plan §4 / decision D7): the
    // per-engine capability bits this row is offerable under. Static data
    // column following the bcp47 precedent (constexpr bit constants, no
    // allocation). kLangHyMt2 marks the Hy-MT2 OFFICIAL 38-target set (the
    // model card fetched 2026-11-16, plan §2): the local built-in engine
    // offers exactly those rows (D7 — no custom row, no probe; OOD quality is
    // the reason). kLangGoogle marks rows Google's published table serves
    // (cloud.google.com/translate/docs/languages, fetched 2026-11-16): BO
    // (Tibetan) is absent from that table and therefore carries NO Google
    // bit. kLangOpenAi / kLangUserGguf mark rows the free-form prompt engines
    // can be OFFERED (research-report §4a/4b: both inject language names
    // free-form, so every registry row is offerable; actual support per model
    // is judged by the P9 capability probe, NOT by this mask). AUTO carries
    // all four bits by design: it is a source-only marker, never an
    // engine-filterable translation target.
    std::uint32_t engine_mask;
};

// 261116_0003 P2: the per-engine capability bits for LanguageInfo::engine_mask
// (values frozen; new engines append new bits).
inline constexpr std::uint32_t kLangHyMt2    = 1u << 0; // built-in local Hy-MT2 official set
inline constexpr std::uint32_t kLangGoogle   = 1u << 1; // Google Translate cloud
inline constexpr std::uint32_t kLangOpenAi   = 1u << 2; // OpenAI-compatible cloud
inline constexpr std::uint32_t kLangUserGguf = 1u << 3; // user-registered .gguf model

// 261116_0003 P2: which engine's offerable set a registry view filters by.
// Deliberately config-local (NOT src/engine.hpp's EngineType): the registry
// is the lower layer, and engine.hpp's EngineType gains a mapper below the
// TranslationManager instead (LanguageEngineFor). Auto is listed so callers
// can pass their engine selection through verbatim; it resolves to the HyMt2
// view because auto prefers local (plan §4 "Routing").
enum class LanguageEngine : unsigned char {
    HyMt2,     // the built-in local Hy-MT2 model (official 38 target names)
    Google,    // Google Translate cloud
    OpenAi,    // OpenAI-compatible cloud
    UserGguf,  // user-registered .gguf model
    Auto,      // resolves to the HyMt2 view (auto prefers local)
};

// ---- 261116_0003 P9a (plan §3 D5/D6, §5 P9): user-defined languages ----
//
// The config.json block is ADDITIVE and OPTIONAL. A config written before P9a
// has no "user_languages" key at all and reads as "no user languages" (empty
// vector, no migration, no crash). Exact writer emission, one entry shown:
//
//   "user_languages_version": 1,
//   "user_languages": [
//     {
//       "native_name": "Twi",
//       "english_name": "",
//       "code": "",
//       "script": "latin",
//       "probes": [
//         {"engine": "openai", "model": "gpt-4o-mini", "status": "supported",
//          "override": "unknown", "checked_at": "2026-11-17T22:45:00Z",
//          "evidence": "nonempty=1 differs=1 script=1 expected=latin detected=latin"}
//       ]
//     }
//   ]
//
// SCHEMA-VERSION DECISION (rationale recorded in the P9a phase note): the block
// carries its OWN version key instead of a global config version.
//   * The flat key/value reader has no version concept, and every other block
//     (engine_host, openai) evolves additively without one; a global version
//     would add "unknown version" handling to keys that need none.
//   * A block-local integer is the smallest unit that can be bumped by a future
//     breaking change (e.g. probes keyed differently) without touching anything
//     else, and it needs no migration code today.
//   * Absent key == version 1.
//   * A version NEWER than kUserLanguagesSchemaVersion is never guessed at: the
//     raw array text is preserved verbatim and re-emitted by the next save, so
//     running an older build after a newer one cannot silently drop the user's
//     languages. Version <= 1 parses normally.
//   * 261116_0003 P9a fix PR9-04 (carrier-vs-edit rule): the carrier preserves
//     ONLY a single array-group span: a '['-opened group scanned string- and
//     comment-aware (261116_0003 P9a fix3 PR11-02: every closer must match its
//     own opener) to its matching ']', with nothing but whitespace or comments
//     after it. Any other value or trailing shape fails closed (the block
//     drops to empty, the next save writes a clean current-version block), so
//     a string value can never be re-emitted unquoted and fabricate
//     sibling config keys. While the carrier is active, in-memory edits to
//     user_languages are DISCARDED on save (the verbatim block wins): P9b must
//     detect the carrier and warn/refuse edits.
//   * 261116_0003 P9a fix PR9-09: the version key parses STRICTLY (digit-only).
//     An unparseable ("abc", "2junk") or newer ("999") version is never guessed
//     at as the current version: the block is preserved verbatim and the
//     original version text round-trips unchanged.
//   * 261116_0003 P9a fix2 PR10-01/PR10-02: that preserved version TEXT is
//     emitted as a quoted, escaped JSON string (an empty or whitespace-only
//     version emits the quoted empty string), so it can never expand into
//     sibling keys and the next load keeps preserving the block instead of
//     reading a 0 sentinel as the current version.
//   * 261116_0003 P9a fix2 PR10-04: carrier eligibility is bracket-balance
//     based (the matching ']' plus trivia only), so a trailing note after the
//     array is trivia rather than a disqualifier and a multi-group span
//     ("[1] [2]") is rejected.
inline constexpr int kUserLanguagesSchemaVersion = 1;

// Bounds for user-writable text: the reader caps the block instead of trusting
// it, which keeps load and menu-merge work bounded. Extras beyond the cap are
// dropped (never stored, never rendered).
inline constexpr std::size_t kMaxUserLanguages = 64;
inline constexpr std::size_t kMaxUserLanguageProbes = 8;

// 261116_0003 P9a fix PR9-03/PR9-05: per-field byte caps. A stored name is a
// menu row and a raw prompt token, a probe's engine/model/evidence strings are
// cache metadata: every string field of the schema is capped so a hand-edited
// file cannot grow any of them without bound. Extras are truncated at a UTF-8
// code-point boundary, never stored in full.
inline constexpr std::size_t kMaxUserLanguageNameBytes = 256;
inline constexpr std::size_t kMaxUserLanguageProbeEngineBytes = 32;
inline constexpr std::size_t kMaxUserLanguageProbeModelBytes = 256;
inline constexpr std::size_t kMaxUserLanguageProbeCheckedAtBytes = 32;
inline constexpr std::size_t kMaxUserLanguageProbeEvidenceBytes = 256;

// D6 probe-cache state. The five states P9b renders are folded from two
// stored fields (see UserLanguageProbe):
//   Unknown        - never probed (this is the automatic first-use trigger)
//   Checking       - a probe call is in flight; RUNTIME ONLY, a persisted
//                    "checking" reads back as Unknown so a crashed run cannot
//                    leave a language stuck in "checking" forever
//   Supported      - the cached probe verdict
//   Unsupported    - the cached probe verdict
//   UserOverridden - DISPLAY ONLY (DisplayUserLanguageProbeStatus): the user
//                    set an override, whose direction lives in
//                    UserLanguageProbe::override_status
enum class UserLanguageProbeStatus : std::uint8_t {
    Unknown = 0,
    Checking = 1,
    Supported = 2,
    Unsupported = 3,
    UserOverridden = 4,
};

// One cached probe verdict for one (engine, model) pair (D6: "cached per
// (engine + model + language)"; the language is the owning UserLanguage).
struct UserLanguageProbe {
    // Stable engine id (UserLanguageEngineId): "google", "openai" or
    // "user_gguf". Empty entries are never stored (the engine is the key).
    std::string engine;
    // Model identity: the OpenAI model string, or the .gguf registry id. ""
    // is legal for engines whose model is implied (Google), and is a distinct
    // cache key from any named model.
    std::string model;
    // The probe's own verdict (Unknown / Supported / Unsupported after a load).
    UserLanguageProbeStatus status = UserLanguageProbeStatus::Unknown;
    // The user's final-authority answer (D6 "user is the final authority"):
    // Unknown = no override, else Supported or Unsupported. Effective status =
    // override when set, probe verdict otherwise.
    UserLanguageProbeStatus override_status = UserLanguageProbeStatus::Unknown;
    // Shape-only ASCII evidence ("nonempty=1 differs=1 script=1 ..."), never
    // any user or model content. Empty until a probe has run.
    std::string checked_at;  // ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ", "" = never
    std::string evidence;
};

// One user-defined language (D5). `native_name` is the only required field;
// everything else is derived:
//   * english_name - optional user input. MakeUserLanguage() fills it from the
//     registry when the code resolves and the user left it empty, because the
//     stored form is what the prompt injects and the English template must not
//     receive a native-script name (Task 1 code-switching finding).
//   * code         - resolved registry code, "" = free-form engines only
//     (D5: "no code = OpenAI/user-.gguf only"). Never "AUTO".
//   * script_mask  - inferred from the native name (D5), using the
//     win32_input.hpp kScriptMask* vocabulary so the probe's script check and
//     the rest of the app speak one script language. 0 = inference failed
//     (fail-open: the probe's script check is skipped, never guessed).
//   * probes       - the D6 verdict cache, one entry per (engine, model).
struct UserLanguage {
    std::string native_name;
    std::string english_name;
    std::string code;
    std::uint32_t script_mask = 0;
    std::vector<UserLanguageProbe> probes;
};

// The stored form of one user language: english_name when present, else
// native_name. This is what a language pin holds, what PlanLanguageSync
// canonicalizes to, and what the prompt injects when the code is empty.
std::string UserLanguageStoredName(const UserLanguage& language);

// Case-insensitive (ASCII) lookup of a registered user language by stored
// name, native name, english name, or resolved code. nullptr when absent.
const UserLanguage* FindUserLanguage(const std::vector<UserLanguage>& languages,
                                     std::string_view token);

// Stable per-engine id used as the probe-cache key ("google" / "openai" /
// "user_gguf"). HyMt2 and Auto return "" (D7: the local built-in engine has no
// custom row and no probe).
std::string UserLanguageEngineId(LanguageEngine engine);

// Script token <-> mask conversion (config.json stores the token, not the
// number: the file is user-editable, and the ws2 pin ties mask bit values to
// engine_core ordinals, so a stored number could silently re-point at another
// script if that ordering ever changed). Tokens are the lowercase mask names in
// ascending bit order, joined with '+' for a composite mask: "hangul",
// "han+kana", "latin". "" for mask 0 and for an unrecognized token.
std::string UserLanguageScriptToken(std::uint32_t script_mask);
std::uint32_t UserLanguageScriptMaskFromToken(std::string_view token);

// D5 script inference: the script classes present in the user-typed native
// name (pure; UTF-8 in, mask out). Fail-open by construction: an empty,
// digit-only, or unclassifiable name yields 0 (callers keep the language and
// skip the probe's script check). Mixed names OR their classes ("한국어
// Korean" -> hangul+latin) instead of guessing one.
std::uint32_t InferUserLanguageScript(std::string_view native_name);

// One entry of the fallback name->code data source (D5 stage 2): an installed
// system locale with the language names Windows reports for it.
struct LocaleLanguageName {
    std::string locale;   // BCP-47 locale name from EnumSystemLocalesEx, e.g. "ko-KR"
    std::string english;  // LOCALE_SENGLISHLANGUAGENAME, e.g. "Korean"
    std::string native;   // LOCALE_SNATIVELANGUAGENAME, e.g. "한국어"
};

// The D5 stage-2 data source: every installed system locale. Live Win32
// read-only query (EnumSystemLocalesEx + GetLocaleInfoEx), no caching, no
// network. Returns an empty vector if the enumeration fails (callers then get
// the "no code" outcome, which is a legal D5 result).
std::vector<LocaleLanguageName> EnumerateSystemLocaleLanguages();

// D5 stage 2 behind an injectable seam so the name->code rule is headless-
// testable without depending on which locales this machine has installed.
// `entries` is the enumeration result; the seam-less overload above uses
// EnumerateSystemLocaleLanguages().
using LocaleLanguageSource = std::vector<LocaleLanguageName> (*)();
std::string ResolveCodeFromLocaleLanguages(std::string_view native_name,
                                           std::string_view english_name,
                                           const std::vector<LocaleLanguageName>& entries);

// D5 resolution result. `code` empty means "free-form engines only"; the
// source flags let the UI/P9b explain how the code was found.
struct UserLanguageResolution {
    std::string code;          // registry code, "" when unresolved
    std::string english_name;  // registry name_en of the resolved row, else ""
    bool from_alias = false;   // resolved by the alias matrix (stage 1)
    bool from_locale = false;  // resolved by the system locale names (stage 2)
};

// D5 ladder: alias matrix (exact match only - prefix/substring stay search-only
// so a typo never silently resolves to the wrong language) -> installed locale
// names -> "" (free-form only). `native_name` is tried first, then
// `english_name`; ASCII-case-insensitive throughout. Pure apart from the
// injectable data source.
UserLanguageResolution ResolveUserLanguageCode(std::string_view native_name,
                                              std::string_view english_name,
                                              LocaleLanguageSource locale_source);
UserLanguageResolution ResolveUserLanguageCode(std::string_view native_name,
                                              std::string_view english_name);

// Builds one registered entry from the two user-typed fields: code resolved,
// script inferred (falling back to the resolved code's script when the name
// yields nothing), english_name filled from the registry when the code
// resolved (alias matrix OR locale fallback, 261116_0003 P9a fix PR9-07) and
// the user left it empty. An empty native_name yields an entry with empty
// native_name (callers must reject it).
UserLanguage MakeUserLanguage(std::string_view native_name, std::string_view english_name);
// 261116_0003 P9a fix2 PR10-05: the same build with the D5 stage-2 data source
// injected, so the PR9-07 back-fill rule (the locale-fallback stage included)
// is testable without depending on the host's installed locales. The overload
// above uses EnumerateSystemLocaleLanguages().
UserLanguage MakeUserLanguage(std::string_view native_name,
                              std::string_view english_name,
                              LocaleLanguageSource locale_source);

// ---- 261116_0003 P9b-3 (PR9-11): creation-time name validation ----
//
// A user entry is created only through AppConfig's locked write mutators, and
// those refuse two classes of name BEFORE the entry reaches the registry:
//
//   * RESERVED spellings. The app's own AUTO row is "Auto Detect" (name_en),
//     "자동 감지" (name_native) and "AUTO" (code). A user entry named after
//     auto-detect was offered by the OpenAI/user-.gguf menus but refused by the
//     planner (PR9-11, fix-list-round9: offers=1 / valid=0 - a dead menu row),
//     so the spelling is refused at creation instead of being rendered and
//     then rejected on pick.
//   * DUPLICATES. A name that collides with an existing registry row's
//     stored/native/English/code spelling set, or with an entry already in the
//     registry (the same four-spelling lookup FindUserLanguage performs). The
//     registry already offers that language, and a second row would shadow or
//     rename it (PR9-01's code-level dedupe drops the later row - another dead
//     row).
//
// The comparison folds exactly like the storage path: ASCII whitespace
// trimmed, the PR9-03/PR10-08 invisible-character strip applied, then ASCII
// case-insensitive. An empty native name is NOT a verdict here (the write
// verbs refuse it themselves with RejectedEmptyName). Pure: reads the static
// registry plus the caller's vector.
enum class UserLanguageNameVerdict : std::uint8_t {
    Accepted = 0,
    Reserved = 1,   // an AUTO spelling ("Auto Detect" / "AUTO" / "자동 감지")
    Duplicate = 2,  // a registry row or an existing user entry owns the name
};

// `existing` is the registry the entry would join; `ignore_index` lets an
// update re-validate an entry without matching its own current name
// (kUserLanguageNoIndex for a creation check).
inline constexpr std::size_t kUserLanguageNoIndex = static_cast<std::size_t>(-1);
UserLanguageNameVerdict ValidateUserLanguageName(
    std::string_view native_name,
    std::string_view english_name,
    const std::vector<UserLanguage>& existing,
    std::size_t ignore_index = kUserLanguageNoIndex);

// Result of one locked write on AppConfig::user_languages (PR9-12). Every
// refusal reason is distinct because the add dialog renders different copy per
// reason; the reserved/duplicate values mirror UserLanguageNameVerdict.
enum class UserLanguageWriteResult : std::uint8_t {
    Applied = 0,
    RefusedForwardCompatCarrier = 1,  // newer-schema block preserved: edits are discarded
    RejectedEmptyName = 2,            // native_name is the one required field
    RejectedReservedName = 3,         // PR9-11 reserved AUTO spelling
    RejectedDuplicateName = 4,        // PR9-11 registry or user duplicate
    RejectedCapacity = 5,             // kMaxUserLanguages / kMaxUserLanguageProbes
    RejectedNotFound = 6,             // no entry at that index / stored name
    RejectedMissingProbeKey = 7,      // empty engine id: the cache key is incomplete
    RejectedInvalidStatus = 8,        // an override is an answer, not a state
};

// ---- D6 capability probe (pure evaluation + cache transitions) ----
//
// The fixed, non-personal probe sentence. A constant on purpose: the probe
// never sends user content (privacy posture, AGENTS.md), and a stable sentence
// keeps cached verdicts comparable across runs. Pinned by the P9a suite.
inline constexpr const char* kUserLanguageProbeSentence =
    "Please send me the report by tomorrow morning.";

// The three D6 checks over one engine call. Returns the verdict plus the
// per-check bits and a shape-only evidence string (scripts are named, the text
// itself never appears), so P9b can show the evidence and re-run.
//   1. non_empty  - the output has a non-whitespace character
//   2. differs    - the trimmed output differs from the trimmed input
//   3. script_ok  - the output carries the expected script class, or
//                   expected_script_mask == 0 (fail-open, "script=na")
// status is Supported only when all applicable checks pass, Unsupported
// otherwise (the evaluator never returns Unknown/Checking).
struct ProbeEvaluation {
    UserLanguageProbeStatus status = UserLanguageProbeStatus::Unknown;
    bool non_empty = false;
    bool differs = false;
    bool script_ok = false;
    std::uint32_t detected_mask = 0;
    std::string evidence;
};
ProbeEvaluation EvaluateProbeResult(std::string_view input_sentence,
                                   std::string_view output_text,
                                   std::uint32_t expected_script_mask);

// Probe-cache accessors/transitions (no transport here: the single engine call
// stays behind the caller's injectable seam, so P9a ships zero network code).
const UserLanguageProbe* FindUserLanguageProbe(const UserLanguage& language,
                                               std::string_view engine,
                                               std::string_view model);

// Override wins (D6: the user is the final authority). Returns Unknown when
// neither an override nor a probe verdict exists, Checking while a probe is in
// flight.
UserLanguageProbeStatus EffectiveUserLanguageProbeStatus(const UserLanguageProbe& probe);

// The five-state display fold: UserOverridden while an override is set, else
// the effective status.
UserLanguageProbeStatus DisplayUserLanguageProbeStatus(const UserLanguageProbe& probe);

// 261116_0003 P9b-3 (PR10-07): the CANONICAL probe-cache key. The runtime
// record verbs used to store whatever the caller passed (the per-field byte
// caps were load-path only), so an over-long model could enter the file and -
// worse - split one logical key into two slots (the comparison used the raw
// argument against a capped stored value). Capping here, before the comparison,
// keeps the key canonical for every writer: the free record verbs and the
// AppConfig write verbs both address slots through this.
struct UserLanguageProbeKey {
    std::string engine;  // trimmed + kMaxUserLanguageProbeEngineBytes
    std::string model;   // kMaxUserLanguageProbeModelBytes
};
UserLanguageProbeKey MakeUserLanguageProbeKey(std::string_view engine,
                                              std::string_view model);

// Stores a completed probe run: overwrites the cached verdict. An existing
// override is NEVER retired here (261116_0003 P9a fix PR9-06: the user is the
// final authority, so a probe run must not wipe the user's answer); the
// automatic first-use pass fires only when no verdict AND no override exists.
// No-op at kMaxUserLanguageProbes when the key is new.
void RecordUserLanguageProbeResult(UserLanguage& language,
                                   std::string_view engine,
                                   std::string_view model,
                                   const ProbeEvaluation& evaluation,
                                   std::string_view checked_at);

// Stores the user's final-authority answer. Only Supported/Unsupported are
// accepted (any other value is a no-op) - an override is an answer, not a
// state.
void RecordUserLanguageProbeOverride(UserLanguage& language,
                                     std::string_view engine,
                                     std::string_view model,
                                     UserLanguageProbeStatus status,
                                     std::string_view checked_at);

// Drops the override (back to the cached probe verdict, or to Unknown when
// nothing was probed yet).
void ClearUserLanguageProbeOverride(UserLanguage& language,
                                    std::string_view engine,
                                    std::string_view model);

// 261116_0003 P9b-3: the record verbs need a checked-at stamp, and the config
// layer had no UTC formatter (diag_logger.cpp's TimestampNow is LOCAL time and
// private to that module). Minimal on purpose: the documented wire form is
// "YYYY-MM-DDTHH:MM:SSZ" - 20 bytes, inside kMaxUserLanguageProbeCheckedAtBytes.
// GetSystemTime is UTC by definition (no DST, no locale).
std::string UserLanguageProbeTimestampNowUtc();

// ---- D5/D7 menu data merge (data only; P9b renders it) ----
//
// Which section a merged row belongs to. The registry rows keep the existing
// registry order; the user rows keep config order and form the "내 언어" group
// that P9b draws at the top of the cloud menus.
enum class LanguageMenuGroup : unsigned char {
    Registry = 0,     // GetLanguagesForEngine view row
    UserLanguage = 1, // user-registered row ("내 언어")
};

// One "내 언어" row. `registry` is set when the user entry resolved to a
// registry code (the row then carries that row's code/bcp47 for display and
// routing), and nullptr for a free-form-only entry (code "" - only the
// free-form engines offer it). `user` is always the owning config entry, so
// the native name, script and probe cache stay reachable.
struct LanguageMenuRow {
    const LanguageInfo* registry = nullptr;
    const UserLanguage* user = nullptr;
    LanguageMenuGroup group = LanguageMenuGroup::UserLanguage;
};

struct LanguageMenuView {
    // The engine's registry rows MINUS the rows a user entry claimed (a
    // language must appear once per menu).
    std::vector<const LanguageInfo*> registry;
    // The "내 언어" group, config order, first occurrence of a code wins.
    std::vector<LanguageMenuRow> user_group;
};

// D5 "내 언어 group atop cloud menus" + D7 "local built-in offers only the
// official set, no custom row, no probe":
//   * HyMt2 -> user_group is ALWAYS empty and `registry` is the
//     GetLanguagesForEngine view verbatim (pointer-identical contents).
//   * Google -> a user entry is offered only when its code resolved AND that
//     registry row carries kLangGoogle (publisher-declared capability, D6);
//     free-form entries are excluded because Google cannot serve them (D5).
//   * OpenAi / UserGguf -> every registered entry is offered (both engines
//     take language NAMES free-form), resolved or not.
//   * Auto -> resolves to the GOOGLE (menu) view, NOT to
//     GetLanguagesForEngine's HyMt2 routing view: under engine_type=auto the
//     language menus render the cloud list (the P8b tray idiom maps auto to
//     Google), so auto must show the same "내 언어" group the Google menu
//     shows. Routing keeps LocalPairReliable's own mask rule; menus and
//     routing are two views with two purposes.
// Pure: reads the static registry and the caller's vector, allocates only the
// returned view.
LanguageMenuView BuildLanguageMenuView(LanguageEngine engine,
                                      bool include_auto,
                                      const std::vector<UserLanguage>& user_languages);

// Returns the full list of 90 language entries (AUTO + 89 supported languages).
const std::vector<LanguageInfo>& GetSupportedLanguages();

// Returns the list of 89 target languages (excluding AUTO).
const std::vector<LanguageInfo>& GetTargetLanguages();

// 261116_0003 P2 (plan §4): an engine-filtered VIEW into the global registry
// (never a copy). The returned vector holds pointers at the static
// kAllLanguages rows, stable for the process lifetime (the registry is const
// after init). include_auto=false drops the AUTO row (target menus); true
// keeps it (source menus). LanguageEngine::Auto resolves to the HyMt2 view
// (auto prefers local). Validation stays registry-global on purpose
// (NormalizeLanguageCode / PlanLanguageSync / BuildPrompt do NOT filter), so
// a cloud-picked pin survives an engine switch — the menus filter, the
// validator does not (research-report §8 items 3/5).
const std::vector<const LanguageInfo*>& GetLanguagesForEngine(LanguageEngine engine,
                                                              bool include_auto);

// ---- 261116_0003 P9c-1a: the OpenAI-compatible declared-language filter ----
//
// The optional user declaration "이 모델이 지원하는 언어": the set of languages the
// user says their OpenAI-compatible ENDPOINT's model actually serves. It is a
// VIEW filter only - the router keeps using engine_mask (LocalPairReliable), so
// translation routing, the AUTO->Google menu contract and every other engine's
// view are untouched.
//
// Wire form (config.json, OPTIONAL + ADDITIVE - the P9a "user_languages"
// precedent): a top-level array of canonical registry codes next to the
// "openai" block,
//   "openai_declared_languages": ["KO", "JA", "EN"],
// emitted ONLY while the set is non-empty. A config.json without the key reads
// as "not declared" = the pre-P9c behavior (the OpenAI view offers every row
// carrying kLangOpenAi), and it round-trips byte-identically. NO schema version
// is bumped: the field is additive, an unknown code inside a present array is
// DROPPED rather than failing the load, and a malformed value reads as "not
// declared" (the lenient ParseOpenAiBlock posture).
//
// The set itself (SanitizeOpenAiDeclaredLanguages is the single canonicalizer):
//   * registry CODE form - "Korean"/"한국어"/"ko" all resolve to "KO"; anything
//     else is dropped;
//   * REGISTRY order, never declaration order, so a hand-reorder of the array
//     produces identical bytes and identical menus;
//   * AUTO is never a declaration member (it is a source-only marker and the
//     source view always offers it);
//   * duplicates collapse first-wins, and the size is bounded by the registry
//     (kMaxOpenAiDeclaredLanguages == the OpenAI target-view size; the constant
//     is the defensive cap, not a user-facing limit).
//
// WHY THE FILTER INPUT IS PROCESS-WIDE: the menu surfaces hold no AppConfig
// reference (tray.cpp / tooltip.cpp / language_search_palette.hpp call
// GetLanguagesForEngine and BuildLanguageMenuView with an engine + user vector
// only), so the declaration is published into a snapshot that those two
// functions read. PublishOpenAiDeclaredLanguages is called from exactly two
// places in src/config.cpp - the load path (FromJsonString) and the locked verb
// AppConfig::SetOpenAiDeclaredLanguages - and from nowhere else; the single
// AppConfig instance main.cpp owns is therefore the only writer. There is ONE
// filter point (GetLanguagesForEngine), so the tray's four language menus
// (through BuildLanguageMenuView), the tooltip's two dropdowns, the search
// picker's pool and the Ctrl+F9 cycle set can never disagree about what the
// endpoint declares.
inline constexpr std::size_t kMaxOpenAiDeclaredLanguages = 89;
inline constexpr std::size_t kMaxOpenAiDeclaredLanguageCodeBytes = 16;

// Canonicalizes ONE declared token (ASCII-trimmed; registry code / English /
// native spelling -> its registry code). Returns "" for AUTO, for an over-long
// token and for anything the registry does not know. Pure.
std::string CanonicalOpenAiDeclaredLanguage(std::string_view token);

// The canonical persisted form (semantics above). Pure and idempotent.
std::vector<std::string> SanitizeOpenAiDeclaredLanguages(
    const std::vector<std::string>& declared);

// Publishes the set the OpenAI views filter by and returns the canonical set the
// caller must persist, so the stored value and the rendered menus can never
// drift. A publish of the SAME canonical set is a no-op (no view rebuild).
// Called ONLY by src/config.cpp (load path + AppConfig::SetOpenAiDeclaredLanguages);
// public because the Config tests drive the same authority.
std::vector<std::string> PublishOpenAiDeclaredLanguages(
    const std::vector<std::string>& declared);

// Read side of the published snapshot: is a non-empty declaration armed (the
// OpenAI views are filtered), and is this registry code declared. Both take the
// snapshot lock; used by tests, tools and diagnostics, never by a menu surface
// (the filter itself lives in GetLanguagesForEngine).
bool OpenAiLanguagesDeclared();
bool OpenAiLanguageDeclared(std::string_view code);

// ---- 261122_0001 B-U1 (OpenAI 설정 창 프로필 재설계, design-pick §1/§2/§7):
// the OpenAI-compatible PROFILE LIBRARY ----
//
// The existing "openai" block stays the ACTIVE profile's mirror (design-pick
// item 1: the engine/worker/REQ-051 runtime consumption paths keep reading
// `openai` and never learn about the library). The library is the optional
// top-level "openai_profiles" array (the openai_declared_languages optional-key
// convention): an entry is the 6-field OpenAiConfig block shape PLUS "name".
// There is deliberately NO version key (design-pick item 4, recorded decision):
// an entry is homomorphic to OpenAiConfig, so it is self-describing; a future
// breaking change adds an additive optional key INSIDE an entry.
//
// Wire form (one entry shown; emitted ONLY while the library is non-empty):
//   "openai_profiles": [
//     {
//       "name": "openai / gpt-4o-mini",
//       "base_url": "https://api.openai.com",
//       "model": "gpt-4o-mini",
//       "reasoning_effort": "",
//       "api_key_dpapi": "<base64 DPAPI blob>",
//       "api_key_sha256": "<64 lowercase hex>",
//       "http_consent_given": false
//     }
//   ]
//
// Key material (api_key_dpapi + api_key_sha256) round-trips as OPAQUE strings,
// exactly like the active block; it is never logged, printed or otherwise
// surfaced (shape-only posture, AGENTS.md §Security).
//
// Load semantics (design-pick item 2, the lenient posture ParseOpenAiBlock
// already takes): the reader captures the raw array verbatim; a non-array value
// reads as an EMPTY library; a malformed single entry is SKIPPED, never a hard
// load failure; entries past kMaxOpenAiProfiles are dropped. A PRESENT key is
// authoritative (it replaces the in-memory library); an ABSENT key keeps the
// current value, exactly like every other key in this reader.
//
// Migration (design-pick item 2/§"마이그레이션"): when the key is ABSENT and the
// "openai" block is non-empty (any of base_url / model / api_key_dpapi /
// api_key_sha256 set), ONE profile is synthesized from the block with a name
// derived by the pure helpers below; the "openai" block itself stays BYTE-
// IDENTICAL (it is copied, never moved). When the key is PRESENT, nothing is
// synthesized. The load-time derivation uses the FIXED ASCII template
// "{h} / {m}" (the en form of OpenAiProfileNameTemplate) so the synthesis is
// DETERMINISTIC — the same input file always yields the same library. The
// UI-locale template derivation at dialog save time (design-pick §7 "저장 시점
// UI 로케일로 1회 파생") is the dialog unit's (U4) job, not the load path's.
//
// WRITER DISCIPLINE (261122_0001 B-U1): the library is mutated ONLY on the GUI
// thread (the settings dialog fold, unit U4) and persisted by the single
// caller-side SaveToFile (design-pick item 4, P9c-1c 단일 writer). The pipeline
// worker reads only the `openai` active block, so — exactly like `openai`
// itself — this member needs no mutex_ and no Snapshot entry. Direct
// assignment is legal only before any thread exists (startup seeding, unit
// tests, the load path above).
struct OpenAiProfile {
    std::string name;  // derived display name, fixed as user data once saved
    OpenAiConfig cfg;  // the full 6-field openai block shape, stored verbatim
};

// Bounds for the library (the reader caps the block instead of trusting it).
inline constexpr std::size_t kMaxOpenAiProfiles = 32;
inline constexpr std::size_t kMaxOpenAiProfileNameBytes = 96;

// Pure display-name derivation helpers (design-pick §7; i18n-INDEPENDENT, so
// they live in this config TU and are unit-test targets). They take base_url /
// model as DATA: control/bidi characters are NOT stripped here — sanitizing
// before save is the dialog's duty (design-pick §7, the R2-M/G-02 sanitize-
// before-save precedent).
//
// OpenAiHostAbbrev: strips scheme / userinfo / port / path / query down to the
// host, then abbreviates: a multi-label DNS hostname yields its SECOND-TO-LAST
// label lowercased (api.openai.com -> "openai", the design-pick host token); a
// single label or an IP literal (IPv4, or [v6] WITH its brackets) yields the
// whole host; an empty/relative base (no host at all) yields "". The result is
// capped at 24 UTF-8 bytes on a code-point boundary, with a "…" suffix when
// truncated (the ellipsis counts toward the cap).
std::string OpenAiHostAbbrev(std::string_view base_url);

// MakeOpenAiProfileName: substitutes the {h} and {m} tokens of name_template.
// An empty host yields the model alone; an empty model yields the host alone;
// both empty yields "" (the CALLER handles that degenerate case). The result is
// capped at kMaxOpenAiProfileNameBytes on a code-point boundary.
std::string MakeOpenAiProfileName(std::string_view host_abbrev,
                                  std::string_view model,
                                  std::string_view name_template);

// MakeUniqueOpenAiProfileName: appends " (2)", " (3)" … on a name collision
// (exact, case-sensitive comparison — a name is user data). The suffix is
// always kept whole: the base is truncated to cap-minus-suffix on a code-point
// boundary first, so the result never exceeds kMaxOpenAiProfileNameBytes.
std::string MakeUniqueOpenAiProfileName(const std::vector<OpenAiProfile>& existing,
                                        std::string_view base_name);

// PlanOpenAiProfileUpsert (design-pick item 3; consumed by the dialog unit U4,
// pure here so it is pinned now): the storage identity is (base_url ASCII
// case-insensitive, model case-SENSITIVE). Returns the index of the matching
// entry (save = overwrite that entry's cfg wholesale, keep its name), or
// existing.size() when no entry matches (save = append a new profile).
std::size_t PlanOpenAiProfileUpsert(const std::vector<OpenAiProfile>& existing,
                                    const OpenAiConfig& candidate_identity);

// Looks up a language by its code (case-insensitive, e.g. "ko", "KO", "ZH-CN").
const LanguageInfo* FindLanguageByCode(std::string_view code);

// Looks up a language by English or native name (case-insensitive).
const LanguageInfo* FindLanguageByName(std::string_view name);

// Resolves a code or name into a canonical uppercase language code (e.g. "korean" -> "KO").
std::string NormalizeLanguageCode(std::string_view code_or_name);

// Resolves the effective target language when the EFFECTIVE source equals the
// configured target (src==tgt is meaningless for translation output).
// F2 (session 260908_0003, verify 220310 §4 option (b)): the collision pivot
// applies ONLY to never-touched AUTO defaults. user_explicit_target marks a
// target the USER deliberately chose (tooltip/tray Drag-target menu pick, or
// a value loaded from a persisted user pick = drag_target_pinned). An
// explicit choice returns nullopt unconditionally - it is honoured verbatim
// even when it collides with the source, per the user rule "사용자가 도착언어를
// 한 번 바꾸면 그 값으로 고정". This preserves the V6 KO->KO anti-corruption
// for AUTO defaults (user_explicit_target=false keeps the full rank ladder).
// ADR-A1-7 policy (language-neutral pivot; supersedes the old "src==EN ?
// Korean : English" hardcoding, which privileged the KO<->EN pair):
//   1. OS UI language, when it is a supported language AND differs from the
//      colliding src (the sys != src guard is what makes an OS==src pivot
//      impossible instead of looping back to the same language);
//   2. English, when src is not English (the ADR-A1-7 neutral pivot value;
//      the ROUTING behind it is mask-driven since 261116_0003 P2: EN carries
//      kLangHyMt2, so an official source pivots to a locally servable pair,
//      while a cloud-only source is routed by LocalPairReliable's D7
//      engine_mask rather than by this rank);
//   3. otherwise NO pivot: nullopt, so the caller keeps its original target.
//      Reached only when src == tgt == EN and the OS language is EN/unknown -
//      the user can already read that language, so showing the source text is
//      not corruption (R7 in ADR-A1-7).
// Returns std::nullopt when no substitution is needed (src != tgt) or when the
// neutral rule found no meaningful alternative; callers skip the sync/pivot.
// Pure: no Win32 message traffic, no config mutation. I18n::GetSystemLanguageCode
// is a read-only locale query, safe from any thread.
std::optional<std::string> ResolveEffectiveTarget(std::string_view detected_src,
                                                   std::string_view current_tgt,
                                                   bool user_explicit_target = false);

// F3 (session 260908_0002, ADR-A1-2): the single source-decision rule shared by
// the three drag-family entry points (drag icon, double-Ctrl+C, tooltip
// re-translate). A NON-AUTO persisted drag source wins over detection ("once
// the user changes it, it stays changed"); "Auto Detect" - and any value that
// fails to resolve, which normalizes to AUTO - keeps the caller's
// detected/last-displayed fallback, which the entry points then inject into the
// engine as an EXPLICIT source (the established drag contract: the engine never
// sees a raw AUTO on this path). Pure: the caller passes snapshot values, no
// config access here; safe from any thread. Unit-tested headlessly
// (TestResolveEffectiveSource).
std::string ResolveEffectiveSource(std::string_view persisted_src,
                                   std::string_view detected_or_fallback);

// 261108_0002 (WS3, v0.10.6): the back-translation SOURCE language for the
// floating verification card. The card re-translates the forward output
// (BBBBB) back into the language the user originally typed (AAAAA') so the
// user can judge whether the meaning survived. That source is resolved with a
// four-tier ladder, all tiers canonicalized through NormalizeLanguageCode
// (so "Auto Detect", "AUTO", "", and any unresolvable token read as AUTO):
//   1. type_source_language      - the user's PINNED typing source is ground
//                                  truth ("once the user changes it, it stays
//                                  changed", the F3/ADR-A1-2 rule);
//   2. detected_source_language  - the per-request detection (the engine's
//                                  resolved source), used while the pinned
//                                  source is AUTO;
//   3. system_language_code      - the OS UI language, the same read-only
//                                  locale fallback ResolveEffectiveTarget
//                                  ranks first (ADR-A1-7);
//   4. the ADR-A1-7 neutral pivot, which ResolveEffectiveTarget owns. Because
//      tier 3 already consumed that helper's rank-1 outcome (the OS language),
//      tier 4 replays its rank-2/3 tail: English when the forward target is
//      not EN, else the forward target itself (the user already reads it, so
//      the card shows the same text as an explicit "nothing to verify"
//      signal). The function therefore never returns an empty string.
// Every tier SKIPS a candidate that canonicalizes to the forward target: a
// back-translation into the language the text was just translated INTO would
// be a no-op round trip (src==tgt) and prove nothing about the meaning.
// The result is the LanguageInfo::name_en spelling (the stored form every
// language surface uses). Pure: the caller passes the snapshot values and the
// OS code, so all four tiers and the skip rule are unit-testable headlessly
// (TestWs3BackTranslationTarget).
std::string ResolveBackTranslationTarget(std::string_view type_source_language,
                                         std::string_view detected_source_language,
                                         std::string_view forward_target_language,
                                         std::string_view system_language_code);

// Phase 3 (REQ-007, plan §1.4): resolves the drag-context default target from
// the OS system language (REQ-007). F2 (session 260908_0003, decisions.md
// 2026-09-08 22:10 APPROVED OVERRIDE): returns the canonical name_en of the
// OS language 1:1 for EVERY supported language (EN OS -> "English", JA OS ->
// "Japanese", KO OS -> "Korean"...), superseding the former REQ-007 §2.6 EN->
// Korean special case, or "English" when the OS language is
// unsupported/unknown. Note: the translation-time src==tgt handling is the
// ADR-A1-7 neutral rule in ResolveEffectiveTarget (with the F2 user-explicit
// bypass); this default-target mapping only picks the INITIAL default value.
// Pure: read-only locale query, safe from any thread.
std::string ResolveDragDefaultTarget();

// Phase 4 (REQ-020, plan §1.2/§1.4): the four system-default language values,
// computed as a pure function of the OS locale. Used by the About-window
// "Reset to system defaults" button (main.cpp coordinator, Batch 3).
// drag_target comes from ResolveDragDefaultTarget() (REQ-007); the other three
// are the fixed defaults from REQ-006/015/016. Pure: read-only locale query,
// no config mutation, safe from any thread. Unit-tested headlessly
// (TestPhase4SystemDefaults).
struct SystemDefaultLanguages {
    std::string drag_source;  // "Auto Detect" (REQ-006)
    std::string drag_target;  // ResolveDragDefaultTarget() (REQ-007)
    std::string type_source;  // "Auto Detect" (REQ-015)
    std::string type_target;  // "English" (REQ-016)
};
SystemDefaultLanguages ComputeSystemDefaultLanguages();

// Cycles to the next target language given current code or name, wrapping around.
// Cycles the FULL registry target list (all 89 targets); the engine-filtered
// overload below is the Ctrl+F9-under-an-active-engine variant.
std::string CycleTargetLanguage(std::string_view current_code_or_name);

// 261116_0003 P2 (plan §4 "Routing": Ctrl+F9 cycles the active engine's list):
// same matching/wrapping semantics as the single-argument form, but the cycle
// set is the given engine's registry view (GetLanguagesForEngine(engine,
// false)). A current value absent from the view restarts at the view's first
// entry, exactly like the unfiltered form.
// 261116_0003 P2 fix round 2 (PR2-02 item 12, k3d1agent P3-1): DEFERRED to P6.
// 261116_0003 P8b: the deferral is CLOSED. Both Ctrl+F9 call sites now derive
// the active engine with the tray-menu idiom
// TrayLanguageEngineFor(TrayPreferredEngineIndex(snap.engine_type)) and pass
// it here: src/main.cpp's ApplyLanguageChange cycle_target branch and
// src/hook.cpp's KeyboardHook::CycleTargetLanguage unwired fallback. The
// hotkey therefore cycles exactly the view the language menus render (under
// "auto" the Google view). Pinned headlessly by TestP8bCtrlF9CycleWiring.
std::string CycleTargetLanguage(std::string_view current_code_or_name,
                                LanguageEngine engine);

// ---- R6 Phase 1 (B3): pure language-sync planner (architect plan §2.4/§2.5) ----
// The ApplyLanguageChange coordinator in src/main.cpp is the single authority
// for every language mutation (tooltip language menu, tray source/target
// submenus, tray Swap / badge double-click, startup config load). All of its
// decision logic lives HERE so it is unit-testable headlessly
// (TestB3LanguageSync); the coordinator consumes the plan verbatim.
//
// Given the currently persisted pair and a mutation request, the planner
// resolves the request (ISO code OR English OR native name, case-insensitive),
// canonicalizes it to the stored form (LanguageInfo::name_en), and reports
// which views the coordinator must refresh afterwards. AUTO resolves only for
// the SOURCE field: a target language is never auto-detect.
enum class LanguageSurface : unsigned char {
    Badge,    // FloatingBadge::SetLanguages (language label)
    Tray,     // SystemTray::UpdateStatus (icon tip + menu check marks)
    Tooltip,  // TooltipWindow::RefreshTargetLanguageFromConfig (best-effort)
};

struct LanguageSyncPlan {
    std::string source_language;  // canonical pair the coordinator persists via the I4 setters
    std::string target_language;
    // False = unresolvable request: the current pair is returned untouched and
    // the coordinator must mutate NOTHING and refresh no surfaces (INV-1 guard).
    bool valid = false;
    // False = the resolved pair already equals the persisted state: skip the
    // locked write and SaveToFile (no-op language re-pick must not churn disk),
    // but the view refreshes still run (self-heal against external drift).
    bool changed = false;
    // View refresh order after the (conditional) persist step: badge -> tray ->
    // tooltip. Empty unless valid. The calling surface re-renders its own
    // content afterwards (the coordinator cannot know its payload).
    std::vector<LanguageSurface> surface_updates;
};

// Phase 3 (plan §2.4): which language pair a mutation applies to. The tooltip
// language menu drives Drag; Ctrl+F9 cycle / tray submenus / swap drive Type.
enum class LanguageContext : unsigned char { Drag, Type };

// Empty new_* arguments mean "keep the current value". A no-request call
// (both empty) is the startup-alignment path: valid, unchanged, refresh-only.
// All-or-nothing: if either requested field fails to resolve, the whole
// mutation is rejected (a half-applied swap is worse than a refused one).
// The planner resolves a PAIR and stays context-agnostic: surface_updates
// still lists badge/tray/tooltip and the coordinator (src/main.cpp
// ApplyLanguageChange) filters the actual refreshes per context (plan §2.4).
LanguageSyncPlan PlanLanguageSync(LanguageContext ctx,
                                  std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target);

// Backward-compat wrapper (plan §3-Batch1): the pre-Phase-3 4-argument calls
// (src/main.cpp startup/cycle path, TestB3LanguageSync) mutate the TYPE pair,
// so they delegate to LanguageContext::Type verbatim.
inline LanguageSyncPlan PlanLanguageSync(std::string_view cur_source,
                                         std::string_view cur_target,
                                         std::string_view new_source,
                                         std::string_view new_target) {
    return PlanLanguageSync(LanguageContext::Type, cur_source, cur_target,
                            new_source, new_target);
}

// 261116_0003 P9a (plan §5 P9 "PlanLanguageSync raw-token allowance"): the same
// planner, extended with the config's REGISTERED user languages (D5). A token
// that does not resolve through the registry is accepted when it names a
// registered user language (stored form / native name / english name / resolved
// code) and canonicalizes to UserLanguageStoredName(entry). Guarantees kept:
//   * the allowance is SCOPED to the passed vector - the 4/5-argument overloads
//     stay registry-only, so every pre-P9a caller and pin is unchanged;
//   * an unregistered free-form token ("Elvish") is still refused
//     (valid=false, persist nothing, refresh nothing) - BuildPrompt's raw-token
//     injection is a PROMPT fallback, not a config-write allowance;
//   * AUTO stays source-only: the registry lookup runs first, so a user entry
//     that happens to be spelled "Auto Detect" cannot make an AUTO target valid;
//   * a persisted user-language pin canonicalizes through the same lookup, so a
//     no-op mutation reports changed=false and no disk churn.
LanguageSyncPlan PlanLanguageSync(LanguageContext ctx,
                                  std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target,
                                  const std::vector<UserLanguage>& user_languages);

// R6 Phase 4 (B2, architect plan §4.1 item 1+2) + Task 1 (session 260910_0004) + Condition C (Tencent 100% Official Full SFT template):
// Formats translation prompt for the Hy-MT2 model.
//
// target_lang / source_lang accept ANY form (ISO code, English name, or native
// name). The language name INJECTED into the instruction is form-matched to the
// template: the Chinese branch (将以下…) uses the native name
// (LanguageInfo::name_native, e.g. 简体中文) — an English name inside the
// localized instruction put the prompt out-of-distribution for non-EN targets
// (plan B2-H1: JA→ZH degraded to English output) — while the English branch
// ("Translate the following … text into …. Note that you should only output the translated result without any additional explanation:\n\n") uses the English name
// (LanguageInfo::name_en, e.g. Korean). Native names in the English template
// caused code-switching ("into 한국어" / "into Deutsch") and degraded the small
// local model's translation quality (Task 1). Unresolvable tokens (e.g. the
// bare word "Chinese", which is not a table entry) are injected raw,
// preserving the historical behavior.
//
// source_lang (optional): when it resolves to a real language (non-AUTO), the
// prompt names it in the same per-branch form (Chinese branch: 将以下日本語文本
// 翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\n; English branch: "Translate the following Japanese text
// into …. Note that you should only output the translated result without any additional explanation:\n\n"). AUTO / empty / unresolvable sources add NO source token, producing
// byte-identical prompts to the historical behavior (plan §4.2
// backward-compatibility requirement).
// REQ-CP T4 (session 260928_0001, design §8 + §9 row 6 + tech-gate A2): the
// worker-side prompt TEMPLATE REGISTRY. A template is a compiled-in instruction
// body selected by `prompt_template_ref` and rendered with the SAME
// {source}/{target}/{text} substitution BuildPrompt already does. Refs are a
// CLOSED enum-like set; unknown refs and the empty ref fall back FAIL-CLOSED to
// kPromptTemplateHymt2Official (the existing Tencent 100% Official Full SFT
// form), so a bogus or absent ref can never select a free-text / unsanctioned
// prompt. This keeps free user text out of the policy file and off the wire
// (design §6): only the ref travels, never the body.
//
// The three refs (verbatim from design §8):
//   * kPromptTemplateHymt2Official     — existing behavior, byte-identical,
//                                        incl. the AUTO/unresolvable degenerate
//                                        forms; ALSO the universal fallback.
//   * kPromptTemplateSubtitleRealtime  — design §8.2 (EmebalaListener): short
//                                        subtitle lines, anti-completion,
//                                        anti-embellishment, pass-through on
//                                        gibberish.
//   * kPromptTemplateLiteraryFlow      — design §8.3 (EmebalaReader slot
//                                        default): long-form on-screen reading.
//
// template_ref (optional, default {} = ""): selects the template. Per tech-gate
// amendment A2 the ref applies to the rung-1 chat form (BuildPrompt) ONLY; the
// rung-2 completion retry (BuildCompletionPrompt) is template-independent and
// keeps its single pinned form for ALL templates, preserving the REQ-059
// byte-identity golden pins.
inline constexpr const char* kPromptTemplateHymt2Official    = "hymt2-official";
inline constexpr const char* kPromptTemplateSubtitleRealtime = "subtitle-realtime-v1";
inline constexpr const char* kPromptTemplateLiteraryFlow     = "literary-flow-v1";

std::string BuildPrompt(std::string_view source_text,
                        std::string_view target_lang,
                        std::string_view source_lang = {},
                        std::string_view template_ref = {});

// REQ-059: the completion-form prompt (rung 2 of the fixed user-model
// ladder) for GGUF models whose chat template is trivial — the model
// family's official completion shape. Mirrors BuildPrompt's language
// resolution (ResolvePromptLanguage, name_en on both sides, AUTO/raw-token
// fallbacks) but emits two pinned forms:
//   resolved source: "Translate this from <src_en> to <tgt_en>:\n<src_en>: <text>\n<tgt_en>:"
//   AUTO / absent / unresolvable source: "Translate into <tgt_en>:\n<text>\n<tgt_en>:"
// The instruction language is always English (no Chinese branch — this form
// exists for Gemma-style third-party models, not the Hy-MT2 SFT template).
std::string BuildCompletionPrompt(std::string_view source_text,
                                  std::string_view target_lang,
                                  std::string_view source_lang = {});

// R6 Phase 4 (B2, architect plan §4.1 item 3): supported-pair policy for the
// LOCAL Hy-MT2 engine, revived as a REAL capability gate by 261116_0003 P2
// (plan §4 "Routing", decision D7). The rule reads the SAME
// LanguageInfo::engine_mask data the menus filter by (single source of
// truth — no second hardcoded list):
//   target in the Hy-MT2 official set AND (source AUTO OR source in the set).
// This REVIVES the auto->cloud branch of PlanTranslationRouting
// (src/engine.cpp): a pair the local model was never trained for (the 51
// cloud-only rows — the 8 pre-existing LO/HU/SV/EL/RO/DA/FI/NO plus the 43
// P3 tranche A+B rows) now routes to Google under
// engine_type=auto, and under an explicit local pin it stays on-device
// unless the user granted cloud consent. The F1 AUTO-source guard is kept
// as a SOURCE-side relaxation only (261116_0003 P2 fix round 2, PR2-04):
// Hy-MT2's built-in language ID decides for auto-detected text, so AUTO -> an
// OFFICIAL target stays local, while AUTO -> a cloud-only target is still
// unreliable (the target side is not the language ID's decision to make), and
// the target check runs FIRST to match plan §4's formula.
// History: F5 Phase 2 (session
// 260908_0003) had generalized EVERY pinned real pair to reliable after the
// EN-side conservative gate pushed pinned non-EN pairs onto the cloud-leak
// path; with the registry grown beyond the model's official set that
// blanket-true made the routing branch dead code (research-report §8 item
// 13), so the capability distinction returned. src/tgt accept any form
// (code or name). Pure function: the whole pair matrix is unit-tested
// (TestR6P4LanguageRouting).
bool LocalPairReliable(std::string_view src_code, std::string_view tgt_code);

// REQ-R11 (audit §4 M3): Directory containing the running executable
// (GetModuleFileNameW → parent_path). Falls back to the current path only if
// the module-path query itself fails, so callers never get an empty surprise.
std::filesystem::path GetExecutableDir();

// REQ-R11 (audit §4 M3): Resolve a possibly-relative model path against the
// EXECUTABLE directory instead of the current working directory. Run-registry
// autostart launches with CWD=C:\Windows\System32, where a CWD-relative
// "models/...gguf" can never exist: IsValidModelPath's regular-file check and
// llama_model_load_from_file (src/engine.cpp) both resolved relative paths
// against the CWD and the model never loaded. Contract:
//   * raw_path / base_dir are UTF-8 (the encoding config.json stores and the
//     encoding TranslationManager/llama.cpp expects); the return value is
//     UTF-8 too, so callers can never reintroduce a lossy ANSI conversion.
//   * empty raw_path -> empty string; absolute raw_path is returned lexically
//     normalised with base_dir ignored; relative raw_path is joined to
//     base_dir (default: GetExecutableDir()) and lexically normalised.
// Pure path arithmetic - no disk access - and base_dir is injectable, so it is
// unit-testable headlessly. main.cpp applies it at the single config→engine
// handoff, so BOTH validation time (IsValidModelPath sees an absolute path)
// and load time (llama gets an absolute path) become CWD-independent.
std::string ResolveModelPath(std::string_view raw_path,
                             std::string_view base_dir = {});

// REQ-208/SEC-1 (session 260911_0002 T3, design 144800 §2.3/§2.6 option (i),
// light-gate 165600/180000 recommendations a/b/c): the pure first-run privacy
// gate seam. TRUE while the privacy notice is still OUTSTANDING: wWinMain must
// fire the blocking first-run MessageBoxW, and because that popup sits BEFORE
// the TranslationManager construction, worker.Start() and hook.Start() in the
// startup order, no translation can be served until the notice is dismissed.
// FALSE once AppConfig::privacy_notice_shown was persisted true. Pulled out as
// a pure predicate so the blocking-popup and serving-block decisions can be
// pinned headlessly (a modal MessageBox cannot run in the test binary).
bool PrivacyNoticeOutstanding(bool privacy_notice_shown);

// Application configuration backed by JSON with zero external dependencies.
//
// I4 (data-race fix): this object is shared by reference between the UI (main)
// thread, the keyboard/mouse hook thread and the pipeline worker thread.
// Concurrency discipline, from smallest mechanism outward:
//   * bool toggles mutated at runtime from more than one thread
//     (auto_send, sound_enabled) are std::atomic.
//   * shared std::string fields (engine_type, source_language,
//     target_language, and the Phase 3 drag_/type_ language fields) and the
//     badge coordinates are guarded by mutex_ and must be read via
//     GetSnapshot() / written via the Set*() mutators once threads are running.
//   * SaveToFile serializes on the same mutex: previously two threads saving
//     concurrently clobbered each other's shared "config.json.tmp".
//   * drag_to_translate / cloud_fallback_enabled / model_path / hotkey_* are
//     only written at startup (before any thread exists) and read-only after,
//     so they need no locking.
struct AppConfig {
    // R6 Phase 6: ui_language becomes runtime-mutable (tray UI-language
    // selector). Like the other shared strings it must be written via
    // SetUiLanguage() and read via GetSnapshot().ui_language once threads
    // exist; direct field access is startup-only (before any thread exists).
    std::string ui_language = "auto";
    std::string ui_theme = "fluent"; // "fluent" (Direction A), "editorial" (Direction B), "monolith" (Direction C)
    std::string engine_type = "auto";
    std::string model_path = "models/Hy-MT2-1.8B-Q8_0.gguf";
    // REQ-045 P4-5 (item 3a-2, design §A.3): the registry id of the user's
    // third-party .gguf model ("" = the pinned Hy-MT2 default). Written ONLY by
    // the tray "사용자 선택(.gguf)…" registration pipeline (main.cpp) and read
    // by the engine host at ITS boot time (host_main.cpp g_user_model_id) —
    // never read by the Chat worker threads, so like cloud_fallback_enabled it
    // needs no mutex_ or Snapshot entry (startup/registration-time write,
    // read-only afterwards).
    std::string user_model_id = "";
    std::string source_language = "Auto Detect";
    std::string target_language = "English";
    // Phase 3 (REQ-006/007/015/016): context-separated language pairs.
    // Sticky model (REQ-008/009): a pair is "sticky" iff its key EXISTS in
    // config.json. Absent key => system default is computed at load.
    // Defaults below are the type-context defaults (REQ-016 target=English);
    // drag defaults are resolved at LoadFromFile time via the OS system
    // language (REQ-007), never as a compile-time constant.
    std::string drag_source_language = "Auto Detect";  // REQ-006: auto
    std::string drag_target_language = "English";      // placeholder; real default = OS lang (REQ-007)
    // F2 (session 260908_0003): true once the user has EXPLICITLY picked a
    // drag TARGET (tooltip/tray Drag-target menu -> ApplyLanguageChange).
    // Persisted as "drag_target_pinned" in config.json; false (key absent on
    // pre-F2 configs) means the drag_target_language is a never-touched AUTO
    // default that the ResolveEffectiveTarget collision pivot may still
    // adjust per request. Cleared by the About-window reset. Guarded by
    // mutex_ like the other shared config fields (runtime-mutated from the
    // GUI thread, snapshot-read by the drag/retranslate worker threads).
    bool drag_target_pinned = false;
    std::string type_source_language = "Auto Detect";  // REQ-015: auto
    std::string type_target_language = "English";      // REQ-016: English
    std::atomic<bool> auto_send{false};
    std::atomic<bool> sound_enabled{true};
    // 261108_0002 (WS3, v0.10.6): floating back-translation verification card
    // master switch (the tray "역번역 플로팅 표시" check row). Default FALSE -
    // the card costs a SECOND engine round trip per translation, so it stays
    // opt-in and the pre-261108 config.json (key absent) keeps it off. Read by
    // the pipeline worker thread through GetSnapshot() while the tray/GUI
    // thread writes it, hence std::atomic like auto_send/sound_enabled.
    std::atomic<bool> back_translation_enabled{false};
    bool drag_to_translate = true;
    // Privacy-first consent gate (H2 fix): when false, typed text is NEVER sent to
    // the Google Translate cloud — neither as a fallback after a local-LLM failure
    // nor as the auto engine when no local model is installed. Translation returns
    // empty instead (the worker already handles empty gracefully). Default false.
    bool cloud_fallback_enabled = false;
    // REQ-043 (plan §5.1-2): shared-inference-host routing block, persisted
    // as the nested "engine_host" object in config.json:
    //   "engine_host": { "enabled": true, "spawn": true, "idle_exit_ms": 600000,
    //                    "worker_idle_ms": 90000, "warmup": false }
    //   * enabled      — host-first routing at the local-engine seam. On ANY
    //                    host failure the app converges to the §V2-8.6 chain:
    //                    one-click repair -> consent-gated cloud (when enabled)
    //                    -> explicit unavailability notice. Default true: with
    //                    no host binary present local translation is DISABLED
    //                    (changed behavior — the app no longer silently skips
    //                    the local seam; §V2-8.6, REQ-043).
    //   * spawn        — CreateProcess the host on demand after a failed pipe
    //                    connect (plan §5.1-4), else retry against a running
    //                    host only.
    //   * idle_exit_ms — the HOST-side idle-exit budget (plan §4.4; default
    //                    600000 = 10 min). Protocol v1 has no configuration
    //                    channel, so the value is persisted here for
    //                    observability/forward-compat; the host currently
    //                    applies its own default (hidden --idle-exit-ms test
    //                    hook) — the value is STILL not forwarded.
    //   * worker_idle_ms / warmup (REQ-060) — translation-worker idle-eviction
    //                    budget (default 90000 ms) and the app's warmup mode
    //                    (default false). The two keys have DIFFERENT
    //                    consumers: the HOST boot read (engine_host_config
    //                    _reader.cpp) loads worker_idle_ms at startup (the
    //                    5 s floor / 24 h ceiling clamp applies); warmup is
    //                    never read by the host — it is a CLIENT-side decision
    //                    input: while warmup && enabled && the local route is
    //                    active, the client's WarmupManager holds a dedicated
    //                    pipe connection whose hello carries warmup:true, and
    //                    the host answers with an idle-eviction hold (+ a
    //                    preload session_open). Startup-only write on both
    //                    sides; no Snapshot entry needed (same discipline as
    //                    cloud_fallback_enabled).
    EngineHostConfig engine_host;

    // REQ-045 P4-3 (design §3b, item 3b): the persisted "openai" block —
    // OpenAI Compatible cloud engine (base_url / model / DPAPI-protected key
    // blob + SHA-256 integrity digest / plaintext-http consent). Written at
    // startup and by the settings dialog, read by the worker thread through
    // the engine; like cloud_fallback_enabled it is never mutated after
    // threads exist, so it needs no mutex or Snapshot entry.
    OpenAiConfig openai;

    // 261116_0003 P9c-1a: the optional "이 모델이 지원하는 언어" declaration for
    // the OpenAI-compatible endpoint - the CANONICAL set (registry codes,
    // registry order) the OpenAI language view filters by. Empty = NOT
    // DECLARED = the pre-P9c behavior (full generous set). Persisted as the
    // top-level "openai_declared_languages" array, emitted only while
    // non-empty (contract: the block above GetLanguagesForEngine). It is a key
    // adjacent to the "openai" block rather than a member of OpenAiConfig
    // because the OpenAiConfig wire struct is owned by
    // src/openai_compatible_client.hpp and this unit's file boundary is
    // config.*; semantically it still belongs to that engine only.
    //
    // WRITE RULE (same I4 discipline as the other locked fields): every runtime
    // write goes through SetOpenAiDeclaredLanguages(), which takes mutex_ and
    // republishes the engine-view snapshot under it. Direct assignment stays
    // legal only before any thread exists (startup seeding, unit tests) - and a
    // direct assignment does NOT publish, so a test that wants the views
    // filtered must call the locked verb (or PublishOpenAiDeclaredLanguages).
    // Reads on the GUI thread are direct (the settings dialog and the menus live
    // there), exactly like the openai block it sits next to.
    std::vector<std::string> openai_declared_languages;

    // 261122_0001 B-U1 (design-pick §1): the OpenAI-compatible profile library,
    // persisted as the optional top-level "openai_profiles" array (schema and
    // load/migration semantics documented at the OpenAiProfile declaration
    // above). Empty = the key is NOT emitted = the pre-261122_0001 posture.
    //
    // WRITER DISCIPLINE (same as the openai block it extends): GUI-thread-only
    // mutation (the settings dialog fold, unit U4), persisted by the single
    // caller-side SaveToFile (design-pick item 4). The pipeline worker reads
    // ONLY the active `openai` block above and never this library, so no mutex_
    // or Snapshot entry is needed. Direct assignment is legal only before any
    // thread exists (startup seeding, unit tests, the load path).
    std::vector<OpenAiProfile> openai_profiles;

    // 261116_0003 P9a (plan §3 D5/D6): the user-defined language registry and
    // its probe-verdict cache, serialized as "user_languages" +
    // "user_languages_version" (schema block above). Key absent on every
    // pre-P9a config.json => empty vector = "no user languages" (no migration,
    // no crash).
    //
    // 261116_0003 P9b-3 (PR9-12) WRITE RULE: every mutation of this vector goes
    // through the locked mutators declared below (Add/Update/RemoveUserLanguage
    // + the three probe-record verbs), which take mutex_ - the same lock
    // SaveToFile()/ToJsonString() hold while they serialise this vector. The
    // race PR9-12 records: the hook thread's Ctrl+F9 fallback and the fx worker
    // both call SaveToFile() off the GUI thread, so a GUI-side push_back while a
    // save iterated the vector was undefined behaviour. Direct assignment stays
    // legal only before any thread exists (startup seeding, unit tests), exactly
    // like the other locked fields.
    //
    // READS stay direct on the GUI thread (menu build / palette /
    // ApplyLanguageChange) and are unchanged: a returned LanguageMenuView points
    // INTO this vector, so a locked copy could not extend its lifetime, and the
    // GUI thread never reads concurrently with its own write. Off-thread readers
    // must go through a locked verb.
    //
    // While the forward-compat carrier is armed (HasUserLanguagesForwardCompatCarrier)
    // every write verb refuses: in-memory edits are DISCARDED on save because the
    // verbatim newer-schema block wins (contract at the top of this header).
    std::vector<UserLanguage> user_languages;

    // REQ-201/202 (session 260911_0002): MASTER switch for the diagnostic
    // log FILE itself. Default FALSE — release posture is NO log file at all:
    // Init() defers the file open and main.cpp applies this field through
    // diag::SetEnabled() right after LoadFromFile, so an opted-out user never
    // gets a logs\ file created. Set true to re-enable the full file sink for
    // debugging (all DIAG_LOG/DIAG_F behavior intact when enabled). Takes
    // effect on restart like the other startup-applied diag flags; no runtime
    // UI toggle in scope. Never mutated after threads exist => no mutex_ or
    // Snapshot entry needed.
    bool diag_log_enabled = false;
    // REQ-003 (session 260909): opt-in gate for writing USER CONTENT (typed
    // characters, foreground-window titles, captured text bodies, translation
    // output, local prompt bodies) into the diagnostic log. Default FALSE —
    // release posture is shape-only logging (lengths, codes, timings, window
    // class). Read ONCE at startup in main.cpp right after LoadFromFile and
    // handed to diag::SetContentLogging(); changing the value in config.json
    // takes effect on restart (no runtime UI toggle in scope). Like
    // cloud_fallback_enabled it is never mutated after threads exist, so it
    // needs no mutex_ or Snapshot entry. Independent of diag_log_enabled:
    // content=true with enabled=false still writes NOTHING (enabled is the
    // master switch — design 144800 §2.1 truth table).
    bool diag_log_content = false;
    // REQ-208/SEC-1 (session 260911_0002 T3): one-shot privacy-notice record.
    // Default FALSE — the key is absent on every pre-260911 config.json, so a
    // fresh install fires the first-run privacy popup (the blocking serving
    // gate; see PrivacyNoticeOutstanding and design 144800 §2.6 option (i)).
    // main.cpp records TRUE and SaveToFile()s BEFORE showing the popup, so a
    // crash while the notice is open cannot loop the popup on every launch
    // (light-gate 180000 recommendation b); a failed save therefore repeats
    // the notice, never skips it. Startup-only write (before worker/hook
    // threads exist), like cloud_fallback_enabled: no mutex_ or Snapshot
    // entry needed.
    bool privacy_notice_shown = false;
    // REQ-022 (Phase 6): gesture-pattern selector. Only "double_ctrl_c" is supported; other values fall back with a DIAG warning (hook.cpp Start()).
    std::string drag_hotkey = "double_ctrl_c";
    int badge_x = -1;
    int badge_y = -1;
    std::string hotkey_toggle = "F9";
    // REQ-022 (Phase 6): wired to the language-cycle trigger (F-18) via ResolveLangFromConfig at hook Start().
    std::string hotkey_lang = "Ctrl+F9";
    // REQ-022 (Phase 6): wired to the auto-send-toggle trigger (F-04) via ResolveModeFromConfig at hook Start().
    std::string hotkey_mode = "Ctrl+Shift+Enter";
    // Task 2 (translation tuning) → Batch 2 empirical grid (150 runs, session
    // 260913_0002 CPO scoring 165411 §7): default lowered 0.7 → 0.0. Greedy
    // decoding (engine.cpp temp<=0.001 branch) measured highest source↔target
    // fidelity with zero hallucinations and byte-stable repeatability for the
    // 1.8B model in a real-time messenger context. Tencent's official safety
    // rails below keep top_p 0.6 / top_k 20. v0.10.7 (261111_0001, U4): the
    // shipped repetition_penalty default is now 1.0 (penalty-free greedy first
    // pass; the engine retries once with 1.10 only when the repetition
    // detector fires), because an unconditional 1.05 mutated recurring
    // domain terminology. Existing config.json files keep their stored
    // value; new installs get 1.0.
    float temperature = 0.0f;
    float top_p = 0.6f;
    int top_k = 20;
    float repetition_penalty = 1.0f;

    // I4: point-in-time, thread-safe copy of the fields read by hook/worker threads.
    struct Snapshot {
        std::string engine_type;
        std::string source_language;   // legacy, kept for migration
        std::string target_language;   // legacy, kept for migration
        std::string drag_source_language;  // Phase 3
        std::string drag_target_language;  // Phase 3
        bool drag_target_pinned = false;   // F2 (session 260908_0003)
        std::string type_source_language;  // Phase 3
        std::string type_target_language;  // Phase 3
        std::string ui_language; // R6 Phase 6: selector read-back (test seam)
        std::string ui_theme = "fluent";
        bool auto_send = false;
        bool sound_enabled = true;
        // 261108_0002 (WS3): the pipeline worker's gate for the floating
        // back-translation card (worker.cpp reads snap.back_translation_enabled).
        bool back_translation_enabled = false;
        bool drag_to_translate = true;
    };

    // I4: thread-safe snapshot of shared fields (takes mutex_).
    Snapshot GetSnapshot() const;

    void SetBadgePosition(int x, int y);

    // I4: locked mutators for the shared string fields. Direct assignment is
    // only permitted at startup before worker/hook threads exist (unit tests,
    // wWinMain initialization) - runtime code must go through these.
    void SetEngineTypeName(std::string value);
    void SetSourceLanguage(std::string value);
    void SetTargetLanguage(std::string value);
    void SetLanguages(std::string source, std::string target);
    // Phase 3 (plan §2.3): locked mutators for the context-separated pairs.
    // Both fields of one context update atomically under mutex_ (same pattern
    // as SetLanguages) so a snapshot never sees a half-applied pair.
    void SetDragLanguages(std::string source, std::string target);
    void SetTypeLanguages(std::string source, std::string target);
    // F2 (session 260908_0003): locks the "user explicitly pinned the drag
    // target" provenance flag. Kept as a separate locked write (not folded
    // into SetDragLanguages) so startup seeding, migration and the reset
    // coordinator can write the pair WITHOUT pinning, while the explicit-pick
    // coordinator (ApplyLanguageChange) pins alongside its pair write. Read
    // via GetSnapshot() once threads are running.
    void SetDragTargetPinned(bool pinned);
    // R6 Phase 6: locked mutator for the tray UI-language selector (same
    // discipline as SetEngineTypeName; SaveToFile() serializes under mutex_).
    void SetUiLanguage(std::string value);
    void SetUiTheme(std::string value);
    static std::string NormalizeUiTheme(std::string_view value);
    // REQ-045 P4-5 (item 3a-2): locked mutator for the third-party .gguf
    // registry id (same discipline as SetEngineTypeName; SaveToFile()
    // serializes under mutex_). Written by the tray registration pipeline on
    // the GUI thread; read by the engine host at ITS boot (never by the Chat
    // worker threads), so no Snapshot entry is needed.
    void SetUserModelId(std::string value);

    // 261116_0003 P9c-1a: locked write of the OpenAI declared-language set
    // (same discipline as SetUserModelId - takes mutex_, so a save that is
    // serialising the config cannot observe a half-applied set). The input may
    // be any spelling (code / English / native); SanitizeOpenAiDeclaredLanguages
    // canonicalizes it, stores the canonical form and publishes it to the
    // engine-view filter, so the NEXT menu build already honours the edit. An
    // empty vector clears the declaration (the OpenAI views return to the full
    // generous set). Returns the stored canonical set.
    std::vector<std::string> SetOpenAiDeclaredLanguages(std::vector<std::string> declared);

    // 261116_0003 P9b-3 (PR9-12): locked write surface for user_languages (the
    // member contract above). Every verb takes mutex_, so a GUI edit can never
    // interleave with the hook-thread / fx-worker SaveToFile() that serialises
    // the same vector under the same lock. Validation happens INSIDE the lock
    // (PR9-11): a name that is reserved or already owned is refused here, so no
    // write path - dialog, future surface or unit test - can create a dead
    // menu row.
    //   AddUserLanguage    - validates, cleans, appends (RejectedCapacity at
    //                        kMaxUserLanguages).
    //   UpdateUserLanguage - validates the replacement's names against the
    //                        OTHER entries, then replaces at `index`. The
    //                        caller passes the complete replacement entry
    //                        (usually a copy of the stored one whose typed
    //                        fields changed), so the probe cache rides along.
    //   RemoveUserLanguage - erases at `index`.
    // The probe-record verbs address the entry by its STORED name
    // (UserLanguageStoredName, the menu dispatch payload) so a worker thread
    // that finished a probe can record the verdict without holding a pointer
    // into the vector. The probe's engine call itself must run OFF the GUI
    // thread (TranslationManager::Translate blocks, engine.hpp:401-408); only
    // this record step takes the lock.
    UserLanguageWriteResult AddUserLanguage(UserLanguage language);
    UserLanguageWriteResult UpdateUserLanguage(std::size_t index, UserLanguage language);
    UserLanguageWriteResult RemoveUserLanguage(std::size_t index);
    UserLanguageWriteResult RecordUserLanguageProbeResultFor(
        std::string_view stored_name,
        std::string_view engine,
        std::string_view model,
        const ProbeEvaluation& evaluation,
        std::string_view checked_at);
    UserLanguageWriteResult RecordUserLanguageProbeOverrideFor(
        std::string_view stored_name,
        std::string_view engine,
        std::string_view model,
        UserLanguageProbeStatus status,
        std::string_view checked_at);
    UserLanguageWriteResult ClearUserLanguageProbeOverrideFor(
        std::string_view stored_name,
        std::string_view engine,
        std::string_view model);
    // 261116_0003 P9b-3 (PR9-12 carrier rule): true while a newer-schema
    // "user_languages" block is preserved verbatim, i.e. while in-memory edits
    // would be DISCARDED on save (contract at the top of this header). A GUI
    // writer consults this to warn/refuse BEFORE offering an edit affordance;
    // the write verbs above enforce the same rule. Takes mutex_ (the carrier is
    // written by FromJsonString under it).
    bool HasUserLanguagesForwardCompatCarrier() const;

    // Returns standard default config path: %LOCALAPPDATA%\Emebalachat\config.json
    // (REQ-029-B single-source-of-truth), falling back to the executable dir /
    // working dir only when the Known-Folder query fails.
    static std::filesystem::path GetDefaultConfigPath();

    // REQ-029-B: canonical per-user config path
    // %LOCALAPPDATA%\Emebalachat\config.json. Returns an empty path when
    // SHGetKnownFolderPath(FOLDERID_LocalAppData) fails so callers can fall
    // back to the legacy exe-dir location.
    static std::filesystem::path GetLocalAppDataConfigPath();

    // Loads configuration from file. If file does not exist, saves defaults to disk.
    // If JSON is invalid, retains existing defaults without throwing.
    bool LoadFromFile(const std::filesystem::path& path = "");

    // Saves current configuration to JSON file. Thread-safe: serialized under mutex_.
    bool SaveToFile(const std::filesystem::path& path = "") const;

    // Serializes config to a formatted JSON string. Thread-safe (takes mutex_).
    std::string ToJsonString() const;

    // Deserializes config from a JSON string. Thread-safe (takes mutex_).
    bool FromJsonString(std::string_view json);

    // Advances target_language to the next supported language and updates state.
    std::string CycleLanguage();

private:
    // Guards engine_type/source_language/target_language, the Phase 3
    // drag_/type_ language fields, badge_x/badge_y and
    // serializes file writes. Non-recursive: the *Locked helpers below assume
    // the lock is ALREADY held and must never be called through public wrappers.
    mutable std::mutex mutex_;
    std::string ToJsonStringLocked() const;
    bool SaveToFileLocked(const std::filesystem::path& target_path) const;

    // 261116_0003 P9b-3: the forward-compat carrier state for callers that
    // ALREADY hold mutex_ (the public accessor takes it; the write verbs do).
    bool HasUserLanguagesForwardCompatCarrierLocked() const;

    // 261116_0003 P9a: forward-compat carrier for a "user_languages" block
    // written by a NEWER schema version (declared version > current). The raw
    // array text AND its declared version are re-emitted verbatim by the next
    // save instead of being parsed by rules that do not know it, so a downgrade
    // cannot silently drop (or half-reinterpret) the user's languages. Both are
    // empty/0 in the normal case (see kUserLanguagesSchemaVersion). Private
    // because they are serialization implementation details, not config state
    // any caller may read.
    std::string user_languages_forward_compat_json_;
    int user_languages_forward_compat_version_ = 0;
    // 261116_0003 P9a fix PR9-09: the ORIGINAL version key text when it could
    // not be parsed as a strict integer ("2junk", "abc", ...). Re-emitted as a
    // quoted, escaped JSON string so an unguessable version round-trips
    // byte-identically through the reader's ParseString instead of being
    // rewritten as a number this build never saw (the quoting is PR10-01; the
    // unquoted emission fabricated sibling keys). Empty when the carrier holds
    // a parsed integer version (or no carrier at all), except for the
    // empty-text case below.
    std::string user_languages_forward_compat_version_raw_;
    // 261116_0003 P9a fix2 PR10-02: true when the carrier's version came from
    // TEXT that is not a usable integer (unparseable, empty or
    // whitespace-only). The save then emits that text QUOTED (an empty text
    // stays as the quoted empty string), so the load/save cycle keeps
    // preserving the block instead of rewriting the version as the 0 sentinel
    // that the next load would read as "current version" and half-parse the
    // newer block as v1. False when the carrier holds a parsed integer version
    // (or no carrier at all).
    bool user_languages_forward_compat_version_text_ = false;
};

using Config = AppConfig;

} // namespace emebalachat
