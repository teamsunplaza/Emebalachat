#include "config.hpp"
#include "diag_logger.hpp"
#include "engine.hpp"
#include "gguf_validator.hpp" // 261111_0001 U6: registration-time GGUF header gate
#include "openai_compatible_client.hpp" // REQ-045 P4-3: OpenAI Compatible config block
#include "engine_host_bootstrap_client.hpp" // REQ-005 (M6 T6): startup repair bootstrapper
#include "vulkan_guard.hpp" // P5-F1: driverless-machine Vulkan pre-probe + stub guard
#include "hook.hpp"
#include "i18n.hpp"
#include "version.hpp"
#include "bidi_utils.hpp" // P4 Batch B-5: DirectionForLocale (cheat-sheet RTL)
#include "mouse_hook.hpp"
#include "single_slot_worker.hpp" // D2 (session 260910_0007): shared single-slot worker loops
#include "smart_bypass.hpp"
#include "translation_history.hpp" // V10F L2 (261122_0002 P2-G): 10-entry in-memory translation ring
#include "tray_toggle_debounce.hpp" // 260922_0001 A1: pure tray single-click debounce
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "user_language_probe.hpp" // 261116_0003 P9b-3 U3: first-use capability-probe runner
#include "win32_input.hpp"
#include "ui/about_window.hpp"
#include "ui/openai_settings_window.hpp" // REQ-045 P4-3: OpenAI settings dialog
#include "ui/settings_hub_window.hpp" // V10F UX-2 (261122_0002 P3-C3): the 4-tab settings hub (SetHubActions/OpenSettingsHub)
#include "ui/gguf_model_manager_window.hpp" // REQ-048 R2-D: user .gguf model manager
#include "ui/engine_host_settings_window.hpp" // REQ-060: translation-model load-mode settings dialog
#include "ui/model_download_dialog.hpp" // REQ-MD (260930_0004 D9): on-demand model download
#include "ui/user_language_dialog.hpp" // 261116_0003 P9b-3 U4a: the "내 언어 추가…" deferred dialog (U2)
#include "ui/user_language_manager_dialog.hpp" // 261116_0003 P9b-3 U4a: the "내 언어…" deferred dialog (U4b; the tray row is UserLangsGroup + "…")
#include "model_downloader.hpp"         // REQ-MD: missing-only predicate + plan
#include "ui/badge.hpp"
#include "ui/badge_transient.hpp" // REQ-013: transient drag-pair badge label flip
#include "ui/drag_icon.hpp"
#include "ui/dpi.hpp"
#include "ui/language_search_palette.hpp" // 261116_0003 P6c: kMsgOpenLanguagePalette consumer + picker open
#include "ui/theme.hpp"
#include "ui/tooltip.hpp"
#include "ui/translation_editor_window.hpp" // V10F L2 (261122_0002 P2-G): translation editor window contract
#include "ui/tray.hpp"
#include "worker.hpp"
#include "engine_host_registry.hpp" // REQ-045 P4-5: registry writer for user .gguf registration

#include <windows.h>
#include <commdlg.h> // REQ-045 P4-5: GetOpenFileNameW (.gguf picker)
#include <bcrypt.h>   // REQ-005 (M6 T6): CNG SHA-256 for the repair HashProvider
#include <objbase.h>
#include <wtsapi32.h> // WTSRegisterSessionNotification (REQ-R14)

#include <atomic> // REQ-004: in-flight guard for the tray-switch async preload
#include <chrono> // REQ-048 P1: bootstrap grace re-check interval
#include <condition_variable> // R6 Phase 3 (audit item 7): joinable drag worker
#include <cctype>  // REQ-045 P4-5: std::tolower (.gguf suffix strip)
#include <cstdio>
#include <deque>    // SEC-B5: language-sync request queue (no pointer in LPARAM)
#include <filesystem> // REQ-005 (M6 T6): CngSha256Hex path
#include <functional> // R6 Phase 1 (B3): language-sync coordinator std::function
#include <mutex>      // R6 Phase 3 (audit item 7): drag job slot guard; SEC-B5: queue guard
#include <optional>   // REQ-047 D3: single-slot pending OpenAiConfig for the deferred dialog open
#include <string_view>
#include <thread> // REQ-R1: drag-icon click worker (copy+translate off the GUI thread)
#include <utility>    // SEC-B5: std::move on the by-value queue transport
#include <vector> // REQ-005 (M6 T6): CNG hash buffers

#pragma comment(lib, "bcrypt.lib") // REQ-005 (M6 T6): CNG SHA-256 (repair HashProvider)
#pragma comment(lib, "comdlg32.lib") // REQ-045 P4-5: GetOpenFileNameW (.gguf picker)

namespace emebalachat {

namespace {
// ---------------------------------------------------------------------------
// REQ-050 Q2 (real-device findings): shared user-model stem helpers. The GUI
// entry TU is deliberately NOT linked into run_tests.exe, so both helpers are
// pinned by source inspection + a re-derived mirror predicate in the unit
// suite (TestReq047BundledReuseRejection precedent).
// ---------------------------------------------------------------------------

// files[0] -> stem: strip a trailing ".gguf" (the same case-sensitive rfind
// policy refresh_tray has always used; our registry writer only emits
// lowercase ".gguf", and a hand-edited casing difference degrades to a
// missed reuse, never a wrong reuse).
std::string GgufFileStem(std::string name) {
    const auto dot = name.rfind(".gguf");
    if (dot != std::string::npos && dot + 5 == name.size()) {
        name.resize(dot);
    }
    return name;
}

// Normalized reuse predicate for RegisterUserGgufModel's registry scan
// (duplicate-copies fix): the exact m.files[0] == bare_utf8 match missed our
// OWN collision-suffix convention, so re-adding a source after a suffix-bump
// (registered "MiLMMT…_2.gguf" vs re-picked source "MiLMMT….gguf") copied
// the multi-GB file again on every re-add (the reporter accumulated _2 AND
// _3). The entry is the SAME model when its stem equals the source stem, or
// the source stem + "_<digits>" (the _2/_3… bump this TU hands out). The
// bundled-origin gate inside the loop still runs first for bundled matches.
bool IsSameUserGgufStem(const std::string& entry_file, const std::string& source_stem) {
    const std::string entry_stem = GgufFileStem(entry_file);
    if (entry_stem == source_stem) return true;
    if (entry_stem.size() > source_stem.size() + 1 &&
        entry_stem.compare(0, source_stem.size(), source_stem) == 0 &&
        entry_stem[source_stem.size()] == '_') {
        for (size_t i = source_stem.size() + 1; i < entry_stem.size(); ++i) {
            if (entry_stem[i] < '0' || entry_stem[i] > '9') return false;
        }
        return true;
    }
    return false;
}

// REQ-051 U-2: the user-model display stem for the ENGINE name — the SAME
// resolution refresh_tray performs for the tray label (bound id -> registry
// files[0] stem; registry miss -> the raw id so the name never lies; empty
// binding -> the FIRST origin=="user" entry), so the tooltip/log engine name
// and the tray menu entry can never disagree. Returns "" when no user model
// is registered (the caller then keeps the pinned Hy-MT2 naming).
std::string ResolveUserModelStem(const std::string& user_model_id) {
    auto reg = engine_host_registry::LoadDefaultRegistry();
    if (reg.status != engine_host_registry::LoadStatus::Ok) {
        return {};
    }
    if (!user_model_id.empty()) {
        if (const auto* m = reg.registry.FindModel(user_model_id)) {
            if (!m->files.empty()) {
                return GgufFileStem(m->files[0]);
            }
        }
        return user_model_id; // registry miss: the raw id still never lies
    }
    for (const auto& m : reg.registry.models) {
        if (m.origin == "user" && !m.files.empty()) {
            return GgufFileStem(m.files[0]);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// REQ-045 P4-5 (item 3a-2, design §A.3/A.4): third-party .gguf registration.
// Invoked on the GUI thread from the tray engine submenu's
// "사용자 선택(.gguf)… > 파일찾기(.gguf)" pick. Steps:
//   1. §A.4 quality-not-guaranteed consent gate (once per app run).
//   2. GetOpenFileNameW (.gguf filter).
//   3. Copy the file to %LOCALAPPDATA%\Emebala\Common\models\ (bare filename,
//      "_2"/"_3"… suffix on collision; the SOURCE file is never moved/deleted).
//   4. Load registry.json, append an origin:"user" ModelEntry (id = the bare
//      filename stem; "_2" suffix when the id is taken by a DIFFERENT file),
//      write it back through SerializeRegistry (failures are LOUD — a damaged
//      or schema-rejected registry is NEVER overwritten).
//   5. Persist config.user_model_id AND switch the active engine to the
//      user-.gguf state (REQ-046 P4-2, Rev2 §B-2 + Tech Gate 결함-1/결함-2):
//      engine_type="user_gguf" + the runtime TranslationManager re-pointed at
//      LocalLlama IMMEDIATELY (the enum is resolved once at startup, so a
//      config-only write would leave routing on the previous engine). The
//      host reads engine_type/user_model_id at ITS next boot (idle-exit
//      respawn); the completion notice states that bound (C2).
// ---------------------------------------------------------------------------
void RegisterUserGgufModel(AppConfig& config, TranslationManager& engine) {
    // (1) §A.4 quality gate — once per app run (the pick itself is the
    // explicit user action, matching the §B "로컬LLM 명시 선택" trigger spirit).
    static bool s_quality_gate_shown = false;
    if (!s_quality_gate_shown) {
        const int consent = ::MessageBoxW(
            nullptr,
            I18n::Get(StringId::UserGgufQualityBody).c_str(),
            I18n::Get(StringId::UserGgufQualityTitle).c_str(),
            MB_YESNO | MB_ICONWARNING);
        if (consent != IDYES) {
            DIAG_F("MAIN/RegisterUserGguf/000: quality gate declined; registration aborted\n");
            return;
        }
        s_quality_gate_shown = true;
    }

    // (2) File picker.
    wchar_t file_buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"GGUF Model (*.gguf)\0*.gguf\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = I18n::Get(StringId::MenuBrowseGgufFile).c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!::GetOpenFileNameW(&ofn)) {
        // Cancel (or a commdlg error we do not distinguish — cancel is the
        // overwhelmingly common path; no loud failure on a plain dismiss).
        DIAG_F("MAIN/RegisterUserGguf/001: file picker cancelled or failed (commdlg err=%lu)\n",
               ::CommDlgExtendedError());
        return;
    }
    const std::filesystem::path src_path(file_buf);

    // (2b) 261111_0001 U6 (v0.10.7): registration-time GGUF header gate — the
    // same structural validation as the manager's add paths (RejectInvalidGguf),
    // so all three registration routes reject a truncated/corrupt/re-named file
    // identically before any copy or registry write.
    if (ggufvalidator::ValidateGgufFile(src_path, nullptr) !=
            ggufvalidator::GgufValidationResult::Ok) {
        ::MessageBoxW(nullptr, I18n::Get(StringId::GgufInvalidModelBody).c_str(),
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        return;
    }

    // (3) Resolve the destination: %LOCALAPPDATA%\Emebala\Common\models\.
    const std::filesystem::path models_dir =
        engine_host_registry::DefaultModelsDir();
    if (models_dir.empty()) {
        ::MessageBoxW(nullptr,
                      L"%LOCALAPPDATA% is unavailable; cannot locate the shared models directory.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/002: LOCALAPPDATA missing; registration aborted\n");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(models_dir, ec);
    if (ec) {
        ::MessageBoxW(nullptr,
                      L"The shared models directory could not be created.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/003: create_directories failed (ec=%d)\n", ec.value());
        return;
    }

    // Bare destination filename: stem + ".gguf"; on collision try _2, _3…
    // (same policy for the registry id so file<->id stay 1:1).
    const std::string bare_utf8 = ToUtf8(src_path.filename().wstring());
    std::string stem = bare_utf8;
    const std::string kGgufExt = ".gguf";
    // Case-insensitive .gguf suffix strip (the file picker filter already
    // restricts to *.gguf, but OFN_ALLOWMULTISELECT-less manual paths can
    // still arrive with any casing).
    if (stem.size() > kGgufExt.size()) {
        const std::string tail = stem.substr(stem.size() - kGgufExt.size());
        bool gguf_tail = true;
        for (size_t ci = 0; ci < kGgufExt.size(); ++ci) {
            if (std::tolower(static_cast<unsigned char>(tail[ci])) !=
                std::tolower(static_cast<unsigned char>(kGgufExt[ci]))) {
                gguf_tail = false;
                break;
            }
        }
        if (gguf_tail) stem.resize(stem.size() - kGgufExt.size());
    }
    if (!engine_host_json::IsBareFilename(bare_utf8)) {
        ::MessageBoxW(nullptr,
                      L"The chosen filename is not usable as a bare model filename.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/004: non-bare source filename (len=%zu)\n",
               bare_utf8.size());
        return;
    }

    // (4) Load the existing registry FIRST — a damaged/schema-rejected
    // document is a LOUD failure and is NEVER overwritten (위임 2c).
    auto reg_result = engine_host_registry::LoadDefaultRegistry();
    if (reg_result.status != engine_host_registry::LoadStatus::Ok &&
        reg_result.status != engine_host_registry::LoadStatus::Missing) {
        ::MessageBoxW(nullptr,
                      L"registry.json is damaged or has an unsupported schema. It was NOT modified. Fix or remove it, then retry.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/005: registry load status=%d; refusing to overwrite\n",
               static_cast<int>(reg_result.status));
        return;
    }
    engine_host_registry::Registry& registry = reg_result.registry;
    if (reg_result.status == engine_host_registry::LoadStatus::Missing) {
        registry.schema_version = engine_host_registry::kRegistrySchemaVersion;
    }

    // Re-selecting the SAME model reuses its existing entry (no duplicate
    // id, no second copy). REQ-050 Q2 (real-device finding — duplicate
    // copies): the match is STEM-NORMALIZED (IsSameUserGgufStem above), so a
    // suffix-bumped registration (registered "MiLMMT…_2.gguf" vs the
    // re-picked source "MiLMMT….gguf") reuses the entry instead of copying
    // the multi-GB file again (the reporter accumulated _2 AND _3).
    for (const auto& m : registry.models) {
        if (!m.files.empty() && IsSameUserGgufStem(m.files[0], stem)) {
            // REQ-047 D2 (design §B.3, diagnosis 235010): the registry match on
            // a BUNDLED file (e.g. the built-in Hy-MT2-1.8B-Q8_0.gguf) is the
            // reuse path that silently re-pointed config at the bundled id,
            // so the user's newly registered model never took effect. Reject
            // the pick with an informational notice; config / registry /
            // engine routing are left UNTOUCHED (early return).
            if (m.origin == "bundled") {
                DIAG_F("MAIN/RegisterUserGguf/006b: bundled entry re-pick refused (id=%s)\n",
                       m.id.c_str());
                ::MessageBoxW(nullptr,
                              I18n::Get(StringId::UserGgufBundledDuplicateBody).c_str(),
                              I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return;
            }
            // REQ-046 P4-2 (Tech Gate 결함-2, reuse path): the SAME engine
            // switch as the fresh-registration path below — config id + type
            // + runtime routing + persist.
            config.SetUserModelId(m.id);
            config.SetEngineTypeName("user_gguf");
            engine.SetEngineType(EngineType::LocalLlama);
            // REQ-051 U-2: the display stem rides the switch so the engine
            // name flips to the reused model immediately (stem from the
            // matched entry's own file).
            engine.SetUserModelDisplayStem(GgufFileStem(m.files[0]));
            config.SaveToFile();
            DIAG_F("MAIN/RegisterUserGguf/006: existing entry reused (id=%s)\n", m.id.c_str());
            ::MessageBoxW(nullptr,
                          I18n::Get(StringId::UserGgufRegisteredBody).c_str(),
                          I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                          MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    // Fresh registration: pick a non-conflicting (bare filename, id) pair.
    std::string dest_bare = bare_utf8;
    std::string model_id = "user-" + stem;
    for (int suffix = 2;; ++suffix) {
        const bool file_taken =
            std::filesystem::exists(models_dir / ToUtf16(dest_bare), ec);
        const bool id_taken = (registry.FindModel(model_id) != nullptr);
        if (!file_taken && !id_taken) break;
        // Same-id-different-file: bump the id. File collision alone: bump the
        // file name (and keep the ids unique alongside).
        dest_bare = stem + "_" + std::to_string(suffix) + kGgufExt;
        model_id = "user-" + stem + "_" + std::to_string(suffix);
        if (suffix > 1000) { // runaway guard (practically unreachable)
            DIAG_F("MAIN/RegisterUserGguf/007: suffix runaway; registration aborted\n");
            return;
        }
    }

    // Copy (never move): the user's original stays where it was.
    const std::filesystem::path dest_path = models_dir / ToUtf16(dest_bare);
    std::filesystem::copy_file(src_path, dest_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        ::MessageBoxW(nullptr,
                      L"The model file could not be copied into the shared models directory.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/008: copy_file failed (ec=%d)\n", ec.value());
        return;
    }

    // Append the origin:"user" entry and write the registry back.
    engine_host_registry::ModelEntry entry;
    entry.id = model_id;
    entry.family = "ggml-translate";
    entry.files = { dest_bare };
    entry.origin = "user";
    registry.models.push_back(std::move(entry));
    // M7 A-2 (session 260922_0001): persist through the ATOMIC writer —
    // registry.json is a family-shared multi-writer file (every Emebala
    // installer merges bundled entries into it; the manager window rewrites
    // it). A truncate-in-place ofstream exposed a torn-document window to
    // concurrent readers, so the write now lands as registry.json.tmp +
    // same-volume rename (config.cpp SaveToFileLocked precedent). Loud
    // failure discipline unchanged: SerializeRefused vs IoError surface
    // their own messages and the previous document is guaranteed intact.
    const auto write_outcome =
        engine_host_registry::WriteRegistryAtomically(registry, models_dir);
    if (write_outcome == engine_host_registry::WriteOutcome::SerializeRefused) {
        ::MessageBoxW(nullptr,
                      L"The registry could not be serialized (a filename was rejected). The model file was copied but NOT registered.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/009: SerializeRegistry refused (non-bare filename)\n");
        return;
    }
    if (write_outcome != engine_host_registry::WriteOutcome::Ok) {
        ::MessageBoxW(nullptr,
                      L"registry.json could not be written. The model file was copied but NOT registered.",
                      I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("MAIN/RegisterUserGguf/010: atomic registry write failed (IoError)\n");
        return;
    }

    // (5) REQ-046 P4-2 (Rev2 §B-2 + Tech Gate 결함-1): persist the pick AND
    // switch the active engine to the user-.gguf state immediately —
    // engine_type="user_gguf" for the host's boot-time C1 gate + the host's
    // user_model_id relay, and the runtime TranslationManager re-pointed at
    // LocalLlama NOW (the config->enum resolution happens once at startup,
    // so config-only persistence would leave routing on the previous
    // engine). The host applies the new model at its next boot (idle-exit
    // respawn, bounded by the C2 notice text).
    config.SetUserModelId(model_id);
    config.SetEngineTypeName("user_gguf");
    engine.SetEngineType(EngineType::LocalLlama);
    // REQ-051 U-2: the display stem rides the switch (the freshly stripped
    // source stem) so the engine name flips to the new model immediately.
    engine.SetUserModelDisplayStem(stem);
    config.SaveToFile();
    DIAG_F("MAIN/RegisterUserGguf/012: registered id=%s file=%s (origin=user)\n",
           model_id.c_str(), dest_bare.c_str());
    ::MessageBoxW(nullptr,
                  I18n::Get(StringId::UserGgufRegisteredBody).c_str(),
                  I18n::Get(StringId::UserGgufRegisteredTitle).c_str(),
                  MB_OK | MB_ICONINFORMATION);
}

// ---- R6 Phase 1 (B3): cross-thread language-sync marshal ----
// (everything below this line is inside the file-top `namespace {` which is
// inside `namespace emebalachat`; the anon namespace closes at the old
// "} // namespace" + "} // namespace emebalachat" pair further down)
// Phase 3 Batch 2 (plan §2.4): the request now also carries WHICH language
// pair (LanguageContext) the mutation applies to.
// Posted to the controller window (GUI thread) by any NON-GUI thread that
// needs a language mutation applied: the keyboard hook thread's Ctrl+F9
// cycle (Type context, cycle semantics) and the drag / double-Ctrl+C worker
// threads when their src==tgt fallback substitutes a new target (Drag
// context). SEC-B5 (CWE-822 shatter fix, release readiness review
// 260911_0002): the old contract shipped a heap LanguageSyncRequest* in
// LPARAM and cycle/context bits in wParam, and ControllerWndProc cast them
// back blindly. Both windows sharing that handler (the message-only
// controller AND the top-level power-sink window, discoverable via
// EnumWindows + the shipped class name) accept WM_APP+0x300 from any
// same-session process, so an attacker-chosen pointer could be dereferenced
// and freed. The message is now a PURE NOTIFICATION (wParam = LPARAM = 0,
// both ignored by the handler) and requests travel BY VALUE through the
// mutex-guarded deque below. See
// docs/260911_0002_session_release-public-readiness-review/
// 234100_verify-sec-blocker5-lparam-pointer.md for the threat model and the
// remediation spec this implementation follows.
constexpr UINT kMsgApplyLanguageSync = WM_APP + 0x300;
struct LanguageSyncRequest {
    LanguageContext context = LanguageContext::Type; // Phase 3: which pair to mutate
    std::string source;      // empty = keep current
    std::string target;      // empty = keep current
    bool cycle = false;      // Ctrl+F9 target cycle (old wParam bit0; SEC-B5
                             // moved the payload into the value struct)
    bool play_chime = false;
};
// Set once at startup to the wWinMain ApplyLanguageChange coordinator; invoked
// on the GUI thread from ControllerWndProc. Cleared after hook/mouse stop at
// shutdown so a late posted message can never call into destroyed state.
std::function<bool(LanguageContext, std::string_view, std::string_view, bool, bool)> g_apply_language_change;
// 261119_0001 P6-U5: the UI-language apply seam, published by wWinMain from the
// SAME body as trayCallbacks.on_select_ui_language (one planner, one persist,
// one refresh) and invoked on the GUI thread from ControllerWndProc when the
// shared picker commits on language_palette::PickerSurface::TrayUiLanguage.
// Cleared at shutdown with g_apply_language_change so a late picker commit can
// never call into destroyed state.
std::function<void(std::string_view)> g_apply_ui_language;
// 261116_0003 P9b-3 U3: the probe-completion drain, published by wWinMain to the
// ControllerWndProc kMsgUserLanguageProbeDone handler (the same pattern as
// g_apply_language_change: the WndProc cannot name wWinMain's locals). Cleared
// at shutdown with the other coordinators so a late wake-up can never call into
// destroyed state.
std::function<void()> g_drain_user_language_probes;

// REQ-047 D3 (design §C.2 안 C-1): the deferred OpenAI settings dialog resolves
// on the GUI thread in ControllerWndProc, which cannot name wWinMain's locals
// (config / engine / refresh_tray). Set once at startup to a wWinMain lambda
// holding the SAME save/switch body that used to run inline in
// on_select_engine(2); invoked on the GUI thread with the edited config when
// the dialog saved. Called with saved=true and the edited config on Save
// (returns true), or saved=false on Cancel (returns false, engine untouched).
// 261116_0003 P9c-1c: the third argument is the dialog's declaration fold —
// non-null ONLY on the Save path, where the body applies it on the LIVE config
// (through AppConfig::SetOpenAiDeclaredLanguages) BEFORE its save, so the file
// this run writes carries the declaration the dialog just folded. nullptr on
// Cancel (nothing was folded; a live declaration must survive a cancel), and
// never on kDeleted (that seam is g_apply_openai_deleted).
// 261122_0001 B-U5 (design-pick item 4): the fourth argument is the dialog's
// PROFILE LIBRARY fold — non-null ONLY on the Save path, where the body adopts
// the staged library on the LIVE config (config.openai_profiles, moved) BEFORE
// its save, so the file this run writes carries the library the dialog staged.
// The dialog performs no config file I/O for profiles (P9c-1c single-writer),
// so this save is the one persist site. nullptr on Cancel and never on
// kDeleted — the [설정 삭제] path preserves the stored library.
// Cleared at shutdown with g_apply_language_change so a late posted
// kMsgOpenOpenAiSettings can never call into destroyed state.
std::function<bool(const OpenAiConfig&, bool saved,
                   const std::vector<std::string>* declared,
                   OpenAiProfilesFold* profiles)>
    g_apply_openai_settings;
// V10F-R1 F-17: the http-consent ORIGIN seam — invoked on the GUI thread with
// the exact "host:port" scope the dialog's consent record was granted for,
// non-null ONLY on the kSaved path. The body applies it on the LIVE config
// through the locked verb AppConfig::SetHttpConsentOrigin BEFORE its save (the
// save g_apply_openai_settings performs right after), so the file carries the
// scope next to http_consent_given and the request-time origin gate sees the
// current value. A sibling seam (not a fifth g_apply_openai_settings argument)
// so the REQ-051-era call text the openai_profile_tests.inc B-U5 pin fixes
// stays byte-identical; the kDeleted seam clears the origin itself, and Cancel
// never fires this. Set once at startup to a wWinMain lambda (the
// g_apply_openai_settings sibling pattern). Cleared at shutdown with
// g_apply_openai_settings so a late posted kMsgOpenOpenAiSettings can never
// call into destroyed state.
std::function<void(const std::string&)> g_apply_openai_consent_origin;
// REQ-051 (Symptom D): the [삭제] kDeleted seam — invoked on the GUI thread
// with the dialog-cleared openai block when the user deleted the settings
// (ShowOpenAiSettingsDialogEx -> OpenAiSettingsOutcome::kDeleted). The body
// mirrors the cleared fields into the in-memory config (so the unconditional
// exit save cannot resurrect the deleted settings), re-points the runtime
// engine's openai block, persists, and refreshes the tray — WITHOUT touching
// engine_type / the engine selection (user-frozen scope). Set once at startup
// to a wWinMain lambda (the g_apply_openai_settings sibling pattern, since
// ControllerWndProc cannot name wWinMain's locals). Cleared at shutdown with
// g_apply_openai_settings so a late posted kMsgOpenOpenAiSettings can never
// call into destroyed state.
std::function<void(const OpenAiConfig&)> g_apply_openai_deleted;
// REQ-048 R2-D: the deferred gguf-model-manager dialog resolves on the GUI
// thread in ControllerWndProc, which cannot name wWinMain's locals (config /
// engine / refresh_tray). Set once at startup to a wWinMain lambda that opens
// the manager dialog on g_hControllerWnd and then refreshes the tray label
// (rename/delete can move the engine check mark or the user-model stem — the
// apply_openai_settings cancel-refresh precedent). Cleared at shutdown with
// g_apply_openai_settings so a late posted kMsgOpenGgufManager can never call
// into destroyed state.
std::function<void()> g_open_gguf_model_manager;
// REQ-060 (session 261009_0001): the deferred load-settings dialog resolves on
// the GUI thread in ControllerWndProc, which cannot name wWinMain's locals
// (config / engine / refresh_tray). Set once at startup to a wWinMain lambda
// that opens ShowEngineHostSettingsDialog on g_hControllerWnd and, on OK,
// mirrors the edited engine_host block into the in-memory config, persists,
// re-pushes the block into the runtime engine (SetEngineHostConfig ->
// RefreshActiveEngine -> engine_host::WarmupSetState, so the warmup manager
// re-evaluates immediately), and refreshes the tray. Cleared at shutdown with
// g_open_gguf_model_manager so a late posted kMsgOpenEngineLoadSettings can
// never call into destroyed state.
std::function<void()> g_open_engine_host_settings;
// 261116_0003 P9b-3 U4a: the deferred user-language dialogs resolve on the GUI
// thread in ControllerWndProc, which cannot name wWinMain's locals (config /
// probe_service / refresh_tray). Set once at startup to wWinMain lambdas: the
// add dialog (U2) writes IN MEMORY only, so the lambda persists on kAdded and
// refreshes the tray; the manager dialog (U4b) returns kChanged when an
// override verb applied, so the lambda persists on kChanged (a retest verdict
// is NOT a change — U3's recorded callback owns that persist) and refreshes.
// Cleared at shutdown with g_open_engine_host_settings so a late posted
// kMsgOpenUserLangAdd / kMsgOpenUserLangManager can never call into destroyed
// state.
std::function<void()> g_open_user_lang_add;
std::function<void()> g_open_user_lang_manager;
// REQ-047 D3 (design §C.2 안 C-1 권장 저장소 (a), Tech Gate §D3 #2): the
// deferred OpenAI dialog's GUI-thread state. Lives in namespace emebalachat
// (NOT the anon namespace) so both ControllerWndProc and the wWinMain tray
// lambda can name it. Both producers/consumers run on the GUI thread, so a
// plain bool + std::optional slot is race-free without a mutex — the same
// reasoning as the SEC-ADJ (0,0) wake-up contract. s_openai_dialog_pending
// latches ONE deferred open (a second tray pick while one is already posted
// is coalesced); s_pending_openai_cfg carries the config snapshot the dialog
// edits in/out.
bool s_openai_dialog_pending = false;
std::optional<OpenAiConfig> s_pending_openai_cfg;
// 261122_0001 B-U5 (design-pick item 4): the profile LIBRARY snapshot rides
// the same deferred-open store as the openai block snapshot above. The tray
// dispatch (engine_idx == 2, the only producer) seeds it from
// config.openai_profiles by copy; the kMsgOpenOpenAiSettings handler moves it
// into the dialog's fold input. Same GUI-thread-only, latch-guarded contract
// as s_pending_openai_cfg — set ONLY while s_openai_dialog_pending is latched,
// consumed when the latch releases.
std::optional<std::vector<OpenAiProfile>> s_pending_openai_profiles;
// ---- SEC-B5: value-queue transport (replaces the LPARAM pointer contract) ----
// Producers (hook thread, REQ-R06 double-Ctrl+C worker, drag worker) emplace
// under the mutex, then PostMessageW the wake trigger. The sole consumer is
// the GUI thread (ControllerWndProc + the shutdown sweep at the identical old
// position: producers joined -> drain -> retire coordinator). A plain
// lock_guard poll-on-message is sufficient - the OS message is the wakeup,
// no condition_variable needed.
// TEST COVERAGE NOTE (SEC-B5): this queue lives in main.cpp, which is the
// GUI entrypoint translation unit and is deliberately NOT linked into
// run_tests.exe (CMakeLists: run_tests builds tests/run_tests.cpp against
// Emebalachat_core only). Push/pop-all-FIFO/drain semantics here are
// therefore pinned by compilation + the Release build + the existing pure-
// planner seam tests (TestB3LanguageSync et al.) rather than dedicated unit
// tests; extracting a queue helper into a core TU was rejected as a
// build-contortion outside the surgical-fix mandate (verification report
// 234100 §7 leaves it optional). The security property (LPARAM never cast)
// is enforced structurally: no reinterpret_cast of this message's LPARAM
// remains anywhere in the repo.
std::mutex g_lang_sync_mu;
std::deque<LanguageSyncRequest> g_lang_sync_queue;
// Moves out ALL pending requests in FIFO order (consumer-side helper).
std::deque<LanguageSyncRequest> DrainLanguageSyncQueue() {
    std::deque<LanguageSyncRequest> out;
    std::lock_guard<std::mutex> lk(g_lang_sync_mu);
    out.swap(g_lang_sync_queue);
    return out;
}
// Posts a sync request; never blocks (PostMessageW). Safe from hook threads.
// When the CALLER already is the controller window's GUI thread (e.g. a test
// invoking KeyboardHook::CycleTargetLanguage directly, or any future same-
// thread wiring), posting would defer behind the rest of the queue, so the
// coordinator runs inline instead - it is documented GUI-thread-only and
// re-entrant here (its config writes are locked, its view seams marshal).
void RequestLanguageSync(HWND hController, LanguageContext ctx,
                         std::string source, std::string target,
                         bool cycle, bool play_chime) {
    if (!hController) {
        DIAG_F("MAIN/LangSync/000: no controller window; language sync request dropped\n");
        return;
    }
    const DWORD gui_tid = ::GetWindowThreadProcessId(hController, nullptr);
    if (g_apply_language_change && gui_tid == ::GetCurrentThreadId()) {
        g_apply_language_change(ctx, source, target, cycle, play_chime);
        return;
    }
    // SEC-B5: the request travels BY VALUE through g_lang_sync_queue; the
    // posted message is a pure wake-up notification with zero parameters.
    LanguageSyncRequest req;
    req.context = ctx;
    req.source = std::move(source);
    req.target = std::move(target);
    req.cycle = cycle;
    req.play_chime = play_chime;
    {
        std::lock_guard<std::mutex> lk(g_lang_sync_mu);
        g_lang_sync_queue.push_back(std::move(req));
    }
    if (::PostMessageW(hController, kMsgApplyLanguageSync, 0, 0) == FALSE) {
        // Leave the entry queued (drain-safe, cannot leak or apply-after-
        // retire): the failed post means no NEW wake trigger, but every
        // later notification (next cycle, or the shutdown drain, which runs
        // after all producers are joined and BEFORE the coordinator is
        // retired) pops ALL queued entries, so this one is still consumed
        // exactly once. Rolling it back here would race with a concurrent
        // consumer that another producer's successful post already woke.
        DIAG_F("MAIN/LangSync/002: PostMessage language sync failed (GLE %lu); request left queued\n", ::GetLastError());
    }
}

// ---- REQ-045 P4-8 (item 1, design §B + Tech Gate Item 6): modal helper ----
// "Local engine unavailable" guidance as a MODAL (MessageBoxW MB_OK|MB_ICONINFORMATION|
// MB_TOPMOST). The user's Ask Gate condition 1 wording — "the first time it
// appears, it must stay up until the user clicks X" — rules out the transient
// tooltip that auto-hides after kMessageAutohideMs; a MessageBoxW has no
// auto-dismiss and stays up until the user clicks OK or the X caption button
// (A1: not auto-dismissible; A3: explicit close). MB_TOPMOST mirrors the §2.2.4
// cheat-sheet / P4-5 quality-gate modal so the notice is not hidden behind a
// maximized editor. Both call sites run AFTER the first-run privacy gate
// (main L845), so the modal can never precede privacy consent.
//
// Threading (Tech Gate Item 6 correction): the detached repair-thread path
// (bootstrap §V2-8.6 with a repair URL) is NOT on the GUI thread, so it cannot
// call MessageBoxW directly. Rev2 §B's "strdup title/body into LPARAM" was
// rejected by Tech Gate Item 6 — it would re-introduce the CWE-822 shatter
// the repo's SEC-ADJ / SEC-B5 fixes removed (any same-session process can
// post an attacker-chosen address to a window with a shipped class name; see
// tooltip.cpp L24-29 and the kMsgApplyLanguageSync block above). The house
// pattern is reused verbatim: the payload (title+body) travels BY VALUE in
// a mutex-guarded deque, and the posted message is a PURE (0,0) wake-up.
// No heap pointer crosses PostMessageW. The consumer is the GUI-thread
// ControllerWndProc handler below, plus the shutdown drain that mirrors the
// language-sync discipline (producers joined -> drain -> retire).
// REQ-MD (260930_0004, decisions.md D9): when the request carries the model
// offer flag (set only by the two notice sites that verified ONLY models/-
// components are missing), the "engine unavailable" notice becomes
// actionable: the consent+progress download dialog runs INSTEAD of the
// dead-end MB_OK. A completed download skips the notice entirely (the store
// now has the model; the host re-probes per request / respawns and picks it
// up). 260930_0004 D9 (fix): a DECLINED offer ("나중에" / cancel / dismissed
// failure state) now ALSO skips the dead-end notice for that beat — falling
// straight into the "reinstall" MessageBox after the user just declined the
// download sent users into pointless reinstalls. The next REAL translation
// attempt still surfaces the normal failure notice per the strict-failure
// rules. Only a non-offer request (or an offer whose model raced into place)
// falls through to the historical notice.
void ShowLocalEngineUnavailableModal(std::wstring_view title, std::wstring_view body,
                                     bool offer_model_download) {
    // R1-24 (docs mining N3): an unresolvable store directory (DefaultModelsDir()
    // empty, i.e. LOCALAPPDATA absent) makes every transfer fail on
    // FetchAndInstallModel's first gate, and the dialog's own disk-space probe
    // is skipped when the directory is empty, so its consent page would offer a
    // [Download] button that cannot possibly succeed. Keep the historical
    // notice in that state instead of a dead-end offer.
    const bool store_dir_resolvable =
        !emebalachat::engine_host_registry::DefaultModelsDir().empty();
    if (offer_model_download && !emebalachat::modeldownloader::ModelFilePresent() &&
        store_dir_resolvable) {
        if (emebalachat::OfferModelDownload(nullptr)) {
            return; // installed (or already present) — no notice warranted
        }
        // Declined (or the dialog's own failure state was dismissed): no
        // dead-end notice for this beat.
        return;
    }
    UINT type = MB_OK | MB_ICONINFORMATION | MB_TOPMOST;
    if (DirectionForLocale(I18n::GetCurrentLocale()) == TextDirection::RTL) {
        type |= MB_RTLREADING; // mirror the REQ-208 privacy-notice RTL policy
    }
    ::MessageBoxW(nullptr, std::wstring(body).c_str(), std::wstring(title).c_str(), type);
}

// ---- REQ-045 P4-8: SEC-ADJ modal-request marshal (mirrors SEC-B5) ----
// Distinct WM_APP band: tooltip owns 0x2xx, drag_icon 0x1xx, about 0x3xx,
// language-sync owns 0x300. 0x400 is free (verified against src/ui/*.hpp).
constexpr UINT kMsgEngineUnavailableModal = WM_APP + 0x400;

// ---- REQ-047 D3 (design §C.2 안 C-1, Tech Gate §D3 #1): deferred OpenAI
// settings-dialog open. The tray cmd dispatch runs while the TrackPopupMenuEx
// modality tear-down may still be in flight (the WM_NULL at tray.cpp is an
// ASYNC release signal), so entering DialogBoxIndirectParamW synchronously
// from on_select_engine(2) produced the "minimized window, no input" symptom
// (P2 root cause). The dialog open is therefore deferred to the ControllerWndProc
// handler below via a pure (0,0) wake-up — the same SEC-ADJ house pattern as
// kMsgEngineUnavailableModal (payload travels in a GUI-thread store, no heap
// pointer crosses PostMessageW). Distinct WM_APP band: 0x500 verified unused
// against drag_icon 0x1xx, tooltip 0x2xx, language_sync 0x300, about 0x3xx,
// engine_modal 0x400.
constexpr UINT kMsgOpenOpenAiSettings = WM_APP + 0x500;

// ---- REQ-048 R2-D: deferred gguf-model-manager open ------------------------
// The tray engine submenu's "모델 관리…" row (tray.cpp, plain item) posts this
// pure (0,0) wake-up after its TrackPopupMenuEx dispatch — the identical
// modality-avoidance contract as kMsgOpenOpenAiSettings above (REQ-047 D3):
// the modal dialog opens only after the menu modality has fully torn down.
// Distinct WM_APP band: 0x501 verified unused (0x500 OpenAI settings, 0x400
// engine_modal, 0x300 language_sync, 0x3xx about, 0x2xx tooltip, 0x1xx
// drag_icon).
constexpr UINT kMsgOpenGgufManager = WM_APP + 0x501;

// ---- REQ-060 (session 261009_0001): deferred load-settings dialog open ------
// The tray engine submenu's "번역 모델 로드 설정…" row (tray.cpp, plain item)
// posts this pure (0,0) wake-up after its TrackPopupMenuEx dispatch — the
// identical modality-avoidance contract as kMsgOpenGgufManager above
// (REQ-047 D3): the modal dialog opens only after the menu modality has fully
// torn down. No coalescing latch is needed for the same reason as the manager
// row: the only producer is the tray dispatch on this GUI thread, and a modal
// dialog blocks that thread's message handling.
// Distinct WM_APP band: 0x502 verified unused (0x500 OpenAI settings, 0x501
// gguf manager, 0x400 engine_modal, 0x300 language_sync, 0x3xx about, 0x2xx
// tooltip, 0x1xx drag_icon).
constexpr UINT kMsgOpenEngineLoadSettings = WM_APP + 0x502;

// ---- 261116_0003 P9b-3 U4a: deferred user-language dialogs ------------------
// The tray main menu's "내 언어 추가…" / "내 언어…" rows (UserLangAddTitle /
// UserLangsGroup + "…", tray.cpp,
// ID_TRAY_USER_LANG_ADD / _MANAGE = 2865/2866) post these pure (0,0) wake-ups
// after their TrackPopupMenuEx dispatch — the IDENTICAL modality-avoidance
// contract as kMsgOpenGgufManager above (REQ-047 D3): the modal dialogs open
// only after the menu modality has fully torn down. No coalescing latch is
// needed for the same reason as the manager handler: the only producer is the
// tray dispatch on this GUI thread, and a modal dialog blocks that thread's
// message handling. Distinct WM_APP band: 0x504/0x505 verified unused (0x500
// OpenAI settings, 0x501 gguf manager, 0x502 load settings, 0x503 language
// palette — language_search_palette.hpp, 0x600 probe done — user_language_probe.hpp).
constexpr UINT kMsgOpenUserLangAdd = WM_APP + 0x504;
constexpr UINT kMsgOpenUserLangManager = WM_APP + 0x505;

struct EngineUnavailableModalRequest {
    std::wstring title;
    std::wstring body;
    // REQ-MD (260930_0004 D9): offer the on-demand model download from this
    // notice (set only when the missing-component set is models/-only).
    bool offer_model_download = false;
};
std::mutex g_engine_modal_mu;
std::deque<EngineUnavailableModalRequest> g_engine_modal_queue;

// ---- REQ-047 D1 (design §A.4, Tech Gate §D1 #2): streak latch --------------
// GUI-THREAD-ONLY state — REQ-047 P5-F1 (technical review 175600 finding #1,
// design §A.4 ⚠️): EVERY read/write of the latch now happens inside the
// kMsgEngineUnavailableModal drain handler below (the GUI thread). The
// pre-P5 worker callback wrapper used to test the latch BEFORE the GUI hop,
// which let the worker thread touch GUI-only state — the exact race design
// §A.4 warned about. The latch suppresses re-arming the modal while a
// previously surfaced failure is still unresolved: the drain handler sets it
// when a strict-failure modal is about to show, and clears it when the
// success sentinel (empty title+body) drains so a LATER recurrence re-arms
// the modal. Non-GUI producers (worker thread, drag/retranslate/dbl-Ctrl+C
// workers, detached repair thread) only ever ENQUEUE through
// RequestEngineUnavailableModal — the latch judgment itself lives exclusively
// in the drain handler, so no cross-thread synchronization is needed.
bool g_engine_modal_latched = false;
// Consumer-side drain helper: swaps out ALL pending requests in FIFO order.
std::deque<EngineUnavailableModalRequest> DrainEngineModalQueue() {
    std::deque<EngineUnavailableModalRequest> out;
    std::lock_guard<std::mutex> lk(g_engine_modal_mu);
    out.swap(g_engine_modal_queue);
    return out;
}
// Posts a modal request from any thread; never blocks (PostMessageW). The
// posted message is a pure (0,0) wake-up; the request itself travels by value
// through g_engine_modal_queue. On post failure the entry stays queued
// (drain-safe) — the same accepted trade-off as RequestLanguageSync.
void RequestEngineUnavailableModal(HWND hController, std::wstring title,
                                   std::wstring body, bool offer_model_download = false) {
    if (!hController) {
        DIAG_F("MAIN/EngineModal/000: no controller window; modal request dropped\n");
        return;
    }
    const DWORD gui_tid = ::GetWindowThreadProcessId(hController, nullptr);
    if (gui_tid == ::GetCurrentThreadId()) {
        // Caller is already the GUI thread (e.g. the tray on_select_engine
        // path and the bootstrap main-thread branch): run the modal inline,
        // matching RequestLanguageSync's same-thread fast path.
        ShowLocalEngineUnavailableModal(title, body, offer_model_download);
        return;
    }
    EngineUnavailableModalRequest req;
    req.title = std::move(title);
    req.body = std::move(body);
    req.offer_model_download = offer_model_download;
    {
        std::lock_guard<std::mutex> lk(g_engine_modal_mu);
        g_engine_modal_queue.push_back(std::move(req));
    }
    if (::PostMessageW(hController, kMsgEngineUnavailableModal, 0, 0) == FALSE) {
        // Leave the entry queued (drain-safe, cannot leak or apply-after-
        // retire): the shutdown drain below runs after all producers are
        // joined and pops ALL queued entries. Rolling back here would race
        // with a concurrent consumer that another producer's successful post
        // already woke — same contract as RequestLanguageSync.
        DIAG_F("MAIN/EngineModal/002: PostMessage engine-unavailable modal failed (GLE %lu); request left queued\n",
               ::GetLastError());
    }
}

const wchar_t kControllerClassName[] = L"Emebalachat_ControllerWindowClass";
// REQ-R14: WM_POWERBROADCAST is sent ONLY to top-level windows - a
// message-only window never receives power broadcasts. This tiny hidden
// top-level window (WS_EX_TOOLWINDOW, zero-size, off-screen) exists purely
// so the same ControllerWndProc sees resume events; WTS session
// notifications are registered on it as well.
const wchar_t kPowerSinkClassName[] = L"Emebalachat_PowerSinkWindowClass";
// W5 (R1-22, session 261029_0001): the cursor bubble used to label the toggle
// hotkey with a hardcoded L"F9", which lied as soon as config.hotkey_toggle was
// remapped. The label is now formatted from the RESOLVED spec at bubble time
// (KeyboardHook::HotkeySpecToDisplayString over ResolveToggleFromConfig - the
// exact seam Start() compiles the hook from), so the bubble and the swallowed
// combo can never drift. See the active-change callback wiring below.
HWND g_hControllerWnd = nullptr;
HWND g_hPowerWnd = nullptr; // REQ-R14 top-level sink for WM_POWERBROADCAST
SystemTray* g_pTray = nullptr;
KeyboardHook* g_pHook = nullptr;
FloatingBadge* g_pBadge = nullptr;
MouseHook* g_pMouseHook = nullptr; // REQ-R14 resume/unlock re-registration
// REQ-005 (M6 T6): stable address of the startup-tooltip for the detached
// repair thread (main() is a fixed frame; the pointer is set right after
// tooltip.Create and cleared at shutdown, mirroring g_pBadge).
TooltipWindow* g_pTooltip = nullptr;

// ---- V10F E2 (261122_0002 P3-C4 wave-B routing): LocalModelMissing offers ---
// engine.cpp now reports TranslationStatus::LocalModelMissing truthfully from
// the strict-local convergence (P3-C4 E2, the header-inline
// StrictLocalFailureStatus), so the four strict-failure display sites can no
// longer dead-end on "files missing, reinstall" (or the transient retry text)
// when ONLY a model file is absent. Routing policy (the one-click offer):
//   * engine_type == "user_gguf"  -> the missing file is the USER-registered
//     .gguf, so the built-in download is the wrong target. 261123_0001
//     (handoff §1.2, Bug A) splits the host's lumped model_missing by the
//     pinned file's ACTUAL presence (the probe below + the header-inline
//     RouteUserGgufModelMissing decision table in engine.hpp): file MISSING
//     -> open the GGUF model manager EVERY time (a real state change, never
//     latched); file PRESENT (a load failure: an HF-incompatible model the
//     host cannot serve) -> open the manager ONCE per session
//     (g_gguf_manager_load_failure_shown), then degrade to a tooltip hint
//     (StringId::UserModelLoadFailedHint) so a repeated failure never
//     re-opens the modal on every translation.
//   * any other engine_type       -> re-check the component set; a models/-
//     ONLY miss posts the existing download OFFER (REQ-MD D9 flow, the same
//     offer the explicit tray Local pick uses), a binaries-involved miss
//     keeps the honest repair modal with NO offer.
// Callable from ANY thread (worker / reading workers / GUI): CheckComponents
// is filesystem probes, PostMessageW never blocks, and
// RequestEngineUnavailableModal is the established thread-safe seam.
//
// The user_gguf manager open is coalesced through g_gguf_manager_offer_posted:
// unlike the tray producer (GUI thread, self-blocking under its own modal),
// this helper's producers are WORKER threads that can post many offers while
// the manager modal runs; without the latch every queued wake-up would reopen
// the dialog after each close. The kMsgOpenGgufManager drain handler clears
// THAT latch AFTER the modal returns, so offers raised during the dialog
// coalesce into the one open. The drain handler does NOT clear
// g_gguf_manager_load_failure_shown: that second latch is the session-scoped
// load-failure one-shot and must survive manager closes (it resets only with
// the process).
std::atomic<bool> g_gguf_manager_offer_posted{false};
// 261123_0001 (handoff §1.2, Bug A): the sticky per-session latch for the
// LOAD-FAILURE class of the user_gguf model_missing (the pinned file EXISTS
// on disk but the host could not serve it: the HF-incompatible-model class;
// a spawn failure lands here too, indistinguishably on the client side).
// Set ONLY when the manager was opened for such a load failure; a file-missing
// open never arms it, so a real state change always re-opens the manager.
// SESSION-SCOPED: the flag dies with the process, which resets the one-shot on
// the next app start (the handoff's "한 세션에 한 번만, 이후 동일 상태면 툴팁
// 안내로 전환". Never cleared by the kMsgOpenGgufManager drain handler.
std::atomic<bool> g_gguf_manager_load_failure_shown{false};

// 261123_0001 (handoff §1.2, Bug A): is the pinned user-model FILE actually on
// disk? The probe re-derives the file-missing-vs-load-failure split the frozen
// host wire cannot send: LoadDefaultRegistry + FindModel(id) -> the binding's
// files[0] under DefaultModelsDir(), existence-checked (both std::filesystem).
// An empty id / an unreadable registry / an unbound id all answer FALSE: the
// manager is the right fix for every one of those, matching the decision
// table's never-latched arm. 261123_0001 R3: a non-bare files[0] (the engine
// repo's IsBareFilename contract re-checked at this consumption point) also
// answers FALSE, so the exists() probe can never stat outside
// DefaultModelsDir() whatever a future registry parser admits.
// Runs on worker threads; the registry read and
// the exists() probe are cheap filesystem calls, so nothing is cached across
// calls and the verdict can never go stale after a manager-side fix.
bool IsPinnedUserModelFilePresent(std::string_view user_model_id) {
    if (user_model_id.empty()) {
        return false; // no binding: opening the manager IS the fix
    }
    auto reg = engine_host_registry::LoadDefaultRegistry();
    if (reg.status != engine_host_registry::LoadStatus::Ok) {
        return false; // unreadable/damaged registry: treat like file-missing
    }
    const auto* model = reg.registry.FindModel(std::string(user_model_id));
    if (model == nullptr || model->files.empty()) {
        return false; // unbound id: the manager is the right fix
    }
    const std::filesystem::path models_dir = engine_host_registry::DefaultModelsDir();
    if (models_dir.empty()) {
        return false;
    }
    // 261123_0001 R3 (security-reviewer LOW-1, two seats): re-check the bare
    // filename contract at the CONSUMPTION point. The engine repo's parser
    // (engine_host_registry.cpp) and its writer both reject a non-bare files[]
    // entry today, so this path cannot escape DefaultModelsDir(); the guard
    // keeps that property if a future parser ever drifts. A non-bare entry
    // (separator, drive colon, control char, "."/"..") answers FALSE, the same
    // conservative arm as a missing file: the manager is the right fix.
    if (!engine_host_json::IsBareFilename(model->files[0])) {
        return false;
    }
    std::error_code ec;
    const bool present = std::filesystem::exists(models_dir / model->files[0], ec);
    return present && !ec;
}

void RequestModelMissingOfferOrModal(std::string_view engine_type,
                                     std::string_view user_model_id) {
    if (engine_type == "user_gguf") {
        // 261123_0001 (handoff §1.2, Bug A): classify the miss BEFORE routing.
        // The host lumps file-missing / spawn failure / worker load failure
        // into one model_missing code; the file probe splits "the file is
        // gone" (manager every time) from "the file exists but will not
        // load" (manager once per session, tooltip hint afterwards).
        const bool file_present = IsPinnedUserModelFilePresent(user_model_id);
        const bool load_failure_shown =
            g_gguf_manager_load_failure_shown.load(std::memory_order_acquire);
        const UserGgufMissingRoute route =
            RouteUserGgufModelMissing(file_present, load_failure_shown);
        // 043 is the client-side discriminator for the three host causes: a
        // future session reads file_present (the file-missing fast-path vs the
        // spawn/load-failure pair) plus load_failure_shown (first vs repeated)
        // straight off this line; the host's own diag separates spawn failure
        // from worker load failure.
        DIAG_F("MAIN/EngineModal/043: user-gguf model-missing classified "
               "file_present=%d load_failure_shown=%d route=%s\n",
               file_present ? 1 : 0, load_failure_shown ? 1 : 0,
               route == UserGgufMissingRoute::kOpenManager ? "manager"
                                                           : "tooltip_hint");
        if (route == UserGgufMissingRoute::kShowTooltipHint) {
            // The same load-failure state recurred: quiet hint at the cursor,
            // NO manager reopen (g_pTooltip is the REQ-R10 thread-safe seam;
            // null only before tooltip creation / after shutdown, where the
            // DIAG line below still records the degradation).
            if (g_pTooltip != nullptr) {
                POINT cur;
                if (!::GetCursorPos(&cur)) {
                    cur = {0, 0};
                }
                g_pTooltip->ShowMessageThreadSafe(
                    cur.x, cur.y,
                    emebalachat::I18n::Get(emebalachat::StringId::UserGgufRegisteredTitle),
                    emebalachat::I18n::Get(emebalachat::StringId::UserModelLoadFailedHint));
            }
            DIAG_F("MAIN/EngineModal/044: user-gguf load-failure hint shown (manager already opened this session)\n");
            return;
        }
        bool expected = false;
        if (!g_gguf_manager_offer_posted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            DIAG_F("MAIN/EngineModal/040: user-gguf model-missing offer coalesced (manager already offered)\n");
            return;
        }
        // 261123_0001 R2 (fix F-02): the SESSION latch arms only AFTER the
        // open is known to be queued. Arming it before the post burned the
        // session's one load-failure manager open whenever PostMessageW failed
        // (only the offer latch rolled back), so every later identical failure
        // degraded to the tooltip hint without the user ever seeing the
        // manager. The coalescing latch above still covers the post -> arm
        // window: a racing producer's CAS fails there and coalesces.
        if (::PostMessageW(emebalachat::g_hControllerWnd,
                           emebalachat::kMsgOpenGgufManager, 0, 0) == FALSE) {
            g_gguf_manager_offer_posted.store(false, std::memory_order_release);
            DIAG_F("MAIN/EngineModal/041: user-gguf manager offer post failed (GLE %lu)\n",
                   ::GetLastError());
            return;
        }
        if (file_present) {
            // The manager is opening for a LOAD FAILURE, not a missing file:
            // arm the sticky per-session latch so the NEXT same-state failure
            // degrades to the tooltip hint instead of reopening the modal.
            g_gguf_manager_load_failure_shown.store(true, std::memory_order_release);
        }
        return;
    }
    namespace bs = emebalachat::engine_host_bootstrap;
    const bs::ComponentCheckResult check = bs::CheckComponents();
    const bool model_only =
        emebalachat::modeldownloader::MissingIsModelOnly(check.missing);
    DIAG_F("MAIN/EngineModal/042: model-missing offer evaluated engine=%s model_only=%d missing=%zu\n",
           std::string(engine_type).c_str(), model_only ? 1 : 0, check.missing.size());
    emebalachat::RequestEngineUnavailableModal(
        emebalachat::g_hControllerWnd,
        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody),
        model_only); // the offer flag: only a models/-only miss one-click-downloads
}

// ---- V10F P3-C2 (UX-3): the badge engine mini-tag token --------------------
// The RIGHT mini-tag chip the badge renders next to the pill (badge.hpp's
// SetEngineTag). One token per persisted engine_type value; the app only ever
// writes the four tokens + "auto" (every switch routes through the tray radio
// / the OpenAI save / the gguf registration), so the default arm covers the
// shipped "auto" and any unrecognized token degrades to the auto router's
// posture instead of hiding the chip.
const wchar_t* BadgeEngineTagFor(std::string_view engine_type) {
    if (engine_type == "local") return L"L";
    if (engine_type == "openai") return L"AI";
    if (engine_type == "user_gguf") return L"U";
    if (engine_type == "google") return L"G";
    return L"A"; // "auto" + the unreachable unknown-token arm
}

// ---- V10F P3 wave B: the dynamic cheat-sheet body ---------------------------
// The localized CheatSheetBody hardcodes the DEFAULT combos ("F9",
// "Ctrl + F9", "Ctrl + Shift + Enter"), so a user who remapped a hotkey in
// config.json got a guide that lied (the ux_and_longform_architecture U4
// finding). The body is therefore REBUILT at show time: the localized text
// stays the base, the default-combo tokens are substituted with the LIVE
// display strings (KeyboardHook::HotkeySpecToDisplayString over the same
// Resolve*FromConfig seams Start() compiles), and the gestures the static
// body never mentioned (selection translate, Double Ctrl+C, ESC cancel,
// badge-click cancel) are appended. Single left-to-right pass: needles are
// tried longest-first at each position and the scan advances PAST an inserted
// replacement, so a live value that itself contains a shorter needle's text
// (a remapped "Ctrl+Alt+F9" holding "F9") can never be re-matched. The toggle
// needle is boundary-guarded: an "F9" that is part of a longer combo ("Ctrl +
// F9") has a non-space, non-bullet predecessor and is skipped. Documented
// limitation: locales that spell the default modifiers in their own words
// (German "Strg + F9", French "Ctrl + Maj + Entrée") do not carry the English
// needle, so their UNREMAPPED body stays byte-identical (true) and their
// REMAPPED body keeps the localized default spelling for that line until the
// phase i18n pass keys placeholder strings (## I18N REQUESTS).
void SubstituteCheatSheetHotkeys(std::wstring& body,
                                 const std::wstring& live_toggle,
                                 const std::wstring& live_lang,
                                 const std::wstring& live_mode) {
    struct Needle {
        std::wstring tight;
        std::wstring spaced;
        const std::wstring* live;
        bool boundary; // whole-token guard (the bare toggle key)
    };
    auto spaced_form = [](const std::wstring& tight) {
        std::wstring s = tight;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'+') {
                s.replace(i, 1, L" + ");
                // Skip the inserted " + " (3 chars): its middle '+' must never
                // be re-processed, or the replace feeds itself forever.
                i += 2;
            }
        }
        return s;
    };
    const std::wstring d_toggle =
        emebalachat::KeyboardHook::HotkeySpecToDisplayString(
            emebalachat::KeyboardHook::kDefaultToggleHotkey);
    const std::wstring d_lang =
        emebalachat::KeyboardHook::HotkeySpecToDisplayString(
            emebalachat::KeyboardHook::kDefaultLangHotkey);
    const std::wstring d_mode =
        emebalachat::KeyboardHook::HotkeySpecToDisplayString(
            emebalachat::KeyboardHook::kDefaultModeHotkey);
    const Needle needles[] = {
        {d_mode, spaced_form(d_mode), &live_mode, false},
        {d_lang, spaced_form(d_lang), &live_lang, false},
        {d_toggle, std::wstring(), &live_toggle, true},
    };
    std::wstring out;
    out.reserve(body.size() + 64);
    std::size_t i = 0;
    while (i < body.size()) {
        bool matched = false;
        for (const Needle& n : needles) {
            std::size_t needle_len = 0;
            const auto try_one = [&](const std::wstring& needle) -> bool {
                if (needle.empty() ||
                    body.compare(i, needle.size(), needle) != 0) {
                    return false;
                }
                if (n.boundary) {
                    // the bare toggle key: refuse when the token continues a
                    // longer combo — walk back over spaces and reject '+',
                    // letters and digits ("Ctrl + F9", "Shift+F9")
                    std::size_t back = i;
                    while (back > 0 && body[back - 1] == L' ') --back;
                    if (back > 0) {
                        const wchar_t prev = body[back - 1];
                        if (prev == L'+' || (prev >= L'a' && prev <= L'z') ||
                            (prev >= L'A' && prev <= L'Z') ||
                            (prev >= L'0' && prev <= L'9')) {
                            return false;
                        }
                    }
                }
                needle_len = needle.size();
                return true;
            };
            const bool hit = try_one(n.tight) || try_one(n.spaced);
            if (hit && *n.live != n.tight) {
                out += *n.live;
                i += needle_len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out += body[i];
            ++i;
        }
    }
    body = std::move(out);
}

// 260922_0001 A1: armed state of the tray single-click debounce timer. The
// decision logic itself is the pure TrayToggleDebounceStateMachine() in
// tray_toggle_debounce.hpp (unit-testable: main.cpp is not linked into
// run_tests.exe); this flag only carries the armed state between WndProc hits.
bool g_tray_toggle_pending = false;

// ---- REQ-R14 (audit §5 latent item 2): hook lifecycle timers ----
// kTimerHookReinstall: debounced (coalesced) reinstall triggered by
// resume/unlock messages. kTimerHookHealth: periodic watchdog that catches
// the transitions with no message of their own (UAC secure-desktop trips).
constexpr UINT_PTR kTimerHookReinstall = 5001;
constexpr UINT_PTR kTimerHookHealth = 5002;
// 260922_0001 A1: tray single-click debounce timer. Isolated from 5001/5002 so
// the hook-lifecycle watchdog and the tray debounce can never collide.
constexpr UINT_PTR kTimerTrayToggle = 5003;

// R1-15b (env research F2): watchdog ticks elapsed since the last periodic
// self-heal reinstall. WndProc-owned (GUI thread only), advanced by the
// kTimerHookHealth tick through the pure PeriodicHookReinstallTick stepper in
// hook.hpp (which pins the cadence headlessly: 5 min / 30 s = 10 ticks).
// main.cpp is not linked into run_tests.exe, so the counter lives here and the
// decision logic stays in the header.
unsigned g_hook_health_ticks = 0;

// ---- V10F P3-C1 wave B: the OpenAI instant-switch gate ----------------------
// The engine submenu's radio row (ID_TRAY_ENGINE_OPENAI -> on_select_engine(2))
// is the INSTANT switch: when the persisted openai block already names a
// usable profile (endpoint + model + credential), picking the row switches
// engine_type WITHOUT opening the settings modal (the P3-C1 UX-1 split; the
// modal moved to its own row, on_open_openai_settings). Only the NO-profile
// case routes into the dialog. "Configured" mirrors what the engine's OpenAI
// leg needs to attempt a request: a base URL, a model id, and a credential
// (either the DPAPI blob or, for legacy/keyless-migrated blocks, the digest).
bool OpenAiProfileConfigured(const emebalachat::OpenAiConfig& cfg) {
    return !cfg.base_url.empty() && !cfg.model.empty() &&
           (!cfg.api_key_dpapi.empty() || !cfg.api_key_sha256.empty());
}

// ---------------------------------------------------------------------------
// V10F (261122_0002 P2-F): the unified active-reading registry.
// The four reading paths (drag icon, tooltip re-translation, floating
// back-translation, double-Ctrl+C) run on their own worker threads, and the
// Enter pipeline runs on the pipeline worker. Before this registry the ONLY
// cancel surface was the pipeline's busy flag (badge click / nothing else);
// Esc dismissed cards while a multi-second reading translation kept running.
// The registry gives every reading job type a per-type cancel token + running
// flag so BOTH Esc and the badge single-click can cancel EVERYTHING that is
// in flight. Pure STL discipline (same precedent as GgufFileStem above): the
// decision rules are header-visible inline functions below, unit-tested as
// re-derived mirrors in tests/p2_f_tests.inc (main.cpp is not linked into
// run_tests.exe); wWinMain owns the one instance.
// ---------------------------------------------------------------------------
struct ReadingJobRegistry {
    std::atomic<bool> drag_running{false};
    std::atomic<bool> drag_cancel{false};
    std::atomic<bool> retranslate_running{false};
    std::atomic<bool> retranslate_cancel{false};
    std::atomic<bool> back_running{false};
    std::atomic<bool> back_cancel{false};
    std::atomic<bool> dbl_running{false};
    std::atomic<bool> dbl_cancel{false};
    // Single-writer lease for the tooltip's shared chunk-preview surface
    // (V10F L3-B): the preview state machine carries NO generation guard, so
    // two concurrent jobs (a drag translation and a re-translation run on
    // separate workers BY DESIGN) must serialize through this CAS lease or
    // their Begin/Append ops would interleave into one corrupted card. The
    // job that loses the lease simply renders no live preview; its final card
    // still lands through the ordinary generation-guarded seams.
    std::atomic<unsigned long long> preview_lease{0};
};

// Job-start protocol, ONE rule: clear the STALE cancel token BEFORE
// publishing running, so a concurrent cancel-reader (Esc/badge on the hook
// threads) that observes running==true also observes a live, current-job
// token. Tokens are latched until the next start, never cleared mid-job.
inline void ReadingJobBegin(std::atomic<bool>& running, std::atomic<bool>& cancel) {
    cancel.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);
}

inline bool ReadingJobRunning(const std::atomic<bool>& running) {
    return running.load(std::memory_order_acquire);
}

inline bool AnyReadingJobRunning(const ReadingJobRegistry& r) {
    return ReadingJobRunning(r.drag_running) || ReadingJobRunning(r.retranslate_running) ||
           ReadingJobRunning(r.back_running) || ReadingJobRunning(r.dbl_running);
}

// Latch ALL per-job cancel tokens. Idempotent; each job clears its own token
// at start (ReadingJobBegin), so a latched token can never cancel a later
// job. The Enter pipeline is NOT covered here - the caller pairs this with
// PipelineWorker::RequestCancelTranslation().
inline void CancelAllReadingJobs(ReadingJobRegistry& r) {
    r.drag_cancel.store(true, std::memory_order_release);
    r.retranslate_cancel.store(true, std::memory_order_release);
    r.back_cancel.store(true, std::memory_order_release);
    r.dbl_cancel.store(true, std::memory_order_release);
}

// Chunk-preview lease primitives (pure CAS rules): Acquire succeeds only from
// the free state and returns the granted id (>0; 0 = held by someone else);
// Release is valid only for the current holder. A failed Acquire/Release
// never mutates the lease.
inline unsigned long long PreviewLeaseAcquire(std::atomic<unsigned long long>& lease,
                                              unsigned long long id) {
    unsigned long long expected = 0;
    return lease.compare_exchange_strong(expected, id, std::memory_order_acq_rel,
                                         std::memory_order_acquire)
               ? id
               : 0;
}
inline bool PreviewLeaseRelease(std::atomic<unsigned long long>& lease, unsigned long long id) {
    unsigned long long expected = id;
    return lease.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

// Scope guard for ONE job body: publishes the running flag via the begin
// protocol on construction and RELEASE-clears it on destruction (all four
// reading lambdas hold one for their whole body, however many early returns
// they take).
class ReadingJobScope {
public:
    ReadingJobScope(std::atomic<bool>& running, std::atomic<bool>& cancel)
        : running_(running) {
        ReadingJobBegin(running_, cancel);
    }
    ~ReadingJobScope() { running_.store(false, std::memory_order_release); }
    ReadingJobScope(const ReadingJobScope&) = delete;
    ReadingJobScope& operator=(const ReadingJobScope&) = delete;
private:
    std::atomic<bool>& running_;
};

// V10F L3-B (261122_0002 P2-F): one job's lease on the tooltip's shared
// chunk-preview surface. TryBegin runs ONLY for a genuine multi-chunk plan
// (ShowChunkProgress; single-chunk jobs never touch the preview, preserving
// the pre-V10F byte-identical behavior for short runs); Append is a no-op
// until the lease is held. End(false) RETIRES the card (the tooltip's
// ApplyChunkPreviewOp ends a failed preview with DismissExplicit), so a
// terminal branch that surfaces any failure notice/modal MUST call End(false)
// BEFORE that show; the destructor is the backstop for paths with no
// subsequent show (cancel arms, silent keeps).
class ChunkPreviewLease {
public:
    ChunkPreviewLease(ReadingJobRegistry& registry, TooltipWindow& tooltip)
        : registry_(registry), tooltip_(tooltip) {}
    ~ChunkPreviewLease() { End(false); }
    ChunkPreviewLease(const ChunkPreviewLease&) = delete;
    ChunkPreviewLease& operator=(const ChunkPreviewLease&) = delete;

    void TryBegin(int total_chunks) {
        if (owned_ || !emebalachat::ShowChunkProgress(static_cast<std::size_t>(total_chunks))) {
            return;
        }
        const unsigned long long id = NextLeaseId();
        if (PreviewLeaseAcquire(registry_.preview_lease, id) == 0) {
            return; // another job owns the surface; this run renders no preview
        }
        lease_id_ = id;
        owned_ = true;
        tooltip_.BeginChunkPreview(total_chunks);
    }
    void Append(const std::wstring& cumulative) {
        if (owned_) tooltip_.AppendChunkPreviewText(cumulative);
    }
    void End(bool success) {
        if (!owned_) return;
        owned_ = false;
        tooltip_.EndChunkPreview(success);
        PreviewLeaseRelease(registry_.preview_lease, lease_id_);
    }

private:
    static unsigned long long NextLeaseId() {
        static std::atomic<unsigned long long> next{1}; // 0 is the free marker
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    ReadingJobRegistry& registry_;
    TooltipWindow& tooltip_;
    unsigned long long lease_id_ = 0;
    bool owned_ = false;
};

// Re-register whichever LL hooks are installed, on the GUI thread (the only
// threads allowed to Start/Stop the hooks). Always reinstall on the event
// path: an OS silent-removal (LowLevelHooksTimeout during a desktop switch)
// leaves our HHOOK handle looking valid while the hook is dead, so the
// handle check is only the fast filter, never the decision. Reinstall()
// returns false (no-op) for hooks that are not running by design.
//
// This is the ONLY hook-lifecycle mutation site outside Start/Stop: the
// resume/unlock debounce timer and the R1-15b periodic self-heal both call
// here, on this thread, so a periodic tick can never race an event-driven
// reinstall into a double install.
void ReinstallHooksAfterLifecycleEvent(const char* reason, HWND debounce_wnd) {
    // KeyboardHook::Stop() joins the REQ-R06 async worker; if a double-Ctrl+C
    // translation is mid-flight (seconds), re-installing right now would
    // stall the GUI thread for its remainder. Defer: re-arm the debounce
    // timer once and retry after the body completes (bounded by the
    // inference itself - the worker is never stuck indefinitely).
    if (g_pHook && KeyboardHook::IsDoubleCtrlCBusy()) {
        ::SetTimer(debounce_wnd, kTimerHookReinstall, kHookReinstallDebounceMs, nullptr);
        return;
    }
    if (g_pHook) {
        if (!g_pHook->Reinstall()) {
            DIAG_F("MAIN/HookLifecycle/001: keyboard hook re-install failed after %s\n", reason);
        }
    }
    if (g_pMouseHook) {
        if (!g_pMouseHook->Reinstall()) {
            DIAG_F("MAIN/HookLifecycle/002: mouse hook re-install failed after %s\n", reason);
        }
    }
}

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // ---- REQ-R14: Sleep/Resume + Win+L session lifecycle ----
    if (ShouldReinstallHooksOnEvent(msg, wParam)) {
        // Coalesce bursts (RESUMEAUTOMATIC+RESUMESUSPEND, power+manual unlock):
        // one pending timer per window, refreshed by each event, fires ONE
        // reinstall. The debounce window is registered against the window that
        // saw the event so the reinstall (which also re-arms via that window on
        // a busy-worker deferral) uses the same timer owner.
        ::SetTimer(hwnd, kTimerHookReinstall, kHookReinstallDebounceMs, nullptr);
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == kMsgApplyLanguageSync) {
        // R6 Phase 1 (B3, plan §2.3): hook/worker-thread language mutations
        // marshal here so they run on the GUI thread through the same
        // ApplyLanguageChange coordinator as every other surface.
        // SEC-B5 (CWE-822 shatter fix): wParam/LPARAM are IGNORED entirely -
        // the message carries no payload anymore (always 0/0; any value an
        // external process posts is untrusted by design). Requests travel by
        // value through g_lang_sync_queue; this handler pops every pending
        // entry (per-item lock, so entries enqueued while an earlier one is
        // being applied are still consumed by this same wake-up) and applies
        // it on the GUI thread. With the coordinator retired (late message
        // after shutdown reset), entries are discarded — matching the old
        // free-without-apply semantics.
        for (;;) {
            LanguageSyncRequest req;
            {
                std::lock_guard<std::mutex> lk(g_lang_sync_mu);
                if (g_lang_sync_queue.empty()) {
                    break;
                }
                req = std::move(g_lang_sync_queue.front());
                g_lang_sync_queue.pop_front();
            }
            if (g_apply_language_change) {
                g_apply_language_change(
                    req.context, std::string_view{ req.source },
                    std::string_view{ req.target }, req.cycle, req.play_chime);
            }
        }
        return 0;
    }

    if (msg == emebalachat::kMsgUserLanguageProbeDone) {
        // 261116_0003 P9b-3 U3: a probe worker finished; drain the by-value
        // completion queue on the GUI thread (pure fold + the locked record
        // verb). SEC-B5 shape: wParam/LPARAM are IGNORED entirely (the payloads
        // travel through the service's own mutex-guarded deque); with the drain
        // retired (late message after shutdown), the handler is a no-op.
        if (emebalachat::g_drain_user_language_probes) {
            emebalachat::g_drain_user_language_probes();
        }
        return 0;
    }

    if (msg == kMsgEngineUnavailableModal) {
        // REQ-045 P4-8 (item 1, design §B + Tech Gate Item 6): the detached
        // bootstrap repair thread (and any other non-GUI producer) marshals the
        // "local engine unavailable" modal here so MessageBoxW runs on the GUI
        // thread. SEC-ADJ: wParam/LPARAM are IGNORED entirely (the wake-up
        // carries no payload — always 0/0; any value an external process posts
        // is untrusted by design). Requests travel by value through
        // g_engine_modal_queue; this handler pops every pending entry (per-item
        // lock, so entries enqueued while an earlier one is being applied are
        // still consumed by this same wake-up).
        //
        // REQ-047 D1 (design §A.4, Tech Gate §D1 #2) + P5-F1 (technical review
        // 175600 finding #1): the streak latch judgment lives HERE — the only
        // GUI-thread drain site — so producer threads (worker, drag icon,
        // tooltip re-translation, double Ctrl+C) never touch it. Producers
        // enqueue unconditionally; the drain applies the policy:
        //   * success sentinel (empty title+body)  -> clear the latch
        //     (a later recurrence re-arms the modal)
        //   * strict-failure request + latch clear -> show + latch
        //   * strict-failure request + latch set   -> drop (suppressed re-arm)
        // every-trigger WITH streak-latch, per design §A.4.
        for (;;) {
            EngineUnavailableModalRequest req;
            {
                std::lock_guard<std::mutex> lk(g_engine_modal_mu);
                if (g_engine_modal_queue.empty()) {
                    break;
                }
                req = std::move(g_engine_modal_queue.front());
                g_engine_modal_queue.pop_front();
            }
            if (req.title.empty() && req.body.empty()) {
                // Success sentinel: the translation path recovered (a non-empty
                // result arrived after a streak of strict local failures).
                // Clear the latch so a LATER recurrence re-arms the modal.
                emebalachat::g_engine_modal_latched = false;
                DIAG_F("MAIN/EngineModal/010: translation recovered; streak latch cleared\n");
                continue;
            }
            if (emebalachat::g_engine_modal_latched) {
                if (!req.offer_model_download) {
                    // REQ-047 P5-F1: the failure this modal describes is still
                    // unresolved — suppress the re-arm (design §A.4 every-trigger
                    // + streak-latch). Judged HERE, on the GUI thread; producers
                    // enqueue unconditionally.
                    DIAG_F("MAIN/EngineModal/030: strict failure while latched; modal suppressed\n");
                    continue;
                }
                // 260930_0004 D9 (fix): a model-download OFFER bypasses the
                // streak latch. The latch predates the offer (REQ-047 D1) and
                // cannot clear while the pinned model is absent — the success
                // sentinel needs a successful translation, which is impossible
                // in exactly the state the offer targets — so without this
                // exemption the user's explicit Built-in-LocalLLM pick (and the
                // post-grace boot offer) would be dropped silently forever.
                // The latch is STILL set below: it keeps streak-suppressing
                // interleaved STRICT failures only — it does NOT coalesce
                // offers (live-verified: two queued offers each delivered a
                // consent). Every explicit pick re-shows the consent dialog.
            }
            emebalachat::g_engine_modal_latched = true; // modal about to show
            ShowLocalEngineUnavailableModal(req.title, req.body,
                                            req.offer_model_download);
        }
        return 0;
    }

    if (msg == kMsgOpenOpenAiSettings) {
        // REQ-047 D3 (design §C.2 안 C-1, Tech Gate §D3 #4): open the OpenAI
        // settings dialog AFTER the TrackPopupMenuEx modality has fully torn
        // down. wParam/LPARAM are IGNORED entirely (pure (0,0) wake-up, same
        // SEC-ADJ contract as kMsgEngineUnavailableModal — any value an
        // external process posts is untrusted by design); the pending config
        // snapshot travels in the GUI-thread store s_pending_openai_cfg.
        // return 0 is MANDATORY (Tech Gate 반영사항 #4) — falling through to
        // DefWindowProc would break the modal-open contract.
        emebalachat::s_openai_dialog_pending = false;   // release the latch FIRST so a
                                                        // re-post during the dialog is legal
        OpenAiConfig edited = emebalachat::s_pending_openai_cfg.value_or(OpenAiConfig{});
        emebalachat::s_pending_openai_cfg.reset();
        // 261122_0001 B-U5 (design-pick item 4): consume the profile library
        // snapshot the tray dispatch seeded next to the openai block. The move
        // gives the fold its own deep copy: the pending slot is single-use and
        // the tray site already copied the vector out of the AppConfig, so no
        // alias to the live library survives. The dialog then takes its own
        // working copy from this fold input.
        emebalachat::OpenAiProfilesFold profiles_fold;
        if (emebalachat::s_pending_openai_profiles.has_value()) {
            profiles_fold.profiles = std::move(*emebalachat::s_pending_openai_profiles);
        }
        emebalachat::s_pending_openai_profiles.reset();
        // REQ-051 (Symptom D): the tri-state dialog — kCancelled/kSaved keep
        // the pre-REQ-051 wrapper's byte-identical behavior (the bool wrapper
        // still maps kDeleted -> false for any remaining caller); kDeleted is
        // dispatched to its own reconcile seam below.
        // 261116_0003 P9c-1c: the declaration-aware entry also hands the
        // checklist fold out (declaration); it is adopted on the owning config
        // by g_apply_openai_settings, never persisted here — this handler
        // cannot name the app's AppConfig.
        // 261122_0001 B-U5: the library-aware entry also hands the profile
        // fold out (profiles_fold); the identical single-writer contract — the
        // staged library is adopted on the owning config by
        // g_apply_openai_settings BEFORE its save, never persisted here.
        emebalachat::OpenAiDeclaredLanguagesFold declaration;
        // V10F-R1 F-17: the http-consent origin channel (in/out). IN: the
        // persisted scope seeds the dialog's consent record at
        // WM_INITDIALOG (the handler cannot name wWinMain's AppConfig, so the
        // seed value travels in this buffer; empty = the dialog falls back to
        // reading the persisted file itself). OUT: on kSaved it holds the
        // scope the dialog recorded, which g_apply_openai_settings persists
        // through the locked verb; on kDeleted it is ignored (that seam
        // clears the origin itself).
        std::string consent_origin_scope;
        const emebalachat::OpenAiSettingsOutcome openai_outcome =
            emebalachat::ShowOpenAiSettingsDialogEx(
                /*parent=*/emebalachat::g_hControllerWnd, edited, declaration, profiles_fold,
                &consent_origin_scope);
        if (openai_outcome == emebalachat::OpenAiSettingsOutcome::kCancelled) {
            // Cancel: engine untouched, refresh the tray check mark back.
            // No declaration is adopted (declaration.edited is false): the
            // user's live declaration must survive a cancelled dialog.
            // 261122_0001 B-U5: no profile library is adopted either — the
            // seam receives nullptr for the fold (double-guard: the dialog
            // also reported it unedited), so the stored library survives a
            // cancelled dialog untouched. V10F-R1 F-17: the consent origin is
            // adopted on the saved path only, so a cancelled grant attempt
            // leaves the stored scope untouched too.
            DIAG_F("MAIN/on_select_engine/003: openai settings cancelled; engine unchanged\n");
            if (g_apply_openai_settings) {
                g_apply_openai_settings(edited, /*saved=*/false, nullptr, nullptr);
            }
            return 0;
        }
        if (openai_outcome == emebalachat::OpenAiSettingsOutcome::kSaved) {
            // V10F-R1 F-17: persist the consent origin FIRST (the locked verb,
            // no save of its own) so the g_apply_openai_settings save below is
            // the single persist that carries it.
            if (g_apply_openai_consent_origin) {
                g_apply_openai_consent_origin(consent_origin_scope);
            }
            if (g_apply_openai_settings) {
                // P9c-1c: pass the folded declaration (possibly EMPTY = the
                // user cleared the declaration, which must remove the key
                // from the file) so the owning config applies it before it
                // saves. `edited` guards the "nothing was folded" contract.
                // 261122_0001 B-U5: pass the profile fold the same way — the
                // owning config adopts the staged library (only when the
                // dialog actually staged one) before the SAME single save.
                g_apply_openai_settings(
                    edited, /*saved=*/true,
                    declaration.edited ? &declaration.declared_languages : nullptr,
                    &profiles_fold);
            }
            return 0;
        }
        // kDeleted: the dialog already cleared + persisted the five openai
        // fields through its own read-modify-write (engine_type untouched by
        // construction). Reconcile the SAME cleared block into the in-memory
        // config + runtime engine so the unconditional exit save cannot
        // resurrect the deleted settings; NO engine switch (the frozen
        // user scope: settings only, engine selection unchanged).
        // 261122_0001 B-U5 (design-pick item 4): the staged profile fold is
        // deliberately NOT applied on this branch — REQ-051 [설정 삭제]
        // preserves the stored profile library (the dialog reports the fold
        // unedited here, and this branch never names it). 261122_0001 fix-A2
        // (R1-01) carve-out, applied inside the seam: entries that MIRROR the
        // deleted block (same key pair, or the synthesizer's base_url/model
        // identity when the block is keyless) are retired with it, so the
        // credential cannot be re-adopted from the library.
        DIAG_F("MAIN/on_select_engine/004: openai settings deleted; engine selection unchanged\n");
        if (g_apply_openai_deleted) {
            g_apply_openai_deleted(edited);
        }
        return 0;
    }

    if (msg == kMsgOpenGgufManager) {
        // REQ-048 R2-D: open the user-.gguf model manager AFTER the
        // TrackPopupMenuEx modality has fully torn down. wParam/LPARAM are
        // IGNORED entirely (pure (0,0) wake-up, same SEC-ADJ contract as
        // kMsgOpenOpenAiSettings). No coalescing latch is needed for the tray
        // producer: the only producer is the tray dispatch on this GUI
        // thread, and a modal dialog blocks that thread's message handling,
        // so a second open cannot be posted while one is up. The open + tray
        // refresh travels through g_open_gguf_model_manager (the wWinMain
        // lambda), so this handler never names config/engine/refresh_tray.
        // V10F E2 (261122_0002 P3-C4 wave B): a SECOND producer now exists:
        // the worker-thread LocalModelMissing offer helper
        // (RequestModelMissingOfferOrModal) posts through the same wake-up,
        // coalesced by its own producer-side atomic. That latch is released
        // HERE, AFTER the modal lambda returns: while the dialog runs the
        // worker can only coalesce. 261123_0001 (Bug A): the SESSION-scoped
        // load-failure latch (g_gguf_manager_load_failure_shown) is NEVER
        // released here — a later file-missing failure re-opens the manager,
        // while a later load-failure failure degrades to the tooltip hint.
        // The during-modal coalescing contract is unchanged.
        // return 0 is MANDATORY by the same contract as the OpenAI handler —
        // falling through to DefWindowProc would break the modal-open flow.
        if (g_open_gguf_model_manager) {
            g_open_gguf_model_manager();
        }
        g_gguf_manager_offer_posted.store(false, std::memory_order_release);
        return 0;
    }

    if (msg == kMsgOpenEngineLoadSettings) {
        // REQ-060: open the translation-model load-mode dialog AFTER the
        // TrackPopupMenuEx modality has fully torn down. wParam/LPARAM are
        // IGNORED entirely (pure (0,0) wake-up, same SEC-ADJ contract as
        // kMsgOpenGgufManager). No coalescing latch is needed (same reasoning
        // as the manager handler): the only producer is the tray dispatch on
        // this GUI thread. The open + persist + engine re-push travels through
        // g_open_engine_host_settings (the wWinMain lambda), so this handler
        // never names config/engine/refresh_tray.
        // return 0 is MANDATORY by the same contract as the manager handler —
        // falling through to DefWindowProc would break the modal-open flow.
        if (g_open_engine_host_settings) {
            g_open_engine_host_settings();
        }
        return 0;
    }

    if (msg == kMsgOpenUserLangAdd) {
        // 261116_0003 P9b-3 U4a: open the add-language dialog (U2) AFTER the
        // TrackPopupMenuEx modality has fully torn down. wParam/LPARAM are
        // IGNORED entirely (pure (0,0) wake-up, same SEC-ADJ contract as
        // kMsgOpenGgufManager — any value an external process posts is
        // untrusted by design). No coalescing latch is needed (the identical
        // reasoning as the manager handler): the only producer is the tray
        // dispatch on this GUI thread, and the modal dialog blocks that
        // thread's message handling, so a second open cannot be posted while
        // one is up. The dialog open + the persist-on-kAdded + the tray
        // refresh travel through g_open_user_lang_add (the wWinMain lambda),
        // so this handler never names config/probe_service/refresh_tray.
        // return 0 is MANDATORY by the same contract as the manager handler —
        // falling through to DefWindowProc would break the modal-open flow.
        if (g_open_user_lang_add) {
            g_open_user_lang_add();
        }
        return 0;
    }

    if (msg == kMsgOpenUserLangManager) {
        // 261116_0003 P9b-3 U4a: open the user-language manager dialog (U4b)
        // AFTER the TrackPopupMenuEx modality has fully torn down — the
        // identical contract as the add handler above (pure (0,0) wake-up, no
        // latch, the wWinMain lambda owns the open + the persist-on-kChanged +
        // the tray refresh). return 0 is MANDATORY on every path.
        if (g_open_user_lang_manager) {
            g_open_user_lang_manager();
        }
        return 0;
    }

    if (msg == kMsgOpenLanguagePalette) {
        // 261116_0003 P6c: the CONSUMER for the tray's two picker surfaces
        // (PR6-32). The tray's four "검색…" rows post this deferred open after
        // the TrackPopupMenuEx dispatch (tray.cpp ID_TRAY_SEARCH_* arms); until
        // this arm landed, the wake-up had no handler, the latch stranded on
        // the first click of a process, and every later click coalesced — the
        // picker never opened on ANY surface. wParam/LPARAM are IGNORED
        // entirely (pure (0,0) wake-up, same SEC-ADJ contract as the three
        // dialog arms above — any value an external process posts is untrusted
        // by design); the pending request travels in the picker module's
        // per-surface GUI-thread latch. return 0 is MANDATORY on every path.
        //
        // PR5-04(a): the arm is fail-closed on the GUI-thread guard — it
        // compares the controller window's owning thread id against the
        // current thread and drops with one diagnostic line on a mismatch.
        // The guard is defence-in-depth: dispatch into this WndProc is
        // expected to run on the controller's own thread (the producers post,
        // never send), so the compare is expected to HOLD — the P6c-3 pin
        // kills the guard's REMOVAL, not an attainable foreign-thread
        // delivery. No re-post on this path either: bouncing the message back
        // to ourselves could loop on a poisoned queue.
        const DWORD gui_tid = ::GetWindowThreadProcessId(hwnd, nullptr);
        if (gui_tid != ::GetCurrentThreadId()) {
            DIAG_LOG("UI", "lang_palette_controller_wake result=foreign_thread tid=%lu",
                     static_cast<unsigned long>(::GetCurrentThreadId()));
            return 0;
        }
        // 261116_0003 P6c fix round (PR6-42, adjudicated fix (a)): a wake-up
        // delivered while a picker is ALREADY open arrives NESTED — the open
        // picker's own hand-rolled modal loop pumps every message on this
        // thread (GetMessageW(nullptr)), so a second "검색…" post that lands
        // during the loop is dispatched HERE. Taking the latch before showing
        // would consume a request the picker module's single-instance guard
        // then refuses to show (lang_palette_open result=already_open): the
        // PR6-42 silent drop. The nested delivery therefore LEAVES ITS
        // REQUEST LATCHED (nothing taken, nothing lost) and exits; the arm
        // whose show() owns the running loop drains the latch after its popup
        // closes and serves the deferred request then (the loop below).
        // The ACTUAL guarantee, stated without overstatement: a request
        // posted while a picker is open is served by the draining arm right
        // after that picker closes — not while the first picker is still up
        // (a modal picker is never nested), and not by waiting for a future
        // unrelated delivery (a same-surface post while one is latched
        // coalesces WITHOUT a new wake-up, so only the drain makes the
        // deferred request deterministic). The latched request survives a
        // cancel of the first picker: it was never shown, so it is still
        // owed. Residual (accepted): if the OPEN picker belongs to the other
        // consumer (the tooltip's pill arm), this arm's drain still runs but
        // the other arm owns the loop — the request stays latched until the
        // next controller delivery; reaching that state requires posting a
        // tray request while the tooltip's picker is up, which the
        // click-outside dismissal ordering makes unreachable in practice.
        if (LanguagePickerShowing()) {
            DIAG_LOG("UI", "lang_palette_controller_wake result=deferred_while_open");
            return 0;
        }
        // PR6-32: the latch is per-surface and the wake-up carries no payload,
        // so all three tray-owned slots are polled (TrayTyping first,
        // TrayTooltip second, TrayUiLanguage third; a take on an empty slot
        // consumes nothing). The loop IS the
        // PR6-42 drain: pass 1 serves this delivery's own request, and each
        // later pass re-takes whatever a nested delivery left latched while
        // the previous pass's picker was open.
        bool first_pass = true;
        for (;;) {
            std::optional<language_palette::PickerInit> init =
                TakeDeferredLanguagePaletteOpen(language_palette::PickerSurface::TrayTyping);
            if (!init.has_value()) {
                init = TakeDeferredLanguagePaletteOpen(
                    language_palette::PickerSurface::TrayTooltip);
            }
            // 261119_0001 P6-U5: the third tray-owned slot — the UI-language
            // picker's latched request (tray.cpp ID_TRAY_UILANG_SEARCH arm).
            if (!init.has_value()) {
                init = TakeDeferredLanguagePaletteOpen(
                    language_palette::PickerSurface::TrayUiLanguage);
            }
            if (!init.has_value()) {
                if (first_pass) {
                    // Empty latch on a real delivery: an orphan wake-up (a
                    // previous consumer already took the request, or a
                    // foreign process posted the id). Exactly one diagnostic
                    // line; no picker, no dispatch. A LATER pass exits
                    // silently — an empty latch after a served request is the
                    // normal drain end, not an orphan.
                    DIAG_LOG("UI", "lang_palette_controller_wake result=orphan_wakeup");
                }
                break;
            }
            first_pass = false;
            // PR6-32 defence-in-depth: the per-surface slots already make a
            // foreign request impossible by construction (a TooltipPill post
            // lands in the pill slot, which this handler never takes — the
            // tooltip's own arm consumes it). The ownership check below stays
            // anyway as the belt-and-braces twin of the tooltip arm's PR6-29
            // check: if either consumer's take is ever regressed to a
            // cross-surface take, the wrong-pair dispatch is still refused
            // here. One diagnostic line, drop.
            // 261119_0001 P6-U5: the ownership guard widens to the third
            // tray-owned surface — a latched TrayUiLanguage request is owned by
            // this arm exactly like the two translation surfaces.
            if (init->surface != language_palette::PickerSurface::TrayTyping &&
                init->surface != language_palette::PickerSurface::TrayTooltip &&
                init->surface != language_palette::PickerSurface::TrayUiLanguage) {
                DIAG_LOG("UI", "lang_palette_controller_wake result=surface_mismatch surface=%d",
                         static_cast<int>(init->surface));
                return 0;
            }
            // PR5-03: the focus choreography the tooltip's picker arm runs —
            // back up the foreground owner BEFORE the modal picker, restore
            // it after, guarded by the same liveness check (the hidden
            // controller window is never a foreground candidate, so nothing
            // is set foreground here). Re-captured per drain pass: each
            // served request opens its own modal picker.
            HWND hwndPrev = ::GetForegroundWindow();
            // No anchor is latched on the tray path: the tray's TrackPopupMenuEx
            // lives at the cursor and leaves no persistent rect behind, so the
            // picker anchors at the current cursor position — the nullptr-anchor
            // contract, the same screen geometry the just-dismissed menu used.
            const LanguagePickerResult picked =
                ShowLanguageSearchPalette(emebalachat::g_hControllerWnd, *init, nullptr);
            if (hwndPrev && hwndPrev != hwnd && ::IsWindow(hwndPrev)) {
                ::SetForegroundWindow(hwndPrev);
            }
            // 261119_0001 P6-U5: the UI-language surface (PickerSurface::
            // TrayUiLanguage) commits a REGISTRY code, not a name_en. The code
            // is dispatched VERBATIM (261119_0001 FIX-R1 R1-07): the planner
            // owns validation AND canonicalization, matching authored codes
            // case-insensitively ("KO" -> "ko", "ZH-CN" -> "zh-CN") and
            // "AUTO" through its own auto branch, while an unknown code is
            // refused fail-closed. Coercing here through StringToLocale would
            // re-introduce its English fallback and hand the planner "en" for
            // any unknown code, bypassing that refusal.
            // g_apply_ui_language is published from wWinMain's
            // on_select_ui_language body (one planner, one persist, one
            // refresh). A cancel / click-outside dismissal (committed == false)
            // dispatches NOTHING here.
            if (picked.committed &&
                picked.surface == language_palette::PickerSurface::TrayUiLanguage) {
                if (g_apply_ui_language) {
                    g_apply_ui_language(picked.code);
                }
            }
            // The translation dispatch below owns every OTHER committed
            // surface; the UI-language surface is excluded above so a single
            // commit can never double-dispatch through both paths.
            if (picked.committed &&
                picked.surface != language_palette::PickerSurface::TrayUiLanguage) {
                // (surface, role) -> the existing tray callback routing, verbatim:
                //   TrayTyping  -> LanguageContext::Type  (the typing pair)
                //   TrayTooltip -> LanguageContext::Drag   (the drag pair)
                //   Source role -> ApplyLanguageChange(ctx, name_en, {})
                //   Target role -> ApplyLanguageChange(ctx, {}, name_en)
                // The surface/role are the picker's COMMIT-TIME echoes (a Tab flip
                // before committing moves the pick to the flipped role), and the
                // payload is the registry row's name_en — byte-identical to the
                // same pick made in the flat menu (PickerState::PayloadOf's
                // contract), so no second dispatch path exists. A cancel /
                // click-outside dismissal (committed == false) dispatches NOTHING.
                const bool typing =
                    (picked.surface == language_palette::PickerSurface::TrayTyping);
                const LanguageContext ctx =
                    typing ? LanguageContext::Type : LanguageContext::Drag;
                if (picked.role == language_palette::PickerRole::Source) {
                    if (g_apply_language_change) {
                        g_apply_language_change(ctx, picked.name_en, std::string_view{},
                                                false, false);
                    }
                } else {
                    if (g_apply_language_change) {
                        g_apply_language_change(ctx, std::string_view{}, picked.name_en,
                                                false, false);
                    }
                }
            }
        }
        return 0;
    }

    if (msg == WM_TIMER) {
        if (wParam == kTimerTrayToggle) {
            // 260922_0001 A1: the debounce window elapsed with no double-click
            // following -> this was a genuine single click, toggle the engine.
            const TrayToggleDecision decision = TrayToggleDebounceStateMachine(
                TrayToggleEvent::kTimerExpired, g_tray_toggle_pending);
            g_tray_toggle_pending = decision.timer_pending;
            ::KillTimer(hwnd, kTimerTrayToggle);
            if (decision.action == TrayToggleAction::kToggleNow && g_pHook) {
                g_pHook->ToggleActive();
            }
            return 0;
        }
        if (wParam == kTimerHookReinstall) {
            ::KillTimer(hwnd, kTimerHookReinstall);
            ReinstallHooksAfterLifecycleEvent("resume/unlock", hwnd);
            return 0;
        }
        if (wParam == kTimerHookHealth) {
            // Watchdog: thread-death detection (a dead hook thread leaves
            // IsHealthy false; a live-but-silently-removed hook cannot be
            // detected from user mode, hence the event-driven full reinstall).
            bool unhealthy = false;
            if (g_pHook && g_pHook->IsRunning() && !g_pHook->IsHealthy()) {
                unhealthy = true;
            }
            if (g_pMouseHook && g_pMouseHook->IsRunning() && !g_pMouseHook->IsHealthy()) {
                unhealthy = true;
            }
            // R1-15b (env research F2): the state the check above can never
            // see. Windows removes a WH_KEYBOARD_LL/WH_MOUSE_LL hook that blew
            // LowLevelHooksTimeout while leaving our HHOOK valid and the pump
            // thread alive - "live-but-dead", no tray/badge signal, and the
            // error only surfaced when the user pressed Enter and nothing
            // happened. There is no user-mode "is my hook still in the chain"
            // query, so the only truthful detector is a blind re-registration:
            // every kHookPeriodicReinstallMs of ticks rebuild the hook through
            // the SAME helper and debounce timer as the resume/unlock path
            // (single hook-lifecycle owner on this thread, never a double
            // install; the helper defers while a double-Ctrl+C body runs).
            const bool periodic_due = PeriodicHookReinstallTick(g_hook_health_ticks);
            if (unhealthy) {
                DIAG_F("MAIN/HookLifecycle/003: health watchdog detected dead hook thread; reinstalling\n");
                ReinstallHooksAfterLifecycleEvent("health check", hwnd);
            } else if (periodic_due &&
                       ((g_pHook && g_pHook->IsRunning()) ||
                        (g_pMouseHook && g_pMouseHook->IsRunning()))) {
                // Shape-only cadence line: no user content, no hwnd, no state.
                DIAG_F("MAIN/HookLifecycle/004: periodic hook self-heal reinstall "
                       "(every %lu min)\n",
                       static_cast<unsigned long>(kHookPeriodicReinstallMs / 60000u));
                ReinstallHooksAfterLifecycleEvent("periodic self-heal", hwnd);
            }
            return 0;
        }
    }

    if (msg == SystemTray::WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            if (g_pTray) {
                g_pTray->ShowContextMenu();
            }
            return 0;
        } else if (lParam == WM_LBUTTONUP) {
            // V10F BUG-02: route the single click through the debounce state
            // machine (the 260922_0001 A1 contract) instead of toggling
            // immediately — the immediate toggle made EVERY double-click also
            // flip the engine before the badge-visibility arm could cancel it.
            // The arm is kTimerTrayToggle for GetDoubleClickTime(); the
            // WM_TIMER handler above performs ToggleActive() only when the
            // dblclk arm has not cancelled the pending arm first.
            const TrayToggleDecision decision = TrayToggleDebounceStateMachine(
                TrayToggleEvent::kLeftButtonUp, g_tray_toggle_pending);
            g_tray_toggle_pending = decision.timer_pending;
            if (decision.action == TrayToggleAction::kArmTimer) {
                ::SetTimer(hwnd, kTimerTrayToggle, ::GetDoubleClickTime(), nullptr);
            }
            return 0;
        } else if (lParam == WM_LBUTTONDBLCLK) {
            // 260922_0001 A1: cancel the pending engine toggle, then keep the
            // original badge-visibility behavior.
            const TrayToggleDecision decision = TrayToggleDebounceStateMachine(
                TrayToggleEvent::kLeftButtonDblClk, g_tray_toggle_pending);
            g_tray_toggle_pending = decision.timer_pending;
            ::KillTimer(hwnd, kTimerTrayToggle);
            if (g_pBadge) {
                g_pBadge->SetVisible(!g_pBadge->IsVisible());
            }
            return 0;
        }
    }

    if (msg == WM_DESTROY) {
        ::KillTimer(hwnd, kTimerHookReinstall);
        ::KillTimer(hwnd, kTimerHookHealth);
        ::KillTimer(hwnd, kTimerTrayToggle); // 260922_0001 A1: no dangling timer at teardown
        ::PostQuitMessage(0);
        return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace (anon)

// REQ-051 note: the drag-icon copy retry backoff constant
// (kDragCopyRetryBackoffMs, REQ-005 session 260910_0003 Task B) MOVED to
// src/win32_input.hpp so the strengthened 3-attempt retry cycle
// (CopyChordWithSettledRetry) and its schedule live next to the other
// clipboard constants - ONE shared definition (see the header contract).

// REQ-045 P4-5 note: `RegisterUserGgufModel` above lives in this anon
// namespace (inside namespace emebalachat), so wWinMain below sees it.

} // namespace emebalachat

// REQ-005 (M6 T6): the repair HashProvider wired to Windows CNG. The Chat exe
// is llama-free (no engine_core link, design §4.2), so it cannot reuse the
// engine-side ComputeFileSha256 — this local CNG implementation is the app-side
// hash the bootstrapper's injection seam accepts (design §1.3 note). Global
// namespace: it is called from main(), outside namespace emebalachat.
bool CngSha256Hex(const std::filesystem::path& file, std::string& out_hex) {
    out_hex.clear();
    const HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE || !h) {
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    std::vector<BYTE> obj;
    constexpr DWORD kBuf = 64 * 1024;
    std::vector<BYTE> buf(kBuf);
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                      BCRYPT_HASH_REUSABLE_FLAG) == 0) {
        DWORD obj_len = 0, data_len = 0;
        ::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len),
                            sizeof(obj_len), &data_len, 0);
        obj.resize(obj_len);
        if (::BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0,
                               BCRYPT_HASH_REUSABLE_FLAG) == 0) {
            bool read_ok = true;
            DWORD read = 0;
            while (::ReadFile(h, buf.data(), kBuf, &read, nullptr) && read > 0) {
                if (::BCryptHashData(hash, buf.data(), read, 0) != 0) {
                    read_ok = false;
                    break;
                }
            }
            // A failed ReadFile whose last error is neither success nor a clean
            // EOF means the stream was truncated/unreadable; refuse the hash.
            if (::GetLastError() != ERROR_SUCCESS && ::GetLastError() != ERROR_HANDLE_EOF) {
                read_ok = false;
            }
            if (read_ok) {
                BYTE digest[32] = {};
                if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                    static const char kHex[] = "0123456789abcdef";
                    out_hex.reserve(64);
                    for (BYTE b : digest) {
                        out_hex.push_back(kHex[b >> 4]);
                        out_hex.push_back(kHex[b & 0x0F]);
                    }
                    ok = true;
                }
            }
        }
    }
    if (hash) ::BCryptDestroyHash(hash);
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    ::CloseHandle(h);
    return ok && out_hex.size() == 64;
}

// 261122_0001 A3 (finding F12, blind-qa verified 2/2): the single-instance
// early-exit used to warn the twice-launching user with a foreground
// MessageBoxW that held the foreground for ~1.5-3 s (input-intercepting
// modal). The notice now renders on the app's EXISTING non-activating
// surface - TooltipWindow in REQ-R08 message mode (WS_EX_NOACTIVATE popup
// shown with SWP_NOACTIVATE, 1.8 s autohide) - so the user is still
// informed while NO window of this process can become foreground. The
// strings stay on StringId::AppName / AppAlreadyRunning; like the old
// MessageBox this runs BEFORE I18n::Initialize on the second instance, so
// they render the English table (the R6 Phase 5 contract at the gate
// below). COM/D2D/DWrite never came up on this path before, so COM is
// initialized here and torn down on return. Degrade: when the notice
// window cannot be created (no COM/D2D on the box), the old MessageBox
// remains the fallback - the twice-launching user is ALWAYS informed.
void ShowSecondInstanceNotice(HINSTANCE hInstance) {
    const HRESULT hr_com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_ok = SUCCEEDED(hr_com) || (hr_com == RPC_E_CHANGED_MODE);

    bool shown = false;
    if (com_ok) {
        emebalachat::TooltipWindow notice;
        if (notice.Create(hInstance)) {
            POINT cur = {};
            if (!::GetCursorPos(&cur)) {
                cur = POINT{0, 0};
            }
            // Offset mirrors the Win+F9 toggle-notice placement (main.cpp
            // uses cursor.x + 12, cursor.y + 12); ShowMessage clamps the
            // card into the monitor work area itself.
            notice.ShowMessage(
                cur.x + 12, cur.y + 12,
                emebalachat::I18n::Get(emebalachat::StringId::AppName),
                emebalachat::I18n::Get(emebalachat::StringId::AppAlreadyRunning));
            // Pump until the REQ-R08 autohide dismisses the card
            // (kTimerMessageAutohide -> Dismiss -> MarkHidden), bounded so a
            // stuck timer can never hold this process past ~6 s.
            bool quit = false;
            // 261122_0001 fix-D (R1-13/f, CR4 F-05): GetTickCount64 — the
            // wrapping 32-bit GetTickCount could roll inside the ~6 s pump
            // (~1-in-2.4 M launches) and cut the notice down to a silent loss.
            const ULONGLONG deadline = ::GetTickCount64() + 6000;
            while (notice.IsVisible() && !quit && ::GetTickCount64() < deadline) {
                MSG msg = {};
                if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        quit = true;
                        break;
                    }
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                } else {
                    ::Sleep(10);
                }
            }
            shown = true; // notice.Create succeeded: the card rendered (or autohid)
        }
    }
    if (!shown) {
        // Documented degrade (A3): D2D/DWrite unavailable -> the original
        // informing modal, unchanged.
        ::MessageBoxW(
            nullptr,
            emebalachat::I18n::Get(emebalachat::StringId::AppAlreadyRunning).c_str(),
            emebalachat::I18n::Get(emebalachat::StringId::AppName).c_str(),
            MB_OK | MB_ICONINFORMATION
        );
    }
    if (com_ok) {
        ::CoUninitialize();
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;
    (void)nCmdShow;

    // -1. REQ-R15 (audit §5 latent item 3): declare Per-Monitor-V2 DPI
    // awareness BEFORE any window or DC is created. Until this call the
    // process ran DPI-UNAWARE (no manifest, no API call anywhere): on mixed
    // 100%/150%/200% multi-monitor setups Windows bitmap-stretched the badge,
    // drag icon and tooltip (blurry, wrong physical size) while the LL mouse
    // hooks delivered raw per-monitor physical coordinates - the exact
    // offset/scale mismatch the audit asked about. All UI surfaces below
    // re-scale their DIB buffers with ui::MonitorDpiAtPoint/WindowDpi per
    // monitor they land on.
    if (!emebalachat::ui::EnsurePerMonitorV2ProcessDpiAwareness()) {
        DIAG_F("MAIN/WinMain/000: per-monitor-v2 DPI awareness unavailable; UI may be bitmap-scaled\n");
    }

    // 0. L3 (DLL search-path hardening, behavior-preserving part): remove the
    // current working directory from the default DLL search order so a later
    // delay-load of cublas/cublasLt/cudart (CMakeLists /DELAYLOAD) can never be
    // hijacked by an attacker-writable CWD (e.g. explorer "start in" on a temp
    // folder). Exact semantics per MSDN SetDllDirectoryW: an EMPTY STRING (L"")
    // removes the CWD from the search order, while NULL RESTORES the default
    // order (i.e. puts the CWD back) - passing nullptr here was a silent no-op
    // (audit Blocker 1, CWE-427); L"" is the actual hardening. PATH and the
    // application directory remain searched, so CUDA resolution via installed
    // toolkit/driver directories is unaffected. A failure is logged, never
    // swallowed, so this control can never regress silently again.
    // SetDefaultDllDirectories(...) is deliberately NOT called here: it would
    // drop PATH, and the installer ships no CUDA DLLs, so GPU acceleration on
    // toolkit machines resolves through PATH - restricting it breaks the CUDA
    // load chain (deferred; documented in CMakeLists.txt).
    if (!::SetDllDirectoryW(L"")) {
        DIAG_F("MAIN/SetDllDirectory/001: SetDllDirectoryW(L\"\") failed "
               "(err=%lu); CWD remains in the DLL search order\n",
               ::GetLastError());
    }

    // REQ-203 (session 260911_0002): enforce the 200MB logs cap ONCE at
    // startup, regardless of the opt-in state (a logs folder full of files
    // from earlier enabled runs must shrink even after the user flips
    // diag_log_enabled back to false). Runs before diag::Init so the new
    // run's own file (not yet existing) is trivially excluded; every
    // directory it might write to is covered because Init resolves the same
    // LOCALAPPDATA-first path. Best-effort: locked/foreign files are skipped,
    // failures are swallowed per file (never a startup blocker).
    {
        const std::filesystem::path log_dir = diag::DefaultLogDir();
        if (!log_dir.empty()) {
            const uint64_t pruned = diag::PruneLogs(log_dir);
            if (pruned > 0) {
                // The logger is not live yet; report through the unconditional
                // stderr mirror only (DIAG_F keeps file writes off the table
                // for opted-out users).
                DIAG_F("MAIN/Diag/002: pruned %llu log file(s) over the 200MB cap\n",
                       static_cast<unsigned long long>(pruned));
            }
        }
    }

    // 260905 diagnostics: initialize the file logger as early as safely
    // possible (after the DLL hardening, before anything that can fail).
    // REQ-201 (260911_0002): Init no longer OPENS the log file — it resolves
    // the directory and starts the flush thread only. The file is created
    // lazily at diag::SetEnabled(true) below (first enabled enqueue), so an
    // opted-out user never gets a file. The single-instance early-exit below
    // therefore records its "second instance" line ONLY when a previous
    // config already enabled logging — which is exactly the correct behavior
    // per design 144800 §2.1. Init failure = graceful disable (never blocks
    // or crashes the app).
    const bool diag_ok = diag::Init();
    if (!diag_ok) {
        DIAG_F("MAIN/Diag/001: diag logger init failed (no LOCALAPPDATA and no "
               "exe-dir logs fallback?); file diagnostics disabled this run\n");
    }

    // P5-F1 (session 260909_0004, REQ-104 runtime gap): probe vulkan-1.dll
    // resolvability BEFORE the engine/warmup thread can trigger the ggml
    // backend-registry ctor (engine.cpp:601 -> llama.cpp:143 ->
    // ggml_backend_vk_reg -> first /DELAYLOAD:vulkan-1.dll touch). On a
    // driverless machine the delay helper would raise uncatchable SEH
    // 0xC06D007E and kill the process instead of CPU-falling back; the
    // guard's delay-load failure hook substitutes VkResult-error stubs so
    // ggml's existing vk::SystemError catch neutralizes the backend cleanly.
    // Runs right after SetDllDirectoryW(L"") above (CWD removed) and diag::Init
    // so the probe sees the exact same search order the delay-load will use,
    // and before any thread that could load a model exists.
    (void)emebalachat::EnsureVulkanGuard();

    // 1. Single Instance Mutex
    // NOTE (R6 Phase 5 sweep): the startup notice texts below are now
    // routed through i18n (StringId::AppAlreadyRunning / AppComFailed; the
    // captions use StringId::AppName per REQ-B-006). They run BEFORE
    // I18n::Initialize (config not loaded yet on the second instance), so
    // they render the English table - behaviorally identical to the old
    // hardcoded literals, but the strings live in exactly one place.
    // 261122_0001 A3 (F12): the AppAlreadyRunning notice is a NON-ACTIVATING
    // TooltipWindow card (ShowSecondInstanceNotice) instead of a foreground
    // MessageBoxW; only the rare D2D-less degrade and the AppComFailed warn
    // still use a MessageBox.
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Local\\Emebalachat_SingleInstance");
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 261122_0001 A3 (F12): the twice-launching user still gets the
        // localized notice, but on the WS_EX_NOACTIVATE tooltip surface -
        // this early-exit path creates no activating window.
        ShowSecondInstanceNotice(hInstance);
        if (hMutex) {
            ::CloseHandle(hMutex);
        }
        // REQ-201 (design 144800 §2.1): this forensic line must land in a
        // file ONLY when the user enabled logging. The lazy-open makes the
        // file's existence equal to "enabled", but the normal SetEnabled
        // application point (below, after the config load) is never reached
        // on this early exit — so peek the flag through the same loader.
        // A fresh-install read auto-creates the default config exactly like
        // the primary path would (flag false there: no file, no change to
        // opted-out behavior); existing configs are read-only here.
        {
            emebalachat::AppConfig early_cfg;
            early_cfg.LoadFromFile();
            diag::SetEnabled(early_cfg.diag_log_enabled);
        }
        DIAG_LOG("SESSION", "second instance detected; exiting");
        diag::Shutdown(); // bounded drain so the record survives the early exit
        return 0;
    }

    // 2. Initialize COM for Direct2D & DirectWrite (I1 fix: check the result).
    // RPC_E_CHANGED_MODE means another component already initialized a different
    // apartment model on this thread; COM is still usable, so it is tolerated.
    // Any other failure means the COM-dependent visual layer (Direct2D badge /
    // DirectWrite / WIC logo / SAPI TTS) cannot work: log a traceable code and
    // warn the user once, then continue running the non-COM core (tray icon,
    // keyboard/mouse hooks, translation pipeline, Beep sounds) rather than
    // crashing or failing silently with an invisible UI.
    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_available = SUCCEEDED(hrCom) || (hrCom == RPC_E_CHANGED_MODE);
    if (!com_available) {
        DIAG_F("MAIN/WinMain/001: CoInitializeEx failed 0x%08lX; D2D badge, logo bitmaps and TTS are disabled\n",
                static_cast<unsigned long>(hrCom));
        ::MessageBoxW(
            nullptr,
            emebalachat::I18n::Get(emebalachat::StringId::AppComFailed).c_str(),
            emebalachat::I18n::Get(emebalachat::StringId::AppName).c_str(),
            MB_OK | MB_ICONWARNING
        );
    }

    // 3. Load or create AppConfig
    emebalachat::AppConfig config;
    const bool config_ok = config.LoadFromFile();
    emebalachat::ui::SetActiveTheme(config.ui_theme);

    // REQ-201/202 (session 260911_0002): apply the MASTER log-file opt-in
    // switch from config exactly once, here after the load and before any
    // hook/worker thread exists. Default is OFF — a shipped build never
    // creates a log file unless the user sets diag_log_enabled=true and
    // restarts. The FIRST SetEnabled(true) transition lazily creates this
    // run's file and writes the SESSION header lines (diag_logger.cpp);
    // everything enqueued earlier this run (currently nothing, since the
    // sink starts disabled) appends after them.
    diag::SetEnabled(config.diag_log_enabled);
    // REQ-003 (session 260909): apply the opt-in USER-CONTENT logging gate from
    // config exactly once, here after the load and before any hook/worker
    // thread exists (the atomic lives in diag_logger; call sites branch on
    // diag::ContentLoggingEnabled()). Default is OFF — shipped logs stay
    // shape-only. There is no runtime UI toggle: editing diag_log_content in
    // config.json takes effect on the next start. content=true with
    // enabled=false still writes NOTHING — diag_log_enabled is the master
    // switch (design 144800 §2.1 truth table).
    diag::SetContentLogging(config.diag_log_content);
    DIAG_LOG("SESSION", "diag_log_enabled=%d diag_log_content=%d (file sink %s; user-content fields %s)",
             config.diag_log_enabled ? 1 : 0,
             config.diag_log_content ? 1 : 0,
             config.diag_log_enabled ? "ENABLED - a log file was just created for this run"
                                     : "disabled - no log file this run",
             config.diag_log_content ? "ENABLED - keystroke chars, window titles, captured and translated text WILL be recorded"
                                     : "disabled - shape-only logging");

    // 260905 diagnostics: config snapshot summary. The values are logged in
    // full ONLY behind the user-content gate (diag_log_content, R15-21); with
    // the gate OFF the model path and the language pairs log as lengths.
    {
        const emebalachat::AppConfig::Snapshot snap = config.GetSnapshot();
        // Phase 3 Batch 2: the legacy single pair is gone from the runtime;
        // log the two context pairs actually driving the pipelines (user
        // authorized config dumps for this diagnostic build, 260905).
        // fix-list-round15 R15-21 (F-3b): the model path and the two context
        // pairs can carry user-typed stored names (a user-language pick
        // persists its name as the planner token; a user .gguf path can embed
        // the profile name) — the R14-06 class — so they log only behind
        // diag::ContentLoggingEnabled(); the else keeps the shape-only
        // lengths. ui_language (fixed code), engine id, toggles, hotkeys and
        // sampling params are fixed-domain / non-content and ride both.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("SESSION", "config loaded=%d ui_language=%s engine=%s model_path=%s "
                                "drag=%s/%s type=%s/%s auto_send=%d sound=%d drag_to_translate=%d "
                                "cloud_fallback=%d hotkey_toggle=%s hotkey_lang=%s hotkey_mode=%s "
                                "temp=%.2f top_p=%.2f top_k=%d rep_pen=%.2f",
                     config_ok ? 1 : 0, config.ui_language.c_str(), snap.engine_type.c_str(),
                     config.model_path.c_str(),
                     snap.drag_source_language.c_str(), snap.drag_target_language.c_str(),
                     snap.type_source_language.c_str(), snap.type_target_language.c_str(),
                     snap.auto_send ? 1 : 0,
                     snap.sound_enabled ? 1 : 0, config.drag_to_translate ? 1 : 0,
                     config.cloud_fallback_enabled ? 1 : 0, config.hotkey_toggle.c_str(),
                     config.hotkey_lang.c_str(), config.hotkey_mode.c_str(),
                     config.temperature, config.top_p, config.top_k, config.repetition_penalty);
        } else {
            DIAG_LOG("SESSION", "config loaded=%d ui_language=%s engine=%s model_path_len=%zu "
                                "drag_len=%zu/%zu type_len=%zu/%zu auto_send=%d sound=%d "
                                "drag_to_translate=%d cloud_fallback=%d hotkey_toggle=%s "
                                "hotkey_lang=%s hotkey_mode=%s temp=%.2f top_p=%.2f top_k=%d "
                                "rep_pen=%.2f",
                     config_ok ? 1 : 0, config.ui_language.c_str(), snap.engine_type.c_str(),
                     config.model_path.size(),
                     snap.drag_source_language.size(), snap.drag_target_language.size(),
                     snap.type_source_language.size(), snap.type_target_language.size(),
                     snap.auto_send ? 1 : 0,
                     snap.sound_enabled ? 1 : 0, config.drag_to_translate ? 1 : 0,
                     config.cloud_fallback_enabled ? 1 : 0, config.hotkey_toggle.c_str(),
                     config.hotkey_lang.c_str(), config.hotkey_mode.c_str(),
                     config.temperature, config.top_p, config.top_k, config.repetition_penalty);
        }
    }

    // 4. Initialize Universal i18n
    emebalachat::I18n::Initialize(config.ui_language);
    // Phase 3 Batch 2 (plan §5 item 3, Batch 1 handoff): the old startup
    // backfill of config.target_language is deleted. Default-target resolution
    // now happens inside AppConfig::LoadFromFile (Batch 1 migration: absent
    // new-schema keys => drag=(Auto/OS-lang), type=(Auto/English), REQ-006/007/
    // 015/016), so a startup patch on the legacy field would only create a
    // drag/type-vs-legacy inconsistency.

    // REQ-208/SEC-1 (session 260911_0002 T3, design 144800 §2.3/§2.6 option
    // (i), light-gate 165600 Q4 + 180000 recommendations a/b/c): FIRST-RUN
    // PRIVACY NOTICE — blocking gate, exactly once per install.
    //   (a) localization: fires AFTER I18n::Initialize above, so the title and
    //       body render in the user's configured (or OS-detected auto) locale
    //       across all 37 supported tables (StringIds shipped by T2, commit
    //       ba9be65). Unlike the pre-Initialize single-instance boxes, this one
    //       is never English-only.
    //   (b) record-then-show: privacy_notice_shown=true is persisted BEFORE the
    //       modal call, so a crash/kill while the notice is open cannot loop
    //       the popup on every launch; the failure direction is a REPEATED
    //       notice (never a skipped disclosure) if the save itself fails.
    //   (c) serving gate: this point precedes the TranslationManager
    //       construction (section 5 below), the warmup thread, worker.Start()
    //       and hook.Start() (section 9) — while the modal MessageBoxW is up,
    //       no WH_KEYBOARD_LL proc exists, no pipeline worker thread exists
    //       and the engine serves nothing, so no text can reach Google before
    //       the disclosure is acknowledged (REQ-209 "disclosure BEFORE any
    //       translation", closing the SEC-1 installer auto-google gap). The
    //       GUI thread calls the modal directly; no hook-thread context ever
    //       blocks (delegation constraint).
    // MB_OK: the system dialog localizes its own OK button (no extra StringId,
    // light-gate 180000 Q2). MB_RTLREADING mirrors the Cheat Sheet call per the
    // §2.2.4 scope discipline (computed per call; owner stays nullptr — the
    // controller window and tray do not exist yet at this point).
    if (emebalachat::PrivacyNoticeOutstanding(config.privacy_notice_shown)) {
        config.privacy_notice_shown = true;
        const bool notice_persisted = config.SaveToFile();
        DIAG_LOG("SESSION", "REQ-208 first-run privacy notice firing (blocking gate before engine/worker/hook start); shown-flag persisted=%d",
                 notice_persisted ? 1 : 0);
        UINT notice_type = MB_OK | MB_ICONINFORMATION;
        if (emebalachat::DirectionForLocale(emebalachat::I18n::GetCurrentLocale()) ==
            emebalachat::TextDirection::RTL) {
            notice_type |= MB_RTLREADING;
        }
        ::MessageBoxW(
            nullptr,
            emebalachat::I18n::Get(emebalachat::StringId::PrivacyNoticeBody).c_str(),
            emebalachat::I18n::Get(emebalachat::StringId::PrivacyNoticeTitle).c_str(),
            notice_type
        );
    }

    // 5. Initialize Sound State
    emebalachat::SetSoundEnabled(config.sound_enabled);

    // 5. Initialize Translation Manager
    emebalachat::EngineType engine_type = emebalachat::EngineType::Auto;
    if (config.engine_type == "google") {
        engine_type = emebalachat::EngineType::GoogleTranslate;
    } else if (config.engine_type == "local") {
        engine_type = emebalachat::EngineType::LocalLlama;
    } else if (config.engine_type == "openai") {
        // REQ-045 P4-3 (design §3b): OpenAI Compatible explicit pick.
        engine_type = emebalachat::EngineType::OpenAi;
    } else if (config.engine_type == "user_gguf") {
        // REQ-046 P4-2 (Rev2 §B-4): the user-.gguf state rides the SAME
        // host-mediated LocalLlama routing — the host's C1-gated boot cache
        // supplies the user's model id to the worker. Enum stays frozen
        // (동결 계약: no EngineType extension); the state lives in the
        // config's engine_type string + user_model_id.
        engine_type = emebalachat::EngineType::LocalLlama;
    }
    // REQ-R11 (audit §4 M3): normalize a relative model_path against the
    // EXECUTABLE directory, not the CWD. Run-registry autostart launches with
    // CWD=C:\Windows\System32, where the CWD-relative "models/...gguf" never
    // resolves and model loading failed. Resolving HERE - at the single
    // config->engine handoff - makes the path absolute BEFORE it reaches both
    // validation time (IsValidModelPath, src/engine.cpp) and load time
    // (llama_model_load_from_file), so the two can never disagree. The
    // persisted config.json value stays untouched (relative = portable across
    // install locations). An explicitly emptied config.model_path is restored
    // to the AppConfig default before resolution so the engine's internal
    // CWD-relative fallback (engine.cpp ctor) is never reached in the app flow.
    std::string model_path_raw = config.model_path;
    if (model_path_raw.empty()) {
        model_path_raw = emebalachat::AppConfig{}.model_path;
    }
    const std::string resolved_model_path =
        emebalachat::ResolveModelPath(model_path_raw);
    emebalachat::TranslationManager engine(engine_type, resolved_model_path);
    // I3: this is the single source of truth - main.cpp loaded config.json once
    // and pushes every value into the engine explicitly (the engine constructor
    // no longer performs its own disk load).
    engine.SetSamplingParams(config.temperature, config.top_p, config.top_k, config.repetition_penalty);
    // H2 consent gate: propagate the privacy-first cloud_fallback_enabled flag so
    // the engine never silently transmits typed text to Google without consent.
    engine.SetCloudFallbackEnabled(config.cloud_fallback_enabled);
    // REQ-043: push the persisted engine_host block. No embedded engine:
    // host-first routing, and on ANY host failure the chain is
    // spawn -> one-click repair -> consent-gated cloud -> feature-unavailable
    // notice (plan §V2-8.6); the engine router owns that policy (plan §5.4).
    engine.SetEngineHostConfig(config.engine_host);
    // REQ-045 P4-3 (design §3b): push the persisted OpenAI Compatible block
    // into the router (same I3 pattern as SetEngineHostConfig above).
    engine.SetOpenAiConfig(config.openai);
    // REQ-051 U-2: the user-model (.gguf) route's display name. The config
    // string (not the resolved enum) is the discriminator — "user_gguf" and
    // "local" both map to LocalLlama, but only the former displays the model
    // stem. The stem resolution mirrors the tray label exactly.
    if (config.engine_type == "user_gguf") {
        engine.SetUserModelDisplayStem(emebalachat::ResolveUserModelStem(config.user_model_id));
    }

    // 6. Create Hidden Controller Window for Tray & Message Pump
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = emebalachat::ControllerWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = emebalachat::kControllerClassName;
    ::RegisterClassExW(&wc);

    HWND hController = ::CreateWindowExW(
        0,
        emebalachat::kControllerClassName,
        L"Emebala Chat Controller",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr
    );
    emebalachat::g_hControllerWnd = hController;

    // REQ-R14 (audit §5 latent item 2): lifecycle event sink for the hook
    // re-registration policy. WM_POWERBROADCAST (Sleep/Resume) is delivered
    // ONLY to top-level windows - the message-only controller above can never
    // receive it - so create a hidden zero-size tool window with the same
    // WndProc. WTS_SESSION_LOCK/UNLOCK (Win+L, Ctrl+Alt+Del, RDP reconnect)
    // are registered on it too (NOTIFY_FOR_THIS_SESSION: no SYSTEM_SERVICE_
    // RIGHTS needed for a normal interactive session). The UAC secure-desktop
    // trip itself delivers no message at all; ControllerWndProc's periodic
    // health-check watchdog (armed in section 9) is the belt-and-braces for
    // silent OS unhook around desktop switches.
    {
        WNDCLASSEXW wc_ps = {};
        wc_ps.cbSize = sizeof(WNDCLASSEXW);
        wc_ps.lpfnWndProc = emebalachat::ControllerWndProc;
        wc_ps.hInstance = hInstance;
        wc_ps.lpszClassName = emebalachat::kPowerSinkClassName;
        ::RegisterClassExW(&wc_ps);

        HWND hPower = ::CreateWindowExW(
            WS_EX_TOOLWINDOW,
            emebalachat::kPowerSinkClassName,
            L"Emebala Chat Power Sink",
            WS_POPUP,
            -1000, -1000, 0, 0,
            nullptr, nullptr, hInstance, nullptr
        );
        emebalachat::g_hPowerWnd = hPower;
        if (!hPower) {
            DIAG_F("MAIN/WinMain/005: power sink window creation failed (GLE %lu); Sleep/Resume hook reinstall degraded to watchdog-only\n",
                    ::GetLastError());
        } else if (!::WTSRegisterSessionNotification(hPower, NOTIFY_FOR_THIS_SESSION)) {
            DIAG_F("MAIN/WinMain/006: WTSRegisterSessionNotification failed (GLE %lu); Win+L hook reinstall degraded to watchdog-only\n",
                    ::GetLastError());
        }
    }

    // 7. Initialize Direct2D Floating Badge (restoring saved position if present)
    emebalachat::FloatingBadge badge;
    emebalachat::g_pBadge = &badge;
    // Create() fails cleanly (returns false) when COM/D2D are unavailable; the
    // badge's public methods all no-op with a null hwnd_, so the rest of the
    // app keeps running without the visual pill (I1 graceful degradation).
    if (!badge.Create(
            hInstance,
            // Phase 3 Batch 2 (plan §2.4): the badge displays the TYPE pair.
            // Startup direct field access is I4-legal (no worker/hook thread
            // exists yet at this point).
            emebalachat::ToUtf16(config.type_source_language),
            emebalachat::ToUtf16(config.type_target_language),
            config.badge_x,
            config.badge_y
        )) {
        DIAG_F("MAIN/WinMain/002: FloatingBadge::Create failed; running without the badge UI\n");
    }

    // Persist badge desktop coordinates whenever dragged by the user
    badge.SetPositionCallback([&](int x, int y) {
        config.SetBadgePosition(x, y);
        config.SaveToFile();
    });

    // REQ-043 (M6 T5, plan §V2-8.6): the STARTUP WARMUP THREAD is removed with
    // the embedded engine. There is no resident model to preload — the local
    // source is the shared inference host (Emebala.Engine.exe), whose worker
    // process spawns on demand at the first host request. Startup work is now
    // only a session-shape log line so the 260908-style "what did startup do"
    // question stays answerable from diagnostics.
    DIAG_LOG("SESSION", "local serving = shared engine host (embedded engine removed, REQ-043 M6 T5); host_available=%d",
             engine.IsLocalModelAvailable() ? 1 : 0);

    // 8. Initialize System Tray with Callbacks
    emebalachat::SystemTray tray;
    emebalachat::g_pTray = &tray;

    emebalachat::PipelineWorker worker(config, engine, badge);
    emebalachat::KeyboardHook hook(config, worker, badge, tray);
    emebalachat::g_pHook = &hook;

    // 8.5. Initialize Drag-to-Translate Components
    emebalachat::DragIconWindow drag_icon;
    drag_icon.Create(hInstance);

    emebalachat::TooltipWindow tooltip;
    tooltip.Create(hInstance);
    emebalachat::g_pTooltip = &tooltip; // REQ-005 (M6 T6): repair-thread access

    // REQ-005 (M6 T6, plan §V2-8.3 + §V2-8.6, design §9 R-3b): engine-host
    // repair bootstrapper. Runs AFTER the first-run privacy gate (above) and
    // BEFORE the engine serves anything, but does NOT block startup: the
    // presence check is a handful of filesystem existence probes, and any
    // repair is a detached background thread (no UI/worker blocking).
    //   * all components present  -> nothing to do.
    //   * missing + a repair URL is compiled in -> silent background repair;
    //     on success the local source becomes serveable on the next request
    //     (the host spawns on demand), on failure the transient tooltip
    //     surfaces the localized "repair unavailable" guidance (never silent).
    //   * missing + NO URL (the shipped default) -> the same guidance, but
    //     only after a bounded grace re-check (REQ-048 P1): right after a
    //     fresh install the installer's model download may still be in
    //     flight, so an immediate modal is a false alarm.
    // The repair thread is detached and best-effort; it touches no user text.
    {
        namespace bs = emebalachat::engine_host_bootstrap;
        const bs::ComponentCheckResult check = bs::CheckComponents();
        if (!check.missing.empty()) {
            DIAG_LOG("ENGINEHOST",
                     "bootstrap/000: components missing (count=%zu engine_dir=%d models_dir=%d); converging the §V2-8.6 UX chain",
                     check.missing.size(), check.engine_dir_resolved ? 1 : 0,
                     check.models_dir_resolved ? 1 : 0);
            const std::string url(bs::CompiledRepairUrl());
            if (!url.empty()) {
                // Silent background repair (§V2-8.3). The hash seam is wired to
                // the app's client-side SHA-256 (engine_core ComputeFileSha256,
                // re-exported via engine.hpp); the unit tests inject a mock.
                std::thread([missing = check.missing, url]() {
                    const bs::RepairOutcome outcome =
                        bs::RepairMissingComponents(missing, url, CngSha256Hex);
                    if (outcome == bs::RepairOutcome::Repaired) {
                        DIAG_LOG("ENGINEHOST", "bootstrap/016: repair finished; local source serveable");
                        return; // no notice — the host spawns on the next request
                    }
                    // REQ-045 P4-8 (item 1, design §B): repair failed (offline /
                    // bad pin / disabled) — surface the "local engine
                    // unavailable" guidance as a MODAL that stays up until the
                    // user clicks OK or X. This thread is detached (not the
                    // GUI thread), so the modal is marshaled through the SEC-ADJ
                    // value-queue + pure (0,0) wake-up seam (Tech Gate Item 6
                    // correction — no heap pointer crosses PostMessageW).
                    emebalachat::RequestEngineUnavailableModal(
                        emebalachat::g_hControllerWnd,
                        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody));
                }).detach();
            } else {
                // No repair URL compiled in (design §9 R-3b: the shipped
                // default) — "repair unavailable" is the honest answer, but
                // the user-device P1 (REQ-048 P1) showed it firing as a FALSE
                // ALARM: right after install the installer's model download
                // is still in flight, so the components land seconds later.
                // Instead of the immediate inline modal, wait out a bounded
                // grace window on a DETACHED thread (the main thread stays
                // non-blocking) and suppress the modal entirely if the
                // components arrive inside it; only a real, persistent
                // absence converges on the REQ-045 P4-8 modal via the same
                // SEC-ADJ seam the URL branch uses (this helper is callable
                // from a detached thread by design).
                //
                // Pick-only offer (260930_0004 D9 revision, CEO directive): the
                // model-download offer fires ONLY from the explicit tray pick
                // (Translation engine > Built-in LocalLLM) — NEVER at startup.
                // A models/-ONLY miss at boot is NOT user-facing: the engine
                // is healthy and the user simply has not chosen the local
                // engine, so boot stays SILENT (a models/-only store is the
                // expected state of a cloud-only user). A binaries-involved
                // miss keeps the mutex-gated INSTANT notice (see the branch
                // below) but with offer_model_download FALSE — boot may
                // report broken engine files, it never pushes the download.
                if (emebalachat::modeldownloader::MissingIsModelOnly(check.missing)) {
                    // Model-only miss at boot: silent by design (pick-only
                    // offer policy). Shape-only log; no user-visible surface.
                    DIAG_LOG("ENGINEHOST", "bootstrap/018: models/-only miss at boot; pick-only offer policy — no notice");
                } else {
                    // Binaries involved — mutex-gated INSTANT notice
                    // (260930_0004 D9, replacing the fixed 40 x 3 s grace):
                    // the M7 A-7 family setup gate (installer/setup.iss:855,
                    // FAMILY_SETUP_MUTEX) is held for the ENTIRE store-writing
                    // window (install + model download + uninstall), so
                    // waiting on it converges THE INSTANT setup exits — no
                    // fixed 120 s, no polling. OBSERVE ONLY: OpenMutexW — the
                    // app must NEVER CreateMutex this name (that would poison
                    // the installer's gate). Caveats (user-wait-audit.md
                    // "How to go INSTANT"): the gate is advisory —
                    // setup.iss:2505 admits CheckForMutexes -> CreateMutex is
                    // not atomic, so a same-ms double launch can slip; worst
                    // case equals the pre-gate status quo, no new harm. The
                    // mutex only exists after Inno's language-selection dialog
                    // (~a second into setup), a window no post-install app
                    // launch can land in. The optional "installation
                    // finishing" tooltip during the wait is a documented
                    // follow-up — intentionally not in this pass.
                    // Pick-only offer revision (260930_0004 D9): the notice
                    // posts with offer_model_download FALSE (the 3-arg shape,
                    // same as the URL branch above) — boot reports broken
                    // engine files, never the download offer. Never block
                    // startup: everything below runs on the existing
                    // detached-thread seam.
                    HANDLE setup_gate = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\EmebalaSetup");
                    const bool gate_held = (setup_gate != nullptr);
                    const bool gate_unknown =
                        !gate_held && ::GetLastError() != ERROR_FILE_NOT_FOUND;
                    std::thread([setup_gate, gate_held, gate_unknown]() {
                        if (gate_held) {
                            // Setup/uninstall is writing the store: block until
                            // the installer process exits (Inno never closes
                            // the handle; the OS signals it on exit), then
                            // exactly ONE fresh check decides.
                            ::WaitForSingleObject(setup_gate, INFINITE);
                            ::CloseHandle(setup_gate);
                            const bs::ComponentCheckResult fresh = bs::CheckComponents();
                            if (fresh.missing.empty()) {
                                DIAG_LOG("ENGINEHOST", "bootstrap/017: components arrived within grace window; suppressing repair-unavailable modal");
                                return; // setup finished the store; no notice warranted
                            }
                            emebalachat::RequestEngineUnavailableModal(
                                emebalachat::g_hControllerWnd,
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody));
                            return;
                        }
                        if (gate_unknown) {
                            // OpenMutex failed for a reason other than
                            // ERROR_FILE_NOT_FOUND: no gate knowledge — fail
                            // OPEN to informing the user (one immediate fresh
                            // check; suppress only if it says healed); never
                            // invent a wait.
                            const bs::ComponentCheckResult fresh = bs::CheckComponents();
                            if (fresh.missing.empty()) {
                                DIAG_LOG("ENGINEHOST", "bootstrap/017: components arrived within grace window; suppressing repair-unavailable modal");
                                return;
                            }
                            emebalachat::RequestEngineUnavailableModal(
                                emebalachat::g_hControllerWnd,
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody));
                            return;
                        }
                        // Gate absent (ERROR_FILE_NOT_FOUND): no setup or
                        // uninstall can heal the state — inform immediately.
                        emebalachat::RequestEngineUnavailableModal(
                            emebalachat::g_hControllerWnd,
                            emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                            emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody));
                    }).detach();
                }
            }
        }
    }

    // R5 (Debug-Surgical): surface the Enter-path empty-capture failure that
    // was previously silent. Same contract as the drag-path failure notice
    // (REQ-R1(b)): the callback runs on the PIPELINE WORKER thread, and
    // TooltipWindow::ShowMessageThreadSafe is the existing REQ-R10 thread-
    // safe seam that marshals the D2D render onto the GUI thread. Registered
    // once at startup BEFORE worker.Start() (read-only afterwards, same
    // discipline as every other SetXxxCallback).
    worker.SetEmptyCaptureCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        // R6 Phase 5 (plan §5.3): notice header literal -> localized
        // StringId::TooltipTitle (was the hardcoded L"Emebala Chat" header).
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection));
    });

    // REQ-051 (session 260921, symptom A-2): the dropped-chord failure notice.
    // The worker calls this (on the pipeline worker thread) exactly when the
    // Enter path's capture seam reports EnterCaptureResult::CopyChordDropped -
    // the synthetic Ctrl+C never committed on a selection-capable geometry
    // (structured contenteditable / Electron editor) after the full retry
    // budget. The streak policy (DroppedChordFailureStreakSurfaces, worker.hpp)
    // surfaces this ONCE per consecutive-streak - the main.cpp:748 engine-
    // modal latch algebra: entry into the failure state notifies, persistence
    // suppresses, recovery (the next successful capture) clears. Reuses the
    // EXISTING TooltipCopyFailed string (no new i18n): "couldn't copy the
    // selection, check the target app" is the honest wording for a dropped
    // chord, where the no-selection text would mislabel a selection the app
    // failed to grab. PLACEMENT CONSTRAINT: this registration must stay
    // BEFORE the engine-unavailable registration below (req047_tests.inc
    // P5-F1 isolates the file's LAST Set*Callback registration body by the
    // literal callback name - keep that the engine-unavailable one).
    worker.SetCopyChordFailedCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipCopyFailed));
    });

    // REQ-042 (계획-2, session 260917_0002): under-slice feedback notice. The
    // worker calls this (on the pipeline worker thread) exactly when the Enter
    // path takes the tail-unchanged short-circuit while the verbatim prefix
    // still holds translatable text (UntranslatedResidueNoticeWarranted) -
    // the REQ-041 under-slice surfaced as policy, not a silent failure. Same
    // contract as SetEmptyCaptureCallback above: registered once BEFORE
    // worker.Start(), ShowMessageThreadSafe is the REQ-R10 thread-safe seam.
    worker.SetUntranslatedResidueCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipUntranslatedAbove));
    });

    // 261121_0002 (T6, design-pick.md "main.cpp"): the user-cancel notice. The
    // worker calls this (on the pipeline worker thread) exactly once per
    // CANCELED task - the Canceled branch next to the failure-tone block (no
    // tone, no streak, no modal). Same contract as SetEmptyCaptureCallback
    // above: registered once BEFORE worker.Start() (read-only afterwards), and
    // TooltipWindow::ShowMessageThreadSafe is the REQ-R10 thread-safe seam that
    // marshals the notice onto the GUI thread. Header and body both carry the
    // cancelled string - the notice is a single statement, there is no title
    // variant.
    worker.SetTranslationCancelledCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                      emebalachat::I18n::Get(emebalachat::StringId::TooltipTranslationCancelled),
                                      emebalachat::I18n::Get(emebalachat::StringId::TooltipTranslationCancelled));
    });

    // 261121_0002 (T6, design-pick.md "main.cpp"): the one-shot multi-chunk
    // hint. The worker fires this (on the pipeline worker thread) at most ONCE
    // per process - the first time a translation reports a genuine multi-chunk
    // plan (ShowChunkProgress) - so the user learns the badge single-click
    // cancels a long run. Same wrapper contract and marshal seam as the
    // cancelled notice above.
    worker.SetFirstMultiChunkHintCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                      emebalachat::I18n::Get(emebalachat::StringId::TooltipCancelHint),
                                      emebalachat::I18n::Get(emebalachat::StringId::TooltipCancelHint));
    });

    // V10F (RISK-03, 261122_0002 P2-D/P2-F): the typing-mode error-detail
    // display. The worker calls this (on the pipeline worker thread) exactly
    // when the Enter path's Translate() reports EngineFailed on an OpenAI leg
    // AND a non-empty per-request cause exists (TranslationTaskContext::
    // out_error_detail, already key-masked + bounded by the client) - the
    // typing path had NO text surface at all before. Registered BEFORE the
    // engine-unavailable registration below (the req047_tests.inc P5-F1
    // placement constraint: the engine-unavailable registration stays the
    // LAST worker Set*Callback in the file). The wrapper marshals through the
    // REQ-R10 ShowMessageThreadSafe seam with the DEDICATED typing-error
    // autohide budget (V10F-R1 F-06: kTypingErrorAutohideMs = 3 s — the shared
    // 1.8 s kMessageAutohideMs every other message-mode notice uses is
    // untouched), with the SAME body shape the drag failure
    // notice uses (TooltipTranslateFailed + "\n" + detail). An empty detail
    // never reaches here (the worker keeps the tone-only surface then).
    worker.SetOnTypingError([&tooltip](const std::wstring& detail) {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        std::wstring body = emebalachat::I18n::Get(emebalachat::StringId::TooltipTranslateFailed);
        body += L"\n";
        body += detail;
        // V10F-R1 F-06: the typing-error notice gets the DEDICATED 3 s
        // autohide (kTypingErrorAutohideMs), not the shared 1.8 s
        // kMessageAutohideMs every other message-mode notice uses.
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                      emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                      body, emebalachat::TooltipWindow::kGenNone,
                                      emebalachat::TooltipWindow::kTypingErrorAutohideMs);
    });

    // V10F (261122_0002 P2-F): the one active-reading registry instance. The
    // four reading lambdas below hold a ReadingJobScope on it for their whole
    // body; the Esc / badge-click arms read it through AnyReadingJobRunning
    // and latch it through CancelAllReadingJobs. Declared before the
    // back-translate worker (the first consumer) is constructed.
    emebalachat::ReadingJobRegistry reading_jobs;

    // ---- 261108_0002 (WS3, v0.10.6): floating back-translation card --------
    // The user rule (verbatim): "config.back_translation_enabled 이 true 면
    // ExecuteTask 가 콜백을 발화하고, back_translate_worker 가 BBBBB 를 원문
    // 언어로 역번역하여 TooltipWindow 플로팅 카드로 표시." worker.cpp fires the
    // pipeline-thread callback only AFTER a successful paste (the moment the
    // user's own words have already been replaced by BBBBB), so this block is
    // the ONE owner of the second engine round trip:
    //   * the registered wrapper runs on the PIPELINE thread (worker.hpp
    //     contract: non-blocking). It resolves the reverse source language with
    //     the pure four-tier ladder config.cpp already provides and hands a job
    //     over - no clipboard, no ledger, no inference, no D2D;
    //   * the dedicated SingleSlotWorker below (the drag/F9 precedent in this
    //     file) runs engine.Translate on its own thread and marshals the result
    //     through the tooltip's existing REQ-R10 thread-safe seam.
    // The whole path is IN-MEMORY: nothing is pasted, the clipboard is never
    // read or written, and the paste ledger is untouched. The card is read-only
    // verification UI, so a failure only means "no card".

    // 261108_0002 (WS3): 2-char ASCII language label for the card header
    // ("Korean" -> "KO", "Chinese Simplified" -> "ZH"). The canonical short code
    // comes from the language registry, so the header never has to render a
    // non-Latin name; an unresolvable name degrades to its first two ASCII
    // letters. The tooltip owns the header layout, so the label stays tiny.
    auto short_lang_label = [](std::string_view name_en) -> std::wstring {
        const emebalachat::LanguageInfo* info = emebalachat::FindLanguageByName(name_en);
        const std::string short_code = info ? info->display_short : std::string();
        std::wstring label;
        label.reserve(2);
        for (const char c : short_code) {
            if (c == '-' || label.size() >= 2) {
                break; // "ZH-CN" -> "ZH"; the header budget is two glyphs
            }
            if (c >= 'a' && c <= 'z') {
                label.push_back(static_cast<wchar_t>(c - 'a' + 'A'));
            } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                label.push_back(static_cast<wchar_t>(c));
            }
        }
        if (label.empty()) {
            for (const char c : name_en) {
                if (label.size() >= 2) {
                    break;
                }
                if (c >= 'a' && c <= 'z') {
                    label.push_back(static_cast<wchar_t>(c - 'a' + 'A'));
                } else if (c >= 'A' && c <= 'Z') {
                    label.push_back(static_cast<wchar_t>(c));
                }
            }
        }
        return label;
    };

    // One verification job = one finished forward translation awaiting its round
    // trip. worker.hpp's BackTranslateRequest carries the languages as the
    // pipeline saw them (configured/detected source, forward target); the
    // resolved reverse target and BOTH header labels are computed here on the
    // producer side, so the worker thread only translates and shows.
    struct BackTranslateJob {
        std::wstring original_text;  // AAAAA - what the user actually typed
        std::wstring forward_text;   // BBBBB - translated segment (never the
                                     // verbatim prefix of a tail translation)
        std::string forward_target;  // BBBBB's language (Translate source)
        std::string back_target;     // AAAAA's language (Translate target)
        // V10F-R1 F-04: AAAAA's language — the forward translation's resolved
        // source (the pipeline's resolved_engine_src, carried by the request's
        // detected_source slot). The history entry needs the TRUE pair
        // (original language -> forward_target); without this field the Push
        // used to stuff forward_target into the src slot and back_target into
        // the tgt slot, exactly backwards.
        std::string forward_source;
        std::wstring source_label;   // header: AAAAA's short code ("KO")
        std::wstring target_label;   // header: BBBBB's short code ("EN")
        HWND target_hwnd = nullptr;  // the window that received BBBBB
        HWND badge_hwnd = nullptr;   // Tier-2 card anchor (null when no badge)
        // Verdict reference (261121_0001, defect 3-5b follow-up): the segment
        // the forward translation came from. == original_text for whole-line
        // captures; the tail/block slice for PrefixWithTail / F3 captures.
        std::wstring compare_text;
    };

    // Runs ON back_translate_worker. Pure in-memory verification: engine round
    // trip -> canonical comparison -> marshal. Any failure keeps the screen
    // untouched (no card) - the required silent-skip policy.
    auto run_back_translate = [&config, &engine, &tooltip, &reading_jobs](const BackTranslateJob& job) {
        // V10F (261122_0002 P2-F): publish this job in the unified registry for
        // its whole body (Esc / badge-click can cancel the round trip).
        emebalachat::ReadingJobScope back_scope(reading_jobs.back_running,
                                                reading_jobs.back_cancel);
        if (job.forward_text.empty() || job.forward_target.empty() ||
            job.back_target.empty()) {
            return;
        }
        // 261108_0002 (FIX-R1, v0.10.6): "off means off" is checked BEFORE the
        // engine round trip, not only before the display. A job can sit in the
        // SingleSlotWorker after the user already toggled the feature off; the
        // post-translate gate alone still transmitted the finished forward
        // translation to the engine (local inference or the cloud consent path)
        // for a feature the user just disabled. The flag is read LIVE from the
        // config snapshot (never captured at enqueue time), so this reflects the
        // toggle as of the instant the worker picks the job up; the gate after
        // the round trip stays as defense in depth for a toggle that lands while
        // the request is in flight.
        if (!config.GetSnapshot().back_translation_enabled) {
            DIAG_LOG("UI", "back_translate_skip_disabled fwd_len=%zu",
                     job.forward_text.size());
            return;
        }
        // Same engine manager the Enter pipeline and the drag/F9 paths use, so
        // consent gating, cancel latch and engine selection are IDENTICAL to the
        // forward translation (no privacy path is bypassed or added here).
        // V10F (261122_0002 P2-F): the per-request task context wires this
        // path into the unified cancel registry (ctx.cancel) and the live
        // chunk-preview surface (on_chunk -> TryBegin / on_chunk_text ->
        // Append, lease-guarded); out_error_detail carries THIS request's
        // bounded cause, replacing any process-global read.
        emebalachat::TranslationTaskContext bt_ctx;
        bt_ctx.cancel = &reading_jobs.back_cancel;
        std::string bt_error_detail;
        bt_ctx.out_error_detail = &bt_error_detail;
        emebalachat::ChunkPreviewLease bt_preview(reading_jobs, tooltip);
        bt_ctx.on_chunk = [&bt_preview](std::size_t, std::size_t total) {
            bt_preview.TryBegin(static_cast<int>(total));
        };
        bt_ctx.on_chunk_text = [&bt_preview](const std::wstring& cumulative) {
            bt_preview.Append(cumulative);
        };
        emebalachat::TranslationStatus bt_status = emebalachat::TranslationStatus::Ok;
        const std::wstring back_text =
            engine.Translate(job.forward_text, job.forward_target, job.back_target, &bt_status, bt_ctx);
        if (!bt_error_detail.empty()) {
            // Shape-only evidence that the per-request carrier fired (the
            // cause itself never logs - R14-06 class).
            DIAG_LOG("UI", "back_translate_error_detail_len=%zu", bt_error_detail.size());
        }
        if (bt_status != emebalachat::TranslationStatus::Ok || back_text.empty()) {
            // fix-list-round15 R15-21 (F-3b): back_tgt is a language token
            // (can be a user-typed stored name) — the R14-06 class — so it
            // logs only behind diag::ContentLoggingEnabled(); status/fwd_len
            // ride both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("UI", "back_translate_skip status=%d fwd_len=%zu back_tgt=%s",
                         static_cast<int>(bt_status), job.forward_text.size(),
                         job.back_target.c_str());
            } else {
                DIAG_LOG("UI", "back_translate_skip status=%d fwd_len=%zu back_tgt_len=%zu",
                         static_cast<int>(bt_status), job.forward_text.size(),
                         job.back_target.size());
            }
            return;
        }
        // Meaning check: the SAME canonical form the paste ledger uses
        // (ADR-001 - byte-exact after CRLF/NBSP/NFC normalization, never fuzzy),
        // so an editor re-serialization cannot flip the verdict. The reference
        // is compare_text (261121_0001, defect 3-5b follow-up): the segment the
        // forward translation actually came from - original_text is the whole
        // captured line since U-B, which a tail fragment can never byte-equal
        // (U-B observation O1).
        const bool meaning_matched =
            emebalachat::CanonicalFormForLedger(back_text) ==
            emebalachat::CanonicalFormForLedger(job.compare_text);
        // Defense in depth (261108_0002 FIX-R1): the pre-call gate above covers
        // a job picked up after the toggle; this one covers a toggle that lands
        // WHILE this round trip is in flight (the toggle already dismissed the
        // visible card), so a late result must not pop a fresh one - "off means
        // off". The engine has no per-request cancel, so the completed round
        // trip is simply discarded.
        if (!config.GetSnapshot().back_translation_enabled) {
            DIAG_LOG("UI", "back_translate_drop_disabled fwd_len=%zu",
                     job.forward_text.size());
            return;
        }
        // fix-list-round15 R15-21 (F-3b): back_tgt is a language token (can
        // be a user-typed stored name) — the R14-06 class — so it logs only
        // behind diag::ContentLoggingEnabled(); the shape-only lengths and
        // matched= ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("UI",
                     "back_translate_done fwd_len=%zu back_len=%zu orig_len=%zu matched=%d back_tgt=%s",
                     job.forward_text.size(), back_text.size(), job.original_text.size(),
                     meaning_matched ? 1 : 0, job.back_target.c_str());
        } else {
            DIAG_LOG("UI",
                     "back_translate_done fwd_len=%zu back_len=%zu orig_len=%zu matched=%d back_tgt_len=%zu",
                     job.forward_text.size(), back_text.size(), job.original_text.size(),
                     meaning_matched ? 1 : 0, job.back_target.size());
        }
        // V10F L2 (261122_0002 P2-G): freeze the live preview BEFORE the card
        // replaces it (the P2-C contract: End(true) -> Show* replaces), then
        // record the finished round trip in the translation-history ring. The
        // back-translate job is the ONLY carrier of the verified back_text, so
        // it Pushes a fresh full entry here (the Push/Recent API has no
        // update verb; original entries recorded by the forward paths keep
        // back empty until this full triple lands).
        bt_preview.End(true);
        tooltip.ShowBackTranslationThreadSafe(job.original_text, back_text, job.source_label,
                                              job.target_label, meaning_matched,
                                              job.target_hwnd, job.badge_hwnd);
        // V10F-R1 F-04: the TRUE pair — src_code is the FORWARD SOURCE language
        // (the language of original_text, the resolved source the pipeline
        // handed us in the request's detected_source slot), tgt_code is the
        // forward target. The pre-fix push stuffed forward_target into the src
        // slot and back_target into the tgt slot, so the editor's retranslate
        // aimed at the wrong language for typing-path entries. (The owning PID
        // is recorded centrally by the history Push, V10F-R1 F-09.)
        emebalachat::translation_history::Push({ job.original_text, job.forward_text, back_text,
                                                 emebalachat::ToUtf16(job.forward_source),
                                                 emebalachat::ToUtf16(job.forward_target),
                                                 job.target_hwnd });
    };

    // Dedicated worker: the second inference call costs the same seconds the
    // first one did, and the pipeline must stay free for the user's next Enter.
    // Single slot + latest-wins Submit (the template's producer contract): a
    // superseded verification is REPLACED, never queued behind a slow one, and
    // the pipeline thread never blocks.
    emebalachat::SingleSlotWorker<BackTranslateJob> back_translate_worker(run_back_translate);

    // Register BEFORE the engine-unavailable registration below. PLACEMENT
    // CONSTRAINT (req047_tests.inc P5-F1): that suite isolates the file's LAST
    // worker callback registration - it must stay the engine-unavailable one.
    // The badge HWND is sampled here on the GUI thread (Create ran long before
    // this line), so the pipeline thread never reads the badge's plain member.
    const HWND back_translate_badge_hwnd = badge.GetHwnd();
    worker.SetBackTranslateCallback(
        [&back_translate_worker, &short_lang_label,
         back_translate_badge_hwnd](const emebalachat::BackTranslateRequest& req) {
            if (req.original_text.empty() || req.forward_text.empty()) {
                return;
            }
            // Four-tier ladder (pinned source -> detection -> OS UI language ->
            // ADR-A1-7 neutral pivot). Pure: snapshot values in, name_en out.
            const std::string back_target = emebalachat::ResolveBackTranslationTarget(
                req.configured_source, req.detected_source, req.forward_target,
                emebalachat::I18n::GetSystemLanguageCode());
            BackTranslateJob job;
            job.original_text = req.original_text;
            // 261121_0001 (defect 3-5b follow-up): the verdict reference is the
            // segment the forward translation came from; an empty compare_text
            // means "use original_text" (whole-line captures).
            job.compare_text =
                req.compare_text.empty() ? req.original_text : req.compare_text;
            job.forward_text = req.forward_text;
            job.forward_target = req.forward_target;
            // V10F-R1 F-04: the forward source the pipeline RESOLVED (pinned or
            // detected) — the language of original_text. The request carries it
            // in the detected_source slot (worker.cpp passes resolved_engine_src
            // there; configured_source may be "auto"/empty).
            job.forward_source = req.detected_source;
            job.back_target = back_target;
            job.source_label = short_lang_label(back_target);
            job.target_label = short_lang_label(req.forward_target);
            job.target_hwnd = req.target_hwnd;
            job.badge_hwnd = back_translate_badge_hwnd;
            // fix-list-round15 R15-21 (F-3b): back_tgt/fwd_tgt are language
            // tokens (can be user-typed stored names) — the R14-06 class —
            // so they log only behind diag::ContentLoggingEnabled(); the
            // shape-only fwd_len/orig_len ride both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("UI",
                         "back_translate_enqueue fwd_len=%zu orig_len=%zu back_tgt=%s fwd_tgt=%s",
                         job.forward_text.size(), job.original_text.size(),
                         job.back_target.c_str(), job.forward_target.c_str());
            } else {
                DIAG_LOG("UI",
                         "back_translate_enqueue fwd_len=%zu orig_len=%zu back_tgt_len=%zu fwd_tgt_len=%zu",
                         job.forward_text.size(), job.original_text.size(),
                         job.back_target.size(), job.forward_target.size());
            }
            back_translate_worker.Submit(std::move(job));
        });

    // REQ-047 D1 (design §A-1/A.4, Tech Gate §D1) + P5-F1 (technical review
    // 175600 finding #1): surface the strict local failure that REQ-046
    // d4d5108 deleted — the "Enter pressed, nothing translated, no notice"
    // symptom. The worker thread calls this on exactly two outcomes (see the
    // worker.cpp site):
    //   * TranslationStatus::CloudConsentBlocked / LocalModelMissing — the
    //     strict local path failed with the H2 consent gate closed or no local
    //     source at all. Marshal the existing RepairFailed* modal through the
    //     SEC-ADJ value-queue seam (RequestEngineUnavailableModal is
    //     thread-safe from any thread; Tech Gate Item 6).
    //   * TranslationStatus::Ok with a non-empty result — the streak that
    //     gated the modal is over. Send the EMPTY-title sentinel through the
    //     SAME seam; the kMsgEngineUnavailableModal drain handler recognizes
    //     it and clears the GUI-thread latch so a later recurrence re-arms.
    // P5-F1: the wrapper NO LONGER reads g_engine_modal_latched. The latch
    // judgment (suppress-when-latched) moved into the kMsgEngineUnavailableModal
    // drain handler — the only GUI-thread site — so this callback, which runs
    // on the WORKER thread, never touches GUI-only state (design §A.4 ⚠️,
    // Tech Gate §D1 #2: no cross-thread bool sharing).
    worker.SetEngineUnavailableCallback([&config](emebalachat::TranslationStatus status) {
        if (status == emebalachat::TranslationStatus::Ok) {
            // Success sentinel: no title/body — the drain handler treats an
            // empty request as the latch-reset signal.
            emebalachat::RequestEngineUnavailableModal(emebalachat::g_hControllerWnd,
                                                       std::wstring(), std::wstring());
            return;
        }
        DIAG_F("MAIN/EngineModal/020: strict local failure status=%d; "
               "requesting engine-unavailable modal\n",
               static_cast<int>(status));
        // REQ-048 R4: split the wording by cause. LocalModelMissing = no local
        // source at all (the honest "engine files missing" case) keeps the
        // RepairFailed* wording. CloudConsentBlocked at this seam means the
        // host IS present but a serving attempt failed (cold-load timeout,
        // transient pipe error) - telling the user "files are missing, reinstall"
        // for a transient failure sent users into pointless reinstalls, so the
        // transient state gets its own retry-oriented body (repair_transient_body).
        // V10F E2 (261122_0002 P3-C4 wave B): LocalModelMissing now also arrives
        // from the post-request convergence (engine.cpp reports model_missing
        // truthfully), so that cause routes to the one-click offer helper
        // (built-in download consent / user-gguf manager) instead of the
        // dead-end wording; CloudConsentBlocked is unchanged. The engine_type
        // read is the locked snapshot (this callback runs on the worker
        // thread; the snapshot accessor is its I4 contract).
        // 261124_0001 (design §2.3 C5): LocalModelLoadFailed = the model file
        // EXISTS but the engine refused the load (too-new arch/quant, corrupt
        // file). The user_gguf pick keeps the established file-presence route
        // (its manager + compatibility hint already word this case); every
        // other engine type gets the dedicated wording — re-downloading the
        // same file is meaningless, so NO download offer and no reinstall
        // guidance (the offer flag stays defaulted off).
        if (status == emebalachat::TranslationStatus::LocalModelLoadFailed) {
            const auto load_failed_snap = config.GetSnapshot();
            if (load_failed_snap.engine_type == "user_gguf") {
                emebalachat::RequestModelMissingOfferOrModal(load_failed_snap.engine_type,
                                                             load_failed_snap.user_model_id);
                return;
            }
            emebalachat::RequestEngineUnavailableModal(
                emebalachat::g_hControllerWnd,
                emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedTitle),
                emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedBody));
            return;
        }
        if (status == emebalachat::TranslationStatus::LocalModelMissing) {
            // 261123_0001: the user_model_id rides the SAME locked snapshot as
            // engine_type (Bug A's file-presence classification is an I4 read).
            const auto missing_snap = config.GetSnapshot();
            emebalachat::RequestModelMissingOfferOrModal(missing_snap.engine_type,
                                                         missing_snap.user_model_id);
            return;
        }
        emebalachat::RequestEngineUnavailableModal(
            emebalachat::g_hControllerWnd,
            emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
            emebalachat::I18n::Get(emebalachat::StringId::RepairTransientBody));
    });

    // REQ-005 (plan §2.2): branded About popup. Singleton next to the tooltip;
    // Create failures degrade to a no-op About menu item (all public methods
    // guard hwnd_), mirroring the badge's graceful-degradation contract.
    emebalachat::AboutWindow about_window;
    if (!about_window.Create(hInstance)) {
        DIAG_F("MAIN/WinMain/007: AboutWindow::Create failed; About menu item disabled\n");
    }

    emebalachat::MouseHook mouse_hook;
    emebalachat::g_pMouseHook = &mouse_hook; // REQ-R14 resume/unlock re-registration

    // SC-02: single writer for the tray status refresh. Every GUI-thread site
    // that used to spell out the UpdateStatus call now funnels through this
    // lambda so the argument list lives in exactly one place.
    // Snapshot read inside keeps the I4 contract (consistent, locked read).
    // GUI-thread only (all call sites are tray callbacks / coordinators).
    // Declared ahead of ApplyLanguageChange because that coordinator's Tray
    // case references it (name lookup in a lambda body binds at definition).
    auto refresh_tray = [&]() {
        const auto snap = config.GetSnapshot();
        // Phase 3 Batch 2 (plan §2.4): the tray tip/checkmarks display the
        // TYPE pair (badge rationale: one displayed pair, the main pipeline).
        // REQ-025 (Phase A §2.1.A3-25): the drag pair is passed through as
        // well, but ONLY drives the new "번역툴팁" submenu check marks - the
        // hover tip keeps showing the type pair.
        // REQ-056: the Engine submenu's config-derived values (check index +
        // user-model stem) moved OUT of this push path into the
        // SetEngineMenuResolver lambda below — derive-at-open from the single
        // authority, no cached copies.
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),          // display-only (tooltip + log)
            snap.type_source_language,
            snap.type_target_language,
            snap.drag_source_language,
            snap.drag_target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
        // 261108_0002 (WS3): the floating-back-translation check mark rides the
        // SAME single-writer refresh as every other tray state, so the startup
        // call, the toggle callback and any later refresh_tray() site can never
        // leave the row out of sync with config.json (the SC-02 discipline).
        tray.SetBackTranslationEnabled(snap.back_translation_enabled);
        // V10F P3-C2 (261122_0002, UX-3 Option A wave B): the badge's right
        // mini-tag state rides the SAME single-writer refresh (SetEngineTag /
        // SetAutoSendIndicator are thread-safe marshals, badge.hpp's contract),
        // so EVERY engine switch (tray radio / OpenAI save / gguf registration)
        // and every auto-send surface that funnels through here keeps the chip
        // truthful with zero per-site push calls. The raw persisted engine_type
        // token names the chip (G/L/AI/U/A); BadgeEngineTagFor's default arm
        // covers "auto" and the unreachable unknown token.
        badge.SetEngineTag(emebalachat::BadgeEngineTagFor(snap.engine_type));
        badge.SetAutoSendIndicator(snap.auto_send);
    };

    // 261116_0003 P9b-3 U3 (plan §5 P9, D6 first-use capability probe): the
    // probe runner. Lives beside the coordinator it triggers from: the engine
    // call runs OFF the GUI thread (Translate blocks, engine.hpp:401-408) and
    // the verdict records back through the locked config verb on the GUI
    // thread. Declared after engine/config so it destructs before them; retired
    // at the shutdown sweep with the other coordinators. The recorded callback
    // persists each verdict (INV-3 discipline; the runner itself does no file
    // I/O); U4's surface refresh will ride the same callback.
    emebalachat::UserLanguageProbeService probe_service(
        config, engine, emebalachat::g_hControllerWnd);
    probe_service.SetRecordedCallback([&config] { config.SaveToFile(); });
    // Publish the GUI-thread completion drain for ControllerWndProc (the
    // g_apply_language_change pattern).
    emebalachat::g_drain_user_language_probes = [&probe_service] {
        probe_service.DrainOutcomes();
    };

    // R6 Phase 1 (B3, architect plan §2.3/§2.4 Option A): the single-source-of-
    // truth language coordinator. AppConfig is the ONE authority; every language
    // mutation - from ANY surface (tooltip language menu, tray source/target
    // submenus, tray Swap / badge double-click, Ctrl+F9 cycle, startup config
    // load) - routes through this one function so config.json, the badge, the
    // tray tip/checkmarks and the visible tooltip can never drift apart again
    // (the reported B3 desync bug). All decision logic lives in the pure
    // PlanLanguageSync seam (config.hpp), pinned by TestB3LanguageSync.
    //
    // Phase 3 Batch 2 (plan §2.4): the coordinator takes the LanguageContext of
    // the mutation as its first argument and owns the SURFACE FILTERING. Drag
    // mutations (tooltip language menu, drag / double-Ctrl+C src==tgt fallback,
    // REQ-025 tray drag submenus) rewrite only the drag pair and refresh the
    // tooltip + the tray menu's drag check marks (the tray TIP keeps showing
    // the type pair - refresh_tray passes type as src/tgt); Type mutations
    // (Ctrl+F9 cycle, tray submenus, swap) rewrite the type pair and refresh
    // badge -> tray -> tooltip (badge and tray DISPLAY the type pair).
    //
    // Thread contract (plan §2.3): must run on the GUI thread. Every tray/menu/
    // tooltip callback already does; the hook thread's Ctrl+F9 cycle and the
    // drag / double-Ctrl+C worker threads marshal through RequestLanguageSync
    // (kMsgApplyLanguageSync on the controller window). Badge/tray/tooltip view
    // updates additionally marshal internally (REQ-R10 seams), so this function
    // never touches another thread's D2D target directly.
    //
    // Returns true when the (possibly no-op) plan was applied. Invalid
    // (unresolvable) requests are refused WITHOUT any write - config and all
    // surfaces keep the previous consistent state (INV-1 guard).
    auto ApplyLanguageChange =
        [&](emebalachat::LanguageContext ctx,
            std::string_view new_source, std::string_view new_target,
            bool cycle_target, bool play_chime) -> bool {
        auto snap = config.GetSnapshot(); // I4: consistent read
        const bool drag = (ctx == emebalachat::LanguageContext::Drag);
        // Phase 3: read the CURRENT pair of the context being mutated. The
        // legacy source_language/target_language fields are migration-only
        // (plan §1.2) and no longer participate in any runtime path.
        const std::string cur_src = drag ? snap.drag_source_language
                                         : snap.type_source_language;
        const std::string cur_tgt = drag ? snap.drag_target_language
                                         : snap.type_target_language;
        std::string req_src(new_source);
        std::string req_tgt(new_target);
        if (cycle_target) {
            // Ctrl+F9 cycle (Type context): next target from the CURRENT
            // persisted target of the mutated context.
            // 261116_0003 P8b (plan §4 "Routing" acceptance, closes the P6
            // deferral on the 2-arg overload): the cycle set is the ACTIVE
            // engine's registry view, derived exactly like the tray menus
            // (tray.hpp) so the hotkey and the menu can never disagree about
            // the offered set; under "auto" that is the Google view (the
            // index-0 row). Pinned headlessly by TestP8bCtrlF9CycleWiring.
            const emebalachat::LanguageEngine cycle_engine =
                emebalachat::TrayLanguageEngineFor(
                    emebalachat::TrayPreferredEngineIndex(snap.engine_type));
            req_tgt = emebalachat::CycleTargetLanguage(cur_tgt, cycle_engine);
            // Shape-only: the engine enum value, never the target string.
            DIAG_LOG("STATE", "lang_cycle ctx=%s engine=%u",
                     drag ? "drag" : "type",
                     static_cast<unsigned>(cycle_engine));
        }
        const emebalachat::LanguageSyncPlan plan = emebalachat::PlanLanguageSync(
            ctx, cur_src, cur_tgt, req_src, req_tgt, config.user_languages);
        // 261116_0003 P9b-2 (plan §3 D5 + §5 P9): the sixth argument is the
        // user-language registry. The menu's "내 언어" rows send the entry's
        // STORED name (UserLanguageStoredName) - the same payload shape every
        // other surface sends - and P9a's planner validates it against this
        // vector, canonicalizing to the stored form so a re-pick is a no-op
        // (changed == false, no disk churn). Scope is exactly this call: an
        // unregistered token ("Elvish", "Klingon") and the AUTO-target refusal
        // keep their pre-P9b-2 behavior, and the 4/5-argument overloads stay
        // registry-only, so no other caller gains the allowance. The member is
        // read directly (config.hpp: GUI-thread-only, no Snapshot entry) because
        // this coordinator runs on the GUI thread, the only writer of the
        // registry.
        if (!plan.valid) {
            // fix-list-round15 R15-21 (F-3b): the refused request's src/tgt
            // are the RAW user-typed planner tokens (a user-language pick
            // stores its name as the planner token) — the R14-06 class — so
            // they log only behind diag::ContentLoggingEnabled(); the else
            // keeps the shape-only lengths. ctx rides both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_F("MAIN/LangSync/001: refused unresolvable language request (ctx=%s, src='%s', tgt='%s')\n",
                        drag ? "drag" : "type",
                        std::string(new_source).c_str(), std::string(new_target).c_str());
            } else {
                DIAG_F("MAIN/LangSync/001: refused unresolvable language request (ctx=%s, src_len=%zu, tgt_len=%zu)\n",
                        drag ? "drag" : "type", new_source.size(), new_target.size());
            }
            return false;
        }
        // 260905 diagnostics: the language-change record (old -> new) at the
        // single authority every surface now routes through. The ctx field is
        // the Phase 3 addition: which pair the record mutated.
        // fix-list-round15 R15-12: the four planner fields can BE user-typed
        // stored names (the master sink ON while the content gate is OFF), so
        // the names log only behind diag::ContentLoggingEnabled() - the R14-06
        // pattern of the tooltip menu_select_user arms; the else branch keeps
        // the shape-only lengths. ctx/valid/changed ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE",
                     "lang_sync ctx=%s valid=%d changed=%d pair %s/%s -> %s/%s (req %s/%s)",
                     drag ? "drag" : "type",
                     plan.valid ? 1 : 0, plan.changed ? 1 : 0,
                     cur_src.c_str(), cur_tgt.c_str(),
                     plan.source_language.c_str(), plan.target_language.c_str(),
                     req_src.empty() ? "-" : req_src.c_str(),
                     req_tgt.empty() ? "-" : req_tgt.c_str());
        } else {
            DIAG_LOG("STATE",
                     "lang_sync ctx=%s valid=%d changed=%d pair_len=%zu/%zu -> %zu/%zu "
                     "(req_len %zu/%zu)",
                     drag ? "drag" : "type",
                     plan.valid ? 1 : 0, plan.changed ? 1 : 0,
                     cur_src.size(), cur_tgt.size(),
                     plan.source_language.size(), plan.target_language.size(),
                     req_src.size(), req_tgt.size());
        }
        // INV-3: persistence is synchronous inside this call. A no-op re-pick
        // of the already-active language skips the locked write and the disk
        // churn, but the view refreshes below still run (self-heal against
        // any external drift).
        if (plan.changed) {
            // Phase 3 (plan §2.3): the pair's two fields update atomically
            // under mutex_ - the single locked write for this context.
            if (drag) {
                config.SetDragLanguages(plan.source_language, plan.target_language);
            } else {
                config.SetTypeLanguages(plan.source_language, plan.target_language);
            }
            config.SaveToFile();
            snap = config.GetSnapshot(); // read the authoritative post-write state
            // V10F (261122_0002 P3-C1 wave B): every TARGET change applied
            // through this ONE coordinator - the tray submenus, the shared
            // picker's commits, the swap rows, the Ctrl+F9 cycle - forwards to
            // the tray's recent-TARGET MRU through NoteRecentLanguage (the
            // wave-B contract; tray-driven picks are ALSO recorded at the
            // tray's own dispatch, and the MRU's move-to-front dedupe makes the
            // double-record idempotent). GUI thread only, both sides.
            // plan.target_language is the resolved canonical form; a
            // user-language stored name normalizes to AUTO inside the tray and
            // is dropped there (the same rule the tray's own rows apply).
            if (!req_tgt.empty()) {
                tray.NoteRecentLanguage(plan.target_language);
            }
        }
        // 261116_0003 P9b-3 U3 (D6 first-use capability probe): the accepted
        // plan's TARGET resolved to a user-defined language on an
        // evidence-judging engine (OpenAI-compatible / user-.gguf; Google is
        // publisher-declared, the local built-in and Auto have no custom row).
        // Fire ONE probe while the effective status is still Unknown and no
        // override exists (the contract at config.hpp:342-345, :351-355); the
        // engine call runs off the GUI thread and the verdict records through
        // the locked verb when the completion drains back here. No-op for
        // registry targets, answered entries, and in-flight keys. The engine
        // resolves exactly like the Ctrl+F9 cycle idiom above.
        {
            const emebalachat::LanguageEngine probe_engine =
                emebalachat::TrayLanguageEngineFor(
                    emebalachat::TrayPreferredEngineIndex(snap.engine_type));
            probe_service.MaybeTriggerForPick(probe_engine, plan.target_language);
        }
        // F2 (session 260908_0003): any VALID explicit Drag-TARGET request
        // pins the provenance flag - even a no-change re-pick of the
        // already-active target is the user re-affirming a deliberate
        // choice, and a pick that happens to collide with the detected
        // source must be honoured verbatim (zero pivot on user choice).
        // `changed` alone cannot gate this: a same-value re-pick is changed==false,
        // yet the user intent ("this value is MINE") is identical. The pin
        // write happens for explicit target requests only; source-only picks
        // (req_tgt empty) leave the target provenance untouched. This is the
        // ONLY place user picks set the pin; the reset coordinator clears it.
        // INV-3 disk discipline: a re-pick that neither changes the pair nor
        // flips the pin (already pinned) stays a no-op write - the flag is
        // already persisted, only the log line fires.
        if (drag && plan.valid && !req_tgt.empty()) {
            const bool was_pinned = snap.drag_target_pinned;
            config.SetDragTargetPinned(true);
            if (!plan.changed || !was_pinned) {
                config.SaveToFile(); // persist the flip (or the pair change) -
                                     // an already-pinned no-op re-pick keeps
                                     // INV-3's no-churn rule for the disk.
            }
            // fix-list-round15 R15-21 (F-3b): tgt is a language token (can be
            // a user-typed stored name) — the R14-06 class — so it logs only
            // behind diag::ContentLoggingEnabled(); the else keeps the
            // shape-only length.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("STATE", "drag_target_pinned tgt=%s (F2 user-explicit pick)",
                         plan.target_language.c_str());
            } else {
                DIAG_LOG("STATE", "drag_target_pinned tgt_len=%zu (F2 user-explicit pick)",
                         plan.target_language.size());
            }
        }
        // plan.surface_updates order (planner): Badge -> Tray -> Tooltip.
        // Phase 3 surface filtering (plan §2.4): the planner stays pair-
        // agnostic and lists all three; the coordinator drops Badge/Tray for
        // Drag-context changes because those surfaces display the TYPE pair
        // and are unaffected by a drag-pair mutation.
        for (const emebalachat::LanguageSurface surface : plan.surface_updates) {
            switch (surface) {
                case emebalachat::LanguageSurface::Badge:
                    if (!drag) {
                        badge.SetLanguages(emebalachat::ToUtf16(snap.type_source_language),
                                           emebalachat::ToUtf16(snap.type_target_language));
                    }
                    break;
                case emebalachat::LanguageSurface::Tray:
                    // REQ-025 (Phase A §2.1.A3-25, DP-2): refresh for BOTH
                    // contexts now. The tray tip is built from the type pair
                    // (refresh_tray passes type as src/tgt), so a drag change
                    // leaves the visible tip identical; the call exists to
                    // re-mirror drag_src_code_/drag_tgt_code_ so the "번역툴팁"
                    // submenu check marks track the persisted drag pair.
                    refresh_tray();
                    break;
                case emebalachat::LanguageSurface::Tooltip:
                    // Best-effort view sync: no-op while hidden/message-mode.
                    // Phase 3: the tooltip is exclusively the drag-result
                    // window, so its label ALWAYS reflects the drag target -
                    // on Type-context changes this refresh is just the legacy
                    // self-heal nudge and must not relabel the tooltip with a
                    // type value the shown content was not translated to.
                    tooltip.RefreshTargetLanguageFromConfig(snap.drag_target_language);
                    break;
            }
        }
        if (plan.changed && play_chime) {
            emebalachat::PlayLangChange();
        }
        // W3 (R1-22): Ctrl+F9 used to be a blind cycle - a chime and nothing
        // else, so stepping through that era's 37 targets (89 today) gave no
        // feedback about which one landed. Confirm the NEW target at the cursor, through the same
        // thread-safe tooltip seam the F9 toggle bubble uses (it marshals to
        // the GUI thread, and this coordinator only ever runs there).
        // Exactly ONE notice per cycle: the hook Ctrl+F9 callback is the only
        // producer that sets cycle_target, so no other source can double it.
        if (cycle_target) {
            POINT cursor = {};
            ::GetCursorPos(&cursor);
            tooltip.ShowMessageThreadSafe(
                cursor.x + 12, cursor.y + 12,
                emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                emebalachat::I18n::GetLanguageDisplayName(plan.target_language));
            // fix-list-round15 R15-21 (F-3b): tgt is a language token (can be
            // a user-typed stored name) — the R14-06 class — so it logs only
            // behind diag::ContentLoggingEnabled(); ctx rides both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("UI", "cycle_confirm tgt=%s ctx=%s",
                         plan.target_language.c_str(), drag ? "drag" : "type");
            } else {
                DIAG_LOG("UI", "cycle_confirm tgt_len=%zu ctx=%s",
                         plan.target_language.size(), drag ? "drag" : "type");
            }
        }
        return true;
    };
    // Publish for the controller WndProc (hook/worker-thread marshal target).
    emebalachat::g_apply_language_change = ApplyLanguageChange;

    // REQ-047 D3 (design §C.2 안 C-1): publish the OpenAI save/switch body for
    // the deferred kMsgOpenOpenAiSettings handler. This is the SAME logic that
    // used to run inline in on_select_engine(2) — moved, not rewritten. On
    // save it persists the config, re-points the runtime engine, and refreshes
    // the tray (gated on the switch actually applying); on cancel it only
    // refreshes the tray, leaving the previous engine untouched.
    // 261116_0003 P9c-1c: `declared` is the dialog's declaration fold, non-null
    // only on the save path. It is applied HERE, on this owning AppConfig,
    // through the locked verb and BEFORE the save below, so the config.json
    // this run writes carries exactly the set the dialog folded (an empty set
    // clears the declaration and the serializer then emits no key at all).
    // The dialog itself performs no config file I/O for the declaration any
    // more, so this is the single apply path and the in-memory instance can
    // never be reverted by a stale re-emission.
    // 261122_0001 B-U5 (design-pick item 4): `profiles` is the dialog's
    // profile LIBRARY fold, non-null only on the save path. The staged library
    // is adopted HERE, on this owning AppConfig, BEFORE the single save below
    // — the dialog performs no config file I/O for profiles either, so this
    // SaveToFile is the one persist site (P9c-1c single-writer).
    emebalachat::g_apply_openai_settings = [&](const emebalachat::OpenAiConfig& edited,
                                               bool saved,
                                               const std::vector<std::string>* declared,
                                               emebalachat::OpenAiProfilesFold* profiles) -> bool {
        if (!saved) {
            refresh_tray();
            return false;
        }
        // 261122_0001 fix-E (round-2 N2): through the locked verb
        // (SetOpenAiConfig), never a direct member write — an off-thread
        // SaveToFile serialises this block under mutex_.
        config.SetOpenAiConfig(edited);
        // 261122_0001 B-U5 (design-pick item 4): adopt the staged profile
        // library on the LIVE config BEFORE the single save below (nullptr on
        // Cancel and never on kDeleted, so the stored library survives both).
        // 261122_0001 fix-A2 (R1-02 caller half): through the locked verb
        // (SetOpenAiProfiles), never a direct member write — an off-thread
        // SaveToFile iterates this vector under mutex_.
        if (profiles && profiles->edited) {
            config.SetOpenAiProfiles(std::move(profiles->profiles));
        }
        engine.SetOpenAiConfig(config.openai);
        engine.SetEngineType(emebalachat::EngineType::OpenAi);
        // REQ-051 U-2: leaving the user-model route clears its display stem.
        engine.SetUserModelDisplayStem({});
        config.SetEngineTypeName("openai");
        if (declared) {
            // One verb, one publication: the locked write canonicalizes the
            // fold and publishes the new set, so the tray menus, the tooltip
            // dropdowns, the search picker and the Ctrl+F9 cycle already
            // filter by it before refresh_tray() runs below.
            config.SetOpenAiDeclaredLanguages(*declared);
        }
        config.SaveToFile();
        refresh_tray();
        DIAG_F("MAIN/on_select_engine/002: engine switch persisted; active=%s\n",
               engine.GetActiveEngineName().c_str());
        return true;
    };
    // REQ-051 (Symptom D): the [삭제] reconcile body for the kDeleted outcome.
    // Mirror the dialog-cleared openai block into the in-memory config (the
    // dialog's own persist already wrote the file; without this mirror the
    // unconditional exit save would resurrect the deleted settings), re-point
    // the runtime engine's openai block, persist, refresh the tray.
    // engine_type / the engine selection is deliberately NOT touched here —
    // the user-frozen scope is "settings only" (decisions.md 260921 17:08).
    // 261122_0001 fix-A2 (R1-01): this is the SECOND persist site the verifier
    // named. On a pre-B2 config the startup migration synthesized a library
    // entry that is a verbatim mirror of the block being deleted, and this save
    // would re-emit it (the dialog's own save cannot cover it — the two are
    // independent copies of the library). Prune every entry carrying the
    // retired credential with the dialog's helper, through the locked verb, so
    // unrelated profiles survive and the pair is gone from both writes.
    emebalachat::g_apply_openai_deleted = [&](const emebalachat::OpenAiConfig& edited) {
        // Capture the identity BEFORE the mirror assignment below: `edited` is
        // already cleared (the dialog ends on kDeleted with an empty block).
        const emebalachat::OpenAiConfig retired = config.openai;
        // 261122_0001 fix-E (round-2 N2): locked verb, never a direct member
        // write (same PR9-12 race as the g_apply_openai_settings site).
        config.SetOpenAiConfig(edited);
        // V10F-R1 F-17: the deleted block carries no http consent, so the
        // stored origin scope is revoked with it (the locked verb, before the
        // save below — the [설정 삭제] path persists its own clear through a
        // second AppConfig instance too, see the dialog unit).
        config.SetHttpConsentOrigin("");
        // 261122_0001 fix-E (round-2 seat1 N-01): this direct read is safe by
        // the member invariant — every WRITER of openai_profiles is GUI-thread-
        // only through the locked verbs, and off-thread SaveToFile reads under
        // the same mutex_, so this is a read-read with no writer interleaving.
        std::vector<emebalachat::OpenAiProfile> kept_profiles = config.openai_profiles;
        if (emebalachat::EraseOpenAiProfileMatchingKey(kept_profiles, retired) > 0) {
            config.SetOpenAiProfiles(std::move(kept_profiles));
        }
        engine.SetOpenAiConfig(config.openai);
        config.SaveToFile();
        refresh_tray();
    };
    // V10F-R1 F-17: the http-consent origin seam body. Applies the scope on the
    // LIVE config through the locked verb; the kSaved branch invokes this
    // BEFORE g_apply_openai_settings, whose single save persists it (this body
    // deliberately performs no save of its own — one writer, one save, the
    // P9c-1c discipline).
    emebalachat::g_apply_openai_consent_origin = [&](const std::string& origin) {
        config.SetHttpConsentOrigin(origin);
    };

    // REQ-048 R2-D + REQ-050: publish the gguf-model-manager open for the
    // deferred kMsgOpenGgufManager handler (the g_apply_openai_settings
    // pattern). The lambda owns the ONLY call site: modal dialog on
    // g_hControllerWnd, then a tray refresh — rename/delete can move the
    // engine submenu's check mark (a deleted active binding falls back to
    // "auto") or the user-model stem label, so the refresh runs
    // unconditionally (idempotent, like the OpenAI cancel path). REQ-050:
    // the manager's [파일에서 추가…] button runs the SAME registration
    // pipeline body the old on_browse_gguf tray callback used
    // (RegisterUserGgufModel + refresh_tray), passed in as the
    // add_from_file argument; the helper lives in this TU's anon namespace
    // (fully qualified — wWinMain is global scope) and is deliberately not
    // linked into Emebalachat_core, which is why it travels as a callback.
    emebalachat::g_open_gguf_model_manager = [&config, &engine, &refresh_tray]() {
        emebalachat::ShowGgufModelManagerDialog(
            emebalachat::g_hControllerWnd, config, engine,
            [&config, &engine, &refresh_tray]() {
                emebalachat::RegisterUserGgufModel(config, engine);
                refresh_tray();
            });
        refresh_tray();
    };

    // REQ-060 (session 261009_0001): publish the load-settings open for the
    // deferred kMsgOpenEngineLoadSettings handler (the g_open_gguf_model_manager
    // sibling pattern). The lambda owns the ONLY call site: modal dialog on
    // g_hControllerWnd; on OK it copies the edited engine_host block back into
    // the in-memory config (the dialog performs NO file I/O — ui-report §3),
    // persists via SaveToFile (the g_apply_openai_settings precedent ignores
    // the bool result), re-pushes the block into the runtime engine —
    // SetEngineHostConfig -> RefreshActiveEngine -> engine_host::WarmupSetState
    // (engine.cpp) re-evaluates the warmup persistent-connection manager
    // immediately, no app restart — and refreshes the tray. Cancel leaves
    // everything untouched (the dialog returns false and `out` is discarded).
    emebalachat::g_open_engine_host_settings = [&config, &engine, &refresh_tray]() {
        emebalachat::EngineHostConfig out;
        if (emebalachat::ShowEngineHostSettingsDialog(emebalachat::g_hControllerWnd,
                                                      config.engine_host, out)) {
            // V10F BUG-08: through the locked verb SetEngineHostConfig (the
            // P1-U4 mirror of SetOpenAiConfig), never a direct member write -
            // an off-thread SaveToFile serialises this block under mutex_.
            // DEPENDENCY: AppConfig::SetEngineHostConfig is added by unit
            // P1-U4 (config.hpp/cpp); until it lands this call does not
            // compile.
            config.SetEngineHostConfig(out); // verbatim: enabled/spawn/idle_exit_ms/worker_idle_ms + the edited warmup
            config.SaveToFile();
            engine.SetEngineHostConfig(config.engine_host); // REQ-060: RefreshActiveEngine -> WarmupSetState
            refresh_tray();
            DIAG_F("MAIN/EngineLoadSettings/001: load-mode settings saved; warmup=%d\n",
                   config.engine_host.warmup ? 1 : 0);
        }
    };

    // 261116_0003 P9b-3 U4a: publish the user-language ADD open for the
    // deferred kMsgOpenUserLangAdd handler (the g_open_gguf_model_manager
    // sibling pattern). The lambda owns the ONLY call site: U2's modal
    // ShowUserLanguageDialogEx on g_hControllerWnd. The dialog writes IN
    // MEMORY through U1's locked verbs and performs NO file I/O (U2's
    // boundary), so the caller persists on kAdded through the existing
    // SaveToFile path and refreshes the tray (the menus rebuild per open, so
    // the refresh is the house consistency contract, not a data push). On
    // kCancelled nothing happened: no persist, no refresh.
    emebalachat::g_open_user_lang_add = [&config, &refresh_tray]() {
        if (emebalachat::user_language_dialog::ShowUserLanguageDialogEx(
                emebalachat::g_hControllerWnd, config) ==
            emebalachat::UserLanguageDialogOutcome::kAdded) {
            config.SaveToFile();
            refresh_tray();
        }
    };

    // 261116_0003 P9b-3 U4a: publish the user-language MANAGER open for the
    // deferred kMsgOpenUserLangManager handler (the same sibling pattern). The
    // engine + model resolution is the SAME one the badge rows read
    // (U4b's handoff: TrayLanguageEngineFor(TrayPreferredEngineIndex(
    // snap.engine_type)) + UserLanguageProbeModelFor), so the dialog and every
    // menu row show ONE verdict per (engine, model). The dialog returns
    // kChanged only when an override verb APPLIED in memory (a retest launch
    // is NOT a change — U3's recorded callback already persists the verdict),
    // so the caller persists on kChanged and refreshes the tray; kClosed does
    // neither.
    emebalachat::g_open_user_lang_manager = [&config, &probe_service, &refresh_tray]() {
        const auto snap = config.GetSnapshot();
        const emebalachat::LanguageEngine probe_engine =
            emebalachat::TrayLanguageEngineFor(
                emebalachat::TrayPreferredEngineIndex(snap.engine_type));
        const std::wstring model =
            emebalachat::ToUtf16(emebalachat::UserLanguageProbeModelFor(probe_engine, config));
        if (emebalachat::user_language_manager_dialog::ShowUserLanguageManagerDialogEx(
                emebalachat::g_hControllerWnd, config, probe_engine, model,
                &probe_service) ==
            emebalachat::UserLanguageManagerOutcome::kChanged) {
            config.SaveToFile();
            refresh_tray();
        }
    };

    // R6 Phase 6 (plan §5.4): locale-change propagation coordinator, mirroring
    // the ApplyLanguageChange pattern above. After I18n::SetLocale, re-render
    // every surface in the plan's order (tray -> badge -> tooltip -> About).
    // GUI-thread-only: called from the tray selector callback (its
    // TrackPopupMenuEx runs on this thread). Surfaces that cache localized
    // strings outside Render() are handled explicitly:
    //   * tray: menu rebuilds lazily on every ShowContextMenu (fresh I18n::Get
    //     each build); UpdateStatus refreshes the localized icon tip.
    //   * badge: SetLanguages nudge forces a repaint of the localized parts.
    //   * tooltip: RefreshTargetLanguageFromConfig repaints while visible (the
    //     same value is fine - it is a view-refresh nudge); hidden tooltips
    //     pick up new strings on the next Show (plan: acceptable).
    //   * About: RequestLocaleRefresh re-applies the caption and repaints the
    //     localized body while visible.
    auto RefreshAllUiForLocaleChange = [&]() {
        const auto snap = config.GetSnapshot(); // I4: consistent read
        tray.SetUiLanguage(snap.ui_language);   // submenu check-mark mirror
        refresh_tray(); // Phase 3: refresh_tray already reads the type pair
        // Phase 3 Batch 2 (plan §2.4): badge displays the type pair; the
        // tooltip label follows the drag pair it was last shown with.
        badge.SetLanguages(emebalachat::ToUtf16(snap.type_source_language),
                           emebalachat::ToUtf16(snap.type_target_language));
        tooltip.RefreshTargetLanguageFromConfig(snap.drag_target_language);
        about_window.RequestLocaleRefresh();
    };

    // Phase 4 (REQ-020): About-window "Reset to system defaults" coordinator.
    // GUI-thread only (invoked from AboutWindow::WndProc via the callback).
    // Rewrites BOTH context pairs to the system defaults (plan §1.2), persists
    // atomically, refreshes all three surfaces (both pairs changed), and plays
    // the language-change chime as audible feedback. NOT routed through
    // ApplyLanguageChange/PlanLanguageSync: a reset is a direct 4-field write,
    // not a request-resolve cycle (plan §2.3).
    //
    // P4 Batch B-6 G-1 (design §2.3.2, user-approved 2026-09-07): the reset
    // ALSO restores ui_language = "auto" ("system defaults" must include the
    // system-driven UI language — the strongest convenience case is a user who
    // set an unreadable UI language and needs this very button to recover).
    //
    // Coordinator ordering (design §5.2-5 "reset write -> refresh" invariant,
    // plus the input-correctness step the B-6 oracle surfaced):
    //   (0) re-resolve "auto" -> DetectSystemLocale and SetLocale FIRST.
    //       261123_0001 R2 (fix F-01): the four language DEFAULTS no longer
    //       depend on this step. Since 261123_0001 (Bug B) they are OS-sourced
    //       and Initialize-independent (ComputeSystemDefaultLanguages ->
    //       ResolveDragDefaultTarget reading DetectSystemLocale +
    //       LocaleToString), so step (1) writes the fresh-install values
    //       whatever locale the reset starts from. The pre-fix rationale,
    //       that the default was "consumed through
    //       I18n::GetSystemLanguageCode, i.e. the ACTIVE locale's code", with
    //       OS=th + UI=ja resetting to Japanese, described the old seam and
    //       is dead. What (0) still buys is the REFRESH input: the tray /
    //       badge / tooltip / About re-render in (2) resolves through the
    //       restored system locale, so (0) must run before (2). The
    //       reset write -> refresh invariant of (1) -> (2) is unchanged.
    //   (1) write all five keys (4 languages + ui_language "auto") and
    //       persist them under ONE SaveToFile atomic swap, so no restart can
    //       ever observe a half-reset config (G-1 without C5's persistence).
    //   (2) THEN RefreshAllUiForLocaleChange so every surface (tray
    //       check-mark + menu, badge, tooltip, About incl. its own
    //       reset-feedback repaint) re-renders through the new locale; the
    //       language-pair surface refreshes ride inside that coordinator (it
    //       re-reads the snapshot), replacing the old hand-rolled trio.
    auto apply_system_defaults = [&]() {
        emebalachat::I18n::SetLocale(emebalachat::I18n::DetectSystemLocale()); // (0) G-1
        const auto defs = emebalachat::ComputeSystemDefaultLanguages(); // pure
        config.SetDragLanguages(defs.drag_source, defs.drag_target);     // I4 locked
        // F2 (session 260908_0003): reset returns the drag target to the
        // never-touched AUTO default state, which includes CLEARING the
        // user-pinned provenance flag - the restored default must be
        // re-pivotable by the ADR-A1-7 collision logic like a fresh install
        // (task item 6: clear the pinned flag so defaults are re-pivotable).
        config.SetDragTargetPinned(false);                               // F2
        config.SetTypeLanguages(defs.type_source, defs.type_target);     // I4 locked
        config.SetUiLanguage("auto");                                    // G-1, I4 locked
        config.SetUiTheme("fluent");
        emebalachat::ui::SetActiveTheme(config.ui_theme);
        config.SaveToFile();                                             // (1) atomic swap
        const auto snap = config.GetSnapshot();
        // fix-list-round15 R15-21 (F-3b): the reset pair echo can carry
        // user-typed stored names (a user-language pick persists its name as
        // the planner token) — the R14-06 class — so the names log only
        // behind diag::ContentLoggingEnabled(); the else keeps the shape-only
        // lengths. ctx=both / ui=auto ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE", "lang_reset ctx=both pair %s/%s %s/%s ui=auto (REQ-020/B-6 G-1)",
                     snap.drag_source_language.c_str(), snap.drag_target_language.c_str(),
                     snap.type_source_language.c_str(), snap.type_target_language.c_str());
        } else {
            DIAG_LOG("STATE", "lang_reset ctx=both pair_len=%zu/%zu %zu/%zu ui=auto (REQ-020/B-6 G-1)",
                     snap.drag_source_language.size(), snap.drag_target_language.size(),
                     snap.type_source_language.size(), snap.type_target_language.size());
        }
        // (2) ALL surfaces (locale + both language pairs) re-render through
        // the existing locale-change coordinator — single refresh authority,
        // no duplicate surface nudges. The About window's own reset-feedback
        // repaint rides inside it (RequestLocaleRefresh).
        RefreshAllUiForLocaleChange();
        tray.SetUiTheme(config.ui_theme);
        tray.RefreshTheme();
        tooltip.RefreshTheme();
        about_window.RefreshTheme();
        // R1-I (v0.10.7, 261111_0001 fix-list round 1): a same-thread GUI
        // REPAINT of the already-shown icon at its CURRENT position after a
        // theme/locale change, deliberately NOT routed through the RequestShowAt
        // generation seam. The seam orders cross-thread show/HIDE requests (a
        // newer drag supersedes an older one; a Hide invalidates a pending
        // show); a repaint is neither - it refreshes the applied show and must
        // not stamp a new generation that could drop a newer in-flight drag.
        // Single-threaded GUI ordering makes the inline call safe: every inline
        // ShowAt runs on the GUI thread, serialized with the WndProc that drains
        // kShowAtMessage/kHideMessage, so a pending newer-drag show still applies
        // AFTER this repaint (its stamp is untouched) and wins. The IsVisible()
        // guard is only a cheap pre-filter, NOT a synchronisation with a hide:
        // an off-thread Hide() posts kHideMessage and the GUI-side teardown
        // clears the visible flag when that message is drained (drag_icon.cpp
        // HideWindow), so a concurrent hide can still read as visible here.
        // That is benign - this repaint re-applies the CURRENT position of an
        // icon the GUI thread still shows, and the queued hide then hides it;
        // a newer show stamped meanwhile is unaffected. (G-16 round 2: the
        // earlier "Hide() clears visible_ synchronously" wording was true only
        // for the inline GUI-thread hide, never for the hook-thread one.)
        if (drag_icon.IsVisible() && drag_icon.GetHwnd()) {
            RECT rcDrag = {};
            if (::GetWindowRect(drag_icon.GetHwnd(), &rcDrag)) {
                drag_icon.ShowAt(rcDrag.left, rcDrag.top);
            }
        }
        emebalachat::PlayLangChange();
    };
    // R6 Phase 1 (B3): Ctrl+F9 cycle now routes through the SAME coordinator.
    // Fires on the hook thread -> RequestLanguageSync posts to the controller
    // window (GUI thread). Set before hook.Start(); read-only afterwards
    // (active_change_cb_ contract).
    hook.SetLanguageCycleCallback([]() {
        // Phase 3 Batch 2 (plan §2.4): Ctrl+F9 cycles the TYPE target pair.
        emebalachat::RequestLanguageSync(emebalachat::g_hControllerWnd,
                                         emebalachat::LanguageContext::Type,
                                         std::string{}, std::string{}, true, true);
    });

    emebalachat::SystemTray::Callbacks trayCallbacks;
    trayCallbacks.on_toggle_active = [&]() {
        // REQ-R07: SetActive() fires active_change_cb_ on every real state
        // change, so the MouseHook sync happens there exactly once. No manual
        // SetEnabled call here anymore - it double-toggled nothing before only
        // because SetEnabled is idempotent; keeping it would risk drift.
        hook.ToggleActive();
    };

    // REQ-043 (M6 T5, plan §V2-8.6): the REQ-004 tray-switch PRELOAD machinery
    // is removed with the embedded engine. Switching to "local" now only
    // re-points routing at the shared inference host — the host's worker
    // process spawns on demand at the first host request, so there is no
    // resident model to warm up and no preload thread to join at shutdown.

    // V10F P3-C1 wave B (UX-1 Option B): the SEEDED DEFERRED OPEN the OpenAI
    // settings dialog uses, extracted from the old inline on_select_engine(2)
    // body so BOTH the engine submenu's separate "OpenAI 호환 엔진 설정…" row
    // (Callbacks::on_open_openai_settings) and the no-profile engine-radio
    // pick share ONE implementation. The contract is byte-identical to the
    // pre-wave-B branch: latch -> snapshot config.openai + the profile library
    // (by copy, no AppConfig alias survives) -> pure (0,0) kMsgOpenOpenAiSettings
    // wake-up, with the latch + snapshots rolled back on a failed post. The
    // save/switch body still resolves in g_apply_openai_settings on the GUI
    // thread (REQ-047 D3 modality contract).
    auto open_openai_settings_deferred = [&]() {
        if (emebalachat::s_openai_dialog_pending) {
            DIAG_F("MAIN/on_select_engine/007: openai settings open already deferred; coalesced\n");
            return;
        }
        emebalachat::s_openai_dialog_pending = true;
        emebalachat::s_pending_openai_cfg = config.openai;
        // 261122_0001 B-U5 (design-pick item 4): seed the deferred open with
        // the profile library snapshot too — a COPY, so the AppConfig vector
        // never aliases the fold the handler will hand the dialog.
        // 261122_0001 fix-A2 (R1-01) trace: this read stays unfiltered on
        // purpose. It is not a persist site — the kDeleted branch applies no
        // fold, and the delete's own prune runs on the live library in
        // g_apply_openai_deleted before that branch's save, so the copy
        // seeded here can never carry a retired credential back to the file
        // (the next open seeds the already-pruned library).
        // 261122_0001 fix-E (round-2 seat1 N-01): the copy is also safe
        // unlocked — all openai_profiles writers are GUI-thread-only via
        // the locked verbs (read-read, no concurrent writer exists).
        emebalachat::s_pending_openai_profiles = config.openai_profiles;
        if (::PostMessageW(emebalachat::g_hControllerWnd,
                           emebalachat::kMsgOpenOpenAiSettings, 0, 0) == FALSE) {
            // Post failed: roll the latch back so the user can retry; the
            // pending snapshot is dropped with it.
            emebalachat::s_openai_dialog_pending = false;
            emebalachat::s_pending_openai_cfg.reset();
            emebalachat::s_pending_openai_profiles.reset();
            DIAG_F("MAIN/on_select_engine/008: PostMessage openai-settings open failed (GLE %lu)\n",
                   ::GetLastError());
            return;
        }
        // The dialog open + engine switch now resolve in the
        // kMsgOpenOpenAiSettings handler on the GUI thread (after the menu
        // modality has fully torn down). SaveToFile / refresh_tray run
        // there via g_apply_openai_settings.
    };
    // The settings hub's Engines tab [OpenAI 호환 설정…] action shares the same
    // deferred open (the hub is modeless, but the dialog's REQ-047 D3 owner and
    // the kMsgOpenOpenAiSettings consumer are identical, so one implementation
    // keeps the latch/snapshot discipline in exactly one place).
    trayCallbacks.on_open_openai_settings = open_openai_settings_deferred;

    trayCallbacks.on_select_engine = [&](int engine_idx) {
        // REQ-029-B (design §2.1 change 4): the tray engine switch was a
        // complete log blind spot (decisions.md 2026-09-07 11:40 item 3 -
        // "why did it go back to local" was untraceable). 001 records the
        // user's request against the PREVIOUS persisted config; 002 confirms
        // the switch persisted and what the engine now honestly reports.
        // REQ-045 P4-3: 0 = Google, 1 = Local, 2 = OpenAI Compatible.
        // REQ-046 P4-2 (Rev2 §B-3, C3): 4-way log string + "unknown" fallback
        // so a stray index never silently logs a wrong engine name.
        const char* requested = (engine_idx == 0) ? "google"
                                : (engine_idx == 1) ? "local"
                                : (engine_idx == 2) ? "openai"
                                : (engine_idx == 3) ? "user_gguf" : "unknown";
        DIAG_F("MAIN/on_select_engine/001: user selected engine=%s (prev config=%s)\n",
               requested, config.GetSnapshot().engine_type.c_str());
        // I4: runtime mutations go through the locked setters because the hook
        // and worker threads read these fields concurrently.
        if (engine_idx == 0) {
            engine.SetEngineType(emebalachat::EngineType::GoogleTranslate);
            // REQ-051 U-2: leaving the user-model route clears its display
            // stem (all three non-user picks do this — google/local/openai).
            engine.SetUserModelDisplayStem({});
            config.SetEngineTypeName("google");
        } else if (engine_idx == 1) {
            engine.SetEngineType(emebalachat::EngineType::LocalLlama);
            // REQ-051 U-2: "local" is the PINNED Hy-MT2 route — the user-model
            // stem must not linger on it.
            engine.SetUserModelDisplayStem({});
            config.SetEngineTypeName("local");
        } else if (engine_idx == 2) {
            // V10F P3-C1 (UX-1 Option B, wave B): the radio row is the INSTANT
            // switch. An EXISTING configured profile (endpoint + model +
            // credential, OpenAiProfileConfigured above) switches engines
            // WITHOUT opening the settings modal - the modal lives on the
            // submenu's separate "OpenAI 호환 엔진 설정…" row now
            // (on_open_openai_settings, wired above). Only the NO-profile case
            // keeps routing into the deferred dialog (today's pre-V10F
            // behavior, shared with the settings row through
            // open_openai_settings_deferred).
            if (emebalachat::OpenAiProfileConfigured(config.openai)) {
                // The switch body mirrors the google/local arms + the
                // g_apply_openai_settings engine half: re-point the runtime
                // engine (refreshing its openai block from the live config so
                // an externally edited config.json is honored too), clear the
                // user-model display stem (REQ-051 U-2), persist, refresh.
                engine.SetOpenAiConfig(config.openai);
                engine.SetEngineType(emebalachat::EngineType::OpenAi);
                engine.SetUserModelDisplayStem({});
                config.SetEngineTypeName("openai");
                config.SaveToFile();
                refresh_tray();
                DIAG_F("MAIN/on_select_engine/002: engine switch persisted; active=%s\n",
                       engine.GetActiveEngineName().c_str());
                return;
            }
            DIAG_F("MAIN/on_select_engine/011: openai radio picked with no configured profile; opening settings dialog\n");
            // No profile: the deferred dialog open stays INLINE in this branch
            // (the req047_tests.inc D3 pin requires the PostMessageW +
            // kMsgOpenOpenAiSettings + latch to live inside the index-2
            // branch) - the body is identical to open_openai_settings_deferred
            // above, which serves the settings row and the hub; the two copies
            // are the same house pattern and must stay in sync (a future
            // test-worker re-pin may factor them).
            if (emebalachat::s_openai_dialog_pending) {
                DIAG_F("MAIN/on_select_engine/007: openai settings open already deferred; coalesced\n");
                return;
            }
            emebalachat::s_openai_dialog_pending = true;
            emebalachat::s_pending_openai_cfg = config.openai;
            // 261122_0001 B-U5 (design-pick item 4): seed the deferred open
            // with the profile library snapshot too — a COPY, so the AppConfig
            // vector never aliases the fold the handler will hand the dialog.
            // 261122_0001 fix-A2 (R1-01) trace: this read stays unfiltered on
            // purpose. It is not a persist site — the kDeleted branch applies
            // no fold, and the delete's own prune runs on the live library in
            // g_apply_openai_deleted before that branch's save, so the copy
            // seeded here can never carry a retired credential back to the
            // file (the next open seeds the already-pruned library).
            // 261122_0001 fix-E (round-2 seat1 N-01): the copy is also safe
            // unlocked — all openai_profiles writers are GUI-thread-only via
            // the locked verbs (read-read, no concurrent writer exists).
            emebalachat::s_pending_openai_profiles = config.openai_profiles;
            if (::PostMessageW(emebalachat::g_hControllerWnd,
                               emebalachat::kMsgOpenOpenAiSettings, 0, 0) == FALSE) {
                // Post failed: roll the latch back so the user can retry; the
                // pending snapshot is dropped with it.
                emebalachat::s_openai_dialog_pending = false;
                emebalachat::s_pending_openai_cfg.reset();
                emebalachat::s_pending_openai_profiles.reset();
                DIAG_F("MAIN/on_select_engine/008: PostMessage openai-settings open failed (GLE %lu)\n",
                       ::GetLastError());
                return;
            }
            // The dialog open + engine switch now resolve in the
            // kMsgOpenOpenAiSettings handler on the GUI thread (after the menu
            // modality has fully torn down). SaveToFile / refresh_tray run
            // there via g_apply_openai_settings.
            return;
        } else if (engine_idx == 3) {
            // REQ-046 P4-2 (Rev2 §B-3): user_gguf routes through the SAME
            // shared-host LocalLlama path, with the host relaying
            // config.user_model_id to the worker at ITS next boot (C1 gates
            // the cache on engine_type=="user_gguf"). Hy-MT2 "local" stays
            // untouched: picking index 1 sets engine_type="local", which the
            // host's C1 gate maps back to the pinned model.
            // user_model_id is read directly here (not via GetSnapshot):
            // this callback runs on the GUI thread, same as the config.openai
            // direct read below. Since 261123_0001 the field ALSO has a
            // Snapshot entry (snapshot.user_model_id), which the WORKER-thread
            // LocalModelMissing routing consumes (config.hpp contract).
            if (config.user_model_id.empty()) {
                // REQ-050 Q2 (real-device finding, one-shot entry): an EMPTY
                // binding is not "no model" — a REGISTERED origin=="user"
                // entry may still exist (config reset after registration, or
                // a binding cleared by the manager). Bind to the FIRST such
                // entry — the same fallback refresh_tray resolves the stem
                // through — so the checkable entry is selectable; only with
                // NO registered user model at all does the INV-B3 manager
                // open below run. The bind mirrors the RegisterUserGgufModel
                // reuse branch (REQ-046 P4-2): config id + type + runtime
                // routing + persist.
                std::string fallback_id;
                auto reg = emebalachat::engine_host_registry::LoadDefaultRegistry();
                if (reg.status == emebalachat::engine_host_registry::LoadStatus::Ok) {
                    for (const auto& m : reg.registry.models) {
                        if (m.origin == "user" && !m.files.empty()) {
                            fallback_id = m.id;
                            break;
                        }
                    }
                }
                if (!fallback_id.empty()) {
                    config.SetUserModelId(fallback_id);
                    config.SetEngineTypeName("user_gguf");
                    engine.SetEngineType(emebalachat::EngineType::LocalLlama);
                    // REQ-051 U-2: display stem from the SAME first-user entry
                    // the bind resolves (fallback_id) — one registry read.
                    engine.SetUserModelDisplayStem(emebalachat::ResolveUserModelStem(fallback_id));
                    config.SaveToFile();
                    refresh_tray();
                    DIAG_F("MAIN/on_select_engine/010: user_gguf bound to the first registered model (id=%s)\n",
                           fallback_id.c_str());
                    return;
                }
                // INV-B3 (Rev2 §B-3 선택안 "파일찾기 유도"), REQ-050 revision:
                // nothing is registered yet, so open the MERGED model
                // manager (add-from-file / add-from-Hugging-Face / rename /
                // delete) instead of the bare file picker. The open is
                // DEFERRED past the TrackPopupMenuEx teardown via the same
                // pure (0,0) wake-up as the manager row (REQ-048 R2-D) —
                // entering a modal synchronously here is the REQ-047 D3
                // "minimized window, no input" root cause. Cancel keeps the
                // previous engine (engine_type is never touched on this
                // path; the check mark only moves when a registration in
                // the manager actually switches it).
                DIAG_F("MAIN/on_select_engine/005: user_gguf selected but no model registered; opening model manager\n");
                if (::PostMessageW(emebalachat::g_hControllerWnd,
                                   emebalachat::kMsgOpenGgufManager, 0, 0) == FALSE) {
                    DIAG_F("MAIN/on_select_engine/009: PostMessage gguf-manager open failed (GLE %lu)\n",
                           ::GetLastError());
                }
                return;
            }
            engine.SetEngineType(emebalachat::EngineType::LocalLlama);
            // REQ-051 U-2: the user-model route displays the resolved model
            // stem (registry files[0], raw-id fallback) from here on.
            engine.SetUserModelDisplayStem(emebalachat::ResolveUserModelStem(config.user_model_id));
            config.SetEngineTypeName("user_gguf");
        } else {
            // REQ-046 P4-2 (C3): defensive guard — an unknown index must
            // never fall through into a persisted engine switch.
            DIAG_F("MAIN/on_select_engine/006: unknown engine_idx=%d ignored\n", engine_idx);
            refresh_tray();
            return;
        }
        config.SaveToFile();
        refresh_tray();
        DIAG_F("MAIN/on_select_engine/002: engine switch persisted; active=%s\n",
               engine.GetActiveEngineName().c_str());

        // REQ-043 (M6 T5): no preload follows the switch. Availability of the
        // newly selected local source (the shared host) is visible in the
        // engine name the 002 line already logs; the first local Translate()
        // spawns the host's worker on demand.

        // REQ-045 P4-8 (item 1, design §B): when the user explicitly picks
        // "Local LLM", surface the "engine unavailable" modal EVERY TIME the
        // local components are missing (no session once_flag — the user's Ask
        // Gate condition 1 wording: "번역 엔진 선택 > 로컬LLM 선택 했을 때에도
        // 역시 이 메시지가 뜨도록"). The pick itself is the explicit user action,
        // so re-exposure is policy, not UX spam. This callback runs on the GUI
        // thread (tray menu commands are dispatched synchronously on the
        // message-loop thread), so the modal helper runs inline — Tech Gate
        // Item 6 confirmed GUI-thread safety.
        if (engine_idx == 1) {
            namespace bs = emebalachat::engine_host_bootstrap;
            const bs::ComponentCheckResult check = bs::CheckComponents();
            if (!check.missing.empty()) {
                // Instant offer (260930_0004 D9, CEO directive): a models/-ONLY
                // miss is posted IMMEDIATELY from the already-computed
                // check.missing — waiting cannot heal it (the binaries are all
                // present) and the dialog itself re-checks ModelFilePresent at
                // show time (ShowLocalEngineUnavailableModal), so the
                // installer-in-flight race is still covered without any grace.
                // This path runs on the GUI thread: RequestEngineUnavailableModal's
                // same-thread fast path shows the consent inline (Tech Gate Item
                // 6 site — the pre-REQ-048-R2 modal ran exactly this way). A
                // binaries-involved miss is mutex-gated INSTANT (see the
                // binaries branch below).
                DIAG_F("MAIN/on_select_engine/004: local engine selected but %zu component(s) missing; converging on the grace re-check\n",
                       check.missing.size());
                if (emebalachat::modeldownloader::MissingIsModelOnly(check.missing)) {
                    // Model-only miss: offer now. The branch condition already
                    // established models/-only from check.missing, so the offer
                    // flag is literal true.
                    emebalachat::RequestEngineUnavailableModal(
                        emebalachat::g_hControllerWnd,
                        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                        emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody),
                        true); // offer_model_download
                } else {
                    // Binaries involved — mutex-gated INSTANT notice (same
                    // contract as the boot site's binaries branch: the
                    // Local\EmebalaSetup family gate covers the whole
                    // store-writing window; observe-only OpenMutexW, advisory
                    // gate, documented caveats). The NOT-held paths post
                    // INLINE on the GUI thread (this site's Tech Gate Item 6
                    // fast path — instant feedback); the held path waits on
                    // the mutex from a DETACHED thread — WaitForSingleObject
                    // INFINITE must never run on the GUI thread.
                    HANDLE setup_gate = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\EmebalaSetup");
                    if (setup_gate == nullptr) {
                        if (::GetLastError() == ERROR_FILE_NOT_FOUND) {
                            // No setup/uninstall holds the store — nothing can
                            // heal the state → inform immediately, from the
                            // already-computed check.missing.
                            const bool offer_model =
                                emebalachat::modeldownloader::MissingIsModelOnly(check.missing);
                            emebalachat::RequestEngineUnavailableModal(
                                emebalachat::g_hControllerWnd,
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody),
                                offer_model);
                        } else {
                            // No gate knowledge (acquisition failed for any
                            // other reason) → fail OPEN to informing the user:
                            // one immediate fresh check + immediate post;
                            // never invent a wait.
                            const bs::ComponentCheckResult fresh = bs::CheckComponents();
                            emebalachat::RequestEngineUnavailableModal(
                                emebalachat::g_hControllerWnd,
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody),
                                emebalachat::modeldownloader::MissingIsModelOnly(fresh.missing));
                        }
                    } else {
                        // Setup/uninstall is writing the store: the user sees
                        // the notice THE INSTANT setup exits — wait on the
                        // handle (signaled at installer process exit), close
                        // it, then exactly ONE fresh check decides.
                        std::thread([setup_gate]() {
                            ::WaitForSingleObject(setup_gate, INFINITE);
                            ::CloseHandle(setup_gate);
                            const bs::ComponentCheckResult fresh = bs::CheckComponents();
                            if (fresh.missing.empty()) {
                                DIAG_LOG("ENGINEHOST", "bootstrap/017: components arrived within grace window; suppressing repair-unavailable modal");
                                return; // setup finished the store; no notice warranted
                            }
                            emebalachat::RequestEngineUnavailableModal(
                                emebalachat::g_hControllerWnd,
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedBody),
                                emebalachat::modeldownloader::MissingIsModelOnly(fresh.missing));
                        }).detach();
                    }
                }
            }
        }
    };

    // REQ-060 (session 261009_0001): the engine submenu's "번역 모델 로드 설정…"
    // row. The modal open is DEFERRED past the TrackPopupMenuEx teardown via
    // kMsgOpenEngineLoadSettings — entering a modal synchronously from the
    // tray dispatch is the REQ-047 D3 "minimized window, no input" root cause
    // (the same contract as the manager row, kMsgOpenGgufManager). The save
    // body lives in g_open_engine_host_settings (set above), so this callback
    // only posts the pure (0,0) wake-up; a post failure logs shape-only.
    trayCallbacks.on_open_engine_load_settings = [&]() {
        if (::PostMessageW(emebalachat::g_hControllerWnd,
                           emebalachat::kMsgOpenEngineLoadSettings, 0, 0) == FALSE) {
            DIAG_F("MAIN/EngineLoadSettings/000: PostMessage load-settings open failed (GLE %lu)\n",
                   ::GetLastError());
        }
    };

    // R6 Phase 1 (B3): tray source/target submenu picks are REQUESTS to the
    // coordinator (plan §2.4). The previous hand-maintained config-write +
    // badge + tray sequences are deleted - the coordinator is now the only
    // writer (INV-2), and it additionally syncs a visible tooltip.
    trayCallbacks.on_select_source_lang = [&](std::string_view code) {
        // Phase 3 Batch 2 (plan §2.4): tray source/target submenus drive the
        // TYPE pair (global settings entry point of the main pipeline).
        ApplyLanguageChange(emebalachat::LanguageContext::Type, code,
                            std::string_view{}, false, false);
    };

    trayCallbacks.on_select_target_lang = [&](std::string_view name) {
        ApplyLanguageChange(emebalachat::LanguageContext::Type, std::string_view{},
                            name, false, false);
    };

    // REQ-025 (Phase A §2.1.A3-25): the tray "번역툴팁" submenu picks drive the
    // DRAG pair through the same coordinator (INV-2: single writer). Same
    // request contract as the type callbacks above.
    trayCallbacks.on_select_drag_source_lang = [&](std::string_view code) {
        ApplyLanguageChange(emebalachat::LanguageContext::Drag, code,
                            std::string_view{}, false, false);
    };

    trayCallbacks.on_select_drag_target_lang = [&](std::string_view name) {
        ApplyLanguageChange(emebalachat::LanguageContext::Drag, std::string_view{},
                            name, false, false);
    };

    trayCallbacks.on_swap_languages = [&]() {
        // R6 Phase 1 (B3): the swap POLICY (AUTO source becomes the concrete
        // old target, new target falls back to the OS-native language, plain
        // pairs swap directly) is unchanged; only the APPLY step moved into
        // the coordinator, which persists + refreshes badge/tray/tooltip in
        // one place instead of this hand-maintained sequence.
        // Phase 3 Batch 2 (plan §2.4): tray Swap / badge double-click swap the
        // TYPE pair, so the swap INPUT read is the type pair as well.
        // W2 (R1-22): the policy now lives in the pure PlanLanguageSwap planner
        // (src/ui/tray.hpp) so the drag-pair Swap row below applies the exact
        // same rule - one definition, both pairs.
        const auto snap_in = config.GetSnapshot(); // I4: consistent read for the swap logic
        const emebalachat::LanguageSwapPlan swap = emebalachat::PlanLanguageSwap(
            snap_in.type_source_language, snap_in.type_target_language,
            emebalachat::I18n::GetSystemLanguageCode());

        // Coordinator is the ONLY writer from here (INV-2): canonicalizes,
        // persists synchronously (INV-3), refreshes badge -> tray -> tooltip.
        // The chime plays only on an actual change (same semantics as before,
        // which always played; a swap can never be a no-op except K<->K).
        ApplyLanguageChange(emebalachat::LanguageContext::Type, swap.source,
                            swap.target, false, true);
    };

    // W2 (R1-22, session 261029_0001): the 번역툴팁 group's own Swap row. Same
    // planner, same chime, applied to the DRAG pair - the drag users previously
    // had to walk two deep submenus to reverse a pair.
    // Pin semantics (main.cpp ApplyLanguageChange + F2): a swap carries a
    // concrete target, so it is an EXPLICIT target request and pins
    // drag_target_pinned exactly like picking the target from the submenu -
    // the pair the user just chose must not be re-pivoted later.
    trayCallbacks.on_swap_drag_languages = [&]() {
        const auto snap_in = config.GetSnapshot(); // I4: consistent read
        const emebalachat::LanguageSwapPlan swap = emebalachat::PlanLanguageSwap(
            snap_in.drag_source_language, snap_in.drag_target_language,
            emebalachat::I18n::GetSystemLanguageCode());
        // fix-list-round15 R15-21 (F-3b): the swap echo names both the
        // persisted pair and the planned swap — language tokens that can be
        // user-typed stored names (the R14-06 class) — so they log only
        // behind diag::ContentLoggingEnabled(); the else keeps the
        // shape-only lengths.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE", "drag_swap_apply %s/%s -> %s/%s",
                     snap_in.drag_source_language.c_str(),
                     snap_in.drag_target_language.c_str(),
                     swap.source.c_str(), swap.target.c_str());
        } else {
            DIAG_LOG("STATE", "drag_swap_apply len=%zu/%zu -> %zu/%zu",
                     snap_in.drag_source_language.size(),
                     snap_in.drag_target_language.size(),
                     swap.source.size(), swap.target.size());
        }
        ApplyLanguageChange(emebalachat::LanguageContext::Drag, swap.source,
                            swap.target, false, true);
    };

    trayCallbacks.on_toggle_auto_send = [&]() {
        hook.ToggleAutoSend();
        // V10F P3-C2 (UX-3 wave B): the badge's auto-send chip follows the
        // flip. ToggleAutoSend stores the new value SYNCHRONOUSLY before it
        // returns (the fx worker only persists + refreshes the tray), so the
        // relaxed atomic read here already sees the post-flip state. The
        // Ctrl+Shift+Enter hotkey path flips inside hook.cpp with no main.cpp
        // seam; its chip catches up on the next refresh_tray() self-heal push
        // (documented in the unit report).
        badge.SetAutoSendIndicator(config.auto_send.load(std::memory_order_relaxed));
    };

    trayCallbacks.on_toggle_sound = [&]() {
        // I4: sound_enabled is std::atomic (read by hook thread for beep gating).
        const bool next = !config.sound_enabled.load(std::memory_order_relaxed);
        config.sound_enabled.store(next, std::memory_order_relaxed);
        config.SaveToFile();
        emebalachat::SetSoundEnabled(next);
        refresh_tray();
    };

    trayCallbacks.on_toggle_badge = [&]() {
        badge.SetVisible(!badge.IsVisible());
        // REQ-R12 (audit §4 I4, main.cpp:344): the tray callback runs on the
        // GUI thread while the hook thread's Ctrl+F9 cycle writes
        // target_language under mutex_ (hook.cpp CycleLanguage). Reading
        // config.source_language/target_language directly here was the
        // remaining data race. GetSnapshot() is the thread-safe accessor the
        // other tray callbacks already use.
        // SC-02: the refresh itself now lives in refresh_tray() (same locked snapshot inside).
        refresh_tray();
    };

    trayCallbacks.on_toggle_start_with_windows = [&]() {
        // State toggled inside tray menu
    };

    // 261108_0002 (WS3, v0.10.6): the "역번역 플로팅 표시 (번역 검증)" check row.
    // Same idiom as on_toggle_sound above (GUI-thread callback, I4 relaxed
    // atomic flip, synchronous persist, single-writer tray refresh). Turning
    // the feature OFF also retires a card that is already on screen: "off"
    // must never leave stale verification content floating, and a round trip
    // still in flight is discarded by the consumer's config re-check.
    trayCallbacks.on_toggle_back_translate = [&]() {
        const bool next = !config.back_translation_enabled.load(std::memory_order_relaxed);
        config.back_translation_enabled.store(next, std::memory_order_relaxed);
        config.SaveToFile();
        refresh_tray();
        if (!next) {
            tooltip.DismissThreadSafe();
        }
        DIAG_LOG("STATE", "back_translation_enabled=%d", next ? 1 : 0);
    };

    // REQ-005: tray menu "About Emebala Chat…" (Batch 1's guarded hook, now
    // wired). Tray callbacks run on the GUI thread; AboutWindow::Show centers
    // on the monitor under the current cursor position.
    trayCallbacks.on_show_about = [&]() {
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        about_window.Show(cursor.x, cursor.y);
    };

    // Phase 4 (REQ-020, plan §1.3): wire the About-window reset button to the
    // apply_system_defaults coordinator defined above. AboutWindow stays a
    // pure view - this callback is its only config-side seam. Safe even when
    // Create failed: the callback is stored regardless of hwnd_ state.
    about_window.SetResetCallback(apply_system_defaults);

    // Shared UI Theme change coordinator wired to both SystemTray and AboutWindow.
    auto apply_theme_change = [&](const std::string& theme_key) {
        const std::string canonical = emebalachat::AppConfig::NormalizeUiTheme(theme_key);
        config.SetUiTheme(canonical);
        emebalachat::ui::SetActiveTheme(canonical);
        config.SaveToFile();

        const auto snap = config.GetSnapshot();
        badge.SetLanguages(emebalachat::ToUtf16(snap.type_source_language),
                           emebalachat::ToUtf16(snap.type_target_language));
        badge.SetStatus(hook.IsActive() ? emebalachat::BadgeStatus::Active
                                        : emebalachat::BadgeStatus::Disabled);
        if (badge.GetHwnd()) {
            ::InvalidateRect(badge.GetHwnd(), nullptr, FALSE);
        }
        tooltip.RefreshTheme();
        about_window.RefreshTheme();
        // R1-I (v0.10.7, 261111_0001 fix-list round 1): a same-thread GUI
        // REPAINT of the already-shown icon at its CURRENT position after a
        // theme/locale change, deliberately NOT routed through the RequestShowAt
        // generation seam. The seam orders cross-thread show/HIDE requests (a
        // newer drag supersedes an older one; a Hide invalidates a pending
        // show); a repaint is neither - it refreshes the applied show and must
        // not stamp a new generation that could drop a newer in-flight drag.
        // Single-threaded GUI ordering makes the inline call safe: every inline
        // ShowAt runs on the GUI thread, serialized with the WndProc that drains
        // kShowAtMessage/kHideMessage, so a pending newer-drag show still applies
        // AFTER this repaint (its stamp is untouched) and wins. The IsVisible()
        // guard is only a cheap pre-filter, NOT a synchronisation with a hide:
        // an off-thread Hide() posts kHideMessage and the GUI-side teardown
        // clears the visible flag when that message is drained (drag_icon.cpp
        // HideWindow), so a concurrent hide can still read as visible here.
        // That is benign - this repaint re-applies the CURRENT position of an
        // icon the GUI thread still shows, and the queued hide then hides it;
        // a newer show stamped meanwhile is unaffected. (G-16 round 2: the
        // earlier "Hide() clears visible_ synchronously" wording was true only
        // for the inline GUI-thread hide, never for the hook-thread one.)
        if (drag_icon.IsVisible() && drag_icon.GetHwnd()) {
            RECT rcDrag = {};
            if (::GetWindowRect(drag_icon.GetHwnd(), &rcDrag)) {
                drag_icon.ShowAt(rcDrag.left, rcDrag.top);
            }
        }
        tray.SetUiTheme(canonical);
        tray.RefreshTheme();
        refresh_tray();
    };
    trayCallbacks.on_select_theme = apply_theme_change;
    tray.SetOnThemeSelectCallback(apply_theme_change);
    about_window.SetOnThemeChangeCallback(apply_theme_change);

    // R6 Phase 6 (plan §5.4): tray UI-language selector. Runs on the GUI
    // thread (tray callback). Validation + canonicalization + refusal of
    // unknown/removed codes (fr/de/ru) live in the pure PlanUiLocaleChange
    // seam (i18n.hpp), pinned by TestR6P5P6I18n. Persistence follows the I4
    // pattern: locked SetUiLanguage + synchronous SaveToFile (same contract
    // as the language coordinator's INV-3). A no-op re-pick skips the write
    // but still refreshes the views (self-heal). "auto" resolves to the
    // DETECTED system locale at apply time; the persisted value stays "auto"
    // so a later OS-language change is honored on next start.
    trayCallbacks.on_select_ui_language = [&](std::string_view code) {
        const auto snap = config.GetSnapshot(); // I4: consistent read
        const emebalachat::UiLocaleChangePlan plan =
            emebalachat::PlanUiLocaleChange(snap.ui_language, code);
        if (!plan.valid) {
            DIAG_F("MAIN/UiLocale/001: refused unknown UI-language code '%s'\n",
                    std::string(code).c_str());
            return;
        }
        if (plan.changed) {
            config.SetUiLanguage(plan.persisted_value); // I4: locked write
            config.SaveToFile();
        }
        const emebalachat::UiLocale applied =
            (plan.applied == emebalachat::UiLocale::Auto)
                ? emebalachat::I18n::DetectSystemLocale()
                : plan.applied;
        emebalachat::I18n::SetLocale(applied);
        RefreshAllUiForLocaleChange();
    };
    // 261119_0001 P6-U5: publish the SAME UI-language apply body to the
    // controller arm (the shared picker's TrayUiLanguage commit is dispatched
    // through this seam on the GUI thread). The std::function copy shares the
    // lambda above - one planner (PlanUiLocaleChange), one persist, one
    // refresh - so the picker path and the inline Auto row can never drift.
    // Cleared at shutdown with g_apply_language_change so a late picker
    // commit can never call into destroyed state.
    emebalachat::g_apply_ui_language = trayCallbacks.on_select_ui_language;

    // P4 Batch B-5 investigation (design §2.2.3): StringId::CheatSheetBody is
    // rendered by a system MessageBoxW dialog HERE and nowhere else (the only
    // CheatSheet* Get() call sites in the tree are this callback and the tray
    // menu item label). A MessageBox is not custom-drawn, so the body-format
    // direction rule does not apply; the dialog's text reading direction
    // follows the thread locale, and the documented per-call override is
    // MB_RTLREADING (winuser.h, 0x00100000L — verified present in the build
    // SDK 10.0.26100), which applies WS_EX_RTLREADING to the message-box
    // dialog window only. Scope discipline (§2.2.4): the owner is nullptr and
    // the flag is computed at THIS call only — it can never leak into other
    // surfaces; LTR UI locales pass no flag (byte-identical dialog today).
    // V10F P3 wave B (ux_and_longform_architecture U4): the body is REBUILT at
    // show time - the localized CheatSheetBody stays the base with the default
    // combo tokens substituted for the LIVE config.hotkey_* display strings
    // (SubstituteCheatSheetHotkeys above), then the gestures the static body
    // never mentioned are appended (selection-translate hotkey, Double Ctrl+C,
    // ESC cancel, badge-click cancel). Shared with the settings hub's
    // [단축키 안내…] action through ONE lambda so the two surfaces cannot drift.
    auto show_cheat_sheet = [&config]() {
        UINT type = MB_OK | MB_ICONINFORMATION;
        if (emebalachat::DirectionForLocale(emebalachat::I18n::GetCurrentLocale()) ==
            emebalachat::TextDirection::RTL) {
            type |= MB_RTLREADING;
        }
        const std::wstring live_toggle =
            emebalachat::KeyboardHook::HotkeySpecToDisplayString(
                emebalachat::KeyboardHook::ResolveToggleFromConfig(config.hotkey_toggle));
        const std::wstring live_lang =
            emebalachat::KeyboardHook::HotkeySpecToDisplayString(
                emebalachat::KeyboardHook::ResolveLangFromConfig(config.hotkey_lang));
        const std::wstring live_mode =
            emebalachat::KeyboardHook::HotkeySpecToDisplayString(
                emebalachat::KeyboardHook::ResolveModeFromConfig(config.hotkey_mode));
        const std::wstring live_selection =
            emebalachat::KeyboardHook::HotkeySpecToDisplayString(
                emebalachat::KeyboardHook::ResolveSelectionFromConfig(
                    config.hotkey_selection_translate));
        std::wstring body =
            emebalachat::I18n::Get(emebalachat::StringId::CheatSheetBody);
        emebalachat::SubstituteCheatSheetHotkeys(body, live_toggle, live_lang, live_mode);
        // The appended gestures. Where an existing StringId carries the
        // meaning it is reused (the badge-cancel line is TooltipCancelHint
        // verbatim - a complete localized sentence describing exactly this
        // gesture); the 261122_0002 Phase-3 i18n micro-pass landed the four
        // appendix ids (CheatSheetMoreGestures / -SelectionTranslate /
        // -DoubleCtrlC / -EscCancel) in all 89 tables, closing the P3-C5
        // I18N REQUEST.
        body += L"\n\n";
        body += emebalachat::I18n::Get(
            emebalachat::StringId::CheatSheetMoreGestures);
        body += L"\n";
        body += L"  • " + live_selection + L" : " +
                emebalachat::I18n::Get(
                    emebalachat::StringId::CheatSheetSelectionTranslate) +
                L"\n";
        body += L"  • Ctrl+C ×2 : " +
                emebalachat::I18n::Get(
                    emebalachat::StringId::CheatSheetDoubleCtrlC) +
                L"\n";
        body += L"  • ESC : " +
                emebalachat::I18n::Get(
                    emebalachat::StringId::CheatSheetEscCancel) +
                L"\n";
        body += L"  • " +
                emebalachat::I18n::Get(emebalachat::StringId::TooltipCancelHint);
        // REQ-206 (session 260911_0002 T4): the Cheat Sheet must tell users
        // WHERE config.json lives, as a relative/env-var path. The line is
        // appended after a '\n' separator so the MessageBox keeps the body
        // and the path on separate lines; the string stays LITERAL
        // (%LOCALAPPDATA% token never expanded via ExpandEnvironmentStrings),
        // per the user's "not an absolute path" requirement.
        body += L"\n" +
                emebalachat::I18n::Get(emebalachat::StringId::CheatSheetConfigPath);
        ::MessageBoxW(
            nullptr,
            body.c_str(),
            emebalachat::I18n::Get(emebalachat::StringId::CheatSheetTitle).c_str(),
            type
        );
    };
    trayCallbacks.on_show_cheat_sheet = show_cheat_sheet;

    trayCallbacks.on_exit = [&]() {
        ::PostQuitMessage(0);
    };

    tray.Create(hController, hInstance, trayCallbacks);
    // V10F P3-C1 (UX-1 Option B, wave B): the settings container's first row
    // ("일반…") opens the settings hub. Precedence: this slot wins over
    // Callbacks::on_open_settings_hub (SetOpenSettingsHubCallback's contract);
    // the hub is opened modeless with the controller as owner (sane z-order,
    // and the owned window tears down with the controller BEFORE wWinMain's
    // locals die) and the LIVE badge visibility flag (HubRuntimeState's truth
    // contract: the General tab badge toggle diffs against the value the
    // caller passes). GUI thread: the row dispatches on this thread.
    tray.SetOpenSettingsHubCallback([&badge, &config]() {
        emebalachat::HubRuntimeState hub_runtime;
        hub_runtime.badge_visible = badge.IsVisible();
        emebalachat::OpenSettingsHub(config, hub_runtime, emebalachat::g_hControllerWnd);
    });
    // R6 Phase 6: mirror the persisted ui_language into the tray's selector
    // check state (snapshot read; the warmup thread already exists here).
    tray.SetUiLanguage(config.GetSnapshot().ui_language);
    tray.SetUiTheme(config.GetSnapshot().ui_theme);
    // 261108_0002 (WS3): mirror the persisted floating-back-translation switch
    // into its check row at startup (same contract as SetUiLanguage above);
    // every later refresh_tray() keeps it in sync from then on.
    tray.SetBackTranslationEnabled(config.GetSnapshot().back_translation_enabled);
    // REQ-056 (derive-at-open): the engine submenu's config-derived values —
    // the 4-way check index (REQ-054's TrayPreferredEngineIndex), the raw
    // auto token (261116_0003 P6a r2, PR6-07) and the registered user-model
    // stem — are resolved HERE, on demand, when the tray menu is built.
    // main.cpp is the single authority (config + registry resolution); the
    // tray keeps no cached copies, so no push path can go stale. Runs ON THE
    // GUI THREAD: ShowContextMenu is reached only from the tray icon's own
    // message and the badge right-click, both marshaled to the GUI thread.
    // The stem block below is the exact logic refresh_tray used to push on
    // every refresh (REQ-047 U1 + REQ-050 3-1/3-2, with the REQ-050 Q2
    // first-origin=="user" fallback).
    tray.SetEngineMenuResolver([&]() {
        emebalachat::SystemTray::EngineMenuView view;
        const auto snap = config.GetSnapshot();
        // REQ-054: the mapping (local->1, openai->2, user_gguf->3,
        // "google"/"auto"/unknown->0; the index never affects routing).
        view.preferred_engine = emebalachat::TrayPreferredEngineIndex(snap.engine_type);
        // 261116_0003 P6a fix round r2 (PR6-07): the collapsed index maps BOTH
        // "google" and "auto" onto 0, so it cannot tell the tray whether the
        // auto router may serve a HyMt2-only pin. Carry the RAW token: only an
        // exactly-"auto" engine_type lets PlanTranslationRouting reach the
        // local leg (Translate() consults the plan only under active ==
        // LocalLlama, engine.cpp:685-691; a persisted "google" latches Google
        // at engine.cpp:313-315), so explicit google keeps the
        // truthful single-view ghost.
        view.is_auto = (snap.engine_type == "auto");
        // 261116_0003 P6a fix round r4 (PR6-19): the ghost gate's host-
        // availability conjunct. HostCouldServeFor (engine.cpp:288-290) =
        // engine_host.enabled && engine_host::IsHostBinaryPresent() is the
        // availability half of the router's local-serve predicate, whose
        // binary half is evaluated here as a LIVE probe while the router's
        // local leg is the LATCHED active_type_ (tray.hpp's rationale scopes
        // that one divergence direction). Under auto with the host leg dead
        // (Zero-Install) the router is never consulted, so the tray must not
        // suppress the ghost either. The block is a direct AppConfig member
        // (config.hpp:527) with NO Snapshot entry; it is read here on the GUI
        // thread — the same thread that writes it (the settings dialog's
        // SetEngineHostConfig push above, at line ~2308; the startup push at
        // ~1475 is the same pattern), so no lock is needed.
        view.local_host_available =
            config.engine_host.enabled && emebalachat::engine_host::IsHostBinaryPresent();
        // Bound id wins; an EMPTY binding falls back to the FIRST
        // origin=="user" registry entry so a registered model never
        // disappears from the menu; a registry miss shows the raw config id
        // so the label never lies. user_model_id is read directly (not via
        // the snapshot): this resolver always runs on the GUI thread (worker
        // threads read the field through snapshot.user_model_id instead,
        // added for the 261123_0001 LocalModelMissing routing).
        std::string user_model_stem;
        {
            auto reg = emebalachat::engine_host_registry::LoadDefaultRegistry();
            if (reg.status == emebalachat::engine_host_registry::LoadStatus::Ok) {
                if (!config.user_model_id.empty()) {
                    if (const auto* m = reg.registry.FindModel(config.user_model_id)) {
                        if (!m->files.empty()) {
                            user_model_stem = emebalachat::GgufFileStem(m->files[0]);
                        }
                    }
                    if (user_model_stem.empty()) {
                        user_model_stem = config.user_model_id;  // fallback: show the id
                    }
                } else {
                    for (const auto& m : reg.registry.models) {
                        if (m.origin == "user" && !m.files.empty()) {
                            user_model_stem = emebalachat::GgufFileStem(m.files[0]);
                            break;
                        }
                    }
                }
            }
        }
        view.user_model_stem = std::move(user_model_stem);
        // 261116_0003 P9b-2: the "내 언어" group's data, copied at menu-open
        // time (derive-at-open, the same contract as every field above). The
        // member is GUI-thread-only per config.hpp and this resolver runs on the
        // GUI thread, which is also its only writer. Empty = no registered user
        // languages, and the four menus render exactly the registry rows.
        view.user_languages = config.user_languages;
        // 261116_0003 P9b-3 U4a: the badge key + the in-flight seam, derived at
        // menu-open time from the SAME snapshot (derive-at-open). The engine
        // half is UserLanguageEngineId(resolved engine) — "" names the no-probe
        // context and renders NO badge; the model half is the OpenAiConfig
        // model / the user-.gguf registry id (UserLanguageProbeModelFor — the
        // U3 runner writes its verdict slots under the SAME key, so a menu row
        // and its verdict can never disagree about the address). The in-flight
        // seam answers per stored name from U3's GUI-thread-only in-flight
        // set (the runner outlives every menu build; Retire only gates new
        // triggers, and the menu can open only while the GUI thread pumps).
        const emebalachat::LanguageEngine badge_engine =
            emebalachat::TrayLanguageEngineFor(view.preferred_engine);
        view.user_lang_probe_engine = emebalachat::UserLanguageEngineId(badge_engine);
        view.user_lang_probe_model =
            emebalachat::UserLanguageProbeModelFor(badge_engine, config);
        view.user_lang_probe_in_flight =
            [&probe_service, engine_id = view.user_lang_probe_engine,
             model = view.user_lang_probe_model](const emebalachat::UserLanguage& language) {
                return probe_service.IsInFlight(emebalachat::UserLanguageStoredName(language),
                                                engine_id, model);
            };
        return view;
    });
    // 261116_0003 P6c (PR6-35): wire the tooltip's widened engine resolver —
    // the seam SetEngineResolver has carried since P6b with zero call sites.
    // Same derive-at-open contract as the tray's resolver above: the dropdown
    // builders call this on demand when a menu opens, and BOTH resolvers
    // resolve the SAME three values from the SAME sources, so the two surfaces
    // feed ONE shared decision (SuppressPinnedAbsentGhost) from identically
    // derived inputs — the P6b-fix shared-decision/unshared-inputs split, with
    // both resolver halves wired since P6c (this site is the tooltip half).
    //   engine                = TrayLanguageEngineFor(TrayPreferredEngineIndex(
    //                           snap.engine_type)) — the tray's OWN mapper,
    //                           reused, never duplicated in the tooltip layer
    //                           (P6b-3's no-duplication pin);
    //   is_auto               = the RAW persisted "auto" token (PR6-07);
    //   local_host_available  = config.engine_host.enabled &&
    //                           engine_host::IsHostBinaryPresent() — the
    //                           router's HostCouldServeFor components (PR6-19).
    // Runs ON THE GUI THREAD, the same reasoning as the tray's resolver: the
    // tooltip calls it from its own mouse-handler menu build on the GUI thread,
    // which is also the thread that writes engine_host (the settings dialog's
    // SetEngineHostConfig push) — so the direct member read needs no lock.
    tooltip.SetEngineResolver([&]() {
        emebalachat::TooltipWindow::MenuEngineResolution resolution;
        const auto snap = config.GetSnapshot();
        resolution.engine = emebalachat::TrayLanguageEngineFor(
            emebalachat::TrayPreferredEngineIndex(snap.engine_type));
        resolution.is_auto = (snap.engine_type == "auto");
        resolution.local_host_available =
            config.engine_host.enabled && emebalachat::engine_host::IsHostBinaryPresent();
        // 261116_0003 P9b-2: the two dropdowns' "내 언어" group data, copied at
        // menu-open time from the same member the tray's resolver copies (the
        // shared-DECISION / unshared-INPUTS contract stays intact: both surfaces
        // still derive their own view, from the same persisted source).
        resolution.user_languages = config.user_languages;
        // 261116_0003 P9b-3 U4a: the badge key + the in-flight seam — the SAME
        // derivation as the tray's resolver above (shared decision, unshared
        // inputs), so the tooltip's two dropdowns and the tray's four submenus
        // evaluate ONE badge per (engine, model, language). Both stay empty on
        // a tooltip whose resolver does not populate them (no suffix; the
        // conservative default).
        resolution.user_lang_probe_engine =
            emebalachat::UserLanguageEngineId(resolution.engine);
        resolution.user_lang_probe_model =
            emebalachat::UserLanguageProbeModelFor(resolution.engine, config);
        resolution.user_lang_probe_in_flight =
            [&probe_service, engine_id = resolution.user_lang_probe_engine,
             model = resolution.user_lang_probe_model](const emebalachat::UserLanguage& language) {
                return probe_service.IsInFlight(emebalachat::UserLanguageStoredName(language),
                                                engine_id, model);
            };
        return resolution;
    });
    // R6 Phase 1 (B3, plan §2.4 "config reload at startup"): initial surface
    // alignment runs through the SAME coordinator as every runtime mutation
    // (empty request = refresh-only: valid, unchanged, no persist). This is
    // why the badge is re-pushed here even though Create() already received
    // the loaded values: one code path, so a startup-only special case can
    // never drift from the runtime one again.
    // Phase 3 Batch 2 (plan §3-Batch2): align BOTH contexts once - Type first
    // (badge/tray ordering), then Drag (the tooltip is hidden at startup so
    // its refresh is a no-op, but the call keeps the code path uniform).
    ApplyLanguageChange(emebalachat::LanguageContext::Type, std::string_view{},
                        std::string_view{}, false, false);
    ApplyLanguageChange(emebalachat::LanguageContext::Drag, std::string_view{},
                        std::string_view{}, false, false);

    // 261111_0001 U1 (v0.10.7, audit D1a/D1b): WHERE the current gesture
    // started, captured by the mouse-down callback below and read by the
    // drag-release callback. Needed because SetDragReleaseCallback does NOT
    // fire only for genuine system-drag-threshold drags: the multi-click settle
    // path fires it too (mouse_hook.cpp ArmDelayedClick -> delayed worker
    // drag_cb_), so a plain double-click must be classified by its START
    // point, not by movement.
    std::atomic<int> drag_start_x{0};
    std::atomic<int> drag_start_y{0};
    // 261111_0001 U1 (v0.10.7, R1-E): whether the CURRENT gesture's mouse-down
    // landed on one of OUR OWN popup menus (Win32 class "#32768", same process -
    // the tooltip language menu, the tray submenus, any in-process
    // TrackPopupMenu). Captured at PRESS time by the mouse-down callback and
    // consulted UNCONDITIONALLY by the drag-release callback, so a no-card
    // multi-click over a menu (the release callback also fires on the
    // multi-click settle path) never summons the drag icon. A multi-click press
    // and its settled release run on DIFFERENT threads (mouse-hook vs delayed
    // worker), so the flag is atomic; relaxed ordering suffices (standalone
    // bool, no data depends on the ordering).
    std::atomic<bool> drag_press_own_popup{false};
    // V10F D1: whether the CURRENT gesture's mouse-down landed on our own
    // DragIconWindow rect. Mirrors the own-popup predicate above: a press on
    // the icon is an icon interaction (its click callback owns the translate
    // launch), so the drag-release callback must never re-summon/move the icon
    // for a gesture that started on it. Same atomic/relaxed discipline.
    std::atomic<bool> drag_press_own_drag_icon{false};
    // V10F FUP-02: whether the CURRENT gesture's mouse-down landed on our own
    // language-search palette (the modal "검색…" popup or its EDIT/LISTBOX
    // children). Mirrors the own-popup predicate: picker-internal gestures
    // (query edit, row drag) must not summon the drag icon over the open
    // picker, and picker clicks must not dismiss the tooltip behind it.
    std::atomic<bool> drag_press_own_palette{false};

    // V10F FUP-02: TRUE when the window at `pt` is our own language picker —
    // the point's window OR any of its ancestors carries the picker class and
    // belongs to this process (the click may land on the picker's EDIT/LISTBOX
    // children, whose own classes are "Edit"/"ListBox"; GetAncestor(GA_ROOT)
    // reaches the popup). The class half is the pure
    // language_palette::IsLanguagePaletteWindowClass predicate; the pid check
    // keeps a foreign same-class window from ever exempting a click.
    auto OwnLanguagePaletteWindowAtPoint = [](POINT pt) -> bool {
        const HWND under = ::WindowFromPoint(pt);
        if (!under) {
            return false;
        }
        HWND probe = ::GetAncestor(under, GA_ROOT);
        if (!probe) {
            probe = under;
        }
        wchar_t cls[64] = {};
        if (::GetClassNameW(probe, cls, 64) <= 0 ||
            !emebalachat::language_palette::IsLanguagePaletteWindowClass(cls)) {
            return false;
        }
        DWORD pid = 0;
        ::GetWindowThreadProcessId(probe, &pid);
        return pid == ::GetCurrentProcessId();
    };

    // Drag release callback. Fires on the mouse-hook thread (a completed drag
    // past the DPI-aware system drag threshold, mouse_hook.cpp
    // DragThresholdPixelsForPress - ISSUE-013 retired the hard-coded 15px) OR on
    // the delayed-click worker thread (a double/triple-click settle). Never on
    // the GUI thread, so every D2D touch below MUST be marshaled.
    mouse_hook.SetDragReleaseCallback([&](int x, int y) {
        // REQ-R12 (I4 sweep): this lambda runs on the mouse-hook thread.
        // drag_to_translate is startup-written/immutable per config.hpp's I4
        // contract, but it is also part of Snapshot, so reading it through
        // GetSnapshot() future-proofs against live config reload and matches
        // the accessor discipline every other cross-thread callback uses.
        // Uncontended mutex_ (~µs) - acceptable on the hook thread (D3 audit).
        const auto snap = config.GetSnapshot();
        if (!snap.drag_to_translate || !hook.IsActive()) {
            return;
        }
        // 261111_0001 U1 (v0.10.7, R1-E): consult the PRESS-time own-popup
        // predicate UNCONDITIONALLY - a gesture whose mouse-down landed on one of
        // our own popup menus is a menu interaction and must never summon the
        // drag icon, EVEN WHEN NO CARD IS VISIBLE. The old exemption lived only
        // inside the tooltip.IsVisible() block below, so a no-card tray-menu
        // multi-click (which still reaches this callback through the delayed-
        // click settle path) summoned the icon over the open menu.
        if (drag_press_own_popup.load(std::memory_order_relaxed)) {
            DIAG_LOG("UI", "drag_release exempted: press started over own popup menu (no card needed)");
            return;
        }
        // V10F D1: consult the PRESS-time own-drag-icon predicate
        // UNCONDITIONALLY (same R1-E discipline as the popup flag): a gesture
        // whose mouse-down landed on the DragIconWindow is an icon interaction,
        // never a drag-translate summon - without this the release re-posted
        // RequestShowAt and teleported the icon off the click site.
        if (drag_press_own_drag_icon.load(std::memory_order_relaxed)) {
            DIAG_LOG("UI", "drag_release exempted: press started on drag icon (no card needed)");
            return;
        }
        // V10F FUP-02: consult the PRESS-time own-palette predicate first (same
        // R1-E unconditional discipline as the popup flag): a gesture whose
        // mouse-down landed on the open picker is a picker interaction (query
        // edit / row drag) even when the release escapes the window.
        if (drag_press_own_palette.load(std::memory_order_relaxed)) {
            DIAG_LOG("UI", "drag_release exempted: press started over own language picker");
            return;
        }
        // V10F FUP-02 second layer (press-time flag is the primary): a release
        // landing on our open language picker while the press started elsewhere
        // (drag across the modal popup) is picker-owned - never summon the
        // drag icon over it.
        {
            const POINT release_pt = { x, y };
            if (OwnLanguagePaletteWindowAtPoint(release_pt)) {
                DIAG_LOG("UI", "drag_release exempted: release landed over own language picker");
                return;
            }
        }
        // 261111_0001 U1 (v0.10.7, audit D1a/D1b/D4, ISSUE-009): a visible
        // translation card must NEVER suppress the gesture globally (the old
        // `tooltip.IsVisible()` early return was the dead zone: every drag
        // release anywhere was dropped while a card was parked, and translation
        // cards never auto-hide). Tooltip visibility is now only a LOCAL
        // qualifier:
        //   - a gesture that STARTED inside the card's rect belongs to the card
        //     (the window is hit-test opaque - no WS_EX_TRANSPARENT, tooltip.cpp
        //     L298 - so its press was consumed by a button/thumb, never by the
        //     underlying app): never summon a drag icon, never dismiss. This is
        //     what keeps tooltip button double-clicks icon-free.
        //   - any OTHER gesture dismisses the card and CONTINUES to the icon.
        //     Dismiss() clears visible_ before returning on this thread (D1a)
        //     and marshals its GUI teardown internally, so it is safe here.
        if (tooltip.IsVisible()) {
            // REQ-026 class, SECOND LAYER (the primary own-popup gate is the
            // press-time flag checked unconditionally above): a gesture whose
            // RELEASE lands on our own popup menu (e.g. pressed on the app, then
            // dragged onto the open menu) is a menu interaction - never dismiss
            // the menu's owner mid-tracking (the posted dismiss would be
            // dispatched inside TrackPopupMenuEx and kill the pick).
            const POINT release_pt = { x, y };
            HWND under = ::WindowFromPoint(release_pt);
            if (under) {
                wchar_t menu_class[16] = {};
                if (::GetClassNameW(under, menu_class, 16) > 0 &&
                    ::lstrcmpW(menu_class, L"#32768") == 0) {
                    DWORD pid = 0;
                    ::GetWindowThreadProcessId(under, &pid);
                    if (pid == ::GetCurrentProcessId()) {
                        DIAG_LOG("UI", "drag_release exempted over own popup menu");
                        return;
                    }
                }
            }
            RECT r = {};
            ::GetWindowRect(tooltip.GetHwnd(), &r);
            const POINT drag_start = { drag_start_x.load(std::memory_order_relaxed),
                                       drag_start_y.load(std::memory_order_relaxed) };
            if (::PtInRect(&r, drag_start)) {
                DIAG_LOG("UI", "drag_release suppressed: gesture started inside tooltip rect");
                return;
            }
            tooltip.Dismiss();
        }
        // V10F D3 (main side): skip the icon summon itself when the foreground
        // window is a console/terminal - there the drag release means a text
        // selection, not a translate request, and the click's synthetic Ctrl+C
        // capture would be a SIGINT (REQ-005). The click-time guard in
        // run_drag_translate (IsConsoleCaptureUnsafe at the drag-icon click) is
        // kept as defense-in-depth; this release-time guard stops the summon
        // one step earlier. Fail-open by construction: any query failure inside
        // IsConsoleCaptureUnsafe (including a null foreground hwnd) returns
        // false and the existing path runs unchanged.
        if (emebalachat::IsConsoleCaptureUnsafe(::GetForegroundWindow())) {
            DIAG_LOG("UI", "drag_release skipped: console/terminal foreground (SIGINT hazard)");
            return;
        }
        // REQ-R10 (audit §3.4): the single-threaded D2D render target belongs
        // to the main GUI thread. Call ShowAt directly here produced
        // D2DERR_WRONG_THREAD and an invisible icon. PostMessage to the icon's
        // own window: non-blocking for the hook thread, WndProc renders on the
        // GUI thread.
        emebalachat::DragIconWindow::RequestShowAt(drag_icon.GetHwnd(), x + 12, y + 12);
    });

    // Outside-click dismissal for DragIconWindow and TooltipWindow
    mouse_hook.SetMouseDownCallback([&](int x, int y) {
        // 261111_0001 U1 (v0.10.7, audit D1a/D1b): record the gesture START
        // point first, for the drag-release scoping above. Every physical
        // press updates it (even menu clicks). R1-E: the press-time own-popup
        // flag is reset here too (every press re-evaluates it); the release
        // callback exempts an own-popup gesture via this flag UNCONDITIONALLY
        // plus its own WindowFromPoint second layer.
        drag_start_x.store(x, std::memory_order_relaxed);
        drag_start_y.store(y, std::memory_order_relaxed);
        drag_press_own_popup.store(false, std::memory_order_relaxed);
        // V10F D1 + FUP-02: every press re-evaluates the own-drag-icon and
        // own-palette predicates too (same R1-E reset contract as the popup
        // flag above).
        drag_press_own_drag_icon.store(false, std::memory_order_relaxed);
        drag_press_own_palette.store(false, std::memory_order_relaxed);
        POINT pt = { x, y };
        // REQ-026 (Phase A §3 B-3, attributed failure mode (a) callback-never-
        // fires): a physical click landing on OUR OWN transient popup menu
        // (Win32 class "#32768" - the tooltip language menu tracked from the
        // tooltip WndProc, the tray submenus, any in-process TrackPopupMenu)
        // is not a "clicked outside" event and must not dismiss anything.
        // Without this guard the dismissal sequence was: mouse-down over the
        // menu -> this callback posts kDismissMessage to the GUI thread ->
        // the modal menu loop dispatches it mid-tracking (probe-verified on
        // this machine: same-thread posted messages ARE dispatched inside
        // TrackPopupMenuEx) -> TooltipWindow::Dismiss() hides the menu's
        // owner window and clears source_text_ (B1-H2) before the pick can
        // reach lang_change_cb_ -> the F-11 re-translate never fires. This
        // is why 0 user logs ever contain the tooltip-menu lang_sync pair
        // despite repeated attempts (B-3 report 260907 §1). WindowFromPoint/
        // GetClassNameW/GetWindowThreadProcessId are all thread-safe here;
        // the pid check keeps foreign context menus (right-click on another
        // app) dismissing the tooltip as before.
        HWND under = ::WindowFromPoint(pt);
        if (under) {
            wchar_t menu_class[16] = {};
            if (::GetClassNameW(under, menu_class, 16) > 0 &&
                ::lstrcmpW(menu_class, L"#32768") == 0) {
                DWORD pid = 0;
                ::GetWindowThreadProcessId(under, &pid);
                if (pid == ::GetCurrentProcessId()) {
                    // R1-E: stamp the own-popup predicate so the drag-release
                    // callback exempts this gesture unconditionally (even with no
                    // card visible) instead of only inside its IsVisible() block.
                    drag_press_own_popup.store(true, std::memory_order_relaxed);
                    DIAG_LOG("UI", "mouse_down exempted over own popup menu (no dismissal)");
                    return;
                }
            }
        }
        // V10F FUP-02: a press on our own language picker (the modal "검색…"
        // popup or its EDIT/LISTBOX children - OwnLanguagePaletteWindowAtPoint
        // resolves the ancestor that carries the picker class) is a picker
        // interaction. Without this exemption the fall-through dismissed the
        // tooltip behind the picker (a palette click sits outside the tooltip
        // rect): Dismiss() empties source_text_/source_lang_code_, and the
        // pick's retranslate then read wiped state (retranslate_skip_empty).
        // The gesture is also stamped picker-owned so the drag-release callback
        // never summons the drag icon over the open picker.
        if (OwnLanguagePaletteWindowAtPoint(pt)) {
            drag_press_own_palette.store(true, std::memory_order_relaxed);
            DIAG_LOG("UI", "mouse_down exempted over own language picker (no dismissal)");
            return;
        }
        if (drag_icon.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(drag_icon.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                drag_icon.Hide();
            } else {
                // V10F D1: the press landed ON our own drag icon - stamp the
                // press-time predicate (the visible icon stays; the release
                // callback consults the flag unconditionally).
                drag_press_own_drag_icon.store(true, std::memory_order_relaxed);
            }
        }
        if (tooltip.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(tooltip.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                tooltip.Dismiss();
            }
        }
        // REQ-005 (plan §2.2): click-outside dismissal for the About popup.
        // AboutWindow::Dismiss marshals internally when called off-thread.
        if (about_window.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(about_window.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                about_window.Dismiss();
            }
        }
    });

    // REQ-002 (plan §2.1): forward wheel input to the tooltip. The LL hook
    // callback runs on the hook thread; Windows routes WM_MOUSEWHEEL to the
    // FOCUSED window only, and the tooltip is WS_EX_NOACTIVATE (never focused),
    // so the hook is the sole delivery path. Gate: tooltip visible + cursor
    // inside its rect (physical px, same units as the hook's ms->pt). Post a
    // kScrollMessage (REQ-R10 marshaling) and return: FORWARD-ONLY - the hook
    // proc always passes the event to CallNextHookEx afterwards (see
    // mouse_hook.cpp), so underlying apps keep their own wheel behavior and
    // the LowLevelHooksTimeout budget sees only an IsWindowVisible +
    // GetWindowRect + PostMessage-cheap callback.
    mouse_hook.SetMouseWheelCallback([&](int x, int y, int delta) {
        // Only atomics + thread-safe Win32 here (I4 discipline): is_message_mode_
        // is a plain bool owned by the GUI thread, so the message-mode gate lives
        // in ScrollByDipWheel (runs on the GUI thread after the post), not here.
        if (!tooltip.IsVisible()) {
            return;
        }
        RECT r = {};
        if (!::GetWindowRect(tooltip.GetHwnd(), &r)) {
            return;
        }
        if (!::PtInRect(&r, POINT{ x, y })) {
            return;
        }
        ::PostMessageW(tooltip.GetHwnd(), emebalachat::TooltipWindow::kScrollMessage, 0,
                       static_cast<LPARAM>(delta));
    });

    // Click on DragIconWindow triggers translation of active selection.
    // Invoked from DragIconWindow::WndProc => runs ON THE MAIN GUI THREAD.
    //
    // REQ-R1 (session 260905_0001) root-cause fix. Two coupled defects made the
    // tooltip "never appear" for the user:
    //   (a) The blocking clipboard-sequence wait (CopySelectionWithSequenceWait
    //       polls with Sleep up to ~200 ms) and the synchronous engine.Translate
    //       (seconds for the local LLM, a network RTT for Google) BOTH ran here
    //       on the GUI thread, freezing the message pump mid-click.
    //   (b) Every failure branch (copy gate false, empty text) returned SILENTLY
    //       - no tooltip, no message - so a slow/again-empty target produced
    //       exactly the reported "I click the floating button and nothing
    //       happens" with zero feedback.
    // Fix (minimal, root-cause): hand the entire copy+translate to a dedicated
    // worker thread and ALWAYS land a tooltip - the translation on success, a
    // clear notice on failure - via the existing REQ-R10 thread-safe marshal
    // seams (ShowTranslationThreadSafe / ShowMessageThreadSafe), never touching
    // the single-threaded D2D target off the GUI thread.
    // ---- R6 Phase 3 (audit item 7): bounded, JOINABLE drag-translate worker ----
    // Replaces the per-click std::thread(...).detach(). The detached variant
    // had NO happens-before with shutdown: config/engine/badge/tooltip are
    // wWinMain stack objects, and a detached thread still inside
    // engine.Translate (seconds on the local LLM) could outlive their scope ->
    // the use-after-free class plan §3.5 A9 asked about. The shutdown
    // WaitInferenceIdle(2000) only WARNS on timeout; it cannot join a detached
    // thread. Pattern: the proven REQ-R06 double-Ctrl+C worker (src/hook.cpp
    // L50-109) - one persistent std::jthread, single pending slot,
    // deterministic join in the shutdown sequence. Latest-wins coalescing
    // matches the B1-H1 generation guard, and a superseded job that never
    // starts is now also never COMPUTED (Phase 2 Issue #1's wasted-work item).
    struct DragTranslateJob {
        int x = 0;
        int y = 0;
        uint64_t gen = emebalachat::TooltipWindow::kGenNone;
    };
    // D2 (session 260910_0007): the slot state (mutex/cv/pending/slot) now
    // lives inside the SingleSlotWorker<DragTranslateJob> instance declared
    // below the handler lambda.

    // The copy + translate body, verbatim from the old detached thread. Runs
    // on drag_translate_worker below; the config/engine/badge/tooltip
    // references are now provably safe - the worker is joined BEFORE those
    // objects can leave scope, and before the surfaces' Destroy() runs.
    auto run_drag_translate = [&](int click_x, int click_y, uint64_t gen) {
        // V10F (261122_0002 P2-F): publish this job in the unified registry
        // for its whole body (Esc / badge-click cancel the inference; a fresh
        // drag click supersedes it through the same token).
        emebalachat::ReadingJobScope drag_scope(reading_jobs.drag_running,
                                                reading_jobs.drag_cancel);
        // FIX-B1 residual (a), FIX-B2: the drag gesture owns its capture-chord
        // write through a task scope, so a concurrent pipeline task can never
        // read it as its own (and vice versa). Declared before the backup and
        // the chord; every restore below runs while it is alive.
        emebalachat::ClipboardTaskScope clipboard_scope;
        // Phase 8 Batch 1 (REQ-005, plan 225900 §1.5/§4.1): console/terminal
        // SIGINT guard. In conhost/Windows Terminal a synthetic Ctrl+C is a
        // process interrupt (SIGINT), not "copy selection", so the drag
        // capture must never reach those windows. The DragIconWindow is
        // WS_EX_NOACTIVATE and returns MA_NOACTIVATE on WM_MOUSEACTIVATE, so
        // the foreground window at click time is still the text source that
        // would receive the keystroke - it is the correct gate subject.
        // Checked BEFORE BackupClipboard so the terminal path performs zero
        // clipboard access, per plan. Reuses TooltipNoSelection (existing
        // StringId) to keep i18n untouched; Fail-open by construction: any
        // query failure inside IsConsoleCaptureUnsafe (including a null
        // foreground hwnd) returns false and the existing path runs unchanged.
        if (emebalachat::IsConsoleCaptureUnsafe(::GetForegroundWindow())) {
            DIAG_F("MAIN/DragIconClick/003: console/terminal foreground; skipping synthetic Ctrl+C capture (SIGINT hazard, REQ-005)\n");
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection),
                                          gen);
            return;
        }

        // V10F L2 (261122_0002 P2-G): the window this selection came from,
        // recorded for the translation-history ring (and the editor window's
        // "apply to original window"). Sampled here - the icon is
        // WS_EX_NOACTIVATE, so the foreground is still the text source. Best
        // effort: the user may have switched windows during a slow inference.
        const HWND drag_target_hwnd = ::GetForegroundWindow();

        emebalachat::ClipboardBackup backup;
        // R1-6 (v0.10.4 sweep, 261029_0001): the backup RESULT gates both restore
        // calls below. Pre-fix the value was discarded and RestoreClipboard ran
        // unconditionally, so a backup that lost the OpenClipboard race (password
        // managers, OneDrive clipboard sync, Teams, a shell copy in flight - the
        // documented 100 ms budget) still emptied the user's clipboard and put
        // nothing back. RestoreClipboard now also refuses an empty snapshot by
        // itself; this gate keeps the CALLER honest (no restore call at all).
        const bool backup_ok = emebalachat::BackupClipboard(backup);
        DIAG_F("MAIN/DragIconClick/005: clipboard backup ok=%d has_text=%d extra_formats=%zu partial=%d\n",
               backup_ok ? 1 : 0, backup.text.has_value() ? 1 : 0, backup.formats.size(),
               backup.partial ? 1 : 0);

        // D2-flagged (report issue 1): the old CopySelection(); Sleep(35);
        // pattern had the same stale-read race as audit §2.3 for slow
        // (Electron IPC) targets. The D2 public seam confirms the copy via
        // GetClipboardSequenceNumber() polling and returns false instead of
        // exposing stale clipboard text. On the worker thread, its
        // Sleep-polling never freezes the GUI pump.
        //
        // REQ-005 (session 260910_0003 Task B) + REQ-051 (symptom A-1,
        // session 260921): the copy-chord cycle now runs through the SHARED
        // retry driver CopyChordWithSettledRetry - kDragCopyChordAttempts (3)
        // attempts with the established kDragCopyRetryBackoffMs (70 ms)
        // settle between them. The click that starts this job lands on our
        // WS_EX_NOACTIVATE drag icon, so the text-source window may not have
        // finished (re-)acquiring the foreground when the first synthetic
        // Ctrl+C fires; slow-focus Electron targets (documented ~100-200 ms
        // focus-transition latency) could outlive the old 2-attempt ~
        // 230 ms cycle - the confirmed symptom-A root cause ("drag shows the
        // wrong content" = capture failure leaving the previous text on
        // screen). The third attempt lands at ~300 ms with the extended
        // 120 ms patience. THE REQ-R04 INVARIANT HOLDS FOR EVERY ATTEMPT:
        // each CopySelectionWithSequenceWait re-reads the clipboard sequence
        // immediately before its own keystroke, so a late commit from a lost
        // chord is absorbed into the fresh baseline and can never be read as
        // stale text. Worst case is 80+70+80+70+120 = 420 ms on THIS worker
        // thread; the GUI pump stays free. The double-Ctrl+C path
        // (MAIN/DoubleCtrlC/001) keeps no retry: it fires only when the USER
        // physically pressed Ctrl+C, so no icon click displaced the
        // foreground.
        bool copy_confirmed = emebalachat::CopyChordWithSettledRetry(
            emebalachat::kDragCopyChordAttempts, emebalachat::kDragCopyRetryBackoffMs);
        if (!copy_confirmed) {
            // R1-6: only a backup that produced a snapshot may touch the
            // clipboard; RestoreClipboard additionally refuses a snapshot the
            // user's newer copy has already superseded.
            if (backup_ok) {
                emebalachat::RestoreClipboard(backup);
            }
            DIAG_F("MAIN/DragIconClick/001: clipboard copy not confirmed after %d attempt(s); selection lost or target too slow\n",
                   emebalachat::kDragCopyChordAttempts);
            // REQ-R1(b): failure is now user-visible, not silent.
            // R6 B1-H1: carries this request's generation - a superseded
            // drag must not stamp a notice over a newer result either.
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipCopyFailed),
                                          gen);
            return;
        }
        std::wstring selected = emebalachat::GetClipboardText();
        if (backup_ok) {
            emebalachat::RestoreClipboard(backup);
        }

        if (selected.empty() || selected.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            DIAG_F("MAIN/DragIconClick/002: clipboard copy confirmed but text empty\n");
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection),
                                          gen);
            return;
        }

        // Phase 3 Batch 2 (plan §2.5, REQ-007): the drag path uses the DRAG
        // pair. I4: snapshot read (worker thread). F3 (session 260908_0002):
        // read the WHOLE pair once - V6 found this path consumed only
        // drag_target_language and forced the detected value as the engine
        // source, silently ignoring a user-pinned drag_source_language.
        const auto snap = config.GetSnapshot();
        // 261111_0001 U6 (v0.10.7, R2-02/R2-04): the drag prompt label comes
        // from the GATED accessor (U2's dominant-script model inside
        // DetectLanguage + the Unknown->AUTO prompt contract), never a raw
        // DetectLanguage call — the resolved label feeds the engine prompt
        // AND ResolveEffectiveTarget's pivot, so one seam governs both.
        std::string detected = emebalachat::DetectLanguageForPrompt(selected);
        // ADR-A1-2: NON-AUTO persisted source wins over detection; "Auto
        // Detect" keeps the established drag contract of injecting the
        // DETECTED language into the engine as an explicit source.
        // F1 (session 260908_0003, verify 220010 §5 item 4) + F5 (audit 181530
        // Option B): the injected value is the AUTO marker "Auto Detect" for
        // ALL Latin script (pure-ASCII included - the "English" label was
        // retired F5) and for true-VI-less diacritic Latin before that -
        // NormalizeLanguageCode maps it to "AUTO". LocalPairReliable is
        // mask-driven since 261116_0003 P2 (PR2-04: the TARGET must carry
        // kLangHyMt2, then an AUTO source is a SOURCE-side relaxation only),
        // so an AUTO source keeps an OFFICIAL target on Hy-MT2 (model's
        // built-in language ID) while a cloud-only target is routed by
        // PlanTranslationRouting's consent rules.
        // 261116_0003 P2 fix round 2b: the unqualified "keeps the request
        // on Hy-MT2" was falsified for the 8 cloud-only targets by the
        // PR2-04 reorder. BuildPrompt
        // emits no source token. The engine call below receives eff_src
        // verbatim - the label policy is in DetectLanguage, not a string
        // special-case here.
        const bool src_pinned = emebalachat::NormalizeLanguageCode(
                                    snap.drag_source_language) != "AUTO";
        std::string eff_src = emebalachat::ResolveEffectiveSource(
                                  snap.drag_source_language, detected);
        // ADR-A1-4: the tooltip tag shows the EFFECTIVE source (pinned value
        // when fixed, detected code under AUTO), not the raw detection.
        std::string src_code = emebalachat::NormalizeLanguageCode(eff_src);
        // F1: an AUTO engine source renders as the localized "Auto Detect" tag
        // via the tooltip's existing empty-label seam (empty source_lang_code
        // -> I18n::AutoDetect), never as the raw string "AUTO".
        const std::string tooltip_src = (src_code == "AUTO") ? std::string() : src_code;
        std::string tgt_lang = snap.drag_target_language;
        // F2 (session 260908_0003): pivot provenance. A drag target the user
        // explicitly picked (drag_target_pinned, set by ApplyLanguageChange
        // on every explicit Drag-target menu pick and persisted across
        // restarts) bypasses the collision pivot entirely; only never-touched
        // AUTO defaults stay pivotable (V6 KO->KO guard remains armed there).
        const bool tgt_user_explicit = snap.drag_target_pinned;

        bool pivot_fired = false;
        if (auto effective = emebalachat::ResolveEffectiveTarget(eff_src, tgt_lang,
                                                                 tgt_user_explicit)) {
            pivot_fired = true;
            tgt_lang = *effective;
            // F1 (session 260908_0002, ADR-A1-1): the pivot is a LOCAL,
            // per-request temporary value. It feeds this engine.Translate
            // call and the tooltip label ONLY - the user-pinned
            // drag_target_language in config.json is never overwritten by an
            // automatic pivot. V1's root cause was the RequestLanguageSync
            // here persisting the ephemeral pivot through SaveToFile(); with
            // that coordinator call removed, no translation-path code can
            // reach persistence (explicit tray/tooltip menu picks remain the
            // only writers). Tray/badge keep the persisted pair while the
            // tooltip shows this effective value - the intended ADR-A1-1
            // surface split. persist=0 marks this pivot as non-persisting.
            // fix-list-round15 R15-21 (F-3b): persisted_tgt/pivot_tgt are
            // language tokens (can be user-typed stored names) — the R14-06
            // class — so they log only behind diag::ContentLoggingEnabled();
            // ctx/path/persist ride both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("STATE", "pivot_temporary ctx=drag path=icon persisted_tgt=%s pivot_tgt=%s persist=0",
                         snap.drag_target_language.c_str(), tgt_lang.c_str());
            } else {
                DIAG_LOG("STATE", "pivot_temporary ctx=drag path=icon persisted_tgt_len=%zu pivot_tgt_len=%zu persist=0",
                         snap.drag_target_language.size(), tgt_lang.size());
            }
        }
        // F3 DIAG (task §3-6): prove per request whether the engine source came
        // from the pinned config or from detection, and whether the ADR-A1-7
        // neutral pivot fired or was skipped (pivot=0 with eff==tgt and a
        // collision is the 3rd-rank skip). QA cross-checks this line against
        // the ENGINE/Translate/040 pair line so a (KO -> KO) regression is
        // immediately attributable.
        // F2: usr_explicit appended as a NEW trailing field - the existing
        // pivot=/tgt= grep axes (F3 QA, B6/C2 parametric) are unchanged.
        // fix-list-round15 R15-21 (F-3b): persisted/detected/eff/tgt are
        // language tokens (can be user-typed stored names) — the R14-06
        // class — so they log only behind diag::ContentLoggingEnabled();
        // mode/pivot/usr_explicit ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE", "drag_src path=icon mode=%s persisted=%s detected=%s eff=%s pivot=%d tgt=%s usr_explicit=%d",
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.c_str(), detected.c_str(),
                     eff_src.c_str(), pivot_fired ? 1 : 0, tgt_lang.c_str(),
                     tgt_user_explicit ? 1 : 0);
        } else {
            DIAG_LOG("STATE", "drag_src path=icon mode=%s persisted_len=%zu detected_len=%zu eff_len=%zu pivot=%d tgt_len=%zu usr_explicit=%d",
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.size(), detected.size(),
                     eff_src.size(), pivot_fired ? 1 : 0, tgt_lang.size(),
                     tgt_user_explicit ? 1 : 0);
        }

        // PIPE-1 (session 261104_0001, handoff section 2.1): the drag path
        // used to send the selection to the engine unconditionally, so URLs,
        // pure numbers, pure punctuation and emoji were translated (or at
        // least run through inference) every time. Run the shared SmartBypass
        // gate on the EFFECTIVE pair: the pivot above has already fired, so
        // text in the configured drag target still auto-pivots to the user's
        // native language and never reaches this check as a rejection. The
        // source argument stays the AUTO marker: a user-pinned drag source
        // must not silently skip mixed-script selections (ShouldTranslate
        // step 4 stays disarmed on this path). Bypassed gestures exit before
        // the transient badge flip below, so the label never flickers.
        if (!emebalachat::ShouldTranslate(selected, tgt_lang, "Auto Detect")) {
            // fix-list-round15 R15-21 (F-3b): detected/tgt are language
            // tokens (can be user-typed stored names) — the R14-06 class —
            // so they log only behind diag::ContentLoggingEnabled(); the
            // else keeps the shape-only lengths.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("STATE", "drag_bypass path=icon detected=%s tgt=%s",
                         detected.c_str(), tgt_lang.c_str());
            } else {
                DIAG_LOG("STATE", "drag_bypass path=icon detected_len=%zu tgt_len=%zu",
                         detected.size(), tgt_lang.size());
            }
            DIAG_F("MAIN/DragIconClick/006: drag content filtered by the smart-bypass gate (URL/number/symbol/emoji or already in the pivoted target); engine not called\n");
            // 261111_0001 U1 (v0.10.7, audit D3-feedback, ISSUE-009): this drop
            // used to be SILENT - the drag icon vanished with zero feedback and
            // the user could not tell why nothing was translated. Surface the
            // same REQ-R1 "the click cannot produce a translation" notice the
            // console-gate above uses: StringId::TooltipNoSelection ("no text
            // selected to translate" family - the closest existing notice; no
            // dedicated "not translatable" StringId exists and src/i18n.* is
            // owned by another unit this session, so no new literal is added).
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection),
                                          gen);
            return;
        }

        // REQ-013 (user ruling 260913 22:02): while THIS drag translation
        // runs, the badge label briefly shows the DRAG pair (the effective
        // pair, post-pivot - the same pair the tooltip below displays); on
        // flow end the guard restores the TYPE pair it reads LIVE at that
        // moment (Ctrl+F9 during the inference is honored at restore).
        // Display-only: no config write, no engine-pair change, no persistence.
        // Placed after every early return (console gate, copy failure, empty
        // text) so aborted gestures never flicker the label; the remaining
        // body has a single exit, which this RAII guard restores.
        emebalachat::TransientDragPairBadge badge_transient(
            config, badge, emebalachat::ToUtf16(eff_src), emebalachat::ToUtf16(tgt_lang));

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        // V10F (261122_0002 P2-F): the per-request task context. ctx.cancel
        // polls the registry token (Esc / badge click / superseding drag);
        // out_error_detail carries THIS request's bounded, sanitized cause -
        // the racy process-global OpenAiCompatibleClient::LastChatErrorDetail
        // read this block used to do immediately after the call is GONE (that
        // slot could be cleared/overwritten by any concurrent request); the
        // lease-guarded on_chunk/on_chunk_text seams drive the live preview
        // card for genuine multi-chunk runs only.
        emebalachat::TranslationTaskContext drag_ctx;
        drag_ctx.cancel = &reading_jobs.drag_cancel;
        std::string drag_error_detail;
        drag_ctx.out_error_detail = &drag_error_detail;
        emebalachat::ChunkPreviewLease drag_preview(reading_jobs, tooltip);
        drag_ctx.on_chunk = [&drag_preview](std::size_t, std::size_t total) {
            drag_preview.TryBegin(static_cast<int>(total));
        };
        drag_ctx.on_chunk_text = [&drag_preview](const std::wstring& cumulative) {
            drag_preview.Append(cumulative);
        };
        // REQ-047 P5-F2 (technical review 175600 finding #2): receive the
        // status instead of discarding it — the 3-arg overload swallowed
        // CloudConsentBlocked / LocalModelMissing, so a strict local failure
        // on this path died silently. Feed the outcome through the same
        // engine-unavailable seam the Enter pipeline uses (the drain handler
        // applies the streak-latch policy on the GUI thread).
        emebalachat::TranslationStatus drag_status = emebalachat::TranslationStatus::Ok;
        std::wstring translated = engine.Translate(selected, eff_src, tgt_lang, &drag_status, drag_ctx);
        badge.SetStatus(emebalachat::BadgeStatus::Active);
        if (drag_status == emebalachat::TranslationStatus::Canceled) {
            // V10F (261122_0002 P2-F): an INTENTIONAL user cancel (Esc / badge
            // click / superseding drag). No failure notice for a cancel the
            // user asked for; the preview lease dtor retires the live card.
            DIAG_LOG("UI", "drag_translate_canceled gen=%llu",
                     static_cast<unsigned long long>(gen));
            return;
        }
        // 261124_0001: same LocalModelLoadFailed routing as the Enter seam
        // (user_gguf keeps the file-presence route; the others get the
        // dedicated load-failed wording, no download offer).
        if (drag_status == emebalachat::TranslationStatus::LocalModelLoadFailed) {
            // V10F: retire the live preview BEFORE the modal (End(false) hides
            // the window - it must not swallow what follows).
            drag_preview.End(false);
            const auto load_failed_snap = config.GetSnapshot();
            if (load_failed_snap.engine_type == "user_gguf") {
                emebalachat::RequestModelMissingOfferOrModal(load_failed_snap.engine_type,
                                                             load_failed_snap.user_model_id);
            } else {
                emebalachat::RequestEngineUnavailableModal(
                    emebalachat::g_hControllerWnd,
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedTitle),
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedBody));
            }
            // 261111_0001 U1 (ISSUE-009): these statuses always carry an EMPTY
            // translation; the modal above IS the user feedback, so stop here
            // (falling through renders a permanent empty ghost card).
            return;
        }
        if (drag_status == emebalachat::TranslationStatus::LocalModelMissing) {
            // V10F E2 (261122_0002 P3-C4 wave B): same one-click offer routing
            // as the Enter-pipeline seam (built-in download consent / user-gguf
            // manager for a model-only miss) instead of the dead-end wording.
            // V10F: retire the live preview BEFORE the offer/modal (End(false)
            // hides the window - it must not swallow what follows).
            drag_preview.End(false);
            // 261123_0001: fresh locked snapshot at failure time (engine_type
            // + user_model_id read together, I4) for the route classification.
            const auto miss_snap = config.GetSnapshot();
            emebalachat::RequestModelMissingOfferOrModal(miss_snap.engine_type,
                                                         miss_snap.user_model_id);
            // 261111_0001 U1 (v0.10.7, audit D3-feedback, ISSUE-009): these
            // statuses always carry an EMPTY translation, and falling through
            // to ShowTranslationThreadSafe below used to render a permanent
            // empty ghost card (ShowTranslation kills the message autohide
            // timer, tooltip.cpp kTimerMessageAutohide, and arms no hide of
            // its own). The modal above IS the user feedback; stop here.
            return;
        }
        if (drag_status == emebalachat::TranslationStatus::CloudConsentBlocked) {
            // REQ-048 R4: same cause split as the Enter-pipeline seam - missing
            // source vs transient serving failure get different bodies.
            // V10F: retire the live preview BEFORE the modal (End(false) hides
            // the window - it must not swallow the modal that follows).
            drag_preview.End(false);
            emebalachat::RequestEngineUnavailableModal(
                emebalachat::g_hControllerWnd,
                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                emebalachat::I18n::Get(emebalachat::StringId::RepairTransientBody));
            // 261111_0001 U1 (v0.10.7, audit D3-feedback, ISSUE-009): these
            // statuses always carry an EMPTY translation, and falling through
            // to ShowTranslationThreadSafe below used to render a permanent
            // empty ghost card (ShowTranslation kills the message autohide
            // timer, tooltip.cpp kTimerMessageAutohide, and arms no hide of
            // its own). The modal above IS the user feedback; stop here.
            return;
        } else if (!translated.empty()) {
            // Success sentinel: resets the streak latch so a later strict
            // failure re-arms the modal (same path as the Enter pipeline).
            emebalachat::RequestEngineUnavailableModal(emebalachat::g_hControllerWnd,
                                                       std::wstring(), std::wstring());
        } else {
            // REQ-048 R4: EngineFailed / timeout / cancel used to update NOTHING
            // on this path - the previous drag's tooltip stayed on screen and
            // read as "it shows the sentence I dragged before" (the reported
            // stale-result bug). Surface a failure notice stamped with THIS
            // request's generation so a newer drag still supersedes it.
            // 261111_0001 U6 (v0.10.7, R1 P1-3): when the failed leg is the
            // OpenAI-compatible engine, the notice carries the REAL cause —
            // the HTTP status + the provider's own message, already bounded,
            // sanitized and API-key-masked by the client - so a 503
            // "Credit accounting is temporarily unavailable" reads as the
            // transient platform state it is, not a silent failure. Local /
            // Google failures keep the established generic notice, and the
            // translation itself stays empty (no invented text).
            // V10F (261122_0002 P2-F): the cause now comes from THIS request's
            // out_error_detail carrier (see the ctx wiring above) - never the
            // process-global slot. Retire the live preview BEFORE the notice
            // (End(false) hides the window; the notice must survive).
            drag_preview.End(false);
            std::wstring fail_body =
                emebalachat::I18n::Get(emebalachat::StringId::TooltipTranslateFailed);
            if (!drag_error_detail.empty()) {
                fail_body += L"\n";
                fail_body += emebalachat::ToUtf16(drag_error_detail);
            }
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          fail_body,
                                          gen);
            // 261111_0001 U1 (v0.10.7, audit D3-feedback, ISSUE-009): return
            // IMMEDIATELY after the failure notice. The old code fell through
            // to ShowTranslationThreadSafe(..., L"") below, and ShowMessage's
            // autohide timer (kTimerMessageAutohide) was killed by
            // ShowTranslation's KillTimer - leaving a permanent empty ghost
            // card that re-created the drag dead zone.
            return;
        }

        // V10F L3-B: freeze the live preview with the last cumulative text
        // BEFORE the final card replaces it (the P2-C contract order).
        drag_preview.End(true);

        // Worker thread, so marshal the D2D render to the GUI thread via the
        // REQ-R10 thread-safe seam (audit §3.4) instead of a direct call.
        // R6 B1-H1: if a newer request was stamped while this thread was
        // inside engine.Translate (local LLM takes seconds), the tooltip
        // drops this stale payload instead of showing the previous
        // translation (the reported intermittent bug).
        // 261111_0001 U1 (v0.10.7): only the non-empty success result reaches
        // this call - every empty-result path above returns first.
        tooltip.ShowTranslationThreadSafe(click_x, click_y, selected, tooltip_src, tgt_lang, translated,
                                          gen);
        // V10F L2 (261122_0002 P2-G): record the successful drag in the
        // translation-history ring (back empty - the drag path never runs the
        // verification round trip; only the back-translate job Pushes the
        // full triple).
        emebalachat::translation_history::Push({ selected, translated, std::wstring(),
                                                 emebalachat::ToUtf16(src_code),
                                                 emebalachat::ToUtf16(tgt_lang),
                                                 drag_target_hwnd });
    };

    // D2 (session 260910_0007): loop body extracted verbatim into
    // SingleSlotWorker<JobT> (src/single_slot_worker.hpp) - same predicate,
    // same stop-before-pop ordering, same no-logging/no-exception-guard
    // contract. The handler keeps the original call shape.
    emebalachat::SingleSlotWorker<DragTranslateJob> drag_translate_worker(
        [&run_drag_translate](const DragTranslateJob& job) {
            run_drag_translate(job.x, job.y, job.gen);
        });

    // ---- F9 (session 260908_0002, ADR-A1-5): async tooltip re-translation ----
    // The tooltip language-change callback used to run engine.Translate
    // SYNCHRONOUSLY on the GUI thread inside the tooltip WndProc (V8:
    // measured 1.26-5.36 s freezes; gen 21 crossed the 5 s "Not
    // Responding" threshold). TranslationManager::Translate holds mutex_
    // for the WHOLE inference, so the message pump stalls until it
    // returns. The re-translation now runs on a dedicated worker mirroring
    // drag_translate_worker above (one persistent std::jthread, single
    // pending slot, deterministic shutdown join). ADR-A1-5 keeps it in a
    // SEPARATE queue from the drag jobs: a queued re-translation must
    // never be superseded by an unrelated drag click; latest-wins applies
    // only between consecutive language picks, and a result superseded in
    // the meantime is dropped at render time by the B1-H1 generation
    // guard (ShowTranslation logs the drop as tooltip_marshal_drop).
    struct RetranslateJob {
        std::wstring src;
        std::string src_code;
        std::string new_tgt;
        int x = 0;
        int y = 0;
        uint64_t gen = emebalachat::TooltipWindow::kGenNone;
        // V10F L2 (261122_0002 P2-G): EDITOR-ORIGIN jobs only (the tooltip
        // header's "edit / re-translate" hand-off). When set, the result is
        // delivered through this completion ON THE RETRANSLATE WORKER THREAD
        // (the editor marshals to its own GUI thread) INSTEAD of re-rendering
        // the tooltip card. Empty = the ordinary tooltip re-render path,
        // byte-identical to the pre-V10F behavior.
        std::function<void(const std::wstring&)> on_done;
    };
    // D2 (session 260910_0007): slot state now lives inside the
    // SingleSlotWorker<RetranslateJob> instance declared below the handler.
    // ADR-A1-5 queue SEPARATION is unchanged: this is still a second, fully
    // independent worker/queue from the drag one above.

    // Runs ON retranslate_worker below. D2D-safe: the result is marshaled
    // through the REQ-R10 ShowTranslationThreadSafe seam, never a direct
    // ShowTranslation call. Empty-result policy (ADR-A1-5 / V8 §d-4): a
    // failed translation keeps the existing tooltip content instead of
    // rendering an empty body.
    auto run_retranslate = [&](const RetranslateJob& job) {
        // V10F (261122_0002 P2-F): publish this job in the unified registry
        // for its whole body (Esc / badge-click cancel it; a fresh language
        // pick or editor re-translate supersedes it through the same token).
        emebalachat::ReadingJobScope rt_scope(reading_jobs.retranslate_running,
                                              reading_jobs.retranslate_cancel);
        // V10F L2: an editor-origin job with an empty source can only fail -
        // report completion immediately so the editor never waits on a job
        // that produces nothing.
        if (job.src.empty()) {
            if (job.on_done) job.on_done(std::wstring());
            return;
        }
        // fix-list-round15 R15-21 (F-3b): tgt is a language token (can be a
        // user-typed stored name) — the R14-06 class — so it logs only
        // behind diag::ContentLoggingEnabled(); gen/src_len ride both
        // branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("UI", "retranslate_worker_run gen=%llu src_len=%zu tgt=%s",
                     static_cast<unsigned long long>(job.gen), job.src.size(),
                     job.new_tgt.c_str());
        } else {
            DIAG_LOG("UI", "retranslate_worker_run gen=%llu src_len=%zu tgt_len=%zu",
                     static_cast<unsigned long long>(job.gen), job.src.size(),
                     job.new_tgt.size());
        }

        // F3-3 (session 260908_0002, ADR-A1-3): V6 entry-point 3 was the true
        // origin of the KO->KO output corruption (log gen 8/10/12/21/24) - it
        // fed the engine the last DISPLAYED detection code ("KO") together with
        // the freshly picked target ("Korean") with no source resolution and no
        // same-language guard. Now: (i) a pinned persisted source wins over the
        // carried detection code (G2 / ADR-A1-2; job.src_code is the value the
        // GUI thread captured at enqueue time, so no cross-thread tooltip read
        // happens here), (ii) an empty AUTO fallback re-detects from the held
        // text (R2 safety net), (iii) the ADR-A1-7 neutral pivot guard fires on
        // eff_src == tgt so no re-translation path can request a same-language
        // pair anymore.
        const auto snap = config.GetSnapshot();
        const bool src_pinned = emebalachat::NormalizeLanguageCode(
                                    snap.drag_source_language) != "AUTO";
        std::string eff_src = emebalachat::ResolveEffectiveSource(
                                  snap.drag_source_language, job.src_code);
        if (eff_src.empty()) {
            // R1-H / R2-17 governance (orchestrator pin-repair round): the
            // re-detect safety net feeds the ENGINE PROMPT source, so it must
            // resolve through the gated accessor (Unknown -> Auto Detect), not
            // the bare DetectLanguage. Every drag-family prompt source now goes
            // through that one accessor: this path, run_drag_translate, and the
            // double-Ctrl+C path (G-03, fix-list round 2).
            eff_src = emebalachat::DetectLanguageForPrompt(job.src);
        }
        std::string src_code = emebalachat::NormalizeLanguageCode(eff_src);
        // F1 (session 260908_0003): AUTO engine source -> localized "Auto
        // Detect" tooltip tag via the empty-label seam (see run_drag_translate).
        // job.src_code is empty exactly when the previous tooltip showed AUTO,
        // so the re-detect above yields "Auto Detect" again for Latin text and
        // the engine receives the AUTO marker end-to-end.
        const std::string tooltip_src = (src_code == "AUTO") ? std::string() : src_code;
        std::string tgt_lang = job.new_tgt;
        // F2 (session 260908_0003): job.new_tgt is BY CONSTRUCTION a direct
        // user pick - the retranslate queue only ever receives targets from
        // the tooltip target menu (SetLanguageChangeCallback) and the source
        // menu's re-render of the currently-shown target. Both are explicit
        // user actions, so the pivot is bypassed unconditionally here
        // (verify 220310 §5 item 3: "always user_pinned=true there").
        const bool tgt_user_explicit = true;
        bool pivot_fired = false;
        if (auto effective = emebalachat::ResolveEffectiveTarget(eff_src, tgt_lang,
                                                                 tgt_user_explicit)) {
            pivot_fired = true;
            tgt_lang = *effective;
        }
        // F3 DIAG (task §3-6): same evidence contract as the drag paths -
        // source decision (config vs carried detection) + pivot fired/skipped.
        // F2: usr_explicit appended as a NEW trailing field; the existing
        // pivot=/tgt= grep axes are unchanged.
        // fix-list-round15 R15-21 (F-3b): persisted/carried/eff/tgt are
        // language tokens (can be user-typed stored names) — the R14-06
        // class — so they log only behind diag::ContentLoggingEnabled();
        // mode/pivot/usr_explicit ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("UI", "retranslate_src gen=%llu mode=%s persisted=%s carried=%s eff=%s pivot=%d tgt=%s usr_explicit=%d",
                     static_cast<unsigned long long>(job.gen),
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.c_str(), job.src_code.c_str(),
                     eff_src.c_str(), pivot_fired ? 1 : 0, tgt_lang.c_str(),
                     tgt_user_explicit ? 1 : 0);
        } else {
            DIAG_LOG("UI", "retranslate_src gen=%llu mode=%s persisted_len=%zu carried_len=%zu eff_len=%zu pivot=%d tgt_len=%zu usr_explicit=%d",
                     static_cast<unsigned long long>(job.gen),
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.size(), job.src_code.size(),
                     eff_src.size(), pivot_fired ? 1 : 0, tgt_lang.size(),
                     tgt_user_explicit ? 1 : 0);
        }

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        // REQ-047 P5-F2 (technical review 175600 finding #2): same strict-
        // failure surfacing as the drag path above — the 3-arg overload used
        // to discard the status, leaving a strict local failure silent.
        // V10F (261122_0002 P2-F): the per-request task context wires this
        // path into the unified cancel registry (ctx.cancel) and the live
        // chunk-preview surface (lease-guarded); out_error_detail carries
        // THIS request's bounded cause (shape-only logged below; the
        // ADR-A1-5 "existing content kept" display policy is unchanged - the
        // retranslate failure never re-rendered the card and still doesn't).
        emebalachat::TranslationTaskContext rt_ctx;
        rt_ctx.cancel = &reading_jobs.retranslate_cancel;
        std::string rt_error_detail;
        rt_ctx.out_error_detail = &rt_error_detail;
        emebalachat::ChunkPreviewLease rt_preview(reading_jobs, tooltip);
        rt_ctx.on_chunk = [&rt_preview](std::size_t, std::size_t total) {
            rt_preview.TryBegin(static_cast<int>(total));
        };
        rt_ctx.on_chunk_text = [&rt_preview](const std::wstring& cumulative) {
            rt_preview.Append(cumulative);
        };
        emebalachat::TranslationStatus rt_status = emebalachat::TranslationStatus::Ok;
        std::wstring translated = engine.Translate(job.src, eff_src, tgt_lang, &rt_status, rt_ctx);
        badge.SetStatus(emebalachat::BadgeStatus::Active);
        if (!rt_error_detail.empty()) {
            DIAG_LOG("UI", "retranslate_error_detail_len=%zu gen=%llu",
                     rt_error_detail.size(), static_cast<unsigned long long>(job.gen));
        }
        // 261124_0001: same LocalModelLoadFailed routing as the Enter/drag
        // seams (user_gguf keeps the file-presence route; the others get the
        // dedicated load-failed wording, no download offer).
        if (rt_status == emebalachat::TranslationStatus::LocalModelLoadFailed) {
            rt_preview.End(false); // retire the live preview BEFORE the modal
            const auto load_failed_snap = config.GetSnapshot();
            if (load_failed_snap.engine_type == "user_gguf") {
                emebalachat::RequestModelMissingOfferOrModal(load_failed_snap.engine_type,
                                                             load_failed_snap.user_model_id);
            } else {
                emebalachat::RequestEngineUnavailableModal(
                    emebalachat::g_hControllerWnd,
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedTitle),
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedBody));
            }
            if (job.on_done) job.on_done(std::wstring());
            return; // existing content kept (ADR-A1-5); the modal carries the notice
        }
        if (rt_status == emebalachat::TranslationStatus::LocalModelMissing) {
            // V10F E2 (261122_0002 P3-C4 wave B): one-click offer routing (the
            // Enter/drag seams' policy): model-only miss -> built-in download
            // consent, user-gguf -> the model manager.
            rt_preview.End(false); // retire the live preview BEFORE the offer
            // 261123_0001 R2 (fix F-04): fresh locked snapshot at failure time
            // (engine_type + user_model_id read together, I4) for the route
            // classification, matching the three sibling producers. The
            // job-start snapshot above is the retranslate INPUT contract
            // (source/target resolution), not the failure route's input.
            const auto miss_snap = config.GetSnapshot();
            emebalachat::RequestModelMissingOfferOrModal(miss_snap.engine_type,
                                                         miss_snap.user_model_id);
            if (job.on_done) job.on_done(std::wstring());
            return; // existing content kept (ADR-A1-5); the offer carries the notice
        }
        if (rt_status == emebalachat::TranslationStatus::CloudConsentBlocked) {
            // REQ-048 R4: the transient serving failure keeps the retry wording
            // (the pre-V10F body named RepairFailedBody for BOTH statuses; the
            // cause split the other seams use is applied here too).
            rt_preview.End(false); // retire the live preview BEFORE the modal
            emebalachat::RequestEngineUnavailableModal(
                emebalachat::g_hControllerWnd,
                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                emebalachat::I18n::Get(emebalachat::StringId::RepairTransientBody));
            if (job.on_done) job.on_done(std::wstring());
            return; // existing content kept (ADR-A1-5); the modal carries the notice
        }
        if (!translated.empty()) {
            // Success sentinel: resets the streak latch (same seam as Enter).
            emebalachat::RequestEngineUnavailableModal(emebalachat::g_hControllerWnd,
                                                       std::wstring(), std::wstring());
        }

        if (translated.empty()) {
            DIAG_LOG("UI", "retranslate_skip_empty gen=%llu existing_content_kept=1",
                     static_cast<unsigned long long>(job.gen));
            // V10F L2: failure / cancel completion for an editor-origin job.
            if (job.on_done) job.on_done(std::wstring());
            return;
        }
        // V10F L3-B: freeze the live preview BEFORE the final card replaces it.
        rt_preview.End(true);
        if (job.on_done) {
            // V10F L2: editor-origin job - the RESULT goes to the editor (on
            // this worker thread; the editor marshals), NOT to the tooltip.
            emebalachat::translation_history::Push({ job.src, translated, std::wstring(),
                                                     emebalachat::ToUtf16(src_code),
                                                     emebalachat::ToUtf16(tgt_lang),
                                                     nullptr });
            job.on_done(translated);
            return;
        }
        // Worker thread -> marshal to the GUI thread. If a newer request
        // was stamped while this thread sat inside engine.Translate, the
        // tooltip's generation guard drops this stale payload.
        tooltip.ShowTranslationThreadSafe(job.x, job.y, job.src, tooltip_src,
                                          tgt_lang, translated, job.gen);
        // V10F L2 (261122_0002 P2-G): record the successful re-translation in
        // the translation-history ring. target_hwnd is unknown on this path
        // (the card is tooltip-anchored) - nullptr, documented in the report.
        emebalachat::translation_history::Push({ job.src, translated, std::wstring(),
                                                 emebalachat::ToUtf16(src_code),
                                                 emebalachat::ToUtf16(tgt_lang),
                                                 nullptr });
    };

    // D2 (session 260910_0007): same template; the handler takes the job
    // directly (the only call-shape delta between the two loops).
    emebalachat::SingleSlotWorker<RetranslateJob> retranslate_worker(run_retranslate);

    // ---- V10F L2 (261122_0002 P2-G): tooltip header -> translation editor --
    // The tooltip's two header buttons ("edit / re-translate" and "open in
    // window") invoke these callbacks ON THE GUI THREAD inside the tooltip
    // WndProc (P2-C contract). Both hand the MOST RECENT history entry to the
    // editor window; the editor's re-translate requests come back through
    // editor_retranslate_fn, which runs the EXISTING retranslate_worker path
    // (ADR-A1-5: never a synchronous engine.Translate) and reports the result
    // through the job's on_done completion.
    const emebalachat::translation_editor_window::RetranslateFn editor_retranslate_fn =
        [&](const std::wstring& source, const std::wstring& tgt_code,
            std::function<void(const std::wstring&)> on_done) {
            // The editor's request is itself a NEW translate request (B1-H1
            // stamp) so an in-flight older result cannot overwrite it.
            const uint64_t gen = tooltip.BeginTranslationRequest();
            // V10F RISK-04 Option C: an editor-issued re-translate supersedes
            // an in-flight one (same-queue latest-wins, now cancel-based
            // instead of render-dropped).
            if (emebalachat::ReadingJobRunning(reading_jobs.retranslate_running)) {
                reading_jobs.retranslate_cancel.store(true, std::memory_order_release);
            }
            RetranslateJob job;
            job.src = source;
            // src_code stays empty: the editor contract carries only the
            // edited source + target, so the run_retranslate safety net
            // re-detects (pinned drag_source_language still wins, F3-3).
            job.new_tgt = emebalachat::ToUtf8(tgt_code);
            job.gen = gen;
            job.on_done = std::move(on_done);
            retranslate_worker.Submit(std::move(job));
        };
    const auto open_translation_editor = [&]() {
        const auto recent = emebalachat::translation_history::Recent();
        if (recent.empty()) {
            // No translation has completed yet (e.g. the Enter path with
            // back-translation off records nothing). Nothing to edit: the
            // click is logged and dropped (no new StringId; see the unit
            // report's I18N REQUESTS for the follow-up hint string).
            DIAG_LOG("UI", "translation_editor_open_skipped: history empty");
            return;
        }
        emebalachat::translation_editor_window::Open(recent.front(), editor_retranslate_fn);
    };
    // P2-C: an UNWIRED callback renders its header button dimmed + inert, so
    // wiring both unconditionally keeps the header geometry stable.
    tooltip.SetEditRequestCallback([open_translation_editor]() { open_translation_editor(); });
    tooltip.SetOpenInWindowCallback([open_translation_editor]() { open_translation_editor(); });

    // Click on DragIconWindow: runs ON THE MAIN GUI THREAD (its WndProc).
    // Stamp the generation at trigger time (R6 Phase 2 B1-H1), hand the job
    // to the worker, return immediately - the message pump is never blocked
    // by clipboard settle or inference.
    drag_icon.SetClickCallback([&](int click_x, int click_y) {
        // REQ-054: this was the only drag entry point lacking the fire-time
        // IsActive() gate the others have (drag release at the
        // SetDragReleaseCallback above checks !hook.IsActive() before showing;
        // the double-Ctrl+C callback below gates on it too) — a stale icon
        // still on screen after an F9 pause must never launch a translation.
        if (!hook.IsActive()) {
            return;
        }
        // V10F RISK-04 Option C (261122_0002): a fresh drag click SUPERSEDES an
        // in-flight drag translation - the old run unwinds at the next
        // between-chunk cancel poll instead of burning seconds of inference
        // whose render the B1-H1 generation guard would drop anyway.
        if (emebalachat::ReadingJobRunning(reading_jobs.drag_running)) {
            reading_jobs.drag_cancel.store(true, std::memory_order_release);
        }
        // Generation stamping STAYS at the producer (D2 acceptance
        // constraint): stamp at trigger time, then hand the job over.
        const uint64_t gen = tooltip.BeginTranslationRequest();
        // Latest-wins overwrite: a job the worker has NOT popped yet is
        // replaced (its render would be dropped by the generation guard
        // anyway). A job already popped runs to completion and is guarded
        // at render time - unchanged Phase 2 semantics. Submit() is the
        // same lock-under-mutex assign + notify_one handshake.
        drag_translate_worker.Submit(DragTranslateJob{ click_x, click_y, gen });
    });

    // Double Ctrl+C Hotkey Detection (< 400ms).
    // REQ-R06 (audit §2.5): KeyboardHook dispatches this callback onto its own
    // persistent worker thread, NOT the LowLevelKeyboardProc thread - engine
    // inference below can run seconds and would get WH_KEYBOARD_LL silently
    // unhooked by the OS once past LowLevelHooksTimeout. Worker-thread rules:
    //  - clipboard reads and PostMessage-based badge updates are fine here;
    //  - NEVER call TooltipWindow/DragIconWindow render paths directly (their
    //    single-threaded D2D targets belong to the main GUI thread,
    //    D2DERR_WRONG_THREAD). tooltip.ShowTranslationThreadSafe() marshals.
    hook.SetDoubleCtrlCCallback([&]() {
        // REQ-R12 (I4 sweep): REQ-R06 worker thread -> snapshot read instead of
        // the direct config.drag_to_translate field access (see the drag
        // callback above for the same-pattern rationale).
        if (!config.GetSnapshot().drag_to_translate || !hook.IsActive()) {
            return;
        }

        // V10F (261122_0002 P2-F): publish this job in the unified registry
        // for its whole body (Esc / badge-click cancel the inference).
        emebalachat::ReadingJobScope dbl_scope(reading_jobs.dbl_running,
                                               reading_jobs.dbl_cancel);

        // R6 Phase 2 (B1-H1): stamp the generation at trigger time on this
        // (REQ-R06 worker) thread; every show below carries it.
        const uint64_t gen = tooltip.BeginTranslationRequest();

        // R6 Phase 2 (B1-H3, plan §1): the old code did a fixed ::Sleep(40)
        // and read the clipboard unconditionally. On slow (Electron IPC)
        // targets the app's copy handler may not have committed within 40 ms,
        // so the read returned the PREVIOUS clipboard content -> the stale
        // translation the user reported. Now the capture goes through the
        // REQ-R04-proven confirmed-capture seam (GetClipboardSequenceNumber
        // polling, same as the drag path): the re-issued Ctrl+C must make the
        // sequence provably move and settle, otherwise nothing is read (never
        // a stale value).
        //
        // Phase 8 Batch 1 (REQ-005) decision: NO console gate here, unlike
        // run_drag_translate above. This path fires only because the USER
        // physically pressed Ctrl+C twice; the re-issued synthetic Ctrl+C
        // duplicates a keystroke the user already sent voluntarily, so any
        // resulting SIGINT is user-initiated, not an AI-side hazard the
        // product is introducing. Gating would also be unreliable: focus may
        // legitimately have moved between the two keypresses and the click.
        if (!emebalachat::CopySelectionWithSequenceWait()) {
            DIAG_F("MAIN/DoubleCtrlC/001: clipboard copy not confirmed; refusing stale read\n");
            return;
        }
        std::wstring copied = emebalachat::GetClipboardText();
        if (copied.empty() || copied.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            return;
        }

        // V10F L2 (261122_0002 P2-G): the window the selection came from, for
        // the translation-history ring. Sampled after the confirmed capture;
        // the user may still switch windows during a slow inference (best
        // effort, documented in the unit report).
        const HWND dbl_target_hwnd = ::GetForegroundWindow();

        // Phase 3 Batch 2 (plan §2.5, REQ-007): double-Ctrl+C is a DRAG-context
        // translation (tooltip surface) - it uses the drag pair.
        // I4: snapshot read (REQ-R06: runs on the hook's async worker thread).
        // F3 (session 260908_0002): same whole-pair + effective-source pattern
        // as run_drag_translate above - this path ignored a pinned
        // drag_source_language exactly like the drag icon path did (V6).
        const auto snap = config.GetSnapshot();
        // 261111_0001 U6 (v0.10.7, R2-02/R2-04) + G-03 (fix-list round 2): this
        // value feeds ResolveEffectiveSource and then engine.Translate's SOURCE
        // slot (the engine prompt), so it must come from the GATED accessor -
        // Unknown -> "Auto Detect" (AUTO) - exactly like run_drag_translate and
        // the retranslate safety net above. The bare DetectLanguage call is a
        // governance breach even where today's labels happen to coincide:
        // garbage/unclassifiable text reached the prompt as "Unknown" instead of
        // the AUTO marker.
        std::string detected = emebalachat::DetectLanguageForPrompt(copied);
        const bool src_pinned = emebalachat::NormalizeLanguageCode(
                                    snap.drag_source_language) != "AUTO";
        std::string eff_src = emebalachat::ResolveEffectiveSource(
                                  snap.drag_source_language, detected);
        std::string src_code = emebalachat::NormalizeLanguageCode(eff_src);
        // F1 (session 260908_0003): AUTO engine source -> localized "Auto
        // Detect" tooltip tag (same empty-label seam as the other drag paths).
        const std::string tooltip_src = (src_code == "AUTO") ? std::string() : src_code;
        std::string tgt_lang = snap.drag_target_language;
        // F2: same provenance rule as run_drag_translate - a pinned drag
        // target (explicit user pick persisted across restarts) bypasses the
        // pivot; only the never-touched AUTO default remains pivotable.
        const bool tgt_user_explicit = snap.drag_target_pinned;

        bool pivot_fired = false;
        if (auto effective = emebalachat::ResolveEffectiveTarget(eff_src, tgt_lang,
                                                                 tgt_user_explicit)) {
            pivot_fired = true;
            tgt_lang = *effective;
            // F1 (session 260908_0002, ADR-A1-1): same local-temporary pivot
            // rule as the drag icon path above - this translation uses the
            // effective target, while config.json keeps the user's pinned
            // drag pair. The former RequestLanguageSync here (V1's second
            // entry point) persisted the ephemeral pivot from the hook's
            // REQ-R06 worker thread; removed so no automatic translation path
            // reaches SaveToFile(). persist=0 marks the pivot non-persisting.
            // fix-list-round15 R15-21 (F-3b): persisted_tgt/pivot_tgt are
            // language tokens (can be user-typed stored names) — the R14-06
            // class — so they log only behind diag::ContentLoggingEnabled();
            // ctx/path/persist ride both branches.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("STATE", "pivot_temporary ctx=drag path=dbl_ctrl_c persisted_tgt=%s pivot_tgt=%s persist=0",
                         snap.drag_target_language.c_str(), tgt_lang.c_str());
            } else {
                DIAG_LOG("STATE", "pivot_temporary ctx=drag path=dbl_ctrl_c persisted_tgt_len=%zu pivot_tgt_len=%zu persist=0",
                         snap.drag_target_language.size(), tgt_lang.size());
            }
        }
        // F3 DIAG (task §3-6): source decision + pivot evidence (see the
        // drag_src line above for the QA contract).
        // fix-list-round15 R15-21 (F-3b): persisted/detected/eff/tgt are
        // language tokens (can be user-typed stored names) — the R14-06
        // class — so they log only behind diag::ContentLoggingEnabled();
        // mode/pivot/usr_explicit ride both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE", "drag_src path=dbl_ctrl_c mode=%s persisted=%s detected=%s eff=%s pivot=%d tgt=%s usr_explicit=%d",
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.c_str(), detected.c_str(),
                     eff_src.c_str(), pivot_fired ? 1 : 0, tgt_lang.c_str(),
                     tgt_user_explicit ? 1 : 0);
        } else {
            DIAG_LOG("STATE", "drag_src path=dbl_ctrl_c mode=%s persisted_len=%zu detected_len=%zu eff_len=%zu pivot=%d tgt_len=%zu usr_explicit=%d",
                     src_pinned ? "pinned" : "detect",
                     snap.drag_source_language.size(), detected.size(),
                     eff_src.size(), pivot_fired ? 1 : 0, tgt_lang.size(),
                     tgt_user_explicit ? 1 : 0);
        }

        // PIPE-1 (session 261104_0001, handoff section 2.1): same smart-bypass
        // gate as the drag icon path above - the double-Ctrl+C entry point is
        // a drag-context translation and must filter URLs, pure numbers,
        // pure punctuation and emoji the same way. The pivot has already
        // fired, so same-language text still auto-pivots to the user's
        // native language (established drag UX preserved). Bypassed gestures
        // exit before the transient badge flip below (no label flicker).
        // R1-B (v0.10.7, 261111_0001 fix-list round 1): the drop is no longer
        // SILENT - the drag-icon path's reused notice (StringId::
        // TooltipNoSelection, the "click cannot produce a translation"
        // family; no dedicated StringId exists and src/i18n.* is owned by
        // another unit this session) is surfaced here too, stamped with THIS
        // request's generation so a newer drag still supersedes it. The
        // cursor anchor is captured BEFORE the gate so the notice carries the
        // same placement the translation card would have had.
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        if (!emebalachat::ShouldTranslate(copied, tgt_lang, "Auto Detect")) {
            // fix-list-round15 R15-21 (F-3b): detected/tgt are language
            // tokens (can be user-typed stored names) — the R14-06 class —
            // so they log only behind diag::ContentLoggingEnabled(); the
            // else keeps the shape-only lengths.
            if (diag::ContentLoggingEnabled()) {
                DIAG_LOG("STATE", "drag_bypass path=dbl_ctrl_c detected=%s tgt=%s",
                         detected.c_str(), tgt_lang.c_str());
            } else {
                DIAG_LOG("STATE", "drag_bypass path=dbl_ctrl_c detected_len=%zu tgt_len=%zu",
                         detected.size(), tgt_lang.size());
            }
            DIAG_F("MAIN/DoubleCtrlC/002: drag content filtered by the smart-bypass gate (URL/number/symbol/emoji or already in the pivoted target); engine not called\n");
            tooltip.ShowMessageThreadSafe(cursor.x + 12, cursor.y + 12,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection),
                                          gen);
            return;
        }

        // REQ-013: same transient badge flip as run_drag_translate - show the
        // DRAG pair while THIS translation runs, restore the TYPE pair on
        // flow end (live read at restore time; display-only). Placed after
        // every early return (feature gate, copy not confirmed, empty
        // clipboard) so aborted gestures never flicker; the remaining body
        // has a single exit, which this RAII guard restores.
        emebalachat::TransientDragPairBadge badge_transient(
            config, badge, emebalachat::ToUtf16(eff_src), emebalachat::ToUtf16(tgt_lang));

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        // REQ-047 P5-F2 (technical review 175600 finding #2): same strict-
        // failure surfacing as the other two drag-family paths — the 3-arg
        // overload used to discard the status, leaving a strict local failure
        // silent.
        // V10F (261122_0002 P2-F): the per-request task context wires this
        // path into the unified cancel registry (ctx.cancel) and the live
        // chunk-preview surface (lease-guarded); out_error_detail carries
        // THIS request's bounded cause - the racy process-global
        // OpenAiCompatibleClient::LastChatErrorDetail read after the call is
        // GONE (that slot could be cleared/overwritten by any concurrent
        // request between the call and the read).
        emebalachat::TranslationTaskContext dbl_ctx;
        dbl_ctx.cancel = &reading_jobs.dbl_cancel;
        std::string dbl_error_detail;
        dbl_ctx.out_error_detail = &dbl_error_detail;
        emebalachat::ChunkPreviewLease dbl_preview(reading_jobs, tooltip);
        dbl_ctx.on_chunk = [&dbl_preview](std::size_t, std::size_t total) {
            dbl_preview.TryBegin(static_cast<int>(total));
        };
        dbl_ctx.on_chunk_text = [&dbl_preview](const std::wstring& cumulative) {
            dbl_preview.Append(cumulative);
        };
        emebalachat::TranslationStatus dbl_status = emebalachat::TranslationStatus::Ok;
        std::wstring translated = engine.Translate(copied, eff_src, tgt_lang, &dbl_status, dbl_ctx);
        badge.SetStatus(emebalachat::BadgeStatus::Active);
        if (dbl_status == emebalachat::TranslationStatus::Canceled) {
            // V10F: an INTENTIONAL user cancel (Esc / badge click). No failure
            // notice for a cancel the user asked for; the preview lease dtor
            // retires the live card.
            DIAG_LOG("UI", "dbl_ctrl_c_canceled gen=%llu",
                     static_cast<unsigned long long>(gen));
            return;
        }
        // 261124_0001: same LocalModelLoadFailed routing as the Enter/drag/
        // retranslate seams (user_gguf keeps the file-presence route; the
        // others get the dedicated load-failed wording, no download offer).
        if (dbl_status == emebalachat::TranslationStatus::LocalModelLoadFailed) {
            // V10F: retire the live preview BEFORE the modal (End(false) hides
            // the window - it must not swallow what follows).
            dbl_preview.End(false);
            const auto load_failed_snap = config.GetSnapshot();
            if (load_failed_snap.engine_type == "user_gguf") {
                emebalachat::RequestModelMissingOfferOrModal(load_failed_snap.engine_type,
                                                             load_failed_snap.user_model_id);
            } else {
                emebalachat::RequestEngineUnavailableModal(
                    emebalachat::g_hControllerWnd,
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedTitle),
                    emebalachat::I18n::Get(emebalachat::StringId::ModelLoadFailedBody));
            }
            // R1-A (v0.10.7, 261111_0001): the modal IS the feedback and these
            // statuses always carry an empty translation, so STOP here.
            return;
        }
        if (dbl_status == emebalachat::TranslationStatus::LocalModelMissing) {
            // V10F E2 (261122_0002 P3-C4 wave B): one-click offer routing (the
            // Enter/drag/retranslate seams' policy).
            // V10F: retire the live preview BEFORE the offer (End(false) hides
            // the window - it must not swallow what follows).
            dbl_preview.End(false);
            // 261123_0001: fresh locked snapshot at failure time (engine_type
            // + user_model_id read together, I4) for the route classification.
            const auto miss_snap = config.GetSnapshot();
            emebalachat::RequestModelMissingOfferOrModal(miss_snap.engine_type,
                                                         miss_snap.user_model_id);
            // R1-A (v0.10.7, 261111_0001 fix-list round 1): mirror the drag-icon
            // path's terminal structure (run_drag_translate) - the modal IS the
            // feedback, and these statuses always carry an empty translation, so
            // STOP here. Falling through used to pass L"" to
            // ShowTranslationThreadSafe and render a permanent empty ghost card
            // (ShowTranslation kills the notice autohide timer and arms no hide).
            return;
        }
        if (dbl_status == emebalachat::TranslationStatus::CloudConsentBlocked) {
            // V10F: retire the live preview BEFORE the modal (End(false) hides
            // the window - it must not swallow the modal that follows).
            dbl_preview.End(false);
            // R1-A (v0.10.7, 261111_0001 fix-list round 1): mirror the drag-icon
            // path's terminal structure (run_drag_translate) - the modal IS the
            // feedback, and these statuses always carry an EMPTY translation, so
            // STOP here. Falling through used to pass L"" to
            // ShowTranslationThreadSafe and render a permanent empty ghost card
            // (ShowTranslation kills the notice autohide timer and arms no hide).
            emebalachat::RequestEngineUnavailableModal(
                emebalachat::g_hControllerWnd,
                emebalachat::I18n::Get(emebalachat::StringId::RepairFailedTitle),
                emebalachat::I18n::Get(emebalachat::StringId::RepairTransientBody));
            return;
        } else if (!translated.empty()) {
            // Success sentinel: resets the streak latch (same seam as Enter).
            emebalachat::RequestEngineUnavailableModal(emebalachat::g_hControllerWnd,
                                                       std::wstring(), std::wstring());
        } else {
            // R1-A: the empty-translation EngineFailed/timeout case has no modal
            // above; without this branch it fell through to
            // ShowTranslationThreadSafe(..., L"") (permanent empty ghost card).
            // Surface the same transient failure notice the drag-icon path uses
            // (StringId::TooltipTranslateFailed), plus the provider detail when
            // the failed leg is the OpenAI-compatible engine, then STOP.
            // V10F: the cause comes from THIS request's out_error_detail carrier
            // (see the ctx wiring above). Retire the live preview BEFORE the
            // notice (End(false) hides the window; the notice must survive).
            dbl_preview.End(false);
            std::wstring fail_body =
                emebalachat::I18n::Get(emebalachat::StringId::TooltipTranslateFailed);
            if (!dbl_error_detail.empty()) {
                fail_body += L"\n";
                fail_body += emebalachat::ToUtf16(dbl_error_detail);
            }
            tooltip.ShowMessageThreadSafe(cursor.x + 12, cursor.y + 12,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          fail_body,
                                          gen);
            return;
        }

        // V10F L3-B: freeze the live preview with the last cumulative text
        // BEFORE the final card replaces it (the P2-C contract order).
        dbl_preview.End(true);

        // REQ-R10-adjacent: worker thread, so marshal the D2D render to the GUI
        // thread instead of calling ShowTranslation() directly (audit §3.4).
        // R6 B1-H1: generation-guarded (a drag request stamped meanwhile makes
        // this result the stale one, and the tooltip drops it instead).
        // 261111_0001 U1: only the non-empty success result reaches this call -
        // every empty-result path above returns first (no empty ghost card).
        tooltip.ShowTranslationThreadSafe(
            cursor.x + 12, cursor.y + 12, copied, tooltip_src, tgt_lang, translated, gen);
        // V10F L2 (261122_0002 P2-G): record the successful double-Ctrl+C
        // translation in the translation-history ring (back empty - this path
        // never runs the verification round trip).
        emebalachat::translation_history::Push({ copied, translated, std::wstring(),
                                                 emebalachat::ToUtf16(src_code),
                                                 emebalachat::ToUtf16(tgt_lang),
                                                 dbl_target_hwnd });
    });

    // V10F L4-C (261122_0002 P2-D): the explicit selection-replace hotkey.
    // The hook compiles the combo from config.hotkey_selection_translate
    // (default "Ctrl+Alt+T" since V10F-R1 F-05, parsed by P2-D), swallows the keydown on the
    // hook thread ONLY while this callback is registered, and dispatches the
    // CURRENT FOREGROUND window here off-thread (DispatchHotkeyFx, the Input
    // P0-1 discipline). Posting the task is thread-safe (the pipeline's
    // single-slot accept); a busy pipeline drops the request with the same
    // silent policy as the Enter path's busy guard - the only feedback is
    // this shape-only diag line (no new StringId; see the unit report).
    hook.SetSelectionReplaceCallback([&worker](HWND target_hwnd) {
        if (!worker.PostSelectionReplaceTask(target_hwnd)) {
            DIAG_LOG("UI", "selection_replace_dropped: pipeline busy");
        }
    });

    // ESC Key Dismissal
    hook.SetEscCallback([&]() -> bool {
        // V10F FUP-02: while the language picker is modal, Esc belongs to the
        // picker - it cancels the invocation (PickerCommand::Cancel), and the
        // tooltip / drag icon / about surfaces behind the picker must stay.
        // Returning false leaves the key unconsumed so the picker's own modal
        // pump (and its focused EDIT) receives it.
        if (emebalachat::LanguagePickerShowing()) {
            DIAG_LOG("UI", "esc deferred: language picker owns the key");
            return false;
        }
        // V10F (261122_0002 P2-F): UNIFIED CANCEL FIRST. While ANY translation
        // is active - the Enter pipeline (worker.IsBusy) OR any of the four
        // reading workers (drag / retranslate / back-translate / double-Ctrl+C
        // via the registry) - Esc CANCELS THEM ALL and is consumed. The
        // cancel is cooperative: the tokens are polled at the engine's
        // between-chunk seams, so the in-flight request unwinds bounded by
        // one chunk/request round; a reading job's chunk-preview card retires
        // itself through the job's lease. Only an IDLE app falls through to
        // the dismiss chain below. This check MUST precede the tooltip
        // IsVisible arm: a live chunk-preview card IS a visible tooltip.
        if (worker.IsBusy() || emebalachat::AnyReadingJobRunning(reading_jobs)) {
            worker.RequestCancelTranslation();
            emebalachat::CancelAllReadingJobs(reading_jobs);
            DIAG_LOG("UI", "esc unified cancel: pipeline=%d drag=%d retranslate=%d back=%d dbl=%d",
                     worker.IsBusy() ? 1 : 0,
                     emebalachat::ReadingJobRunning(reading_jobs.drag_running) ? 1 : 0,
                     emebalachat::ReadingJobRunning(reading_jobs.retranslate_running) ? 1 : 0,
                     emebalachat::ReadingJobRunning(reading_jobs.back_running) ? 1 : 0,
                     emebalachat::ReadingJobRunning(reading_jobs.dbl_running) ? 1 : 0);
            return true;
        }
        if (tooltip.IsVisible()) {
            tooltip.Dismiss();
            return true;
        }
        if (drag_icon.IsVisible()) {
            drag_icon.Hide();
            return true;
        }
        if (about_window.IsVisible()) {
            about_window.Dismiss();
            return true;
        }
        return false;
    });

    // Tooltip language switcher: THE reported B3 bug site. Previously this
    // callback re-translated + re-showed the tooltip ONLY - config, badge and
    // tray kept the old target and drifted forever (architect plan §1 B3).
    // R6 Phase 1: the menu pick is now a REQUEST to the single coordinator
    // (INV-2): persist -> badge -> tray -> tooltip label sync, THEN the local
    // re-translation with the NEW target passed explicitly (INV-5), dispatched
    // to the F9 retranslate_worker so the GUI thread never blocks (ADR-A1-5).
    tooltip.SetLanguageChangeCallback([&](std::string_view new_target_lang) {
        // R6 Phase 2 (B1-H1): the language change is itself a NEW translate
        // request - stamp it so an in-flight older drag result cannot
        // overwrite the re-translation when it lands.
        const uint64_t gen = tooltip.BeginTranslationRequest();

        std::wstring src = tooltip.GetSourceText();
        std::string src_code = tooltip.GetSourceLangCode();
        // V10F FUP-02: a palette pick can dispatch after a mid-session
        // dismissal emptied the card state (TeardownCard clears both fields);
        // the snapshot latched at picker-open time is then the fallback that
        // keeps this retranslate fed. Live non-empty state always wins, so a
        // card refreshed while the picker was open still retranslates its
        // newest content.
        if (src.empty()) {
            if (auto pick_snap = tooltip.GetLanguagePickerSourceSnapshot();
                pick_snap.valid) {
                src = std::move(pick_snap.source_text);
                src_code = std::move(pick_snap.source_lang_code);
            }
        }
        std::string new_tgt = std::string(new_target_lang);

        RECT r = {};
        ::GetWindowRect(tooltip.GetHwnd(), &r);

        // Runs on the GUI thread (tooltip WndProc) - the coordinator's
        // required thread per plan §2.3. If the request is refused (invalid
        // name), everything stays consistent at the OLD pair; the worker
        // re-translate below then just re-renders the current state.
        // Phase 3 Batch 2 (plan §2.4, REQ-008/009): the tooltip language menu
        // belongs to the drag-result window, so it mutates the DRAG pair and
        // refreshes only the tooltip (sticky per-plan §2.2 model).
        ApplyLanguageChange(emebalachat::LanguageContext::Drag, std::string_view{},
                            new_tgt, false, false);

        // F9 (ADR-A1-5): engine.Translate is OFF the GUI thread now. It used
        // to run HERE, inside the tooltip WndProc, and held the engine mutex
        // for the whole local-LLM inference - V8 measured 1.26-5.36 s
        // message-pump freezes (gen 21 crossed the 5 s "Not Responding"
        // threshold). The job carries the src/src_code captured above plus
        // this request's fresh gen; the worker translates and delivers via
        // ShowTranslationThreadSafe, where the B1-H1 guard drops a result
        // superseded while inference ran. Latest-wins on the single pending
        // slot coalesces only between consecutive LANGUAGE PICKS on this
        // queue - drag clicks live in the separate drag_translate_worker
        // queue and can never swallow a user-requested re-translation.
        // fix-list-round15 R15-21 (F-3b): tgt is a language token (can be a
        // user-typed stored name) — the R14-06 class — so it logs only
        // behind diag::ContentLoggingEnabled(); gen rides both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("UI", "retranslate_enqueued gen=%llu tgt=%s",
                     static_cast<unsigned long long>(gen), new_tgt.c_str());
        } else {
            DIAG_LOG("UI", "retranslate_enqueued gen=%llu tgt_len=%zu",
                     static_cast<unsigned long long>(gen), new_tgt.size());
        }
        // V10F RISK-04 Option C (261122_0002): a fresh language pick supersedes
        // an in-flight re-translation through the unified cancel token (the
        // old run unwinds at the next between-chunk poll instead of burning
        // inference whose render the B1-H1 guard would drop anyway).
        if (emebalachat::ReadingJobRunning(reading_jobs.retranslate_running)) {
            reading_jobs.retranslate_cancel.store(true, std::memory_order_release);
        }
        retranslate_worker.Submit(RetranslateJob{ src, src_code, new_tgt,
                                                  r.left, r.top, gen });
    });

    // F8 (ADR-A1-4): source-language dropdown on the tooltip header. A pick is
    // a REQUEST to the single coordinator: ApplyLanguageChange persists the
    // drag_source_language (an AUTO pick re-records "Auto Detect" - the
    // re-record contract, user decision ④) and refreshes the tray check marks.
    // Re-translation then runs on the F9 retranslate_worker above (ADR-A1-5:
    // NEVER a synchronous engine.Translate on the GUI thread - V8 freeze
    // regression guard). The worker resolves eff_src from the NEW persisted
    // value - pinned wins over the carried detection code, AUTO falls back to
    // it (F3-3) - and its ADR-A1-7 pivot guard re-checks eff_src == tgt, so
    // the KO->KO corruption path stays closed for source picks too.
    tooltip.SetSourceLanguageChangeCallback([&](std::string_view new_source_lang) {
        // The source change is itself a NEW translate request (B1-H1 stamp):
        // an in-flight older drag/retranslate result must not overwrite this
        // re-translation when it lands.
        const uint64_t gen = tooltip.BeginTranslationRequest();

        std::wstring src = tooltip.GetSourceText();
        std::string src_code = tooltip.GetSourceLangCode(); // AUTO fallback only
        // V10F FUP-02: same empty-state fallback as the target-pick callback
        // above - the picker-open snapshot keeps a wiped card's retranslate
        // fed. Live non-empty state always wins.
        if (src.empty()) {
            if (auto pick_snap = tooltip.GetLanguagePickerSourceSnapshot();
                pick_snap.valid) {
                src = std::move(pick_snap.source_text);
                src_code = std::move(pick_snap.source_lang_code);
            }
        }
        // V10F FUP-03: an "Auto Detect" source pick must DROP the carried
        // detection code - ResolveEffectiveSource("Auto Detect", "KO") returns
        // "KO" verbatim (config.cpp), so the stale carried code would feed the
        // engine the old source and the run_retranslate empty-path re-detect
        // would never run. A concrete pick keeps the carry (F3-3).
        src_code = emebalachat::CarriedSourceCodeForSourcePick(src_code,
                                                               new_source_lang);
        // Keep the currently SHOWN effective target (the drag pair's target
        // may have been pivoted - the tooltip re-shows at the same effective
        // target it is displaying right now).
        std::string tgt_lang = tooltip.GetTargetLang();

        RECT r = {};
        ::GetWindowRect(tooltip.GetHwnd(), &r);

        // Runs on the GUI thread (tooltip WndProc) - the coordinator's
        // required thread (plan §2.3). Only EXPLICIT tooltip picks reach
        // persistence here (C2: no translation path calls this coordinator).
        const bool applied = ApplyLanguageChange(
            emebalachat::LanguageContext::Drag, new_source_lang, std::string_view{},
            false, false);
        // F8 DIAG: menu pick + whether the coordinator accepted it (a
        // refused request leaves config untouched and re-renders current
        // state, exactly like the target-menu callback above).
        // fix-list-round15 R15-21 (F-3b): lang is a language token (can be a
        // user-typed stored name) — the R14-06 class — so it logs only
        // behind diag::ContentLoggingEnabled(); applied/gen ride both
        // branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("STATE", "tooltip_src lang=%s applied=%d gen=%llu",
                     std::string(new_source_lang).c_str(), applied ? 1 : 0,
                     static_cast<unsigned long long>(gen));
        } else {
            DIAG_LOG("STATE", "tooltip_src lang_len=%zu applied=%d gen=%llu",
                     new_source_lang.size(), applied ? 1 : 0,
                     static_cast<unsigned long long>(gen));
        }

        // fix-list-round15 R15-21 (F-3b): pick/tgt are language tokens (can
        // be user-typed stored names) — the R14-06 class — so they log only
        // behind diag::ContentLoggingEnabled(); gen rides both branches.
        if (diag::ContentLoggingEnabled()) {
            DIAG_LOG("UI", "retranslate_enqueued_src gen=%llu pick=%s tgt=%s",
                     static_cast<unsigned long long>(gen),
                     std::string(new_source_lang).c_str(), tgt_lang.c_str());
        } else {
            DIAG_LOG("UI", "retranslate_enqueued_src gen=%llu pick_len=%zu tgt_len=%zu",
                     static_cast<unsigned long long>(gen),
                     new_source_lang.size(), tgt_lang.size());
        }
        // V10F RISK-04 Option C: same-queue supersede (see the target-pick site
        // above for the rationale).
        if (emebalachat::ReadingJobRunning(reading_jobs.retranslate_running)) {
            reading_jobs.retranslate_cancel.store(true, std::memory_order_release);
        }
        retranslate_worker.Submit(RetranslateJob{ src, src_code, tgt_lang,
                                                  r.left, r.top, gen });
    });

    // REQ-R08 visual feedback: localized state-change bubble at the cursor,
    // audio chime comes from SetActive() itself. Runs on the hook thread
    // (Win+F9 path) or the main thread (badge/tray paths) - the thread-safe
    // tooltip seam marshals to the GUI thread either way.
    hook.SetActiveChangeCallback([&](bool active) {
        // REQ-R07 (audit §3.1): 1:1 sync. This lambda is the ONLY place that
        // touches mouse_hook.SetEnabled for activation state, and it fires on
        // every real SetActive() change - including the Win+F9 hotkey path that
        // previously left the mouse hook permanently disabled.
        mouse_hook.SetEnabled(active);

        // REQ-054: the drag icon is only SHOWN while active, but nothing hid
        // it when F9 paused the app — leaving a clickable translate affordance
        // on screen while paused. Hide() is thread-safe from any thread
        // (kHideMessage marshaling, drag_icon.hpp).
        if (!active) {
            drag_icon.Hide();
        }

        POINT cursor = {};
        ::GetCursorPos(&cursor);
        // W5 (R1-22): label the combo the hook was actually compiled with, not
        // a hardcoded "F9". Resolved from config.hotkey_toggle through the same
        // pure seam Start() uses (hotkey_* is a startup-only field, so this
        // read needs no lock); an unmapped-to-display spec can only come from a
        // broken config, and ResolveToggleFromConfig never returns an invalid
        // one, but the default-spec fallback keeps the bubble well-formed.
        std::wstring hotkey_label = emebalachat::KeyboardHook::HotkeySpecToDisplayString(
            emebalachat::KeyboardHook::ResolveToggleFromConfig(config.hotkey_toggle));
        if (hotkey_label.empty()) {
            hotkey_label = emebalachat::KeyboardHook::HotkeySpecToDisplayString(
                emebalachat::KeyboardHook::kDefaultToggleHotkey);
        }
        const std::wstring body = std::wstring(active
                ? emebalachat::I18n::Get(emebalachat::StringId::BadgeActive)
                : emebalachat::I18n::Get(emebalachat::StringId::BadgePaused))
            + L" (" + hotkey_label + L")";
        tooltip.ShowMessageThreadSafe(cursor.x + 12, cursor.y + 12,
                                      hotkey_label, body);
    });

    // ---- V10F P3-C2 (UX-3 Option A, wave B): the badge 3-zone wiring --------
    // Wired once after badge.Create (the C2 cross-unit contract). The SRC/TGT
    // zone clicks open the SHARED language picker on the TYPE pair through the
    // SAME deferred-open seam the tray's "검색…" rows use (the picker module's
    // per-surface latch + the controller's kMsgOpenLanguagePalette drain own
    // the modal loop + the single-instance guard, so a badge-opened picker
    // behaves byte-identically to a tray-opened one); the swap arrow runs the
    // EXISTING swap verb (PlanLanguageSwap + ApplyLanguageChange, persist +
    // badge/tray/tooltip refresh + chime all inside the coordinator), which
    // is ALSO the badge double-click's verb, so both affordances can never
    // drift. SetSwapRequestCallback is the ONLY swap executor when set (the
    // zone callback is never handed zone 1), so wiring both cannot swap twice.
    badge.SetZoneLanguageClickCallback([&](int zone) {
        if (zone != 0 && zone != 2) {
            // Zone 1 (the arrow) arrives here ONLY when no swap callback is
            // wired - ours is, so this is the unreachable defensive arm; run
            // the swap verb anyway rather than dropping the click.
            if (zone == 1) trayCallbacks.on_swap_languages();
            return;
        }
        // Mirror the tray's typing-picker request verbatim (tray.cpp
        // typing_src_req/typing_tgt_req): the same surface/role the tray's
        // "검색…" rows latch, the active engine's registry view, and the
        // normalized persisted pair as the preselection anchors. The commit
        // dispatches through the controller drain into ApplyLanguageChange
        // (LanguageContext::Type), so the badge path and the tray path are
        // ONE code path end to end.
        const auto snap = config.GetSnapshot();
        emebalachat::language_palette::PickerInit req;
        req.surface = emebalachat::language_palette::PickerSurface::TrayTyping;
        req.role = (zone == 0) ? emebalachat::language_palette::PickerRole::Source
                               : emebalachat::language_palette::PickerRole::Target;
        req.engine = emebalachat::TrayLanguageEngineFor(
            emebalachat::TrayPreferredEngineIndex(snap.engine_type));
        req.source_code = emebalachat::NormalizeLanguageCode(snap.type_source_language);
        req.target_code = emebalachat::NormalizeLanguageCode(snap.type_target_language);
        ::SetLastError(ERROR_SUCCESS);
        if (!emebalachat::PostDeferredLanguagePaletteOpen(emebalachat::g_hControllerWnd,
                                                          req)) {
            const DWORD post_gle = ::GetLastError();
            if (post_gle != ERROR_SUCCESS) {
                DIAG_F("MAIN/BadgeZone/001: deferred language-picker open post failed (GLE %lu)\n",
                       post_gle);
            } else {
                DIAG_F("MAIN/BadgeZone/002: deferred language-picker open not posted (coalesced)\n");
            }
        }
    });
    badge.SetSwapRequestCallback([&]() {
        // The existing tray Swap verb: same planner, same coordinator, same
        // persist + chime; ApplyLanguageChange's surface refresh re-pushes the
        // badge label (and the new AUTO-source "" contract rides the FUP-03
        // coordinator path, so the pill never shows a stale code).
        trayCallbacks.on_swap_languages();
    });
    // The engine mini-tag chip click opens the tray context menu at the badge
    // (the same surface the badge right-click already summons): engine
    // switching, recent languages and the settings container all stay one
    // click away from the chip.
    badge.SetTagClickCallback([&]() {
        tray.ShowContextMenu();
    });

    // ---- V10F P3-C3 (UX-2, wave B): the settings hub action set -------------
    // SetHubActions runs ONCE at startup (the g_open_* contract: the hub
    // deep-copies the set). Every action reuses an EXISTING entry point - the
    // deferred dialog opens, the cheat-sheet lambda, the About update check,
    // the system-defaults reset - so the hub surfaces and their pre-hub
    // equivalents can never drift. The half-wired-build gate (SyncActionGates)
    // greys exactly the control whose callback is empty; this set fills all
    // of them. The lambdas capture wWinMain locals by reference and the hub
    // window is opened with the CONTROLLER as owner, so a live hub at exit is
    // destroyed with the controller BEFORE those locals die (the C3 risk
    // note's owner contract).
    emebalachat::SetHubActions({
        // Engines tab.
        /*open_openai=*/open_openai_settings_deferred,
        /*open_gguf_manager=*/
        [&]() {
            // Direct call, not a post: the hub is a modeless dialog, not a
            // TrackPopupMenuEx, so the REQ-047 D3 teardown deferral does not
            // apply; the lambda opens the modal on the controller + refreshes.
            if (emebalachat::g_open_gguf_model_manager) {
                emebalachat::g_open_gguf_model_manager();
            }
        },
        /*open_model_download=*/
        [&]() {
            // The existing consent + progress flow (REQ-MD D9); the hub is
            // modeless so a modal parented to the controller is legal.
            emebalachat::OfferModelDownload(emebalachat::g_hControllerWnd);
        },
        // Languages tab.
        /*open_user_language=*/
        [&]() {
            if (emebalachat::g_open_user_lang_add) {
                emebalachat::g_open_user_lang_add();
            }
        },
        /*open_user_language_manager=*/
        [&]() {
            if (emebalachat::g_open_user_lang_manager) {
                emebalachat::g_open_user_lang_manager();
            }
        },
        // Advanced tab.
        /*open_cheat_sheet=*/show_cheat_sheet,
        /*check_updates=*/
        [&about_window]() {
            // The About window owns the whole update flow (job slot, zone
            // states, consent + download); its StartUpdateCheck is private
            // and reachable only from the card's own [업데이트 확인…] zone, so
            // the existing PUBLIC entry point is to open the card centered at
            // the cursor - the same surface the tray "소개" row summons. The
            // check itself starts on the zone click (documented in the unit
            // report; a public RequestUpdateCheck on AboutWindow would let a
            // follow-up start the check directly).
            POINT cursor = {};
            ::GetCursorPos(&cursor);
            about_window.Show(cursor.x, cursor.y);
        },
        /*reset_defaults=*/apply_system_defaults,
        // The additive seams (the C3 wiring contract).
        /*open_ui_language_picker=*/
        [&config]() {
            // The shared 검색… picker in its UI-language mode: the same
            // TrayUiLanguage request shape the tray's own row latches
            // (surface = mode discriminator, role inert, engine Auto, the
            // canonical persisted locale spelling - "auto" routed through
            // StringToLocale's auto branch, never the raw empty string).
            const auto snap = config.GetSnapshot();
            const bool auto_checked = (snap.ui_language == "auto" ||
                                       snap.ui_language.empty());
            emebalachat::language_palette::PickerInit req;
            req.surface = emebalachat::language_palette::PickerSurface::TrayUiLanguage;
            req.role = emebalachat::language_palette::PickerRole::Source;
            req.engine = emebalachat::LanguageEngine::Auto;
            req.source_code = emebalachat::I18n::LocaleToString(
                emebalachat::I18n::StringToLocale(auto_checked ? "auto"
                                                               : snap.ui_language));
            req.target_code.clear();
            ::SetLastError(ERROR_SUCCESS);
            if (!emebalachat::PostDeferredLanguagePaletteOpen(
                    emebalachat::g_hControllerWnd, req)) {
                const DWORD post_gle = ::GetLastError();
                if (post_gle != ERROR_SUCCESS) {
                    DIAG_F("MAIN/HubPicker/001: deferred UI-language picker post failed (GLE %lu)\n",
                           post_gle);
                }
            }
        },
        /*change_typing_pair=*/
        [&config]() {
            const auto snap = config.GetSnapshot();
            emebalachat::language_palette::PickerInit req;
            req.surface = emebalachat::language_palette::PickerSurface::TrayTyping;
            req.role = emebalachat::language_palette::PickerRole::Source; // Tab flips to target
            req.engine = emebalachat::TrayLanguageEngineFor(
                emebalachat::TrayPreferredEngineIndex(snap.engine_type));
            req.source_code = emebalachat::NormalizeLanguageCode(snap.type_source_language);
            req.target_code = emebalachat::NormalizeLanguageCode(snap.type_target_language);
            ::SetLastError(ERROR_SUCCESS);
            if (!emebalachat::PostDeferredLanguagePaletteOpen(
                    emebalachat::g_hControllerWnd, req)) {
                const DWORD post_gle = ::GetLastError();
                if (post_gle != ERROR_SUCCESS) {
                    DIAG_F("MAIN/HubPicker/002: deferred typing-pair picker post failed (GLE %lu)\n",
                           post_gle);
                }
            }
        },
        /*change_drag_pair=*/
        [&config]() {
            const auto snap = config.GetSnapshot();
            emebalachat::language_palette::PickerInit req;
            req.surface = emebalachat::language_palette::PickerSurface::TrayTooltip;
            req.role = emebalachat::language_palette::PickerRole::Source; // Tab flips to target
            req.engine = emebalachat::TrayLanguageEngineFor(
                emebalachat::TrayPreferredEngineIndex(snap.engine_type));
            req.source_code = emebalachat::NormalizeLanguageCode(snap.drag_source_language);
            req.target_code = emebalachat::NormalizeLanguageCode(snap.drag_target_language);
            ::SetLastError(ERROR_SUCCESS);
            if (!emebalachat::PostDeferredLanguagePaletteOpen(
                    emebalachat::g_hControllerWnd, req)) {
                const DWORD post_gle = ::GetLastError();
                if (post_gle != ERROR_SUCCESS) {
                    DIAG_F("MAIN/HubPicker/003: deferred drag-pair picker post failed (GLE %lu)\n",
                           post_gle);
                }
            }
        },
        /*set_badge_visible=*/
        [&badge, &refresh_tray](bool visible) {
            // The tray on_toggle_badge idiom minus the flip: runtime-only
            // visibility (never persisted - the tray toggle's contract), with
            // the SC-02 refresh so the tray check mark follows immediately.
            badge.SetVisible(visible);
            refresh_tray();
        },
        /*apply_extra=*/
        [&config, &engine](const emebalachat::settings_hub::HubExtraSettings& extra) {
            // The fields the hub deliberately does NOT write itself. The
            // hotkey_* strings restart-apply (the hook compiles them at
            // Start()); the sampling values and the cloud-fallback switch take
            // effect NOW through the same engine pushers the startup path uses
            // (engine.cpp applies them under its own mutex). V10F-R1 F-02: the
            // config fields are written through the LOCKED verbs (the BUG-08
            // class — an off-thread SaveToFile serialises them under mutex_, so
            // the direct assignments this callback used to make could interleave
            // with that iteration); drag_to_translate / cloud_fallback_enabled
            // keep their established write discipline (snapshot-pair atomic /
            // startup-gated single writer).
            config.SetHotkeyToggle(extra.hotkey_toggle);
            config.SetHotkeyLang(extra.hotkey_lang);
            config.SetHotkeyMode(extra.hotkey_mode);
            config.SetHotkeySelectionTranslate(extra.hotkey_selection_translate);
            config.SetSamplingParams(extra.temperature, extra.top_p, extra.top_k,
                                     extra.repetition_penalty);
            config.drag_to_translate = extra.drag_to_translate;
            config.cloud_fallback_enabled = extra.cloud_fallback_enabled;
            engine.SetSamplingParams(extra.temperature, extra.top_p, extra.top_k,
                                     extra.repetition_penalty);
            engine.SetCloudFallbackEnabled(extra.cloud_fallback_enabled);
        },
        /*after_apply=*/
        [&](const emebalachat::settings_hub::HubApplyResult& result) {
            // Re-push exactly the surfaces that moved (the C3 contract). The
            // hub already persisted + wrote the config; this is view-only.
            if (result.theme_changed) {
                // Reuses the shared theme coordinator: ui::SetActiveTheme +
                // badge/tray/tooltip/about repaint + drag-icon re-show. The
                // config already carries the normalized theme, so this is an
                // apply of the persisted value, not a second decision.
                apply_theme_change(config.GetSnapshot().ui_theme);
            }
            if (result.engine_host_changed) {
                // SetEngineHostConfig -> RefreshActiveEngine -> warmup
                // re-evaluation, the g_open_engine_host_settings OK-path push.
                engine.SetEngineHostConfig(config.engine_host);
            }
            // The hub wrote auto_send/back_translation/sound directly: the
            // SC-02 refresh re-pushes the tray (incl. the back-translate check)
            // and the badge auto-send chip in one place.
            refresh_tray();
            if (!config.back_translation_enabled.load(std::memory_order_relaxed)) {
                tooltip.DismissThreadSafe();
            }
        },
    });

    // Badge Mouse Controls:
    // Single Click: Cancel an in-flight translation, otherwise Toggle Active / Paused
    badge.SetClickCallback([&]() {
        // 261121_0002 (T6, design-pick.md "main.cpp") + V10F (261122_0002
        // P2-F): while ANY translation is in flight the single click CANCELS
        // it - the Enter pipeline (worker.IsBusy) AND the four reading
        // workers (drag / retranslate / back-translate / double-Ctrl+C via
        // the unified registry). The cancel tokens are latched (RELEASE
        // stores) and polled at the engine's between-chunk seams, so the
        // tasks unwind bounded by one chunk/request round; while idle the
        // click keeps the legacy pause toggle. The click still arrives
        // through the kTimerSingleClick deferral, so the cancel lands after
        // the OS double-click window - by design, a double-click (swap
        // languages) must never be swallowed by a cancel.
        if (worker.IsBusy() || emebalachat::AnyReadingJobRunning(reading_jobs)) {
            worker.RequestCancelTranslation();
            emebalachat::CancelAllReadingJobs(reading_jobs);
            return;
        }
        hook.ToggleActive();
    });

    // Double Click: Swap Source ⇄ Target
    badge.SetDoubleClickCallback([&]() {
        trayCallbacks.on_swap_languages();
    });

    // Right Click: Directly popup the full Settings Menu right at the badge!
    badge.SetRightClickCallback([&]() {
        tray.ShowContextMenu();
    });

    // 9. Start Pipeline Worker, Keyboard Hook, and Mouse Hook
    worker.Start();
    const bool hook_started = hook.Start();
    const bool mouse_started = mouse_hook.Start();
    // 260905 diagnostics: subsystem startup state recorded right after the
    // hooks are (or are not) installed — a dead hook is the first thing to
    // rule out for "nothing translates" reports.
    DIAG_LOG("SESSION", "subsystems started worker=1 hook=%s mouse=%s active=%d toggle_vk=0x%02X",
             hook_started ? "ok" : "FAILED",
             mouse_started ? "ok" : "FAILED",
             hook.IsActive() ? 1 : 0,
             hook.ToggleHotkey().vk);

    // REQ-R14 watchdog: re-verify hook thread liveness every 30 s (covers the
    // UAC secure-desktop trip and any silent OS removal path that emitted no
    // resume/unlock message to this session).
    ::SetTimer(hController, emebalachat::kTimerHookHealth,
               emebalachat::kHookHealthWatchdogMs, nullptr);

    // 10. Startup sound
    emebalachat::PlayToggleOn();

    // 11. Main Message Loop
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // 12. Clean Shutdown
    //
    // REQ-R16 (audit §5 latent item 4): deterministic, cancellation-first
    // teardown. Order matters:
    //  a) engine.RequestCancel() latches the shutdown flag. REQ-043 (M6 T5):
    //     with the embedded engine gone there is no llama decode/warmup to
    //     unwind — the latch now short-circuits the surviving engines: an
    //     in-flight or queued cloud WinHTTP request and an in-flight
    //     engine-host pipe request report Canceled instead of starting work
    //     that could transmit user text after an exit intent, and every
    //     subsequent Translate() short-circuits.
    //  b) WaitInferenceIdle gives the bounded (2 s) confirmation that no
    //     Translate() is still in flight (the engine mutex is held across the
    //     whole cloud/host request) before the manager is destroyed.
    engine.RequestCancel();
    // V10F BUG-09/RISK-01: the pipeline worker can hold a queued or in-flight
    // Enter translation that the engine latch alone does not unwind promptly -
    // RequestCancelTranslation flips its per-task cancel (GUI-thread safe,
    // RELEASE store) so the task ends Canceled instead of starting engine
    // work that could transmit user text after the exit intent.
    worker.RequestCancelTranslation();
    // fix-list-round15 R18 (unit F-4): latch the probe service's shutdown
    // intent HERE - at the start of the sequence, before the language-sync
    // drain below, whose accepted-plan path calls
    // probe_service.MaybeTriggerForPick (:2513). Without this latch a pick
    // drained there STARTS a probe after the cancel latch above; its Translate
    // short-circuits to Canceled+empty, the transport maps that to {}, the fold
    // reads Unsupported(reason=empty) and the locked verb PERSISTS the false
    // verdict - suppressing that language's first-use trigger until a manual
    // retest. Both spawn seams decline while the intent is set, so the drain
    // can produce no new work at all; the bounded join below still covers every
    // probe started BEFORE this point, so its exhaustive claim holds a fortiori.
    probe_service.BeginShutdown();
    mouse_hook.Stop();
    hook.Stop();
    worker.Stop();
    // V10F BUG-09/RISK-01: the tray icon and the on-disk config are retired
    // BEFORE the bounded worker joins below. Rationale: (a) a tray click/menu
    // pick in the join window could dispatch new work or a config write while
    // the teardown is bounded-waiting - Destroy() removes the notify icon so
    // the interaction surface is gone (the later language-sync drain's
    // refresh_tray() call is benign: SystemTray::UpdateStatus's NIM_MODIFY
    // simply fails on the deleted icon); (b) if a bounded join escalates to
    // the TerminateProcess backstop (probe_service.ShutdownJoinWorkers, the
    // last bounded join below), everything after this point would never run -
    // persisting config HERE guarantees the session's writes survive that
    // escalation. Every later mutation path still self-persists (the drain's
    // ApplyLanguageChange and the probe recorded callback both call
    // SaveToFile themselves), so nothing is lost by the early write.
    tray.Destroy();
    config.SaveToFile();
    // R6 Phase 3 (audit item 7): join the drag-translate worker here, BEFORE
    // the language-sync queue drain below (no producer can post a
    // kMsgApplyLanguageSync payload after this point) and before any surface
    // Destroy(). Bounded: engine.RequestCancel() makes an in-flight decode
    // unwind at the next token boundary (REQ-R16), and a clipboard settle is
    // <= ~200 ms. Warmup-thread join precedent ~30 lines below.
    drag_translate_worker.Stop(); // request_stop() + cv.notify_all()
    drag_translate_worker.Join(); // if (joinable()) join()
    // F9 (ADR-A1-5): same shutdown discipline for the re-translate worker -
    // stop + notify + join right here, so it is down before the surfaces it
    // marshals to are Destroy()ed. Bounded the same way: engine.RequestCancel()
    // (latched above) makes an in-flight decode unwind at the next token
    // boundary, and a superseded/empty result renders nothing anyway.
    retranslate_worker.Stop(); // request_stop() + cv.notify_all()
    retranslate_worker.Join(); // if (joinable()) join()
    // 261108_0002 (WS3): the back-translation worker owns the SAME engine
    // manager and marshals into the SAME tooltip, so it is torn down at this
    // identical position - after every producer is down (worker.Stop() above
    // means the pipeline can no longer fire the callback) and before any
    // surface Destroy(). Bounded like the two joins above: engine.RequestCancel()
    // (latched at the top of this block) short-circuits an in-flight or queued
    // round trip, and an empty/canceled result renders nothing.
    back_translate_worker.Stop(); // request_stop() + cv.notify_all()
    back_translate_worker.Join(); // if (joinable()) join()
    // R6 Phase 1 (B3): drain any language-sync requests still queued before
    // the coordinator is retired, so a cycle posted a moment before shutdown
    // still persists instead of being silently dropped. SEC-B5: the payloads
    // now live in g_lang_sync_queue (the message is a parameter-less wake-up,
    // so no PeekMessageW sweep exists anymore); the same-thread GUI deque
    // drain at the IDENTICAL old position (every language-sync producer joined
    // above, coordinator retired below) preserves the shutdown ordering:
    // producers joined -> drain -> retire. The probe service produces nothing
    // for THIS queue, but this loop is itself a probe PRODUCER: the
    // coordinator it calls reaches probe_service.MaybeTriggerForPick (:2513),
    // so the probe-worker join has to come AFTER this loop (R15-20, below).
    // Any kMsgApplyLanguageSync notification still sitting in the message
    // queue is harmless (carries no payload).
    for (const emebalachat::LanguageSyncRequest& req : emebalachat::DrainLanguageSyncQueue()) {
        if (emebalachat::g_apply_language_change) {
            emebalachat::g_apply_language_change(
                req.context, std::string_view{ req.source },
                std::string_view{ req.target }, req.cycle, req.play_chime);
        }
    }
    // fix-list-round15 R15-11 + R15-20: the probe workers own JOINABLE threads
    // (never detached), so they join here - at the tail of the shutdown drain,
    // which is what makes the join exhaustive. The language-sync drain just
    // above re-runs the coordinator, whose accepted-plan path calls
    // probe_service.MaybeTriggerForPick (:2513), and the service's own retired
    // latch flips only after the completion drain below - so that loop can
    // START a probe. The R15-20 finding: the join used to sit BEFORE the loop,
    // leaving a drain-spawned worker unjoined while the comments claimed
    // otherwise. After the move it covers that probe too, while still running
    // after engine.RequestCancel() (latched above) unwinds any in-flight probe
    // Translate and before the completion drain, the coordinator retires,
    // WaitInferenceIdle and every engine-teardown step. Bounded like the
    // WaitInferenceIdle confirmation below: a cancel-ignoring request that
    // outlives the bound escalates to the safe terminal state inside
    // ShutdownJoinWorkers (TerminateProcess - the engine is never destroyed
    // under the live worker). No deadlock in the new position: the bounded wait
    // blocks on the workers' own completion signal only, and the drain's spawn
    // path returns right after StartProbe (it never waits on a probe). Past
    // this point every started probe - including one the drain just spawned -
    // has queued its outcome, so the in-flight set drains exact below.
    probe_service.ShutdownJoinWorkers(
        std::chrono::milliseconds(emebalachat::kUserLanguageProbeShutdownJoinBoundMs));
    // 261116_0003 P9b-3 U3 + fix-list-round15 R15-11/R15-20: drain probe
    // completions at the identical position. The worker join immediately above
    // makes this drain EXHAUSTIVE (no probe thread can still be inside
    // Translate): it runs AFTER the language-sync drain, so a probe that drain
    // just spawned through MaybeTriggerForPick (:2513) is joined too. Each
    // queued completion folds + records through the locked verb and frees its
    // in-flight slot, so the in-flight bookkeeping ends exact. A completion
    // that somehow still lands after this drain stays queued and is dropped by
    // Retire(), never applied after teardown.
    probe_service.DrainOutcomes();
    // Retire the probe drain with the coordinators below so a late
    // kMsgUserLanguageProbeDone can never call into destroyed state.
    emebalachat::g_drain_user_language_probes = nullptr;
    probe_service.Retire();
    // Retire the coordinator BEFORE the surfaces it writes to are destroyed.
    // The double-Ctrl+C worker (joined by hook.Stop() above) and the drag
    // worker (joined immediately before this block, R6 Phase 3) can no longer
    // post sync requests; the hook thread itself is down. Once unset,
    // ControllerWndProc discards any still-queued requests (pop without
    // apply) without touching config/engine/badge/tray/tooltip.
    emebalachat::g_apply_language_change = nullptr;
    // 261119_0001 P6-U5: retire the UI-language seam with it, so a late picker
    // commit (kMsgOpenLanguagePalette) can never call into destroyed
    // config/tray state.
    emebalachat::g_apply_ui_language = nullptr;
    // REQ-047 D3: retire the OpenAI coordinator with it, so a late posted
    // kMsgOpenOpenAiSettings can never call into destroyed config/engine/tray.
    emebalachat::g_apply_openai_settings = nullptr;
    // V10F-R1 F-17: the consent-origin seam retires with the same contract.
    emebalachat::g_apply_openai_consent_origin = nullptr;
    // REQ-051 (Symptom D): the kDeleted seam retires with the same contract.
    emebalachat::g_apply_openai_deleted = nullptr;
    // REQ-048 R2-D: retire the gguf-model-manager coordinator with it, so a
    // late posted kMsgOpenGgufManager can never call into destroyed state.
    emebalachat::g_open_gguf_model_manager = nullptr;
    // REQ-060: retire the load-settings coordinator with it, so a late posted
    // kMsgOpenEngineLoadSettings can never call into destroyed state.
    emebalachat::g_open_engine_host_settings = nullptr;
    // 261116_0003 P9b-3 U4a: retire the user-language dialog coordinators with
    // the same contract, so a late posted kMsgOpenUserLangAdd /
    // kMsgOpenUserLangManager can never call into destroyed state.
    emebalachat::g_open_user_lang_add = nullptr;
    emebalachat::g_open_user_lang_manager = nullptr;
    // Post-retire verify-empty (SEC-B5, spec 234100 §7.4): with all producers
    // joined and the drain above already run, the deque is expected to be
    // empty; the swap-out releases any residue's std::string memory before
    // teardown continues.
    emebalachat::DrainLanguageSyncQueue(); // discard any residue
    // REQ-045 P4-8 (item 1): drain any still-queued "engine unavailable" modal
    // requests at the identical shutdown position (all producers joined above).
    // REQ-048 P1 note: the detached bootstrap grace thread (up to 120 s) is NOT
    // a joined producer — it may still PostMessageW after this drain; a post to
    // an already-destroyed controller HWND fails harmlessly (FALSE return, no
    // payload — pure (0,0) wake-up), same accepted trade-off as the REQ-045
    // repair thread. The queue itself holds std::wstring payloads whose memory
    // is released by the swap-out; the drain prevents an apply-after-destroy
    // path on the surfaces the modal parents to. Any kMsgEngineUnavailableModal
    // notification still sitting in the OS queue is harmless.
    emebalachat::DrainEngineModalQueue();
    // REQ-043 (M6 T5): the warmup_thread / engine_switch_preload_thread joins
    // are removed with the preload machinery — no preload thread exists
    // anymore, so no join is needed and no thread can touch `engine` before
    // destruction (all producers joined above via Stop()/Join() calls).
    if (!engine.WaitInferenceIdle(2000)) {
        DIAG_F("MAIN/Shutdown/004: engine did not report idle within 2 s; forcing teardown (a host/cloud request may still be winding down)\n");
    }
    if (emebalachat::g_hPowerWnd) {
        ::WTSUnRegisterSessionNotification(emebalachat::g_hPowerWnd);
    }
    // V10F BUG-09/RISK-01: tray.Destroy() moved ABOVE the bounded worker joins
    // (right after worker.Stop()); the notify icon is already gone here.
    badge.Destroy();
    tooltip.Destroy();
    drag_icon.Destroy();
    about_window.Destroy();
    // V10F-R1 F-09: the modeless translation editor has no parent/owner, so it
    // never appeared in any teardown list; destroy it here (GUI thread) before
    // the HWND teardown continues, then zeroize the translation-history ring
    // (full user content) before the process teardown frees its buffers.
    emebalachat::translation_editor_window::ShutdownDestroy();
    emebalachat::translation_history::Clear();
    // 260922_0001 A8: join the resident sound worker last — the hook thread
    // (stopped above via hook.Stop()) and the tray menu callbacks (retired by
    // tray.Destroy()) were the only PlaySoundAsync producers, so no enqueue
    // can race the shutdown latch. Bounded by at most one in-flight Beep.
    emebalachat::ShutdownSound();

    if (hController) {
        ::DestroyWindow(hController);
    }
    if (emebalachat::g_hPowerWnd) {
        ::DestroyWindow(emebalachat::g_hPowerWnd);
        emebalachat::g_hPowerWnd = nullptr;
    }
    emebalachat::g_pMouseHook = nullptr;
    emebalachat::g_pHook = nullptr;
    emebalachat::g_pBadge = nullptr;
    emebalachat::g_pTray = nullptr;
    emebalachat::g_pTooltip = nullptr; // REQ-005 (M6 T6): release the repair-thread view
    emebalachat::g_hControllerWnd = nullptr;

    // V10F BUG-09/RISK-01: the unconditional exit save moved ABOVE the bounded
    // worker joins (right after worker.Stop()); nothing below mutates config.
    ::CoUninitialize();

    if (hMutex) {
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
    }

    // 260905 diagnostics: session end bookkeeping + deterministic drain of the
    // queue (Shutdown joins the flush thread after writing everything still
    // enqueued — the last lines of every run are "session end" + drop count).
    DIAG_LOG("SESSION", "session end (dropped=%llu)",
             static_cast<unsigned long long>(diag::DroppedCount()));
    diag::Shutdown();

    return 0;
}
