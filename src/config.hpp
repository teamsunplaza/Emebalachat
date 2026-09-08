#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // purposes — cross-reference prevents drift. Non-empty for all 38 rows
    // (pinned by TestReq038B2RegistryBcp47). const char* per the design's
    // B-2 row: static-literal storage, no allocation, copy-safe.
    const char* bcp47;          // e.g. "en", "ko", "zh-CN", "ar", "fil", "my"
};

// Returns the full list of 38 language entries (AUTO + 37 supported languages).
const std::vector<LanguageInfo>& GetSupportedLanguages();

// Returns the list of 37 target languages (excluding AUTO).
const std::vector<LanguageInfo>& GetTargetLanguages();

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
//   2. English, when src is not English (EN forms a reliable local pair with
//      every non-EN source - LocalPairReliable);
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
std::string CycleTargetLanguage(std::string_view current_code_or_name);

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

// R6 Phase 4 (B2, architect plan §4.1 item 1+2): Formats translation prompt for
// the Hy-MT2 model.
//
// target_lang / source_lang accept ANY form (ISO code, English name, or native
// name). The language name INJECTED into the instruction is the native name
// (LanguageInfo::name_native, e.g. 简体中文), NOT the English name: injecting
// "Chinese Simplified" into the Chinese instruction put the prompt
// out-of-distribution for non-EN targets (plan B2-H1: JA→ZH degraded to
// English output). Unresolvable tokens (e.g. the bare word "Chinese", which is
// not a table entry) are injected raw, preserving the historical behavior.
//
// source_lang (optional): when it resolves to a real language (non-AUTO), the
// prompt names it (Chinese branch: 将以下日本語文本翻译为简体中文…; English
// branch: "Translate the following 日本語 segment into …"). AUTO / empty /
// unresolvable sources add NO source token, producing byte-identical prompts
// to the historical behavior (plan §4.2 backward-compatibility requirement).
std::string BuildPrompt(std::string_view source_text,
                        std::string_view target_lang,
                        std::string_view source_lang = {});

// R6 Phase 4 (B2, architect plan §4.1 item 3): supported-pair policy default
// list for the LOCAL Hy-MT2 engine. True only for pairs the model handles
// reliably without degrading to English. Conservative default per the plan:
// every pair involving English on either side (en↔*), which includes the
// user-confirmed-working AUTO→EN case (English output is the model's strongest
// behavior). zh↔ja was left open in the plan ("zh↔ja?") and Option A routes
// JA→ZH to Google, so it is EXCLUDED pending VP/user confirmation. src/tgt
// accept any form (code or name). AUTO is never a reliable TARGET. Pure
// function: the whole pair matrix is unit-tested (TestR6P4LanguageRouting).
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
    std::string engine_type = "auto";
    std::string model_path = "models/Hy-MT2-1.8B-Q8_0.gguf";
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
    bool drag_to_translate = true;
    // Privacy-first consent gate (H2 fix): when false, typed text is NEVER sent to
    // the Google Translate cloud — neither as a fallback after a local-LLM failure
    // nor as the auto engine when no local model is installed. Translation returns
    // empty instead (the worker already handles empty gracefully). Default false.
    bool cloud_fallback_enabled = false;
    // REQ-022 (Phase 6): gesture-pattern selector. Only "double_ctrl_c" is supported; other values fall back with a DIAG warning (hook.cpp Start()).
    std::string drag_hotkey = "double_ctrl_c";
    int badge_x = -1;
    int badge_y = -1;
    std::string hotkey_toggle = "F9";
    // REQ-022 (Phase 6): wired to the language-cycle trigger (F-18) via ResolveLangFromConfig at hook Start().
    std::string hotkey_lang = "Ctrl+F9";
    // REQ-022 (Phase 6): wired to the auto-send-toggle trigger (F-04) via ResolveModeFromConfig at hook Start().
    std::string hotkey_mode = "Ctrl+Shift+Enter";
    float temperature = 0.7f;
    float top_p = 0.6f;
    int top_k = 20;
    float repetition_penalty = 1.05f;

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
        bool auto_send = false;
        bool sound_enabled = true;
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
};

} // namespace emebalachat
