#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace emebalachat {

// REQ-037 (P4 Batch B-3, design §2.1.1) + UI-language-89 phase 1A (2026-11-19,
// docs/context/context-handoff-20261119-ui-language-89.md): the enum carries
// ALL 89 selectable translation-target locales in kAllLanguages registry
// order (src/config.cpp) - Auto first - so the enum, the LocaleMapping table,
// the selector join and the registry share one canonical ordering. The 52
// values appended after Burmese (the GU..GOM P2/P3 registry rows) landed in
// phase 1A with their mapping rows gated authored=false; phase 3
// (261119_0001 batches 1-4) authored all 52 string tables, so the gating
// described below is HISTORY now - all 89 rows are selectable.
//
// History note: the R6-era removal of FR/DE/RU (half-wired locales with no
// translation tables, plan §5.5) is SUPERSEDED by REQ-037's "all 37" mandate -
// the user explicitly forbade silent-English duplicates ("no checklist
// gaming"). The removal MECHANISM survives as the authenticity gate: a locale
// whose table is not authored is withheld from GetSupportedUiLocales() and
// refused by PlanUiLocaleChange (design §2-Q4.2). The first 37 tables are
// authored since B-3; the 52 phase-1A rows were gated until phase 3 authored
// their tables (261119_0001 batches 1-4) - all 89 rows are authored now, and
// the gate stays in place for any future locale.
enum class UiLocale {
    Auto,
    Korean,
    English,
    Vietnamese,
    ChineseSimplified,
    ChineseTraditional,
    Japanese,
    Spanish,
    French,
    German,
    Russian,
    Thai,
    Arabic,
    Portuguese,
    Italian,
    Indonesian,
    Malay,
    Filipino,
    Khmer,
    Lao,
    Hindi,
    Bengali,
    Turkish,
    Polish,
    Dutch,
    Ukrainian,
    Persian,
    Urdu,
    Hebrew,
    Czech,
    Hungarian,
    Swedish,
    Greek,
    Romanian,
    Danish,
    Finnish,
    Norwegian,
    Burmese,
    // ---- UI-language-89 phase 1A (2026-11-19): the remaining 52 registry
    // targets (GU..GOM), appended in kAllLanguages order after MY. The
    // mapping rows landed gated authored=false (authenticity gate) and phase
    // 3 (261119_0001 batches 1-4) authored every string table - all 52 are
    // selectable now; the gate stays in place for any future locale.
    Gujarati,
    Telugu,
    Marathi,
    Tamil,
    Tibetan,
    Kazakh,
    Mongolian,
    Uyghur,
    Cantonese,
    Bosnian,
    Serbian,
    Croatian,
    Slovenian,
    Slovak,
    Bulgarian,
    Lithuanian,
    Latvian,
    Estonian,
    Afrikaans,
    Catalan,
    Icelandic,
    Albanian,
    Macedonian,
    Welsh,
    Irish,
    Basque,
    Galician,
    Swahili,
    HaitianCreole,
    Uzbek,
    Azerbaijani,
    Belarusian,
    Kyrgyz,
    Tajik,
    Esperanto,
    Armenian,
    Georgian,
    Sinhala,
    Malayalam,
    Kannada,
    Punjabi,
    Odia,
    Amharic,
    Tigrinya,
    Pashto,
    Sindhi,
    Nepali,
    Assamese,
    Dzongkha,
    Sanskrit,
    Maithili,
    KonkaniGoan
};

enum class StringId {
    // Menu & Status
    MenuStatusActive,
    MenuStatusPaused,
    MenuEngine,
    MenuEngineGoogle,
    MenuEngineLocal,
    MenuSourceLang,
    MenuTargetLang,
    MenuSwapLangs,
    MenuAutoSend,
    MenuSoundFeedback,
    MenuShowBadge,
    MenuStartWithWindows,
    MenuCheatSheet,
    MenuExit,
    MenuAbout,

    // Cheat Sheet Dialog
    CheatSheetTitle,
    CheatSheetBody,

    // About Window
    AboutTitle,

    // Badge Status Words
    BadgeActive,
    BadgeTranslating,
    BadgePaused,

    // Tray Tooltip
    TooltipTitle,

    // Drag-icon click feedback (REQ-R1, session 260905_0001): transient
    // notice bodies shown when the click cannot produce a translation.
    TooltipCopyFailed,  // clipboard copy could not be confirmed (slow/lost selection)
    TooltipNoSelection, // copy confirmed but the captured text was empty/whitespace

    // ---- R6 Phase 5 (plan §5.2/§5.3): About body + migrated literals ----
    // Startup diagnostics (single-instance notice, COM-failure warning).
    AppAlreadyRunning,
    AppComFailed,

    // About window body copy (marketing text -> translated in all locales;
    // the contact LINES carry universal factual data - phone/address/name -
    // with localized labels, per the plan's §5.2 decision).
    AboutTagline,
    AboutFeature0,
    AboutFeature1,
    AboutFeature2,
    AboutEtymology,
    AboutLinkWebsite,
    AboutLinkContact,
    AboutLinkReddit,   // replaces AboutLinkDownload (Reddit r/emebala, user decision)
    AboutContactOrg,
    AboutContactPhone,
    AboutContactLead,

    // Phase 4 (REQ-020): About-window "Reset to system defaults" button label
    // and its transient post-click confirmation label (plan §1.4/§2.5).
    AboutResetButton,
    AboutResetDone,

    // Tooltip footer buttons + copy feedback (plan §5.3 sweep).
    TooltipCopied,
    TooltipButtonCopy,
    TooltipButtonTts,

    // R6 Phase 6 (plan §5.4): UI-language selector submenu.
    MenuUiLanguage,     // submenu title ("Interface Language")
    MenuUiLanguageAuto, // "Auto (system language)" entry of the selector

    // REQ-025 (Phase A §2.1.A3-25): tray language-picker group headers -
    // "키보드타이핑"(typing-translation) and "번역툴팁"(drag tooltip) contexts.
    MenuTypingGroup,
    MenuTooltipGroup,

    // Common
    AutoDetect,

    // REQ-B-001 (session 260909_0001 Batch-1): per-locale brand display name
    // ("에메발라 챗" etc.). Appended last so the EnumCount completeness loop
    // covers it automatically (49x37 with TooltipNoTtsVoice below).
    AppName,
    // REQ-C-004 (session 260909_0001 Batch-1): no-voice TTS notice body
    // (Phase C, design §1.2.2 - "No Windows voice installed..." message).
    TooltipNoTtsVoice,

