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

// Empty new_* arguments mean "keep the current value". A no-request call
// (both empty) is the startup-alignment path: valid, unchanged, refresh-only.
// All-or-nothing: if either requested field fails to resolve, the whole
// mutation is rejected (a half-applied swap is worse than a refused one).
LanguageSyncPlan PlanLanguageSync(std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target);

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
//   * shared std::string fields (engine_type, source_language, target_language)
//     and the badge coordinates are guarded by mutex_ and must be read via
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
    std::atomic<bool> auto_send{false};
    std::atomic<bool> sound_enabled{true};
    bool drag_to_translate = true;
    // Privacy-first consent gate (H2 fix): when false, typed text is NEVER sent to
    // the Google Translate cloud — neither as a fallback after a local-LLM failure
    // nor as the auto engine when no local model is installed. Translation returns
    // empty instead (the worker already handles empty gracefully). Default false.
    bool cloud_fallback_enabled = false;
    std::string drag_hotkey = "double_ctrl_c";
    int badge_x = -1;
    int badge_y = -1;
    std::string hotkey_toggle = "F9";
    std::string hotkey_lang = "Ctrl+F9";
    std::string hotkey_mode = "Ctrl+Shift+Enter";
    float temperature = 0.7f;
    float top_p = 0.6f;
    int top_k = 20;
    float repetition_penalty = 1.05f;

    // I4: point-in-time, thread-safe copy of the fields read by hook/worker threads.
    struct Snapshot {
        std::string engine_type;
        std::string source_language;
        std::string target_language;
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
    // R6 Phase 6: locked mutator for the tray UI-language selector (same
    // discipline as SetEngineTypeName; SaveToFile() serializes under mutex_).
    void SetUiLanguage(std::string value);

    // Returns standard default config path: config.json next to executable or working dir.
    static std::filesystem::path GetDefaultConfigPath();

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
    // Guards engine_type/source_language/target_language/badge_x/badge_y and
    // serializes file writes. Non-recursive: the *Locked helpers below assume
    // the lock is ALREADY held and must never be called through public wrappers.
    mutable std::mutex mutex_;
    std::string ToJsonStringLocked() const;
    bool SaveToFileLocked(const std::filesystem::path& target_path) const;
};

} // namespace emebalachat
