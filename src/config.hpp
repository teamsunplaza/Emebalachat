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

// Formats translation prompt for Hy-MT2 model.
std::string BuildPrompt(std::string_view source_text, std::string_view target_lang);

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