    // REQ-206/208 (session 260911_0002 T2, P3 design §2.3/§2.4): first-run
    // privacy consent popup (title + body incl. the mandated "re-read the
    // README anytime" guidance line) and the Cheat Sheet config.json
    // relative-path line. The popup is a MB_OK MessageBoxW, so its OK button
    // is system-localized and needs no StringId here. Appended last so the
    // EnumCount completeness loop covers them automatically (52x37).
    PrivacyNoticeTitle,
    PrivacyNoticeBody,
    CheatSheetConfigPath,

    // REQ-042 (session 260917_0002, 계획-2): notice shown when the Enter path
    // takes the tail-unchanged short-circuit while the verbatim prefix still
    // holds translatable text (the REQ-041 under-slice is policy, not
    // failure). Appended last so the EnumCount completeness loop covers it
    // automatically (58x37 with the REQ-005 repair trio below).
    TooltipUntranslatedAbove,

    // REQ-005 (M6 T6, design §6.4, plan §V2-8.6): engine-host repair
    // guidance. Surfaced when the local engine components are missing and
    // the silent repair (§V2-8.3) is disabled or failed — the "repair
    // unavailable" convergence, never a silent failure. Appended last so
    // the EnumCount completeness loop covers them automatically (57x37).
    RepairInProgress,   // background repair running (tray/tooltip status)
    RepairFailedTitle,  // repair-unavailable notice title
    RepairFailedBody,   // local translation temporarily disabled + cloud-consent guidance

    // REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible engine. Tray
    // entry + the settings dialog (base URL / key / model / fetch list /
    // http warning / save confirmation / validation errors). Appended last
    // so the EnumCount completeness loop covers them automatically (68x37).
    MenuEngineOpenAi,          // tray engine submenu: "OpenAI Compatible"
    OpenAiSettingsTitle,       // dialog caption
    OpenAiSettingsAction,      // tray/settings entry that opens the dialog
    OpenAiBaseUrlLabel,        // Base URL edit label
    OpenAiApiKeyLabel,         // API Key edit label
    OpenAiModelLabel,          // Model combo label
    OpenAiFetchModels,         // "Fetch model list" button
    OpenAiFetchFailed,         // notice when the model list could not be fetched
    OpenAiHttpWarningTitle,    // plaintext-http consent dialog title
    OpenAiHttpWarningBody,     // plaintext-http consent dialog body
    OpenAiSaved,               // post-save confirmation
    OpenAiKeyMasked,           // masked key read-back display
    OpenAiInvalidBaseUrl,      // validation error when the base URL is unusable

    // REQ-045 P4-5 (item 3a-2, design §A.3/A.4): third-party .gguf user-model
    // registration. Tray "사용자 선택(.gguf)…" submenu + browse entry, the
    // quality-not-guaranteed consent gate (§A.4, shown once per app run before
    // the file picker), and the post-registration notice (incl. the
    // local-engine-selection / host-restart guidance). Appended last so the
    // EnumCount completeness loop covers them automatically (70x37 -> 76x37).
    MenuEngineUserGguf,        // tray engine submenu: "사용자 선택(.gguf)…"
    MenuBrowseGgufFile,        // nested entry: "파일찾기(.gguf)"
    UserGgufQualityTitle,      // quality-gate dialog title
    UserGgufQualityBody,       // quality-gate dialog body (Hy-MT2 미보장 안내)
    UserGgufRegisteredTitle,   // post-registration dialog title
    UserGgufRegisteredBody,    // post-registration body (사용 방법 + 호스트 재기동 안내)
    // REQ-047 D2 (design §B.3): informational body for the bundled-origin
    // reuse-path rejection (re-picking the built-in .gguf through the user
    // registration picker). Title reuses UserGgufRegisteredTitle.
    UserGgufBundledDuplicateBody, // "already built in" notice (76 -> 77)

    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker. REQ-050
    // (3-1): the tray no longer appends the placeholder — with no model
    // registered the checkable user-model entry is omitted entirely and only
    // the manager row shows. The string stays in the tables (frozen field
    // count); reserved for a future empty-state hint.
    MenuEngineUserGgufEmpty,     // "(미등록)" empty-slot marker (unused by tray)

    // REQ-048 R2-D: registered user-.gguf model manager (rename/delete). Tray
    // engine-submenu row + the manager dialog (list / empty placeholder /
    // Rename / Delete / Close), the rename prompt (title/body/invalid), the
    // delete confirmation (title/body — states the .gguf FILE stays on disk),
    // and the post-apply notice. Appended last so the EnumCount completeness
    // loop covers them automatically (78 -> 90).
    MenuManageGgufModels,          // tray engine submenu: "모델 관리…"
    GgufManagerTitle,              // manager dialog caption
    GgufManagerEmpty,              // placeholder when no user model is registered
    GgufManagerRename,             // "이름 바꾸기…" button
    GgufManagerDelete,             // "삭제…" button
    GgufManagerClose,              // "닫기" button
    GgufManagerDeleteConfirmTitle, // delete confirmation caption
    GgufManagerDeleteConfirmBody,  // delete confirmation body (file kept on disk)
    GgufManagerRenameTitle,        // rename prompt caption
    GgufManagerRenameBody,         // rename prompt instructions
    GgufManagerRenameInvalid,      // validation error (empty/dup/separator/space/length)
    GgufManagerDone,               // post-apply completion notice

    GgufManagerErrSerialize,
    GgufManagerErrNoLocalappdata,
    GgufManagerErrWrite,
    GgufManagerErrWritePartial,
    GgufManagerErrRegistryDamaged,

    TooltipTranslateFailed,      // drag-path failure notice (stale-tooltip fix)
    RepairTransientBody,         // strict-local TRANSIENT failure wording (vs missing-files)

    // REQ-050: localized dialog push-button labels (OK/Cancel). Appended
    // before the HF block below so the EnumCount completeness loop covers
    // them automatically.
    DialogOk,
    DialogCancel,

    // REQ-050 (merged gguf manager): the manager window's two add-methods row
    // ([파일에서 추가…] routes into the existing file-picker registration
    // pipeline; [Hugging Face에서 추가…] opens the download dialog below) plus
    // the Hugging Face add dialog strings. Appended at the tail so the
    // EnumCount completeness loop covers them automatically (109x37 after
    // the two OpenAI cue-banner hints appended below).
    GgufManagerAddFile, // "파일에서 추가…" top-row button
    GgufManagerAddHf,   // "Hugging Face에서 추가…" top-row button
    HfAddTitle,         // HF add dialog caption
    HfUrlLabel,         // "모델 URL" edit label
    HfDownloading,      // "다운로드 중…" status text
    HfFailed,           // generic download/move failure status text
    HfInvalidUrl,       // fail-closed URL rejection notice
    HfDone,             // post-registration completion notice

    // REQ-050 (OpenAI settings cue banners): gray placeholder hints on the
    // two single-line edits (EM_SETCUEBANNER). Appended at the tail so the
    // EnumCount completeness loop covers them automatically (109x37;
    // REQ-051 bumped the tail to 110 — see below).
    OpenAiBaseUrlHint,  // "e.g. https://api.openai.com/v1"
    OpenAiApiKeyHint,   // "sk-..."

    // REQ-051 (Symptom D, decisions.md 260921 17:08 — user-frozen scope): the
    // OpenAI settings dialog [삭제] push button. Clears exactly the saved
    // OpenAI block (base_url / model / api_key_dpapi+sha256 pair / http
    // consent) and leaves the engine selection untouched. Appended at the
    // tail so the EnumCount completeness loop covers it automatically
    // (110x37).
    OpenAiDeleteSettings,  // "설정 삭제" / "Delete settings"

    // REQ-UC (260930_0004, update-checker design §12): the About-card update
    // zone strings (six states + consent dialog + progress + ready + errors).
    // English + Korean authored verbatim from the spec's master string table;
    // the other 35 tables carry careful short imperative translations. The
    // {v}/{s} placeholders are positional tokens replaced by the About window
    // (version string / human size) — every translation keeps them intact.
    // Appended at the tail so the EnumCount completeness loop covers them
    // automatically (135x37).
    UpdateCheck,         // "업데이트 확인" / "Check for updates"
    UpdateIdleNote,      // "Emebala Chat이 최신인지 확인합니다." / "See if you are running the latest Emebala Chat."
    UpdateChecking,      // "업데이트 확인 중…" / "Checking for updates…"
    UpdateCheckingHint,  // "백그라운드에서 실행됩니다 — 이 창을 닫아도 됩니다." / "Runs in the background; you may close this window."
    UpdateUptodate,      // "최신 버전을 사용 중입니다" / "You are up to date"
    UpdateDlgTitle,      // "업데이트를 사용할 수 있습니다" / "Update available"
    UpdateDlgVersion,    // "새 버전" / "Version"
    UpdateDlgVersionCur, // "현재 {v}" / "you have {v}"
    UpdateDlgSize,       // "크기" / "Size"
    UpdateDlgSizeApprox, // "약 {s}" / "about {s}"
    UpdateDlgPrivacy,    // "확인/다운로드 시 GitHub에 연결됩니다." / "Checking and downloading connect to GitHub."
    UpdateDlgDownload,   // "다운로드" / "Download"
    UpdateDlgLater,      // "나중에" / "Later"
    UpdateDlProgress,    // "다운로드 중…" / "Downloading…"
    UpdateDlCancel,      // "취소" / "Cancel"
    UpdateDlCancelled,   // "다운로드가 취소되었습니다" / "Download cancelled."
    UpdateReadyTitle,    // "다운로드 완료, 설치합니다" / "Download complete — ready to install"
    UpdateReadyWarn,     // "설치를 시작하면 앱이 종료됩니다." / "The app will close to install."
    UpdateReadyInstall,  // "지금 설치" / "Install now"
    UpdateReadyLater,    // "나중에" / "Later"
    UpdateErrOffline,    // "오프라인 상태입니다. 인터넷 연결을 확인한 후 다시 시도하세요." / "You are offline. Check your connection, then try again."
    UpdateErrRate,       // "확인 요청이 너무 많습니다. 잠시 후 다시 시도하세요." / "Too many requests. Please try again in a little while."
    UpdateErrHash,       // "다운로드한 파일이 손상되었습니다" / "The downloaded file did not pass verification."
    UpdateErrRetry,      // "다시 시도" / "Retry"
    UpdateSnoozedLine,   // "업데이트를 나중에 설치할 수 있습니다" / "You can install the update later."

    // REQ-MD (260930_0004, decisions.md D9): the on-demand bundled-model
    // download consent/progress dialog. Later/Cancel/Retry/Close/the download
    // label/hash-mismatch line REUSE the update-zone + manager ids; these 9
    // carry the consent, privacy, phase, result and disk-space copy. The
    // {s} placeholders are the human size (update-checker FormatSizeMb).
    // Appended at the tail so the EnumCount completeness loop covers them
    // automatically (144x37).
    ModelDlCaption,      // "모델 다운로드" / "Model download"
    ModelDlConsentBody,  // "내장 번역 모델이 설치되어 있지 않습니다. 지금 다운로드하시겠습니까? (약 {s})" / "The built-in translation model is not installed. Download it now? (about {s})"
    ModelDlPrivacy,      // "다운로드 시 huggingface.co에 연결됩니다." / "Downloading connects to huggingface.co."
    ModelDlDownload,     // "지금 다운로드" / "Download now"
    ModelDlVerifying,    // "다운로드한 파일을 확인하는 중…" / "Verifying the download…"
    ModelDlPlacing,      // "모델을 설치하는 중…" / "Installing the model…"
    ModelDlDone,         // "모델이 준비되었습니다. 로컬 엔진이 다음 번역부터 사용합니다." / "The model is ready. The local engine will use it from the next translation."
    ModelDlFailed,       // "다운로드에 실패했습니다. 연결을 확인하고 다시 시도하세요." / "The download failed. Check your connection and try again."
    ModelDlDiskSpace,    // "디스크 여유 공간이 부족합니다(약 {s} 필요)." / "Not enough free disk space (about {s} required)."

    // REQ-060 (session 261009_0001, engine-idle-eviction-warmup): the
    // translation-model load-mode settings UI. The first six carry the new
    // engine-host settings dialog (src/ui/engine_host_settings_window.*): the
    // tray entry row (wired by the integration agent), the dialog title, the
    // two fixed combo options and their per-selection description lines. The
    // remaining seven extend the gguf-model manager (resident-policy label
    // suffixes, the policy-toggle button, and the duplicate-file warning /
    // cleanup pair — {n}/{s}/{c} placeholders are the normalized stem, the
    // human size and the copy count). Appended at the tail so the EnumCount
    // completeness loop covers them automatically (157x37).
    EngineLoadSettingsMenuRow, // "번역 모델 로드 설정…" / "Translation model load settings…"
    EngineLoadSettingsTitle,   // "번역 모델 로드" / "Translation model loading"
    LoadModeOnUse,             // "사용할 때 로드 (권장)" / "Load when used (recommended)"
    LoadModeWarmup,            // "미리 로드" / "Preload"
    LoadModeOnUseDesc,         // "메모리를 아껴요. 첫 번째 번역이 잠깐 늦어질 수 있어요." / "Saves memory. The first translation may be a few seconds late."
    LoadModeWarmupDesc,        // "번역이 바로 시작돼요. 모델이 메모리에 계속 올라가 있어요." / "Translation starts right away. The model stays in memory."
    GgufPolicySticky,          // "(상주 유지)" / "(Keep resident)"
    GgufPolicyEvict,           // "(사용 시 로드)" / "(Load on use)"
    GgufManagerTogglePolicy,   // "로드 방식 전환" / "Load mode"
    GgufDuplicateWarning,      // "중복 파일: {n} {s} × {c}" / "Duplicate files: {n} {s} × {c}"
    GgufManagerDedupe,         // "중복 정리" / "Clean up duplicates"
    GgufManagerDedupeDone,     // "중복 파일을 휴지통으로 옮겼습니다." / "The duplicate files were moved to the Recycle Bin."
    GgufManagerDedupeFailed,   // "중복 파일을 정리하지 못했습니다. 파일이 다른 프로그램에서 사용 중인지 확인하세요." / "The duplicate files could not be cleaned up. Check whether another program is using them."

    // UI Theme selector (Direction A: Fluent, Direction B: Editorial, Direction C: Monolith)
    MenuUiTheme,
    ThemeFluent,
    ThemeEditorial,
    ThemeMonolith,

    // 261108_0002 (WS3, v0.10.6): floating back-translation verification card
    // + its tray toggle. The tray row carries the check mark
    // (ID_TRAY_BACK_TRANSLATE); the card itself renders the header line and
    // the two meaning-verdict chips. Appended at the tail so the EnumCount
    // completeness loop covers them automatically (161x37 -> 165x37).
    MenuBackTranslation,     // "역번역 플로팅 표시 (번역 검증)" / "Floating Back-Translation"
    BackTranslateHeader,     // "🔄 역번역 확인" / "🔄 Back-Translation"
    BackTranslateMatchExact, // "✓ 원문 일치" / "✓ Meaning Matched"
    BackTranslateMatchCheck, // "⚡ 표현 확인" / "⚡ Check Nuance"
    // 261108_0002 (FIX-WS3B, v0.10.6): the card's reference-line label. It was
    // a hardcoded Korean literal in tooltip.cpp, so "원문:" leaked into all 37
    // UI languages while the header and the two pills were localized. Appended
    // (not inserted) so every pre-existing StringId keeps its numeric value.
    BackTranslateReference,  // "↳ 원문: " / "↳ Original: "

    // 261121_0001 (v0.10.9, session 261121_0001 U-C — contract C5): the
    // reasoning-effort setting's UI copy (OpenAI settings dialog) + the
    // redesigned back-translation card's two section headers. Inserted
    // right AFTER BackTranslateReference on purpose: every later enum tail
    // run is locked by an adjacency pin owned elsewhere (ws3's 5-run
    // contiguity ends at BackTranslateReference; the 261111_0001 U6 /
    // 261116_0003 P9b-1 / P9b-3 U5 / P9c-1b / P6a / P7 / P5 runs and the
    // "EnumCount - 1 == LanguageSearchNoMatch" sentinel are pinned in
    // tests/run_tests.cpp and p5_language_picker_tests.inc), so this is the
    // only free tail slot. The Get() switch is the mapping; EnumCount
    // absorbs +4 (199 -> 203) and the completeness loop covers them.
    OpenAiReasoningLabel,         // "추론 강도" / "Reasoning effort"
    OpenAiReasoningServerDefault, // "서버 기본값 (미지정)" / "Server default (unset)"
    BackTransSectionBack,         // "역번역" / "Back-translation"
    BackTransSectionOriginal,     // "원문" / "Original text"

    // 261122_0001 B-U2 (OpenAI 설정 창 프로필 재설계, design-pick.md item 6):
    // the OpenAI profile row's six strings: the row label, the upsert and
    // delete buttons, the display-name derivation template ("{h}" = host
    // abbreviation, "{m}" = model id; the tokens stay verbatim, the word order
    // is per-locale), the MB_YESNO delete confirmation (it MUST keep saying
    // that the stored API key goes too), and the profile-cap notice ("{n}").
    // Inserted IMMEDIATELY AFTER BackTransSectionOriginal (the 261121_0002
    // boundary): the six ids sit between BackTransSectionOriginal and the
    // 261121_0002 progress/cancel run, so every later enum value shifts by +6
    // TOGETHER, all relative adjacency pins stay valid, and EnumCount absorbs
    // +6 (207 -> 213). The struct slots are trailing defaults in the same
    // relative position (src/i18n.cpp, right after
    // back_trans_section_original); the Get() switch is the mapping.
    OpenAiProfileLabel,          // "프로필" / "Profile"
    OpenAiProfileSave,           // "저장" / "Save"
    OpenAiProfileDelete,         // "삭제" / "Delete"
    OpenAiProfileNameTemplate,   // "{h}의 {m} 모델" / "{h} / {m}"
    OpenAiProfileDeleteConfirm,  // delete confirmation incl. the API-key loss
    OpenAiProfileLimit,          // profile-cap notice ("{n}")

    // 261121_0002 (v0.10.9 leftover, session 261121_0002 progress+cancel
    // design): the multi-chunk translation PROGRESS + CANCEL UI copy (badge
    // progress label, chunk-count plan label, cancelling feedback, the
    // post-cancel notice, and the one-shot cancel hint). This run was appended
    // at the same boundary per the binding design-pick.md and now follows the
    // 261122_0001 B-U2 profile block: every later enum value shifts TOGETHER,
    // so all relative adjacency pins (the P6a/P7 MenuSearchLangs..
    // LanguageSearchNoMatch run, the P5 "EnumCount - 1 ==
    // LanguageSearchNoMatch" sentinel, the P9c-1b declaration-checklist run)
    // stay valid. The struct fields are trailing defaults in the matching
    // relative position (src/i18n.cpp, right after the profile block); the
    // Get() switch is the mapping. "{c}"/"{t}"/"{n}" are runtime-substituted
    // number tokens.
    BadgeTranslatingProgress,    // "번역 중… {c}/{t}" / "Translating… {c}/{t}"
    BadgeTranslatingChunks,      // "번역 중 ({n}개 청크)" / "Translating ({n} chunks)"
    BadgeCancelling,             // "취소 중…" / "Cancelling…"
    TooltipTranslationCancelled, // "번역이 취소되었습니다." / "Translation cancelled."
    TooltipCancelHint,           // "긴 번역 중입니다. 배지를 클릭하면 취소할 수 있습니다." / "Long translation in progress. Click the badge to cancel."

    // 261111_0001 (v0.10.7) U6 — user-model surface completeness (R1 P1):
    //   * GgufInvalidModelBody: the registration-time GGUF gate rejection
    //     (src/gguf_validator.* wired into the manager's HF/orphan add
    //     paths) — a truncated/corrupt/non-GGUF file is refused BEFORE the
    //     registry write;
    //   * GgufManagerActiveSuffix: the model-manager list marker on the
    //     currently active user-model binding (double-click switches it);
    //   * GgufManagerActivated: the post-switch confirmation notice.
    // Appended at the tail so the EnumCount completeness loop covers them
    // automatically (166x37 -> 169x37).
    GgufInvalidModelBody,  // "올바른 GGUF 모델 파일이 아니어서 등록하지 않았습니다." / "Not a valid GGUF model file; it was not registered."
    GgufManagerActiveSuffix, // "(사용 중)" / "(in use)"
    GgufManagerActivated,  // "사용 중인 모델로 설정했습니다." / "This model is now in use."

    // 261116_0003 P9b-1 (language-menu redesign, plan §4 Phase 3 + D5/D6): the
    // user-language surface's UI copy - the "내 언어" group header, the add
    // dialog, the free-form (no code) notice, the probe badge states + evidence
    // labels + retest action, the two final-authority overrides, the
    // "support is not quality" disclaimer, and the reserved-name refusal. The
    // P9a core (src/config.hpp: UserLanguage / UserLanguageProbe /
    // UserLanguageProbeStatus / ProbeEvaluation / LanguageMenuGroup) owns every
    // behavior; this block owns only the words P9b-2/P9b-3 will render.
    //
    // ENUM PLACEMENT (same workaround as P6a's MenuSearchLangs and P7's
    // LanguageSearchCount, one step earlier): these ids sit BEFORE
    // MenuSearchLangs, not after LanguageSearchNoMatch, because
    // p5_language_picker_tests.inc pins "EnumCount - 1 ==
    // LanguageSearchNoMatch" and that file is outside this unit's write
    // boundary. The P6a/P7 adjacency (MenuSearchLangs + 1 ==
    // LanguageSearchCount, + 2 == LanguageSearchCue, + 3 ==
    // LanguageSearchNoMatch) is therefore left untouched as well. The STRUCT
    // slots are the trailing ones in src/i18n.cpp, so enum order and struct
    // order differ on purpose; the Get() switch is the mapping and a pin in
    // tests/run_tests.cpp asserts this ordering contract (173 -> 196).
    UserLangsGroup,            // "내 언어" / "My languages" (menu group header)
    // Add dialog (P9b-3): title, the two name fields (the English one marked
    // optional), the two DERIVED rows the dialog SHOWS so the user never needs
    // a code (D5), and the two push buttons.
    UserLangAddTitle,          // "내 언어 추가" / "Add my language"
    UserLangNativeNameLabel,   // "원어 이름" / "Native name"
    UserLangEnglishNameLabel,  // "영어 이름 (선택)" / "English name (optional)"
    UserLangCodeLabel,         // "언어 코드" / "Language code" (resolved code row)
    UserLangScriptLabel,       // "문자" / "Script" (inferred script row)
    UserLangAdd,               // "추가" / "Add"
    UserLangCancel,            // "취소" / "Cancel" (this dialog's own label)
    // D5 "no code = OpenAI/user-.gguf only": shown when the ladder resolved
    // nothing, so the entry stays usable on the free-form engines.
    UserLangNoCodeNotice,      // notice body (see src/i18n.cpp for the full text)
    // Probe badge (D6): the three non-empty display states. Unknown renders no
    // badge (nothing has been judged yet), and UserOverridden renders
    // UserLangOverrideActive instead of a verdict.
    UserLangProbeChecking,     // "확인 중" / "Checking"
    UserLangProbeSupported,    // "지원" / "Supported"
    UserLangProbeUnsupported,  // "미지원 의심" / "Possibly unsupported"
    UserLangProbeEvidenceLabel, // "판정 근거" / "Evidence" (shape-only, never text)
    UserLangProbeRetest,       // "다시 시험" / "Test again"
    // Override (D6 "the user is the final authority"): the two answers plus the
    // marker that says the displayed verdict is the user's, not a probe's.
    UserLangOverrideSupported, // "지원함으로 표시" / "Mark as supported"
    UserLangOverrideUnsupported, // "미지원으로 표시" / "Mark as unsupported"
    UserLangOverrideActive,    // "직접 설정함" / "Set by you"
    // D6 "support != quality", mandated in UI copy.
    UserLangSupportNotQuality, // "지원 여부는 번역 품질을 뜻하지 않습니다." / "Support does not mean translation quality."
    // Add-dialog refusal (P9b-3 enforces it): the typed name collides with a
    // registry entry or an "Auto Detect"/AUTO token. Carries the typed name as
    // the "{n}" token (the same runtime-substitution convention as P7's
    // LanguageSearchCount and the gguf manager's "{n}" stem).
    UserLangNameReserved,      // "\"{n}\" ..." refusal (see src/i18n.cpp)
    // Probe evidence detail (D6): one label per check bit ProbeEvaluation
    // reports, plus the fail-open script-row value when inference returned 0.
    // The evidence STRING itself stays the P9a ASCII shape ("nonempty=1
    // differs=1 script=1 expected=latin detected=latin"), because it is cached
    // per (engine, model) and compared across runs; only these labels around it
    // are localized.
    UserLangCheckNonEmpty,     // "응답 있음" / "Reply received"
    UserLangCheckDiffers,      // "원문과 다름" / "Differs from input"
    UserLangCheckScript,       // "문자 일치" / "Script matches"
    UserLangScriptNone,        // "확인 안 됨" / "Not detected" (script mask 0)

    // 261116_0003 P9b-3 U5 (language-menu redesign, CEO decision 1.b,
    // decisions.md:540): the picker's AUTO-view header chip. The Auto view
    // used to render the LOCAL engine name ("내장 로컬 엔진 · N개 언어"),
    // a half-truth that hid AUTO's cloud leg; the Auto chip now carries this
    // dedicated label and NO count (the count stays on the four concrete
    // engine chips). Inserted inside the pre-MenuSearchLangs block for the
    // same P5-sentinel reason as P9b-1/P7; the STRUCT slot is the trailing
    // one and the Get() switch is the mapping.
    LanguageSearchAutoChip, // "자동 - 지원 언어는 로컬, 나머지는 클라우드" / "Auto - local for supported languages, cloud for the rest"

    // 261116_0003 P9c-1b (language-menu redesign, plan Phase 3 / P9c): the
    // OpenAI settings dialog's optional "이 모델이 지원하는 언어" declaration
    // checklist (P9c-1a owns the config half) - the section title and the
    // fail-open hint. Same enum placement rule as P9b-1/U5 (before
    // MenuSearchLangs; the P5 sentinel pin owns the enum tail). The STRUCT
    // slots sit immediately BEFORE language_search_auto_chip (not after it),
    // because the English positional table must keep the AUTO chip label as
    // its LAST initializer (the P9b-3 U5 tail pin) - the Get() switch is the
    // mapping and the enum/struct orders differ on purpose, same as P9b-1.
    OpenAiDeclaredTitle,   // "이 모델이 지원하는 언어" / "Languages this model supports"
    OpenAiDeclaredHint,    // the empty-selection fail-open hint (see src/i18n.cpp)

    // 261116_0003 P6 (language-menu redesign, plan §5 P6 + D3): the "검색…"
    // row label prepended to the six flat language menus (the four tray menus
    // land in P6a, the tooltip pair in P6b). ONE string — the picker's own
    // cue/no-match pair below already exists. Inserted BEFORE the P5 pair on
    // purpose: p5_language_picker_tests.inc pins
    // "sentinel - 1 == LanguageSearchNoMatch" (enum-tail order), and that test
    // file is outside the P6a write boundary, so the new id slots between
    // ThemeMonolith and LanguageSearchCue and the P5 pin stays green
    // untouched (171 -> 172).
    MenuSearchLangs,        // "검색…" / "Search…"

    // 261116_0003 P7 (language-menu redesign, plan §5 P7 "engine chip
    // format"): the picker header's COUNT phrase. Exactly one new id is
    // needed: the chip's engine name reuses MenuEngineGoogle/Local/OpenAi/
    // UserGguf, and the language count itself is substituted at runtime, so
    // the string carries the number as the "{n}" token (the same placeholder
    // convention model_dl_disk_space's "{s}" and the gguf manager's "{n} {s} ×
    // {c}" already use; i18n.cpp has no printf/format layer). Inserted BEFORE
    // the P5 pair for the same reason MenuSearchLangs was: the P5 suite pins
    // "sentinel - 1 == LanguageSearchNoMatch" (enum-tail order) and
    // p5_language_picker_tests.inc is outside the P7 write boundary
    // (172 -> 173).
    LanguageSearchCount,    // "{n}개 언어" / "{n} languages"

    // 261116_0003 P5 (language-menu redesign, plan §4 "Search" / §5 P5): the
    // shared searchable language picker's OWN strings. Exactly two are needed —
    // the cue banner on the search field and the no-match state — because the
    // header line reuses MenuSourceLang/MenuTargetLang, every row reuses
    // GetLanguageDisplayName, and the "검색…" menu ROW label was P6 scope (the
    // engine chip's own count phrase landed in P7 as LanguageSearchCount
    // above; the P5 unit pre-empted neither).
    // Appended at the tail so the EnumCount completeness loop covers them
    // automatically (169 -> 171).
    LanguageSearchCue,      // "언어 검색…" / "Search languages…"
    LanguageSearchNoMatch,  // "일치하는 언어가 없습니다" / "No matching language"

    // 261122_0002 (v0.10.10 phased fixes, Phase-2 i18n worker): the
    // translation-card editor + pin surface's eight strings: the tooltip
    // card's pin/unpin actions, "open in window", the edit/retranslate
    // action, the editor dialog's title, the apply-to-original-window
    // action, the cancelling progress line, and the keep-translated-part
    // action. Inserted at the ENUM TAIL, immediately before EnumCount, so
    // every pre-existing id keeps its numeric value and EnumCount absorbs
    // +8 (213 -> 221); the P5 "EnumCount - 1 == LanguageSearchNoMatch"
    // sentinel therefore becomes EnumCount - 9 (the pin-site list for the
    // Phase-2 test update lives in
    // docs/261122_0002_session_v01010-phased-fixes/i18n-report-p2.md).
    // The STRUCT slots take the eight positions right AFTER
    // openai_declared_hint and right BEFORE language_search_auto_chip (the
    // AUTO chip closes the struct tail per the P9b-3 U5 pin, the same move
    // P9c-1b made), so the enum order and the struct order differ on
    // purpose; the Get() switch is the mapping. Per-locale fill: Korean is
    // authored; every other designated table carries the English draft
    // verbatim (the deliberate machine-draft baseline of the 261119 policy).
    PinTooltipCard,          // "고정" / "Pin"
    UnpinTooltipCard,        // "고정 해제" / "Unpin"
    OpenInWindow,            // "창으로 열기" / "Open in window"
    EditRetranslate,         // "수정 · 재번역" / "Edit / retranslate"
    ApplyToOriginalWindow,   // "원래 창에 적용" / "Apply to original window"
    TranslationEditorTitle,  // "번역 편집기" / "Translation editor"
    CancellingProgress,      // "취소 중…" / "Cancelling..."
    KeepTranslatedPart,      // "번역된 부분만 반영" / "Keep translated part"

    // 261122_0002 (v0.10.10 phased fixes, Phase-3 i18n worker): the settings
    // window redesign's seven strings: the four tab captions (General /
    // Translation engines / Languages / Advanced), the open-config-folder
    // action, the reset-declared-languages action, and the recent-languages
    // section header. Inserted at the ENUM TAIL, immediately before EnumCount,
    // so every pre-existing id keeps its numeric value and EnumCount absorbs
    // +7 (221 -> 228); the P5 "EnumCount - 9 == LanguageSearchNoMatch"
    // sentinel therefore becomes EnumCount - 16 (the pin-site list for the
    // Phase-3 test update lives in
    // docs/261122_0002_session_v01010-phased-fixes/i18n-report-p3.md).
    // The STRUCT slots take the seven positions right AFTER
    // keep_translated_part and right BEFORE language_search_auto_chip (the
    // AUTO chip closes the struct tail per the P9b-3 U5 pin, the same move
    // P9c-1b and Phase-2 made), so the enum order and the struct order differ
    // on purpose; the Get() switch is the mapping. Per-locale fill: Korean is
    // authored; every other designated table carries the English draft
    // verbatim (the deliberate machine-draft baseline of the 261119 policy).
    SettingsTabGeneral,       // "일반" / "General"
    SettingsTabEngines,       // "번역 엔진" / "Translation engines"
    SettingsTabLanguages,     // "언어" / "Languages"
    SettingsTabAdvanced,      // "고급" / "Advanced"
    OpenConfigFolder,         // "config.json 폴더 열기" / "Open config folder"
    ResetDeclaredLanguages,   // "선택 초기화(전체 지원)" / "Reset selection (support all)"
    RecentLanguagesHeader,    // "최근 사용 언어" / "Recent languages"


    // 261122_0002 (v0.10.10 phased fixes, Phase-3 i18n micro-pass): the
    // remaining user-visible English literals the Phase-3 units flagged for
    // this pass: the generic settings-menu label (MenuSettings, the
    // P3-C1 request), the settings hub's own caption + General/Advanced copy
    // (the P3-C3 request, 14 strings), and the cheat sheet's appended-gestures
    // appendix (the P3-C5 request, 4 strings). Inserted at the ENUM TAIL,
    // immediately before EnumCount, so every pre-existing id keeps its numeric
    // value and EnumCount absorbs +19 (228 -> 247); the P5 "EnumCount - 16 ==
    // LanguageSearchNoMatch" sentinel therefore becomes EnumCount - 35 (the
    // pin-site list for the Phase-3 test update lives in
    // docs/261122_0002_session_v01010-phased-fixes/i18n-report-p3-micropass.md).
    // The STRUCT slots take the nineteen positions right AFTER
    // recent_languages_header and right BEFORE language_search_auto_chip (the
    // AUTO chip closes the struct tail per the P9b-3 U5 pin, the same move
    // P9c-1b/Phase-2/Phase-3 made), so the enum order and the struct order
    // differ on purpose; the Get() switch is the mapping. Per-locale fill:
    // Korean is authored; every other designated table carries the English
    // draft verbatim (the deliberate machine-draft baseline of the 261119
    // policy).
    MenuSettings,                 // "환경설정" / "Settings"
    SettingsHubTitle,             // "설정" / "Settings"
    SettingsDragToTranslate,      // "드래그로 번역" / "Drag to translate"
    SettingsCloudFallback,        // "클라우드 대체 번역 허용" / "Allow cloud fallback"
    SettingsHotkeyToggle,         // "켜기/끄기 전환" / "Toggle on/off"
    SettingsHotkeyLang,           // "언어 순환" / "Cycle language"
    SettingsHotkeyMode,           // "자동 전송 켜기/끄기" / "Toggle auto-send"
    SettingsHotkeySelection,      // "선택한 텍스트 번역" / "Translate selection"
    SettingsDiagLogEnabled,       // "진단 로그" / "Diagnostic log"
    SettingsDiagLogContent,       // "사용자 콘텐츠 기록 (입력한 텍스트, 창 제목)" / "Log user content (typed text, window titles)"
    SettingsDiagContentWarning,   // "문제 해결 시에만 사용하세요. 이때 로그에는 입력한 내용과 창 제목이 포함될 수 있습니다." / "Only for troubleshooting: the log can then contain what you type and the titles of your windows."
    SettingsTemperature,          // "Temperature" / "Temperature"
    SettingsTopP,                 // "Top-p" / "Top-p"
    SettingsTopK,                 // "Top-k" / "Top-k"
    SettingsRepetitionPenalty,    // "반복 패널티" / "Repetition penalty"
    CheatSheetMoreGestures,       // "추가 제스처:" / "More gestures:"
    CheatSheetSelectionTranslate, // "선택한 텍스트 번역" / "Translate the selected text"
    CheatSheetDoubleCtrlC,        // "선택한 텍스트를 복사(Ctrl+C)하면 즉시 번역 툴팁" / "Copy selected text (Ctrl+C) to see its translation"
    CheatSheetEscCancel,          // "번역 취소 / 카드 닫기" / "Cancel the translation / close the card"

    // 261123_0001 (v0.10.10 rebuild-2 follow-ups, Bug A): the user-model
    // load-failure tooltip hint, shown instead of reopening the GGUF manager
    // when a repeated user-model load fails (session one-shot per the
    // load-failure class). Inserted at the ENUM TAIL, immediately before
    // EnumCount, so every pre-existing id keeps its numeric value and
    // EnumCount absorbs +1 (247 -> 248); the P5 "EnumCount - 35 ==
    // LanguageSearchNoMatch" sentinel therefore becomes EnumCount - 36 (the
    // pin-site list for the test update lives in
    // docs/261123_0001_session_gguf-missing-defaults-followup/). The STRUCT
    // slot takes the position right AFTER cheat_sheet_esc_cancel and right
    // BEFORE language_search_auto_chip (the AUTO chip closes the struct tail
    // per the P9b-3 U5 pin, the same move every tail pass made), so the enum
    // order and the struct order differ on purpose; the Get() switch is the
    // mapping. Per-locale fill: Korean is authored; every other designated
    // table carries the English draft verbatim (the deliberate machine-draft
    // baseline of the 261119 policy).
    UserModelLoadFailedHint,      // "사용자 모델을 불러오지 못했습니다. 모델 관리에서 파일과 호환성을 확인하세요." / "Could not load the user model. Check the file and compatibility in Model Manager."
    // R6 Phase 5: sentinel for the table-completeness unit test
    // (every StringId below it must return a non-empty value in all locales
    // exposed by GetSupportedUiLocales() - 89 since UI-language-89 phase 3,
    // 261119_0001 batches 1-4).
    EnumCount
};

// ---- REQ-037 (P4 Batch B-3, design §2.1.3): code-mapping table ----------
//
// One row per selectable UI locale (89 rows: the 37 REQ-037 locales + the 52
// GU..GOM phase-1A rows, all authored since phase 3 - NO Auto row).
// StringToLocale,
// LocaleToString, GetLocaleCode and the two phases of DetectSystemLocale are
// all data-driven from this table, so adding a future locale is a one-row
// change plus a string table (design §2.1.3). The table is exposed
// (not file-static) so TestReq037LocaleMapping can pin row integrity,
// round-trips and prefix hygiene headlessly (design §2.1.6).
struct LocaleMapping {
    UiLocale locale;
    // Canonical PERSISTED spelling in config.ui_language (design §2.1.3);
    // case-insensitive at match time, canonical case in storage/output.
    const char* config_code;     // "ko", "zh-CN", "fil", "no", ...
    // BCP-47 language-subtag prefix for DetectSystemLocale phase 1
    // (GetUserDefaultLocaleName returns e.g. "ko-KR"). Matched on the
    // hyphen/underscore boundary (design §5.1 R6): subtag == prefix exactly,
    // never a raw starts_with (L"no" must not hit L"nob", L"fi" must not
    // hit L"fil"). The zh rows carry their canonical prefix for the
    // non-empty-prefix assertion, but Chinese detection is resolved by the
    // explicit script-subtag pre-check BEFORE the generic loop (design
    // §2.1.3), so the zh prefixes never participate in boundary matching.
    const wchar_t* bcp47_prefix;   // L"ko" (never empty)
    // Optional second prefix for locales Windows reports under two tags:
    // Norwegian (nb-NO primary, no-* legacy). nullptr when unused.
    const wchar_t* bcp47_prefix_alt;
    // Representative full BCP-47 tag for DWrite CreateTextFormat localeName
    // (design §2-Q5 verdict A; UI-chrome counterpart of LanguageInfo.bcp47 -
    // two owners, two purposes, cross-reference comment in src/config.cpp).
    const wchar_t* bcp47_full;   // L"ko-KR"
    // PRIMARYLANGID for DetectSystemLocale's fallback phase (design §2.1.3).
    WORD langid_primary;
    // Authenticity gate (design §2-Q4.2): false = no authored string table;
    // the locale is withheld from GetSupportedUiLocales() and therefore
    // refused by PlanUiLocaleChange. All 89 tables are authored since
    // UI-language-89 phase 3 (261119_0001 batches 1-4; MT-draft tables count
    // as authored per the standing 261119_0001 decision - the pending
    // human/native review pass is tracked as a release gate, not by this
    // flag). The flag remains as the gate knob for any future locale.
    bool authored;
};

// The 89 mapping rows, in UiLocale enum order (kAllLanguages registry order).
const std::vector<LocaleMapping>& GetLocaleMappings();

// Pure BCP-47 tag -> UiLocale resolution used by DetectSystemLocale phase 1
// (zh script pre-check + boundary-checked prefix loop). Returns UiLocale::Auto
// as the "no match" sentinel (Auto is never produced by detection itself),
// letting the caller fall through to the LANGID phase. Exposed headless for
// TestReq037LocaleMapping (design §2.1.6: nb-NO -> Norwegian, fil -> Filipino,
// unknown -> sentinel).
UiLocale LocaleFromBcp47Tag(std::wstring_view tag);

// ---- R6 Phase 6 (plan §5.4): UI-language selector data + pure change plan ----

// 261119_0001 U1 (UI-language-89 phase 2, architect-pick.md "Pool build"):
// forward declaration ONLY, so this header can name the pool's element type
// without including config.hpp (the registry's owner) - no include cycle,
// and config.hpp's LanguageInfo remains the single definition.
struct LanguageInfo;

// One selectable UI locale: enum value + ENDONYM (the language's own name,
// the standard for language pickers: "한국어", "日本語", "简体中文" ... - these
// are deliberately NOT translated per entry).
struct UiLocaleEntry {
    UiLocale locale;
    const wchar_t* native_name;
};

// The selector locales the authenticity gate currently exposes (all 89
// authored rows: the 37 REQ-037/B-3 locales + the 52 UI-language-89 phase-1A
// locales), built once by joining the
// LocaleMapping table against the kAllLanguages registry (endonym source of
// truth, design §2.1.4) through the authenticity gate. Display order per
// design §2-Q3: the legacy front block (KO, JA, zh-CN, zh-TW, VI, ES) keeps
// its muscle memory, the remaining 30 follow registry order, English last.
const std::vector<UiLocaleEntry>& GetSupportedUiLocales();

// 261119_0001 U1 (UI-language-89 phase 2, architect-pick.md "Pool build"):
// the searchable picker's UI-locale pool. Authored-only - the SAME
// authenticity gate as the selector (89 authored rows since phase 3,
// 261119_0001 batches 1-4) - and engine-mask independent (a UI chrome locale
// is
// offerable regardless of any translation engine's capability). Row 0 is the
// registry AUTO row; every following row is the registry row of one selector
// entry, in the selector's exact display order, so an empty query reproduces
// the retired flat menu's order. Pointers reference the immutable registry
// statics (valid for the process lifetime); read-only from the GUI thread
// and from tests. The pool is emitted by the SAME single join walk as
// GetSupportedUiLocales(), so selector and picker can never drift.
const std::vector<const LanguageInfo*>& GetSupportedUiLocalePickerPool();

// Surfaces RefreshAllUiForLocaleChange (src/main.cpp) must re-render after a
// locale switch, in the plan §5.4 propagation order.
enum class LocaleSurface : unsigned char {
    Tray,    // menu rebuild (rebuild is lazy per ShowContextMenu) + tip update
    Badge,   // SetLanguages nudge -> localized status/language label
    Tooltip, // RequestLocaleRefresh -> re-render while visible
    About,   // RequestLocaleRefresh -> caption + body re-render while visible
};

struct UiLocaleChangePlan {
    std::string persisted_value;          // value to store in config.ui_language
    UiLocale applied = UiLocale::Auto;    // Auto sentinel = resolve via DetectSystemLocale at apply time
    bool valid = false;                   // false = unknown code: apply NOTHING
    bool changed = false;                 // false = same persisted value (views still refresh: self-heal)
    std::vector<LocaleSurface> surfaces;  // empty unless valid
};

// Pure planner (headless-testable per plan §7.2): "auto" selects the system
// locale at apply time; any AUTHORED-locale code is stored verbatim in
// lowercase-insensitive canonical form (design §2.1.3 aliases accepted).
// Values that are not selectable codes - unknown spellings, or a locale
// withheld by the authenticity gate - are REFUSED (valid=false) so config can
// never carry a half-wired locale again (the gate's enforcement point).
UiLocaleChangePlan PlanUiLocaleChange(std::string_view current_persisted,
                                      std::string_view requested);

// Pure overload taking the selector vector explicitly (design §2-Q4.2): the
// gate's enforcement point is the selector loop, so tests pin "a locale
// withheld from the vector is refused" with a SYNTHETIC reduced selector,
// independent of whether every real locale is authored this batch. The
// production entry point above simply forwards GetSupportedUiLocales().
UiLocaleChangePlan PlanUiLocaleChangeWithSelector(const std::vector<UiLocaleEntry>& selector,
                                                  std::string_view current_persisted,
                                                  std::string_view requested);

struct I18nStrings {
    std::wstring menu_ui_theme = L"UI Theme";
    std::wstring theme_fluent = L"Fluent Craft";
    std::wstring theme_editorial = L"Warm Editorial";
    std::wstring theme_monolith = L"Refined Monolith";
};

class I18n {
public:
    // Initializes localization state based on config override or Windows OS UI language
    static void Initialize(std::string_view config_ui_lang = "auto");

    // Sets active UI locale
    static void SetLocale(UiLocale locale);

    // Returns currently active UI locale
    static UiLocale GetCurrentLocale();

    // Returns locale identifier (e.g. "ko", "ja", "zh-CN", "zh-TW", "vi", "es",
    // "ar", "my", ... - all 89 canonical codes; design §2.1.3 table-driven).
    static std::string_view GetLocaleCode();
    static std::string GetSystemLanguageCode() { return std::string(GetLocaleCode()); }

    // Returns localized UI string for given StringId
    static std::wstring Get(StringId id);

    // Returns localized UI theme strings bundle
    static I18nStrings GetThemeStrings();

    // Returns localized language display name for a given language code (e.g. "KO" -> "한국어 (Korean)")
    static std::wstring GetLanguageDisplayName(std::string_view lang_code);

    // Detects Windows system UI language via GetUserDefaultLocaleName() / GetUserDefaultUILanguage()
    static UiLocale DetectSystemLocale();

    // Conversion helpers
    static UiLocale StringToLocale(std::string_view str);
    static std::string LocaleToString(UiLocale locale);

    // Windows Startup registry helpers
    static bool IsStartWithWindowsEnabled();
    static void SetStartWithWindows(bool enable);
};

} // namespace emebalachat
