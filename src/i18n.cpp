#include "i18n.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic> // R6 Phase 6 (plan §5.4): race-free runtime locale switching
#include <cctype>
#include <deque>  // REQ-037 B-3: persistent endonym storage for the selector join
#include <unordered_map>
#include <cwctype> // LocaleFromBcp47Tag: std::towlower for ASCII/BCP-47 case-fold

namespace emebalachat {

namespace {

// R6 Phase 6 (plan §5.4): the UI-language selector mutates the active locale at
// RUNTIME while the hook thread may concurrently read I18n::Get for its
// toggle bubble. A plain global was a data race; the enum-sized atomic is
// lock-free on MSVC x64.
std::atomic<UiLocale> s_current_locale{UiLocale::English};

const wchar_t kRunRegistryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Compat token: intentionally NOT rebranded to "Emebala Chat". Installed 0.9.x
// builds wrote this exact value name under HKCU\...\Run; renaming it would
// orphan (duplicate) existing auto-start entries on upgrade. Not user-visible.
const wchar_t kRunValueName[] = L"Emebalachat";

// REQ-044 (P3 item 4, option b — Tech Gate E-3a): single-source the field
// list with an X-macro so the struct layout and its field count can never
// drift apart. One entry per field, IN DECLARATION ORDER. The struct and
// kLocalizedStringsFieldCount below are both generated FROM this list, so
// adding/removing a field here is the ONLY edit needed and the
// static_assert keeps all 37 locale tables honest.
#define EMEBALA_LSTR_FIELDS(X) \
    X(menu_status_active) \
    X(menu_status_paused) \
    X(menu_engine) \
    X(menu_engine_google) \
    X(menu_engine_local) \
    X(menu_source_lang) \
    X(menu_target_lang) \
    X(menu_swap_langs) \
    X(menu_auto_send) \
    X(menu_sound_feedback) \
    X(menu_show_badge) \
    X(menu_start_with_windows) \
    X(menu_cheatsheet) \
    X(menu_exit) \
    X(menu_about) \
    X(cheatsheet_title) \
    X(cheatsheet_body) \
    X(about_title) \
    X(badge_active) \
    X(badge_translating) \
    X(badge_paused) \
    X(tooltip_title) \
    X(tooltip_copy_failed) \
    X(tooltip_no_selection) \
    X(auto_detect) \
    X(app_already_running) \
    X(app_com_failed) \
    X(about_tagline) \
    X(about_feature0) \
    X(about_feature1) \
    X(about_feature2) \
    X(about_etymology) \
    X(about_link_website) \
    X(about_link_contact) \
    X(about_link_reddit) \
    X(about_contact_org) \
    X(about_contact_phone) \
    X(about_contact_lead) \
    X(tooltip_copied) \
    X(tooltip_button_copy) \
    X(tooltip_button_tts) \
    X(menu_ui_language) \
    X(menu_ui_language_auto) \
    X(about_reset_button) \
    X(about_reset_done) \
    X(menu_typing_group) \
    X(menu_tooltip_group) \
    X(app_name) \
    X(tooltip_no_tts_voice) \
    X(privacy_notice_title) \
    X(privacy_notice_body) \
    X(cheatsheet_config_path) \
    X(translate_truncated_notice) \
    X(tooltip_untranslated_above) \
    X(repair_in_progress) \
    X(repair_failed_title) \
    X(repair_failed_body) \
    X(menu_engine_openai) \
    X(openai_settings_title) \
    X(openai_settings_action) \
    X(openai_base_url_label) \
    X(openai_api_key_label) \
    X(openai_model_label) \
    X(openai_fetch_models) \
    X(openai_fetch_failed) \
    X(openai_http_warning_title) \
    X(openai_http_warning_body) \
    X(openai_saved) \
    X(openai_key_masked) \
    X(openai_invalid_base_url) \
    X(menu_engine_user_gguf) \
    X(menu_browse_gguf_file) \
    X(user_gguf_quality_title) \
    X(user_gguf_quality_body) \
    X(user_gguf_registered_title) \
    X(user_gguf_registered_body) \
    X(user_gguf_bundled_duplicate_body) \
    X(menu_engine_user_gguf_empty) \
    X(menu_manage_gguf_models) \
    X(gguf_manager_title) \
    X(gguf_manager_empty) \
    X(gguf_manager_rename) \
    X(gguf_manager_delete) \
    X(gguf_manager_close) \
    X(gguf_manager_delete_confirm_title) \
    X(gguf_manager_delete_confirm_body) \
    X(gguf_manager_rename_title) \
    X(gguf_manager_rename_body) \
    X(gguf_manager_rename_invalid) \
    X(gguf_manager_done) \
    X(gguf_manager_err_serialize) \
    X(gguf_manager_err_no_localappdata) \
    X(gguf_manager_err_write) \
    X(gguf_manager_err_write_partial) \
    X(gguf_manager_err_registry_damaged) \
    X(tooltip_translate_failed) \
    X(repair_transient_body) \
    X(dialog_ok) \
    X(dialog_cancel) \
    X(gguf_manager_add_file) \
    X(gguf_manager_add_hf) \
    X(hf_add_title) \
    X(hf_url_label) \
    X(hf_downloading) \
    X(hf_failed) \
    X(hf_invalid_url) \
    X(hf_done) \
    X(openai_base_url_hint) \
    X(openai_api_key_hint) \
    X(openai_delete_settings) \
    X(update_check) \
    X(update_idle_note) \
    X(update_checking) \
    X(update_checking_hint) \
    X(update_uptodate) \
    X(update_dlg_title) \
    X(update_dlg_version) \
    X(update_dlg_version_cur) \
    X(update_dlg_size) \
    X(update_dlg_size_approx) \
    X(update_dlg_privacy) \
    X(update_dlg_download) \
    X(update_dlg_later) \
    X(update_dl_progress) \
    X(update_dl_cancel) \
    X(update_dl_cancelled) \
    X(update_ready_title) \
    X(update_ready_warn) \
    X(update_ready_install) \
    X(update_ready_later) \
    X(update_err_offline) \
    X(update_err_rate) \
    X(update_err_hash) \
    X(update_err_retry) \
    X(update_snoozed_line) \
    X(model_dl_caption) \
    X(model_dl_consent_body) \
    X(model_dl_privacy) \
    X(model_dl_download) \
    X(model_dl_verifying) \
    X(model_dl_placing) \
    X(model_dl_done) \
    X(model_dl_failed) \
    X(model_dl_disk_space) \
    X(engine_load_settings_menu_row) \
    X(engine_load_settings_title) \
    X(load_mode_on_use) \
    X(load_mode_warmup) \
    X(load_mode_on_use_desc) \
    X(load_mode_warmup_desc) \
    X(gguf_policy_sticky) \
    X(gguf_policy_evict) \
    X(gguf_manager_toggle_policy) \
    X(gguf_duplicate_warning) \
    X(gguf_manager_dedupe) \
    X(gguf_manager_dedupe_done) \
    X(gguf_manager_dedupe_failed)

struct LocalizedStrings {
#define EMEBALA_LSTR_FIELD(name) const wchar_t* name;
    EMEBALA_LSTR_FIELDS(EMEBALA_LSTR_FIELD)
#undef EMEBALA_LSTR_FIELD
    const wchar_t* menu_ui_theme = L"UI Theme";
    const wchar_t* theme_fluent = L"Fluent Craft";
    const wchar_t* theme_editorial = L"Warm Editorial";
    const wchar_t* theme_monolith = L"Refined Monolith";
    // 261108_0002 (WS3, v0.10.6): floating back-translation verification card
    // + tray toggle. TRAILING default-initialized fields, exactly like the
    // four theme strings above: they are NOT part of EMEBALA_LSTR_FIELDS, so
    // kLocalizedStringsFieldCount (157) and the X-macro stay untouched, and
    // the 37 locale tables carry their own explicit designators below.
    // FIX-WS3B added the fifth (back_translate_reference): the card's
    // reference-line label, which used to be a hardcoded Korean literal.
    const wchar_t* menu_back_translation = L"Floating Back-Translation";
    const wchar_t* back_translate_header = L"🔄 Back-Translation";
    const wchar_t* back_translate_match_exact = L"✓ Meaning Matched";
    const wchar_t* back_translate_match_check = L"⚡ Check Nuance";
    // 261108_0002 (FIX-WS3B, v0.10.6): the card's reference-line label
    // ("↳ 원문: " in Korean). Trailing default like its four siblings.
    const wchar_t* back_translate_reference = L"↳ Original: ";
    // 261111_0001 U6 (v0.10.7, R1 P1): user-model surface completeness —
    // trailing defaults like the back-translation block above (NOT part of
    // EMEBALA_LSTR_FIELDS; the 37 tables carry their own designators).
    const wchar_t* gguf_invalid_model_body =
        L"Not a valid GGUF model file; it was not registered.";
    const wchar_t* gguf_manager_active_suffix = L"(in use)";
    const wchar_t* gguf_manager_activated = L"This model is now in use.";
};

// Compile-time field count — generated from the SAME X-macro list above,
// so it can never disagree with the struct layout. Fires the moment a
// field is added or removed (Tech Gate E-3a pattern).
inline constexpr std::size_t kLocalizedStringsFieldCount =
    [] { std::size_t n = 0;
#define EMEBALA_LSTR_COUNT(name) ++n;
         EMEBALA_LSTR_FIELDS(EMEBALA_LSTR_COUNT)
#undef EMEBALA_LSTR_COUNT
         return n; }();
// Running total: 57 base fields (REQ-044) + 13 OpenAI (REQ-045 P4-3, 70)
// + 6 user-.gguf (REQ-045 P4-5, 76) + 1 bundled-duplicate body (REQ-047 D2,
// 77) + 1 "(미등록)" tray marker (REQ-047 U1, 78) + 17 gguf-manager family
// (REQ-048 R2-D: 12 manager strings + 5 manager-error strings, 95)
// + tooltip_translate_failed + repair_transient_body (97) + dialog OK/Cancel
// (REQ-050, 99) + the merged-manager Hugging Face block (REQ-050: 2 add-
// method buttons + 6 HF dialog strings, 107) + the OpenAI settings cue-banner
// hints (REQ-050: base-URL + API-key edit placeholders, 109) + the OpenAI
// settings delete-button caption (REQ-051, 110) + the update-checker About
// zone block (REQ-UC 260930_0004: 25 strings, 135) + the on-demand
// model download dialog (REQ-MD: 9 strings, 144) + the REQ-060 load-mode
// block (session 261009_0001: 6 engine-host load-mode strings + 7 manager
// policy/duplicate strings, 157). The Get()
// switch maps exactly these 157 named fields.
static_assert(kLocalizedStringsFieldCount == 157,
    "LocalizedStrings field count changed - update all 37 locale tables");

// 261108_0002 (FIX-R1, v0.10.6): the REAL field-count invariant. The assert
// above is generated from the same X-macro that generates the struct, so it can
// never notice the 8 trailing NON-X-macro members (4 theme + 4 back-translation)
// - and the test-side mirror in tests/run_tests.cpp is a hand-kept copy that
// catches drift only after someone remembers to update it. This one compares the
// actual struct layout against StringId::EnumCount (the Get() switch's case
// count, i18n.hpp): one member per pointer-sized slot means
// sizeof / sizeof(const wchar_t*) IS the member count. Every LocalizedStrings
// member is a const wchar_t* (X-macro fields + the 8 trailing), so the divisor
// is exact; adding a member of another type would change the quotient and trip
// this assert, which is the intended signal to revisit both sides.
// Fires when: a member is added/removed without a StringId, or a StringId is
// added without a member - the two drift modes the old mirror could not see.
static_assert(sizeof(LocalizedStrings) / sizeof(const wchar_t*) ==
                  static_cast<std::size_t>(StringId::EnumCount),
    "LocalizedStrings member count must equal StringId::EnumCount");

// 1. Korean (ko)
// REQ-044 (P3 item 4, option b — Tech Gate E-3b): designated-initializer
// pilot table. All 57 fields are named, in declaration order, so a future
// reorder/typo is a compile error instead of a silent text shift. The string
// literals are the SAME bytes as the previous positional aggregate.
const LocalizedStrings kStringsKorean = {

    .menu_status_active = L"상태: 활성 (F9: 일시 정지)",
    .menu_status_paused = L"상태: 일시 정지 (F9: 활성화)",
    .menu_engine = L"번역 엔진 선택",
    .menu_engine_google = L"Google 번역 (무료 / 무설치 / 실시간)",
    .menu_engine_local = L"내장 로컬 엔진 (Hy-MT2-1.8B 오프라인)",
    .menu_source_lang = L"출발 언어 (입력 언어)",
    .menu_target_lang = L"도착 언어 (번역 대상)",
    .menu_swap_langs = L"출발어 ⇄ 도착어 맞교환 (더블클릭)",
    .menu_auto_send = L"엔터 시 자동 전송 (Auto-Send)",
    .menu_sound_feedback = L"알림음 효과 (Tones)",
    .menu_show_badge = L"화면 플로팅 뱃지 표시",
    .menu_start_with_windows = L"Windows 시작 시 자동 실행",
    .menu_cheatsheet = L"단축키 안내 및 사용법 (도움말)...",
    .menu_exit = L"에메발라 챗 종료",
    .menu_about = L"에메발라 챗 소개…",
    .cheatsheet_title = L"에메발라 챗 단축키 및 사용 안내",
    .cheatsheet_body = L"에메발라 챗 단축키 및 간편 사용법:\n\n"
    L"  • F9 : 활성화 / 일시 정지 토글 (마우스 클릭으로도 가능)\n"
    L"  • Ctrl + F9 : 도착어(번역 대상 언어) 순환 변경\n"
    L"  • Ctrl + Shift + Enter : 자동 전송 모드 토글\n"
    L"  • Shift + Enter : 번역 후 엔터 즉시 전송\n\n"
    L"마우스 플로팅 뱃지 편의 기능:\n"
    L"  • 뱃지 좌클릭 : 활성화 / 일시 정지 토글\n"
    L"  • 뱃지 더블클릭 : 출발어 ⇄ 도착어 언어 맞교환\n"
    L"  • 뱃지 우클릭 : 트레이 메뉴를 화면에 즉시 열기 (설정 간편 변경)\n\n"
    L"동작 모드:\n"
    L"  • 일반 모드 (자동 전송 꺼짐): 번역문으로 문장을 치환 후 확인하고 엔터 전송.\n"
    L"  • 자동 전송 모드 (자동 전송 켜짐): 번역문으로 치환 후 즉시 자동 전송.",
    .about_title = L"에메발라 챗 소개",
    .badge_active = L"활성",
    .badge_translating = L"번역 중...",
    .badge_paused = L"일시 정지",
    .tooltip_title = L"에메발라 챗",
    .tooltip_copy_failed = L"선택한 텍스트를 복사하지 못했습니다. 대상 앱을 확인하고 다시 시도하세요.",
    .tooltip_no_selection = L"번역할 텍스트가 선택되어 있지 않습니다.",
    .auto_detect = L"자동 감지",
    .app_already_running = L"에메발라 챗이 백그라운드에서 이미 실행 중입니다.\n시스템 알림 트레이를 확인하세요.",
    .app_com_failed = L"COM 초기화에 실패했습니다.\n플로팅 배지와 음성 읽기(TTS)는 사용할 수 없지만,\n번역, 단축키, 트레이, 알림음은 계속 동작합니다.",
    .about_tagline = L"복사·붙여넣기는 이제 그만. 모국어로 자연스럽게 입력하면 어떤 Windows 앱에서든 실시간으로 번역문이 타이핑을 대체합니다.",
    .about_feature0 = L"⚡ 드래그 번역 — 어떤 앱에서든 텍스트를 선택하면 플로팅 아이콘이 즉시 번역합니다.",
    .about_feature1 = L"🔊 뉴럴 TTS — Windows 음성팩 연동 시 37개 전 언어 발음 지원.",
    .about_feature2 = L"🔒 100% 온디바이스·프라이빗 — 단축키를 누르는 동안만 작동하며 클립보드는 건드리지 않습니다.",
    .about_etymology = L"기원전 2000년, 메소포타미아 서기들은 언어로 세계를 잇는 자들을 '에메-발라(Eme-bala)'라 불렀습니다.",
    .about_link_website = L"웹사이트",
    .about_link_contact = L"문의",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · 서울 영등포 (영중로 65, 219호)",
    .about_contact_phone = L"+82 2 575 0414 · 업무시간 10:00–19:00 KST",
    .about_contact_lead = L"총괄 아키텍트: Yongtai Kim",
    .tooltip_copied = L"✓ 복사됨!",
    .tooltip_button_copy = L"📋 복사",
    .tooltip_button_tts = L"🔊 음성",
    .menu_ui_language = L"인터페이스 언어",
    .menu_ui_language_auto = L"자동 (시스템 언어)",
    .about_reset_button = L"시스템 기본값으로 리셋",
    .about_reset_done = L"기본값으로 복원됨",
    .menu_typing_group = L"키보드 타이핑",
    .menu_tooltip_group = L"번역 툴팁",
    .app_name = L"에메발라 챗",
    .tooltip_no_tts_voice = L"이 언어에 설치된 Windows 음성이 없습니다. 🔊를 다시 클릭하면 음성 설정이 열립니다.",
    .privacy_notice_title = L"개인정보 보호 안내",
    .privacy_notice_body = L"에메발라 챗의 개인정보 처리 원칙을 알려드립니다.\n"
    L"\n"
    L"• 에메발라 챗은 자체 서버를 운영하지 않습니다.\n"
    L"• 로컬 모델을 사용하면 번역 내용이 기기를 벗어나지 않습니다.\n"
    L"• Google 번역 엔진을 선택하거나 자동 전환된 경우, 선택하거나 입력한 텍스트가 에메발라를 거치지 않고 Google로 직접 전송되어 번역에 사용됩니다.\n"
    L"• 진단 로그는 기본 꺼짐(OFF) 상태이며, 설정에서 켜는 옵트인 기능입니다.\n"
    L"• 텍스트를 클라우드(Google)로 전송하기 원치 않으시면 트레이 아이콘 메뉴의 “번역 엔진 선택”에서 “내장 로컬 엔진”을 선택하세요. 로컬 모델이 설치되지 않았고 클라우드 전환이 꺼져 있으면 번역은 전송 없이 동작하지 않습니다.\n"
    L"\n"
    L"전체 내용은 README 파일을 참고하세요. 언제든 다시 읽으실 수 있습니다.\n",
    .cheatsheet_config_path = L"설정 파일: %LOCALAPPDATA%\\Emebalachat\\config.json",
    .translate_truncated_notice = L"텍스트가 너무 길어 앞부분과 뒷부분만 번역했습니다.",
    .tooltip_untranslated_above = L"위쪽에 번역되지 않은 새 텍스트가 있습니다. 그 줄 끝에 커서를 두고 Enter를 누를면 번역됩니다.",
    .repair_in_progress = L"로컬 엔진 구성 요소를 복구하는 중…",
    .repair_failed_title = L"로컬 번역을 사용할 수 없습니다",
    .repair_failed_body = L"로컬 번역 엔진 파일이 없어 번역이 일시 중단되었습니다. 에메발라 챗을 재설치하면 로컬 엔진을 복구할 수 있습니다. 또는 클라우드(Google) 번역으로 전환하려면 트레이 메뉴의 \"번역 엔진 선택\"에서 \"Google 번역\"을 선택하세요.",
    .menu_engine_openai = L"OpenAI 호환 (사용자 지정 서버)…",
    .openai_settings_title = L"OpenAI 호환 엔진 설정",
    .openai_settings_action = L"OpenAI 호환 엔진 설정…",
    .openai_base_url_label = L"기본 URL (Base URL)",
    .openai_api_key_label = L"API 키",
    .openai_model_label = L"모델 (Model)",
    .openai_fetch_models = L"모델 목록 가져오기",
    .openai_fetch_failed = L"모델 목록을 가져오지 못했습니다. 모델 이름을 직접 입력할 수 있습니다.",
    .openai_http_warning_title = L"보안되지 않은 연결 (HTTP)",
    .openai_http_warning_body = L"기본 URL이 HTTP(암호화되지 않음)입니다. API 키와 텍스트가 평문으로 전송됩니다. 계속하시겠습니까?",
    .openai_saved = L"OpenAI 호환 설정이 저장되었습니다.",
    .openai_key_masked = L"저장된 키: ",
    .openai_invalid_base_url = L"기본 URL이 올바르지 않습니다. 예: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    // REQ-047 U1 (designer 164500): dynamic labels — "지정" wording, no "…"
    // on the checkable entry; the browse row is an action ("등록").
    .menu_engine_user_gguf = L"사용자 지정 모델 (.gguf)",
    .menu_browse_gguf_file = L"다른 .gguf 모델 등록…",
    .user_gguf_quality_title = L"번역 품질 안내",
    .user_gguf_quality_body = L"선택한 모델은 Hy-MT2가 아닙니다. 현재 버전은 Hy-MT2 전용 프롬프트를 사용하므로, 이 모델의 번역 품질은 보장되지 않습니다. 계속하시겠습니까?",
    // REQ-046 P4-2 (Rev2 §B-5, C2): '로컬 LLM 선택' 안내 제거(등록으로
    // 즉시 user_gguf 상태가 되므로) + 적용 지연 상한 명시(최대 1분, 유휴
    // 엔진 종료 후 자동 반영).
    .user_gguf_registered_title = L"모델 등록 완료",
    .user_gguf_registered_body = L"선택한 모델이 로컬 엔진에 등록되었습니다.\n\n이 모델은 \"번역 엔진 선택 > 사용자 선택(.gguf)\"으로 선택하면 번역에 사용됩니다.\n\n적용 시점: 등록 후 최대 약 1분(엔진 유휴 종료 후)에 새 모델이 적용됩니다. 이전 번역 요청까지는 기존 모델이 사용될 수 있습니다.",
    // REQ-047 D2 (design §B.3, Rev2 §6 용어 완화): '로컬 LLM' 직접 인용
    // 없이 기능 서술 — 내장 로컬 번역 엔진을 직접 선택하라는 안내.
    .user_gguf_bundled_duplicate_body = L"이 모델은 에메발라 챗에 이미 내장되어 있습니다. 별도의 등록은 필요 없습니다. 내장 로컬 번역 엔진을 직접 선택하시면 바로 사용할 수 있습니다.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(미등록)",
    // REQ-048 R2-D: 등록된 사용자 .gguf 모델 관리(이름 바꾸기/삭제). 삭제 확인
    // 본문은 .gguf 파일 자체는 디스크에 유지됨을 명시(수 GB 파일 자동 삭제 금지).
    .menu_manage_gguf_models = L"모델 관리…",
    .gguf_manager_title = L"사용자 모델 관리",
    .gguf_manager_empty = L"등록된 사용자 모델이 없습니다.",
    .gguf_manager_rename = L"이름 바꾸기…",
    .gguf_manager_delete = L"삭제…",
    .gguf_manager_close = L"닫기",
    .gguf_manager_delete_confirm_title = L"모델 등록 삭제",
    .gguf_manager_delete_confirm_body = L"선택한 모델의 등록이 삭제됩니다.\n"
    L"\n"
    L"모델 파일(.gguf)은 디스크에 유지되며 삭제되지 않습니다. 이 모델을 사용 중이었다면 번역 엔진 선택이 자동(Auto)으로 돌아갑니다.\n"
    L"\n"
    L"계속하시겠습니까?",
    .gguf_manager_rename_title = L"모델 이름 바꾸기",
    .gguf_manager_rename_body = L"새 이름을 입력하세요. (공백 없이 64자 이내)",
    .gguf_manager_rename_invalid = L"사용할 수 없는 이름입니다. 비어 있지 않고 기존 이름과 다르게, 공백/경로 구분자 없이 64자 이내로 입력하세요.",
    .gguf_manager_done = L"변경사항이 저장되었습니다.",
    .gguf_manager_err_serialize = L"registry.json을 직렬화할 수 없습니다(파일명이 거부됨). 변경된 것은 없습니다.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA%를 사용할 수 없어 공유 모델 폴드를 찾을 수 없습니다.",
    .gguf_manager_err_write = L"registry.json을 쓸 수 없습니다.",
    .gguf_manager_err_write_partial = L"registry.json을 끝까지 쓰지 못했습니다.",
    .gguf_manager_err_registry_damaged = L"registry.json이 손상되었거나 지원하지 않는 스키마입니다. 수정하지 않았습니다. 복구하거나 삭제한 후 다시 시도하세요.",
    .tooltip_translate_failed = L"번역에 실패했습니다. 다시 시도해 주세요.",
    .repair_transient_body = L"로컬 번역 엔진을 일시적으로 사용할 수 없습니다(시작 중일 수 있습니다). 잠시 후 다시 시도해 주세요.",
    // REQ-050: 대화상자 공통 버튼 레이블(확인/취소).
    .dialog_ok = L"확인",
    .dialog_cancel = L"취소",
    // REQ-050: 통합 모델 관리자 — 상단 추가 방법 2종(파일 / Hugging Face)과
    // HF 추가 대화상자 문자열. 취소 버튼은 dialog_cancel 재사용, 쓰기/폴드
    // 오류는 기존 gguf_manager_err_* 재사용.
    .gguf_manager_add_file = L"파일에서 추가…",
    .gguf_manager_add_hf = L"Hugging Face에서 추가…",
    .hf_add_title = L"Hugging Face 모델 추가",
    .hf_url_label = L"모델 URL",
    .hf_downloading = L"다운로드 중…",
    .hf_failed = L"다운로드에 실패했습니다.",
    .hf_invalid_url = L"huggingface.co 모델 주소를 입력하세요 (…/resolve/main/파일.gguf 또는 모델 페이지 주소)",
    .hf_done = L"등록되었습니다.",
    // REQ-050: OpenAI settings cue banners (edit placeholder hints).
    .openai_base_url_hint = L"예: https://api.openai.com/v1",
    .openai_api_key_hint = L"API 키",
    // REQ-051 (Symptom D): 설정 다이얼로그 [설정 삭제] 버튼 캡션.
    .openai_delete_settings = L"설정 삭제",
    // REQ-UC (260930_0004, 업데이트 확인 About 존 — 디자인 §12 마스터 문자열표).
    .update_check = L"업데이트 확인",
    .update_idle_note = L"Emebala Chat이 최신인지 확인합니다.",
    .update_checking = L"업데이트 확인 중…",
    .update_checking_hint = L"백그라운드에서 실행됩니다 — 이 창을 닫아도 됩니다.",
    .update_uptodate = L"최신 버전을 사용 중입니다",
    .update_dlg_title = L"업데이트를 사용할 수 있습니다",
    .update_dlg_version = L"새 버전",
    .update_dlg_version_cur = L"현재 {v}",
    .update_dlg_size = L"크기",
    .update_dlg_size_approx = L"약 {s}",
    .update_dlg_privacy = L"확인/다운로드 시 GitHub에 연결됩니다.",
    .update_dlg_download = L"다운로드",
    .update_dlg_later = L"나중에",
    .update_dl_progress = L"다운로드 중…",
    .update_dl_cancel = L"취소",
    .update_dl_cancelled = L"다운로드가 취소되었습니다",
    .update_ready_title = L"다운로드 완료, 설치합니다",
    .update_ready_warn = L"설치를 시작하면 앱이 종료됩니다.",
    .update_ready_install = L"지금 설치",
    .update_ready_later = L"나중에",
    .update_err_offline = L"오프라인 상태입니다. 인터넷 연결을 확인한 후 다시 시도하세요.",
    .update_err_rate = L"확인 요청이 너무 많습니다. 잠시 후 다시 시도하세요.",
    .update_err_hash = L"다운로드한 파일이 손상되었습니다",
    .update_err_retry = L"다시 시도",
    .update_snoozed_line = L"업데이트를 나중에 설치할 수 있습니다",
    // REQ-MD (260930_0004, update-checker design §12 계열): 모델 다운로드 대화상자.
    .model_dl_caption = L"모델 다운로드",
    .model_dl_consent_body = L"내장 번역 모델이 설치되어 있지 않습니다. 지금 다운로드하시겠습니까? (약 {s})",
    .model_dl_privacy = L"다운로드 시 huggingface.co에 연결됩니다.",
    .model_dl_download = L"지금 다운로드",
    .model_dl_verifying = L"다운로드한 파일을 확인하는 중…",
    .model_dl_placing = L"모델을 설치하는 중…",
    .model_dl_done = L"모델이 준비되었습니다. 로컬 엔진이 다음 번역부터 사용합니다.",
    .model_dl_failed = L"다운로드에 실패했습니다. 연결을 확인하고 다시 시도하세요.",
    .model_dl_disk_space = L"디스크 여유 공간이 부족합니다 (약 {s} 필요).",
    // REQ-060 (session 261009_0001): 번역 모델 로드 설정 + 모델 관리 정책/중복 블록.
    .engine_load_settings_menu_row = L"번역 모델 로드 설정…",
    .engine_load_settings_title = L"번역 모델 로드",
    .load_mode_on_use = L"사용할 때 로드 (권장)",
    .load_mode_warmup = L"미리 로드",
    .load_mode_on_use_desc = L"메모리를 아껴요. 첫 번째 번역이 잠깐 늦어질 수 있어요.",
    .load_mode_warmup_desc = L"번역이 바로 시작돼요. 모델이 메모리에 계속 올라가 있어요.",
    .gguf_policy_sticky = L"(상주 유지)",
    .gguf_policy_evict = L"(사용 시 로드)",
    .gguf_manager_toggle_policy = L"정책 전환",
    .gguf_duplicate_warning = L"중복 파일: {n} {s} × {c}",
    .gguf_manager_dedupe = L"중복 정리",
    .gguf_manager_dedupe_done = L"중복 파일을 휴지통으로 옮겼습니다.",
    .gguf_manager_dedupe_failed = L"중복 파일을 정리하지 못했습니다. 파일이 다른 프로그램에서 사용 중인지 확인하세요.",
    .menu_ui_theme = L"디자인 테마",
    .theme_fluent = L"플루언트 크래프트 (기본)",
    .theme_editorial = L"웜 에디토리얼",
    .theme_monolith = L"리파인드 모놀리스",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"역번역 플로팅 표시 (번역 검증)",
    .back_translate_header = L"🔄 역번역 확인",
    .back_translate_match_exact = L"✓ 원문 일치",
    .back_translate_match_check = L"⚡ 표현 확인",
    .back_translate_reference = L"↳ 원문: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"올바른 GGUF 모델 파일이 아니어서 등록하지 않았습니다.",
    .gguf_manager_active_suffix = L"(사용 중)",
    .gguf_manager_activated = L"사용 중인 모델로 설정했습니다.",
};
// 2. Japanese (ja)
const LocalizedStrings kStringsJapanese = {
    .menu_status_active = L"状態: 有効 (F9: 一時停止)",
    .menu_status_paused = L"状態: 一時停止 (F9: 再開)",
    .menu_engine = L"翻訳エンジンの選択",
    .menu_engine_google = L"Google 翻訳 (無料 / インストール不要)",
    .menu_engine_local = L"内蔵ローカルエンジン (Hy-MT2-1.8B オフライン)",
    .menu_source_lang = L"元の言語 (入力)",
    .menu_target_lang = L"翻訳先言語 (ターゲット)",
    .menu_swap_langs = L"言語を入れ替える (ダブルクリック)",
    .menu_auto_send = L"Enterで自動送信 (Auto-Send)",
    .menu_sound_feedback = L"効果音 (Sound)",
    .menu_show_badge = L"フローティングバッジを表示",
    .menu_start_with_windows = L"Windows 起動時に自動実行",
    .menu_cheatsheet = L"ショートカット案内とヘルプ...",
    .menu_exit = L"エメバラチャット を終了",
    .menu_about = L"エメバラチャット について…",
    .cheatsheet_title = L"エメバラチャット ショートカットと使用案内",
    .cheatsheet_body = L"エメバラチャット ショートカットと使用案内:\n\n"
    L"  • F9 : 有効 / 一時停止の切り替え\n"
    L"  • Ctrl + F9 : 翻訳先言語を切り替える\n"
    L"  • Ctrl + Shift + Enter : 自動送信モードの切り替え\n"
    L"  • Shift + Enter : 翻訳後、直ちに送信 (Enter)\n\n"
    L"フローティングバッジのマウス操作:\n"
    L"  • 左クリック : 有効 / 一時停止 切り替え\n"
    L"  • ダブルクリック : 元の言語 ⇄ 翻訳先言語の入れ替え\n"
    L"  • 右クリック : 設定メニューをその場で開く\n\n"
    L"動作モード:\n"
    L"  • 置換のみ (自動送信OFF): 入力行を翻訳文に置き換え、確認後にEnterで送信できます。\n"
    L"  • 自動送信 (自動送信ON): 翻訳文に置き換えた直後、自動的にEnterを送信します。",
    .about_title = L"エメバラチャット について",
    .badge_active = L"有効",
    .badge_translating = L"翻訳中...",
    .badge_paused = L"一時停止",
    .tooltip_title = L"エメバラチャット",
    .tooltip_copy_failed = L"選択したテキストをコピーできませんでした。対象アプリを確認して再試行してください。",
    .tooltip_no_selection = L"翻訳するテキストが選択されていません。",
    .auto_detect = L"自動検出",
    .app_already_running = L"エメバラチャット はすでにバックグラウンドで実行中です。\nシステムトレイを確認してください。",
    .app_com_failed = L"COM の初期化に失敗しました。\nフローティングバッジと音声読み上げは利用できませんが、\n翻訳・ショートカット・トレイ・効果音は引き続き動作します。",
    .about_tagline = L"コピー＆ペーストはもう不要。母語で自然に入力すると、あらゆる Windows アプリの中で打鍵がリアルタイムに翻訳へ置きわります。",
    .about_feature0 = L"⚡ ドラッグ翻訳 — 任意のアプリでテキストを選択すると、フローティングアイコンが即座に翻訳。",
    .about_feature1 = L"🔊 ニューラルTTS — Windows 音声パック導入時、37言語すべての発音に対応。",
    .about_feature2 = L"🔒 100% ローカル・プライバシー — ショートカットを押している間だけ作動し、クリップボードは使いません。",
    .about_etymology = L"紀元前2000年、メソポタミアの書記たちは「エメ＝バラ」——言語で世界を結ぶ者——と呼びました。",
    .about_link_website = L"ウェブサイト",
    .about_link_contact = L"お問い合わせ",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · 営業時間 10:00–19:00 KST",
    .about_contact_lead = L"リードアーキテクト: Yongtai Kim",
    .tooltip_copied = L"✓ コピーしました!",
    .tooltip_button_copy = L"📋 コピー",
    .tooltip_button_tts = L"🔊 読み上げ",
    .menu_ui_language = L"表示言語",
    .menu_ui_language_auto = L"自動 (システム言語)",
    .about_reset_button = L"システム既定値にリセット",
    .about_reset_done = L"既定値に復元しました",
    .menu_typing_group = L"キーボード入力",
    .menu_tooltip_group = L"ドラッグ翻訳ツールチップ",
    .app_name = L"エメバラチャット",
    .tooltip_no_tts_voice = L"この言語用の Windows 音声がインストールされていません。🔊 をもう一度クリックすると音声設定が開きます。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"プライバシーに関するご案内",
    .privacy_notice_body = L"Emebala Chat のプライバシー方針をご案内します。\n"
    L"\n"
    L"• Emebala Chat に独自サーバーはありません。\n"
    L"• ローカルモデル使用時、翻訳内容はデバイスの外に出ません。\n"
    L"• Google 翻訳エンジンを選択した場合、または自動切り替えで作動中は、選択・入力したテキストは Emebala を経由せず Google に直接送信され翻訳に使われます。\n"
    L"• 診断ログは既定で OFF で、設定で ON にするオプトイン機能です。\n"
    L"• テキストをクラウド(Google)に送信したくない場合は、トレイアイコンのメニューで「翻訳エンジンの選択」から「内蔵ローカルエンジン」を選択してください。ローカルモデル未インストールでクラウド切替が無効の場合、翻訳は送信されず動作しません。\n"
    L"\n"
    L"詳細は README ファイルをご覧ください。いつでも再読できます。\n",
    .cheatsheet_config_path = L"設定ファイル: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"テキストが長すぎるため、冒頭と末尾のみを翻訳しました。",
    .tooltip_untranslated_above = L"上に翻訳されていない新しいテキストがあります。その行の末尾にカーソルを置いて Enter を押すと翻訳されます。",
    .repair_in_progress = L"ローカルエンジンコンポーネントを修復しています…",
    .repair_failed_title = L"ローカル翻訳を利用できません",
    .repair_failed_body = L"ローカル翻訳エンジンのファイルが見つからないため、翻訳は一時的に停止しています。Emebala Chat を再インストールするとローカルエンジンを復元できます。または、クラウド（Google）翻訳に切り替えるには、トレイメニューの「翻訳エンジンの選択」から「Google 翻訳」を選んでください。",
    .menu_engine_openai = L"OpenAI 互換 (ユーザー指定サーバー)…",
    .openai_settings_title = L"OpenAI 互換エンジン設定",
    .openai_settings_action = L"OpenAI 互換エンジン設定…",
    .openai_base_url_label = L"ベース URL",
    .openai_api_key_label = L"API キー",
    .openai_model_label = L"モデル",
    .openai_fetch_models = L"モデル一覧を取得",
    .openai_fetch_failed = L"モデル一覧を取得できませんでした。モデル名を直接入力できます。",
    .openai_http_warning_title = L"安全でない接続 (HTTP)",
    .openai_http_warning_body = L"ベース URL が HTTP (暗号化なし) です。API キーとテキストが平文で送信されます。続行しますか？",
    .openai_saved = L"OpenAI 互換設定が保存されました。",
    .openai_key_masked = L"保存済みキー: ",
    .openai_invalid_base_url = L"ベース URL が正しくありません。例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"ユーザー指定モデル (.gguf)",
    .menu_browse_gguf_file = L"別の .gguf モデルを登録…",
    .user_gguf_quality_title = L"翻訳品質の案内",
    .user_gguf_quality_body = L"選択したモデルは Hy-MT2 ではありません。現在のバージョンは Hy-MT2 専用のプロンプトを使用するため、このモデルの翻訳品質は保証されません。続行しますか？",
    .user_gguf_registered_title = L"モデル登録完了",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"選択したモデルがローカルエンジンに登録されました。\n"
    L"\n"
    L"このモデルは「翻訳エンジン選択 > ユーザー選択 (.gguf)」で選択すると翻訳に使用されます。\n"
    L"\n"
    L"反映時刻: 登録後、最大約 1 分 (エンジンがアイドル終了してから)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"このモデルはEmebala Chatにすでに内蔵されています。登録は必要ありません。内蔵のローカル翻訳エンジンを直接選択してご利用ください。",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(未登録)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"モデルの管理…",
    .gguf_manager_title = L"ユーザーモデル管理",
    .gguf_manager_empty = L"登録されたユーザーモデルはありません。",
    .gguf_manager_rename = L"名前の変更…",
    .gguf_manager_delete = L"削除…",
    .gguf_manager_close = L"閉じる",
    .gguf_manager_delete_confirm_title = L"モデル登録の削除",
    .gguf_manager_delete_confirm_body = L"選択したモデルの登録が削除されます。\n"
    L"\n"
    L"モデルファイル(.gguf)はディスクに保持され、削除されません。このモデルを使用中だった場合、翻訳エンジンの選択は自動(Auto)に戻ります。\n"
    L"\n"
    L"続行しますか？",
    .gguf_manager_rename_title = L"モデル名の変更",
    .gguf_manager_rename_body = L"新しい名前を入力してください (空白なし64文字以内)。",
    .gguf_manager_rename_invalid = L"その名前は使用できません。空白やパス区切り文字を含まず、64文字以内で、既存のものと異なる空でない名前を入力してください。",
    .gguf_manager_done = L"変更が保存されました。",
    .gguf_manager_err_serialize = L"registry.json をシリアライズできません(ファイル名が拒否されました)。変更はありません。",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% を使用できず、共有モデルフォルダーが見つかりません。",
    .gguf_manager_err_write = L"registry.json を書き込めません。",
    .gguf_manager_err_write_partial = L"registry.json の書き込みが完了しませんでした。",
    .gguf_manager_err_registry_damaged = L"registry.json が破損しているか、サポートされていないスキーマです。変更は加えていません。修復するか削除してから再試行してください。",
    .tooltip_translate_failed = L"翻訳に失敗しました。もう一度お試しください。",
    .repair_transient_body = L"ローカル翻訳エンジンを一時的に使用できません(起動中の可能性があります)。しばらくしてからもう一度お試しください。",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"決定",
    .dialog_cancel = L"キャンセル",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"ファイルから追加…",
    .gguf_manager_add_hf = L"Hugging Face から追加…",
    .hf_add_title = L"Hugging Face モデルを追加",
    .hf_url_label = L"モデル URL",
    .hf_downloading = L"ダウンロード中…",
    .hf_failed = L"ダウンロードに失敗しました。",
    .hf_invalid_url = L"huggingface.co のモデル URL を入力してください (resolve、blob、またはモデルページ)",
    .hf_done = L"登録しました。",
    .openai_base_url_hint = L"例: https://api.openai.com/v1",
    .openai_api_key_hint = L"APIキー",
    // REQ-051: [設定を削除] ボタン。
    .openai_delete_settings = L"設定を削除",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"更新を確認",
    .update_idle_note = L"最新の Emebala Chat をご利用かどうかを確認します。",
    .update_checking = L"更新を確認しています…",
    .update_checking_hint = L"バックグラウンドで実行されます。このウィンドウを閉じてもかまいません。",
    .update_uptodate = L"最新バージョンをご利用中です",
    .update_dlg_title = L"更新プログラムがあります",
    .update_dlg_version = L"新しいバージョン",
    .update_dlg_version_cur = L"現在のバージョンは {v}",
    .update_dlg_size = L"サイズ",
    .update_dlg_size_approx = L"約 {s}",
    .update_dlg_privacy = L"確認とダウンロードは GitHub に接続します。",
    .update_dlg_download = L"ダウンロード",
    .update_dlg_later = L"後で",
    .update_dl_progress = L"ダウンロード中…",
    .update_dl_cancel = L"キャンセル",
    .update_dl_cancelled = L"ダウンロードはキャンセルされました",
    .update_ready_title = L"ダウンロード完了 — インストールできます",
    .update_ready_warn = L"インストールを開始するとアプリは終了します。",
    .update_ready_install = L"今すぐインストール",
    .update_ready_later = L"後で",
    .update_err_offline = L"オフラインです。接続を確認してからもう一度お試しください。",
    .update_err_rate = L"リクエストが多すぎます。しばらくしてからもう一度お試しください。",
    .update_err_hash = L"ダウンロードしたファイルは検証に合格しませんでした",
    .update_err_retry = L"再試行",
    .update_snoozed_line = L"更新プログラムは後でインストールできます。",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"モデルのダウンロード",
    .model_dl_consent_body = L"内蔵翻訳モデルがインストールされていません。今すぐダウンロードしますか? (約 {s})",
    .model_dl_privacy = L"ダウンロードは huggingface.co に接続します。",
    .model_dl_download = L"今すぐダウンロード",
    .model_dl_verifying = L"ダウンロードしたファイルを確認しています…",
    .model_dl_placing = L"モデルをインストールしています…",
    .model_dl_done = L"モデルの準備ができました。ローカル エンジンが次の翻訳から使用します。",
    .model_dl_failed = L"ダウンロードに失敗しました。接続を確認してもう一度お試しください。",
    .model_dl_disk_space = L"ディスクの空き容量が不足しています (約 {s} 必要)。",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"翻訳モデルの読み込み設定…",
    .engine_load_settings_title = L"翻訳モデルの読み込み",
    .load_mode_on_use = L"使用時に読み込む (推奨)",
    .load_mode_warmup = L"事前に読み込む",
    .load_mode_on_use_desc = L"メモリを節約できます。最初の翻訳が少し遅れることがあります。",
    .load_mode_warmup_desc = L"すぐに翻訳を開始できます。モデルは常にメモリに読み込まれたままです。",
    .gguf_policy_sticky = L"(常駐)",
    .gguf_policy_evict = L"(使用時に読み込み)",
    .gguf_manager_toggle_policy = L"ポリシー切替",
    .gguf_duplicate_warning = L"重複ファイル: {n} {s} × {c}",
    .gguf_manager_dedupe = L"重複を整理",
    .gguf_manager_dedupe_done = L"重複ファイルをごみ箱に移動しました。",
    .gguf_manager_dedupe_failed = L"重複ファイルを整理できませんでした。他のプログラムがファイルを使用していないか確認してください。",
    .menu_ui_theme = L"デザインテーマ",
    .theme_fluent = L"フルーエント・クラフト (標準)",
    .theme_editorial = L"ウォーム・エディトリアル",
    .theme_monolith = L"リファインド・モノリス",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"逆翻訳フローティング表示 (翻訳検証)",
    .back_translate_header = L"🔄 逆翻訳チェック",
    .back_translate_match_exact = L"✓ 原文一致",
    .back_translate_match_check = L"⚡ 表現チェック",
    .back_translate_reference = L"↳ 原文: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"有効なGGUFモデルファイルではないため、登録しませんでした。",
    .gguf_manager_active_suffix = L"(使用中)",
    .gguf_manager_activated = L"使用中のモデルに設定しました。",
};
// 3. Chinese Simplified (zh-CN)
const LocalizedStrings kStringsChineseSimp = {
    .menu_status_active = L"状态: 运行中 (F9: 暂停)",
    .menu_status_paused = L"状态: 已暂停 (F9: 启用)",
    .menu_engine = L"选择翻译引擎",
    .menu_engine_google = L"Google 翻译 (免费 / 免安装 / 极速)",
    .menu_engine_local = L"内置本地引擎 (Hy-MT2-1.8B 离线)",
    .menu_source_lang = L"源语言 (输入语言)",
    .menu_target_lang = L"目标语言 (翻译目标)",
    .menu_swap_langs = L"源语言 ⇄ 目标语言 互换 (双击)",
    .menu_auto_send = L"回车自动发送 (Auto-Send)",
    .menu_sound_feedback = L"声音提示反馈 (Tones)",
    .menu_show_badge = L"显示桌面悬浮徽章",
    .menu_start_with_windows = L"开机自动启动 (Start with Windows)",
    .menu_cheatsheet = L"快捷键与使用说明 (帮助)...",
    .menu_exit = L"退出 埃梅巴拉 翻译",
    .menu_about = L"关于 埃梅巴拉 翻译…",
    .cheatsheet_title = L"埃梅巴拉 翻译 快捷键与使用说明",
    .cheatsheet_body = L"埃梅巴拉 翻译 快捷键与使用说明:\n\n"
    L"  • F9 : 启用 / 暂停 切换 (或点击悬浮徽章)\n"
    L"  • Ctrl + F9 : 循环切换目标语言\n"
    L"  • Ctrl + Shift + Enter : 切换自动发送模式\n"
    L"  • Shift + Enter : 翻译并立即发送 (Enter)\n\n"
    L"悬浮徽章鼠标快捷操作:\n"
    L"  • 左键点击 : 启用 / 暂停 切换\n"
    L"  • 双击徽章 : 源语言 ⇄ 目标语言 快速互换\n"
    L"  • 右键点击 : 立即弹出完整设置菜单\n\n"
    L"工作模式:\n"
    L"  • 仅替换模式 (自动发送关闭): 替换为译文并保留光标，方便发送前检查。\n"
    L"  • 自动发送模式 (自动发送开启): 替换为译文后自动模拟按下 Enter 发送。",
    .about_title = L"关于 埃梅巴拉 翻译",
    .badge_active = L"运行中",
    .badge_translating = L"翻译中...",
    .badge_paused = L"已暂停",
    .tooltip_title = L"埃梅巴拉 翻译",
    .tooltip_copy_failed = L"无法复制所选文本。请检查目标应用后重试。",
    .tooltip_no_selection = L"未选择要翻译的文本。",
    .auto_detect = L"自动检测",
    .app_already_running = L"埃梅巴拉 翻译 已在后台运行。\n请查看系统通知托盘。",
    .app_com_failed = L"COM 初始化失败。\n悬浮徽章和语音朗读将不可用，\n但翻译、快捷键、托盘和提示音仍可正常使用。",
    .about_tagline = L"告别复制粘贴。用母语自然输入，译文会在任何 Windows 应用中实时替换你的键入。",
    .about_feature0 = L"⚡ 拖拽翻译 — 在任意应用中选中文本，悬浮图标即刻翻译。",
    .about_feature1 = L"🔊 神经 TTS — 接入 Windows 语音包后支持全部 37 种语言发音。",
    .about_feature2 = L"🔒 100% 本地运行且私密 — 仅在按住快捷键时生效，不触碰剪贴板。",
    .about_etymology = L"公元前 2000 年，美索不达米亚的书吏称那些以语言连通世界的人为 Eme-bala。",
    .about_link_website = L"官网",
    .about_link_contact = L"联系",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · 办公时间 10:00–19:00 KST",
    .about_contact_lead = L"首席架构师：Yongtai Kim",
    .tooltip_copied = L"✓ 已复制！",
    .tooltip_button_copy = L"📋 复制",
    .tooltip_button_tts = L"🔊 朗读",
    .menu_ui_language = L"界面语言",
    .menu_ui_language_auto = L"自动（系统语言）",
    .about_reset_button = L"重置为系统默认值",
    .about_reset_done = L"已恢复默认值",
    .menu_typing_group = L"键盘输入",
    .menu_tooltip_group = L"划译工具提示",
    .app_name = L"埃梅巴拉 翻译",
    .tooltip_no_tts_voice = L"此语言未安装 Windows 语音。再次点击 🔊 可打开语音设置。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"隐私保护说明",
    .privacy_notice_body = L"请阅读 Emebala Chat 的隐私处理原则。\n"
    L"\n"
    L"• Emebala Chat 不运营任何自有服务器。\n"
    L"• 使用本地模型时，翻译内容不会离开您的设备。\n"
    L"• 选择 Google 翻译引擎或自动切换到云端时，所选或输入的文本将直接发送给 Google 进行翻译，不经过 Emebala。\n"
    L"• 诊断日志默认关闭（OFF），需在设置中手动开启（选择性加入）。\n"
    L"• 如不希望将文本发送至云端 (Google)，请在系统托盘图标菜单的“选择翻译引擎”中选择“内置本地引擎”。若未安装本地模型且已关闭云端回退，翻译将不会被发送，也不会运行。\n"
    L"\n"
    L"完整说明请查看 README 文件，您可随时重新阅读。\n",
    .cheatsheet_config_path = L"配置文件: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"文本过长，仅翻译了开头和结尾部分。",
    .tooltip_untranslated_above = L"上方有未翻译的新文本。将光标置于该行末尾并按 Enter 即可翻译。",
    .repair_in_progress = L"正在修复本地引擎组件…",
    .repair_failed_title = L"本地翻译不可用",
    .repair_failed_body = L"找不到本地翻译引擎文件，翻译已暂时停止。重新安装 Emebala Chat 可恢复本地引擎，或者要切换到云（Google）翻译，请在托盘菜单的“选择翻译引擎”中选择“Google 翻译”。",
    .menu_engine_openai = L"OpenAI 兼容 (用户自定义服务器)…",
    .openai_settings_title = L"OpenAI 兼容引擎设置",
    .openai_settings_action = L"OpenAI 兼容引擎设置…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"API 密钥",
    .openai_model_label = L"模型",
    .openai_fetch_models = L"获取模型列表",
    .openai_fetch_failed = L"无法获取模型列表。您可以直接输入模型名称。",
    .openai_http_warning_title = L"不安全的连接 (HTTP)",
    .openai_http_warning_body = L"Base URL 使用 HTTP(未加密)。您的 API 密钥和文本将以明文发送。是否继续？",
    .openai_saved = L"OpenAI 兼容设置已保存。",
    .openai_key_masked = L"已保存的密钥: ",
    .openai_invalid_base_url = L"Base URL 无效。示例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"用户自定义模型 (.gguf)",
    .menu_browse_gguf_file = L"注册其他 .gguf 模型…",
    .user_gguf_quality_title = L"翻译质量提示",
    .user_gguf_quality_body = L"所选模型不是 Hy-MT2。当前版本使用 Hy-MT2 专用提示词,因此此模型的翻译质量不予保证。是否继续？",
    .user_gguf_registered_title = L"模型注册完成",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"所选模型已注册到本地引擎。\n"
    L"\n"
    L"在“翻译引擎选择 > 用户选择 (.gguf)”中选择此模型后,即可用于翻译。\n"
    L"\n"
    L"生效时间: 注册后最多约 1 分钟(引擎空闲退出后)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"该模型已内置在 Emebala Chat 中，无需注册。直接选择内置的本地翻译引擎即可使用。",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(未注册)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"管理模型…",
    .gguf_manager_title = L"用户模型管理",
    .gguf_manager_empty = L"没有已注册的用户模型。",
    .gguf_manager_rename = L"重命名…",
    .gguf_manager_delete = L"删除…",
    .gguf_manager_close = L"关闭",
    .gguf_manager_delete_confirm_title = L"删除模型注册",
    .gguf_manager_delete_confirm_body = L"所选模型的注册将被删除。\n"
    L"\n"
    L"模型文件(.gguf)会保留在磁盘上,不会被删除。如果正在使用该模型,翻译引擎选择将自动返回“自动(Auto)”。\n"
    L"\n"
    L"是否继续?",
    .gguf_manager_rename_title = L"重命名模型",
    .gguf_manager_rename_body = L"输入新名称(不超过64个字符,不含空格)。",
    .gguf_manager_rename_invalid = L"该名称无法使用。请输入一个非空、与现有名称不同、不含空格或路径分隔符且不超过64个字符的名称。",
    .gguf_manager_done = L"更改已保存。",
    .gguf_manager_err_serialize = L"无法序列化 registry.json(文件名被拒绝)。未做任何更改。",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% 不可用,找不到共享模型文件夹。",
    .gguf_manager_err_write = L"无法写入 registry.json。",
    .gguf_manager_err_write_partial = L"registry.json 未能完整写入。",
    .gguf_manager_err_registry_damaged = L"registry.json 已损坏或架构不受支持。未做修改。请修复或删除后重试。",
    .tooltip_translate_failed = L"翻译失败。请重试。",
    .repair_transient_body = L"本地翻译引擎暂时不可用(可能正在启动)。请稍后重试。",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"确定",
    .dialog_cancel = L"取消",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"从文件添加…",
    .gguf_manager_add_hf = L"从 Hugging Face 添加…",
    .hf_add_title = L"添加 Hugging Face 模型",
    .hf_url_label = L"模型 URL",
    .hf_downloading = L"正在下载…",
    .hf_failed = L"下载失败。",
    .hf_invalid_url = L"请输入 huggingface.co 模型地址（resolve、blob 链接或模型页面）",
    .hf_done = L"已注册。",
    .openai_base_url_hint = L"例如: https://api.openai.com/v1",
    .openai_api_key_hint = L"API 密钥",
    // REQ-051: [删除设置] 按钮。
    .openai_delete_settings = L"删除设置",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"检查更新",
    .update_idle_note = L"查看你是否在用最新版本的 Emebala Chat。",
    .update_checking = L"正在检查更新…",
    .update_checking_hint = L"在后台运行；你可以关闭此窗口。",
    .update_uptodate = L"你使用的是最新版本",
    .update_dlg_title = L"有可用更新",
    .update_dlg_version = L"新版本",
    .update_dlg_version_cur = L"你当前为 {v}",
    .update_dlg_size = L"大小",
    .update_dlg_size_approx = L"约 {s}",
    .update_dlg_privacy = L"检查和下载时会连接到 GitHub。",
    .update_dlg_download = L"下载",
    .update_dlg_later = L"稍后",
    .update_dl_progress = L"正在下载…",
    .update_dl_cancel = L"取消",
    .update_dl_cancelled = L"下载已取消。",
    .update_ready_title = L"下载完成，可安装",
    .update_ready_warn = L"安装开始时应用将关闭。",
    .update_ready_install = L"立即安装",
    .update_ready_later = L"稍后",
    .update_err_offline = L"你处于离线状态。请检查网络连接后重试。",
    .update_err_rate = L"请求过于频繁。请稍后再试。",
    .update_err_hash = L"下载的文件未通过验证。",
    .update_err_retry = L"重试",
    .update_snoozed_line = L"你可以稍后安装此更新。",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"下载模型",
    .model_dl_consent_body = L"内置翻译模型尚未安装。立即下载? (约 {s})",
    .model_dl_privacy = L"下载时会连接到 huggingface.co。",
    .model_dl_download = L"立即下载",
    .model_dl_verifying = L"正在校验下载的文件…",
    .model_dl_placing = L"正在安装模型…",
    .model_dl_done = L"模型已就绪。本地引擎将从下一次翻译开始使用。",
    .model_dl_failed = L"下载失败。请检查网络连接后重试。",
    .model_dl_disk_space = L"磁盘可用空间不足 (需要约 {s})。",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"翻译模型加载设置…",
    .engine_load_settings_title = L"翻译模型加载",
    .load_mode_on_use = L"使用时加载 (推荐)",
    .load_mode_warmup = L"预先加载",
    .load_mode_on_use_desc = L"节省内存。首次翻译可能会稍有延迟。",
    .load_mode_warmup_desc = L"翻译立即开始。模型将一直保留在内存中。",
    .gguf_policy_sticky = L"(保持常驻)",
    .gguf_policy_evict = L"(使用时加载)",
    .gguf_manager_toggle_policy = L"切换策略",
    .gguf_duplicate_warning = L"重复文件: {n} {s} × {c}",
    .gguf_manager_dedupe = L"清理重复",
    .gguf_manager_dedupe_done = L"重复文件已移至回收站。",
    .gguf_manager_dedupe_failed = L"无法清理重复文件。请检查是否有其他程序正在使用这些文件。",
    .menu_ui_theme = L"界面主题",
    .theme_fluent = L"流畅工艺 (默认)",
    .theme_editorial = L"温暖社论",
    .theme_monolith = L"精致碑铭",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"回译浮动显示（译文验证）",
    .back_translate_header = L"🔄 回译核对",
    .back_translate_match_exact = L"✓ 与原文一致",
    .back_translate_match_check = L"⚡ 检查表达",
    .back_translate_reference = L"↳ 原文: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"不是有效的 GGUF 模型文件，未注册。",
    .gguf_manager_active_suffix = L"(使用中)",
    .gguf_manager_activated = L"已设为当前使用的模型。",
};
// 4. Chinese Traditional (zh-TW)
const LocalizedStrings kStringsChineseTrad = {
    .menu_status_active = L"狀態: 運行中 (F9: 暫停)",
    .menu_status_paused = L"狀態: 已暫停 (F9: 啟用)",
    .menu_engine = L"選擇翻譯引擎",
    .menu_engine_google = L"Google 翻譯 (免費 / 免安裝 / 線上)",
    .menu_engine_local = L"內建本機引擎 (Hy-MT2-1.8B 離線)",
    .menu_source_lang = L"來源語言 (輸入語言)",
    .menu_target_lang = L"目標語言 (翻譯目標)",
    .menu_swap_langs = L"來源語言 ⇄ 目標語言 對調 (雙擊)",
    .menu_auto_send = L"Enter 自動發送 (Auto-Send)",
    .menu_sound_feedback = L"音效回饋 (Sound)",
    .menu_show_badge = L"顯示桌面懸浮徽章",
    .menu_start_with_windows = L"開機時自動啟動",
    .menu_cheatsheet = L"快捷鍵與使用說明 (說明)...",
    .menu_exit = L"結束 埃梅巴拉 翻譯",
    .menu_about = L"關於 埃梅巴拉 翻譯…",
    .cheatsheet_title = L"埃梅巴拉 翻譯 快捷鍵與使用說明",
    .cheatsheet_body = L"埃梅巴拉 翻譯 快捷鍵與使用說明:\n\n"
    L"  • F9 : 啟用 / 暫停 切換\n"
    L"  • Ctrl + F9 : 循環切換目標語言\n"
    L"  • Ctrl + Shift + Enter : 切換自動發送模式\n"
    L"  • Shift + Enter : 翻譯並立即發送 (Enter)\n\n"
    L"懸浮徽章滑鼠操作:\n"
    L"  • 左鍵點擊 : 啟用 / 暫停 切換\n"
    L"  • 雙擊徽章 : 來源語言 ⇄ 目標語言 快速互換\n"
    L"  • 右鍵點擊 : 直接打開設定選單\n\n"
    L"工作模式:\n"
    L"  • 僅替換模式 (自動發送關閉): 替換為譯文供確認後發送。\n"
    L"  • 自動發送模式 (自動發送開啟): 替換為譯文後自動發送。",
    .about_title = L"關於 埃梅巴拉 翻譯",
    .badge_active = L"運行中",
    .badge_translating = L"翻譯中...",
    .badge_paused = L"已暫停",
    .tooltip_title = L"埃梅巴拉 翻譯",
    .tooltip_copy_failed = L"無法複製所選文字。請檢查目標應用程式後重試。",
    .tooltip_no_selection = L"尚未選取要翻譯的文字。",
    .auto_detect = L"自動檢測",
    .app_already_running = L"埃梅巴拉 翻譯 已在背景執行。\n請檢視系統通知列。",
    .app_com_failed = L"COM 初始化失敗。\n懸浮徽章與語音朗讀將不可用，\n但翻譯、快捷鍵、系統匣與提示音仍可正常使用。",
    .about_tagline = L"告別複製貼上。用母語自然輸入，譯文會在任何 Windows 應用程式中即時取代你的鍵入。",
    .about_feature0 = L"⚡ 拖曳翻譯 — 在任何應用程式中選取文字，懸浮圖示立即翻譯。",
    .about_feature1 = L"🔊 神經 TTS — 接入 Windows 語音包後支援全部 37 種語言發音。",
    .about_feature2 = L"🔒 100% 本機執行且私密 — 僅在按住快捷鍵時生效，不觸碰剪貼簿。",
    .about_etymology = L"西元前 2000 年，美索不達米亞的書吏稱那些以語言連結世界的人為 Eme-bala。",
    .about_link_website = L"官網",
    .about_link_contact = L"聯絡",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · 辦公時間 10:00–19:00 KST",
    .about_contact_lead = L"首席架構師：Yongtai Kim",
    .tooltip_copied = L"✓ 已複製！",
    .tooltip_button_copy = L"📋 複製",
    .tooltip_button_tts = L"🔊 朗讀",
    .menu_ui_language = L"介面語言",
    .menu_ui_language_auto = L"自動（系統語言）",
    .about_reset_button = L"重設為系統預設值",
    .about_reset_done = L"已還原預設值",
    .menu_typing_group = L"鍵盤輸入",
    .menu_tooltip_group = L"划譯工具提示",
    .app_name = L"埃梅巴拉 翻譯",
    .tooltip_no_tts_voice = L"此語言未安裝 Windows 語音。再次點擊 🔊 可開啟語音設定。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"隱私保護說明",
    .privacy_notice_body = L"請閱讀 Emebala Chat 的隱私處理原則。\n"
    L"\n"
    L"• Emebala Chat 不營運任何自有伺服器。\n"
    L"• 使用本機模型時，翻譯內容不會離開您的裝置。\n"
    L"• 選擇 Google 翻譯引擎或自動切換至雲端時，所選或輸入的文字將直接傳送至 Google 進行翻譯，不經過 Emebala。\n"
    L"• 診斷記錄預設關閉（OFF），需在設定中手動開啟（選擇性加入）。\n"
    L"• 若您不希望將文字傳送至雲端 (Google)，請在系統匣圖示選單的「選擇翻譯引擎」中選取「內建本機引擎」。若未安裝本地模型且已關閉雲端退回，翻譯將不會傳送任何資料，也不會執行。\n"
    L"\n"
    L"完整說明請查閱 README 檔案，您可隨時重新閱讀。\n",
    .cheatsheet_config_path = L"設定檔: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"文字過長，僅翻譯了開頭和結尾部分。",
    .tooltip_untranslated_above = L"上方有未翻譯的新文字。將游標置於該行末尾並按 Enter 即可翻譯。",
    .repair_in_progress = L"正在修復本機引擎元件…",
    .repair_failed_title = L"本機翻譯無法使用",
    .repair_failed_body = L"找不到本機翻譯引擎檔案，翻譯已暫時停止。重新安裝 Emebala Chat 可還原本機引擎，或者若要切換到雲端（Google）翻譯，請在系統匣選單的「選擇翻譯引擎」中選取「Google 翻譯」。",
    .menu_engine_openai = L"OpenAI 相容 (使用者自訂伺服器)…",
    .openai_settings_title = L"OpenAI 相容引擎設定",
    .openai_settings_action = L"OpenAI 相容引擎設定…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"API 金鑰",
    .openai_model_label = L"模型",
    .openai_fetch_models = L"取得模型列表",
    .openai_fetch_failed = L"無法取得模型列表。您可以直接輸入模型名稱。",
    .openai_http_warning_title = L"不安全的連線 (HTTP)",
    .openai_http_warning_body = L"Base URL 使用 HTTP(未加密)。您的 API 金鑰和文字將以明文傳送。是否繼續？",
    .openai_saved = L"OpenAI 相容設定已儲存。",
    .openai_key_masked = L"已儲存的金鑰: ",
    .openai_invalid_base_url = L"Base URL 無效。範例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"使用者自訂模型 (.gguf)",
    .menu_browse_gguf_file = L"註冊其他 .gguf 模型…",
    .user_gguf_quality_title = L"翻譯品質提示",
    .user_gguf_quality_body = L"所選模型不是 Hy-MT2。目前版本使用 Hy-MT2 專用提示詞,因此此模型的翻譯品質不予保證。是否繼續？",
    .user_gguf_registered_title = L"模型註冊完成",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"所選模型已註冊到本機引擎。\n"
    L"\n"
    L"在「翻譯引擎選擇 > 使用者選擇 (.gguf)」中選擇此模型後,即可用於翻譯。\n"
    L"\n"
    L"生效時間: 註冊後最多約 1 分鐘(引擎閒置結束後)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"此模型已內建於 Emebala Chat，無需註冊。直接選擇內建的本機翻譯引擎即可使用。",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(未註冊)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"管理模型…",
    .gguf_manager_title = L"使用者模型管理",
    .gguf_manager_empty = L"沒有已註冊的使用者模型。",
    .gguf_manager_rename = L"重新命名…",
    .gguf_manager_delete = L"刪除…",
    .gguf_manager_close = L"關閉",
    .gguf_manager_delete_confirm_title = L"刪除模型註冊",
    .gguf_manager_delete_confirm_body = L"所選模型的註冊將被刪除。\n"
    L"\n"
    L"模型檔案(.gguf)會保留在磁碟上,不會被刪除。如果正在使用該模型,翻譯引擎選擇將自動返回「自動(Auto)」。\n"
    L"\n"
    L"是否繼續?",
    .gguf_manager_rename_title = L"重新命名模型",
    .gguf_manager_rename_body = L"輸入新名稱(不超過64個字元,不含空格)。",
    .gguf_manager_rename_invalid = L"該名稱無法使用。請輸入一個非空、與現有名稱不同、不含空格或路徑分隔字元且不超過64個字元的名稱。",
    .gguf_manager_done = L"變更已儲存。",
    .gguf_manager_err_serialize = L"無法序列化 registry.json(檔名被拒絕)。未做任何變更。",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% 不可用,找不到共用模型資料夾。",
    .gguf_manager_err_write = L"無法寫入 registry.json。",
    .gguf_manager_err_write_partial = L"registry.json 未能完整寫入。",
    .gguf_manager_err_registry_damaged = L"registry.json 已損毀或結構不受支援。未做修改。請修復或刪除後重試。",
    .tooltip_translate_failed = L"翻譯失敗。請重試。",
    .repair_transient_body = L"本機翻譯引擎暫時不可用(可能正在啟動)。請稍後重試。",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"確定",
    .dialog_cancel = L"取消",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"從檔案新增…",
    .gguf_manager_add_hf = L"從 Hugging Face 新增…",
    .hf_add_title = L"新增 Hugging Face 模型",
    .hf_url_label = L"模型 URL",
    .hf_downloading = L"正在下載…",
    .hf_failed = L"下載失敗。",
    .hf_invalid_url = L"請輸入 huggingface.co 模型網址（resolve、blob 連結或模型頁面）",
    .hf_done = L"已註冊。",
    .openai_base_url_hint = L"例如: https://api.openai.com/v1",
    .openai_api_key_hint = L"API 金鑰",
    // REQ-051: [刪除設定] 按鈕。
    .openai_delete_settings = L"刪除設定",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"檢查更新",
    .update_idle_note = L"查看你是否在使用最新版本的 Emebala Chat。",
    .update_checking = L"正在檢查更新…",
    .update_checking_hint = L"在背景執行；你可以關閉此視窗。",
    .update_uptodate = L"你使用的是最新版本",
    .update_dlg_title = L"有可用更新",
    .update_dlg_version = L"新版本",
    .update_dlg_version_cur = L"你目前為 {v}",
    .update_dlg_size = L"大小",
    .update_dlg_size_approx = L"約 {s}",
    .update_dlg_privacy = L"檢查和下載時會連線到 GitHub。",
    .update_dlg_download = L"下載",
    .update_dlg_later = L"稍後",
    .update_dl_progress = L"正在下載…",
    .update_dl_cancel = L"取消",
    .update_dl_cancelled = L"下載已取消。",
    .update_ready_title = L"下載完成，可安裝",
    .update_ready_warn = L"安裝開始時應用程式將關閉。",
    .update_ready_install = L"立即安裝",
    .update_ready_later = L"稍後",
    .update_err_offline = L"你目前離線。請檢查網路連線後再試一次。",
    .update_err_rate = L"要求過於頻繁。請稍後再試。",
    .update_err_hash = L"下載的檔案未通過驗證。",
    .update_err_retry = L"重試",
    .update_snoozed_line = L"你可以稍後安裝此更新。",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"下載模型",
    .model_dl_consent_body = L"內建翻譯模型尚未安裝。立即下載? (約 {s})",
    .model_dl_privacy = L"下載時會連線到 huggingface.co。",
    .model_dl_download = L"立即下載",
    .model_dl_verifying = L"正在驗證下載的檔案…",
    .model_dl_placing = L"正在安裝模型…",
    .model_dl_done = L"模型已就緒。本機引擎將從下一場翻譯開始使用。",
    .model_dl_failed = L"下載失敗。請檢查網路連線後再試。",
    .model_dl_disk_space = L"磁碟可用空間不足 (需要約 {s})。",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"翻譯模型載入設定…",
    .engine_load_settings_title = L"翻譯模型載入",
    .load_mode_on_use = L"使用時載入 (建議)",
    .load_mode_warmup = L"預先載入",
    .load_mode_on_use_desc = L"節省記憶體。首次翻譯可能會稍有延遲。",
    .load_mode_warmup_desc = L"翻譯立即開始。模型將一直保留在記憶體中。",
    .gguf_policy_sticky = L"(保持常駐)",
    .gguf_policy_evict = L"(使用時載入)",
    .gguf_manager_toggle_policy = L"切換原則",
    .gguf_duplicate_warning = L"重複檔案: {n} {s} × {c}",
    .gguf_manager_dedupe = L"清理重複",
    .gguf_manager_dedupe_done = L"重複檔案已移至資源回收筒。",
    .gguf_manager_dedupe_failed = L"無法清理重複檔案。請檢查是否有其他程式正在使用這些檔案。",
    .menu_ui_theme = L"介面主題",
    .theme_fluent = L"流暢工藝 (預設)",
    .theme_editorial = L"溫暖社論",
    .theme_monolith = L"精緻碑銘",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"回譯浮動顯示（譯文驗證）",
    .back_translate_header = L"🔄 回譯核對",
    .back_translate_match_exact = L"✓ 與原文一致",
    .back_translate_match_check = L"⚡ 檢查表達",
    .back_translate_reference = L"↳ 原文: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"不是有效的 GGUF 模型檔案，未註冊。",
    .gguf_manager_active_suffix = L"(使用中)",
    .gguf_manager_activated = L"已設為目前使用的模型。",
};
// 5. Vietnamese (vi)
const LocalizedStrings kStringsVietnamese = {
    .menu_status_active = L"Trạng thái: Đang bật (F9: Tạm dừng)",
    .menu_status_paused = L"Trạng thái: Tạm dừng (F9: Bật lại)",
    .menu_engine = L"Chọn công cụ dịch",
    .menu_engine_google = L"Google Dịch (Miễn phí / Trực tuyến)",
    .menu_engine_local = L"Công cụ cục bộ tích hợp (Hy-MT2-1.8B ngoại tuyến)",
    .menu_source_lang = L"Ngôn ngữ nguồn (Nhập)",
    .menu_target_lang = L"Ngôn ngữ đích (Dịch sang)",
    .menu_swap_langs = L"Hoán đổi nguồn ⇄ đích (Nhấp đúp)",
    .menu_auto_send = L"Tự động gửi khi nhấn Enter (Auto-Send)",
    .menu_sound_feedback = L"Âm thanh thông báo (Sound)",
    .menu_show_badge = L"Hiển thị huy hiệu nổi",
    .menu_start_with_windows = L"Khởi động cùng Windows",
    .menu_cheatsheet = L"Hướng dẫn phím tắt (Trợ giúp)...",
    .menu_exit = L"Thoát Emebala Chat",
    .menu_about = L"Giới thiệu Emebala Chat…",
    .cheatsheet_title = L"Hướng dẫn sử dụng Emebala Chat",
    .cheatsheet_body = L"Phím tắt & Hướng dẫn sử dụng Emebala Chat:\n\n"
    L"  • F9 : Bật / Tạm dừng dịch\n"
    L"  • Ctrl + F9 : Đổi ngôn ngữ đích kế tiếp\n"
    L"  • Ctrl + Shift + Enter : Bật/Tắt chế độ tự động gửi\n"
    L"  • Shift + Enter : Dịch và gửi ngay lập tức\n\n"
    L"Thao tác chuột trên huy hiệu:\n"
    L"  • Nhấp chuột trái : Bật / Tạm dừng\n"
    L"  • Nhấp đúp : Đảo ngược ngôn ngữ nguồn ⇄ đích\n"
    L"  • Nhấp chuột phải : Mở ngay menu cài đặt\n\n"
    L"Chế độ:\n"
    L"  • Chỉ thay thế (Tắt tự động gửi): Thay văn bản dịch để bạn kiểm tra trước khi gửi.\n"
    L"  • Tự động gửi (Bật tự động gửi): Tự động nhấn Enter gửi tin nhắn sau khi dịch.",
    .about_title = L"Giới thiệu Emebala Chat",
    .badge_active = L"Đang bật",
    .badge_translating = L"Đang dịch...",
    .badge_paused = L"Tạm dừng",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Không thể sao chép văn bản đã chọn. Hãy kiểm tra ứng dụng đích rồi thử lại.",
    .tooltip_no_selection = L"Chưa chọn văn bản nào để dịch.",
    .auto_detect = L"Tự động phát hiện",
    .app_already_running = L"Emebala Chat đang chạy ngầm.\nHãy kiểm tra khay thông báo hệ thống.",
    .app_com_failed = L"Khởi tạo COM thất bại.\nHuy hiệu nổi và đọc văn bản sẽ không khả dụng,\nnhưng dịch, phím tắt, khay hệ thống và âm thanh vẫn hoạt động.",
    .about_tagline = L"Không còn copy-paste. Gõ tự nhiên bằng tiếng mẹ đẻ — bản dịch thay thế ngay câu bạn gõ trong mọi ứng dụng Windows.",
    .about_feature0 = L"⚡ Kéo để dịch — chọn văn bản trong bất kỳ ứng dụng nào, biểu tượng nổi dịch ngay lập tức.",
    .about_feature1 = L"🔊 TTS thần kinh — phát âm tất cả 37 ngôn ngữ khi có gói giọng nói Windows.",
    .about_feature2 = L"🔒 100% trên máy & riêng tư — chỉ hoạt động khi giữ phím tắt; không đụng tới clipboard.",
    .about_etymology = L"Năm 2000 TCN, các thư lại Lưỡng Hà gọi 'Eme-bala' — những người dùng ngôn ngữ bắc nhịp nối các thế giới.",
    .about_link_website = L"Website",
    .about_link_contact = L"Liên hệ",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Giờ làm việc 10:00–19:00 KST",
    .about_contact_lead = L"Kiến trúc sư chính: Yongtai Kim",
    .tooltip_copied = L"✓ Đã sao chép!",
    .tooltip_button_copy = L"📋 Sao chép",
    .tooltip_button_tts = L"🔊 Đọc",
    .menu_ui_language = L"Ngôn ngữ giao diện",
    .menu_ui_language_auto = L"Tự động (ngôn ngữ hệ thống)",
    .about_reset_button = L"Đặt lại về mặc định hệ thống",
    .about_reset_done = L"Đã khôi phục mặc định",
    .menu_typing_group = L"Gõ phím",
    .menu_tooltip_group = L"Dịch khi chọn văn bản",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Chưa cài đặt giọng nói Windows cho ngôn ngữ này. Nhấp lại 🔊 để mở Cài đặt Nhận dạng và giọng nói.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Thông báo về quyền riêng tư",
    .privacy_notice_body = L"Đây là nguyên tắc xử lý thông tin cá nhân của Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat không vận hành bất kỳ máy chủ riêng nào.\n"
    L"• Khi dùng mô hình cục bộ, nội dung dịch không rời khỏi máy của bạn.\n"
    L"• Khi chọn Google Dịch hoặc được tự động chuyển sang đám mây, văn bản bạn chọn hoặc nhập được gửi trực tiếp tới Google để dịch, không qua Emebala.\n"
    L"• Nhật ký chẩn đoán mặc định TẮT; bạn phải bật trong cài đặt (chọn tham gia).\n"
    L"• Nếu không muốn gửi văn bản lên đám mây (Google), hãy mở menu biểu tượng ở khay hệ thống, chọn “Chọn công cụ dịch” rồi chọn “Công cụ cục bộ tích hợp”. Khi chưa cài mô hình cục bộ và tùy chọn chuyển lên đám mây đang tắt, bản dịch sẽ không chạy — không có gì được gửi đi.\n"
    L"\n"
    L"Xem toàn bộ nội dung trong tệp README. Bạn có thể đọc lại bất cứ lúc nào.\n",
    .cheatsheet_config_path = L"Tệp cấu hình: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Văn bản quá dài nên chỉ phần đầu và phần cuối được dịch.",
    .tooltip_untranslated_above = L"Có văn bản mới chưa dịch ở trên. Đặt con trỏ ở cuối dòng đó và nhấn Enter để dịch.",
    .repair_in_progress = L"Đang sửa chữa các thành phần cục bộ…",
    .repair_failed_title = L"Không thể dùng bản dịch cục bộ",
    .repair_failed_body = L"Không tìm thấy tệp của cục bộ nên bản dịch tạm dừng. Cài đặt lại Emebala Chat để khôi phục cục bộ, hoặc để chuyển sang bản dịch đám mây (Google), hãy chọn “Google Dịch” trong menu khay “Chọn công cụ dịch”.",
    .menu_engine_openai = L"Tương thích OpenAI (máy chủ do ngườ dùng chỉ định)…",
    .openai_settings_title = L"Cài đặt công cụ tương thích OpenAI",
    .openai_settings_action = L"Cài đặt công cụ tương thích OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"API Key",
    .openai_model_label = L"Mô hình",
    .openai_fetch_models = L"Lấy danh sách mô hình",
    .openai_fetch_failed = L"Không thể lấy danh sách mô hình. Bạn có thể nhập tên mô hình trực tiếp.",
    .openai_http_warning_title = L"Kết nối không an toàn (HTTP)",
    .openai_http_warning_body = L"Base URL sử dụng HTTP (không mã hóa). API key và văn bản của bạn sẽ được gửi dạng plaintext. Tiếp tục?",
    .openai_saved = L"Đã lưu cài đặt tương thích OpenAI.",
    .openai_key_masked = L"Khóa đã lưu: ",
    .openai_invalid_base_url = L"Base URL không hợp lệ. Ví dụ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Mô hình do ngườ dùng chỉ định (.gguf)",
    .menu_browse_gguf_file = L"Đăng ký mô hình .gguf khác…",
    .user_gguf_quality_title = L"Thông báo chất lượng dịch",
    .user_gguf_quality_body = L"Mô hình đã chọn không phải Hy-MT2. Phiên bản hiện tại chỉ dùng prompt dành riêng cho Hy-MT2, vì vậy chất lượng dịch của mô hình này không được đảm bảo. Tiếp tục?",
    .user_gguf_registered_title = L"Đăng ký mô hình hoàn tất",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Mô hình đã chọn đã được đăng ký với công cụ cục bộ.\n"
    L"\n"
    L"Mô hình này được dùng để dịch khi bạn chọn \"Chọn công cụ dịch > Người dùng tự chọn (.gguf)\".\n"
    L"\n"
    L"Thời điểm áp dụng: tối đa khoảng 1 phút sau khi đăng ký (sau khi công cụ kết thúc nhàn rỗi)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Mô hình này đã được tích hợp sẵn trong Emebala Chat. Bạn không cần đăng ký. Hãy chọn trực tiếp công cụ dịch nội bộ để sử dụng.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(chưa đăng ký)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Quản lý mô hình…",
    .gguf_manager_title = L"Quản lý mô hình người dùng",
    .gguf_manager_empty = L"Chưa có mô hình người dùng nào được đăng ký.",
    .gguf_manager_rename = L"Đổi tên…",
    .gguf_manager_delete = L"Xóa…",
    .gguf_manager_close = L"Đóng",
    .gguf_manager_delete_confirm_title = L"Xóa đăng ký mô hình",
    .gguf_manager_delete_confirm_body = L"Đăng ký của mô hình đã chọn sẽ bị xóa.\n"
    L"\n"
    L"Tệp mô hình (.gguf) vẫn được giữ trên ổ đĩa và không bị xóa. Nếu đang sử dụng mô hình này, lựa chọn công cụ dịch sẽ quay về Tự động (Auto).\n"
    L"\n"
    L"Tiếp tục?",
    .gguf_manager_rename_title = L"Đổi tên mô hình",
    .gguf_manager_rename_body = L"Nhập tên mới (tối đa 64 ký tự, không dấu cách).",
    .gguf_manager_rename_invalid = L"Không thể sử dụng tên đó. Vui lòng nhập tên không trống, khác với tên hiện có, không chứa dấu cách hoặc dấu phân cách đường dẫn, trong 64 ký tự.",
    .gguf_manager_done = L"Đã lưu thay đổi.",
    .gguf_manager_err_serialize = L"Không thể tuần tự hóa registry.json (tên tệp bị từ chối). Không có gì thay đổi.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% không khả dụng; không thể định vị thư mục mô hình dùng chung.",
    .gguf_manager_err_write = L"Không thể ghi registry.json.",
    .gguf_manager_err_write_partial = L"Không thể ghi registry.json hoàn chỉnh.",
    .gguf_manager_err_registry_damaged = L"registry.json bị hỏng hoặc có lược đồ không được hỗ trợ. Nó KHÔNG bị sửa đổi. Hãy khắc phục hoặc xóa rồi thử lại.",
    .tooltip_translate_failed = L"Dịch thất bại. Vui lòng thử lại.",
    .repair_transient_body = L"Không thể sử dụng tạm thời công cụ dịch cục bộ (có thể đang khởi động). Vui lòng đợi một chút và thử lại.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Đồng ý",
    .dialog_cancel = L"Hủy",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Thêm từ tệp…",
    .gguf_manager_add_hf = L"Thêm từ Hugging Face…",
    .hf_add_title = L"Thêm mô hình Hugging Face",
    .hf_url_label = L"URL mô hình",
    .hf_downloading = L"Đang tải xuống…",
    .hf_failed = L"Tải xuống thất bại.",
    .hf_invalid_url = L"Nhập URL mô hình huggingface.co (resolve, blob hoặc trang mô hình)",
    .hf_done = L"Đã đăng ký.",
    .openai_base_url_hint = L"VD: https://api.openai.com/v1",
    .openai_api_key_hint = L"Khóa API",
    // REQ-051: nút [Xóa cài đặt].
    .openai_delete_settings = L"Xóa cài đặt",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Kiểm tra cập nhật",
    .update_idle_note = L"Xem bạn có đang dùng phiên bản mới nhất của Emebala Chat không.",
    .update_checking = L"Đang kiểm tra cập nhật…",
    .update_checking_hint = L"Chạy trong nền; bạn có thể đóng cửa sổ này.",
    .update_uptodate = L"Bạn đang dùng phiên bản mới nhất",
    .update_dlg_title = L"Có bản cập nhật mới",
    .update_dlg_version = L"Phiên bản mới",
    .update_dlg_version_cur = L"bạn đang dùng {v}",
    .update_dlg_size = L"Kích thước",
    .update_dlg_size_approx = L"khoảng {s}",
    .update_dlg_privacy = L"Việc kiểm tra và tải xuống sẽ kết nối tới GitHub.",
    .update_dlg_download = L"Tải xuống",
    .update_dlg_later = L"Để sau",
    .update_dl_progress = L"Đang tải xuống…",
    .update_dl_cancel = L"Hủy",
    .update_dl_cancelled = L"Đã hủy tải xuống.",
    .update_ready_title = L"Tải xuống hoàn tất — sẵn sàng cài đặt",
    .update_ready_warn = L"Ứng dụng sẽ đóng để cài đặt.",
    .update_ready_install = L"Cài đặt ngay",
    .update_ready_later = L"Để sau",
    .update_err_offline = L"Bạn đang ngoại tuyến. Hãy kiểm tra kết nối rồi thử lại.",
    .update_err_rate = L"Quá nhiều yêu cầu. Vui lòng thử lại sau ít phút.",
    .update_err_hash = L"Tệp đã tải không vượt qua xác minh.",
    .update_err_retry = L"Thử lại",
    .update_snoozed_line = L"Bạn có thể cài đặt bản cập nhật sau.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Tải mô hình",
    .model_dl_consent_body = L"Mô hình dịch tích hợp chưa được cài đặt. Tải ngay bây giờ? (khoảng {s})",
    .model_dl_privacy = L"Việc tải xuống sẽ kết nối tới huggingface.co.",
    .model_dl_download = L"Tải ngay",
    .model_dl_verifying = L"Đang xác minh tệp đã tải…",
    .model_dl_placing = L"Đang cài đặt mô hình…",
    .model_dl_done = L"Mô hình đã sẵn sàng. Cỗ máy cục bộ sẽ sử dụng từ lần dịch tiếp theo.",
    .model_dl_failed = L"Tải xuống thất bại. Hãy kiểm tra kết nối và thử lại.",
    .model_dl_disk_space = L"Không đủ dung lượng đĩa trống (cần khoảng {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Cài đặt tải mô hình dịch…",
    .engine_load_settings_title = L"Tải mô hình dịch",
    .load_mode_on_use = L"Tải khi sử dụng (khuyến nghị)",
    .load_mode_warmup = L"Tải trước",
    .load_mode_on_use_desc = L"Tiết kiệm bộ nhớ. Bản dịch đầu tiên có thể chậm vài giây.",
    .load_mode_warmup_desc = L"Bắt đầu dịch ngay. Mô hình luôn được giữ trong bộ nhớ.",
    .gguf_policy_sticky = L"(Giữ thường trú)",
    .gguf_policy_evict = L"(Tải khi sử dụng)",
    .gguf_manager_toggle_policy = L"Chuyển chính sách",
    .gguf_duplicate_warning = L"Tệp trùng lặp: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Dọn tệp trùng lặp",
    .gguf_manager_dedupe_done = L"Các tệp trùng lặp đã được chuyển vào Thùng rác.",
    .gguf_manager_dedupe_failed = L"Không thể dọn các tệp trùng lặp. Hãy kiểm tra xem chương trình khác có đang sử dụng tệp không.",
    .menu_ui_theme = L"Chủ đề giao diện",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Warm Editorial",
    .theme_monolith = L"Refined Monolith",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Hiển thị dịch ngược nổi (kiểm chứng bản dịch)",
    .back_translate_header = L"🔄 Kiểm tra dịch ngược",
    .back_translate_match_exact = L"✓ Khớp nghĩa gốc",
    .back_translate_match_check = L"⚡ Kiểm tra sắc thái",
    .back_translate_reference = L"↳ Bản gốc: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Không phải tệp mô hình GGUF hợp lệ nên chưa được đăng ký.",
    .gguf_manager_active_suffix = L"(đang dùng)",
    .gguf_manager_activated = L"Đã đặt làm mô hình đang dùng.",
};
// 6. Spanish (es)
const LocalizedStrings kStringsSpanish = {
    .menu_status_active = L"Estado: Activo (F9: Pausar)",
    .menu_status_paused = L"Estado: Pausado (F9: Activar)",
    .menu_engine = L"Motor de traducción",
    .menu_engine_google = L"Google Translate (Gratuito / En línea)",
    .menu_engine_local = L"Motor local integrado (Hy-MT2-1.8B sin conexión)",
    .menu_source_lang = L"Idioma de origen (Entrada)",
    .menu_target_lang = L"Idioma de destino (Traducción)",
    .menu_swap_langs = L"Intercambiar idiomas (Doble clic)",
    .menu_auto_send = L"Enviar automáticamente con Enter",
    .menu_sound_feedback = L"Sonidos de notificación",
    .menu_show_badge = L"Mostrar insignia flotante",
    .menu_start_with_windows = L"Iniciar con Windows",
    .menu_cheatsheet = L"Guía de atajos de teclado...",
    .menu_exit = L"Salir de Emebala Chat",
    .menu_about = L"Acerca de Emebala Chat…",
    .cheatsheet_title = L"Guía de atajos de Emebala Chat",
    .cheatsheet_body = L"Guía de uso y atajos de Emebala Chat:\n\n"
    L"  • F9 : Activar / Pausar\n"
    L"  • Ctrl + F9 : Cambiar idioma de destino\n"
    L"  • Ctrl + Shift + Enter : Alternar envío automático\n"
    L"  • Shift + Enter : Traducir y enviar de inmediato\n\n"
    L"Acciones de ratón en la insignia:\n"
    L"  • Clic izquierdo : Activar / Pausar\n"
    L"  • Doble clic : Intercambiar origen ⇄ destino\n"
    L"  • Clic derecho : Abrir menú de opciones",
    .about_title = L"Acerca de Emebala Chat",
    .badge_active = L"Activo",
    .badge_translating = L"Traduciendo...",
    .badge_paused = L"Pausado",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"No se pudo copiar el texto seleccionado. Revisa la aplicación de destino e inténtalo de nuevo.",
    .tooltip_no_selection = L"No hay texto seleccionado para traducir.",
    .auto_detect = L"Detectar automáticamente",
    .app_already_running = L"Emebala Chat ya se está ejecutando en segundo plano.\nRevisa la bandeja de notificaciones.",
    .app_com_failed = L"Error al iniciar COM.\nLa insignia flotante y la voz no estarán disponibles,\npero la traducción, los atajos, la bandeja y los sonidos siguen funcionando.",
    .about_tagline = L"Nunca más copiar y pegar. Escribe con naturalidad en tu idioma: la traducción reemplaza tu texto en tiempo real en cualquier aplicación de Windows.",
    .about_feature0 = L"⚡ Arrastrar y traducir — selecciona texto en cualquier app y el icono flotante lo traduce al instante.",
    .about_feature1 = L"🔊 TTS neuronal — pronuncia los 37 idiomas con los paquetes de voz de Windows instalados.",
    .about_feature2 = L"🔒 100% local y privado — solo activo mientras mantienes el atajo; sin tocar el portapapeles.",
    .about_etymology = L"En el 2000 a. C., los escribas mesopotámicos llamaban «Eme-bala» a quienes convierten el lenguaje en un puente entre mundos.",
    .about_link_website = L"Sitio web",
    .about_link_contact = L"Contacto",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Horario 10:00–19:00 KST",
    .about_contact_lead = L"Arquitecto principal: Yongtai Kim",
    .tooltip_copied = L"✓ ¡Copiado!",
    .tooltip_button_copy = L"📋 Copiar",
    .tooltip_button_tts = L"🔊 Voz",
    .menu_ui_language = L"Idioma de la interfaz",
    .menu_ui_language_auto = L"Automático (idioma del sistema)",
    .about_reset_button = L"Restablecer valores predeterminados",
    .about_reset_done = L"Valores restaurados",
    .menu_typing_group = L"Escritura con teclado",
    .menu_tooltip_group = L"Tooltip de selección",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"No hay una voz de Windows instalada para este idioma. Haz clic de nuevo en 🔊 para abrir la configuración de Voz.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Aviso de privacidad",
    .privacy_notice_body = L"Estos son los principios de privacidad de Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat no opera ningún servidor propio.\n"
    L"• Con el modelo local, el texto traducido nunca sale de tu dispositivo.\n"
    L"• Si eliges Google Translate o se activa la nube automáticamente, el texto seleccionado o escrito se envía directamente a Google para su traducción, sin pasar por Emebala.\n"
    L"• Los registros de diagnóstico están DESACTIVADOS por defecto; debes activarlos en la configuración (opt-in).\n"
    L"• Si no quieres enviar texto a la nube (Google), abre el menú del icono de la bandeja, elige “Motor de traducción” y selecciona “Motor local integrado”. Sin modelo local instalado y con el modo cloud desactivado, la traducción no se ejecuta y no se envía nada.\n"
    L"\n"
    L"Puedes leer el detalle completo en el archivo README cuando quieras.\n",
    .cheatsheet_config_path = L"Archivo de configuración: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"El texto era demasiado largo: solo se tradujeron el principio y el final.",
    .tooltip_untranslated_above = L"Hay texto nuevo sin traducir arriba. Coloca el cursor al final de esa línea y pulsa Enter para traducirlo.",
    .repair_in_progress = L"Reparando los componentes del motor local…",
    .repair_failed_title = L"Traducción local no disponible",
    .repair_failed_body = L"No se encuentran los archivos del motor de traducción local, por lo que la traducción se detuvo temporalmente. Reinstala Emebala Chat para restaurar el motor local, o para cambiar a la traducción en la nube (Google), elige “Google Translate” en el menú de la bandeja, “Motor de traducción”.",
    .menu_engine_openai = L"Compatible con OpenAI (servidor personalizado)…",
    .openai_settings_title = L"Ajustes del motor compatible con OpenAI",
    .openai_settings_action = L"Ajustes del motor compatible con OpenAI…",
    .openai_base_url_label = L"URL base",
    .openai_api_key_label = L"Clave de API",
    .openai_model_label = L"Modelo",
    .openai_fetch_models = L"Obtener lista de modelos",
    .openai_fetch_failed = L"No se pudo obtener la lista de modelos. Puede escribir el nombre del modelo directamente.",
    .openai_http_warning_title = L"Conexión no segura (HTTP)",
    .openai_http_warning_body = L"La URL base usa HTTP (sin cifrar). Su clave de API y el texto se enviarán en texto plano. ¿Continuar?",
    .openai_saved = L"Ajustes compatibles con OpenAI guardados.",
    .openai_key_masked = L"Clave guardada: ",
    .openai_invalid_base_url = L"La URL base no es válida. Ejemplo: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Modelo personalizado (.gguf)",
    .menu_browse_gguf_file = L"Registrar otro modelo .gguf…",
    .user_gguf_quality_title = L"Aviso sobre la calidad de la traducción",
    .user_gguf_quality_body = L"El modelo seleccionado no es Hy-MT2. La versión actual usa el prompt exclusivo de Hy-MT2, por lo que la calidad de traducción con este modelo no está garantizada. ¿Continuar?",
    .user_gguf_registered_title = L"Modelo registrado",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"El modelo seleccionado se ha registrado en el motor local.\n"
    L"\n"
    L"Este modelo se usa para traducir cuando elige \"Selección de motor de traducción > Selección del usuario (.gguf)\".\n"
    L"\n"
    L"Cuándo surte efecto: hasta aproximadamente 1 minuto después del registro (tras la salida por inactividad del motor)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Este modelo ya está integrado en Emebala Chat. No es necesario registrarlo. Puedes seleccionar directamente el motor de traducción local integrado.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(sin registrar)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Administrar modelos…",
    .gguf_manager_title = L"Administrador de modelos de usuario",
    .gguf_manager_empty = L"No hay modelos de usuario registrados.",
    .gguf_manager_rename = L"Cambiar nombre…",
    .gguf_manager_delete = L"Eliminar…",
    .gguf_manager_close = L"Cerrar",
    .gguf_manager_delete_confirm_title = L"Eliminar registro del modelo",
    .gguf_manager_delete_confirm_body = L"Se eliminará el registro del modelo seleccionado.\n"
    L"\n"
    L"El archivo del modelo (.gguf) se conserva en el disco y no se elimina. Si este modelo estaba en uso, la selección del motor de traducción volverá a Automático (Auto).\n"
    L"\n"
    L"¿Continuar?",
    .gguf_manager_rename_title = L"Cambiar nombre del modelo",
    .gguf_manager_rename_body = L"Introduzca un nombre nuevo (máx. 64 caracteres, sin espacios).",
    .gguf_manager_rename_invalid = L"Ese nombre no se puede usar. Introduzca un nombre no vacío, distinto de los existentes, sin espacios ni separadores de ruta, de hasta 64 caracteres.",
    .gguf_manager_done = L"Cambios guardados.",
    .gguf_manager_err_serialize = L"No se pudo serializar registry.json (se rechazó un nombre de archivo). No se cambió nada.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% no está disponible; no se puede ubicar el directorio de modelos compartido.",
    .gguf_manager_err_write = L"No se pudo escribir registry.json.",
    .gguf_manager_err_write_partial = L"No se pudo escribir registry.json por completo.",
    .gguf_manager_err_registry_damaged = L"registry.json está dañado o tiene un esquema no compatible. NO se modificó. Repárelo o elimínelo y vuelva a intentarlo.",
    .tooltip_translate_failed = L"La traducción falló. Inténtelo de nuevo.",
    .repair_transient_body = L"El motor de traducción local no está disponible temporalmente (puede estar iniciándose). Espere un momento e inténtelo de nuevo.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Aceptar",
    .dialog_cancel = L"Cancelar",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Añadir desde archivo…",
    .gguf_manager_add_hf = L"Añadir desde Hugging Face…",
    .hf_add_title = L"Añadir modelo de Hugging Face",
    .hf_url_label = L"URL del modelo",
    .hf_downloading = L"Descargando…",
    .hf_failed = L"Error al descargar.",
    .hf_invalid_url = L"Introduce la URL del modelo en huggingface.co (resolve, blob o página del modelo)",
    .hf_done = L"Registrado.",
    .openai_base_url_hint = L"es.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Clave de API",
    // REQ-051: botón [Eliminar ajustes].
    .openai_delete_settings = L"Eliminar ajustes",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Buscar actualizaciones",
    .update_idle_note = L"Comprueba si tienes la última versión de Emebala Chat.",
    .update_checking = L"Buscando actualizaciones…",
    .update_checking_hint = L"Se ejecuta en segundo plano; puedes cerrar esta ventana.",
    .update_uptodate = L"Ya tienes la versión más reciente",
    .update_dlg_title = L"Actualización disponible",
    .update_dlg_version = L"Versión nueva",
    .update_dlg_version_cur = L"tienes la {v}",
    .update_dlg_size = L"Tamaño",
    .update_dlg_size_approx = L"unos {s}",
    .update_dlg_privacy = L"Al comprobar y descargar se conecta a GitHub.",
    .update_dlg_download = L"Descargar",
    .update_dlg_later = L"Más tarde",
    .update_dl_progress = L"Descargando…",
    .update_dl_cancel = L"Cancelar",
    .update_dl_cancelled = L"Descarga cancelada.",
    .update_ready_title = L"Descarga completa — listo para instalar",
    .update_ready_warn = L"La aplicación se cerrará para instalar.",
    .update_ready_install = L"Instalar ahora",
    .update_ready_later = L"Más tarde",
    .update_err_offline = L"Estás sin conexión. Comprueba tu conexión e inténtalo de nuevo.",
    .update_err_rate = L"Demasiadas solicitudes. Inténtalo de nuevo en un rato.",
    .update_err_hash = L"El archivo descargado no pasó la verificación.",
    .update_err_retry = L"Reintentar",
    .update_snoozed_line = L"Puedes instalar la actualización más tarde.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Descargar modelo",
    .model_dl_consent_body = L"El modelo de traducción integrado no está instalado. ¿Descargarlo ahora? (unos {s})",
    .model_dl_privacy = L"Al descargar se conecta a huggingface.co.",
    .model_dl_download = L"Descargar ahora",
    .model_dl_verifying = L"Verificando la descarga…",
    .model_dl_placing = L"Instalando el modelo…",
    .model_dl_done = L"El modelo está listo. El motor local lo usará desde la próxima traducción.",
    .model_dl_failed = L"La descarga falló. Comprueba tu conexión e inténtalo de nuevo.",
    .model_dl_disk_space = L"No hay suficiente espacio libre en disco (se requieren unos {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Ajustes de carga del modelo de traducción…",
    .engine_load_settings_title = L"Carga del modelo de traducción",
    .load_mode_on_use = L"Cargar al usarlo (recomendado)",
    .load_mode_warmup = L"Precargar",
    .load_mode_on_use_desc = L"Ahorra memoria. La primera traducción puede tardar unos segundos más.",
    .load_mode_warmup_desc = L"La traducción empieza enseguida. El modelo permanece en memoria.",
    .gguf_policy_sticky = L"(Mantener residente)",
    .gguf_policy_evict = L"(Cargar al usarlo)",
    .gguf_manager_toggle_policy = L"Cambiar política",
    .gguf_duplicate_warning = L"Archivos duplicados: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Limpiar duplicados",
    .gguf_manager_dedupe_done = L"Los archivos duplicados se movieron a la Papelera de reciclaje.",
    .gguf_manager_dedupe_failed = L"No se pudieron limpiar los archivos duplicados. Comprueba si otro programa los está usando.",
    .menu_ui_theme = L"Tema de interfaz",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editorial cálido",
    .theme_monolith = L"Monolito refinado",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Mostrar retrotraducción flotante (verificación)",
    .back_translate_header = L"🔄 Verificación de retrotraducción",
    .back_translate_match_exact = L"✓ Significado coincidente",
    .back_translate_match_check = L"⚡ Revisar matiz",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"No es un archivo de modelo GGUF válido; no se ha registrado.",
    .gguf_manager_active_suffix = L"(en uso)",
    .gguf_manager_activated = L"Este modelo ahora está en uso.",
};
// 7. English (en) - Default Fallback
const LocalizedStrings kStringsEnglish = {
    L"Status: Active (F9: Pause)",
    L"Status: Paused (F9: Resume)",
    L"Translation Engine",
    L"Google Translate (Free / Zero-Install)",
    L"Built-in Local Engine (Hy-MT2-1.8B Offline)",
    L"Source Language (Input)",
    L"Target Language (Output)",
    L"Swap Source ⇄ Target (Double-click)",
    L"Auto-Send on Enter",
    L"Sound Feedback (Tones)",
    L"Floating Badge Visible",
    L"Start with Windows",
    L"Hotkey Cheat Sheet & Help...",
    L"Exit Emebala Chat",
    L"About Emebala Chat…",
    L"Emebala Chat Hotkeys & Usage Guide",
    L"Emebala Chat Hotkeys & Usage Guide:\n\n"
    L"  • F9 : Toggle Active / Paused\n"
    L"  • Ctrl + F9 : Cycle Target Language\n"
    L"  • Ctrl + Shift + Enter : Toggle Auto-Send Mode\n"
    L"  • Shift + Enter : Immediate Translate & Send\n\n"
    L"Floating Badge Mouse Controls:\n"
    L"  • Left Click : Toggle Active / Paused\n"
    L"  • Double Click : Swap Source ⇄ Target\n"
    L"  • Right Click : Open Settings Menu Anywhere\n\n"
    L"Translation Modes:\n"
    L"  • Replace-Only (Auto-Send OFF): Replaces line with translation for review.\n"
    L"  • Auto-Send (Auto-Send ON): Replaces line and immediately presses Enter.",
    L"About Emebala Chat",
    L"Active",
    L"Translating...",
    L"Paused",
    L"Emebala Chat",
    L"Could not copy the selected text. Check the target app and try again.",
    L"No text is selected to translate.",
    L"Auto Detect",
    L"Emebala Chat is already running in the background.\nCheck the system notification tray.",
    L"COM initialization failed.\nThe floating badge and text-to-speech will be unavailable,\nbut translation, hotkeys, tray and sounds still work.",
    L"Never copy-paste again. Type naturally in your native tongue \u2014 "
    L"translations replace your keystrokes in real time inside any Windows application.",
    L"\u26A1 Drag-to-Translate \u2014 select text in any app, the floating icon translates instantly.",
    L"🔊 Neural TTS — speaks all 37 languages via installed Windows voice packs.",
    L"\U0001F512 100% on-device & private \u2014 active only while the shortcut is held; clipboard untouched.",
    L"In 2000 BCE, Mesopotamian scribes called \u201CEme-bala\u201D \u2014 those who turn language to bridge worlds.",
    L"Website",
    L"Contact",
    L"Reddit",
    L"Team Sunplaza \u00B7 Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 \u00B7 Office hours 10:00\u201319:00 KST",
    L"Lead Architect: Yongtai Kim",
    L"\u2713 Copied!",
    L"\U0001F4CB Copy",
    L"\U0001F50A TTS",
    L"Interface Language",
    L"Auto (system language)",
    L"Reset to system defaults",
    L"Defaults restored",
    L"Keyboard Typing",
    L"Drag Tooltip",
    L"Emebala Chat",
    L"No Windows voice installed for this language. Click 🔊 again to open Speech settings.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Privacy Notice",
    L"How Emebala Chat handles your data:\n"
    L"\n"
    L"• Emebala Chat operates no servers of its own.\n"
    L"• With the local model, translated text never leaves your device.\n"
    L"• If you choose Google Translate, or the engine switches to cloud automatically, the selected or typed text is sent directly to Google for translation - not through Emebala.\n"
    L"• Diagnostic logs are OFF by default; enable them in settings (opt-in).\n"
    L"• If you do not want your text sent to the cloud (Google), open the tray icon menu, choose \"Translation Engine\" and select \"Built-in Local Engine\". With no local model installed and cloud fallback disabled, translation does not run - nothing is sent.\n"
    L"\n"
    L"You can re-read this anytime in the README file.\n",
    L"Settings file: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"The text was too long, so only its beginning and end were translated.",
    L"There is new untranslated text above. Place the cursor at the end of that line and press Enter to translate it.",
    L"Repairing the local engine components…",
    L"Local translation unavailable",
    L"The local translation engine files are missing, so translation is paused. Reinstall Emebala Chat to restore the local engine, or switch to cloud (Google) translation by choosing “Google Translate” under “Translation engine” in the tray menu.",
    L"OpenAI Compatible (custom server)…",
    L"OpenAI Compatible Engine Settings",
    L"OpenAI Compatible engine settings…",
    L"Base URL",
    L"API Key",
    L"Model",
    L"Fetch model list",
    L"Could not fetch the model list. You can type a model name directly.",
    L"Insecure connection (HTTP)",
    L"The base URL uses HTTP (not encrypted). Your API key and text will be sent in plaintext. Continue?",
    L"OpenAI Compatible settings saved.",
    L"Saved key: ",
    L"The base URL is not valid. Example: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"User-specified model (.gguf)",
    L"Register another .gguf model…",
    L"Translation quality notice",
    L"The selected model is not Hy-MT2. The current version uses the Hy-MT2-only prompt, so translation quality with this model is not guaranteed. Continue?",
    L"Model registered",
    // REQ-046 P4-2 (Rev2 §B-5, C2): drop the "pick Local LLM" instruction and
    // state the apply bound (up to ~1 minute, after the idle engine exits).
    L"The selected model has been registered with the local engine.\n\nThis model is used for translation when you choose \"Translation Engine > User model (.gguf)\".\n\nWhen it applies: the new model takes effect within about 1 minute at most (after the idle engine exits). Requests made before then may still use the previous model.",
    // REQ-047 D2 (design §B.3, Rev2 §6 wording): no direct 'local LLM' quote;
    // describe the capability — the built-in local translation engine can be
    // selected directly.
    L"This model is already built into Emebala Chat. No registration is needed - the built-in local translation engine can be selected directly.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(not registered)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Manage models…",
    L"User Model Manager",
    L"No user models registered.",
    L"Rename…",
    L"Delete…",
    L"Close",
    L"Delete model registration",
    L"The selected model's registration will be removed.\n"
    L"\n"
    L"The model file (.gguf) is KEPT on disk and is not deleted. If this model was in use, the translation engine selection returns to Auto.\n"
    L"\n"
    L"Continue?",
    L"Rename model",
    L"Enter a new name (up to 64 characters, no spaces).",
    L"That name cannot be used. Enter a non-empty name that differs from existing ones, without spaces or path separators, within 64 characters.",
    L"Changes saved.",

    L"registry.json could not be serialized (a filename was rejected). Nothing was changed.",
    L"%LOCALAPPDATA% is unavailable; cannot locate the shared models directory.",
    L"registry.json could not be written.",
    L"registry.json could not be written completely.",
    L"registry.json is damaged or has an unsupported schema. It was NOT modified. Fix or remove it, then retry.",
    L"Translation failed. Please try again.",
    L"The local translation engine is temporarily unavailable (it may be starting up). Please wait a moment and try again.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"OK",
    L"Cancel",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    L"Add from file…",
    L"Add from Hugging Face…",
    L"Add Hugging Face model",
    L"Model URL",
    L"Downloading…",
    L"Download failed.",
    L"Enter a huggingface.co model URL (resolve, blob, or model page)",
    L"Registered.",
    L"e.g. https://api.openai.com/v1",
    L"sk-...",
    // REQ-051: [Delete settings] button.
    L"Delete settings",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    L"Check for updates",
    L"See if you are running the latest Emebala Chat.",
    L"Checking for updates…",
    L"Runs in the background; you may close this window.",
    L"You are up to date",
    L"Update available",
    L"Version",
    L"you have {v}",
    L"Size",
    L"about {s}",
    L"Checking and downloading connect to GitHub.",
    L"Download",
    L"Later",
    L"Downloading…",
    L"Cancel",
    L"Download cancelled.",
    L"Download complete — ready to install",
    L"The app will close to install.",
    L"Install now",
    L"Later",
    L"You are offline. Check your connection, then try again.",
    L"Too many requests. Please try again in a little while.",
    L"The downloaded file did not pass verification.",
    L"Retry",
    L"You can install the update later.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    L"Model download",
    L"The built-in translation model is not installed. Download it now? (about {s})",
    L"Downloading connects to huggingface.co.",
    L"Download now",
    L"Verifying the download…",
    L"Installing the model…",
    L"The model is ready. The local engine will use it from the next translation.",
    L"The download failed. Check your connection and try again.",
    L"Not enough free disk space (about {s} required).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    L"Translation model load settings…",
    L"Translation model loading",
    L"Load when used (recommended)",
    L"Preload",
    L"Saves memory. The first translation may be a few seconds late.",
    L"Translation starts right away. The model stays in memory.",
    L"(Keep resident)",
    L"(Load on use)",
    L"Toggle policy",
    L"Duplicate files: {n} {s} × {c}",
    L"Clean up duplicates",
    L"The duplicate files were moved to the Recycle Bin.",
    L"The duplicate files could not be cleaned up. Check whether another program is using them.",
    // 261108_0002 (FIX-R1, v0.10.6): POSITIONAL TAIL ALIGNMENT - verified, not
    // assumed. This table is a positional aggregate (MSVC C7556 forbids mixing
    // styles in one aggregate), so every initializer lands on the struct member
    // at the SAME index. LocalizedStrings member order is:
    //   [1..157]   EMEBALA_LSTR_FIELDS X-macro (kLocalizedStringsFieldCount)
    //   [158..161] menu_ui_theme / theme_fluent / theme_editorial / theme_monolith
    //   [162..165] menu_back_translation / back_translate_header /
    //              back_translate_match_exact / back_translate_match_check
    //   [166]      back_translate_reference  (261108_0002 FIX-WS3B)
    //   [167..169] gguf_invalid_model_body / gguf_manager_active_suffix /
    //              gguf_manager_activated  (261111_0001 U6, v0.10.7)
    // = 169 initializers for 169 members. Before the FIX-R1 fix the four theme
    // slots were skipped, so the WS3 strings took 158..161 and the EN tray theme
    // submenu rendered "🔄 Back-Translation" while the WS3 fields silently kept
    // their struct defaults. The four theme values below are the struct defaults
    // spelled out (this is the English table, so they ARE the English strings).
    // Drift is now caught in three places: the sizeof static_assert next to
    // kLocalizedStringsFieldCount, the English element-count/alignment pins in
    // tests/run_tests.cpp, and MSVC itself for any wrong tail order.
    L"UI Theme",
    L"Fluent Craft",
    L"Warm Editorial",
    L"Refined Monolith",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle,
    // on struct slots 162..165.
    L"Floating Back-Translation",
    L"🔄 Back-Translation",
    L"✓ Meaning Matched",
    L"⚡ Check Nuance",
    // 261108_0002 (FIX-WS3B, v0.10.6): the card's reference-line label, slot 166.
    L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): slots 167..169 — the registration-gate
    // rejection body, the active-binding list marker, and the activation
    // notice (designated-initializer spellings forbidden here: MSVC C7556).
    L"Not a valid GGUF model file; it was not registered.",
    L"(in use)",
    L"This model is now in use.",
};
// ---- REQ-037 (P4 Batch B-3, design §2.1.2): 30 new locale tables below.
// Authored per §2-Q4 verdict A (aggregate-struct pattern). Field order and
// grouping mirror kStringsEnglish EXACTLY (49 since Batch-1 of session
// 260909_0001: 47 legacy + app_name + tooltip_no_tts_voice) so a reviewer can
// eyeball alignment (design §5.2 focus 3). Every field is consciously authored.
// Per REQ-B (session 260909_0001, decisions.md APPROVED brand transliteration
// table): the brand token "Emebala Chat" is now per-locale (app_name +
// re-bound composites) in the 17 non-Latin locales; Latin-20 keep the ASCII
// brand. Universal factual tokens remain: "Reddit", "Google Translate",
// "Hy-MT2-1.8B", contact data (org proper noun, address, phone, "KST",
// "Yongtai Kim").

// 8. French (fr)
const LocalizedStrings kStringsFrench = {
    .menu_status_active = L"Statut : actif (F9 : pause)",
    .menu_status_paused = L"Statut : en pause (F9 : reprendre)",
    .menu_engine = L"Moteur de traduction",
    .menu_engine_google = L"Google Traduction (gratuit / sans installation)",
    .menu_engine_local = L"Moteur local intégré (Hy-MT2-1.8B hors ligne)",
    .menu_source_lang = L"Langue source (entrée)",
    .menu_target_lang = L"Langue cible (sortie)",
    .menu_swap_langs = L"Inverser source ⇄ cible (double-clic)",
    .menu_auto_send = L"Envoi auto avec Entrée",
    .menu_sound_feedback = L"Retour sonore (tonalités)",
    .menu_show_badge = L"Badge flottant visible",
    .menu_start_with_windows = L"Démarrer avec Windows",
    .menu_cheatsheet = L"Aide-mémoire des raccourcis et aide...",
    .menu_exit = L"Quitter Emebala Chat",
    .menu_about = L"À propos d'Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — Raccourcis et guide d'utilisation",
    .cheatsheet_body = L"Raccourcis et guide d'Emebala Chat :\n\n"
    L"  • F9 : activer / mettre en pause\n"
    L"  • Ctrl + F9 : changer de langue cible\n"
    L"  • Ctrl + Maj + Entrée : activer/désactiver l'envoi auto\n"
    L"  • Maj + Entrée : traduire et envoyer immédiatement\n\n"
    L"Souris sur le badge flottant :\n"
    L"  • Clic gauche : activer / pause\n"
    L"  • Double-clic : inverser source ⇄ cible\n"
    L"  • Clic droit : ouvrir le menu des réglages\n\n"
    L"Modes de traduction :\n"
    L"  • Remplacement seul (envoi auto désactivé) : remplace la ligne par la traduction pour relecture.\n"
    L"  • Envoi auto activé : remplace la ligne puis appuie immédiatement sur Entrée.",
    .about_title = L"À propos d'Emebala Chat",
    .badge_active = L"Actif",
    .badge_translating = L"Traduction...",
    .badge_paused = L"En pause",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Impossible de copier le texte sélectionné. Vérifiez l'application cible et réessayez.",
    .tooltip_no_selection = L"Aucun texte sélectionné à traduire.",
    .auto_detect = L"Détection auto",
    .app_already_running = L"Emebala Chat est déjà exécuté en arrière-plan.\nVérifiez la zone de notification système.",
    .app_com_failed = L"L'initialisation COM a échoué.\nLe badge flottant et la synthèse vocale seront indisponibles,\nmais la traduction, les raccourcis, la barre d'état et les sons fonctionnent toujours.",
    .about_tagline = L"Fini le copier-coller. Tapez naturellement dans votre langue maternelle — la traduction remplace votre frappe en temps réel dans n'importe quelle application Windows.",
    .about_feature0 = L"⚡ Glisser-traduire — sélectionnez un texte dans n'importe quelle app, l'icône flottante traduit instantanément.",
    .about_feature1 = L"🔊 TTS neuronal — prononce les 37 langues via les packs de voix Windows installés.",
    .about_feature2 = L"🔒 100 % local et privé — actif uniquement pendant que le raccourci est maintenu ; presse-papiers intact.",
    .about_etymology = L"En 2000 av. J.-C., les scribes mésopotamiens appelaient « Eme-bala » ceux qui font de la langue un pont entre les mondes.",
    .about_link_website = L"Site web",
    .about_link_contact = L"Contact",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Séoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Heures ouvrables 10:00–19:00 KST",
    .about_contact_lead = L"Architecte principal : Yongtai Kim",
    .tooltip_copied = L"✓ Copié !",
    .tooltip_button_copy = L"📋 Copier",
    .tooltip_button_tts = L"🔊 Voix",
    .menu_ui_language = L"Langue de l'interface",
    .menu_ui_language_auto = L"Auto (langue système)",
    .about_reset_button = L"Réinitialiser aux valeurs système",
    .about_reset_done = L"Valeurs restaurées",
    .menu_typing_group = L"Saisie au clavier",
    .menu_tooltip_group = L"Infobulle de glissement",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Aucune voix Windows installée pour cette langue. Cliquez à nouveau sur 🔊 pour ouvrir les paramètres de la Voix.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Avertissement de confidentialité",
    .privacy_notice_body = L"Voici les principes de confidentialité d’Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat n’exploite aucun serveur propre.\n"
    L"• Avec le modèle local, le texte traduit ne quitte jamais votre appareil.\n"
    L"• Si vous choisissez Google Traduction ou si le basculement cloud est automatique, le texte sélectionné ou saisi est envoyé directement à Google pour la traduction, sans transiter par Emebala.\n"
    L"• Les journaux de diagnostic sont DÉSACTIVÉS par défaut ; activez-les dans la configuration (consentement explicite).\n"
    L"• Si vous ne souhaitez pas envoyer de texte vers le cloud (Google), ouvrez le menu de l'icône de la barre des tâches, choisissez « Moteur de traduction » puis « Moteur local intégré ». Sans modèle local installé et avec le repli cloud désactivé, la traduction ne s'exécute pas : rien n'est envoyé.\n"
    L"\n"
    L"Le détail complet est dans le fichier README, relisible à tout moment.\n",
    .cheatsheet_config_path = L"Fichier de configuration : %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Texte trop long : seul le début et la fin ont été traduits.",
    .tooltip_untranslated_above = L"Il y a un nouveau texte non traduit au-dessus. Placez le curseur à la fin de cette ligne et appuyez sur Entrée pour le traduire.",
    .repair_in_progress = L"Restauration des composants du moteur local…",
    .repair_failed_title = L"Traduction locale indisponible",
    .repair_failed_body = L"Les fichiers du moteur de traduction locale sont introuvables, la traduction est donc suspendue. Réinstallez Emebala Chat pour restaurer le moteur local, ou passez à la traduction cloud (Google) en choisissant « Google Traduction » dans le menu de la barre d’état, « Moteur de traduction ».",
    .menu_engine_openai = L"Compatible OpenAI (serveur personnalisé)…",
    .openai_settings_title = L"Paramètres du moteur compatible OpenAI",
    .openai_settings_action = L"Paramètres du moteur compatible OpenAI…",
    .openai_base_url_label = L"URL de base",
    .openai_api_key_label = L"Clé API",
    .openai_model_label = L"Modèle",
    .openai_fetch_models = L"Récupérer la liste des modèles",
    .openai_fetch_failed = L"Impossible de récupérer la liste des modèles. Vous pouvez saisir un nom de modèle directement.",
    .openai_http_warning_title = L"Connexion non sécurisée (HTTP)",
    .openai_http_warning_body = L"L'URL de base utilise HTTP (non chiffré). Votre clé API et votre texte seront envoyés en clair. Continuer ?",
    .openai_saved = L"Paramètres compatibles OpenAI enregistrés.",
    .openai_key_masked = L"Clé enregistrée : ",
    .openai_invalid_base_url = L"L'URL de base n'est pas valide. Exemple : https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Modèle spécifié par l'utilisateur (.gguf)",
    .menu_browse_gguf_file = L"Enregistrer un autre modèle .gguf…",
    .user_gguf_quality_title = L"Avis sur la qualité de la traduction",
    .user_gguf_quality_body = L"Le modèle sélectionné n'est pas Hy-MT2. La version actuelle utilise le prompt réservé à Hy-MT2 ; la qualité de traduction avec ce modèle n'est donc pas garantie. Continuer ?",
    .user_gguf_registered_title = L"Modèle enregistré",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Le modèle sélectionné a été enregistré auprès du moteur local.\n"
    L"\n"
    L"Ce modèle est utilisé pour la traduction lorsque vous choisissez « Sélection du moteur de traduction > Choix de l'utilisateur (.gguf) ».\n"
    L"\n"
    L"Prise d'effet : au maximum environ 1 minute après l'enregistrement (après la sortie du moteur par inactivité)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Ce modèle est déjà intégré à Emebala Chat. Aucune inscription n'est nécessaire. Vous pouvez sélectionner directement le moteur de traduction local intégré.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(non enregistré)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Gérer les modèles…",
    .gguf_manager_title = L"Gestionnaire de modèles utilisateur",
    .gguf_manager_empty = L"Aucun modèle utilisateur enregistré.",
    .gguf_manager_rename = L"Renommer…",
    .gguf_manager_delete = L"Supprimer…",
    .gguf_manager_close = L"Fermer",
    .gguf_manager_delete_confirm_title = L"Supprimer l'inscription du modèle",
    .gguf_manager_delete_confirm_body = L"L'inscription du modèle sélectionné sera supprimée.\n"
    L"\n"
    L"Le fichier du modèle (.gguf) est conservé sur le disque et n'est pas supprimé. Si ce modèle était utilisé, le choix du moteur de traduction reviendra à Auto.\n"
    L"\n"
    L"Continuer ?",
    .gguf_manager_rename_title = L"Renommer le modèle",
    .gguf_manager_rename_body = L"Saisissez un nouveau nom (64 caractères max, sans espaces).",
    .gguf_manager_rename_invalid = L"Ce nom ne peut pas être utilisé. Saisissez un nom non vide, différent des noms existants, sans espaces ni séparateurs de chemin, de 64 caractères maximum.",
    .gguf_manager_done = L"Modifications enregistrées.",
    .gguf_manager_err_serialize = L"Impossible de sérialiser registry.json (un nom de fichier a été refusé). Rien n'a été modifié.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% est indisponible ; impossible de localiser le dossier de modèles partagé.",
    .gguf_manager_err_write = L"Impossible d'écrire registry.json.",
    .gguf_manager_err_write_partial = L"registry.json n'a pas pu être écrit complètement.",
    .gguf_manager_err_registry_damaged = L"registry.json est endommagé ou utilise un schéma non pris en charge. Il n'a PAS été modifié. Réparez-le ou supprimez-le, puis réessayez.",
    .tooltip_translate_failed = L"Échec de la traduction. Veuillez réessayer.",
    .repair_transient_body = L"Le moteur de traduction local est temporairement indisponible (il est peut-être en cours de démarrage). Patientez un instant puis réessayez.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Valider",
    .dialog_cancel = L"Annuler",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Ajouter depuis un fichier…",
    .gguf_manager_add_hf = L"Ajouter depuis Hugging Face…",
    .hf_add_title = L"Ajouter un modèle Hugging Face",
    .hf_url_label = L"URL du modèle",
    .hf_downloading = L"Téléchargement en cours…",
    .hf_failed = L"Échec du téléchargement.",
    .hf_invalid_url = L"Saisissez l'URL du modèle huggingface.co (resolve, blob ou page du modèle)",
    .hf_done = L"Enregistré.",
    .openai_base_url_hint = L"par ex. : https://api.openai.com/v1",
    .openai_api_key_hint = L"Clé API",
    // REQ-051: bouton [Supprimer les paramètres].
    .openai_delete_settings = L"Supprimer les paramètres",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Vérifier les mises à jour",
    .update_idle_note = L"Vérifiez que vous utilisez la dernière version d'Emebala Chat.",
    .update_checking = L"Recherche de mises à jour…",
    .update_checking_hint = L"S'exécute en arrière-plan ; vous pouvez fermer cette fenêtre.",
    .update_uptodate = L"Vous êtes à jour",
    .update_dlg_title = L"Mise à jour disponible",
    .update_dlg_version = L"Nouvelle version",
    .update_dlg_version_cur = L"vous avez la {v}",
    .update_dlg_size = L"Taille",
    .update_dlg_size_approx = L"environ {s}",
    .update_dlg_privacy = L"La vérification et le téléchargement se connectent à GitHub.",
    .update_dlg_download = L"Télécharger",
    .update_dlg_later = L"Plus tard",
    .update_dl_progress = L"Téléchargement…",
    .update_dl_cancel = L"Annuler",
    .update_dl_cancelled = L"Téléchargement annulé.",
    .update_ready_title = L"Téléchargement terminé — prêt à installer",
    .update_ready_warn = L"L'application se fermera pour s'installer.",
    .update_ready_install = L"Installer maintenant",
    .update_ready_later = L"Plus tard",
    .update_err_offline = L"Vous êtes hors ligne. Vérifiez votre connexion, puis réessayez.",
    .update_err_rate = L"Trop de requêtes. Veuillez réessayer dans un moment.",
    .update_err_hash = L"Le fichier téléchargé n'a pas passé la vérification.",
    .update_err_retry = L"Réessayer",
    .update_snoozed_line = L"Vous pouvez installer la mise à jour plus tard.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Téléchargement du modèle",
    .model_dl_consent_body = L"Le modèle de traduction intégré n'est pas installé. Le télécharger maintenant ? (environ {s})",
    .model_dl_privacy = L"Le téléchargement se connecte à huggingface.co.",
    .model_dl_download = L"Télécharger maintenant",
    .model_dl_verifying = L"Vérification du téléchargement…",
    .model_dl_placing = L"Installation du modèle…",
    .model_dl_done = L"Le modèle est prêt. Le moteur local l'utilisera dès la prochaine traduction.",
    .model_dl_failed = L"Le téléchargement a échoué. Vérifiez votre connexion et réessayez.",
    .model_dl_disk_space = L"Espace disque libre insuffisant (environ {s} requis).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Paramètres de chargement du modèle de traduction…",
    .engine_load_settings_title = L"Chargement du modèle de traduction",
    .load_mode_on_use = L"Charger à l'utilisation (recommandé)",
    .load_mode_warmup = L"Précharger",
    .load_mode_on_use_desc = L"Économise la mémoire. La première traduction peut être retardée de quelques secondes.",
    .load_mode_warmup_desc = L"La traduction démarre immédiatement. Le modèle reste en mémoire.",
    .gguf_policy_sticky = L"(Rester résident)",
    .gguf_policy_evict = L"(Charger à l'utilisation)",
    .gguf_manager_toggle_policy = L"Changer de politique",
    .gguf_duplicate_warning = L"Fichiers en double : {n} {s} × {c}",
    .gguf_manager_dedupe = L"Nettoyer les doublons",
    .gguf_manager_dedupe_done = L"Les fichiers en double ont été déplacés dans la Corbeille.",
    .gguf_manager_dedupe_failed = L"Impossible de nettoyer les fichiers en double. Vérifiez si un autre programme les utilise.",
    .menu_ui_theme = L"Thème d'interface",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Éditorial chaleureux",
    .theme_monolith = L"Monolithe raffiné",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Afficher la rétrotraduction flottante (vérification)",
    .back_translate_header = L"🔄 Vérification de rétrotraduction",
    .back_translate_match_exact = L"✓ Sens concordant",
    .back_translate_match_check = L"⚡ Vérifier la nuance",
    .back_translate_reference = L"↳ Original : ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Ce fichier n'est pas un modèle GGUF valide ; il n'a pas été enregistré.",
    .gguf_manager_active_suffix = L"(en cours d'utilisation)",
    .gguf_manager_activated = L"Ce modèle est maintenant utilisé.",
};
// 9. German (de)
const LocalizedStrings kStringsGerman = {
    .menu_status_active = L"Status: Aktiv (F9: Pause)",
    .menu_status_paused = L"Status: Pausiert (F9: Fortsetzen)",
    .menu_engine = L"Übersetzungsengine",
    .menu_engine_google = L"Google Übersetzen (kostenlos / ohne Installation)",
    .menu_engine_local = L"Eingebaute lokale Engine (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Ausgangssprache (Eingabe)",
    .menu_target_lang = L"Zielsprache (Ausgabe)",
    .menu_swap_langs = L"Ausgangssprache ⇄ Zielsprache tauschen (Doppelklick)",
    .menu_auto_send = L"Automatisch mit Enter senden",
    .menu_sound_feedback = L"Sound-Rückmeldung (Töne)",
    .menu_show_badge = L"Schwebendes Badge sichtbar",
    .menu_start_with_windows = L"Mit Windows starten",
    .menu_cheatsheet = L"Tastenkürzel-Spickzettel und Hilfe...",
    .menu_exit = L"Emebala Chat beenden",
    .menu_about = L"Über Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — Tastenkürzel und Anleitung",
    .cheatsheet_body = L"Tastenkürzel und Anleitung für Emebala Chat:\n\n"
    L"  • F9 : Aktivieren / Pausieren\n"
    L"  • Strg + F9 : Zielsprache wechseln\n"
    L"  • Strg + Umschalt + Enter : Auto-Send umschalten\n"
    L"  • Umschalt + Enter : Übersetzen und sofort senden\n\n"
    L"Maussteuerung auf dem Badge:\n"
    L"  • Linksklick : Aktivieren / Pausieren\n"
    L"  • Doppelklick : Ausgangs- ⇄ Zielsprache tauschen\n"
    L"  • Rechtsklick : Einstellungsmenü öffnen\n\n"
    L"Übersetzungsmodi:\n"
    L"  • Nur Ersetzen (Auto-Send aus): ersetzt die Zeile durch die Übersetzung zur Kontrolle.\n"
    L"  • Auto-Send (ein): ersetzt die Zeile und drückt sofort Enter.",
    .about_title = L"Über Emebala Chat",
    .badge_active = L"Aktiv",
    .badge_translating = L"Übersetze...",
    .badge_paused = L"Pausiert",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Der markierte Text konnte nicht kopiert werden. Prüfen Sie die Ziel-App und versuchen Sie es erneut.",
    .tooltip_no_selection = L"Es ist kein Text zum Übersetzen ausgewählt.",
    .auto_detect = L"Automatisch erkennen",
    .app_already_running = L"Emebala Chat läuft bereits im Hintergrund.\nPrüfen Sie das Benachrichtigungsfeld.",
    .app_com_failed = L"COM-Initialisierung fehlgeschlagen.\nSchwebendes Badge und Sprachausgabe sind nicht verfügbar,\naber Übersetzung, Tastenkürzel, Tray und Sounds funktionieren weiterhin.",
    .about_tagline = L"Schluss mit Kopieren und Einfügen. Tippen Sie natürlich in Ihrer Muttersprache — die Übersetzung ersetzt Ihre Eingabe in Echtzeit in jeder Windows-Anwendung.",
    .about_feature0 = L"⚡ Ziehen-und-Übersetzen — Text in einer beliebigen App markieren, das schwebende Symbol übersetzt sofort.",
    .about_feature1 = L"🔊 Neuronales TTS — spricht alle 37 Sprachen über installierte Windows-Sprachpakete.",
    .about_feature2 = L"🔒 100 % lokal und privat — nur aktiv, während das Tastenkürzel gehalten wird; Zwischenablage unberührt.",
    .about_etymology = L"Im Jahr 2000 v. Chr. nannten mesopotamische Schreiber „Eme-bala“ — jene, die Sprache zur Brücke zwischen Welten machen.",
    .about_link_website = L"Webseite",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Bürozeiten 10:00–19:00 KST",
    .about_contact_lead = L"Leitender Architekt: Yongtai Kim",
    .tooltip_copied = L"✓ Kopiert!",
    .tooltip_button_copy = L"📋 Kopieren",
    .tooltip_button_tts = L"🔊 Stimme",
    .menu_ui_language = L"Oberflächensprache",
    .menu_ui_language_auto = L"Auto (Systemsprache)",
    .about_reset_button = L"Auf Systemstandards zurücksetzen",
    .about_reset_done = L"Standards wiederhergestellt",
    .menu_typing_group = L"Tastatureingabe",
    .menu_tooltip_group = L"Drag-Tooltip",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Für diese Sprache ist keine Windows-Stimme installiert. Klicken Sie erneut auf 🔊, um die Spracheinstellungen zu öffnen.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Datenschutzhinweis",
    .privacy_notice_body = L"Dies sind die Datenschutzgrundsätze von Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat betreibt keine eigenen Server.\n"
    L"• Bei Nutzung des lokalen Modells verlässt der übersetzte Text Ihr Gerät nicht.\n"
    L"• Wenn Sie Google Übersetzen wählen oder automatisch in den Cloud-Modus gewechselt wird, wird der markierte oder eingegebene Text direkt an Google zur Übersetzung gesendet – nicht über Emebala.\n"
    L"• Diagnostic-Protokolle sind standardmäßig AUS; aktivieren Sie sie in den Einstellungen (Opt-in).\n"
    L"• Wenn Sie keinen Text in die Cloud (Google) senden möchten, öffnen Sie das Menü des Taskleistensymbols, wählen Sie “Übersetzungsengine” und dann “Eingebaute lokale Engine”. Ohne installiertes lokales Modell und mit deaktiviertem Cloud-Fallback wird nicht übersetzt — es wird nichts gesendet.\n"
    L"\n"
    L"Details stehen in der README-Datei – jederzeit erneut lesbar.\n",
    .cheatsheet_config_path = L"Konfigurationsdatei: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Der Text war zu lang: Es wurden nur Anfang und Ende übersetzt.",
    .tooltip_untranslated_above = L"Oben gibt es neuen unübersetzten Text. Setzen Sie den Cursor an das Ende dieser Zeile und drücken Sie die Eingabetaste, um ihn zu übersetzen.",
    .repair_in_progress = L"Lokale Engine-Komponenten werden repariert…",
    .repair_failed_title = L"Lokale Übersetzung nicht verfügbar",
    .repair_failed_body = L"Die Dateien der lokalen Übersetzungsengine fehlen, daher ist die Übersetzung vorübergehend angehalten. Installieren Sie Emebala Chat erneut, um die lokale Engine wiederherzustellen, oder wechseln Sie im Tray-Menü unter „Übersetzungsengine“ zu „Google Übersetzer“.",
    .menu_engine_openai = L"OpenAI-kompatibel (benutzerdefinierter Server)…",
    .openai_settings_title = L"OpenAI-kompatible Engine-Einstellungen",
    .openai_settings_action = L"OpenAI-kompatible Engine-Einstellungen…",
    .openai_base_url_label = L"Basis-URL",
    .openai_api_key_label = L"API-Schlüssel",
    .openai_model_label = L"Modell",
    .openai_fetch_models = L"Modellliste abrufen",
    .openai_fetch_failed = L"Die Modellliste konnte nicht abgerufen werden. Sie können einen Modellnamen direkt eingeben.",
    .openai_http_warning_title = L"Unsichere Verbindung (HTTP)",
    .openai_http_warning_body = L"Die Basis-URL verwendet HTTP (unverschlüsselt). Ihr API-Schlüssel und Ihr Text werden im Klartext gesendet. Fortfahren?",
    .openai_saved = L"OpenAI-kompatible Einstellungen gespeichert.",
    .openai_key_masked = L"Gespeicherter Schlüssel: ",
    .openai_invalid_base_url = L"Die Basis-URL ist ungültig. Beispiel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Benutzerdefiniertes Modell (.gguf)",
    .menu_browse_gguf_file = L"Anderes .gguf-Modell registrieren…",
    .user_gguf_quality_title = L"Hinweis zur Übersetzungsqualität",
    .user_gguf_quality_body = L"Das ausgewählte Modell ist nicht Hy-MT2. Die aktuelle Version verwendet den Hy-MT2-spezifischen Prompt, daher ist die Übersetzungsqualität mit diesem Modell nicht garantiert. Fortfahren?",
    .user_gguf_registered_title = L"Modell registriert",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Das ausgewählte Modell wurde bei der lokalen Engine registriert.\n"
    L"\n"
    L"Dieses Modell wird für die Übersetzung verwendet, wenn Sie \"Übersetzungsengine wählen > Benutzerauswahl (.gguf)\" wählen.\n"
    L"\n"
    L"Wann wirksam: bis zu etwa 1 Minute nach der Registrierung (nach dem Leerlaufende der Engine)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Dieses Modell ist bereits in Emebala Chat integriert. Eine Registrierung ist nicht erforderlich. Sie können die integrierte lokale Übersetzungsengine direkt auswählen.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(nicht registriert)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Modelle verwalten…",
    .gguf_manager_title = L"Benutzermodell-Verwaltung",
    .gguf_manager_empty = L"Keine Benutzermodelle registriert.",
    .gguf_manager_rename = L"Umbenennen…",
    .gguf_manager_delete = L"Löschen…",
    .gguf_manager_close = L"Schließen",
    .gguf_manager_delete_confirm_title = L"Modellregistrierung löschen",
    .gguf_manager_delete_confirm_body = L"Die Registrierung des ausgewählten Modells wird entfernt.\n"
    L"\n"
    L"Die Modelldatei (.gguf) bleibt auf dem Datenträger erhalten und wird nicht gelöscht. Wenn dieses Modell verwendet wurde, kehrt die Auswahl der Übersetzungsengine zu Automatisch (Auto) zurück.\n"
    L"\n"
    L"Fortfahren?",
    .gguf_manager_rename_title = L"Modell umbenennen",
    .gguf_manager_rename_body = L"Neuen Namen eingeben (max. 64 Zeichen, ohne Leerzeichen).",
    .gguf_manager_rename_invalid = L"Dieser Name kann nicht verwendet werden. Bitte einen nicht leeren Namen eingeben, der von vorhandenen abweicht und keine Leerzeichen oder Pfadtrennzeichen enthält (max. 64 Zeichen).",
    .gguf_manager_done = L"Änderungen gespeichert.",
    .gguf_manager_err_serialize = L"registry.json konnte nicht serialisiert werden (ein Dateiname wurde abgelehnt). Es wurde nichts geändert.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% ist nicht verfügbar; das freigegebene Modellverzeichnis kann nicht gefunden werden.",
    .gguf_manager_err_write = L"registry.json konnte nicht geschrieben werden.",
    .gguf_manager_err_write_partial = L"registry.json konnte nicht vollständig geschrieben werden.",
    .gguf_manager_err_registry_damaged = L"registry.json ist beschädigt oder hat ein nicht unterstütztes Schema. Es wurde NICHT geändert. Reparieren oder entfernen Sie es und versuchen Sie es erneut.",
    .tooltip_translate_failed = L"Übersetzung fehlgeschlagen. Bitte versuchen Sie es erneut.",
    .repair_transient_body = L"Die lokale Übersetzungsengine ist vorübergehend nicht verfügbar (möglicherweise beim Start). Warten Sie einen Moment und versuchen Sie es erneut.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Bestätigen",
    .dialog_cancel = L"Abbrechen",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Aus Datei hinzufügen…",
    .gguf_manager_add_hf = L"Von Hugging Face hinzufügen…",
    .hf_add_title = L"Hugging-Face-Modell hinzufügen",
    .hf_url_label = L"Modell-URL",
    .hf_downloading = L"Wird heruntergeladen…",
    .hf_failed = L"Download fehlgeschlagen.",
    .hf_invalid_url = L"Geben Sie die huggingface.co-Modell-URL ein (resolve, blob oder Modellseite)",
    .hf_done = L"Registriert.",
    .openai_base_url_hint = L"z. B. https://api.openai.com/v1",
    .openai_api_key_hint = L"API-Schlüssel",
    // REQ-051: Schaltfläche [Einstellungen löschen].
    .openai_delete_settings = L"Einstellungen löschen",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Nach Updates suchen",
    .update_idle_note = L"Prüfen, ob Sie die neueste Version von Emebala Chat nutzen.",
    .update_checking = L"Nach Updates wird gesucht…",
    .update_checking_hint = L"Läuft im Hintergrund; Sie können dieses Fenster schließen.",
    .update_uptodate = L"Sie sind auf dem neuesten Stand",
    .update_dlg_title = L"Update verfügbar",
    .update_dlg_version = L"Neue Version",
    .update_dlg_version_cur = L"Sie haben {v}",
    .update_dlg_size = L"Größe",
    .update_dlg_size_approx = L"etwa {s}",
    .update_dlg_privacy = L"Prüfung und Download stellen eine Verbindung zu GitHub her.",
    .update_dlg_download = L"Herunterladen",
    .update_dlg_later = L"Später",
    .update_dl_progress = L"Wird heruntergeladen…",
    .update_dl_cancel = L"Abbrechen",
    .update_dl_cancelled = L"Download abgebrochen.",
    .update_ready_title = L"Download abgeschlossen — bereit zur Installation",
    .update_ready_warn = L"Die App wird zum Installieren geschlossen.",
    .update_ready_install = L"Jetzt installieren",
    .update_ready_later = L"Später",
    .update_err_offline = L"Sie sind offline. Prüfen Sie Ihre Verbindung und versuchen Sie es erneut.",
    .update_err_rate = L"Zu viele Anfragen. Bitte versuchen Sie es gleich erneut.",
    .update_err_hash = L"Die heruntergeladene Datei hat die Überprüfung nicht bestanden.",
    .update_err_retry = L"Erneut versuchen",
    .update_snoozed_line = L"Sie können das Update später installieren.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Modell herunterladen",
    .model_dl_consent_body = L"Das integrierte Übersetzungsmodell ist nicht installiert. Jetzt herunterladen? (etwa {s})",
    .model_dl_privacy = L"Der Download stellt eine Verbindung zu huggingface.co her.",
    .model_dl_download = L"Jetzt herunterladen",
    .model_dl_verifying = L"Download wird überprüft…",
    .model_dl_placing = L"Modell wird installiert…",
    .model_dl_done = L"Das Modell ist bereit. Die lokale Engine nutzt es ab der nächsten Übersetzung.",
    .model_dl_failed = L"Der Download ist fehlgeschlagen. Prüfen Sie Ihre Verbindung und versuchen Sie es erneut.",
    .model_dl_disk_space = L"Nicht genügend freier Speicherplatz (etwa {s} erforderlich).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Einstellungen zum Laden des Übersetzungsmodells…",
    .engine_load_settings_title = L"Laden des Übersetzungsmodells",
    .load_mode_on_use = L"Bei Verwendung laden (empfohlen)",
    .load_mode_warmup = L"Vorab laden",
    .load_mode_on_use_desc = L"Spart Arbeitsspeicher. Die erste Übersetzung kann einige Sekunden später beginnen.",
    .load_mode_warmup_desc = L"Die Übersetzung startet sofort. Das Modell bleibt im Arbeitsspeicher.",
    .gguf_policy_sticky = L"(Dauerhaft laden)",
    .gguf_policy_evict = L"(Bei Verwendung laden)",
    .gguf_manager_toggle_policy = L"Richtlinie wechseln",
    .gguf_duplicate_warning = L"Duplizierte Dateien: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Duplikate bereinigen",
    .gguf_manager_dedupe_done = L"Die duplizierten Dateien wurden in den Papierkorb verschoben.",
    .gguf_manager_dedupe_failed = L"Die duplizierten Dateien konnten nicht bereinigt werden. Prüfen Sie, ob ein anderes Programm sie verwendet.",
    .menu_ui_theme = L"UI-Design",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Warmes Editorial",
    .theme_monolith = L"Reduzierter Monolith",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Schwebende Rückübersetzung anzeigen (Prüfung)",
    .back_translate_header = L"🔄 Rückübersetzungs-Prüfung",
    .back_translate_match_exact = L"✓ Bedeutung stimmt überein",
    .back_translate_match_check = L"⚡ Nuance prüfen",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Keine gültige GGUF-Modelldatei; sie wurde nicht registriert.",
    .gguf_manager_active_suffix = L"(in Verwendung)",
    .gguf_manager_activated = L"Dieses Modell wird jetzt verwendet.",
};
// 10. Russian (ru)
const LocalizedStrings kStringsRussian = {
    .menu_status_active = L"Состояние: активно (F9: пауза)",
    .menu_status_paused = L"Состояние: пауза (F9: возобновить)",
    .menu_engine = L"Движок перевода",
    .menu_engine_google = L"Google Переводчик (бесплатно / без установки)",
    .menu_engine_local = L"Встроенный локальный движок (Hy-MT2-1.8B офлайн)",
    .menu_source_lang = L"Исходный язык (ввод)",
    .menu_target_lang = L"Язык перевода (вывод)",
    .menu_swap_langs = L"Поменять исходный ⇄ целевой (двойной клик)",
    .menu_auto_send = L"Автоотправка по Enter",
    .menu_sound_feedback = L"Звуковая обратная связь",
    .menu_show_badge = L"Показывать плавающий бейдж",
    .menu_start_with_windows = L"Запускать с Windows",
    .menu_cheatsheet = L"Шпаргалка горячих клавиш и помощь...",
    .menu_exit = L"Выйти из Эмебала Чат",
    .menu_about = L"Об Эмебала Чат…",
    .cheatsheet_title = L"Эмебала Чат: горячие клавиши и руководство",
    .cheatsheet_body = L"Горячие клавиши и руководство Эмебала Чат:\n\n"
    L"  • F9 : включить / пауза\n"
    L"  • Ctrl + F9 : смена языка перевода\n"
    L"  • Ctrl + Shift + Enter : режим автоотправки\n"
    L"  • Shift + Enter : перевести и сразу отправить\n\n"
    L"Управление мышью на бейдже:\n"
    L"  • Левый клик : включить / пауза\n"
    L"  • Двойной клик : поменять языки местами\n"
    L"  • Правый клик : открыть меню настроек\n\n"
    L"Режимы перевода:\n"
    L"  • Только замена (автоотправка выкл): заменяет строку переводом для проверки.\n"
    L"  • Автоотправка (вкл): заменяет строку и сразу нажимает Enter.",
    .about_title = L"Об Эмебала Чат",
    .badge_active = L"Активно",
    .badge_translating = L"Перевод...",
    .badge_paused = L"Пауза",
    .tooltip_title = L"Эмебала Чат",
    .tooltip_copy_failed = L"Не удалось скопировать выделенный текст. Проверьте целевое приложение и повторите попытку.",
    .tooltip_no_selection = L"Текст для перевода не выбран.",
    .auto_detect = L"Определять автоматически",
    .app_already_running = L"Эмебала Чат уже запущен в фоновом режиме.\nПроверьте область уведомлений.",
    .app_com_failed = L"Сбой инициализации COM.\nПлавающий бейдж и озвучивание текста будут недоступны,\nно перевод, горячие клавиши, трей и звуки продолжат работать.",
    .about_tagline = L"Хватит копировать и вставлять. Печатайте естественно на родном языке — перевод заменяет ваш ввод в реальном времени в любом приложении Windows.",
    .about_feature0 = L"⚡ Перевод перетаскиванием — выделите текст в любом приложении, плавающий значок переведёт мгновенно.",
    .about_feature1 = L"🔊 Нейронное TTS — озвучивает все 37 языков через установленные голосовые пакеты Windows.",
    .about_feature2 = L"🔒 100 % на устройстве и приватно — активно, только пока удерживается горячая клавиша; буфер обмена не затрагивается.",
    .about_etymology = L"В 2000 г. до н. э. месопотамские писцы называли «Eme-bala» тех, кто превращает язык в мост между мирами.",
    .about_link_website = L"Сайт",
    .about_link_contact = L"Контакты",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Рабочие часы 10:00–19:00 KST",
    .about_contact_lead = L"Ведущий архитектор: Yongtai Kim",
    .tooltip_copied = L"✓ Скопировано!",
    .tooltip_button_copy = L"📋 Копировать",
    .tooltip_button_tts = L"🔊 Озвучка",
    .menu_ui_language = L"Язык интерфейса",
    .menu_ui_language_auto = L"Авто (язык системы)",
    .about_reset_button = L"Сброс к системным значениям",
    .about_reset_done = L"Значения восстановлены",
    .menu_typing_group = L"Ввод с клавиатуры",
    .menu_tooltip_group = L"Всплывающая подсказка",
    .app_name = L"Эмебала Чат",
    .tooltip_no_tts_voice = L"Для этого языка не установлен голос Windows. Нажмите 🔊 ещё раз, чтобы открыть параметры распознавания речи.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Уведомление о конфиденциальности",
    .privacy_notice_body = L"Принципы обработки данных в Emebala Chat:\n"
    L"\n"
    L"• Emebala Chat не использует собственные серверы.\n"
    L"• При локальной модели переводимый текст не покидает устройство.\n"
    L"• Если выбран Google Переводчик или включён автопереход в облако, выделенный или введённый текст отправляется напрямую в Google для перевода, минуя Emebala.\n"
    L"• Диагностические журналы по умолчанию ВЫКЛЮЧЕНЫ; их можно включить в настройках (по согласию).\n"
    L"• Если вы не хотите отправлять текст в облако (Google), откройте меню значка в системном трее, выберите «Движок перевода» и укажите «Встроенный локальный движок». Без установленной локальной модели и при отключённом облачном резерве перевод не выполняется — ничего не отправляется.\n"
    L"\n"
    L"Полные сведения — в файле README, доступном для чтения в любое время.\n",
    .cheatsheet_config_path = L"Файл конфигурации: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Текст слишком длинный: переведены только начало и конец.",
    .tooltip_untranslated_above = L"Выше есть новый непереведенный текст. Поместите курсор в конец этой строки и нажмите Enter, чтобы перевести.",
    .repair_in_progress = L"Восстановление компонентов локального движка…",
    .repair_failed_title = L"Локальный перевод недоступен",
    .repair_failed_body = L"Файлы локального движка перевода не найдены, поэтому перевод приостановлен. Переустановите Emebala Chat, чтобы восстановить локальный движок, или переключитесь на облачный (Google) перевод, выбрав «Google Переводчик» в меню области уведомлений, «Движок перевода».",
    .menu_engine_openai = L"Совместимо с OpenAI (пользовательский сервер)…",
    .openai_settings_title = L"Настройки совместимого с OpenAI движка",
    .openai_settings_action = L"Настройки совместимого с OpenAI движка…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"API-ключ",
    .openai_model_label = L"Модель",
    .openai_fetch_models = L"Получить список моделей",
    .openai_fetch_failed = L"Не удалось получить список моделей. Вы можете ввести имя модели вручную.",
    .openai_http_warning_title = L"Незащищённое соединение (HTTP)",
    .openai_http_warning_body = L"Base URL использует HTTP (без шифрования). Ваш API-ключ и текст будут отправлены в открытом виде. Продолжить?",
    .openai_saved = L"Совместимые с OpenAI настройки сохранены.",
    .openai_key_masked = L"Сохранённый ключ: ",
    .openai_invalid_base_url = L"Недопустимый Base URL. Пример: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Модель, указанная пользователем (.gguf)",
    .menu_browse_gguf_file = L"Зарегистрировать другую модель .gguf…",
    .user_gguf_quality_title = L"Уведомление о качестве перевода",
    .user_gguf_quality_body = L"Выбранная модель не Hy-MT2. Текущая версия использует промпт только для Hy-MT2, поэтому качество перевода этой моделью не гарантируется. Продолжить?",
    .user_gguf_registered_title = L"Модель зарегистрирована",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Выбранная модель зарегистрирована в локальном движке.\n"
    L"\n"
    L"Эта модель используется для перевода, когда вы выбираете «Выбор движка перевода > Выбор пользователя (.gguf)».\n"
    L"\n"
    L"Когда вступает в силу: не более примерно 1 минуты после регистрации (после выхода движка из простоя)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Эта модель уже встроена в Emebala Chat. Регистрация не требуется. Выберите встроенный локальный движок перевода напрямую.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(не зарегистрировано)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Управление моделями…",
    .gguf_manager_title = L"Управление моделями пользователя",
    .gguf_manager_empty = L"Нет зарегистрированных пользовательских моделей.",
    .gguf_manager_rename = L"Переименовать…",
    .gguf_manager_delete = L"Удалить…",
    .gguf_manager_close = L"Закрыть",
    .gguf_manager_delete_confirm_title = L"Удаление регистрации модели",
    .gguf_manager_delete_confirm_body = L"Регистрация выбранной модели будет удалена.\n"
    L"\n"
    L"Файл модели (.gguf) остаётся на диске и не удаляется. Если эта модель использовалась, выбор движка перевода вернётся к Авто (Auto).\n"
    L"\n"
    L"Продолжить?",
    .gguf_manager_rename_title = L"Переименование модели",
    .gguf_manager_rename_body = L"Введите новое имя (до 64 символов, без пробелов).",
    .gguf_manager_rename_invalid = L"Такое имя использовать нельзя. Введите непустое имя, отличающееся от существующих, без пробелов и разделителей пути, длиной до 64 символов.",
    .gguf_manager_done = L"Изменения сохранены.",
    .gguf_manager_err_serialize = L"Не удалось сериализовать registry.json (имя файла отклонено). Ничего не изменено.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% недоступен; не удаётся найти общий каталог моделей.",
    .gguf_manager_err_write = L"Не удалось записать registry.json.",
    .gguf_manager_err_write_partial = L"Не удалось полностью записать registry.json.",
    .gguf_manager_err_registry_damaged = L"registry.json повреждён или имеет неподдерживаемую схему. Он НЕ был изменён. Исправьте или удалите его и повторите попытку.",
    .tooltip_translate_failed = L"Перевод не удался. Попробуйте ещё раз.",
    .repair_transient_body = L"Локальный движок перевода временно недоступен (возможно, запускается). Подождите немного и повторите попытку.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Подтвердить",
    .dialog_cancel = L"Отмена",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Добавить из файла…",
    .gguf_manager_add_hf = L"Добавить из Hugging Face…",
    .hf_add_title = L"Добавить модель Hugging Face",
    .hf_url_label = L"URL модели",
    .hf_downloading = L"Загрузка…",
    .hf_failed = L"Сбой загрузки.",
    .hf_invalid_url = L"Введите URL модели huggingface.co (resolve, blob или страница модели)",
    .hf_done = L"Зарегистрировано.",
    .openai_base_url_hint = L"напр.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Ключ API",
    // REQ-051: кнопка [Удалить настройки].
    .openai_delete_settings = L"Удалить настройки",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Проверить обновления",
    .update_idle_note = L"Проверьте, используете ли вы последнюю версию Emebala Chat.",
    .update_checking = L"Проверка обновлений…",
    .update_checking_hint = L"Выполняется в фоне; это окно можно закрыть.",
    .update_uptodate = L"У вас актуальная версия",
    .update_dlg_title = L"Доступно обновление",
    .update_dlg_version = L"Новая версия",
    .update_dlg_version_cur = L"у вас {v}",
    .update_dlg_size = L"Размер",
    .update_dlg_size_approx = L"около {s}",
    .update_dlg_privacy = L"Проверка и скачивание выполняются через GitHub.",
    .update_dlg_download = L"Скачать",
    .update_dlg_later = L"Позже",
    .update_dl_progress = L"Загрузка…",
    .update_dl_cancel = L"Отмена",
    .update_dl_cancelled = L"Загрузка отменена.",
    .update_ready_title = L"Загрузка завершена — можно устанавливать",
    .update_ready_warn = L"Приложение закроется для установки.",
    .update_ready_install = L"Установить сейчас",
    .update_ready_later = L"Позже",
    .update_err_offline = L"Нет подключения. Проверьте соединение и повторите попытку.",
    .update_err_rate = L"Слишком много запросов. Повторите попытку через некоторое время.",
    .update_err_hash = L"Загруженный файл не прошёл проверку.",
    .update_err_retry = L"Повторить",
    .update_snoozed_line = L"Обновление можно установить позже.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Загрузка модели",
    .model_dl_consent_body = L"Встроенная модель перевода не установлена. Скачать её сейчас? (около {s})",
    .model_dl_privacy = L"Загрузка выполняется через huggingface.co.",
    .model_dl_download = L"Скачать сейчас",
    .model_dl_verifying = L"Проверка загруженного файла…",
    .model_dl_placing = L"Установка модели…",
    .model_dl_done = L"Модель готова. Локальный движок будет использовать её со следующего перевода.",
    .model_dl_failed = L"Загрузка не удалась. Проверьте подключение и повторите попытку.",
    .model_dl_disk_space = L"Недостаточно свободного места на диске (требуется около {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Настройки загрузки модели перевода…",
    .engine_load_settings_title = L"Загрузка модели перевода",
    .load_mode_on_use = L"Загружать при использовании (рекомендуется)",
    .load_mode_warmup = L"Загружать заранее",
    .load_mode_on_use_desc = L"Экономит память. Первый перевод может начаться на несколько секунд позже.",
    .load_mode_warmup_desc = L"Перевод начинается сразу. Модель постоянно находится в памяти.",
    .gguf_policy_sticky = L"(Оставлять в памяти)",
    .gguf_policy_evict = L"(Загружать при использовании)",
    .gguf_manager_toggle_policy = L"Сменить политику",
    .gguf_duplicate_warning = L"Дубликаты файлов: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Удалить дубликаты",
    .gguf_manager_dedupe_done = L"Дубликаты файлов перемещены в корзину.",
    .gguf_manager_dedupe_failed = L"Не удалось удалить дубликаты файлов. Проверьте, не использует ли их другая программа.",
    .menu_ui_theme = L"Тема интерфейса",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Тёплый Editorial",
    .theme_monolith = L"Утончённый Монолит",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Плавающий обратный перевод (проверка)",
    .back_translate_header = L"🔄 Проверка обратного перевода",
    .back_translate_match_exact = L"✓ Смысл совпадает",
    .back_translate_match_check = L"⚡ Проверить оттенок",
    .back_translate_reference = L"↳ Оригинал: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Это не действительный файл модели GGUF; он не зарегистрирован.",
    .gguf_manager_active_suffix = L"(используется)",
    .gguf_manager_activated = L"Эта модель теперь используется.",
};
// 11. Portuguese (pt)
const LocalizedStrings kStringsPortuguese = {
    .menu_status_active = L"Estado: ativo (F9: pausar)",
    .menu_status_paused = L"Estado: pausado (F9: retomar)",
    .menu_engine = L"Mecanismo de tradução",
    .menu_engine_google = L"Google Tradutor (grátis / sem instalação)",
    .menu_engine_local = L"Motor local integrado (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Idioma de origem (entrada)",
    .menu_target_lang = L"Idioma de destino (saída)",
    .menu_swap_langs = L"Trocar origem ⇄ destino (clique duplo)",
    .menu_auto_send = L"Envio automático com Enter",
    .menu_sound_feedback = L"Feedback sonoro (tons)",
    .menu_show_badge = L"Mostrar selo flutuante",
    .menu_start_with_windows = L"Iniciar com o Windows",
    .menu_cheatsheet = L"Resumo de atalhos e ajuda...",
    .menu_exit = L"Sair do Emebala Chat",
    .menu_about = L"Sobre o Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — Atalhos e guia de uso",
    .cheatsheet_body = L"Atalhos e guia de uso do Emebala Chat:\n\n"
    L"  • F9 : ativar / pausar\n"
    L"  • Ctrl + F9 : alternar idioma de destino\n"
    L"  • Ctrl + Shift + Enter : alternar envio automático\n"
    L"  • Shift + Enter : traduzir e enviar na hora\n\n"
    L"Mouse sobre o selo flutuante:\n"
    L"  • Clique esquerdo : ativar / pausar\n"
    L"  • Clique duplo : trocar origem ⇄ destino\n"
    L"  • Clique direito : abrir o menu de configurações\n\n"
    L"Modos de tradução:\n"
    L"  • Substituir apenas (envio automático desativado): substitui a linha pela tradução para revisão.\n"
    L"  • Envio automático ativado: substitui a linha e pressiona Enter imediatamente.",
    .about_title = L"Sobre o Emebala Chat",
    .badge_active = L"Ativo",
    .badge_translating = L"Traduzindo...",
    .badge_paused = L"Pausado",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Não foi possível copiar o texto selecionado. Verifique o aplicativo de destino e tente novamente.",
    .tooltip_no_selection = L"Nenhum texto selecionado para traduzir.",
    .auto_detect = L"Detecção automática",
    .app_already_running = L"O Emebala Chat já está em execução em segundo plano.\nVerifique a bandeja de notificações.",
    .app_com_failed = L"Falha na inicialização do COM.\nO selo flutuante e a leitura de voz ficarão indisponíveis,\nmas tradução, atalhos, bandeja e sons continuam funcionando.",
    .about_tagline = L"Chega de copiar e colar. Digite naturalmente no seu idioma nativo — a tradução substitui sua digitação em tempo real em qualquer aplicativo do Windows.",
    .about_feature0 = L"⚡ Arrastar para traduzir — selecione texto em qualquer app e o ícone flutuante traduz na hora.",
    .about_feature1 = L"🔊 TTS neural — fala todos os 37 idiomas via pacotes de voz do Windows instalados.",
    .about_feature2 = L"🔒 100% no dispositivo e privado — ativo apenas enquanto o atalho é pressionado; área de transferência intacta.",
    .about_etymology = L"Em 2000 a.C., escribas mesopotâmicos chamavam de «Eme-bala» aqueles que transformam a linguagem em ponte entre mundos.",
    .about_link_website = L"Site",
    .about_link_contact = L"Contato",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Horário comercial 10:00–19:00 KST",
    .about_contact_lead = L"Arquiteto principal: Yongtai Kim",
    .tooltip_copied = L"✓ Copiado!",
    .tooltip_button_copy = L"📋 Copiar",
    .tooltip_button_tts = L"🔊 Voz",
    .menu_ui_language = L"Idioma da interface",
    .menu_ui_language_auto = L"Automático (idioma do sistema)",
    .about_reset_button = L"Restaurar padrões do sistema",
    .about_reset_done = L"Padrões restaurados",
    .menu_typing_group = L"Digitação no teclado",
    .menu_tooltip_group = L"Dica de ferramenta",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Não há voz do Windows instalada para este idioma. Clique novamente em 🔊 para abrir as configurações de Fala.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Aviso de privacidade",
    .privacy_notice_body = L"Estes são os princípios de privacidade do Emebala Chat.\n"
    L"\n"
    L"• O Emebala Chat não opera servidores próprios.\n"
    L"• Com o modelo local, o texto traduzido nunca sai do seu dispositivo.\n"
    L"• Se escolher o Google Tradutor ou houver mudança automática para a nuvem, o texto selecionado ou digitado é enviado diretamente ao Google para tradução, sem passar pelo Emebala.\n"
    L"• Os registros de diagnóstico estão DESATIVADOS por padrão; ative-os nas configurações (opt-in).\n"
    L"• Se não quiser enviar texto para a nuvem (Google), abra o menu do ícone da bandeja, escolha “Mecanismo de tradução” e selecione “Motor local integrado”. Sem modelo local instalado e com o recurso de nuvem desativado, a tradução não é executada — nada é enviado.\n"
    L"\n"
    L"O detalhamento completo está no arquivo README, para reler quando quiser.\n",
    .cheatsheet_config_path = L"Arquivo de configuração: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Texto longo demais: apenas o início e o fim foram traduzidos.",
    .tooltip_untranslated_above = L"Há texto novo não traduzido acima. Coloque o cursor no final dessa linha e prima Enter para traduzir.",
    .repair_in_progress = L"A reparar os componentes do motor local…",
    .repair_failed_title = L"Tradução local indisponível",
    .repair_failed_body = L"Os ficheiros do motor de tradução local não foram encontrados, pelo que a tradução foi suspensa. Reinstale o Emebala Chat para restaurar o motor local, ou mude para a tradução na nuvem (Google) escolhendo “Google Tradutor” no menu da bandeja, em “Motor de tradução”.",
    .menu_engine_openai = L"Compatível com OpenAI (servidor personalizado)…",
    .openai_settings_title = L"Definições do motor compatível com OpenAI",
    .openai_settings_action = L"Definições do motor compatível com OpenAI…",
    .openai_base_url_label = L"URL base",
    .openai_api_key_label = L"Chave de API",
    .openai_model_label = L"Modelo",
    .openai_fetch_models = L"Obter lista de modelos",
    .openai_fetch_failed = L"Não foi possível obter a lista de modelos. Pode introduzir o nome do modelo diretamente.",
    .openai_http_warning_title = L"Ligação insegura (HTTP)",
    .openai_http_warning_body = L"O URL base utiliza HTTP (não encriptado). A sua chave de API e o texto serão enviados em texto simples. Continuar?",
    .openai_saved = L"Definições compatíveis com OpenAI guardadas.",
    .openai_key_masked = L"Chave guardada: ",
    .openai_invalid_base_url = L"O URL base não é válido. Exemplo: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Modelo especificado pelo usuário (.gguf)",
    .menu_browse_gguf_file = L"Registrar outro modelo .gguf…",
    .user_gguf_quality_title = L"Aviso sobre a qualidade da tradução",
    .user_gguf_quality_body = L"O modelo selecionado não é Hy-MT2. A versão atual utiliza o prompt exclusivo do Hy-MT2, pelo que a qualidade da tradução com este modelo não é garantida. Continuar?",
    .user_gguf_registered_title = L"Modelo registado",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"O modelo selecionado foi registado no motor local.\n"
    L"\n"
    L"Este modelo é utilizado para tradução quando escolhe \"Seleção do motor de tradução > Escolha do utilizador (.gguf)\".\n"
    L"\n"
    L"Quando surte efeito: até cerca de 1 minuto após o registo (após a saída do motor por inatividade)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Este modelo já está integrado no Emebala Chat. Não é necessário registrá-lo. Selecione diretamente o mecanismo de tradução local integrado.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(não registrado)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Gerir modelos…",
    .gguf_manager_title = L"Gestor de modelos do utilizador",
    .gguf_manager_empty = L"Sem modelos de utilizador registados.",
    .gguf_manager_rename = L"Mudar o nome…",
    .gguf_manager_delete = L"Eliminar…",
    .gguf_manager_close = L"Fechar",
    .gguf_manager_delete_confirm_title = L"Eliminar registo do modelo",
    .gguf_manager_delete_confirm_body = L"O registo do modelo selecionado será eliminado.\n"
    L"\n"
    L"O ficheiro do modelo (.gguf) é mantido no disco e não é eliminado. Se este modelo estava em utilização, a seleção do motor de tradução voltará a Automático (Auto).\n"
    L"\n"
    L"Continuar?",
    .gguf_manager_rename_title = L"Mudar o nome do modelo",
    .gguf_manager_rename_body = L"Introduza um novo nome (máx. 64 caracteres, sem espaços).",
    .gguf_manager_rename_invalid = L"Esse nome não pode ser utilizado. Introduza um nome não vazio, diferente dos existentes, sem espaços nem separadores de caminho, até 64 caracteres.",
    .gguf_manager_done = L"Alterações guardadas.",
    .gguf_manager_err_serialize = L"Não foi possível serializar registry.json (um nome de ficheiro foi rejeitado). Nada foi alterado.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% está indisponível; não foi possível localizar a pasta de modelos partilhada.",
    .gguf_manager_err_write = L"Não foi possível escrever registry.json.",
    .gguf_manager_err_write_partial = L"registry.json não foi completamente escrito.",
    .gguf_manager_err_registry_damaged = L"registry.json está danificado ou tem um esquema não suportado. NÃO foi modificado. Corrija-o ou remova-o e tente novamente.",
    .tooltip_translate_failed = L"Falha na tradução. Tente novamente.",
    .repair_transient_body = L"O motor de tradução local está temporariamente indisponível (pode estar a iniciar). Aguarde um momento e tente novamente.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Confirmar",
    .dialog_cancel = L"Cancelar",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Adicionar de arquivo…",
    .gguf_manager_add_hf = L"Adicionar do Hugging Face…",
    .hf_add_title = L"Adicionar modelo do Hugging Face",
    .hf_url_label = L"URL do modelo",
    .hf_downloading = L"Baixando…",
    .hf_failed = L"Falha no download.",
    .hf_invalid_url = L"Insira o URL do modelo no huggingface.co (resolve, blob ou página do modelo)",
    .hf_done = L"Registrado.",
    .openai_base_url_hint = L"ex.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Chave de API",
    // REQ-051: botão [Eliminar configurações].
    .openai_delete_settings = L"Eliminar configurações",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Verificar atualizações",
    .update_idle_note = L"Veja se está a usar a versão mais recente do Emebala Chat.",
    .update_checking = L"A verificar atualizações…",
    .update_checking_hint = L"Executa em segundo plano; pode fechar esta janela.",
    .update_uptodate = L"Está atualizado",
    .update_dlg_title = L"Atualização disponível",
    .update_dlg_version = L"Nova versão",
    .update_dlg_version_cur = L"tem a {v}",
    .update_dlg_size = L"Tamanho",
    .update_dlg_size_approx = L"cerca de {s}",
    .update_dlg_privacy = L"A verificação e o download ligam-se ao GitHub.",
    .update_dlg_download = L"Transferir",
    .update_dlg_later = L"Mais tarde",
    .update_dl_progress = L"A transferir…",
    .update_dl_cancel = L"Cancelar",
    .update_dl_cancelled = L"Transferência cancelada.",
    .update_ready_title = L"Transferência concluída — pronto para instalar",
    .update_ready_warn = L"A aplicação vai fechar para instalar.",
    .update_ready_install = L"Instalar agora",
    .update_ready_later = L"Mais tarde",
    .update_err_offline = L"Está offline. Verifique a ligação e tente novamente.",
    .update_err_rate = L"Demasiados pedidos. Tente novamente dentro de momentos.",
    .update_err_hash = L"O ficheiro transferido não passou na verificação.",
    .update_err_retry = L"Tentar novamente",
    .update_snoozed_line = L"Pode instalar a atualização mais tarde.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Transferir modelo",
    .model_dl_consent_body = L"O modelo de tradução integrado não está instalado. Transferir agora? (cerca de {s})",
    .model_dl_privacy = L"A transferência liga-se a huggingface.co.",
    .model_dl_download = L"Transferir agora",
    .model_dl_verifying = L"A verificar a transferência…",
    .model_dl_placing = L"A instalar o modelo…",
    .model_dl_done = L"O modelo está pronto. O motor local irá utilizá-lo a partir da próxima tradução.",
    .model_dl_failed = L"A transferência falhou. Verifique a ligação e tente novamente.",
    .model_dl_disk_space = L"Espaço livre em disco insuficiente (necessários cerca de {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Definições de carregamento do modelo de tradução…",
    .engine_load_settings_title = L"Carregamento do modelo de tradução",
    .load_mode_on_use = L"Carregar ao utilizar (recomendado)",
    .load_mode_warmup = L"Pré-carregar",
    .load_mode_on_use_desc = L"Poupa memória. A primeira tradução pode demorar alguns segundos extra.",
    .load_mode_warmup_desc = L"A tradução começa imediatamente. O modelo permanece em memória.",
    .gguf_policy_sticky = L"(Manter residente)",
    .gguf_policy_evict = L"(Carregar ao utilizar)",
    .gguf_manager_toggle_policy = L"Alterar política",
    .gguf_duplicate_warning = L"Ficheiros duplicados: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Limpar duplicados",
    .gguf_manager_dedupe_done = L"Os ficheiros duplicados foram movidos para a Reciclagem.",
    .gguf_manager_dedupe_failed = L"Não foi possível limpar os ficheiros duplicados. Verifique se outro programa os está a utilizar.",
    .menu_ui_theme = L"Tema da interface",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editorial aconchegante",
    .theme_monolith = L"Monólito refinado",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Mostrar retrotradução flutuante (verificação)",
    .back_translate_header = L"🔄 Verificação da retrotradução",
    .back_translate_match_exact = L"✓ Significado correspondente",
    .back_translate_match_check = L"⚡ Verificar nuance",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Não é um ficheiro de modelo GGUF válido; não foi registado.",
    .gguf_manager_active_suffix = L"(em utilização)",
    .gguf_manager_activated = L"Este modelo está agora em utilização.",
};
// 12. Italian (it)
const LocalizedStrings kStringsItalian = {
    .menu_status_active = L"Stato: attivo (F9: pausa)",
    .menu_status_paused = L"Stato: in pausa (F9: riprendi)",
    .menu_engine = L"Motore di traduzione",
    .menu_engine_google = L"Google Traduttore (gratis / senza installazione)",
    .menu_engine_local = L"Motore locale integrato (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Lingua di origine (input)",
    .menu_target_lang = L"Lingua di destinazione (output)",
    .menu_swap_langs = L"Scambia origine ⇄ destinazione (doppio clic)",
    .menu_auto_send = L"Invio automatico con Enter",
    .menu_sound_feedback = L"Feedback sonoro (toni)",
    .menu_show_badge = L"Mostra badge fluttuante",
    .menu_start_with_windows = L"Avvia con Windows",
    .menu_cheatsheet = L"Promemoria scorciatoie e guida...",
    .menu_exit = L"Esci da Emebala Chat",
    .menu_about = L"Informazioni su Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — scorciatoie e guida",
    .cheatsheet_body = L"Scorciatoie e guida all'uso di Emebala Chat:\n\n"
    L"  • F9 : attiva / metti in pausa\n"
    L"  • Ctrl + F9 : cambia lingua di destinazione\n"
    L"  • Ctrl + Maiusc + Invio : attiva/disattiva invio automatico\n"
    L"  • Maiusc + Invio : traduci e invia subito\n\n"
    L"Mouse sul badge fluttuante:\n"
    L"  • Clic sinistro : attiva / pausa\n"
    L"  • Doppio clic : scambia origine ⇄ destinazione\n"
    L"  • Clic destro : apri il menu delle impostazioni\n\n"
    L"Modalità di traduzione:\n"
    L"  • Solo sostituzione (invio automatico OFF): sostituisce la riga con la traduzione per la revisione.\n"
    L"  • Invio automatico (ON): sostituisce la riga e preme subito Invio.",
    .about_title = L"Informazioni su Emebala Chat",
    .badge_active = L"Attivo",
    .badge_translating = L"Traduzione...",
    .badge_paused = L"In pausa",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Impossibile copiare il testo selezionato. Controlla l'app di destinazione e riprova.",
    .tooltip_no_selection = L"Nessun testo selezionato da tradurre.",
    .auto_detect = L"Rilevamento automatico",
    .app_already_running = L"Emebala Chat è già in esecuzione in background.\nControlla l'area di notifica.",
    .app_com_failed = L"Inizializzazione COM non riuscita.\nIl badge fluttuante e la sintesi vocale non saranno disponibili,\nma traduzione, scorciatoie, tray e suoni continueranno a funzionare.",
    .about_tagline = L"Basta copiare e incollare. Scrivi naturalmente nella tua lingua madre: la traduzione sostituisce la tua digitazione in tempo reale in qualsiasi app Windows.",
    .about_feature0 = L"⚡ Trascina e traduci — seleziona il testo in qualsiasi app, l'icona fluttuante traduce all'istante.",
    .about_feature1 = L"🔊 TTS neurale — pronuncia tutte le 37 lingue tramite i pacchetti vocali di Windows installati.",
    .about_feature2 = L"🔒 100% sul dispositivo e privato — attivo solo mentre la scorciatoia è premuta; appunti intatti.",
    .about_etymology = L"Nel 2000 a.C., gli scribi mesopotamici chiamavano «Eme-bala» coloro che fanno del linguaggio un ponte tra mondi.",
    .about_link_website = L"Sito web",
    .about_link_contact = L"Contatti",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Orari d'ufficio 10:00–19:00 KST",
    .about_contact_lead = L"Architetto principale: Yongtai Kim",
    .tooltip_copied = L"✓ Copiato!",
    .tooltip_button_copy = L"📋 Copia",
    .tooltip_button_tts = L"🔊 Voce",
    .menu_ui_language = L"Lingua dell'interfaccia",
    .menu_ui_language_auto = L"Auto (lingua di sistema)",
    .about_reset_button = L"Ripristina valori predefiniti di sistema",
    .about_reset_done = L"Valori ripristinati",
    .menu_typing_group = L"Digitazione da tastiera",
    .menu_tooltip_group = L"Tooltip di trascinamento",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Nessuna voce Windows installata per questa lingua. Fai di nuovo clic su 🔊 per aprire le impostazioni di Riconoscimento vocale.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Informativa sulla privacy",
    .privacy_notice_body = L"Ecco i principi sulla privacy di Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat non gestisce alcun server proprio.\n"
    L"• Con il modello locale, il testo tradotto non lascia il dispositivo.\n"
    L"• Se scegli Google Traduttore o il passaggio al cloud è automatico, il testo selezionato o digitato viene inviato direttamente a Google per la traduzione, senza passare da Emebala.\n"
    L"• I log di diagnostica sono DISATTIVATI per impostazione predefinita; attivali nelle impostazioni (opt-in).\n"
    L"• Se non vuoi inviare testo al cloud (Google), apri il menu dell'icona nella barra delle applicazioni, scegli “Motore di traduzione” e seleziona “Motore locale integrato”. Senza modello locale installato e con il fallback cloud disattivato, la traduzione non viene eseguita e non viene inviato nulla.\n"
    L"\n"
    L"Il dettaglio completo è nel file README, rileggibile in qualsiasi momento.\n",
    .cheatsheet_config_path = L"File di configurazione: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Testo troppo lungo: sono stati tradotti solo l'inizio e la fine.",
    .tooltip_untranslated_above = L"C'è nuovo testo non tradotto sopra. Posiziona il cursore alla fine di quella riga e premi Invio per tradurlo.",
    .repair_in_progress = L"Riparazione dei componenti del motore locale…",
    .repair_failed_title = L"Traduzione locale non disponibile",
    .repair_failed_body = L"I file del motore di traduzione locale non sono stati trovati, quindi la traduzione è sospesa. Reinstalla Emebala Chat per ripristinare il motore locale, oppure passa alla traduzione cloud (Google) scegliendo “Google Traduttore” dal menu dell’area di notifica, “Motore di traduzione”.",
    .menu_engine_openai = L"Compatibile con OpenAI (server personalizzato)…",
    .openai_settings_title = L"Impostazioni motore compatibile OpenAI",
    .openai_settings_action = L"Impostazioni motore compatibile OpenAI…",
    .openai_base_url_label = L"URL base",
    .openai_api_key_label = L"Chiave API",
    .openai_model_label = L"Modello",
    .openai_fetch_models = L"Recupera elenco modelli",
    .openai_fetch_failed = L"Impossibile recuperare l'elenco dei modelli. Puoi digitare direttamente il nome del modello.",
    .openai_http_warning_title = L"Connessione non sicura (HTTP)",
    .openai_http_warning_body = L"L'URL base usa HTTP (non crittografato). La chiave API e il testo verranno inviati in chiaro. Continuare?",
    .openai_saved = L"Impostazioni compatibili OpenAI salvate.",
    .openai_key_masked = L"Chiave salvata: ",
    .openai_invalid_base_url = L"L'URL base non è valido. Esempio: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Modello specificato dall'utente (.gguf)",
    .menu_browse_gguf_file = L"Registra un altro modello .gguf…",
    .user_gguf_quality_title = L"Avviso sulla qualità della traduzione",
    .user_gguf_quality_body = L"Il modello selezionato non è Hy-MT2. La versione attuale usa il prompt esclusivo di Hy-MT2, quindi la qualità della traduzione con questo modello non è garantita. Continuare?",
    .user_gguf_registered_title = L"Modello registrato",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Il modello selezionato è stato registrato nel motore locale.\n"
    L"\n"
    L"Questo modello viene usato per la traduzione quando scegli \"Selezione motore di traduzione > Scelta utente (.gguf)\".\n"
    L"\n"
    L"Quando ha effetto: al massimo circa 1 minuto dopo la registrazione (dopo l'uscita del motore per inattività)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Questo modello è già integrato in Emebala Chat. Non è necessario registrarlo. Puoi selezionare direttamente il motore di traduzione locale integrato.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(non registrato)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Gestisci modelli…",
    .gguf_manager_title = L"Gestione modelli utente",
    .gguf_manager_empty = L"Nessun modello utente registrato.",
    .gguf_manager_rename = L"Rinomina…",
    .gguf_manager_delete = L"Elimina…",
    .gguf_manager_close = L"Chiudi",
    .gguf_manager_delete_confirm_title = L"Elimina registrazione modello",
    .gguf_manager_delete_confirm_body = L"La registrazione del modello selezionato verrà eliminata.\n"
    L"\n"
    L"Il file del modello (.gguf) viene mantenuto sul disco e non viene eliminato. Se questo modello era in uso, la selezione del motore di traduzione tornerà a Automatico (Auto).\n"
    L"\n"
    L"Continuare?",
    .gguf_manager_rename_title = L"Rinomina modello",
    .gguf_manager_rename_body = L"Inserisci un nuovo nome (max 64 caratteri, senza spazi).",
    .gguf_manager_rename_invalid = L"Questo nome non può essere usato. Inserisci un nome non vuoto, diverso da quelli esistenti, senza spazi né separatori di percorso, entro 64 caratteri.",
    .gguf_manager_done = L"Modifiche salvate.",
    .gguf_manager_err_serialize = L"Impossibile serializzare registry.json (un nome file è stato rifiutato). Non è stato modificato nulla.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% non disponibile; impossibile trovare la cartella dei modelli condivisa.",
    .gguf_manager_err_write = L"Impossibile scrivere registry.json.",
    .gguf_manager_err_write_partial = L"registry.json non è stato scritto completamente.",
    .gguf_manager_err_registry_damaged = L"registry.json è danneggiato o ha uno schema non supportato. NON è stato modificato. Riparalo o rimuovilo e riprova.",
    .tooltip_translate_failed = L"Traduzione non riuscita. Riprova.",
    .repair_transient_body = L"Il motore di traduzione locale è temporaneamente non disponibile (potrebbe essere in avvio). Attendi un momento e riprova.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Conferma",
    .dialog_cancel = L"Annulla",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Aggiungi da file…",
    .gguf_manager_add_hf = L"Aggiungi da Hugging Face…",
    .hf_add_title = L"Aggiungi modello da Hugging Face",
    .hf_url_label = L"URL del modello",
    .hf_downloading = L"Download in corso…",
    .hf_failed = L"Download non riuscito.",
    .hf_invalid_url = L"Inserisci l'URL del modello huggingface.co (resolve, blob o pagina del modello)",
    .hf_done = L"Registrato.",
    .openai_base_url_hint = L"es.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Chiave API",
    // REQ-051: pulsante [Elimina impostazioni].
    .openai_delete_settings = L"Elimina impostazioni",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Verifica aggiornamenti",
    .update_idle_note = L"Verifica se stai usando l'ultima versione di Emebala Chat.",
    .update_checking = L"Ricerca aggiornamenti…",
    .update_checking_hint = L"Viene eseguito in background; puoi chiudere questa finestra.",
    .update_uptodate = L"Sei aggiornato",
    .update_dlg_title = L"Aggiornamento disponibile",
    .update_dlg_version = L"Nuova versione",
    .update_dlg_version_cur = L"hai la {v}",
    .update_dlg_size = L"Dimensione",
    .update_dlg_size_approx = L"circa {s}",
    .update_dlg_privacy = L"Il controllo e il download si connettono a GitHub.",
    .update_dlg_download = L"Scarica",
    .update_dlg_later = L"Più tardi",
    .update_dl_progress = L"Download in corso…",
    .update_dl_cancel = L"Annulla",
    .update_dl_cancelled = L"Download annullato.",
    .update_ready_title = L"Download completato — pronto per l'installazione",
    .update_ready_warn = L"L'app si chiuderà per l'installazione.",
    .update_ready_install = L"Installa ora",
    .update_ready_later = L"Più tardi",
    .update_err_offline = L"Sei offline. Controlla la connessione e riprova.",
    .update_err_rate = L"Troppe richieste. Riprova tra poco.",
    .update_err_hash = L"Il file scaricato non ha superato la verifica.",
    .update_err_retry = L"Riprova",
    .update_snoozed_line = L"Puoi installare l'aggiornamento più tardi.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Download del modello",
    .model_dl_consent_body = L"Il modello di traduzione integrato non è installato. Scaricarlo ora? (circa {s})",
    .model_dl_privacy = L"Il download si connette a huggingface.co.",
    .model_dl_download = L"Scarica ora",
    .model_dl_verifying = L"Verifica del download…",
    .model_dl_placing = L"Installazione del modello…",
    .model_dl_done = L"Il modello è pronto. Il motore locale lo userà dalla prossima traduzione.",
    .model_dl_failed = L"Il download non è riuscito. Controlla la connessione e riprova.",
    .model_dl_disk_space = L"Spazio libero su disco insufficiente (circa {s} richiesti).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Impostazioni di caricamento del modello di traduzione…",
    .engine_load_settings_title = L"Caricamento del modello di traduzione",
    .load_mode_on_use = L"Carica all'uso (consigliato)",
    .load_mode_warmup = L"Precarica",
    .load_mode_on_use_desc = L"Risparmia memoria. La prima traduzione potrebbe ritardare di qualche secondo.",
    .load_mode_warmup_desc = L"La traduzione inizia subito. Il modello resta in memoria.",
    .gguf_policy_sticky = L"(Mantieni residente)",
    .gguf_policy_evict = L"(Carica all'uso)",
    .gguf_manager_toggle_policy = L"Cambia criterio",
    .gguf_duplicate_warning = L"File duplicati: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Pulisci duplicati",
    .gguf_manager_dedupe_done = L"I file duplicati sono stati spostati nel Cestino.",
    .gguf_manager_dedupe_failed = L"Impossibile pulire i file duplicati. Controlla se un altro programma li sta usando.",
    .menu_ui_theme = L"Tema interfaccia",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editoriale caldo",
    .theme_monolith = L"Monolito raffinato",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Mostra retrotraduzione flottante (verifica)",
    .back_translate_header = L"🔄 Verifica retrotraduzione",
    .back_translate_match_exact = L"✓ Significato corrispondente",
    .back_translate_match_check = L"⚡ Controlla la sfumatura",
    .back_translate_reference = L"↳ Originale: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Non è un file di modello GGUF valido; non è stato registrato.",
    .gguf_manager_active_suffix = L"(in uso)",
    .gguf_manager_activated = L"Questo modello è ora in uso.",
};
// 13. Dutch (nl)
const LocalizedStrings kStringsDutch = {
    .menu_status_active = L"Status: actief (F9: pauze)",
    .menu_status_paused = L"Status: gepauzeerd (F9: hervatten)",
    .menu_engine = L"Vertaalengine",
    .menu_engine_google = L"Google Vertalen (gratis / zonder installatie)",
    .menu_engine_local = L"Ingebouwde lokale engine (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Brontaal (invoer)",
    .menu_target_lang = L"Doeltaal (uitvoer)",
    .menu_swap_langs = L"Bron ⇄ doel wisselen (dubbelklik)",
    .menu_auto_send = L"Automatisch verzenden met Enter",
    .menu_sound_feedback = L"Geluidsfeedback (tonen)",
    .menu_show_badge = L"Zwevend badge zichtbaar",
    .menu_start_with_windows = L"Starten met Windows",
    .menu_cheatsheet = L"Sneltoetsenoverzicht en hulp...",
    .menu_exit = L"Emebala Chat afsluiten",
    .menu_about = L"Over Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — sneltoetsen en handleiding",
    .cheatsheet_body = L"Sneltoetsen en handleiding voor Emebala Chat:\n\n"
    L"  • F9 : activeren / pauzeren\n"
    L"  • Ctrl + F9 : doeltaal wisselen\n"
    L"  • Ctrl + Shift + Enter : automatisch verzenden omschakelen\n"
    L"  • Shift + Enter : vertalen en direct verzenden\n\n"
    L"Muisbediening op het badge:\n"
    L"  • Linkermuisklik : activeren / pauzeren\n"
    L"  • Dubbelklik : bron ⇄ doel wisselen\n"
    L"  • Rechtermuisklik : instellingenmenu openen\n\n"
    L"Vertaalmodi:\n"
    L"  • Alleen vervangen (automatisch verzenden uit): vervangt de regel door de vertaling ter controle.\n"
    L"  • Automatisch verzenden (aan): vervangt de regel en drukt meteen op Enter.",
    .about_title = L"Over Emebala Chat",
    .badge_active = L"Actief",
    .badge_translating = L"Vertalen...",
    .badge_paused = L"Gepauzeerd",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Kon de geselecteerde tekst niet kopiëren. Controleer de doel-app en probeer het opnieuw.",
    .tooltip_no_selection = L"Er is geen tekst geselecteerd om te vertalen.",
    .auto_detect = L"Automatisch herkennen",
    .app_already_running = L"Emebala Chat wordt al uitgevoerd op de achtergrond.\nControleer het meldingsvak.",
    .app_com_failed = L"COM-initialisatie mislukt.\nHet zwevende badge en voorlezen zijn niet beschikbaar,\nmaar vertalen, sneltoetsen, tray en geluiden werken nog steeds.",
    .about_tagline = L"Nooit meer kopiëren en plakken. Typ natuurlijk in je moedertaal — de vertaling vervangt je getynde tekst realtime in elke Windows-app.",
    .about_feature0 = L"⚡ Sleep-om-te-vertalen — selecteer tekst in elke app, het zwevende pictogram vertaalt meteen.",
    .about_feature1 = L"🔊 Neurale TTS — spreekt alle 37 talen via geïnstalleerde Windows-spraakpakketten.",
    .about_feature2 = L"🔒 100% lokaal en privé — alleen actief zolang de sneltoets wordt ingehouden; klembord onaangeroerd.",
    .about_etymology = L"In 2000 v.Chr. noemden Mesopotamische schrijvers «Eme-bala» — hen die taal omzetten in een brug tussen werelden.",
    .about_link_website = L"Website",
    .about_link_contact = L"Contact",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Kantooruren 10:00–19:00 KST",
    .about_contact_lead = L"Hoofdarchitect: Yongtai Kim",
    .tooltip_copied = L"✓ Gekopieerd!",
    .tooltip_button_copy = L"📋 Kopiëren",
    .tooltip_button_tts = L"🔊 Stem",
    .menu_ui_language = L"Interfacetaal",
    .menu_ui_language_auto = L"Automatisch (systeemtaal)",
    .about_reset_button = L"Reset naar systeemstandaarden",
    .about_reset_done = L"Standaarden hersteld",
    .menu_typing_group = L"Typen op toetsenbord",
    .menu_tooltip_group = L"Sleep-tooltip",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Er is geen Windows-stem geïnstalleerd voor deze taal. Klik nogmaals op 🔊 om de Spraak-instellingen te openen.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Privacymelding",
    .privacy_notice_body = L"Dit zijn de privacyprincipes van Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat beheert geen eigen servers.\n"
    L"• Met het lokale model verlaat de vertaalde tekst uw apparaat niet.\n"
    L"• Als u Google Translate kiest of automatisch naar de cloud schakelt, wordt de geselecteerde of getypte tekst rechtstreeks naar Google verzonden voor vertaling, niet via Emebala.\n"
    L"• Diagnosticelogboeken zijn standaard UIT; schakel ze in via de instellingen (opt-in).\n"
    L"• Als u geen tekst naar de cloud (Google) wilt sturen, opent u het menu van het systeemvakpictogram, kiest u “Vertaalengine” en vervolgens “Ingebouwde lokale engine”. Zonder lokaal model en met cloud-omleiding uitgeschakeld wordt er niet vertaald — er wordt niets verzonden.\n"
    L"\n"
    L"Alle details staan in het README-bestand, dat u altijd opnieuw kunt lezen.\n",
    .cheatsheet_config_path = L"Configuratiebestand: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"De tekst was te lang: alleen het begin en het einde zijn vertaald.",
    .tooltip_untranslated_above = L"Er staat nieuwe onvertaalde tekst hierboven. Plaats de cursor aan het einde van die regel en druk op Enter om te vertalen.",
    .repair_in_progress = L"Lokale engine-onderdelen herstellen…",
    .repair_failed_title = L"Lokale vertaling niet beschikbaar",
    .repair_failed_body = L"De bestanden van de lokale vertaalengine ontbreken, dus de vertaling is onderbroken. Installeer Emebala Chat opnieuw om de lokale engine te herstellen, of schakel over naar cloud(Google)-vertaling door “Google Vertalen” te kiezen in het menubalkmenu bij “Vertaalengine”.",
    .menu_engine_openai = L"OpenAI-compatibel (aangepaste server)…",
    .openai_settings_title = L"OpenAI-compatibele engine-instellingen",
    .openai_settings_action = L"OpenAI-compatibele engine-instellingen…",
    .openai_base_url_label = L"Basis-URL",
    .openai_api_key_label = L"API-sleutel",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Modellenlijst ophalen",
    .openai_fetch_failed = L"De modellenlijst kon niet worden opgehaald. U kunt een modelnaam direct typen.",
    .openai_http_warning_title = L"Onbeveiligde verbinding (HTTP)",
    .openai_http_warning_body = L"De basis-URL gebruikt HTTP (niet versleuteld). Uw API-sleutel en tekst worden als leesbare tekst verzonden. Doorgaan?",
    .openai_saved = L"OpenAI-compatibele instellingen opgeslagen.",
    .openai_key_masked = L"Opgeslagen sleutel: ",
    .openai_invalid_base_url = L"De basis-URL is ongeldig. Voorbeeld: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Door gebruiker gespecificeerd model (.gguf)",
    .menu_browse_gguf_file = L"Ander .gguf-model registreren…",
    .user_gguf_quality_title = L"Mededeling over vertaalkwaliteit",
    .user_gguf_quality_body = L"Het geselecteerde model is geen Hy-MT2. De huidige versie gebruikt de alleen-voor-Hy-MT2 prompt, dus de vertaalkwaliteit met dit model is niet gegarandeerd. Doorgaan?",
    .user_gguf_registered_title = L"Model geregistreerd",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Het geselecteerde model is geregistreerd bij de lokale engine.\n"
    L"\n"
    L"Dit model wordt gebruikt voor vertaling wanneer u \"Vertaalengine selecteren > Keuze van gebruiker (.gguf)\" kiest.\n"
    L"\n"
    L"Wanneer van kracht: tot ongeveer 1 minuut na registratie (na het beëindigen van de engine door inactiviteit)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Dit model is al ingebouwd in Emebala Chat. Registratie is niet nodig. Selecteer de ingebouwde lokale vertaalengine direct.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(niet geregistreerd)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Modellen beheren…",
    .gguf_manager_title = L"Gebruikersmodelbeheer",
    .gguf_manager_empty = L"Geen gebruikersmodellen geregistreerd.",
    .gguf_manager_rename = L"Hernoemen…",
    .gguf_manager_delete = L"Verwijderen…",
    .gguf_manager_close = L"Sluiten",
    .gguf_manager_delete_confirm_title = L"Modelregistratie verwijderen",
    .gguf_manager_delete_confirm_body = L"De registratie van het geselecteerde model wordt verwijderd.\n"
    L"\n"
    L"Het modelbestand (.gguf) blijft op de schijf staan en wordt niet verwijderd. Als dit model in gebruik was, keert de keuze van de vertaalengine terug naar Auto.\n"
    L"\n"
    L"Doorgaan?",
    .gguf_manager_rename_title = L"Model hernoemen",
    .gguf_manager_rename_body = L"Voer een nieuwe naam in (max. 64 tekens, geen spaties).",
    .gguf_manager_rename_invalid = L"Die naam kan niet worden gebruikt. Voer een niet-lege naam in die afwijkt van bestaande namen, zonder spaties of padscheidingstekens, van maximaal 64 tekens.",
    .gguf_manager_done = L"Wijzigingen opgeslagen.",
    .gguf_manager_err_serialize = L"registry.json kon niet worden geserialiseerd (een bestandsnaam is afgewezen). Er is niets gewijzigd.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% is niet beschikbaar; de gedeelde modellenmap kan niet worden gevonden.",
    .gguf_manager_err_write = L"registry.json kon niet worden weggeschreven.",
    .gguf_manager_err_write_partial = L"registry.json kon niet volledig worden weggeschreven.",
    .gguf_manager_err_registry_damaged = L"registry.json is beschadigd of heeft een niet-ondersteund schema. Het is NIET gewijzigd. Herstel of verwijder het en probeer het opnieuw.",
    .tooltip_translate_failed = L"Vertaling mislukt. Probeer het opnieuw.",
    .repair_transient_body = L"De lokale vertaalengine is tijdelijk niet beschikbaar (mogelijk aan het starten). Wacht even en probeer het opnieuw.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Bevestigen",
    .dialog_cancel = L"Annuleren",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Toevoegen vanuit bestand…",
    .gguf_manager_add_hf = L"Toevoegen vanuit Hugging Face…",
    .hf_add_title = L"Hugging Face-model toevoegen",
    .hf_url_label = L"Model-URL",
    .hf_downloading = L"Bezig met downloaden…",
    .hf_failed = L"Downloaden mislukt.",
    .hf_invalid_url = L"Voer de huggingface.co-model-URL in (resolve, blob of modelpagina)",
    .hf_done = L"Geregistreerd.",
    .openai_base_url_hint = L"bijv.: https://api.openai.com/v1",
    .openai_api_key_hint = L"API-sleutel",
    // REQ-051: knop [Instellingen verwijderen].
    .openai_delete_settings = L"Instellingen verwijderen",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Controleren op updates",
    .update_idle_note = L"Kijk of je de nieuwste versie van Emebala Chat gebruikt.",
    .update_checking = L"Updates worden gecontroleerd…",
    .update_checking_hint = L"Draait op de achtergrond; je mag dit venster sluiten.",
    .update_uptodate = L"Je hebt de nieuwste versie",
    .update_dlg_title = L"Update beschikbaar",
    .update_dlg_version = L"Nieuwe versie",
    .update_dlg_version_cur = L"jij hebt {v}",
    .update_dlg_size = L"Grootte",
    .update_dlg_size_approx = L"ongeveer {s}",
    .update_dlg_privacy = L"Controleren en downloaden verbindt met GitHub.",
    .update_dlg_download = L"Downloaden",
    .update_dlg_later = L"Later",
    .update_dl_progress = L"Bezig met downloaden…",
    .update_dl_cancel = L"Annuleren",
    .update_dl_cancelled = L"Download geannuleerd.",
    .update_ready_title = L"Download voltooid — klaar om te installeren",
    .update_ready_warn = L"De app wordt afgesloten om te installeren.",
    .update_ready_install = L"Nu installeren",
    .update_ready_later = L"Later",
    .update_err_offline = L"Je bent offline. Controleer je verbinding en probeer het opnieuw.",
    .update_err_rate = L"Te veel aanvragen. Probeer het zo opnieuw.",
    .update_err_hash = L"Het gedownloade bestand is niet geverifieerd.",
    .update_err_retry = L"Opnieuw proberen",
    .update_snoozed_line = L"Je kunt de update later installeren.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Model downloaden",
    .model_dl_consent_body = L"Het ingebouwde vertaalmodel is niet geïnstalleerd. Nu downloaden? (ongeveer {s})",
    .model_dl_privacy = L"Het downloaden verbindt met huggingface.co.",
    .model_dl_download = L"Nu downloaden",
    .model_dl_verifying = L"Download wordt geverifieerd…",
    .model_dl_placing = L"Model wordt geïnstalleerd…",
    .model_dl_done = L"Het model is klaar. De lokale engine gebruikt het vanaf de volgende vertaling.",
    .model_dl_failed = L"De download is mislukt. Controleer je verbinding en probeer het opnieuw.",
    .model_dl_disk_space = L"Niet genoeg vrije schijfruimte (ongeveer {s} vereist).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Instellingen voor laden van het vertaalmodel…",
    .engine_load_settings_title = L"Vertaalmodel laden",
    .load_mode_on_use = L"Laden bij gebruik (aanbevolen)",
    .load_mode_warmup = L"Vooraf laden",
    .load_mode_on_use_desc = L"Bespaart geheugen. De eerste vertaling kan enkele seconden later starten.",
    .load_mode_warmup_desc = L"De vertaling start meteen. Het model blijft in het geheugen.",
    .gguf_policy_sticky = L"(Resident houden)",
    .gguf_policy_evict = L"(Laden bij gebruik)",
    .gguf_manager_toggle_policy = L"Beleid wijzigen",
    .gguf_duplicate_warning = L"Gedupliceerde bestanden: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Duplicaten opschonen",
    .gguf_manager_dedupe_done = L"De gedupliceerde bestanden zijn naar de Prullenbak verplaatst.",
    .gguf_manager_dedupe_failed = L"De gedupliceerde bestanden konden niet worden opgeschoond. Controleer of een ander programma ze gebruikt.",
    .menu_ui_theme = L"UI-thema",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Warm Editorial",
    .theme_monolith = L"Verfijnde monolith",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Zwevende terugvertaling tonen (controle)",
    .back_translate_header = L"🔄 Terugvertaling controleren",
    .back_translate_match_exact = L"✓ Betekenis komt overeen",
    .back_translate_match_check = L"⚡ Nuance controleren",
    .back_translate_reference = L"↳ Origineel: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Dit is geen geldig GGUF-modelbestand; het is niet geregistreerd.",
    .gguf_manager_active_suffix = L"(in gebruik)",
    .gguf_manager_activated = L"Dit model is nu in gebruik.",
};
// 14. Polish (pl)
const LocalizedStrings kStringsPolish = {
    .menu_status_active = L"Status: aktywny (F9: pauza)",
    .menu_status_paused = L"Status: wstrzymany (F9: wznow)",
    .menu_engine = L"Silnik tłumaczenia",
    .menu_engine_google = L"Tłumacz Google (darmowy / bez instalacji)",
    .menu_engine_local = L"Wbudowany silnik lokalny (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Język źródłowy (wejście)",
    .menu_target_lang = L"Język docelowy (wyjście)",
    .menu_swap_langs = L"Zamień źródło ⇄ cel (dwuklik)",
    .menu_auto_send = L"Automatyczne wysyłanie po Enter",
    .menu_sound_feedback = L"Dźwięki potwierdzenia",
    .menu_show_badge = L"Pływająca odznaka widoczna",
    .menu_start_with_windows = L"Uruchamiaj z Windowsem",
    .menu_cheatsheet = L"Ściąga skrótów i pomoc...",
    .menu_exit = L"Zamknij Emebala Chat",
    .menu_about = L"O Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — skróty i przewodnik",
    .cheatsheet_body = L"Skróty i przewodnik po Emebala Chat:\n\n"
    L"  • F9 : włącz / pauza\n"
    L"  • Ctrl + F9 : zmiana języka docelowego\n"
    L"  • Ctrl + Shift + Enter : przełącz automatyczne wysyłanie\n"
    L"  • Shift + Enter : tłumacz i wyślij natychmiast\n\n"
    L"Sterowanie myszą na odznace:\n"
    L"  • Lewy przycisk : włącz / pauza\n"
    L"  • Dwuklik : zamień źródło ⇄ cel\n"
    L"  • Prawy przycisk : otwórz menu ustawień\n\n"
    L"Tryby tłumaczenia:\n"
    L"  • Tylko zamiana (auto-wysyłanie wył): zastępuje linię tłumaczeniem do sprawdzenia.\n"
    L"  • Auto-wysyłanie (wł): zastępuje linię i od razu naciska Enter.",
    .about_title = L"O Emebala Chat",
    .badge_active = L"Aktywny",
    .badge_translating = L"Tłumaczenie...",
    .badge_paused = L"Wstrzymany",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Nie udało się skopiować zaznaczonego tekstu. Sprawdź aplikację docelową i spróbuj ponownie.",
    .tooltip_no_selection = L"Nie wybrano tekstu do przetłumaczenia.",
    .auto_detect = L"Wykryj automatycznie",
    .app_already_running = L"Emebala Chat jest już uruchomiony w tle.\nSprawdź zasobnik powiadomień.",
    .app_com_failed = L"Inicjalizacja COM nie powiodła się.\nPływająca odznaka i czytanie na głos będą niedostępne,\nale tłumaczenie, skróty, zasobnik i dźwięki nadal działają.",
    .about_tagline = L"Koniec z kopiowaniem i wklejaniem. Pis naturalnie w swoim języku ojczystym — tłumaczenie zastępuje Twoje wpisy w czasie rzeczywistym w każdej aplikacji Windows.",
    .about_feature0 = L"⚡ Przeciągnij, aby przetłumaczyć — zaznacz tekst w dowolnej aplikacji, pływająca ikona tłumaczy natychmiast.",
    .about_feature1 = L"🔊 Neuralne TTS — wymawia wszystkie 37 języków dzięki zainstalowanym pakietom głosowym Windows.",
    .about_feature2 = L"🔒 100% na urządzeniu i prywatne — aktywne tylko podczas trzymania skrótu; schowek nienaruszony.",
    .about_etymology = L"W 2000 r. p.n.e. mezopotamscy skrybowie nazywali «Eme-bala» tych, którzy zamieniają język w most między światami.",
    .about_link_website = L"Strona internetowa",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Godziny pracy 10:00–19:00 KST",
    .about_contact_lead = L"Główny architekt: Yongtai Kim",
    .tooltip_copied = L"✓ Skopiowano!",
    .tooltip_button_copy = L"📋 Kopiuj",
    .tooltip_button_tts = L"🔊 Głos",
    .menu_ui_language = L"Język interfejsu",
    .menu_ui_language_auto = L"Auto (język systemu)",
    .about_reset_button = L"Przywróć ustawienia systemowe",
    .about_reset_done = L"Przywrócono wartości domyślne",
    .menu_typing_group = L"Pisanie na klawiaturze",
    .menu_tooltip_group = L"Etykieta przeciągania",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Dla tego języka nie zainstalowano głosu Windows. Kliknij ponownie 🔊, aby otworzyć ustawienia Mowy.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Informacja o prywatności",
    .privacy_notice_body = L"Oto zasady prywatności w Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat nie prowadzi własnych serwerów.\n"
    L"• Przy modelu lokalnym tłumaczony tekst nie opuszcza urządzenia.\n"
    L"• Jeśli wybierzesz Tłumacz Google lub nastąpi automatyczne przełączenie w chmurę, zaznaczony lub wpisany tekst jest wysyłany bezpośrednio do Google w celu tłumaczenia, bez pośrednictwa Emebala.\n"
    L"• Logi diagnostyczne są domyślnie WYŁĄCZONE; włącz je w konfiguracji (zgoda).\n"
    L"• Jeśli nie chcesz wysyłać tekstu do chmury (Google), otwórz menu ikony w zasobniku systemowym, wybierz “Silnik tłumaczenia” i zaznacz “Wbudowany silnik lokalny”. Bez zainstalowanego modelu lokalnego i przy wyłączonym przejściu do chmury tłumaczenie nie działa — nic nie jest wysyłane.\n"
    L"\n"
    L"Szczegóły znajdziesz w pliku README — możesz go przeczytać w dowolnej chwili.\n",
    .cheatsheet_config_path = L"Plik konfiguracji: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Tekst był zbyt długi: przetłumaczono tylko początek i koniec.",
    .tooltip_untranslated_above = L"Powyżej znajduje się nowy nieprzetłumaczony tekst. Umieść kursor na końcu tego wiersza i naciśnij Enter, aby przetłumaczyć.",
    .repair_in_progress = L"Naprawianie lokalnych składników silnika…",
    .repair_failed_title = L"Tłumaczenie lokalne niedostępne",
    .repair_failed_body = L"Nie znaleziono plików lokalnego silnika tłumaczenia, więc tłumaczenie zostało wstrzymane. Zainstaluj ponownie Emebala Chat, aby przywrócić lokalny silnik, lub przełącz się na tłumaczenie w chmurze (Google), wybierając „Google Translate” w menu zasobnika, „Silnik tłumaczenia”.",
    .menu_engine_openai = L"Zgodne z OpenAI (serwer użytkownika)…",
    .openai_settings_title = L"Ustawienia silnika zgodnego z OpenAI",
    .openai_settings_action = L"Ustawienia silnika zgodnego z OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"Klucz API",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Pobierz listę modeli",
    .openai_fetch_failed = L"Nie udało się pobrać listy modeli. Możesz wpisać nazwę modelu bezpośrednio.",
    .openai_http_warning_title = L"Niezabezpieczone połączenie (HTTP)",
    .openai_http_warning_body = L"Base URL używa HTTP (bez szyfrowania). Twój klucz API i tekst zostaną wysłane jako zwykły tekst. Kontynuować?",
    .openai_saved = L"Zapisano ustawienia zgodne z OpenAI.",
    .openai_key_masked = L"Zapisany klucz: ",
    .openai_invalid_base_url = L"Base URL jest nieprawidłowy. Przykład: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Model określony przez użytkownika (.gguf)",
    .menu_browse_gguf_file = L"Zarejestruj inny model .gguf…",
    .user_gguf_quality_title = L"Informacja o jakości tłumaczenia",
    .user_gguf_quality_body = L"Wybrany model nie jest Hy-MT2. Bieżąca wersja używa promptu wyłącznie dla Hy-MT2, więc jakość tłumaczenia tym modelem nie jest gwarantowana. Kontynuować?",
    .user_gguf_registered_title = L"Model zarejestrowany",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Wybrany model został zarejestrowany w lokalnym silniku.\n"
    L"\n"
    L"Model ten jest używany do tłumaczenia po wybraniu \"Wybór silnika tłumaczenia > Wybór użytkownika (.gguf)\".\n"
    L"\n"
    L"Kiedy zaczyna obowiązywać: do około 1 minuty po rejestracji (po zakończeniu działania silnika z powodu bezczynności)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Ten model jest już wbudowany w Emebala Chat. Rejestracja nie jest potrzebna. Wystarczy bezpośrednio wybrać wbudowany lokalny silnik tłumaczenia.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(niezarejestrowany)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Zarządzaj modelami…",
    .gguf_manager_title = L"Menedżer modeli użytkownika",
    .gguf_manager_empty = L"Brak zarejestrowanych modeli użytkownika.",
    .gguf_manager_rename = L"Zmień nazwę…",
    .gguf_manager_delete = L"Usuń…",
    .gguf_manager_close = L"Zamknij",
    .gguf_manager_delete_confirm_title = L"Usuń rejestrację modelu",
    .gguf_manager_delete_confirm_body = L"Rejestracja wybranego modelu zostanie usunięta.\n"
    L"\n"
    L"Plik modelu (.gguf) pozostaje na dysku i nie zostanie usunięty. Jeśli ten model był używany, wybór silnika tłumaczenia powróci do Automatyczny (Auto).\n"
    L"\n"
    L"Kontynuować?",
    .gguf_manager_rename_title = L"Zmień nazwę modelu",
    .gguf_manager_rename_body = L"Wpisz nową nazwę (maks. 64 znaki, bez spacji).",
    .gguf_manager_rename_invalid = L"Tej nazwy nie można użyć. Wpisz niepustą nazwę, różną od istniejących, bez spacji i separatorów ścieżki, do 64 znaków.",
    .gguf_manager_done = L"Zapisano zmiany.",
    .gguf_manager_err_serialize = L"Nie można zserializować registry.json (nazwa pliku została odrzucona). Nic nie zmieniono.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% jest niedostępny; nie można zlokalizować folderu wspólnych modeli.",
    .gguf_manager_err_write = L"Nie można zapisać registry.json.",
    .gguf_manager_err_write_partial = L"Nie można w pełni zapisać registry.json.",
    .gguf_manager_err_registry_damaged = L"registry.json jest uszkodzony lub ma nieobsługiwaną strukturę. NIE został zmieniony. Napraw go lub usuń i spróbuj ponownie.",
    .tooltip_translate_failed = L"Tłumaczenie nie powiodło się. Spróbuj ponownie.",
    .repair_transient_body = L"Lokalny silnik tłumaczenia jest tymczasowo niedostępny (może się uruchamiać). Poczekaj chwilę i spróbuj ponownie.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Potwierdź",
    .dialog_cancel = L"Anuluj",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Dodaj z pliku…",
    .gguf_manager_add_hf = L"Dodaj z Hugging Face…",
    .hf_add_title = L"Dodaj model Hugging Face",
    .hf_url_label = L"Adres URL modelu",
    .hf_downloading = L"Pobieranie…",
    .hf_failed = L"Pobieranie nie powiodło się.",
    .hf_invalid_url = L"Podaj adres URL modelu huggingface.co (resolve, blob lub strona modelu)",
    .hf_done = L"Zarejestrowano.",
    .openai_base_url_hint = L"np.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Klucz API",
    // REQ-051: przycisk [Usuń ustawienia].
    .openai_delete_settings = L"Usuń ustawienia",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Sprawdź aktualizacje",
    .update_idle_note = L"Sprawdź, czy używasz najnowszej wersji Emebala Chat.",
    .update_checking = L"Sprawdzanie aktualizacji…",
    .update_checking_hint = L"Działa w tle; możesz zamknąć to okno.",
    .update_uptodate = L"Masz najnowszą wersję",
    .update_dlg_title = L"Dostępna aktualizacja",
    .update_dlg_version = L"Nowa wersja",
    .update_dlg_version_cur = L"masz wersję {v}",
    .update_dlg_size = L"Rozmiar",
    .update_dlg_size_approx = L"około {s}",
    .update_dlg_privacy = L"Sprawdzanie i pobieranie łączą się z GitHubem.",
    .update_dlg_download = L"Pobierz",
    .update_dlg_later = L"Później",
    .update_dl_progress = L"Pobieranie…",
    .update_dl_cancel = L"Anuluj",
    .update_dl_cancelled = L"Pobieranie anulowane.",
    .update_ready_title = L"Pobieranie ukończone — gotowe do instalacji",
    .update_ready_warn = L"Aplikacja zamknie się, aby zainstalować.",
    .update_ready_install = L"Zainstaluj teraz",
    .update_ready_later = L"Później",
    .update_err_offline = L"Jesteś offline. Sprawdź połączenie i spróbuj ponownie.",
    .update_err_rate = L"Zbyt wiele żądań. Spróbuj ponownie za chwilę.",
    .update_err_hash = L"Pobrany plik nie przeszedł weryfikacji.",
    .update_err_retry = L"Spróbuj ponownie",
    .update_snoozed_line = L"Możesz zainstalować aktualizację później.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Pobieranie modelu",
    .model_dl_consent_body = L"Wbudowany model tłumaczeń nie jest zainstalowany. Pobrać teraz? (około {s})",
    .model_dl_privacy = L"Pobieranie łączy się z huggingface.co.",
    .model_dl_download = L"Pobierz teraz",
    .model_dl_verifying = L"Weryfikacja pobranego pliku…",
    .model_dl_placing = L"Instalowanie modelu…",
    .model_dl_done = L"Model jest gotowy. Lokalny silnik użyje go od następnego tłumaczenia.",
    .model_dl_failed = L"Pobieranie nie powiodło się. Sprawdź połączenie i spróbuj ponownie.",
    .model_dl_disk_space = L"Za mało wolnego miejsca na dysku (wymagane około {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Ustawienia ładowania modelu tłumaczenia…",
    .engine_load_settings_title = L"Ładowanie modelu tłumaczenia",
    .load_mode_on_use = L"Ładuj przy użyciu (zalecane)",
    .load_mode_warmup = L"Ładuj wcześniej",
    .load_mode_on_use_desc = L"Oszczędza pamięć. Pierwsze tłumaczenie może się rozpocząć kilka sekund później.",
    .load_mode_warmup_desc = L"Tłumaczenie zaczyna się od razu. Model pozostaje w pamięci.",
    .gguf_policy_sticky = L"(Pozostaw w pamięci)",
    .gguf_policy_evict = L"(Ładuj przy użyciu)",
    .gguf_manager_toggle_policy = L"Zmień zasady",
    .gguf_duplicate_warning = L"Zduplikowane pliki: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Wyczyść duplikaty",
    .gguf_manager_dedupe_done = L"Zduplikowane pliki zostały przeniesione do Kosza.",
    .gguf_manager_dedupe_failed = L"Nie można wyczyścić zduplikowanych plików. Sprawdź, czy nie są używane przez inny program.",
    .menu_ui_theme = L"Motyw interfejsu",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Ciepły Editorial",
    .theme_monolith = L"Dopracowany monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Pływające tłumaczenie zwrotne (weryfikacja)",
    .back_translate_header = L"🔄 Sprawdzenie tłumaczenia zwrotnego",
    .back_translate_match_exact = L"✓ Znaczenie zgodne",
    .back_translate_match_check = L"⚡ Sprawdź niuans",
    .back_translate_reference = L"↳ Oryginał: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"To nie jest prawidłowy plik modelu GGUF; nie został zarejestrowany.",
    .gguf_manager_active_suffix = L"(w użyciu)",
    .gguf_manager_activated = L"Ten model jest teraz używany.",
};
// 15. Czech (cs)
const LocalizedStrings kStringsCzech = {
    .menu_status_active = L"Stav: aktivní (F9: pauza)",
    .menu_status_paused = L"Stav: pozastaveno (F9: pokračovat)",
    .menu_engine = L"Překladový engine",
    .menu_engine_google = L"Google Překladač (zdarma / bez instalace)",
    .menu_engine_local = L"Vestavěný lokální engine (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Zdrojový jazyk (vstup)",
    .menu_target_lang = L"Cílový jazyk (výstup)",
    .menu_swap_langs = L"Prohodit zdroj ⇄ cíl (dvojklik)",
    .menu_auto_send = L"Automatické odeslání klávesou Enter",
    .menu_sound_feedback = L"Zvuková zpětná vazba",
    .menu_show_badge = L"Plovoucí odznak viditelný",
    .menu_start_with_windows = L"Spouštět s Windows",
    .menu_cheatsheet = L"Přehled klávesových zkratek a nápověda...",
    .menu_exit = L"Ukončit Emebala Chat",
    .menu_about = L"O Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — klávesové zkratky a průvodce",
    .cheatsheet_body = L"Klávesové zkratky a průvodce Emebala Chat:\n\n"
    L"  • F9 : zapnout / pauza\n"
    L"  • Ctrl + F9 : změnit cílový jazyk\n"
    L"  • Ctrl + Shift + Enter : přepnout automatické odesílání\n"
    L"  • Shift + Enter : přeložit a ihned odeslat\n\n"
    L"Ovládání myší na odznaku:\n"
    L"  • Levé tlačítko : zapnout / pauza\n"
    L"  • Dvojklik : prohodit zdroj ⇄ cíl\n"
    L"  • Pravé tlačítko : otevřít menu nastavení\n\n"
    L"Režimy překladu:\n"
    L"  • Pouze nahrazení (auto-odesílání vypnuto): nahradí řádek překladem ke kontrole.\n"
    L"  • Automatické odesílání (zapnuto): nahradí řádek a ihned stiskne Enter.",
    .about_title = L"O Emebala Chat",
    .badge_active = L"Aktivní",
    .badge_translating = L"Překládám...",
    .badge_paused = L"Pozastaveno",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Vybraný text nelze zkopírovat. Zkontrolujte cílovou aplikaci a zkuste to znovu.",
    .tooltip_no_selection = L"Nebyl vybrán žádný text k překladu.",
    .auto_detect = L"Automaticky rozpoznat",
    .app_already_running = L"Emebala Chat již běží na pozadí.\nZkontrolujte oznamovací oblast.",
    .app_com_failed = L"Inicializace COM se nezdařila.\nPlovoucí odznak a převod textu na řeč budou nedostupné,\nale překlad, klávesové zkratky, tray a zvuky stále fungují.",
    .about_tagline = L"Konec kopírování a vkládání. Pište přirozeně ve svém rodném jazyce — překlad nahrazuje váš psaní v reálném čase v libovolné aplikaci Windows.",
    .about_feature0 = L"⚡ Přetáhněte pro překlad — vyberte text v libovolné aplikaci, plovoucí ikna přeloží okamžitě.",
    .about_feature1 = L"🔊 Neuronové TTS — vyslovuje všech 37 jazyků pomocí nainstalovaných hlasových balíčků Windows.",
    .about_feature2 = L"🔒 100% na zařízení a soukromé — aktivní pouze při podržení zkratky; schránka nedotčena.",
    .about_etymology = L"V roce 2000 př. n. l. mezopotámští písaři nazývaly «Eme-bala» ty, kdo mění jazyk v most mezi světy.",
    .about_link_website = L"Web",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Pracovní doba 10:00–19:00 KST",
    .about_contact_lead = L"Hlavní architekt: Yongtai Kim",
    .tooltip_copied = L"✓ Zkopírováno!",
    .tooltip_button_copy = L"📋 Kopírovat",
    .tooltip_button_tts = L"🔊 Hlas",
    .menu_ui_language = L"Jazyk rozhraní",
    .menu_ui_language_auto = L"Auto (systémový jazyk)",
    .about_reset_button = L"Obnovit výchozí nastavení systému",
    .about_reset_done = L"Výchozí hodnoty obnoveny",
    .menu_typing_group = L"Psaní na klávesnici",
    .menu_tooltip_group = L"Tooltip tažení",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Pro tento jazyk není nainstalován žádný hlas Windows. Klikněte znovu na 🔊 pro otevření nastavení Rozpoznávání řeči.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Oznámení o ochraně soukromí",
    .privacy_notice_body = L"Zde jsou zásady ochrany soukromí aplikace Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat neprovožuje žádné vlastní servery.\n"
    L"• Při lokálním modelu překládaný text neopouští vaše zařízení.\n"
    L"• Pokud zvolíte Google Translate nebo dojde k automatickému přepnutí do cloudu, vybraný nebo zadaný text je posílán přímo společnosti Google k přeložení, bez prochzení přes Emebala.\n"
    L"• Diagnostické protokoly jsou ve výchozím nastavení VYPNUTÉ; zapnete je v nastavení (opt-in).\n"
    L"• Pokud nechcete odesílat text do cloudu (Google), otevřete nabídku ikony v oznamovací oblasti, zvolte “Překladový engine” a vyberte “Vestavěný lokální engine”. Bez nainstalovaného lokálního modelu a se zakázaným cloudovým zálohováním překlad neběží — nic se neodesílá.\n"
    L"\n"
    L"Podrobnosti najdete v souboru README, který lze kdykoli znovu přečíst.\n",
    .cheatsheet_config_path = L"Konfigurační soubor: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Text byl příliš dlouhý: přeloženy byly pouze začátek a konec.",
    .tooltip_untranslated_above = L"Nahoře je nový nepřeložený text. Umístěte kurzor na konec tohoto řádku a stisknutím Enteru jej přeložte.",
    .repair_in_progress = L"Oprava místních součástí enginu…",
    .repair_failed_title = L"Místní překlad není dostupný",
    .repair_failed_body = L"Soubory místního překladového enginu chybí, takže je překlad pozastaven. Pro obnovení místního enginu přeinstalujte Emebala Chat, nebo pro přechod na cloudový (Google) překlad vyberte v nabídce oznamovací oblasti „Google Překladač“ v části „Překladový engine“.",
    .menu_engine_openai = L"Kompatibilní s OpenAI (uživatelský server)…",
    .openai_settings_title = L"Nastavení enginu kompatibilního s OpenAI",
    .openai_settings_action = L"Nastavení enginu kompatibilního s OpenAI…",
    .openai_base_url_label = L"Základní URL",
    .openai_api_key_label = L"API klíč",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Načíst seznam modelů",
    .openai_fetch_failed = L"Seznam modelů se nepodařilo načíst. Název modelu můžete zadat přímo.",
    .openai_http_warning_title = L"Nezabezpečené připojení (HTTP)",
    .openai_http_warning_body = L"Základní URL používá HTTP (nešifrované). Váš API klíč a text budou odeslány v nešifrované podobě. Pokračovat?",
    .openai_saved = L"Nastavení kompatibilní s OpenAI uložena.",
    .openai_key_masked = L"Uložený klíč: ",
    .openai_invalid_base_url = L"Základní URL není platná. Příklad: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Model určený uživatelem (.gguf)",
    .menu_browse_gguf_file = L"Zaregistrovat jiný model .gguf…",
    .user_gguf_quality_title = L"Upozornění na kvalitu překladu",
    .user_gguf_quality_body = L"Vybraný model není Hy-MT2. Aktuální verze používá výhradně prompt pro Hy-MT2, takže kvalita překladu s tímto modelem není zaručena. Pokračovat?",
    .user_gguf_registered_title = L"Model zaregistrován",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Vybraný model byl zaregistrován v lokálním enginu.\n"
    L"\n"
    L"Tento model se používá k překladu, když zvolíte \"Výběr překladového enginu > Volba uživatele (.gguf)\".\n"
    L"\n"
    L"Kdy se projeví: nejpozději přibližně 1 minutu po registraci (po ukončení enginu z důvodu nečinnosti)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Tento model je již vestavěný v Emebala Chat. Registrace není potřeba. Vestavěný lokální překladový engine můžete vybrat přímo.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(není registrován)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Spravovat modely…",
    .gguf_manager_title = L"Správa uživatelských modelů",
    .gguf_manager_empty = L"Nejsou zaregistrovány žádné uživatelské modely.",
    .gguf_manager_rename = L"Přejmenovat…",
    .gguf_manager_delete = L"Odstranit…",
    .gguf_manager_close = L"Zavřít",
    .gguf_manager_delete_confirm_title = L"Odstranit registraci modelu",
    .gguf_manager_delete_confirm_body = L"Registrace vybraného modelu bude odstraněna.\n"
    L"\n"
    L"Soubor modelu (.gguf) zůstane na disku a nebude odstraněn. Pokud byl tento model používán, výběr překladového enginu se vrátí na Automaticky (Auto).\n"
    L"\n"
    L"Pokračovat?",
    .gguf_manager_rename_title = L"Přejmenovat model",
    .gguf_manager_rename_body = L"Zadejte nový název (max. 64 znaků, bez mezer).",
    .gguf_manager_rename_invalid = L"Tento název nelze použít. Zadejte neprázdný název, který se liší od existujících, bez mezer a oddělovačů cesty, do 64 znaků.",
    .gguf_manager_done = L"Změny byly uloženy.",
    .gguf_manager_err_serialize = L"registry.json se nepodařilo serializovat (název souboru byl odmítnut). Nic se nezměnilo.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% není k dispozici; nelze najít sdílenou složku modelů.",
    .gguf_manager_err_write = L"registry.json se nepodařilo zapsat.",
    .gguf_manager_err_write_partial = L"registry.json se nepodařilo zapsat celý.",
    .gguf_manager_err_registry_damaged = L"registry.json je poškozený nebo má nepodporované schéma. Nebyl změněn. Opravte ho nebo ho odstraňte a zkuste to znovu.",
    .tooltip_translate_failed = L"Překlad se nezdařil. Zkuste to znovu.",
    .repair_transient_body = L"Místní překladový engine je dočasně nedostupný (možná se spouští). Počkejte chvíli a zkuste to znovu.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Potvrdit",
    .dialog_cancel = L"Storno",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Přidat ze souboru…",
    .gguf_manager_add_hf = L"Přidat z Hugging Face…",
    .hf_add_title = L"Přidat model Hugging Face",
    .hf_url_label = L"URL modelu",
    .hf_downloading = L"Stahování…",
    .hf_failed = L"Stažení se nezdařilo.",
    .hf_invalid_url = L"Zadejte URL modelu huggingface.co (resolve, blob nebo stránka modelu)",
    .hf_done = L"Zaregistrováno.",
    .openai_base_url_hint = L"např.: https://api.openai.com/v1",
    .openai_api_key_hint = L"API klíč",
    // REQ-051: tlačítko [Odstranit nastavení].
    .openai_delete_settings = L"Odstranit nastavení",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Zkontrolovat aktualizace",
    .update_idle_note = L"Zkontrolujte, zda používáte nejnovější verzi aplikace Emebala Chat.",
    .update_checking = L"Kontrola aktualizací…",
    .update_checking_hint = L"Běží na pozadí; toto okno můžete zavřít.",
    .update_uptodate = L"Používáte nejnovější verzi",
    .update_dlg_title = L"Je k dispozici aktualizace",
    .update_dlg_version = L"Nová verze",
    .update_dlg_version_cur = L"máte {v}",
    .update_dlg_size = L"Velikost",
    .update_dlg_size_approx = L"přibližně {s}",
    .update_dlg_privacy = L"Kontrola a stahování se připojuje k GitHubu.",
    .update_dlg_download = L"Stáhnout",
    .update_dlg_later = L"Později",
    .update_dl_progress = L"Stahování…",
    .update_dl_cancel = L"Zrušit",
    .update_dl_cancelled = L"Stahování bylo zrušeno.",
    .update_ready_title = L"Stahování dokončeno — připraveno k instalaci",
    .update_ready_warn = L"Aplikace se pro instalaci zavře.",
    .update_ready_install = L"Nainstalovat nyní",
    .update_ready_later = L"Později",
    .update_err_offline = L"Jste offline. Zkontrolujte připojení a zkuste to znovu.",
    .update_err_rate = L"Příliš mnoho požadavků. Zkuste to znovu za chvíli.",
    .update_err_hash = L"Stažený soubor neprošel ověřením.",
    .update_err_retry = L"Zkusit znovu",
    .update_snoozed_line = L"Aktualizaci můžete nainstalovat později.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Stáhnout model",
    .model_dl_consent_body = L"Integrovaný překladatelský model není nainstalován. Stáhnout nyní? (přibližně {s})",
    .model_dl_privacy = L"Stahování se připojuje k huggingface.co.",
    .model_dl_download = L"Stáhnout nyní",
    .model_dl_verifying = L"Ověřování staženého souboru…",
    .model_dl_placing = L"Instalace modelu…",
    .model_dl_done = L"Model je připraven. Místní engine ho použije od dalšího překladu.",
    .model_dl_failed = L"Stahování se nezdařilo. Zkontrolujte připojení a zkuste to znovu.",
    .model_dl_disk_space = L"Nedostatek volného místa na disku (vyžadováno přibližně {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Nastavení načítání překladového modelu…",
    .engine_load_settings_title = L"Načítání překladového modelu",
    .load_mode_on_use = L"Načíst při použití (doporučeno)",
    .load_mode_warmup = L"Načíst předem",
    .load_mode_on_use_desc = L"Šetří paměť. První překlad se může začít o několik sekund později.",
    .load_mode_warmup_desc = L"Překlad začne okamžitě. Model zůstává v paměti.",
    .gguf_policy_sticky = L"(Ponechat načtený)",
    .gguf_policy_evict = L"(Načíst při použití)",
    .gguf_manager_toggle_policy = L"Přepnout zásady",
    .gguf_duplicate_warning = L"Duplicitní soubory: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Vyčistit duplicity",
    .gguf_manager_dedupe_done = L"Duplicitní soubory byly přesunuty do Koše.",
    .gguf_manager_dedupe_failed = L"Duplicitní soubory nelze vyčistit. Zkontrolujte, jestli je nepoužívá jiný program.",
    .menu_ui_theme = L"Motiv rozhraní",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Teplý Editorial",
    .theme_monolith = L"Ušlechtilý monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Plovoucí zpětný překlad (ověření)",
    .back_translate_header = L"🔄 Ověření zpětného překladu",
    .back_translate_match_exact = L"✓ Význam odpovídá",
    .back_translate_match_check = L"⚡ Zkontrolovat odstín",
    .back_translate_reference = L"↳ Originál: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Toto není platný soubor modelu GGUF; nebyl zaregistrován.",
    .gguf_manager_active_suffix = L"(používá se)",
    .gguf_manager_activated = L"Tento model se nyní používá.",
};
// 16. Hungarian (hu)
const LocalizedStrings kStringsHungarian = {
    .menu_status_active = L"Állapot: aktív (F9: szünet)",
    .menu_status_paused = L"Állapot: szünetel (F9: folytatás)",
    .menu_engine = L"Fordítómotor",
    .menu_engine_google = L"Google Fordító (ingyenes / telepítés nélkül)",
    .menu_engine_local = L"Beépített helyi motor (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Forrásnyelv (bemenet)",
    .menu_target_lang = L"Célnyelv (kimenet)",
    .menu_swap_langs = L"Forrás ⇄ cél felcserélése (dupla kattintás)",
    .menu_auto_send = L"Automatikus küldés Enterrel",
    .menu_sound_feedback = L"Hangvisszajelzés (hangnemek)",
    .menu_show_badge = L"Lebegő jelvény látható",
    .menu_start_with_windows = L"Indítás Windows-szal",
    .menu_cheatsheet = L"Gyorsbillentyű-súgó és súgó...",
    .menu_exit = L"Emebala Chat bezárása",
    .menu_about = L"Az Emebala Chat névjegye…",
    .cheatsheet_title = L"Emebala Chat — gyorsbillentyűk és útmutató",
    .cheatsheet_body = L"Gyorsbillentyűk és útmutató az Emebala Chathez:\n\n"
    L"  • F9 : bekapcsolás / szünet\n"
    L"  • Ctrl + F9 : célnyelv váltása\n"
    L"  • Ctrl + Shift + Enter : automatikus küldés átváltása\n"
    L"  • Shift + Enter : fordítás és azonnali küldés\n\n"
    L"Egérvezérlés a jelvényen:\n"
    L"  • Bal kattintás : bekapcsolás / szünet\n"
    L"  • Dupla kattintás : forrás ⇄ cél felcserélése\n"
    L"  • Jobb kattintás : beállítási menü megnyitása\n\n"
    L"Fordítási módok:\n"
    L"  • Csere csak (auto-küldés ki): a sort a fordításra cseréli ellenőrzésre.\n"
    L"  • Automatikus küldés (be): cserél és azonnal Entert nyom.",
    .about_title = L"Az Emebala Chat névjegye",
    .badge_active = L"Aktív",
    .badge_translating = L"Fordítás...",
    .badge_paused = L"Szünetel",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Nem sikerült kimásolni a kijelölt szöveget. Ellenőrizze a célalkalmazást, és próbálja újra.",
    .tooltip_no_selection = L"Nincs fordításra kijelölt szöveg.",
    .auto_detect = L"Automatikus felismerés",
    .app_already_running = L"Az Emebala Chat már fut a háttérben.\nEllenőrizze az értesítési tálcát.",
    .app_com_failed = L"A COM inicializálása sikertelen.\nA lebegő jelvény és a felolvasás nem lesz elérhető,\nde a fordítás, gyorsbillentyűk, tálca és hangok tovább működnek.",
    .about_tagline = L"Kopírozás és beillesztés többé nem kell. Gépeljen természetesen az anyanyelvén — a fordítás valós időben felváltja a gépelést bármely Windows-alkalmazásban.",
    .about_feature0 = L"⚡ Húzássos fordítás — jelöljön ki szöveget bármely appban, a lebegő ikon azonnal fordít.",
    .about_feature1 = L"🔊 Neurális TTS — mind a 37 nyelvet kimondja a telepített Windows hangcsomagokkal.",
    .about_feature2 = L"🔒 100% eszközön és privát — csak a gyorsbillentyű tartása közben aktív; vágólap érintetlen.",
    .about_etymology = L"Kr. e. 2000-ben a mezopotámiai írnokok „Eme-bala”-nak nevezték azokat, akik a nyelvet a világok hidává alakítják.",
    .about_link_website = L"Weboldal",
    .about_link_contact = L"Kapcsolat",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Munkaidő 10:00–19:00 KST",
    .about_contact_lead = L"Vezető építész: Yongtai Kim",
    .tooltip_copied = L"✓ Kimásolva!",
    .tooltip_button_copy = L"📋 Másolás",
    .tooltip_button_tts = L"🔊 Hang",
    .menu_ui_language = L"Felhasználói felület nyelve",
    .menu_ui_language_auto = L"Automatikus (rendszernyelv)",
    .about_reset_button = L"Visszaállítás rendszeralapértelmezettekre",
    .about_reset_done = L"Alapértelmezések visszaállítva",
    .menu_typing_group = L"Billentyűzetes gépelés",
    .menu_tooltip_group = L"Húzás tippablak",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Ehhez a nyelvhez nincs telepítve Windows-hang. Kattintson újra a 🔊 gombra a Beszéd beállításainak megnyitásához.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Adatvédelmi tájékoztató",
    .privacy_notice_body = L"Az Emebala Chat adatvédelmi elvei:\n"
    L"\n"
    L"• Az Emebala Chat nem üzemeltet saját szervert.\n"
    L"• Helyi modell használatakor a fordított szöveg nem hagyja el az eszközt.\n"
    L"• Ha a Google Fordítót választja, vagy automatikusan felhő üzemmódra vált, a kijelölt vagy bevitt szöveg közvetlenül a Google-höz megy a fordításhoz – nem az Emebala-n keresztül.\n"
    L"• A diagnosztikai naplók alapértelmezés szerint KI vannak kapcsolva; a beállításokban kapcsolhatja be (hozzájulás).\n"
    L"• Ha nem szeretné szöveget a felhőbe (Google) küldeni, nyissa meg a tálcaikon menüjét, válassza a “Fordítómotor” pontot, majd a “Beépített helyi motor” lehetőséget. Ha nincs telepített helyi modell és a felhőtartalék ki van kapcsolva, a fordítás nem fut — semmi sem kerül küldésre.\n"
    L"\n"
    L"A teljes részletezés a README fájlban olvasható, bármikor újra.\n",
    .cheatsheet_config_path = L"Konfigurációs fájl: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"A szöveg túl hosszú: csak az eleje és a vége lett lefordítva.",
    .tooltip_untranslated_above = L"Új, le nem fordított szöveg van fent. Helyezze a kurzort a sor végére, és az Enter megnyomásával fordítsa le.",
    .repair_in_progress = L"A helyi motor összetevőinek javítása…",
    .repair_failed_title = L"A helyi fordítás nem érhető el",
    .repair_failed_body = L"A helyi fordítómotor fájljai hiányoznak, ezért a fordítás szünetel. A helyi motor helyreállításához telepítse újra az Emebala Chatet, vagy váltson a felhőalapú (Google) fordításra a „Google Fordító” választásával a tálca „Fordítómotor” menüjében.",
    .menu_engine_openai = L"OpenAI-kompatibilis (egyéni szerver)…",
    .openai_settings_title = L"OpenAI-kompatibilis motor beállításai",
    .openai_settings_action = L"OpenAI-kompatibilis motor beállításai…",
    .openai_base_url_label = L"Alap-URL",
    .openai_api_key_label = L"API-kulcs",
    .openai_model_label = L"Modell",
    .openai_fetch_models = L"Modelllista lekérése",
    .openai_fetch_failed = L"A modelllista lekérése nem sikerült. Modellnevet közvetlenül is beírhat.",
    .openai_http_warning_title = L"Nem biztonságos kapcsolat (HTTP)",
    .openai_http_warning_body = L"Az alap-URL HTTP-et használ (titkosítás nélkül). API-kulcsa és szövege titkosítatlanul lesz elküldve. Folytatja?",
    .openai_saved = L"OpenAI-kompatibilis beállítások mentve.",
    .openai_key_masked = L"Mentett kulcs: ",
    .openai_invalid_base_url = L"Az alap-URL érvénytelen. Példa: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Felhasználó által megadott modell (.gguf)",
    .menu_browse_gguf_file = L"Más .gguf modell regisztrálása…",
    .user_gguf_quality_title = L"Tájékoztatás a fordítás minőségéről",
    .user_gguf_quality_body = L"A kiválasztott modell nem Hy-MT2. A jelenlegi verzió csak Hy-MT2-höz készült promptot használ, ezért a fordítás minősége ezzel a modellel nem garantált. Folytatja?",
    .user_gguf_registered_title = L"Modell regisztrálva",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"A kiválasztott modell regisztrálva lett a helyi motorban.\n"
    L"\n"
    L"Ez a modell akkor használható fordításhoz, amikor a \"Fordítómotor választása > Felhasználói választás (.gguf)\" lehetőséget választja.\n"
    L"\n"
    L"Mikor lép hatályba: regisztráció után legfeljebb kb. 1 perc (a motor inaktivitás miatti leállása után)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Ez a modell már beépítve van az Emebala Chatbe. Regisztrációra nincs szükség. A beépített helyi fordítómotort közvetlenül kiválaszthatod.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(nincs regisztrálva)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Modellek kezelése…",
    .gguf_manager_title = L"Felhasználói modellek kezelése",
    .gguf_manager_empty = L"Nincsenek regisztrált felhasználói modellek.",
    .gguf_manager_rename = L"Átnevezés…",
    .gguf_manager_delete = L"Törlés…",
    .gguf_manager_close = L"Bezárás",
    .gguf_manager_delete_confirm_title = L"Modellregisztráció törlése",
    .gguf_manager_delete_confirm_body = L"A kiválasztott modell regisztrációja törlődik.\n"
    L"\n"
    L"A modellfájl (.gguf) a lemezen marad és nem törlődik. Ha ezt a modellt használta, a fordítómotor választása visszaáll Automatikus (Auto) értékre.\n"
    L"\n"
    L"Folytatja?",
    .gguf_manager_rename_title = L"Modell átnevezése",
    .gguf_manager_rename_body = L"Adjon meg új nevet (legfeljebb 64 karakter, szóköz nélkül).",
    .gguf_manager_rename_invalid = L"Ez a név nem használható. Adj meg egy nem üres, a meglévőktől eltérő, szóközöket és elválasztókat nem tartalmazó, legfeljebb 64 karakteres nevet.",
    .gguf_manager_done = L"Változások mentve.",
    .gguf_manager_err_serialize = L"A registry.json nem szerializálható (egy fájlnév elutasítva). Semmi sem változott.",
    .gguf_manager_err_no_localappdata = L"A %LOCALAPPDATA% nem érhető el; a megosztott modellmappa nem található.",
    .gguf_manager_err_write = L"A registry.json nem írható.",
    .gguf_manager_err_write_partial = L"A registry.json nem írható teljesen.",
    .gguf_manager_err_registry_damaged = L"A registry.json sérült vagy nem támogatott sémájú. NEM lett módosítva. Javítsa ki vagy távolítsa el, majd próbálja újra.",
    .tooltip_translate_failed = L"A fordítás nem sikerült. Próbálja újra.",
    .repair_transient_body = L"A helyi fordítómotor átmenetileg nem érhető el (előfordulhat, hogy éppen indul). Várjon egy pillanatot, és próbálja újra.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Rendben",
    .dialog_cancel = L"Mégsem",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Hozzáadás fájlból…",
    .gguf_manager_add_hf = L"Hozzáadás a Hugging Face-ről…",
    .hf_add_title = L"Hugging Face-modell hozzáadása",
    .hf_url_label = L"Modell URL-címe",
    .hf_downloading = L"Letöltés folyamatban…",
    .hf_failed = L"A letöltés nem sikerült.",
    .hf_invalid_url = L"Adja meg a huggingface.co modell URL-jét (resolve, blob vagy modelloldal)",
    .hf_done = L"Regisztrálva.",
    .openai_base_url_hint = L"pl.: https://api.openai.com/v1",
    .openai_api_key_hint = L"API-kulcs",
    // REQ-051: [Beállítások törlése] gomb.
    .openai_delete_settings = L"Beállítások törlése",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Frissítések keresése",
    .update_idle_note = L"Nézze meg, a legújabb Emebala Chatet futtatja-e.",
    .update_checking = L"Frissítések keresése…",
    .update_checking_hint = L"A háttérben fut; bezárhatja ezt az ablakot.",
    .update_uptodate = L"Ön a legújabb verziót használja",
    .update_dlg_title = L"Frissítés érhető el",
    .update_dlg_version = L"Új verzió",
    .update_dlg_version_cur = L"Öné: {v}",
    .update_dlg_size = L"Méret",
    .update_dlg_size_approx = L"körülbelül {s}",
    .update_dlg_privacy = L"A keresés és a letöltés a GitHubhoz kapcsolódik.",
    .update_dlg_download = L"Letöltés",
    .update_dlg_later = L"Később",
    .update_dl_progress = L"Letöltés folyamatban…",
    .update_dl_cancel = L"Mégse",
    .update_dl_cancelled = L"A letöltés megszakítva.",
    .update_ready_title = L"A letöltés kész — telepítésre kész",
    .update_ready_warn = L"A telepítéshez az alkalmazás bezár.",
    .update_ready_install = L"Telepítés most",
    .update_ready_later = L"Később",
    .update_err_offline = L"Ön offline. Ellenőrizze a kapcsolatot, és próbálja újra.",
    .update_err_rate = L"Túl sok kérés. Kérjük, próbálja meg újra egy kis idő múlva.",
    .update_err_hash = L"A letöltött fájl nem ment át az ellenőrzésen.",
    .update_err_retry = L"Újrapróbálás",
    .update_snoozed_line = L"A frissítés később is telepíthető.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Modell letöltése",
    .model_dl_consent_body = L"A beépített fordítómodell nincs telepítve. Letölti most? (körülbelül {s})",
    .model_dl_privacy = L"A letöltés a huggingface.co-hoz kapcsolódik.",
    .model_dl_download = L"Letöltés most",
    .model_dl_verifying = L"A letöltés ellenőrzése…",
    .model_dl_placing = L"A modell telepítése…",
    .model_dl_done = L"A modell kész. A helyi motor a következő fordítástól használja.",
    .model_dl_failed = L"A letöltés nem sikerült. Ellenőrizze a kapcsolatot, és próbálja újra.",
    .model_dl_disk_space = L"Nincs elég szabad lemezterület (kb. {s} szükséges).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"A fordítómodell betöltésének beállításai…",
    .engine_load_settings_title = L"Fordítómodell betöltése",
    .load_mode_on_use = L"Betöltés használatkor (ajánlott)",
    .load_mode_warmup = L"Előre betöltés",
    .load_mode_on_use_desc = L"Memóriát takarít meg. Az első fordítás néhány másodperccel később indulhat.",
    .load_mode_warmup_desc = L"A fordítás azonnal elindul. A modell a memóriában marad.",
    .gguf_policy_sticky = L"(Maradjon bent)",
    .gguf_policy_evict = L"(Betöltés használatkor)",
    .gguf_manager_toggle_policy = L"Szabály váltása",
    .gguf_duplicate_warning = L"Duplikált fájlok: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Duplikációk törlése",
    .gguf_manager_dedupe_done = L"A duplikált fájlok a Lomtárba lettek áthelyezve.",
    .gguf_manager_dedupe_failed = L"A duplikált fájlokat nem sikerült eltávolítani. Ellenőrizze, hogy más program nem használja-e őket.",
    .menu_ui_theme = L"Felület téma",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Meleg Editorial",
    .theme_monolith = L"Finom monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Lebegő visszafordítás megjelenítése (ellenőrzés)",
    .back_translate_header = L"🔄 Visszafordítás ellenőrzése",
    .back_translate_match_exact = L"✓ Jelentés egyezik",
    .back_translate_match_check = L"⚡ Árnyalat ellenőrzése",
    .back_translate_reference = L"↳ Eredeti: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Ez nem érvényes GGUF modellfájl; nem lett regisztrálva.",
    .gguf_manager_active_suffix = L"(használatban)",
    .gguf_manager_activated = L"Ez a modell most használatban van.",
};
// 17. Romanian (ro)
const LocalizedStrings kStringsRomanian = {
    .menu_status_active = L"Stare: activ (F9: pauză)",
    .menu_status_paused = L"Stare: întrerupt (F9: reia)",
    .menu_engine = L"Motor de traducere",
    .menu_engine_google = L"Google Translate (gratuit / fără instalare)",
    .menu_engine_local = L"Motor local încorporat (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Limba sursă (intrare)",
    .menu_target_lang = L"Limba țintă (ieșire)",
    .menu_swap_langs = L"Schimbă sursa ⇄ ținta (dublu clic)",
    .menu_auto_send = L"Trimitere automată cu Enter",
    .menu_sound_feedback = L"Semnal sonor (tonuri)",
    .menu_show_badge = L"Insignă flotantă vizibilă",
    .menu_start_with_windows = L"Pornește cu Windows",
    .menu_cheatsheet = L"Ghid de scurtături și ajutor...",
    .menu_exit = L"Închide Emebala Chat",
    .menu_about = L"Despre Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — scurtături și ghid",
    .cheatsheet_body = L"Scurtături și ghid de utilizare pentru Emebala Chat:\n\n"
    L"  • F9 : activează / întrerupe\n"
    L"  • Ctrl + F9 : schimbă limba țintă\n"
    L"  • Ctrl + Shift + Enter : comută trimiterea automată\n"
    L"  • Shift + Enter : trad și trimite imediat\n\n"
    L"Comenzi cu mouse-ul pe insignă:\n"
    L"  • Clic stânga : activează / întrerupe\n"
    L"  • Dublu clic : schimbă sursa ⇄ ținta\n"
    L"  • Clic dreapta : deschide meniul de setări\n\n"
    L"Moduri de traducere:\n"
    L"  • Doar înlocuire (trimitere auto oprită): înlocuiește linia cu traducerea pentru verificare.\n"
    L"  • Trimitere automată (pornită): înlocuiește linia și apasă imediat Enter.",
    .about_title = L"Despre Emebala Chat",
    .badge_active = L"Activ",
    .badge_translating = L"Se traduce...",
    .badge_paused = L"Întrerupt",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Nu s-a putut copia textul selectat. Verificați aplicația țintă și încercați din nou.",
    .tooltip_no_selection = L"Nu este selectat niciun text de tradus.",
    .auto_detect = L"Detectare automată",
    .app_already_running = L"Emebala Chat rulează deja în fundal.\nVerificați zona de notificări.",
    .app_com_failed = L"Inicializarea COM a eșuat.\nInsigna flotantă și citirea textului vor fi indisponibile,\ndar traducerea, scurtăturile, tava și sunetele continuă să funcționeze.",
    .about_tagline = L"Gata cu copierea și lipirea. Tastați natural în limba maternă — traducerea vă înlocuiește tastarea în timp real în orice aplicație Windows.",
    .about_feature0 = L"⚡ Trage pentru a traduce — selectează text în orice aplicație, iconița flotantă traduce instant.",
    .about_feature1 = L"🔊 TTS neuronal — pronunță toate cele 37 de limbi prin pachetele de voci Windows instalate.",
    .about_feature2 = L"🔒 100% pe dispozitiv și privat — activ doar cât timp este ținută scurtătura; clipboard neatins.",
    .about_etymology = L"În anul 2000 î.Hr., scribii mesopotamieni numeau «Eme-bala» pe aceia care prefac limba în punte între lumi.",
    .about_link_website = L"Site web",
    .about_link_contact = L"Contact",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Program 10:00–19:00 KST",
    .about_contact_lead = L"Arhitect principal: Yongtai Kim",
    .tooltip_copied = L"✓ Copiat!",
    .tooltip_button_copy = L"📋 Copiază",
    .tooltip_button_tts = L"🔊 Voce",
    .menu_ui_language = L"Limba interfeței",
    .menu_ui_language_auto = L"Auto (limba sistemului)",
    .about_reset_button = L"Resetează la valorile implicite de sistem",
    .about_reset_done = L"Valori implicite restaurate",
    .menu_typing_group = L"Tastare de la tastatură",
    .menu_tooltip_group = L"Sfat de glisare",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Nu există o voce Windows instalată pentru această limbă. Faceți clic din nou pe 🔊 pentru a deschide setările Vocii.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Notificare de confidențialitate",
    .privacy_notice_body = L"Iată principiile de confidențialitate ale Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat nu operează niciun server propriu.\n"
    L"• Cu modelul local, textul tradus nu părăsește dispozitivul.\n"
    L"• Dacă alegi Google Translate sau se comută automat în cloud, textul selectat sau tastat este trimis direct la Google pentru traducere, fără a trece prin Emebala.\n"
    L"• Jurnalele de diagnostic sunt IMPLICIT DEZACTIVATE; le activezi din setări (consimțământ).\n"
    L"• Dacă nu doriți să trimiteți text în cloud (Google), deschideți meniul pictogramei din bara de sistem, alegeți “Motor de traducere” și apoi “Motor local încorporat”. Fără model local instalat și cu preluarea în cloud dezactivată, traducerea nu rulează — nimic nu este trimis.\n"
    L"\n"
    L"Detaliile complete sunt în fișierul README, recitibil oricând.\n",
    .cheatsheet_config_path = L"Fișier de configurare: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Textul era prea lung: au fost traduse doar începutul și sfârșitul.",
    .tooltip_untranslated_above = L"Există text nou netradus mai sus. Plasați cursorul la sfârșitul acelui rând și apăsați Enter pentru a traduce.",
    .repair_in_progress = L"Se repară componentele motorului local…",
    .repair_failed_title = L"Traducerea locală nu este disponibilă",
    .repair_failed_body = L"Fișierele motorului de traducere local lipsesc, deci traducerea este întreruptă. Reinstalați Emebala Chat pentru a restabili motorul local, sau treceți la traducerea în cloud (Google) alegând „Google Translate” din meniul barei de sistem, „Motor de traducere”.",
    .menu_engine_openai = L"Compatibil OpenAI (server personalizat)…",
    .openai_settings_title = L"Setări motor compatibil OpenAI",
    .openai_settings_action = L"Setări motor compatibil OpenAI…",
    .openai_base_url_label = L"URL de bază",
    .openai_api_key_label = L"Cheie API",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Preia lista de modele",
    .openai_fetch_failed = L"Lista de modele nu a putut fi preluată. Puteți tasta direct numele modelului.",
    .openai_http_warning_title = L"Conexiune nesecurizată (HTTP)",
    .openai_http_warning_body = L"URL-ul de bază folosește HTTP (necriptat). Cheia API și textul dvs. vor fi trimise în clar. Continuați?",
    .openai_saved = L"Setări compatibile OpenAI salvate.",
    .openai_key_masked = L"Cheie salvată: ",
    .openai_invalid_base_url = L"URL-ul de bază nu este valid. Exemplu: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Model specificat de utilizator (.gguf)",
    .menu_browse_gguf_file = L"Înregistrează alt model .gguf…",
    .user_gguf_quality_title = L"Notă despre calitatea traducerii",
    .user_gguf_quality_body = L"Modelul selectat nu este Hy-MT2. Versiunea actuală folosește promptul exclusiv Hy-MT2, deci calitatea traducerii cu acest model nu este garantată. Continuați?",
    .user_gguf_registered_title = L"Model înregistrat",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Modelul selectat a fost înregistrat la motorul local.\n"
    L"\n"
    L"Acest model este folosit pentru traducere când alegeți \"Selectarea motorului de traducere > Alegerea utilizatorului (.gguf)\".\n"
    L"\n"
    L"Când intră în vigoare: cel mult aproximativ 1 minut după înregistrare (după ieșirea motorului din inactivitate)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Acest model este deja integrat în Emebala Chat. Înregistrarea nu este necesară. Poți selecta direct motorul de traducere local integrat.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(neregistrat)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Gestionare modele…",
    .gguf_manager_title = L"Manager modele utilizator",
    .gguf_manager_empty = L"Nu există modele de utilizator înregistrate.",
    .gguf_manager_rename = L"Redenumește…",
    .gguf_manager_delete = L"Șterge…",
    .gguf_manager_close = L"Închide",
    .gguf_manager_delete_confirm_title = L"Ștergere înregistrare model",
    .gguf_manager_delete_confirm_body = L"Înregistrarea modelului selectat va fi ștearsă.\n"
    L"\n"
    L"Fișierul modelului (.gguf) rămâne pe disc și nu este șters. Dacă acest model era utilizat, selectarea motorului de traducere revine la Automat (Auto).\n"
    L"\n"
    L"Continuați?",
    .gguf_manager_rename_title = L"Redenumește modelul",
    .gguf_manager_rename_body = L"Introduceți un nume nou (max. 64 de caractere, fără spații).",
    .gguf_manager_rename_invalid = L"Acest nume nu poate fi folosit. Introduceți un nume nevid, diferit de cele existente, fără spații sau separatoare de cale, de maximum 64 de caractere.",
    .gguf_manager_done = L"Modificări salvate.",
    .gguf_manager_err_serialize = L"registry.json nu a putut fi serializat (un nume de fișier a fost respins). Nu s-a modificat nimic.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% nu este disponibil; nu se poate localiza directorul de modele partajat.",
    .gguf_manager_err_write = L"registry.json nu a putut fi scris.",
    .gguf_manager_err_write_partial = L"registry.json nu a putut fi scris complet.",
    .gguf_manager_err_registry_damaged = L"registry.json este deteriorat sau are un schemă neacceptată. NU a fost modificat. Reparați-l sau eliminați-l și încercați din nou.",
    .tooltip_translate_failed = L"Traducerea a eșuat. Încercați din nou.",
    .repair_transient_body = L"Motorul de traducere local este temporar indisponibil (se poate inițializa). Așteptați un moment și încercați din nou.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Confirmare",
    .dialog_cancel = L"Anulare",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Adaugă din fișier…",
    .gguf_manager_add_hf = L"Adaugă de pe Hugging Face…",
    .hf_add_title = L"Adaugă model de pe Hugging Face",
    .hf_url_label = L"URL-ul modelului",
    .hf_downloading = L"Se descarcă…",
    .hf_failed = L"Descărcarea a eșuat.",
    .hf_invalid_url = L"Introduceți URL-ul modelului de pe huggingface.co (resolve, blob sau pagina modelului)",
    .hf_done = L"Înregistrat.",
    .openai_base_url_hint = L"ex.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Cheie API",
    // REQ-051: buton [Șterge setările].
    .openai_delete_settings = L"Șterge setările",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Verifică actualizări",
    .update_idle_note = L"Vedeți dacă rulați cea mai recentă versiune de Emebala Chat.",
    .update_checking = L"Se verifică actualizările…",
    .update_checking_hint = L"Rulează în fundal; puteți închide această fereastră.",
    .update_uptodate = L"Utilizați cea mai recentă versiune",
    .update_dlg_title = L"Actualizare disponibilă",
    .update_dlg_version = L"Versiune nouă",
    .update_dlg_version_cur = L"aveți {v}",
    .update_dlg_size = L"Dimensiune",
    .update_dlg_size_approx = L"aproximativ {s}",
    .update_dlg_privacy = L"Verificarea și descărcarea se conectează la GitHub.",
    .update_dlg_download = L"Descarcă",
    .update_dlg_later = L"Mai târziu",
    .update_dl_progress = L"Se descarcă…",
    .update_dl_cancel = L"Anulează",
    .update_dl_cancelled = L"Descărcare anulată.",
    .update_ready_title = L"Descărcare finalizată — gata de instalare",
    .update_ready_warn = L"Aplicația se va închide pentru instalare.",
    .update_ready_install = L"Instalează acum",
    .update_ready_later = L"Mai târziu",
    .update_err_offline = L"Sunteți offline. Verificați conexiunea, apoi reîncercați.",
    .update_err_rate = L"Prea multe solicitări. Reîncercați peste puțin timp.",
    .update_err_hash = L"Fișierul descărcat nu a trecut verificarea.",
    .update_err_retry = L"Reîncearcă",
    .update_snoozed_line = L"Puteți instala actualizarea mai târziu.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Descărcare model",
    .model_dl_consent_body = L"Modelul de traducere integrat nu este instalat. Îl descărcați acum? (aproximativ {s})",
    .model_dl_privacy = L"Descărcarea se conectează la huggingface.co.",
    .model_dl_download = L"Descarcă acum",
    .model_dl_verifying = L"Se verifică descărcarea…",
    .model_dl_placing = L"Se instalează modelul…",
    .model_dl_done = L"Modelul este gata. Motorul local îl va folosi de la următoarea traducere.",
    .model_dl_failed = L"Descărcarea a eșuat. Verificați conexiunea și încercați din nou.",
    .model_dl_disk_space = L"Spațiu liber insuficient pe disc (necesari aproximativ {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Setări pentru încărcarea modelului de traducere…",
    .engine_load_settings_title = L"Încărcarea modelului de traducere",
    .load_mode_on_use = L"Încarcă la utilizare (recomandat)",
    .load_mode_warmup = L"Preîncarcă",
    .load_mode_on_use_desc = L"Economisește memorie. Prima traducere poate începe cu câteva secunde mai târziu.",
    .load_mode_warmup_desc = L"Traducerea începe imediat. Modelul rămâne în memorie.",
    .gguf_policy_sticky = L"(Păstrează rezident)",
    .gguf_policy_evict = L"(Încarcă la utilizare)",
    .gguf_manager_toggle_policy = L"Comută politica",
    .gguf_duplicate_warning = L"Fișiere duplicate: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Curăță duplicatele",
    .gguf_manager_dedupe_done = L"Fișierele duplicate au fost mutate în Coșul de gunoi.",
    .gguf_manager_dedupe_failed = L"Fișierele duplicate nu au putut fi curățate. Verificați dacă alt program le utilizează.",
    .menu_ui_theme = L"Temă interfață",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editorial cald",
    .theme_monolith = L"Monolit rafinat",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Afișare traducere inversă flotantă (verificare)",
    .back_translate_header = L"🔄 Verificare traducere inversă",
    .back_translate_match_exact = L"✓ Sens identic",
    .back_translate_match_check = L"⚡ Verifică nuanța",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Acesta nu este un fișier GGUF valid; nu a fost înregistrat.",
    .gguf_manager_active_suffix = L"(în uz)",
    .gguf_manager_activated = L"Acest model este acum în uz.",
};
// 18. Swedish (sv)
const LocalizedStrings kStringsSwedish = {
    .menu_status_active = L"Status: aktiv (F9: paus)",
    .menu_status_paused = L"Status: pausad (F9: återuppta)",
    .menu_engine = L"Översättningsmotor",
    .menu_engine_google = L"Google Översätt (gratis / utan installation)",
    .menu_engine_local = L"Inbyggd lokal motor (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Källspråk (inmatning)",
    .menu_target_lang = L"Målspråk (utmatning)",
    .menu_swap_langs = L"Byt källa ⇄ mål (dubbelklick)",
    .menu_auto_send = L"Skicka automatiskt med Enter",
    .menu_sound_feedback = L"Ljudåterkoppling (toner)",
    .menu_show_badge = L"Flytande bricka synlig",
    .menu_start_with_windows = L"Starta med Windows",
    .menu_cheatsheet = L"Snabbtangentsguide och hjälp...",
    .menu_exit = L"Avsluta Emebala Chat",
    .menu_about = L"Om Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — snabbtangenter och guide",
    .cheatsheet_body = L"Snabbtangenter och guide för Emebala Chat:\n\n"
    L"  • F9 : aktivera / pausa\n"
    L"  • Ctrl + F9 : byt målspråk\n"
    L"  • Ctrl + Shift + Enter : växla automatiskt sändande\n"
    L"  • Shift + Enter : översätt och skicka direkt\n\n"
    L"Musstyrning på brickan:\n"
    L"  • Vänsterklick : aktivera / pausa\n"
    L"  • Dubbelklick : byt källa ⇄ mål\n"
    L"  • Högerklick : öppna inställningsmenyn\n\n"
    L"Översättningslägen:\n"
    L"  • Endast ersätt (autosänd av): ersätter raden med översättningen för granskning.\n"
    L"  • Autosänd (på): ersätter raden och trycker direkt på Enter.",
    .about_title = L"Om Emebala Chat",
    .badge_active = L"Aktiv",
    .badge_translating = L"Översätter...",
    .badge_paused = L"Pausad",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Kunde inte kopiera den markerade texten. Kontrollera målappen och försök igen.",
    .tooltip_no_selection = L"Ingen text är markerad att översätta.",
    .auto_detect = L"Identifiera automatiskt",
    .app_already_running = L"Emebala Chat körs redan i bakgrunden.\nKontrollera meddelandefältet.",
    .app_com_failed = L"COM-initieringen misslyckades.\nDen flytande brickan och uppläsning är inte tillgängliga,\nmen översättning, snabbtangenter, aktivitetsfält och ljud fungerar fortfarande.",
    .about_tagline = L"Inget mer kopierande. Skriv naturligt på ditt modersmål — översättningen ersätter din inskrift i realtid i alla Windows-appar.",
    .about_feature0 = L"⚡ Dra-för-att-översätta — markera text i vilken app som helst, den flytande ikonen översätter direkt.",
    .about_feature1 = L"🔊 Neuronalt TTS — talar alla 37 språk via installerade Windows-röstpaket.",
    .about_feature2 = L"🔒 100 % på enheten och privat — aktivt endast medan snabbtangenten hås; urklipp orört.",
    .about_etymology = L"År 2000 f.Kr. kallade mesopotamiska skribenter «Eme-bala» — dem som gör språket till en bro mellan världar.",
    .about_link_website = L"Webbplats",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Kontorstider 10:00–19:00 KST",
    .about_contact_lead = L"Leadande arkitekt: Yongtai Kim",
    .tooltip_copied = L"✓ Kopierad!",
    .tooltip_button_copy = L"📋 Kopiera",
    .tooltip_button_tts = L"🔊 Röst",
    .menu_ui_language = L"Gränssnittsspråk",
    .menu_ui_language_auto = L"Auto (systemspråk)",
    .about_reset_button = L"Återställ till systemstandard",
    .about_reset_done = L"Standardvärden återställda",
    .menu_typing_group = L"Tangentbordsskrivning",
    .menu_tooltip_group = L"Dra-tooltip",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Ingen Windows-röst är installerad för det här språket. Klicka på 🔊 igen för att öppna Tal-inställningarna.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Integritetsinformation",
    .privacy_notice_body = L"Så här hanterar Emebala Chat din integritet.\n"
    L"\n"
    L"• Emebala Chat driver inga egna servrar.\n"
    L"• Med lokal modell lämnar den översatta texten aldrig enheten.\n"
    L"• Om du väljer Google Översätt, eller om molnläge aktiveras automatiskt, skickas den markerade eller inmatade texten direkt till Google för översättning – inte via Emebala.\n"
    L"• Diagnossloggar är AV som standard; du slår på dem i inställningarna (opt-in).\n"
    L"• Om du inte vill skicka text till molnet (Google) öppnar du menyn från aktivitetsikonet, väljer “Översättningsmotor” och sedan “Inbyggd lokal motor”. Utan lokal modell och med molnfallback inaktiverat körs ingen översättning — ingenting skickas.\n"
    L"\n"
    L"Alla detaljer finns i README-filen, som du kan läsa om när som helst.\n",
    .cheatsheet_config_path = L"Konfigurationsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Texten var för lång: endast början och slutet översattes.",
    .tooltip_untranslated_above = L"Det finns ny oöversatt text ovanför. Placera markören i slutet av den raden och tryck på Enter för att översätta.",
    .repair_in_progress = L"Reparerar de lokala motorkomponenterna…",
    .repair_failed_title = L"Lokal översättning är inte tillgänglig",
    .repair_failed_body = L"Filerna för den lokala översättningsmotorn saknas, så översättningen har pausats. Installera om Emebala Chat för att återställa den lokala motorn, eller byt till molnöversättning (Google) genom att välja “Google Översätt” i menyn för systemfältet, “Översättningsmotor”.",
    .menu_engine_openai = L"OpenAI-kompatibel (anpassad server)…",
    .openai_settings_title = L"Inställningar för OpenAI-kompatibel motor",
    .openai_settings_action = L"Inställningar för OpenAI-kompatibel motor…",
    .openai_base_url_label = L"Bas-URL",
    .openai_api_key_label = L"API-nyckel",
    .openai_model_label = L"Modell",
    .openai_fetch_models = L"Hämta modellista",
    .openai_fetch_failed = L"Modellistan kunde inte hämtas. Du kan skriva modellnamnet direkt.",
    .openai_http_warning_title = L"Oskyddad anslutning (HTTP)",
    .openai_http_warning_body = L"Bas-URL:en använder HTTP (okrypterat). Din API-nyckel och text skickas i klartext. Fortsätt?",
    .openai_saved = L"OpenAI-kompatibla inställningar sparade.",
    .openai_key_masked = L"Sparad nyckel: ",
    .openai_invalid_base_url = L"Bas-URL:en är ogiltig. Exempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Användarspecificerad modell (.gguf)",
    .menu_browse_gguf_file = L"Registrera en annan .gguf-modell…",
    .user_gguf_quality_title = L"Meddelande om översättningskvalitet",
    .user_gguf_quality_body = L"Den valda modellen är inte Hy-MT2. Den aktuella versionen använder prompten endast för Hy-MT2, så översättningskvaliteten med den här modellen garanteras inte. Fortsätt?",
    .user_gguf_registered_title = L"Modell registrerad",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Den valda modellen har registrerats hos den lokala motorn.\n"
    L"\n"
    L"Den här modellen används för översättning när du väljer \"Välj översättningsmotor > Användarval (.gguf)\".\n"
    L"\n"
    L"När den träder i kraft: högst cirka 1 minut efter registreringen (efter att motorn avslutats vid inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Den här modellen är redan inbyggd i Emebala Chat. Ingen registrering behövs. Välj den inbyggda lokala översättningsmotorn direkt.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ej registrerad)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Hantera modeller…",
    .gguf_manager_title = L"Hanterare av användarmodeller",
    .gguf_manager_empty = L"Inga användarmodeller registrerade.",
    .gguf_manager_rename = L"Byt namn…",
    .gguf_manager_delete = L"Ta bort…",
    .gguf_manager_close = L"Stäng",
    .gguf_manager_delete_confirm_title = L"Ta bort modellregistrering",
    .gguf_manager_delete_confirm_body = L"Registreringen av den valda modellen tas bort.\n"
    L"\n"
    L"Modellfilen (.gguf) kvarstår på disken och tas inte bort. Om den här modellen användes återgår valet av översättningsmotor till Automatiskt (Auto).\n"
    L"\n"
    L"Fortsätta?",
    .gguf_manager_rename_title = L"Byt namn på modell",
    .gguf_manager_rename_body = L"Ange ett nytt namn (max 64 tecken, utan mellanslag).",
    .gguf_manager_rename_invalid = L"Det namnet kan inte användas. Ange ett icke-tomt namn som skiljer sig från befintliga, utan mellanslag eller sökvägsavgränsare, på högst 64 tecken.",
    .gguf_manager_done = L"Ändringar sparade.",
    .gguf_manager_err_serialize = L"registry.json kunde inte serialiseras (ett filnamn avvisades). Inget har ändrats.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% är inte tillgängligt; den delade modellmappen kunde inte hittas.",
    .gguf_manager_err_write = L"registry.json kunde inte skrivas.",
    .gguf_manager_err_write_partial = L"registry.json kunde inte skrivas fullständigt.",
    .gguf_manager_err_registry_damaged = L"registry.json är skadad eller har ett schema som inte stöds. Den har INTE ändrats. Reparera eller ta bort den och försök igen.",
    .tooltip_translate_failed = L"Översättningen misslyckades. Försök igen.",
    .repair_transient_body = L"Den lokala översättningsmotorn är tillfälligt otillgänglig (den kan starta). Vänta en stund och försök igen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Bekräfta",
    .dialog_cancel = L"Avbryt",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Lägg till från fil…",
    .gguf_manager_add_hf = L"Lägg till från Hugging Face…",
    .hf_add_title = L"Lägg till en Hugging Face-modell",
    .hf_url_label = L"Modell-URL",
    .hf_downloading = L"Laddar ner…",
    .hf_failed = L"Nedladdningen misslyckades.",
    .hf_invalid_url = L"Ange modell-URL från huggingface.co (resolve, blob eller modellsida)",
    .hf_done = L"Registrerad.",
    .openai_base_url_hint = L"t.ex. https://api.openai.com/v1",
    .openai_api_key_hint = L"API-nyckel",
    // REQ-051: knapp [Ta bort inställningar].
    .openai_delete_settings = L"Ta bort inställningar",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Sök efter uppdateringar",
    .update_idle_note = L"Se om du kör den senaste versionen av Emebala Chat.",
    .update_checking = L"Söker efter uppdateringar…",
    .update_checking_hint = L"Körs i bakgrunden; du kan stänga det här fönstret.",
    .update_uptodate = L"Du har den senaste versionen",
    .update_dlg_title = L"Uppdatering tillgänglig",
    .update_dlg_version = L"Ny version",
    .update_dlg_version_cur = L"du har {v}",
    .update_dlg_size = L"Storlek",
    .update_dlg_size_approx = L"cirka {s}",
    .update_dlg_privacy = L"Kontroll och hämtning ansluter till GitHub.",
    .update_dlg_download = L"Ladda ner",
    .update_dlg_later = L"Senare",
    .update_dl_progress = L"Laddar ner…",
    .update_dl_cancel = L"Avbryt",
    .update_dl_cancelled = L"Nedladdningen avbröts.",
    .update_ready_title = L"Nedladdning klar — redo att installera",
    .update_ready_warn = L"Appen stängs för att installera.",
    .update_ready_install = L"Installera nu",
    .update_ready_later = L"Senare",
    .update_err_offline = L"Du är offline. Kontrollera din anslutning och försök igen.",
    .update_err_rate = L"För många förfrågningar. Försök igen om en stund.",
    .update_err_hash = L"Den nedladdade filen klarade inte verifieringen.",
    .update_err_retry = L"Försök igen",
    .update_snoozed_line = L"Du kan installera uppdateringen senare.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Ladda ner modell",
    .model_dl_consent_body = L"Den inbyggda översättningsmodellen är inte installerad. Ladda ner den nu? (cirka {s})",
    .model_dl_privacy = L"Nedladdningen ansluter till huggingface.co.",
    .model_dl_download = L"Ladda ner nu",
    .model_dl_verifying = L"Verifierar nedladdningen…",
    .model_dl_placing = L"Installerar modellen…",
    .model_dl_done = L"Modellen är klar. Den lokala motorn använder den från nästa översättning.",
    .model_dl_failed = L"Nedladdningen misslyckades. Kontrollera din anslutning och försök igen.",
    .model_dl_disk_space = L"Inte tillräckligt med ledigt diskutrymme (cirka {s} krävs).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Inställningar för laddning av översättningsmodell…",
    .engine_load_settings_title = L"Laddning av översättningsmodell",
    .load_mode_on_use = L"Ladda vid användning (rekommenderas)",
    .load_mode_warmup = L"Förhandsladda",
    .load_mode_on_use_desc = L"Sparar minne. Den första översättningen kan börja några sekunder senare.",
    .load_mode_warmup_desc = L"Översättningen startar direkt. Modellen stannar i minnet.",
    .gguf_policy_sticky = L"(Håll kvar i minnet)",
    .gguf_policy_evict = L"(Ladda vid användning)",
    .gguf_manager_toggle_policy = L"Byt princip",
    .gguf_duplicate_warning = L"Dubblerade filer: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Rensa dubbletter",
    .gguf_manager_dedupe_done = L"De dubblerade filerna har flyttats till Papperskorgen.",
    .gguf_manager_dedupe_failed = L"De dubblerade filerna kunde inte rensas. Kontrollera om ett annat program använder dem.",
    .menu_ui_theme = L"UI-tema",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Varm Editorial",
    .theme_monolith = L"Förfinad monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Flytande återöversättning (verifiering)",
    .back_translate_header = L"🔄 Kontroll av återöversättning",
    .back_translate_match_exact = L"✓ Betydelsen stämmer",
    .back_translate_match_check = L"⚡ Kontrollera nyans",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Det är ingen giltig GGUF-modellfil; den har inte registrerats.",
    .gguf_manager_active_suffix = L"(används)",
    .gguf_manager_activated = L"Den här modellen används nu.",
};
// 19. Danish (da)
const LocalizedStrings kStringsDanish = {
    .menu_status_active = L"Status: aktiv (F9: pause)",
    .menu_status_paused = L"Status: pause (F9: genoptag)",
    .menu_engine = L"Oversættelsesmotor",
    .menu_engine_google = L"Google Oversæt (gratis / uden installation)",
    .menu_engine_local = L"Indbygget lokal motor (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Kildesprog (input)",
    .menu_target_lang = L"Formsprog (output)",
    .menu_swap_langs = L"Byt kilde ⇄ mål (dobbeltklik)",
    .menu_auto_send = L"Automatisk afsendelse med Enter",
    .menu_sound_feedback = L"Lydfeedback (toner)",
    .menu_show_badge = L"Flydende badge synlig",
    .menu_start_with_windows = L"Start med Windows",
    .menu_cheatsheet = L"Genvejs oversigt og hjælp...",
    .menu_exit = L"Afslut Emebala Chat",
    .menu_about = L"Om Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — genveje og guide",
    .cheatsheet_body = L"Genveje og guide til Emebala Chat:\n\n"
    L"  • F9 : aktiver / pause\n"
    L"  • Ctrl + F9 : skift formsprog\n"
    L"  • Ctrl + Shift + Enter : skift automatisk afsendelse\n"
    L"  • Shift + Enter : oversæt og send straks\n\n"
    L"Mus på badgen:\n"
    L"  • Venstreklik : aktiver / pause\n"
    L"  • Dobbeltklik : byt kilde ⇄ mål\n"
    L"  • Højreklik : åbn indstillingsmenuen\n\n"
    L"Oversættelsestilstande:\n"
    L"  • Kun erstatning (auto-send fra): erstatter linjen med oversættelsen til gennemse.\n"
    L"  • Auto-send (til): erstatter linjen og trykker straks Enter.",
    .about_title = L"Om Emebala Chat",
    .badge_active = L"Aktiv",
    .badge_translating = L"Oversætter...",
    .badge_paused = L"Pause",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Kunne ikke kopiere den markerede tekst. Tjek målappen, og prøv igen.",
    .tooltip_no_selection = L"Der er ikke markeret tekst at oversætte.",
    .auto_detect = L"Registrér automatisk",
    .app_already_running = L"Emebala Chat kører allerede i baggrunden.\nTjek notifikationsområdet.",
    .app_com_failed = L"COM-initialisering mislykkedes.\nDen flydende badge og oplæsning er utilgængelige,\nmen oversættelse, genveje, bakke og lyde fungerer stadig.",
    .about_tagline = L"Stop med at kopiere og indsætte. Skriv naturligt på dit modersmål — oversættelsen erstatter din indtastning i realtid i enhver Windows-app.",
    .about_feature0 = L"⚡ Træk-for-at-oversætte — markér tekst i en vilkårlig app, det flydende ikon oversætter med det samme.",
    .about_feature1 = L"🔊 Neuralt TTS — udtaler alle 37 sprog via installerede Windows-talepakker.",
    .about_feature2 = L"🔒 100 % på enheden og privat — aktiv kun mens genvejen holdes; udklipsholder urørt.",
    .about_etymology = L"I år 2000 f.Kr. kaldte mesopotamiske skriver «Eme-bala» — dem der gør sproget til en bro mellem verdener.",
    .about_link_website = L"Hjemmeside",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Åbningstider 10:00–19:00 KST",
    .about_contact_lead = L"Leadende arkitekt: Yongtai Kim",
    .tooltip_copied = L"✓ Kopieret!",
    .tooltip_button_copy = L"📋 Kopiér",
    .tooltip_button_tts = L"🔊 Læs op",
    .menu_ui_language = L"Grænsefladesprog",
    .menu_ui_language_auto = L"Auto (system sprog)",
    .about_reset_button = L"Nulstil til systemstandarder",
    .about_reset_done = L"Standarder gendannet",
    .menu_typing_group = L"Tastaturindtastning",
    .menu_tooltip_group = L"Drag-værktøjstip",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Der er ikke installeret en Windows-tale til dette sprog. Klik på 🔊 igen for at åbne Tale-indstillingerne.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Privatlivspolitik",
    .privacy_notice_body = L"Sådan behandler Emebala Chat dine data.\n"
    L"\n"
    L"• Emebala Chat driver ingen egen server.\n"
    L"• Ved lokal model forlader den oversatte tekst ikke din enhed.\n"
    L"• Hvis du vælger Google Translate, eller der skiftes automatisk til cloud, sendes den markerede eller indtastede tekst direkte til Google til oversættelse – ikke via Emebala.\n"
    L"• Diagnosedokumentation er SOM STANDARD FRA; du slår den til i indstillingerne (tilvalg).\n"
    L"• Hvis du ikke vil sende tekst til skyen (Google), skal du åbne menuen fra statusfeltikonet, vælge “Oversættelsesmotor” og derefter “Indbygget lokal motor”. Uden en lokal model og med cloud-backup deaktiveret kører oversættelsen ikke — intet sendes.\n"
    L"\n"
    L"Detaljerne står i README-filen, som du kan læse når som helst.\n",
    .cheatsheet_config_path = L"Konfigurationsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Teksten var for lang: kun starten og slutningen blev oversat.",
    .tooltip_untranslated_above = L"Der er ny uoversat tekst ovenfor. Placer markøren i slutningen af den linje, og tryk på Enter for at oversætte.",
    .repair_in_progress = L"Reparerer de lokale motorkomponenter…",
    .repair_failed_title = L"Lokal oversættelse er ikke tilgængelig",
    .repair_failed_body = L"Filerne til den lokale oversættelsesmotor mangler, så oversættelsen er sat på pause. Geninstaller Emebala Chat for at gendanne den lokale motor, eller skift til sky-oversættelse (Google) ved at vælge “Google Oversæt” fra bakkemenuen under “Oversættelsesmotor”.",
    .menu_engine_openai = L"OpenAI-kompatibel (brugerdefineret server)…",
    .openai_settings_title = L"Indstillinger for OpenAI-kompatibel motor",
    .openai_settings_action = L"Indstillinger for OpenAI-kompatibel motor…",
    .openai_base_url_label = L"Basis-URL",
    .openai_api_key_label = L"API-nøgle",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Hent modelliste",
    .openai_fetch_failed = L"Modellisten kunne ikke hentes. Du kan skrive modelnavnet direkte.",
    .openai_http_warning_title = L"Usikker forbindelse (HTTP)",
    .openai_http_warning_body = L"Basis-URL'en bruger HTTP (ukrypteret). Din API-nøgle og tekst sendes i klartekst. Fortsæt?",
    .openai_saved = L"OpenAI-kompatible indstillinger gemt.",
    .openai_key_masked = L"Gemt nøgle: ",
    .openai_invalid_base_url = L"Basis-URL'en er ugyldig. Eksempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Brugerspecificeret model (.gguf)",
    .menu_browse_gguf_file = L"Registrér en anden .gguf-model…",
    .user_gguf_quality_title = L"Meddelelse om oversættelseskvalitet",
    .user_gguf_quality_body = L"Den valgte model er ikke Hy-MT2. Den aktuelle version bruger kun prompten til Hy-MT2, så oversættelseskvaliteten med denne model kan ikke garanteres. Fortsæt?",
    .user_gguf_registered_title = L"Model registreret",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Den valgte model er registreret hos den lokale motor.\n"
    L"\n"
    L"Denne model bruges til oversættelse, når du vælger \"Valg af oversættelsesmotor > Brugervalgt (.gguf)\".\n"
    L"\n"
    L"Hvornår den træder i kraft: senest ca. 1 minut efter registreringen (efter motorens afslutning ved inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Denne model er allerede indbygget i Emebala Chat. Registrering er ikke nødvendig. Vælg den indbyggede lokale oversættelsesmotor direkte.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ikke registreret)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Administrer modeller…",
    .gguf_manager_title = L"Administration af brugermodeller",
    .gguf_manager_empty = L"Ingen brugermodeller registreret.",
    .gguf_manager_rename = L"Omdøb…",
    .gguf_manager_delete = L"Slet…",
    .gguf_manager_close = L"Luk",
    .gguf_manager_delete_confirm_title = L"Slet modelregistrering",
    .gguf_manager_delete_confirm_body = L"Registreringen af den valgte model slettes.\n"
    L"\n"
    L"Modelfilen (.gguf) beholdes på disken og slettes ikke. Hvis denne model var i brug, vender valget af oversættelsesmotor tilbage til Automatisk (Auto).\n"
    L"\n"
    L"Fortsæt?",
    .gguf_manager_rename_title = L"Omdøb model",
    .gguf_manager_rename_body = L"Indtast et nyt navn (maks. 64 tegn, uden mellemrum).",
    .gguf_manager_rename_invalid = L"Det navn kan ikke bruges. Indtast et ikke-tomt navn, der adskiller sig fra eksisterende, uden mellemrum eller stiangrænsesymboler, på højst 64 tegn.",
    .gguf_manager_done = L"Ændringer gemt.",
    .gguf_manager_err_serialize = L"registry.json kunne ikke serialiseres (et filnavn blev afvist). Intet blev ændret.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% er ikke tilgængelig; den delte modelmappe kunne ikke findes.",
    .gguf_manager_err_write = L"registry.json kunne ikke skrives.",
    .gguf_manager_err_write_partial = L"registry.json kunne ikke skrives færdig.",
    .gguf_manager_err_registry_damaged = L"registry.json er beskadiget eller har et ikke-understøttet skema. Den er IKKE ændret. Reparér eller fjern den, og prøv igen.",
    .tooltip_translate_failed = L"Oversættelsen mislykkedes. Prøv igen.",
    .repair_transient_body = L"Den lokale oversættelsesmotor er midlertidigt utilgængelig (den er muligvis ved at starte). Vent et øjeblik og prøv igen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Bekræft",
    .dialog_cancel = L"Annuller",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Tilføj fra fil…",
    .gguf_manager_add_hf = L"Tilføj fra Hugging Face…",
    .hf_add_title = L"Tilføj en Hugging Face-model",
    .hf_url_label = L"Model-URL",
    .hf_downloading = L"Download i gang…",
    .hf_failed = L"Download mislykkedes.",
    .hf_invalid_url = L"Indtast model-URL'en fra huggingface.co (resolve, blob eller modelside)",
    .hf_done = L"Registreret.",
    .openai_base_url_hint = L"f.eks. https://api.openai.com/v1",
    .openai_api_key_hint = L"API-nøgle",
    // REQ-051: knap [Slet indstillinger].
    .openai_delete_settings = L"Slet indstillinger",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Søg efter opdateringer",
    .update_idle_note = L"Se, om du kører den nyeste version af Emebala Chat.",
    .update_checking = L"Søger efter opdateringer…",
    .update_checking_hint = L"Kører i baggrunden; du kan lukke dette vindue.",
    .update_uptodate = L"Du har den nyeste version",
    .update_dlg_title = L"Opdatering tilgængelig",
    .update_dlg_version = L"Ny version",
    .update_dlg_version_cur = L"du har {v}",
    .update_dlg_size = L"Størrelse",
    .update_dlg_size_approx = L"ca. {s}",
    .update_dlg_privacy = L"Kontrol og download opretter forbindelse til GitHub.",
    .update_dlg_download = L"Download",
    .update_dlg_later = L"Senere",
    .update_dl_progress = L"Download i gang…",
    .update_dl_cancel = L"Annullér",
    .update_dl_cancelled = L"Download annulleret.",
    .update_ready_title = L"Download fuldført — klar til at installere",
    .update_ready_warn = L"Appen lukkes for at installere.",
    .update_ready_install = L"Installer nu",
    .update_ready_later = L"Senere",
    .update_err_offline = L"Du er offline. Tjek din forbindelse, og prøv igen.",
    .update_err_rate = L"For mange anmodninger. Prøv igen om lidt.",
    .update_err_hash = L"Den downloadede fil bestod ikke verificeringen.",
    .update_err_retry = L"Prøv igen",
    .update_snoozed_line = L"Du kan installere opdateringen senere.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Download model",
    .model_dl_consent_body = L"Den indbyggede oversættelsesmodel er ikke installeret. Download nu? (ca. {s})",
    .model_dl_privacy = L"Download opretter forbindelse til huggingface.co.",
    .model_dl_download = L"Download nu",
    .model_dl_verifying = L"Verificerer download…",
    .model_dl_placing = L"Installerer modellen…",
    .model_dl_done = L"Modellen er klar. Den lokale motor bruger den fra næste oversættelse.",
    .model_dl_failed = L"Download mislykkedes. Tjek din forbindelse, og prøv igen.",
    .model_dl_disk_space = L"Ikke nok ledig diskplads (ca. {s} påkrævet).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Indstillinger for indlæsning af oversættelsesmodellen…",
    .engine_load_settings_title = L"Indlæsning af oversættelsesmodel",
    .load_mode_on_use = L"Indlæs ved brug (anbefalet)",
    .load_mode_warmup = L"Forindlæs",
    .load_mode_on_use_desc = L"Sparer hukommelse. Den første oversættelse kan starte et par sekunder senere.",
    .load_mode_warmup_desc = L"Oversættelsen starter med det samme. Modellen bliver i hukommelsen.",
    .gguf_policy_sticky = L"(Hold indlæst)",
    .gguf_policy_evict = L"(Indlæs ved brug)",
    .gguf_manager_toggle_policy = L"Skift politik",
    .gguf_duplicate_warning = L"Dublerede filer: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Rens dubletter",
    .gguf_manager_dedupe_done = L"De dublerede filer er flyttet til Papirkurven.",
    .gguf_manager_dedupe_failed = L"De dublerede filer kunne ikke renses. Tjek, om et andet program bruger dem.",
    .menu_ui_theme = L"UI-tema",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Varm Editorial",
    .theme_monolith = L"Finpudset monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Flydende tilbagetranslation (kontrol)",
    .back_translate_header = L"🔄 Kontrol af tilbagetranslation",
    .back_translate_match_exact = L"✓ Betydningen stemmer",
    .back_translate_match_check = L"⚡ Kontroller nuance",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Det er ikke en gyldig GGUF-modelfil; den er ikke registreret.",
    .gguf_manager_active_suffix = L"(i brug)",
    .gguf_manager_activated = L"Denne model er nu i brug.",
};
// 20. Finnish (fi)
const LocalizedStrings kStringsFinnish = {
    .menu_status_active = L"Tila: aktiivinen (F9: tauko)",
    .menu_status_paused = L"Tila: keskeytetty (F9: jatka)",
    .menu_engine = L"Käännösmoottori",
    .menu_engine_google = L"Google Kääntäjä (ilmainen / ei asennusta)",
    .menu_engine_local = L"Sisäänrakennettu paikallinen moottori (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Lähd kieli (syöte)",
    .menu_target_lang = L"Koh kieli (tuloste)",
    .menu_swap_langs = L"Vaihda lähde ⇄ kohde (kaksoiskautistus)",
    .menu_auto_send = L"Automaattinen lähetys Enterillä",
    .menu_sound_feedback = L"Äänipalaute (sävelmät)",
    .menu_show_badge = L"Kelluva merkki näkyy",
    .menu_start_with_windows = L"Käynnistä Windowsin mukana",
    .menu_cheatsheet = L"Pikanäppäinopas ja ohje...",
    .menu_exit = L"Sulje Emebala Chat",
    .menu_about = L"Tietoja Emebala Chatista…",
    .cheatsheet_title = L"Emebala Chat — pikanäppäimet ja opas",
    .cheatsheet_body = L"Emebala Chatin pikanäppäimet ja käyttöopas:\n\n"
    L"  • F9 : ota käyttöön / tauko\n"
    L"  • Ctrl + F9 : vaihda koh kieltä\n"
    L"  • Ctrl + Shift + Enter : automaattinen lähetys päälle/pois\n"
    L"  • Shift + Enter : käännä ja lähetä heti\n\n"
    L"Hiiren ohjaus merkin päällä:\n"
    L"  • Vasen painallus : ota käyttöön / tauko\n"
    L"  • Kaksoiskautistus : vaihda lähde ⇄ kohde\n"
    L"  • Oikea painallus : avaa asetusvalikko\n\n"
    L"Käännöstilat:\n"
    L"  • Vain korvaus (automaattilähetys pois): korvaa rivin käännöksellä tarkistettavaksi.\n"
    L"  • Automaattinen lähetys (päällä): korvaa rivin ja painaa heti Enteriä.",
    .about_title = L"Tietoja Emebala Chatista",
    .badge_active = L"Aktiivinen",
    .badge_translating = L"Kääntää...",
    .badge_paused = L"Keskeytetty",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Valitun tekstin kopiointi epäonnistui. Tarkista kohdesovellus ja yritä uudelleen.",
    .tooltip_no_selection = L"Käännettävää tekstiä ei ole valittu.",
    .auto_detect = L"Tunnista automaattisesti",
    .app_already_running = L"Emebala Chat on jo käynnissä taustalla.\nTarkista ilmoitusalue.",
    .app_com_failed = L"COM-käynnistys epäonnistui.\nKelluva merkki ja puheentuotto eivät ole käytettävissä,\nmutta käännös, pikanäppäimet, palkki ja äänet toimivat edelleen.",
    .about_tagline = L"Liitä kopiointi ja liittäminen luonnollisesti aidolla kielelläsi — käännös korvaa kirjoituksesi reaaliajassa missä tahansa Windows-sovelluksessa.",
    .about_feature0 = L"⚡ Vedä kääntääksesi — valitse teksti missä tahansa sovelluksessa, kelluva kuvake kääntää heti.",
    .about_feature1 = L"🔊 Neuro TTS — puhuu kaikki 37 kieltä asennettujen Windows-äänipakettien avulla.",
    .about_feature2 = L"🔒 100 % laitteella ja yksityinen — aktiivinen vain pikanäppäintä pidettäessä; leikepöytää ei kosketa.",
    .about_etymology = L"Vuonna 2000 eaa. mesopotamialaiset kirjurit kutsuivat kielen maailmat sillaksi kääntäviä «Eme-bala».",
    .about_link_website = L"Verkkosivusto",
    .about_link_contact = L"Yhteys",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Toimistoaika 10:00–19:00 KST",
    .about_contact_lead = L"Johtava arkkitehti: Yongtai Kim",
    .tooltip_copied = L"✓ Kopioitu!",
    .tooltip_button_copy = L"📋 Kopioi",
    .tooltip_button_tts = L"🔊 Puhe",
    .menu_ui_language = L"Käyttöliittymän kieli",
    .menu_ui_language_auto = L"Auto (järjestelmän kieli)",
    .about_reset_button = L"Palauta järjestelmän oletukset",
    .about_reset_done = L"Oletukset palautettu",
    .menu_typing_group = L"Näppäimistökirjoitus",
    .menu_tooltip_group = L"Veto-työkaluvihje",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Tälle kielelle ei ole asennettu Windows-ääntä. Napsauta 🔊 uudelleen avataksesi Puhe-asetukset.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Tietosuojailmoitus",
    .privacy_notice_body = L"Nämä ovat Emebala Chatin tietosuojaperiaatteet.\n"
    L"\n"
    L"• Emebala Chatilla ei ole omia palvelimia.\n"
    L"• Paikallista mallia käytettäessä käännettävä teksti ei poistu laitteesta.\n"
    L"• Jos valitset Google-kääntäjän tai tila vaihtuu automaattisesti pilveen, valittu tai kirjoitettu teksti lähetetään suoraan Googlelle käännöstä varten – ei Emebalan kautta.\n"
    L"• Vianmäärityslokit ovat oletuksena POIS PAALTA; ota ne käyttöön asetuksissa (valinta).\n"
    L"• Jos et halua lähettää tekstiä pilveen (Google), avaa tehtäväpalkin kuvakkeen valikko, valitse “Käännösmoottori” ja sitten “Sisäänrakennettu paikallinen moottori”. Ilman paikallista mallia ja kun pilvivarajärjestelmä on pois päältä, käännös ei toimi — mitään ei lähetetä.\n"
    L"\n"
    L"Tarkat tiedot ovat README-tiedostossa, jonka voit lukea milloin tahansa.\n",
    .cheatsheet_config_path = L"Asetustiedosto: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Teksti oli liian pitkä: vain alku ja loppu käännettiin.",
    .tooltip_untranslated_above = L"Yläpuolella on uutta kääntämätöntä tekstiä. Aseta kohdistin rivin loppuun ja paina Enter kääntääksesi.",
    .repair_in_progress = L"Korjataan paikallisia moottorikomponentteja…",
    .repair_failed_title = L"Paikallinen käännös ei ole käytettävissä",
    .repair_failed_body = L"Paikallisen käännösmoottorin tiedostoja ei löydy, joten käännös on keskeytetty. Asenna Emebala Chat uudelleen palauttaaksesi paikallisen moottorin, tai vaihda pilvikäännökseen (Google) valitsemalla “Google Kääntäjä” ilmoitusalueen valikosta, “Käännösmoottori”.",
    .menu_engine_openai = L"OpenAI-yhteensopiva (mukautettu palvelin)…",
    .openai_settings_title = L"OpenAI-yhteensopivan moottorin asetukset",
    .openai_settings_action = L"OpenAI-yhteensopivan moottorin asetukset…",
    .openai_base_url_label = L"Perus-URL",
    .openai_api_key_label = L"API-avain",
    .openai_model_label = L"Malli",
    .openai_fetch_models = L"Hae malliluettelo",
    .openai_fetch_failed = L"Malliluetteloa ei voitu hakea. Voit kirjoittaa mallin nimen suoraan.",
    .openai_http_warning_title = L"Suojaamaton yhteys (HTTP)",
    .openai_http_warning_body = L"Perus-URL käyttää HTTP:tä (salaamaton). API-avaimesi ja tekstisi lähetetään salaamattomana. Jatketaanko?",
    .openai_saved = L"OpenAI-yhteensopivat asetukset tallennettu.",
    .openai_key_masked = L"Tallennettu avain: ",
    .openai_invalid_base_url = L"Perus-URL on virheellinen. Esimerkki: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Käyttäjän määrittämä malli (.gguf)",
    .menu_browse_gguf_file = L"Rekisteröi toinen .gguf-malli…",
    .user_gguf_quality_title = L"Huomautus käännöksen laadusta",
    .user_gguf_quality_body = L"Valittu malli ei ole Hy-MT2. Nykyinen versio käyttää vain Hy-MT2:lle tarkoitettua promptia, joten tämän mallin käännöksen laatua ei taata. Jatketaanko?",
    .user_gguf_registered_title = L"Malli rekisteröity",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Valittu malli on rekisteröity paikalliseen moottoriin.\n"
    L"\n"
    L"Tätä mallia käytetään kääntämiseen, kun valitset \"Käännösmoottorin valinta > Käyttäjän valinta (.gguf)\".\n"
    L"\n"
    L"Milloin voimaan: viimeistään noin 1 minuutin kuluttua rekisteröinnistä (moottorin ollessa sulkeutunut toimettomuuden vuoksi)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Tämä malli on jo sisäänrakennettu Emebala Chatiin. Rekisteröintiä ei tarvita. Valitse sisäänrakennettu paikallinen käännösmoottori suoraan.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ei rekisteröity)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Hallitse malleja…",
    .gguf_manager_title = L"Käyttäjämallien hallinta",
    .gguf_manager_empty = L"Ei rekisteröityjä käyttäjämalleja.",
    .gguf_manager_rename = L"Nimeä uudelleen…",
    .gguf_manager_delete = L"Poista…",
    .gguf_manager_close = L"Sulje",
    .gguf_manager_delete_confirm_title = L"Poista mallin rekisteröinti",
    .gguf_manager_delete_confirm_body = L"Valitun mallin rekisteröinti poistetaan.\n"
    L"\n"
    L"Mallitiedosto (.gguf) säilyy levyllä eikä poisteta. Jos tätä mallia käytettiin, käännösmoottorin valinta palaa automaattiseen (Auto).\n"
    L"\n"
    L"Jatka?",
    .gguf_manager_rename_title = L"Nimeä malli uudelleen",
    .gguf_manager_rename_body = L"Syötä uusi nimi (enintään 64 merkkiä, ei välilyöntejä).",
    .gguf_manager_rename_invalid = L"Tätä nimeä ei voi käyttää. Syötä nimi, joka ei ole tyhjä, eroaa olemassa olevista eikä sisällä välilyöntejä tai polkuerottimia, enintään 64 merkkiä.",
    .gguf_manager_done = L"Muutokset tallennettu.",
    .gguf_manager_err_serialize = L"registry.jsonia ei voitu serialisoida (tiedostonimi hylättiin). Mitään ei muutettu.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% ei ole käytettävissä; jaettua mallikansiota ei löydy.",
    .gguf_manager_err_write = L"registry.jsonia ei voitu kirjoittaa.",
    .gguf_manager_err_write_partial = L"registry.jsonia ei voitu kirjoittaa loppuun.",
    .gguf_manager_err_registry_damaged = L"registry.json on vioittunut tai sen skeemaa ei tueta. Sitä EI muutettu. Korjaa tai poista se ja yritä uudelleen.",
    .tooltip_translate_failed = L"Käännös epäonnistui. Yritä uudelleen.",
    .repair_transient_body = L"Paikallinen käännösmoottori on tilapäisesti EI käytettävissä (se saattaa olla käynnistymässä). Odota hetki ja yritä uudelleen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Vahvista",
    .dialog_cancel = L"Peruuta",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Lisää tiedostosta…",
    .gguf_manager_add_hf = L"Lisää Hugging Facesta…",
    .hf_add_title = L"Lisää Hugging Face -malli",
    .hf_url_label = L"Mallin URL-osoite",
    .hf_downloading = L"Ladataan…",
    .hf_failed = L"Lataus epäonnistui.",
    .hf_invalid_url = L"Syötä huggingface.co-mallin URL-osoite (resolve, blob tai mallisivu)",
    .hf_done = L"Rekisteröity.",
    .openai_base_url_hint = L"esim. https://api.openai.com/v1",
    .openai_api_key_hint = L"API-avain",
    // REQ-051: painike [Poista asetukset].
    .openai_delete_settings = L"Poista asetukset",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Tarkista päivitykset",
    .update_idle_note = L"Katso, käytätkö uusinta Emebala Chat -versiota.",
    .update_checking = L"Tarkistetaan päivityksiä…",
    .update_checking_hint = L"Toimii taustalla; voit sulkea tämän ikkunan.",
    .update_uptodate = L"Käytät uusinta versiota",
    .update_dlg_title = L"Päivitys saatavilla",
    .update_dlg_version = L"Uusi versio",
    .update_dlg_version_cur = L"käytössäsi on {v}",
    .update_dlg_size = L"Koko",
    .update_dlg_size_approx = L"noin {s}",
    .update_dlg_privacy = L"Tarkistus ja lataus yhdistävät GitHubiin.",
    .update_dlg_download = L"Lataa",
    .update_dlg_later = L"Myöhemmin",
    .update_dl_progress = L"Ladataan…",
    .update_dl_cancel = L"Peruuta",
    .update_dl_cancelled = L"Lataus peruutettu.",
    .update_ready_title = L"Lataus valmis — valmis asennettavaksi",
    .update_ready_warn = L"Sovellus sulkeutuu asennettaessa.",
    .update_ready_install = L"Asenna nyt",
    .update_ready_later = L"Myöhemmin",
    .update_err_offline = L"Olet offline-tilassa. Tarkista yhteys ja yritä uudelleen.",
    .update_err_rate = L"Liikaa pyyntöjä. Yritä uudelleen hetken kuluttua.",
    .update_err_hash = L"Ladattu tiedosto ei läpäissyt tarkistusta.",
    .update_err_retry = L"Yritä uudelleen",
    .update_snoozed_line = L"Voit asentaa päivityksen myöhemmin.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Lataa malli",
    .model_dl_consent_body = L"Sisäänrakennettua käännösmallia ei ole asennettu. Ladataanko nyt? (noin {s})",
    .model_dl_privacy = L"Lataus yhdistää huggingface.co-palveluun.",
    .model_dl_download = L"Lataa nyt",
    .model_dl_verifying = L"Tarkistetaan latausta…",
    .model_dl_placing = L"Asennetaan mallia…",
    .model_dl_done = L"Malli on valmis. Paikallinen moottori käyttää sitä seuraavasta käännöksestä lähtien.",
    .model_dl_failed = L"Lataus epäonnistui. Tarkista yhteys ja yritä uudelleen.",
    .model_dl_disk_space = L"Levytilaa ei ole riittävästi (tarvitaan noin {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Käännösmallin latausasetukset…",
    .engine_load_settings_title = L"Käännösmallin lataus",
    .load_mode_on_use = L"Lataa käytettäessä (suositus)",
    .load_mode_warmup = L"Lataa etukäteen",
    .load_mode_on_use_desc = L"Säästää muistia. Ensimmäinen käännös saattaa alkaa muutaman sekunnin viiveellä.",
    .load_mode_warmup_desc = L"Käännös alkaa heti. Malli pysyy muistissa.",
    .gguf_policy_sticky = L"(Pysy muistissa)",
    .gguf_policy_evict = L"(Lataa käytettäessä)",
    .gguf_manager_toggle_policy = L"Vaihda käytäntöä",
    .gguf_duplicate_warning = L"Kaksoiskappaleet: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Siivoa kaksoiskappaleet",
    .gguf_manager_dedupe_done = L"Kaksoiskappaletiedostot siirrettiin Roskakoriin.",
    .gguf_manager_dedupe_failed = L"Kaksoiskappaleita ei voitu siivota. Tarkista, käyttääkö jokin muu ohjelma niitä.",
    .menu_ui_theme = L"Käyttöliittymäteema",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Lämmin Editorial",
    .theme_monolith = L"Hienostunut monoliitti",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Kelluva takaisinkäännös (tarkistus)",
    .back_translate_header = L"🔄 Takaisinkäännöksen tarkistus",
    .back_translate_match_exact = L"✓ Merkitys täsmää",
    .back_translate_match_check = L"⚡ Tarkista sävy",
    .back_translate_reference = L"↳ Alkuperäinen: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Tämä ei ole kelvollinen GGUF-mallitiedosto; sitä ei rekisteröity.",
    .gguf_manager_active_suffix = L"(käytössä)",
    .gguf_manager_activated = L"Tämä malli on nyt käytössä.",
};
// 21. Norwegian (no / nb)
const LocalizedStrings kStringsNorwegian = {
    .menu_status_active = L"Status: aktiv (F9: pause)",
    .menu_status_paused = L"Status: pause (F9: gjenoppta)",
    .menu_engine = L"Oversettelsesmotor",
    .menu_engine_google = L"Google Oversetter (gratis / uten installasjon)",
    .menu_engine_local = L"Innebygd lokal motor (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Kildespråk (inndata)",
    .menu_target_lang = L"Målspråk (utdata)",
    .menu_swap_langs = L"Bytt kilde ⇄ mål (dobbeltklikk)",
    .menu_auto_send = L"Automatisk sending med Enter",
    .menu_sound_feedback = L"Lydtilbakemelding (toner)",
    .menu_show_badge = L"Svevende merke synlig",
    .menu_start_with_windows = L"Start med Windows",
    .menu_cheatsheet = L"Snarveisveiledning og hjelp...",
    .menu_exit = L"Avslutt Emebala Chat",
    .menu_about = L"Om Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — snarveier og veiledning",
    .cheatsheet_body = L"Snarveier og veiledning for Emebala Chat:\n\n"
    L"  • F9 : aktiver / pause\n"
    L"  • Ctrl + F9 : bytt målspråk\n"
    L"  • Ctrl + Shift + Enter : veksle automatisk sending\n"
    L"  • Shift + Enter : oversett og send umiddelbart\n\n"
    L"Mus på merket:\n"
    L"  • Venstreklikk : aktiver / pause\n"
    L"  • Dobbeltklikk : bytt kilde ⇄ mål\n"
    L"  • Høyreklikk : åpne innstillingsmenyen\n\n"
    L"Oversettelsesmoduser:\n"
    L"  • Bare erstatt (autosending av): erstatter linjen med oversettelsen for gjennomgang.\n"
    L"  • Autosending (på): erstatter linjen og trykker Enter med en gang.",
    .about_title = L"Om Emebala Chat",
    .badge_active = L"Aktiv",
    .badge_translating = L"Oversetter...",
    .badge_paused = L"Pause",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Kunne ikke kopiere den valgte teksten. Sjekk målappen og prøv igjen.",
    .tooltip_no_selection = L"Ingen tekst er valgt for oversettelse.",
    .auto_detect = L"Oppdag automatisk",
    .app_already_running = L"Emebala Chat kjører allerede i bakgrunnen.\nSjekk varslingsfeltet.",
    .app_com_failed = L"COM-initialisering mislyktes.\nDet svevende merket og taleavspilling er utilgjengelige,\nmen oversettelse, snarveier, varleske og lyder fungerer fortsatt.",
    .about_tagline = L"Slutt å kopiere og lime inn. Skriv naturally på morsmålet ditt — oversettelsen erstatter tastene dine i sanntid i enhver Windows-app.",
    .about_feature0 = L"⚡ Dra-for-å-oversette — merk tekst i en vilkårlig app, det svevende ikonet oversetter umiddelbart.",
    .about_feature1 = L"🔊 Nøyalt TTS — uttaler alle 37 språk via installerte Windows-talepakker.",
    .about_feature2 = L"🔒 100 % på enheten og privat — aktivt kun mens snarveien holdes; utklippstavla urørt.",
    .about_etymology = L"I år 2000 f.Kr. kalde mesopotamiske skrivere «Eme-bala» — de som gjør språket til ei bru mellom verdener.",
    .about_link_website = L"Nettside",
    .about_link_contact = L"Kontakt",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Åpningstider 10:00–19:00 KST",
    .about_contact_lead = L"Ledende arkitekt: Yongtai Kim",
    .tooltip_copied = L"✓ Kopiert!",
    .tooltip_button_copy = L"📋 Kopier",
    .tooltip_button_tts = L"🔊 Tale",
    .menu_ui_language = L"Grensesnittspråk",
    .menu_ui_language_auto = L"Auto (systemspråk)",
    .about_reset_button = L"Tilbakestill til systemstandarder",
    .about_reset_done = L"Standarder gjenopprettet",
    .menu_typing_group = L"Tastaturskriving",
    .menu_tooltip_group = L"Dra-verktøytips",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Ingen Windows-stemme er installert for dette språket. Klikk på 🔊 igjen for å åpne Tale-innstillingene.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Personvernerklæring",
    .privacy_notice_body = L"Dette er personvernprinsippene til Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat driver ingen egne servere.\n"
    L"• Med lokal modell forlater den oversatte teksten ikke enheten.\n"
    L"• Hvis du velger Google Oversett, eller det bytter automatisk til sky, sendes markert eller skrevet tekst direkte til Google for oversettelse – ikke via Emebala.\n"
    L"• Diagnosticslogger er AV som standard; du slår dem på i innstillingene (opt-in).\n"
    L"• Hvis du ikke vil sende tekst til skyen (Google), åpne menyen fra systemstatusfeltets ikon, velg “Oversettelsesmotor” og deretter “Innebygd lokal motor”. Uten lokal modell og med sky-reserve deaktivert, kjører ikke oversettelsen — ingenting sendes.\n"
    L"\n"
    L"Detaljene finnes i README-filen, som kan leses på nytt når som helst.\n",
    .cheatsheet_config_path = L"Konfigurasjonsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Teksten var for lang: bare starten og slutten ble oversatt.",
    .tooltip_untranslated_above = L"Det er ny uoversatt tekst ovenfor. Plasser markøren på slutten av den linjen og trykk Enter for å oversette.",
    .repair_in_progress = L"Reparerer de lokale motorkomponentene…",
    .repair_failed_title = L"Lokal oversettelse er ikke tilgjengelig",
    .repair_failed_body = L"Filene til den lokale oversettelsesmotoren mangler, så oversettelsen er satt på pause. Installer Emebala Chat på nytt for å gjenopprette den lokale motoren, eller bytt til skyoversettelse (Google) ved å velge “Google Oversetter” fra menyen i systemfeltet, “Oversettelsesmotor”.",
    .menu_engine_openai = L"OpenAI-kompatibel (tilpasset server)…",
    .openai_settings_title = L"Innstillinger for OpenAI-kompatibel motor",
    .openai_settings_action = L"Innstillinger for OpenAI-kompatibel motor…",
    .openai_base_url_label = L"Basis-URL",
    .openai_api_key_label = L"API-nøkkel",
    .openai_model_label = L"Modell",
    .openai_fetch_models = L"Hent modelliste",
    .openai_fetch_failed = L"Modellisten kunne ikke hentes. Du kan skrive modellnavnet direkte.",
    .openai_http_warning_title = L"Usikker tilkobling (HTTP)",
    .openai_http_warning_body = L"Basis-URL-en bruker HTTP (ukryptert). API-nøkkelen og teksten din sendes i klartekst. Fortsette?",
    .openai_saved = L"OpenAI-kompatible innstillinger lagret.",
    .openai_key_masked = L"Lagret nøkkel: ",
    .openai_invalid_base_url = L"Basis-URL-en er ugyldig. Eksempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Brukerspesifisert modell (.gguf)",
    .menu_browse_gguf_file = L"Registrer en annen .gguf-modell…",
    .user_gguf_quality_title = L"Varsel om oversettelseskvalitet",
    .user_gguf_quality_body = L"Den valgte modellen er ikke Hy-MT2. Denne versjonen bruker prompten kun for Hy-MT2, så oversettelseskvaliteten med denne modellen kan ikke garanteres. Fortsette?",
    .user_gguf_registered_title = L"Modell registrert",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Den valgte modellen er registrert hos den lokale motoren.\n"
    L"\n"
    L"Denne modellen brukes til oversettelse når du velger \"Velg oversettelsesmotor > Brukervalg (.gguf)\".\n"
    L"\n"
    L"Når den trer i kraft: innen cirka 1 minutt etter registreringen (etter at motoren avsluttes ved inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Denne modellen er allerede innebygd i Emebala Chat. Registrering er ikke nødvendig. Velg den innebygde lokale oversettelsesmotoren direkte.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ikke registrert)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Administrer modeller…",
    .gguf_manager_title = L"Administrasjon av brukermodeller",
    .gguf_manager_empty = L"Ingen brukermodeller registrert.",
    .gguf_manager_rename = L"Gi nytt navn…",
    .gguf_manager_delete = L"Slett…",
    .gguf_manager_close = L"Lukk",
    .gguf_manager_delete_confirm_title = L"Slett modellregistrering",
    .gguf_manager_delete_confirm_body = L"Registreringen av den valgte modellen slettes.\n"
    L"\n"
    L"Modellfilen (.gguf) beholdes på disken og slettes ikke. Hvis denne modellen var i bruk, går valget av oversettelsesmotor tilbake til Automatisk (Auto).\n"
    L"\n"
    L"Fortsett?",
    .gguf_manager_rename_title = L"Gi modell nytt navn",
    .gguf_manager_rename_body = L"Skriv inn et nytt navn (maks. 64 tegn, uten mellomrom).",
    .gguf_manager_rename_invalid = L"Det navnet kan ikke brukes. Skriv inn et navn som ikke er tomt, er forskjellig fra eksisterende, uten mellomrom eller sti-separatorer, opptil 64 tegn.",
    .gguf_manager_done = L"Endringer lagret.",
    .gguf_manager_err_serialize = L"registry.json kunne ikke serialiseres (et filnavn ble avvist). Ingenting ble endret.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% er ikke tilgjengelig; den delte modellmappen ble ikke funnet.",
    .gguf_manager_err_write = L"registry.json kunne ikke skrives.",
    .gguf_manager_err_write_partial = L"registry.json kunne ikke skrives fullstendig.",
    .gguf_manager_err_registry_damaged = L"registry.json er skadet eller har et schema som ikke støttes. Den har IKKE blitt endret. Reparer eller fjern den, og prøv igjen.",
    .tooltip_translate_failed = L"Oversettelsen mislyktes. Prøv igjen.",
    .repair_transient_body = L"Den lokale oversettelsesmotoren er midlertidig utilgjengelig (den kan være under oppstart). Vent et øyeblikk og prøv igjen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Bekreft",
    .dialog_cancel = L"Avbryt",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Legg til fra fil…",
    .gguf_manager_add_hf = L"Legg til fra Hugging Face…",
    .hf_add_title = L"Legg til en Hugging Face-modell",
    .hf_url_label = L"Modell-URL",
    .hf_downloading = L"Laster ned…",
    .hf_failed = L"Nedlastingen mislyktes.",
    .hf_invalid_url = L"Skriv inn modell-URL fra huggingface.co (resolve, blob eller modellside)",
    .hf_done = L"Registrert.",
    .openai_base_url_hint = L"f.eks. https://api.openai.com/v1",
    .openai_api_key_hint = L"API-nøkkel",
    // REQ-051: knapp [Slett innstillinger].
    .openai_delete_settings = L"Slett innstillinger",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Se etter oppdateringer",
    .update_idle_note = L"Sjekk om du kjører den nyeste versjonen av Emebala Chat.",
    .update_checking = L"Ser etter oppdateringer…",
    .update_checking_hint = L"Kjører i bakgrunnen; du kan lukke dette vinduet.",
    .update_uptodate = L"Du har den nyeste versjonen",
    .update_dlg_title = L"Oppdatering tilgjengelig",
    .update_dlg_version = L"Ny versjon",
    .update_dlg_version_cur = L"du har {v}",
    .update_dlg_size = L"Størrelse",
    .update_dlg_size_approx = L"omtrent {s}",
    .update_dlg_privacy = L"Sjekk og nedlasting kobler til GitHub.",
    .update_dlg_download = L"Last ned",
    .update_dlg_later = L"Senere",
    .update_dl_progress = L"Laster ned…",
    .update_dl_cancel = L"Avbryt",
    .update_dl_cancelled = L"Nedlastingen avbrutt.",
    .update_ready_title = L"Nedlastingen er fullført — klar til å installere",
    .update_ready_warn = L"Appen lukkes for å installere.",
    .update_ready_install = L"Installer nå",
    .update_ready_later = L"Senere",
    .update_err_offline = L"Du er frakoblet. Sjekk tilkoblingen din, og prøv igjen.",
    .update_err_rate = L"For mange forespørsler. Prøv igjen om litt.",
    .update_err_hash = L"Den nedlastede filen bestod ikke verifiseringen.",
    .update_err_retry = L"Prøv igjen",
    .update_snoozed_line = L"Du kan installere oppdateringen senere.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Last ned modell",
    .model_dl_consent_body = L"Den innebygde oversettelsesmodellen er ikke installert. Last ned nå? (ca. {s})",
    .model_dl_privacy = L"Nedlastingen kobler til huggingface.co.",
    .model_dl_download = L"Last ned nå",
    .model_dl_verifying = L"Verifiserer nedlastingen…",
    .model_dl_placing = L"Installerer modellen…",
    .model_dl_done = L"Modellen er klar. Den lokale motoren bruker den fra neste oversettelse.",
    .model_dl_failed = L"Nedlastingen mislyktes. Sjekk tilkoblingen din og prøv igjen.",
    .model_dl_disk_space = L"Ikke nok ledig diskplass (ca. {s} kreves).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Innstillinger for lasting av oversettelsesmodellen…",
    .engine_load_settings_title = L"Lasting av oversettelsesmodell",
    .load_mode_on_use = L"Last ved bruk (anbefalt)",
    .load_mode_warmup = L"Forhåndslast",
    .load_mode_on_use_desc = L"Sparer minne. Den første oversettelsen kan starte noen sekunder senere.",
    .load_mode_warmup_desc = L"Oversettelsen starter med en gang. Modellen blir værende i minnet.",
    .gguf_policy_sticky = L"(Hold lastet)",
    .gguf_policy_evict = L"(Last ved bruk)",
    .gguf_manager_toggle_policy = L"Bytt policy",
    .gguf_duplicate_warning = L"Dupliserte filer: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Rens duplikater",
    .gguf_manager_dedupe_done = L"De dupliserte filene ble flyttet til papirkurven.",
    .gguf_manager_dedupe_failed = L"De dupliserte filene kunne ikke renses. Sjekk om et annet program bruker dem.",
    .menu_ui_theme = L"UI-tema",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Varm Editorial",
    .theme_monolith = L"Forfinet monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Flytende tilbakeoversettelse (kontroll)",
    .back_translate_header = L"🔄 Kontroll av tilbakeoversettelse",
    .back_translate_match_exact = L"✓ Betydningen stemmer",
    .back_translate_match_check = L"⚡ Sjekk nyanse",
    .back_translate_reference = L"↳ Original: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Dette er ikke en gyldig GGUF-modellfil; den ble ikke registrert.",
    .gguf_manager_active_suffix = L"(i bruk)",
    .gguf_manager_activated = L"Denne modellen er nå i bruk.",
};
// 22. Greek (el)
const LocalizedStrings kStringsGreek = {
    .menu_status_active = L"Κατάσταση: ενεργό (F9: παύση)",
    .menu_status_paused = L"Κατάσταση: σε παύση (F9: συνέχεια)",
    .menu_engine = L"Μηχανή μετάφρασης",
    .menu_engine_google = L"Google Μετάφραση (δωρεάν / χωρίς εγκατάσταση)",
    .menu_engine_local = L"Ενσωματωμένη τοπική μηχανή (Hy-MT2-1.8B εκτός σύνδεσης)",
    .menu_source_lang = L"Γλώσσα πηγής (είσοδος)",
    .menu_target_lang = L"Γλώσταση προορισμού (έξοδος)",
    .menu_swap_langs = L"Εναλλαγή πηγής ⇄ προορισμού (διπλό κλικ)",
    .menu_auto_send = L"Αυτόματη αποστολή με Enter",
    .menu_sound_feedback = L"Ηχητική ανάδραση (ήχοι)",
    .menu_show_badge = L"Πloating σήμα ορατό",
    .menu_start_with_windows = L"Έναρξη με τα Windows",
    .menu_cheatsheet = L"Οδηγός συντομεύσεων και βοήθεια...",
    .menu_exit = L"Έξοδος από το Εμεμπάλα Τσατ",
    .menu_about = L"Σχετικά με το Εμεμπάλα Τσατ…",
    .cheatsheet_title = L"Εμεμπάλα Τσατ — συντομεύσεις και οδηγός",
    .cheatsheet_body = L"Συντομεύσεις και οδηγός χρήσης του Εμεμπάλα Τσατ:\n\n"
    L"  • F9 : ενεργοποίηση / παύση\n"
    L"  • Ctrl + F9 : αλλαγή γλώσσας προορισμού\n"
    L"  • Ctrl + Shift + Enter : εναλλαγή αυτόματης αποστολής\n"
    L"  • Shift + Enter : μετάφραση και άμεση αποστολή\n\n"
    L"Χειρισμός ποντικιού στο σήμα:\n"
    L"  • Αριστερό κλικ : ενεργοποίηση / παύση\n"
    L"  • Διπλό κλικ : εναλλαγή πηγής ⇄ προορισμού\n"
    L"  • Δεξί κλικ : άνοιγμα μενού ρυθμίσεων\n\n"
    L"Λειτουργίες μετάφρασης:\n"
    L"  • Αντικατάσταση μόνο (αυτόματη αποστολή off): αντικαθιστά τη γραμμή με τη μετάφραση για έλεγχο.\n"
    L"  • Αυτόματη αποστολή (on): αντικαθιστά τη γραμμή και πατά αμέσως Enter.",
    .about_title = L"Σχετικά με το Εμεμπάλα Τσατ",
    .badge_active = L"Ενεργό",
    .badge_translating = L"Μετάφραση...",
    .badge_paused = L"Σε παύση",
    .tooltip_title = L"Εμεμπάλα Τσατ",
    .tooltip_copy_failed = L"Δεν ήταν δυνατή η αντιγραφή του επιλεγμένου κειμένου. Ελέγξτε την εφαρμογή προορισμού και δοκιμάστε ξανά.",
    .tooltip_no_selection = L"Δεν υπάρχει επιλεγμένο κείμενο για μετάφραση.",
    .auto_detect = L"Αυτόματος εντοπισμός",
    .app_already_running = L"Το Εμεμπάλα Τσατ εκτελείται ήδη στο παρασκήνιο.\nΕλέγξτε το πεδίο ειδοποιήσεων.",
    .app_com_failed = L"Η αρχικοποίηση COM απέτυχε.\nΤο πloating σήμα και η συνθετική ομιλία δεν θα είναι διαθέσιμα,\nαλλά η μετάφραση, οι συντομεύσεις, το εικονίδιο και οι ήχοι συνεχίζουν να λειτουργούν.",
    .about_tagline = L"Σταματήστε το αντιγραφή-επικόλληση. Πληκτρολογήστε φυσικά στη μητρική σας γλώσσα — η μετάφραση αντικαθιστά την πληκτρολόγησή σας σε πραγματικό χρόνο σε οποιαδήποτε εφαρμογή Windows.",
    .about_feature0 = L"⚡ Σύρε-για-μετάφραση — επιλέξτε κείμενο σε οποιαδήποτε εφαρμογή, το σήμα μεταφράζει αμέσως.",
    .about_feature1 = L"🔊 Νευρωνικό TTS — προφέρει και τις 37 γλώσσες μέσω εγκατεστημένων πακέτων φωνής των Windows.",
    .about_feature2 = L"🔒 100% στη συσκευή και ιδιωτικό — ενεργό μόνο όσο κρατιέται η συντόμευση· πρόχειρο ανέπαφο.",
    .about_etymology = L"Το 2000 π.Χ., οι Μεσοποτάμιοι γραφείς ονόμαζαν «Eme-bala» όσους έκαναν τη γλώσσα γέφυρα μεταξύ κόσμων.",
    .about_link_website = L"Ιστοσελίδα",
    .about_link_contact = L"Επικοινωνία",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Ώρες γραφείου 10:00–19:00 KST",
    .about_contact_lead = L"Κύριος αρχιτέκτονας: Yongtai Kim",
    .tooltip_copied = L"✓ Αντιγράφηκε!",
    .tooltip_button_copy = L"📋 Αντιγραφή",
    .tooltip_button_tts = L"🔊 Ομιλία",
    .menu_ui_language = L"Γλώσσα διεπαφής",
    .menu_ui_language_auto = L"Αυτόματο (γλώσσα συστήματος)",
    .about_reset_button = L"Επαναφορά στις προεπιλογές συστήματος",
    .about_reset_done = L"Οι προεπιλογές αποκαταστάθηκαν",
    .menu_typing_group = L"Πληκτρολόγηση",
    .menu_tooltip_group = L"Υπόδειξη σύρσιμο",
    .app_name = L"Εμεμπάλα Τσατ",
    .tooltip_no_tts_voice = L"Δεν είναι εγκατεστημένη φωνή των Windows για αυτή τη γλώσσα. Κάντε ξανά κλικ στο 🔊 για να ανοίξετε τις ρυθμίσεις Ομιλίας.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Γνωστοποίηση απορρήτου",
    .privacy_notice_body = L"Αυτές είναι οι αρχές απορρήτου του Emebala Chat.\n"
    L"\n"
    L"• Το Emebala Chat δεν λειτουργεί δικούς του διακομιστές.\n"
    L"• Με τοπικό μοντέλο, το μεταφραζόμενο κείμενο δεν φεύγει από τη συσκευή σας.\n"
    L"• Αν επιλέξετε τη Μετάφραση Google ή γίνει αυτόματη εναλλαγή στο cloud, το επιλεγμένο ή πληκτρολογημένο κείμενο στέλνεται απευθείας στην Google για μετάφραση, όχι μέσω Emebala.\n"
    L"• Τα αρχεία διαγνωστικών είναι ΑΠΕΝΕΡΓΟΠΟΙΗΜΕΝΑ από προεπιλογή· ενεργοποιούνται στις ρυθμίσεις (ρητή συναίνεση).\n"
    L"• Εάν δεν θέλετε να στείλετε κείμενο στο cloud (Google), ανοίξτε το μενού του εικονιδίου στη γραμμή εργασιών, επιλέξτε «Μηχανή μετάφρασης» και στη συνέχεια «Ενσωματωμένη τοπική μηχανή». Χωρίς εγκατεστημένο τοπικό μοντέλο και με απενεργοποιημένη την εφεδρική λειτουργία cloud, η μετάφραση δεν εκτελείται — τίποτα δεν στέλνεται.\n"
    L"\n"
    L"Οι πλήρεις λεπτομέρειες βρίσκονται στο αρχείο README, που διαβάζεται ξανά ανά πάσα στιγμή.\n",
    .cheatsheet_config_path = L"Αρχείο ρυθμίσεων: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Το κείμενο είναι πολύ μακρύ: μεταφράστηκαν μόνο η αρχή και το τέλος.",
    .tooltip_untranslated_above = L"Υπάρχει νέο μη μεταφρασμένο κείμενο παραπάνω. Τοποθετήστε τον δείκτη στο τέλος εκείνης της γραμμής και πατήστε Enter για μετάφραση.",
    .repair_in_progress = L"Επισκευή των τοπικών συστατικών μηχανής…",
    .repair_failed_title = L"Η τοπική μετάφραση δεν είναι διαθέσιμη",
    .repair_failed_body = L"Τα αρχεία της τοπικής μηχανής μετάφρασης λείπουν, επομένως η μετάφραση έχει διακοπεί. Εγκαταστήστε ξανά το Emebala Chat για να επαναφέρετε την τοπική μηχανή, ή μεταβείτε σε μετάφραση cloud (Google) επιλέγοντας «Google Μετάφραση» από το μενού της περιοχής ειδοποιήσεων, «Μηχανή μετάφρασης».",
    .menu_engine_openai = L"Συμβατό με OpenAI (προσαρμοσμένος διακομιστής)…",
    .openai_settings_title = L"Ρυθμίσεις συμβατής μηχανής OpenAI",
    .openai_settings_action = L"Ρυθμίσεις συμβατής μηχανής OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"Κλειδί API",
    .openai_model_label = L"Μοντέλο",
    .openai_fetch_models = L"Λήψη λίστας μοντέλων",
    .openai_fetch_failed = L"Δεν ήταν δυνατή η λήψη της λίστας μοντέλων. Μπορείτε να πληκτρολογήσετε το όνομα του μοντέλου απευθείας.",
    .openai_http_warning_title = L"Μη ασφαλής σύνδεση (HTTP)",
    .openai_http_warning_body = L"Το Base URL χρησιμοποιεί HTTP (μη κρυπτογραφημένο). Το κλειδί API και το κείμενό σας θα σταλούν σε απλή μορφή. Συνέχεια;",
    .openai_saved = L"Οι συμβατές ρυθμίσεις OpenAI αποθηκεύτηκαν.",
    .openai_key_masked = L"Αποθηκευμένο κλειδί: ",
    .openai_invalid_base_url = L"Το Base URL δεν είναι έγκυρο. Παράδειγμα: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Μοντέλο καθορισμένο από τον χρήστη (.gguf)",
    .menu_browse_gguf_file = L"Καταχώριση άλλου μοντέλου .gguf…",
    .user_gguf_quality_title = L"Ειδοποίηση ποιότητας μετάφρασης",
    .user_gguf_quality_body = L"Το επιλεγμένο μοντέλο δεν είναι Hy-MT2. Η τρέχουσα έκδοση χρησιμοποιεί το prompt μόνο για Hy-MT2, επομένως η ποιότητα μετάφρασης με αυτό το μοντέλο δεν είναι εγγυημένη. Συνέχεια;",
    .user_gguf_registered_title = L"Το μοντέλο καταχωρήθηκε",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Το επιλεγμένο μοντέλο καταχωρήθηκε στην τοπική μηχανή.\n"
    L"\n"
    L"Αυτό το μοντέλο χρησιμοποιείται για μετάφραση όταν επιλέγετε «Επιλογή μηχανής μετάφρασης > Επιλογή χρήστη (.gguf)».\n"
    L"\n"
    L"Πότε ισχύει: έως περίπου 1 λεπτό μετά την καταχώρηση (μετά τον τερματισμό της μηχανής λόγω αδράνειας)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Αυτό το μοντέλο είναι ήδη ενσωματωμένο στο Emebala Chat. Δεν απαιτείται εγγραφή. Μπορείτε να επιλέξετε απευθείας τον ενσωματωμένο τοπικό μηχανισμό μετάφρασης.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(μη καταχωρημένο)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Διαχείριση μοντέλων…",
    .gguf_manager_title = L"Διαχείριση μοντέλων χρήστη",
    .gguf_manager_empty = L"Δεν υπάρχουν εγγεγραμμένα μοντέλα χρήστη.",
    .gguf_manager_rename = L"Μετονομασία…",
    .gguf_manager_delete = L"Διαγραφή…",
    .gguf_manager_close = L"Κλείσιμο",
    .gguf_manager_delete_confirm_title = L"Διαγραφή καταχώρισης μοντέλου",
    .gguf_manager_delete_confirm_body = L"Η καταχώριση του επιλεγμένου μοντέλου θα διαγραφεί.\n"
    L"\n"
    L"Το αρχείο μοντέλου (.gguf) διατηρείται στον δίσκο και δεν διαγράφεται. Αν αυτό το μοντέλο ήταν σε χρήση, η επιλογή μηχανής μετάφρασης επιστρέφει σε Αυτόματο (Auto).\n"
    L"\n"
    L"Συνέχεια;",
    .gguf_manager_rename_title = L"Μετονομασία μοντέλου",
    .gguf_manager_rename_body = L"Εισαγάγετε νέο όνομα (έως 64 χαρακτήρες, χωρίς κενά).",
    .gguf_manager_rename_invalid = L"Αυτό το όνομα δεν μπορεί να χρησιμοποιηθεί. Εισαγάγετε ένα μη κενό όνομα, διαφορετικό από τα υπάρχοντα, χωρίς κενά ή διαχωριστικά διαδρομής, έως 64 χαρακτήρες.",
    .gguf_manager_done = L"Οι αλλαγές αποθηκεύτηκαν.",
    .gguf_manager_err_serialize = L"Δεν ήταν δυνατή η σειριοποίηση του registry.json (απορρίφθηκε ένα όνομα αρχείου). Δεν άλλαξε τίποτα.",
    .gguf_manager_err_no_localappdata = L"Το %LOCALAPPDATA% δεν είναι διαθέσιμο· δεν είναι δυνατός ο εντοπισμός του κοινόχρηστου φακέλου μοντέλων.",
    .gguf_manager_err_write = L"Δεν ήταν δυνατή η εγγραφή του registry.json.",
    .gguf_manager_err_write_partial = L"Το registry.json δεν εγγράφηκε πλήρως.",
    .gguf_manager_err_registry_damaged = L"Το registry.json είναι κατεστραμμένο ή έχει μη υποστηριζόμενο σχήμα. ΔΕΝ τροποποιήθηκε. Επισκευάστε το ή αφαιρέστε το και δοκιμάστε ξανά.",
    .tooltip_translate_failed = L"Η μετάφραση απέτυχε. Δοκιμάστε ξανά.",
    .repair_transient_body = L"Η τοπική μηχανή μετάφρασης είναι προσωρινά μη διαθέσιμη (πιθανώς ξεκινά). Περιμένετε λίγο και δοκιμάστε ξανά.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Εντάξει",
    .dialog_cancel = L"Άκυρο",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Προσθήκη από αρχείο…",
    .gguf_manager_add_hf = L"Προσθήκη από το Hugging Face…",
    .hf_add_title = L"Προσθήκη μοντέλου Hugging Face",
    .hf_url_label = L"URL μοντέλου",
    .hf_downloading = L"Λήψη σε εξέλιξη…",
    .hf_failed = L"Η λήψη απέτυχε.",
    .hf_invalid_url = L"Εισαγάγετε το URL μοντέλου από το huggingface.co (resolve, blob ή σελίδα μοντέλου)",
    .hf_done = L"Καταχωρήθηκε.",
    .openai_base_url_hint = L"π.χ. https://api.openai.com/v1",
    .openai_api_key_hint = L"Κλειδί API",
    // REQ-051: κουμπί [Διαγραφή ρυθμίσεων].
    .openai_delete_settings = L"Διαγραφή ρυθμίσεων",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Έλεγχος για ενημερώσεις",
    .update_idle_note = L"Δείτε αν χρησιμοποιείτε την πιο πρόσφατη έκδοση του Emebala Chat.",
    .update_checking = L"Έλεγχος για ενημερώσεις…",
    .update_checking_hint = L"Εκτελείται στο παρασκήνιο· μπορείτε να κλείσετε αυτό το παράθυρο.",
    .update_uptodate = L"Έχετε την πιο πρόσφατη έκδοση",
    .update_dlg_title = L"Διαθέσιμη ενημέρωση",
    .update_dlg_version = L"Νέα έκδοση",
    .update_dlg_version_cur = L"έχετε την {v}",
    .update_dlg_size = L"Μέγεθος",
    .update_dlg_size_approx = L"περίπου {s}",
    .update_dlg_privacy = L"Ο έλεγχος και η λήψη συνδέονται στο GitHub.",
    .update_dlg_download = L"Λήψη",
    .update_dlg_later = L"Αργότερα",
    .update_dl_progress = L"Λήψη σε εξέλιξη…",
    .update_dl_cancel = L"Άκυρο",
    .update_dl_cancelled = L"Η λήψη ακυρώθηκε.",
    .update_ready_title = L"Η λήψη ολοκληρώθηκε — έτοιμο για εγκατάσταση",
    .update_ready_warn = L"Η εφαρμογή θα κλείσει για εγκατάσταση.",
    .update_ready_install = L"Εγκατάσταση τώρα",
    .update_ready_later = L"Αργότερα",
    .update_err_offline = L"Είστε εκτός σύνδεσης. Ελέγξτε τη σύνδεσή σας και δοκιμάστε ξανά.",
    .update_err_rate = L"Πάρα πολλά αιτήματα. Δοκιμάστε ξανά σε λίγο.",
    .update_err_hash = L"Το ληφθέν αρχείο δεν πέρασε τον έλεγχο.",
    .update_err_retry = L"Δοκιμάστε ξανά",
    .update_snoozed_line = L"Μπορείτε να εγκαταστήσετε την ενημέρωση αργότερα.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Λήψη μοντέλου",
    .model_dl_consent_body = L"Το ενσωματωμένο μοντέλο μετάφρασης δεν είναι εγκατεστημένο. Λήψη τώρα; (περίπου {s})",
    .model_dl_privacy = L"Η λήψη συνδέεται στο huggingface.co.",
    .model_dl_download = L"Λήψη τώρα",
    .model_dl_verifying = L"Επαλήθευση λήψης…",
    .model_dl_placing = L"Εγκατάσταση μοντέλου…",
    .model_dl_done = L"Το μοντέλο είναι έτοιμο. Η τοπική μηχανή θα το χρησιμοποιεί από την επόμενη μετάφραση.",
    .model_dl_failed = L"Η λήψη απέτυχε. Ελέγξτε τη σύνδεσή σας και δοκιμάστε ξανά.",
    .model_dl_disk_space = L"Δεν υπάρχει αρκετός ελεύθερος χώρος στο δίσκο (απαιτούνται περίπου {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Ρυθμίσεις φόρτωσης μοντέλου μετάφρασης…",
    .engine_load_settings_title = L"Φόρτωση μοντέλου μετάφρασης",
    .load_mode_on_use = L"Φόρτωση κατά τη χρήση (συνιστάται)",
    .load_mode_warmup = L"Προφόρτωση",
    .load_mode_on_use_desc = L"Εξοικονομεί μνήμη. Η πρώτη μετάφραση ενδέχεται να αρχίσει μερικά δευτερόλεπτα αργότερα.",
    .load_mode_warmup_desc = L"Η μετάφραση ξεκινά αμέσως. Το μοντέλο παραμένει στη μνήμη.",
    .gguf_policy_sticky = L"(Διατήρηση στη μνήμη)",
    .gguf_policy_evict = L"(Φόρτωση κατά τη χρήση)",
    .gguf_manager_toggle_policy = L"Αλλαγή πολιτικής",
    .gguf_duplicate_warning = L"Διπλότυπα αρχεία: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Καθαρισμός διπλότυπων",
    .gguf_manager_dedupe_done = L"Τα διπλότυπα αρχεία μετακινήθηκαν στον Κάδο ανακύκλωσης.",
    .gguf_manager_dedupe_failed = L"Δεν ήταν δυνατός ο καθαρισμός των διπλότυπων αρχείων. Ελέγξτε αν χρησιμοποιούνται από άλλο πρόγραμμα.",
    .menu_ui_theme = L"Θέμα περιβάλλοντος",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Θερμό Editorial",
    .theme_monolith = L"Εξεζητημένο μονόλιθος",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Αιωρούμενη αντίστροφη μετάφραση (έλεγχος)",
    .back_translate_header = L"🔄 Έλεγχος αντίστροφης μετάφρασης",
    .back_translate_match_exact = L"✓ Το νόημα ταιριάζει",
    .back_translate_match_check = L"⚡ Έλεγχος απόχρωσης",
    .back_translate_reference = L"↳ Πρωτότυπο: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Δεν είναι έγκυρο αρχείο μοντέλου GGUF· δεν καταχωρήθηκε.",
    .gguf_manager_active_suffix = L"(σε χρήση)",
    .gguf_manager_activated = L"Αυτό το μοντέλο είναι πλέον σε χρήση.",
};
// 23. Turkish (tr)
const LocalizedStrings kStringsTurkish = {
    .menu_status_active = L"Durum: etkin (F9: duraklat)",
    .menu_status_paused = L"Durum: duraklatıldı (F9: devam et)",
    .menu_engine = L"Çeviri motoru",
    .menu_engine_google = L"Google Çeviri (ücretsiz / kurulum gerektirmez)",
    .menu_engine_local = L"Yerleşik yerel motor (Hy-MT2-1.8B çevrimdışı)",
    .menu_source_lang = L"Kaynak dil (giriş)",
    .menu_target_lang = L"Hedef dil (çıkış)",
    .menu_swap_langs = L"Kaynak ⇄ hedef değiştir (çift tık)",
    .menu_auto_send = L"Enter ile otomatik gönder",
    .menu_sound_feedback = L"Sesli geri bildirim (tonlar)",
    .menu_show_badge = L"Yüzen rozet görünür",
    .menu_start_with_windows = L"Windows ile başlat",
    .menu_cheatsheet = L"Kısayol rehberi ve yardım...",
    .menu_exit = L"Emebala Chat'ten çık",
    .menu_about = L"Emebala Chat hakkında…",
    .cheatsheet_title = L"Emebala Chat — kısayollar ve rehber",
    .cheatsheet_body = L"Emebala Chat kısayolları ve kullanım rehberi:\n\n"
    L"  • F9 : etkinleştir / duraklat\n"
    L"  • Ctrl + F9 : hedef dili değiştir\n"
    L"  • Ctrl + Shift + Enter : otomatik gönderimi aç/kapat\n"
    L"  • Shift + Enter : çevir ve hemen gönder\n\n"
    L"Rozet üzerinde fare denetimi:\n"
    L"  • Sol tık : etkinleştir / duraklat\n"
    L"  • Çift tık : kaynak ⇄ hedef değiştir\n"
    L"  • Sağ tık : ayarlar menüsünü aç\n\n"
    L"Çeviri modları:\n"
    L"  • Yalnızca değiştir (otomatik gönderim kapalı): satırı incelenmek üzere çeviriyle değiştirir.\n"
    L"  • Otomatik gönderim (açık): satırı değiştirir ve hemen Enter'a basar.",
    .about_title = L"Emebala Chat hakkında",
    .badge_active = L"Etkin",
    .badge_translating = L"Çevriliyor...",
    .badge_paused = L"Duraklatıldı",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Seçili metin kopyalanamadı. Hedef uygulamayı denetleyip yeniden deneyin.",
    .tooltip_no_selection = L"Çevrilecek metin seçilmedi.",
    .auto_detect = L"Otomatik algıla",
    .app_already_running = L"Emebala Chat arka planda zaten çalışıyor.\nSistem bildirim tepsisini denetleyin.",
    .app_com_failed = L"COM başlatma başarısız.\nYüzen rozet ve metinden sese kullanılamayacak,\nancak çeviri, kısayollar, tepsi ve sesler çalışmaya devam ediyor.",
    .about_tagline = L"Kopyala-yapıştırı bırakın. Ana dilinizde doğal yazın — çeviri, herhangi bir Windows uygulamasında yazdıklarınızı gerçek zamanlı değiştirir.",
    .about_feature0 = L"⚡ Sürükle-çevir — herhangi bir uygulamada metni seçin, yüzen simge anında çevirir.",
    .about_feature1 = L"🔊 Nöral TTS — yüklü Windows ses paketleriyle 37 dilin tamamını seslendirir.",
    .about_feature2 = L"🔒 %100 cihazda ve özel — yalnızca kısayol basılıyken etkin; panoya dokunulmaz.",
    .about_etymology = L"MÖ 2000'de Mezopotamyalı kâtipler, dili dünyalar arasında köprüye çevirenlere «Eme-bala» derdi.",
    .about_link_website = L"Web sitesi",
    .about_link_contact = L"İletişim",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Çalışma saatleri 10:00–19:00 KST",
    .about_contact_lead = L"Baş mimar: Yongtai Kim",
    .tooltip_copied = L"✓ Kopyalandı!",
    .tooltip_button_copy = L"📋 Kopyala",
    .tooltip_button_tts = L"🔊 Ses",
    .menu_ui_language = L"Arayüz dili",
    .menu_ui_language_auto = L"Otomatik (sistem dili)",
    .about_reset_button = L"Sistem varsayılanlarına sıfırla",
    .about_reset_done = L"Varsayılanlar geri yüklendi",
    .menu_typing_group = L"Klavye yazımı",
    .menu_tooltip_group = L"Sürükle ipucu",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Bu dil için yüklü bir Windows sesi yok. Konuşma ayarlarını açmak için 🔊 simgesine yeniden tıklayın.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Gizlilik uyarısı",
    .privacy_notice_body = L"Emebala Chat gizlilik ilkeleri şöyledir:\n"
    L"\n"
    L"• Emebala Chat kendi sunucularını işletmez.\n"
    L"• Yerel model kullanıldığında çevrilen metin cihazınızdan çıkmaz.\n"
    L"• Google Çeviri seçerseniz veya otomatik buluta geçilirse, seçilen ya da yazılan metin çeviri için doğrudan Google’a gönderilir; Emebala üzerinden geçmez.\n"
    L"• Tanılama günlükleri varsayılan olarak KAPALIDIR; ayarlardan açmanız gerekir (seçmeli onay).\n"
    L"• Metni buluta (Google) göndermek istemiyorsanız sistem tepsisindeki simgenin menüsünü açın, “Çeviri motoru” bölümünden “Yerleşik yerel motor” seçeneğini seçin. Yerel model kurulu değilse ve bulut yedeği kapalıysa çeviri çalışmaz — hiçbir şey gönderilmez.\n"
    L"\n"
    L"Tam ayrıntılar README dosyasındadır; istediğiniz zaman tekrar okuyabilirsiniz.\n",
    .cheatsheet_config_path = L"Yapılandırma dosyası: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Metin çok uzun olduğu için yalnızca başı ve sonu çevrildi.",
    .tooltip_untranslated_above = L"Yukarıda çevrilmemiş yeni metin var. Çevirmek için imleci o satırın sonuna getirin ve Enter'a basın.",
    .repair_in_progress = L"Yerel motor bileşenleri onarılıyor…",
    .repair_failed_title = L"Yerel çeviri kullanılamıyor",
    .repair_failed_body = L"Yerel çeviri motoru dosyaları bulunamadığı için çeviri duraklatıldı. Yerel motoru geri yüklemek için Emebala Chat’i yeniden yükleyin veya bulut (Google) çevirisine geçmek için tepsi menüsünden “Çeviri motoru” altında “Google Çeviri” seçin.",
    .menu_engine_openai = L"OpenAI uyumlu (özel sunucu)…",
    .openai_settings_title = L"OpenAI Uyumlu Motor Ayarları",
    .openai_settings_action = L"OpenAI Uyumlu Motor Ayarları…",
    .openai_base_url_label = L"Temel URL",
    .openai_api_key_label = L"API Anahtarı",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Model listesini getir",
    .openai_fetch_failed = L"Model listesi alınamadı. Model adını doğrudan yazabilirsiniz.",
    .openai_http_warning_title = L"Güvenli olmayan bağlantı (HTTP)",
    .openai_http_warning_body = L"Temel URL HTTP kullanıyor (şifrelenmemiş). API anahtarınız ve metniniz düz metin olarak gönderilecek. Devam edilsin mi?",
    .openai_saved = L"OpenAI uyumlu ayarlar kaydedildi.",
    .openai_key_masked = L"Kaydedilen anahtar: ",
    .openai_invalid_base_url = L"Temel URL geçersiz. Örnek: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Kullanıcı tarafından belirtilen model (.gguf)",
    .menu_browse_gguf_file = L"Başka bir .gguf modeli kaydet…",
    .user_gguf_quality_title = L"Çeviri kalitesi bildirimi",
    .user_gguf_quality_body = L"Seçilen model Hy-MT2 değil. Mevcut sürüm yalnızca Hy-MT2'ye özel istem kullandığından, bu modelle çeviri kalitesi garanti edilmez. Devam edilsin mi?",
    .user_gguf_registered_title = L"Model kaydedildi",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Seçilen model yerel motora kaydedildi.\n"
    L"\n"
    L"Bu model, \"Çeviri motorunu seç > Kullanıcı seçimi (.gguf)\" seçtiğinizde çeviri için kullanılır.\n"
    L"\n"
    L"Ne zaman etkili olur: kayıttan sonra en çok yaklaşık 1 dakika (motorun boşta kalma sonrası kapanmasından sonra)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Bu model Emebala Chat'e zaten yerleşiktir. Kayıt gerekmez. Yerleşik yerel çeviri motorunu doğrudan seçebilirsiniz.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(kayıtlı değil)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Modelleri yönet…",
    .gguf_manager_title = L"Kullanıcı Modeli Yöneticisi",
    .gguf_manager_empty = L"Kayıtlı kullanıcı modeli yok.",
    .gguf_manager_rename = L"Yeniden adlandır…",
    .gguf_manager_delete = L"Sil…",
    .gguf_manager_close = L"Kapat",
    .gguf_manager_delete_confirm_title = L"Model kaydını sil",
    .gguf_manager_delete_confirm_body = L"Seçilen modelin kaydı silinecek.\n"
    L"\n"
    L"Model dosyası (.gguf) diskte tutulur ve silinmez. Bu model kullanılıyorsa, çeviri motoru seçimi Otomatik (Auto) olarak döner.\n"
    L"\n"
    L"Devam edilsin mi?",
    .gguf_manager_rename_title = L"Modeli yeniden adlandır",
    .gguf_manager_rename_body = L"Yeni bir ad girin (en fazla 64 karakter, boşluksuz).",
    .gguf_manager_rename_invalid = L"Bu ad kullanılamaz. Boş olmayan, mevcutlardan farklı, boşluk veya yol ayıracı içermeyen, en fazla 64 karakterlik bir ad girin.",
    .gguf_manager_done = L"Değişiklikler kaydedildi.",
    .gguf_manager_err_serialize = L"registry.json serileştirilemedi (bir dosya adı reddedildi). Hiçbir şey değiştirilmedi.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% kullanılamıyor; paylaşılan modeller klasörü bulunamıyor.",
    .gguf_manager_err_write = L"registry.json yazılamadı.",
    .gguf_manager_err_write_partial = L"registry.json tamamen yazılamadı.",
    .gguf_manager_err_registry_damaged = L"registry.json hasarlı veya desteklenmeyen bir şemaya sahip. DeğiştirilMEdi. Düzeltin veya kaldırın ve yeniden deneyin.",
    .tooltip_translate_failed = L"Çeviri başarısız oldu. Lütfen tekrar deneyin.",
    .repair_transient_body = L"Yerel çeviri motoru geçici olarak kullanılamıyor (başlatılıyor olabilir). Lütfen biraz bekleyip tekrar deneyin.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Tamam",
    .dialog_cancel = L"İptal",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Dosyadan ekle…",
    .gguf_manager_add_hf = L"Hugging Face'den ekle…",
    .hf_add_title = L"Hugging Face modeli ekle",
    .hf_url_label = L"Model URL'si",
    .hf_downloading = L"İndiriliyor…",
    .hf_failed = L"İndirme başarısız oldu.",
    .hf_invalid_url = L"huggingface.co model URL'sini girin (resolve, blob veya model sayfası)",
    .hf_done = L"Kaydedildi.",
    .openai_base_url_hint = L"örn. https://api.openai.com/v1",
    .openai_api_key_hint = L"API anahtarı",
    // REQ-051: [Ayarları sil] düğmesi.
    .openai_delete_settings = L"Ayarları sil",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Güncelleştirmeleri denetle",
    .update_idle_note = L"En son Emebala Chat sürümünü kullanıp kullanmadığınızı görün.",
    .update_checking = L"Güncelleştirmeler denetleniyor…",
    .update_checking_hint = L"Arka planda çalışır; bu pencereyi kapatabilirsiniz.",
    .update_uptodate = L"En son sürümü kullanıyorsunuz",
    .update_dlg_title = L"Güncelleştirme mevcut",
    .update_dlg_version = L"Yeni sürüm",
    .update_dlg_version_cur = L"sürümünüz {v}",
    .update_dlg_size = L"Boyut",
    .update_dlg_size_approx = L"yaklaşık {s}",
    .update_dlg_privacy = L"Denetleme ve indirme GitHub'a bağlanır.",
    .update_dlg_download = L"İndir",
    .update_dlg_later = L"Daha sonra",
    .update_dl_progress = L"İndiriliyor…",
    .update_dl_cancel = L"İptal",
    .update_dl_cancelled = L"İndirme iptal edildi.",
    .update_ready_title = L"İndirme tamamlandı — kuruluma hazır",
    .update_ready_warn = L"Kurulum için uygulama kapanacak.",
    .update_ready_install = L"Şimdi kur",
    .update_ready_later = L"Daha sonra",
    .update_err_offline = L"Çevrimdışısınız. Bağlantınızı kontrol edip tekrar deneyin.",
    .update_err_rate = L"Çok fazla istek. Lütfen birazdan tekrar deneyin.",
    .update_err_hash = L"İndirilen dosya doğrulamayı geçemedi.",
    .update_err_retry = L"Yeniden dene",
    .update_snoozed_line = L"Güncelleştirmeyi daha sonra kurabilirsiniz.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Model indir",
    .model_dl_consent_body = L"Yerleşik çeviri modeli kurulu değil. Şimdi indirilsin mi? (yaklaşık {s})",
    .model_dl_privacy = L"İndirme huggingface.co adresine bağlanır.",
    .model_dl_download = L"Şimdi indir",
    .model_dl_verifying = L"İndirilen doğrulanıyor…",
    .model_dl_placing = L"Model kuruluyor…",
    .model_dl_done = L"Model hazır. Yerel motor bir sonraki çeviriden itibaren kullanacak.",
    .model_dl_failed = L"İndirme başarısız oldu. Bağlantınızı kontrol edip tekrar deneyin.",
    .model_dl_disk_space = L"Yetersiz boş disk alanı (yaklaşık {s} gerekli).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Çeviri modeli yükleme ayarları…",
    .engine_load_settings_title = L"Çeviri modeli yükleme",
    .load_mode_on_use = L"Kullanıldığında yükle (önerilir)",
    .load_mode_warmup = L"Önceden yükle",
    .load_mode_on_use_desc = L"Bellekten tasarruf sağlar. İlk çeviri birkaç saniye geç başlayabilir.",
    .load_mode_warmup_desc = L"Çeviri hemen başlar. Model bellekte kalır.",
    .gguf_policy_sticky = L"(Sürekli yüklü tut)",
    .gguf_policy_evict = L"(Kullanıldığında yükle)",
    .gguf_manager_toggle_policy = L"İlkeyi değiştir",
    .gguf_duplicate_warning = L"Yinelenen dosyalar: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Yinelenenleri temizle",
    .gguf_manager_dedupe_done = L"Yinelenen dosyalar Geri Dönüşüm Kutusu'na taşındı.",
    .gguf_manager_dedupe_failed = L"Yinelenen dosyalar temizlenemedi. Başka bir program tarafından kullanılıp kullanılmadığını kontrol edin.",
    .menu_ui_theme = L"Arayüz teması",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Sıcak Editorial",
    .theme_monolith = L"Rafine monolit",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Yüzen geri çeviri göster (doğrulama)",
    .back_translate_header = L"🔄 Geri çeviri kontrolü",
    .back_translate_match_exact = L"✓ Anlam eşleşti",
    .back_translate_match_check = L"⚡ Nüansı kontrol et",
    .back_translate_reference = L"↳ Orijinal: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Geçerli bir GGUF model dosyası değil; kaydedilmedi.",
    .gguf_manager_active_suffix = L"(kullanımda)",
    .gguf_manager_activated = L"Bu model artık kullanımda.",
};
// 24. Ukrainian (uk)
const LocalizedStrings kStringsUkrainian = {
    .menu_status_active = L"Стан: активний (F9: пауза)",
    .menu_status_paused = L"Стан: призупинено (F9: відновити)",
    .menu_engine = L"Рушій перекладу",
    .menu_engine_google = L"Google Перекладач (безкоштовно / без встановлення)",
    .menu_engine_local = L"Вбудований локальний рушій (Hy-MT2-1.8B офлайн)",
    .menu_source_lang = L"Мова джерела (вхід)",
    .menu_target_lang = L"Цільова мова (вихід)",
    .menu_swap_langs = L"Помняти джерело ⇄ ціль (подвійний клік)",
    .menu_auto_send = L"Автонадсилання Enterом",
    .menu_sound_feedback = L"Звуковий відгук (тони)",
    .menu_show_badge = L"Плаваюча відзнака видима",
    .menu_start_with_windows = L"Запускати з Windows",
    .menu_cheatsheet = L"Гарячі клавіші та довідка...",
    .menu_exit = L"Вийти з Емебала Чат",
    .menu_about = L"Про Емебала Чат…",
    .cheatsheet_title = L"Емебала Чат — гарячі клавіші та посібник",
    .cheatsheet_body = L"Гарячі клавіші та посібник Емебала Чат:\n\n"
    L"  • F9 : увімкнути / пауза\n"
    L"  • Ctrl + F9 : змінити цільову мову\n"
    L"  • Ctrl + Shift + Enter : перемкнути автонадсилання\n"
    L"  • Shift + Enter : перекласти й надіслати одразу\n\n"
    L"Керування мишею на відзначці:\n"
    L"  • Ляжий клік : увімкнути / пауза\n"
    L"  • Подвійний клік : поміняти джерело ⇄ ціль\n"
    L"  • Правий клік : відкрити меню налаштувань\n\n"
    L"Режими перекладу:\n"
    L"  • Лише заміна (автонадсилання вимк): замінює рядок перекладом для перевірки.\n"
    L"  • Автонадсилання (увімк): замінює рядок і одразу натискає Enter.",
    .about_title = L"Про Емебала Чат",
    .badge_active = L"Активний",
    .badge_translating = L"Переклад...",
    .badge_paused = L"Призупинено",
    .tooltip_title = L"Емебала Чат",
    .tooltip_copy_failed = L"Не вдалося скопіювати вибраний текст. Перевірте цільовий застосунок і спробуйте ще раз.",
    .tooltip_no_selection = L"Не вибрано тексту для перекладу.",
    .auto_detect = L"Визначати автоматично",
    .app_already_running = L"Емебала Чат уже працює у фоні.\nПеревірте область сповіщень.",
    .app_com_failed = L"Помилка ініціалізації COM.\nПлаваюча відзнака й озвучення тексту будуть недоступні,\nале переклад, гарячі клавіші, трей і звуки продовжують працювати.",
    .about_tagline = L"Геть копіювання та вставляння. Друкуйте природно рідною мовою — переклад замінює ваш набір у реальному часі в будь-якій програмі Windows.",
    .about_feature0 = L"⚡ Перетягни, щоб перекласти — виділіть текст у будь-якій програмі, плаваюча піктограма перекрить миттєво.",
    .about_feature1 = L"🔊 Нейронне TTS — озвучує всі 37 мов через установлені голосові пакети Windows.",
    .about_feature2 = L"🔒 100% на пристрої та приватно — активно лише доки утримується скорочення; буфер обміну не чіпается.",
    .about_etymology = L"У 2000 р. до н. е. месопотамські писці називали «Eme-bala» тих, хто перетворює мову на міст між світами.",
    .about_link_website = L"Вебсайт",
    .about_link_contact = L"Контакт",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Робочі години 10:00–19:00 KST",
    .about_contact_lead = L"Головний архітектор: Yongtai Kim",
    .tooltip_copied = L"✓ Скопійовано!",
    .tooltip_button_copy = L"📋 Копіювати",
    .tooltip_button_tts = L"🔊 Озвучення",
    .menu_ui_language = L"Мова інтерфейсу",
    .menu_ui_language_auto = L"Авто (системна мова)",
    .about_reset_button = L"Скинути до системних типових",
    .about_reset_done = L"Типові значення відновлено",
    .menu_typing_group = L"Набирання з клавіатури",
    .menu_tooltip_group = L"Підказка перетягування",
    .app_name = L"Емебала Чат",
    .tooltip_no_tts_voice = L"Для цієї мови не встановлено голос Windows. Натисніть 🔊 ще раз, щоб відкрити параметри мовлення.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Повідомлення про конфіденційність",
    .privacy_notice_body = L"Ось принципи обробки даних в Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat не використовує власних серверів.\n"
    L"• З локальною моделлю текст перекладу не покидає вашого пристрою.\n"
    L"• Якщо обрано Google Перекладач або відбувається автоматичний перехід у хмару, виділений або надрукований текст надсилається напряму до Google для перекладу, минаючи Emebala.\n"
    L"• Діагностичні журнали типово ВИМКНЕНО; увімкніть їх у налаштуваннях (за згодою).\n"
    L"• Якщо не хочете надсилати текст у хмару (Google), відкрийте меню піктограми в системному треї, оберіть «Рушій перекладу» і потім «Вбудований локальний рушій». Без встановленої локальної моделі та з вимкненим хмарним резервуванням переклад не виконується — нічого не надсилається.\n"
    L"\n"
    L"Повні відомості — у файлі README, який можна перечитати будь-коли.\n",
    .cheatsheet_config_path = L"Файл налаштувань: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Текст надто довгий: перекладено лише початок і кінець.",
    .tooltip_untranslated_above = L"Вище є новий неперекладений текст. Поставте курсор у кінець цього рядка та натисніть Enter, щоб перекласти.",
    .repair_in_progress = L"Відновлення компонентів локального рушія…",
    .repair_failed_title = L"Локальний переклад недоступний",
    .repair_failed_body = L"Файли локального рушія перекладу не знайдено, тому переклад призупинено. Переустановіть Emebala Chat, щоб відновити локальний рушій, або перейдіть на хмарний (Google) переклад, вибравши «Google Перекладач» у меню області сповіщень, «Рушій перекладу».",
    .menu_engine_openai = L"Сумісно з OpenAI (користувацький сервер)…",
    .openai_settings_title = L"Налаштування сумісного з OpenAI рушія",
    .openai_settings_action = L"Налаштування сумісного з OpenAI рушія…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"Ключ API",
    .openai_model_label = L"Модель",
    .openai_fetch_models = L"Отримати список моделей",
    .openai_fetch_failed = L"Не вдалося отримати список моделей. Ви можете ввести назву моделі безпосередньо.",
    .openai_http_warning_title = L"Незахищене з'єднання (HTTP)",
    .openai_http_warning_body = L"Base URL використовує HTTP (без шифрування). Ваш ключ API і текст будуть надіслані у відкритому вигляді. Продовжити?",
    .openai_saved = L"Сумісні з OpenAI налаштування збережено.",
    .openai_key_masked = L"Збережений ключ: ",
    .openai_invalid_base_url = L"Неприпустимий Base URL. Приклад: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Модель, вказана користувачем (.gguf)",
    .menu_browse_gguf_file = L"Зареєструвати іншу модель .gguf…",
    .user_gguf_quality_title = L"Повідомлення про якість перекладу",
    .user_gguf_quality_body = L"Вибрана модель не Hy-MT2. Поточна версія використовує промпт лише для Hy-MT2, тому якість перекладу цією моделлю не гарантується. Продовжити?",
    .user_gguf_registered_title = L"Модель зареєстровано",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Вибрану модель зареєстровано в локальному рушії.\n"
    L"\n"
    L"Ця модель використовується для перекладу, коли ви вибираєте «Вибір рушія перекладу > Вибір користувача (.gguf)».\n"
    L"\n"
    L"Коли набирає чинності: не більше приблизно 1 хвилини після реєстрації (після завершення роботи рушія через бездіяльність)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Ця модель уже вбудована в Emebala Chat. Реєстрація не потрібна. Ви можете напряму вибрати вбудований локальний рушій перекладу.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(не зареєстровано)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Керування моделями…",
    .gguf_manager_title = L"Керування моделями користувача",
    .gguf_manager_empty = L"Немає зареєстрованих моделей користувача.",
    .gguf_manager_rename = L"Перейменувати…",
    .gguf_manager_delete = L"Видалити…",
    .gguf_manager_close = L"Закрити",
    .gguf_manager_delete_confirm_title = L"Видалення реєстрації моделі",
    .gguf_manager_delete_confirm_body = L"Реєстрацію вибраної моделі буде видалено.\n"
    L"\n"
    L"Файл моделі (.gguf) залишається на диску і не видаляється. Якщо цю модель було використано, вибір рушія перекладу повернеться до Авто (Auto).\n"
    L"\n"
    L"Продовжити?",
    .gguf_manager_rename_title = L"Перейменування моделі",
    .gguf_manager_rename_body = L"Введіть нову назву (до 64 символів, без пробілів).",
    .gguf_manager_rename_invalid = L"Цю назву не можна використати. Введіть непорожню назву, що відрізняється від наявних, без пробілів і роздільників шляху, до 64 символів.",
    .gguf_manager_done = L"Зміни збережено.",
    .gguf_manager_err_serialize = L"Не вдалося серіалізувати registry.json (назву файлу відхилено). Нічого не змінено.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% недоступний; не вдається знайти спільну папку моделей.",
    .gguf_manager_err_write = L"Не вдалося записати registry.json.",
    .gguf_manager_err_write_partial = L"Не вдалося повністю записати registry.json.",
    .gguf_manager_err_registry_damaged = L"registry.json пошкоджено або має непідтримувану схему. Він НЕ змінений. Відновіть його або видаліть і повторіть спробу.",
    .tooltip_translate_failed = L"Переклад не вдався. Спробуйте ще раз.",
    .repair_transient_body = L"Локальний рушій перекладу тимчасово недоступний (можливо, запускається). Зачекайте трохи й повторіть спробу.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Підтвердити",
    .dialog_cancel = L"Скасувати",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Додати з файлу…",
    .gguf_manager_add_hf = L"Додати з Hugging Face…",
    .hf_add_title = L"Додати модель Hugging Face",
    .hf_url_label = L"URL-адреса моделі",
    .hf_downloading = L"Завантаження…",
    .hf_failed = L"Не вдалося завантажити.",
    .hf_invalid_url = L"Введіть URL-адресу моделі huggingface.co (resolve, blob або сторінка моделі)",
    .hf_done = L"Зареєстровано.",
    .openai_base_url_hint = L"напр.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Ключ API",
    // REQ-051: кнопка [Видалити налаштування].
    .openai_delete_settings = L"Видалити налаштування",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Перевірити оновлення",
    .update_idle_note = L"Перевірте, чи ви використовуєте найновішу версію Emebala Chat.",
    .update_checking = L"Перевірка оновлень…",
    .update_checking_hint = L"Працює у фоні; ви можете закрити це вікно.",
    .update_uptodate = L"У вас найновіша версія",
    .update_dlg_title = L"Доступне оновлення",
    .update_dlg_version = L"Нова версія",
    .update_dlg_version_cur = L"у вас {v}",
    .update_dlg_size = L"Розмір",
    .update_dlg_size_approx = L"близько {s}",
    .update_dlg_privacy = L"Перевірка та завантаження з'єднуються з GitHub.",
    .update_dlg_download = L"Завантажити",
    .update_dlg_later = L"Пізніше",
    .update_dl_progress = L"Завантаження…",
    .update_dl_cancel = L"Скасувати",
    .update_dl_cancelled = L"Завантаження скасовано.",
    .update_ready_title = L"Завантаження завершено — готово до встановлення",
    .update_ready_warn = L"Застосунок закриється для встановлення.",
    .update_ready_install = L"Встановити зараз",
    .update_ready_later = L"Пізніше",
    .update_err_offline = L"Ви офлайн. Перевірте з'єднання та повторіть спробу.",
    .update_err_rate = L"Занадто багато запитів. Повторіть спробу трохи згодом.",
    .update_err_hash = L"Завантажений файл не пройшов перевірку.",
    .update_err_retry = L"Повторити",
    .update_snoozed_line = L"Ви можете встановити оновлення пізніше.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Завантаження моделі",
    .model_dl_consent_body = L"Вбудовану модель перекладу не встановлено. Завантажити зараз? (близько {s})",
    .model_dl_privacy = L"Завантаження з'єднується з huggingface.co.",
    .model_dl_download = L"Завантажити зараз",
    .model_dl_verifying = L"Перевірка завантаженого файлу…",
    .model_dl_placing = L"Встановлення моделі…",
    .model_dl_done = L"Модель готова. Локальний рушій використовуватиме її з наступного перекладу.",
    .model_dl_failed = L"Завантаження не вдалося. Перевірте з'єднання та повторіть спробу.",
    .model_dl_disk_space = L"Недостатньо вільного місця на диску (потрібно близько {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Налаштування завантаження моделі перекладу…",
    .engine_load_settings_title = L"Завантаження моделі перекладу",
    .load_mode_on_use = L"Завантажувати під час використання (рекомендовано)",
    .load_mode_warmup = L"Завантажувати заздалегідь",
    .load_mode_on_use_desc = L"Заощаджує пам'ять. Перший переклад може початися на кілька секунд пізніше.",
    .load_mode_warmup_desc = L"Переклад починається одразу. Модель постійно залишається в пам'яті.",
    .gguf_policy_sticky = L"(Лишати в пам'яті)",
    .gguf_policy_evict = L"(Завантажувати під час використання)",
    .gguf_manager_toggle_policy = L"Змінити політику",
    .gguf_duplicate_warning = L"Дублікати файлів: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Очистити дублікати",
    .gguf_manager_dedupe_done = L"Дублікати файлів переміщено в Кошик.",
    .gguf_manager_dedupe_failed = L"Не вдалося очистити дублікати файлів. Перевірте, чи не використовує їх інша програма.",
    .menu_ui_theme = L"Тема інтерфейсу",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Теплий Editorial",
    .theme_monolith = L"Вигладний моноліт",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Плаваючий зворотний переклад (перевірка)",
    .back_translate_header = L"🔄 Перевірка зворотного перекладу",
    .back_translate_match_exact = L"✓ Зміст збігається",
    .back_translate_match_check = L"⚡ Перевірити відтінок",
    .back_translate_reference = L"↳ Оригінал: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Це недійсний файл моделі GGUF; він не зареєстрований.",
    .gguf_manager_active_suffix = L"(використовується)",
    .gguf_manager_activated = L"Цю модель зараз використовується.",
};
// 25. Thai (th)
const LocalizedStrings kStringsThai = {
    .menu_status_active = L"สถานะ: ใช้งานอยู่ (F9: หยุดชั่วคราว)",
    .menu_status_paused = L"สถานะ: หยุดชั่วคราว (F9: ทำงานต่อ)",
    .menu_engine = L"ระบบแปลภาษา",
    .menu_engine_google = L"Google แปลภาษา (ฟรี / ไม่ต้องติดตั้ง)",
    .menu_engine_local = L"เครื่องยนต์ในตัวแบบออฟไลน์ (Hy-MT2-1.8B ออฟไลน์)",
    .menu_source_lang = L"ภาษาต้นทาง (อินพุต)",
    .menu_target_lang = L"ภาษาปลายทาง (เอาต์พุต)",
    .menu_swap_langs = L"สลับต้นทาง ⇄ ปลายทาง (ดับเบิลคลิก)",
    .menu_auto_send = L"ส่งอัตโนมัติเมื่อกด Enter",
    .menu_sound_feedback = L"เสียงตอบกลับ (โทนเสียง)",
    .menu_show_badge = L"แสดงป้ายลอย",
    .menu_start_with_windows = L"เริ่มทำงานพร้อม Windows",
    .menu_cheatsheet = L"สรุปคีย์ลัดและวิธีใช้...",
    .menu_exit = L"ออกจาก เอเมบาลา แชท",
    .menu_about = L"เกี่ยวกับ เอเมบาลา แชท…",
    .cheatsheet_title = L"เอเมบาลา แชท — คีย์ลัดและคู่มือการใช้งาน",
    .cheatsheet_body = L"คีย์ลัดและคู่มือการใช้งาน เอเมบาลา แชท:\n\n"
    L"  • F9 : เปิดใช้งาน / หยุดชั่วคราว\n"
    L"  • Ctrl + F9 : เปลี่ยนภาษาปลายทาง\n"
    L"  • Ctrl + Shift + Enter : สลับโหมดส่งอัตโนมัติ\n"
    L"  • Shift + Enter : แปลแล้วส่งทันที\n\n"
    L"การควบคุมด้วยเมาส์บนป้ายลอย:\n"
    L"  • คลิกซ้าย : เปิดใช้งาน / หยุดชั่วคราว\n"
    L"  • ดับเบิลคลิก : สลับต้นทาง ⇄ ปลายทาง\n"
    L"  • คลิกขวา : เปิดเมนูการตั้งค่า\n\n"
    L"โหมดการแปล:\n"
    L"  • แทนที่อย่างเดียว (ส่งอัตโนมัติปิด): แทนที่บรรทัดด้วยการแปลเพื่อตรวจสอบก่อน\n"
    L"  • ส่งอัตโนมัติ (เปิด): แทนที่บรรทัดแล้วกด Enter ทันที",
    .about_title = L"เกี่ยวกับ เอเมบาลา แชท",
    .badge_active = L"ใช้งานอยู่",
    .badge_translating = L"กำลังแปล...",
    .badge_paused = L"หยุดชั่วคราว",
    .tooltip_title = L"เอเมบาลา แชท",
    .tooltip_copy_failed = L"คัดลอกข้อความที่เลือกไม่ได้ โปรดตรวจสอบแอปปลายทางแล้วลองอีกครั้ง",
    .tooltip_no_selection = L"ยังไม่ได้เลือกข้อความสำหรับแปล",
    .auto_detect = L"ตรวจจับอัตโนมัติ",
    .app_already_running = L"เอเมบาลา แชท ทำงานอยู่แล้วในพื้นหลัง\nโปรดตรวจสอบถาดการแจ้งเตือนของระบบ",
    .app_com_failed = L"ไม่สามารถเริ่มการทำงาน COM ได้\nป้ายลอยและการอ่านออกเสียงจะไม่สามารถใช้งานได้\nแต่การแปล คีย์ลัด ถาดระบบ และเสียงจะยังทำงานตามปกติ",
    .about_tagline = L"เลิกลั่บการคัดลอกวาง พิมพ์ตามธรรมชาติในภาษาแม่ของคุณ แล้วคำแปลจะมาแทนที่สิ่งที่คุณพิมพ์แบบเรียลไทม์ในทุกแอปของ Windows",
    .about_feature0 = L"⚡ ลากเพื่อแปล — เลือกข้อความในแอปใดก็ได้ ไอคอนลอยจะแปลให้ทันที",
    .about_feature1 = L"🔊 TTS แบบประสาท — ออกเสียงได้ครบทั้ง 37 ภาษาเมื่อติดตั้งชุดเสียงของ Windows",
    .about_feature2 = L"🔒 ความเป็นส่วนตัว 100% บนอุปกรณ์ — ทำงานเฉพาะตอนกดคีย์ลัดเท่านั้น ไม่แตะคลิปบอร์ด",
    .about_etymology = L"ในปี 2000 ปีก่อนคริสตกาล เสมียนเมโสโปเตเมียเรียกผู้ที่เปลี่ยนภาษาให้เป็นสะพานเชื่อมโลกว่า «Eme-bala»",
    .about_link_website = L"เว็บไซต์",
    .about_link_contact = L"ติดต่อ",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · โซล ยองดึงโป (ห้อง 219, 65 ยองจุง-โร)",
    .about_contact_phone = L"+82 2 575 0414 · เวลาทำการ 10:00–19:00 KST",
    .about_contact_lead = L"สถาปนิกหลัก: Yongtai Kim",
    .tooltip_copied = L"✓ คัดลอกแล้ว!",
    .tooltip_button_copy = L"📋 คัดลอก",
    .tooltip_button_tts = L"🔊 อ่านออกเสียง",
    .menu_ui_language = L"ภาษาอินเทอร์เฟซ",
    .menu_ui_language_auto = L"อัตโนมัติ (ภาษาของระบบ)",
    .about_reset_button = L"คืนค่าเป็นค่าเริ่มต้นของระบบ",
    .about_reset_done = L"กู้คืนค่าเริ่มต้นแล้ว",
    .menu_typing_group = L"การพิมพ์ด้วยคีย์บอร์ด",
    .menu_tooltip_group = L"ทูลทิปแบบลาก",
    .app_name = L"เอเมบาลา แชท",
    .tooltip_no_tts_voice = L"ยังไม่ได้ติดตั้งเสียงของ Windows สำหรับภาษานี้ คลิก 🔊 อีกครั้งเพื่อเปิดการตั้งค่าคำพูด",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"ประกาศด้านความเป็นส่วนตัว",
    .privacy_notice_body = L"หลักเกณฑ์ด้านความเป็นส่วนตัวของ Emebala Chat มีดังนี้\n"
    L"\n"
    L"• Emebala Chat ไม่มีเซิร์ฟเวอร์ของตนเอง\n"
    L"• เมื่อใช้โมเดลในเครื่อง ข้อความที่แปลจะไม่ออกจากอุปกรณ์ของคุณ\n"
    L"• หากคุณเลือก Google Translate หรือสลับไปใช้ระบบคลาวด์อัตโนมัติ ข้อความที่คุณเลือกหรือพิมพ์จะถูกส่งตรงไปยัง Google เพื่อแปล โดยไม่ผ่าน Emebala\n"
    L"• บันทึกการวินิจฉัยปิด (OFF) เป็นค่าเริ่มต้น ต้องเปิดในการตั้งค่า (เลือกเข้าร่วม)\n"
    L"• หากคุณไม่ต้องการส่งข้อความไปยังคลาวด์ (Google) ให้เปิดเมนูที่ไอคอนถาดระบบ เลือก “ระบบแปลภาษา” แล้วเลือก “เครื่องยนต์ในตัวแบบออฟไลน์” หากไม่ได้ติดตั้งโมเดลภายในเครื่องและปิดการสลับไปคลาวด์ไว้ การแปลจะไม่ทำงานโดยไม่มีการส่งข้อมูลใดๆ\n"
    L"\n"
    L"ดูรายละเอียดฉบับเต็มในไฟล์ README ซึ่งอ่านซ้ำได้ทุกเมื่อ\n",
    .cheatsheet_config_path = L"ไฟล์ config: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"ข้อความยาวเกินไป จึงแปลเฉพาะส่วนต้นและส่วนท้าย",
    .tooltip_untranslated_above = L"มีข้อความใหม่ที่ยังไม่ได้แปลด้านบน วางเคอร์เซอร์ที่ท้ายบรรทัดนั้นแล้วกด Enter เพื่อแปล",
    .repair_in_progress = L"กำลังซ่อมแซมส่วนประกอบเอนจิ้นในเครื่อง…",
    .repair_failed_title = L"การแปลในเครื่องไม่พร้อมใช้งาน",
    .repair_failed_body = L"ไม่พบไฟล์เอนจิ้นแปลในเครื่อง จึงหยุดการแปลชั่วคราว ติดตั้ง Emebala Chat อีกครั้งเพื่อกู้คืนเอนจิ้นในเครื่อง หรือหากต้องการสลับไปใช้การแปลบนคลาวด์ (Google) ให้เลือก “Google แปลภาษา” จากเมนูถาดระบบ ที่ “เอนจิ้นการแปล”",
    .menu_engine_openai = L"เข้ากันได้กับ OpenAI (เซิร์ฟเวอร์ที่ผู้ใช้กำหนด)…",
    .openai_settings_title = L"การตั้งค่าเอนจินแบบ OpenAI",
    .openai_settings_action = L"การตั้งค่าเอนจินแบบ OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"คีย์ API",
    .openai_model_label = L"โมเดล",
    .openai_fetch_models = L"ดึงรายการโมเดล",
    .openai_fetch_failed = L"ไม่สามารถดึงรายการโมเดลได้ คุณสามารถพิมพ์ชื่อโมเดลโดยตรง",
    .openai_http_warning_title = L"การเชื่อมต่อที่ไม่ปลอดภัย (HTTP)",
    .openai_http_warning_body = L"Base URL ใช้ HTTP (ไม่ได้เข้ารหัส) คีย์ API และข้อความของคุณจะถูกส่งแบบไม่เข้ารหัส ดำเนินการต่อ?",
    .openai_saved = L"บันทึกการตั้งค่าแบบ OpenAI แล้ว",
    .openai_key_masked = L"คีย์ที่บันทึก: ",
    .openai_invalid_base_url = L"Base URL ไม่ถูกต้อง ตัวอย่าง: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"โมเดลที่ผู้ใช้กำหนด (.gguf)",
    .menu_browse_gguf_file = L"ลงทะเบียนโมเดล .gguf อื่น…",
    .user_gguf_quality_title = L"ประกาศเกี่ยวกับคุณภาพการแปล",
    .user_gguf_quality_body = L"โมเดลที่เลือกไม่ใช่ Hy-MT2 เวอร์ชันปัจจุบันใช้พรอมต์เฉพาะสำหรับ Hy-MT2 ดังนั้นคุณภาพการแปลด้วยโมเดลนี้จึงไม่ได้รับประกัน ดำเนินการต่อ?",
    .user_gguf_registered_title = L"ลงทะเบียนโมเดลแล้ว",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"ลงทะเบียนโมเดลที่เลือกกับเอนจินในเครื่องแล้ว\n"
    L"\n"
    L"โมเดลนี้จะถูกใช้แปลเมื่อคุณเลือก \"เลือกเอนจินแปล > ผู้ใช้เลือก (.gguf)\"\n"
    L"\n"
    L"เมื่อมีผล: ภายในไม่เกินประมาณ 1 นาทีหลังลงทะเบียน (หลังเอนจินหยุดทำงานจากการไม่ได้ใช้งาน)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"โมเดลนี้มีอยู่ในตัว Emebala Chat อยู่แล้ว ไม่จำเป็นต้องลงทะเบียน เลือกเครื่องยนต์แปลภาษาในตัวได้โดยตรง",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ยังไม่ได้ลงทะเบียน)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"จัดการโมเดล…",
    .gguf_manager_title = L"ตัวจัดการโมเดลผู้ใช้",
    .gguf_manager_empty = L"ไม่มีโมเดลผู้ใช้ที่ลงทะเบียน",
    .gguf_manager_rename = L"เปลี่ยนชื่อ…",
    .gguf_manager_delete = L"ลบ…",
    .gguf_manager_close = L"ปิด",
    .gguf_manager_delete_confirm_title = L"ลบการลงทะเบียนโมเดล",
    .gguf_manager_delete_confirm_body = L"การลงทะเบียนของโมเดลที่เลือกจะถูกลบ\n"
    L"\n"
    L"ไฟล์โมเดล (.gguf) ยังคงอยู่บนดิสก์และจะไม่ถูกลบ หากใช้โมเดลนี้อยู่ การเลือกเอนจินแปลจะกลับไปที่อัตโนมัติ (Auto)\n"
    L"\n"
    L"ดำเนินการต่อ?",
    .gguf_manager_rename_title = L"เปลี่ยนชื่อโมเดล",
    .gguf_manager_rename_body = L"ป้อนชื่อใหม่ (สูงสุด 64 อักขระ ไม่มีช่องว่าง)",
    .gguf_manager_rename_invalid = L"ใช้ชื่อนี้ไม่ได้ โปรดป้อนชื่อที่ไม่ว่าง แตกต่างจากที่มีอยู่ ไม่มีช่องว่างหรือตัวคั่นเส้นทาง ไม่เกิน 64 อักขระ",
    .gguf_manager_done = L"บันทึกการเปลี่ยนแปลงแล้ว",
    .gguf_manager_err_serialize = L"ไม่สามารถ serialize registry.json ได้ (ชื่อไฟล์ถูกปฏิเสธ) ไม่มีการเปลี่ยนแปลงใดๆ",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% ไม่พร้อมใช้งาน ไม่พบโฟลเดอร์โมเดลที่แชร์",
    .gguf_manager_err_write = L"ไม่สามารถเขียน registry.json ได้",
    .gguf_manager_err_write_partial = L"เขียน registry.json ไม่เสร็จสมบูรณ์",
    .gguf_manager_err_registry_damaged = L"registry.json เสียหายหรือมี schema ที่ไม่รองรับ ไม่ได้ถูกแก้ไข โปรดซ่อมแซมหรือลบแล้วลองใหม่",
    .tooltip_translate_failed = L"การแปลล้มเหลว โปรดลองใหม่",
    .repair_transient_body = L"เอนจินแปลในเครื่องไม่พร้อมใช้งานชั่วคราว (อาจกำลังเริ่มต้น) โปรดรอครู่แล้วลองใหม่",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"ตกลง",
    .dialog_cancel = L"ยกเลิก",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"เพิ่มจากไฟล์…",
    .gguf_manager_add_hf = L"เพิ่มจาก Hugging Face…",
    .hf_add_title = L"เพิ่มโมเดล Hugging Face",
    .hf_url_label = L"URL ของโมเดล",
    .hf_downloading = L"กำลังดาวน์โหลด…",
    .hf_failed = L"ดาวน์โหลดไม่สำเร็จ",
    .hf_invalid_url = L"ป้อน URL โมเดลจาก huggingface.co (resolve, blob หรือหน้าโมเดล)",
    .hf_done = L"ลงทะเบียนแล้ว",
    .openai_base_url_hint = L"เช่น https://api.openai.com/v1",
    .openai_api_key_hint = L"คีย์ API",
    // REQ-051: ปุ่ม [ลบการตั้งค่า]
    .openai_delete_settings = L"ลบการตั้งค่า",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"ตรวจสอบการอัปเดต",
    .update_idle_note = L"ตรวจสอบว่าคุณกำลังใช้ Emebala Chat เวอร์ชันล่าสุดหรือไม่",
    .update_checking = L"กำลังตรวจสอบการอัปเดต…",
    .update_checking_hint = L"ทำงานในเบื้องหลัง คุณสามารถปิดหน้าต่างนี้ได้",
    .update_uptodate = L"คุณกำลังใช้เวอร์ชันล่าสุดแล้ว",
    .update_dlg_title = L"มีการอัปเดตใหม่",
    .update_dlg_version = L"เวอร์ชันใหม่",
    .update_dlg_version_cur = L"คุณกำลังใช้ {v}",
    .update_dlg_size = L"ขนาด",
    .update_dlg_size_approx = L"ประมาณ {s}",
    .update_dlg_privacy = L"การตรวจสอบและดาวน์โหลดจะเชื่อมต่อกับ GitHub",
    .update_dlg_download = L"ดาวน์โหลด",
    .update_dlg_later = L"ภายหลัง",
    .update_dl_progress = L"กำลังดาวน์โหลด…",
    .update_dl_cancel = L"ยกเลิก",
    .update_dl_cancelled = L"ยกเลิกการดาวน์โหลดแล้ว",
    .update_ready_title = L"ดาวน์โหลดเสร็จสมบูรณ์ — พร้อมติดตั้ง",
    .update_ready_warn = L"แอปจะปิดเพื่อติดตั้ง",
    .update_ready_install = L"ติดตั้งเลย",
    .update_ready_later = L"ภายหลัง",
    .update_err_offline = L"คุณออฟไลน์อยู่ ตรวจสอบการเชื่อมต่อแล้วลองอีกครั้ง",
    .update_err_rate = L"คำขอมากเกินไป โปรดลองอีกครั้งในภายหลัง",
    .update_err_hash = L"ไฟล์ที่ดาวน์โหลดไม่ผ่านการตรวจสอบ",
    .update_err_retry = L"ลองอีกครั้ง",
    .update_snoozed_line = L"คุณสามารถติดตั้งการอัปเดตภายหลังได้",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"ดาวน์โหลดโมเดล",
    .model_dl_consent_body = L"โมเดลแปลภาษาในตัวยังไม่ได้ติดตั้ง ดาวน์โหลดตอนนี้หรือไม่ (ประมาณ {s})",
    .model_dl_privacy = L"การดาวน์โหลดจะเชื่อมต่อกับ huggingface.co",
    .model_dl_download = L"ดาวน์โหลดเลย",
    .model_dl_verifying = L"กำลังตรวจสอบไฟล์ที่ดาวน์โหลด…",
    .model_dl_placing = L"กำลังติดตั้งโมเดล…",
    .model_dl_done = L"โมเดลพร้อมใช้งาน เครื่องยนต์ในเครื่องจะใช้งานตั้งแต่การแปลครั้งถัดไป",
    .model_dl_failed = L"ดาวน์โหลดไม่สำเร็จ ตรวจสอบการเชื่อมต่อแล้วลองอีกครั้ง",
    .model_dl_disk_space = L"พื้นที่ดิสก์ว่างไม่เพียงพอ (ต้องการประมาณ {s})",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"การตั้งค่าการโหลดโมเดลแปล…",
    .engine_load_settings_title = L"การโหลดโมเดลแปล",
    .load_mode_on_use = L"โหลดเมื่อใช้งาน (แนะนำ)",
    .load_mode_warmup = L"โหลดล่วงหน้า",
    .load_mode_on_use_desc = L"ประหยัดหน่วยความจำ การแปลครั้งแรกอาจเริ่มช้ากว่าปกติไม่กี่วินาที",
    .load_mode_warmup_desc = L"เริ่มแปลทันที โมเดลจะถูกเก็บไว้ในหน่วยความจำตลอด",
    .gguf_policy_sticky = L"(คงอยู่ในหน่วยความจำ)",
    .gguf_policy_evict = L"(โหลดเมื่อใช้งาน)",
    .gguf_manager_toggle_policy = L"สลับนโยบาย",
    .gguf_duplicate_warning = L"ไฟล์ซ้ำ: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ล้างไฟล์ซ้ำ",
    .gguf_manager_dedupe_done = L"ย้ายไฟล์ซ้ำไปยังถังรีไซเคิลแล้ว",
    .gguf_manager_dedupe_failed = L"ไม่สามารถล้างไฟล์ซ้ำได้ โปรดตรวจสอบว่าโปรแกรมอื่นกำลังใช้ไฟล์อยู่หรือไม่",
    .menu_ui_theme = L"ธีมส่วนติดต่อ",
    .theme_fluent = L"ฟลูเอ็นต์คราฟต์",
    .theme_editorial = L"วอร์มเอดิทอเรียล",
    .theme_monolith = L"รีไฟน์ดโมโนลิธ",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"แสดงการแปลย้อนกลับแบบลอย (ตรวจสอบ)",
    .back_translate_header = L"🔄 ตรวจสอบการแปลย้อนกลับ",
    .back_translate_match_exact = L"✓ ความหมายตรงกัน",
    .back_translate_match_check = L"⚡ ตรวจสอบน้ำเสียง",
    .back_translate_reference = L"↳ ต้นฉบับ: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"ไม่ใช่ไฟล์โมเดล GGUF ที่ถูกต้อง จึงไม่ได้ลงทะเบียน",
    .gguf_manager_active_suffix = L"(กำลังใช้งาน)",
    .gguf_manager_activated = L"ตั้งโมเดลนี้เป็นโมเดลที่ใช้งานอยู่แล้ว",
};
// 26. Indonesian (id)
const LocalizedStrings kStringsIndonesian = {
    .menu_status_active = L"Status: aktif (F9: jeda)",
    .menu_status_paused = L"Status: dijeda (F9: lanjutkan)",
    .menu_engine = L"Mesin penerjemah",
    .menu_engine_google = L"Google Translate (gratis / tanpa instalasi)",
    .menu_engine_local = L"Mesin lokal bawaan (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Bahasa sumber (masukan)",
    .menu_target_lang = L"Bahasa tujuan (keluaran)",
    .menu_swap_langs = L"Tukar sumber ⇄ tujuan (klik ganda)",
    .menu_auto_send = L"Kirim otomatis saat Enter",
    .menu_sound_feedback = L"Umpan balik suara (nada)",
    .menu_show_badge = L"Lambang mengambang terlihat",
    .menu_start_with_windows = L"Mulai bersama Windows",
    .menu_cheatsheet = L"Lembar contekan pintasan dan bantuan...",
    .menu_exit = L"Keluar dari Emebala Chat",
    .menu_about = L"Tentang Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — pintasan dan panduan",
    .cheatsheet_body = L"Pintasan dan panduan penggunaan Emebala Chat:\n\n"
    L"  • F9 : aktif / jeda\n"
    L"  • Ctrl + F9 : ganti bahasa tujuan\n"
    L"  • Ctrl + Shift + Enter : alih mode kirim otomatis\n"
    L"  • Shift + Enter : terjemahkan dan kirim seketika\n\n"
    L"Kontrol tetikus pada lambang:\n"
    L"  • Klik kiri : aktif / jeda\n"
    L"  • Klik ganda : tukar sumber ⇄ tujuan\n"
    L"  • Klik kanan : buka menu pengaturan\n\n"
    L"Mode penerjemahan:\n"
    L"  • Ganti saja (kirim otomatis mati): mengganti baris dengan terjemahan untuk ditinjau.\n"
    L"  • Kirim otomatis (nyala): mengganti baris lalu menekan Enter seketika.",
    .about_title = L"Tentang Emebala Chat",
    .badge_active = L"Aktif",
    .badge_translating = L"Menerjemahkan...",
    .badge_paused = L"Terdahenti",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Tidak dapat menyalin teks yang dipilih. Periksa aplikasi tujuan lalu coba lagi.",
    .tooltip_no_selection = L"Tidak ada teks yang dipilih untuk diterjemahkan.",
    .auto_detect = L"Deteksi otomatis",
    .app_already_running = L"Emebala Chat sudah berjalan di latar belakang.\nPeriksa baki notifikasi sistem.",
    .app_com_failed = L"Inisialisasi COM gagal.\nLambang mengambang dan teks-ke-suara tidak tersedia,\ntapi terjemahan, pintasan, baki, dan suara tetap berfungsi.",
    .about_tagline = L"Lupakan salin-tempel. Ketik secara alami dalam bahasa ibu Anda — terjemahan menggantikan ketikan Anda secara langsung di aplikasi Windows mana pun.",
    .about_feature0 = L"⚡ Seret-untuk-menerjemahkan — pilih teks di aplikasi mana pun, ikon mengambang menerjemahkan seketika.",
    .about_feature1 = L"🔊 TTS neural — mengucapkan semua 37 bahasa melalui paket suara Windows yang terpasang.",
    .about_feature2 = L"🔒 100% di perangkat & privat — aktif hanya saat pintasan ditekan; papan klip tak tersentuh.",
    .about_etymology = L"Pada 2000 SM, para juru tulis Mesopotamia menyebut «Eme-bala» — mereka yang menjelma bahasa menjadi jembatan antar-dunia.",
    .about_link_website = L"Situs web",
    .about_link_contact = L"Kontak",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Jam kerja 10:00–19:00 KST",
    .about_contact_lead = L"Arsitek utama: Yongtai Kim",
    .tooltip_copied = L"✓ Disalin!",
    .tooltip_button_copy = L"📋 Salin",
    .tooltip_button_tts = L"🔊 Suara",
    .menu_ui_language = L"Bahasa antarmuka",
    .menu_ui_language_auto = L"Otomatis (bahasa sistem)",
    .about_reset_button = L"Setel ulang ke setelan bawaan sistem",
    .about_reset_done = L"Setelan bawaan dipulihkan",
    .menu_typing_group = L"Pengetikan keyboard",
    .menu_tooltip_group = L"Tooltip seret",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Tidak ada suara Windows yang terpasang untuk bahasa ini. Klik 🔊 lagi untuk membuka pengaturan Ucapan.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Pemberitahuan privasi",
    .privacy_notice_body = L"Berikut prinsip privasi Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat tidak mengoperasikan server sendiri.\n"
    L"• Dengan model lokal, teks terjemahan tidak meninggalkan perangkat Anda.\n"
    L"• Jika Anda memilih Google Translate atau beralih otomatis ke cloud, teks yang dipilih atau diketik dikirim langsung ke Google untuk diterjemahkan, bukan melalui Emebala.\n"
    L"• Log diagnostik secara default MATI; aktifkan di pengaturan (opt-in).\n"
    L"• Jika tidak ingin mengirim teks ke cloud (Google), buka menu ikon di bilah tugas, pilih “Mesin penerjemah” lalu pilih “Mesin lokal bawaan”. Tanpa model lokal terpasang dan dengan cadangan cloud dinonaktifkan, terjemahan tidak berjalan — tidak ada yang dikirim.\n"
    L"\n"
    L"Detail lengkap ada di berkas README, yang dapat dibaca ulang kapan saja.\n",
    .cheatsheet_config_path = L"Berkas konfigurasi: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Teks terlalu panjang: hanya bagian awal dan akhir yang diterjemahkan.",
    .tooltip_untranslated_above = L"Ada teks baru yang belum diterjemahkan di atas. Letakkan kursor di akhir baris itu dan tekan Enter untuk menerjemahkan.",
    .repair_in_progress = L"Memperbaiki komponen mesin lokal…",
    .repair_failed_title = L"Terjemahan lokal tidak tersedia",
    .repair_failed_body = L"File mesin terjemahan lokal tidak ditemukan, jadi terjemahan dijeda. Instal ulang Emebala Chat untuk memulihkan mesin lokal, atau untuk beralih ke terjemahan cloud (Google), pilih “Google Terjemahan” dari menu baki, “Mesin terjemahan”.",
    .menu_engine_openai = L"Kompatibel OpenAI (server khusus)…",
    .openai_settings_title = L"Pengaturan mesin kompatibel OpenAI",
    .openai_settings_action = L"Pengaturan mesin kompatibel OpenAI…",
    .openai_base_url_label = L"URL dasar",
    .openai_api_key_label = L"Kunci API",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Ambil daftar model",
    .openai_fetch_failed = L"Daftar model tidak dapat diambil. Anda dapat mengetik nama model secara langsung.",
    .openai_http_warning_title = L"Koneksi tidak aman (HTTP)",
    .openai_http_warning_body = L"URL dasar menggunakan HTTP (tidak terenkripsi). Kunci API dan teks Anda akan dikirim tanpa enkripsi. Lanjutkan?",
    .openai_saved = L"Pengaturan kompatibel OpenAI disimpan.",
    .openai_key_masked = L"Kunci tersimpan: ",
    .openai_invalid_base_url = L"URL dasar tidak valid. Contoh: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Model yang ditentukan pengguna (.gguf)",
    .menu_browse_gguf_file = L"Daftarkan model .gguf lainnya…",
    .user_gguf_quality_title = L"Pemberitahuan kualitas terjemahan",
    .user_gguf_quality_body = L"Model yang dipilih bukan Hy-MT2. Versi saat ini menggunakan prompt khusus Hy-MT2, sehingga kualitas terjemahan dengan model ini tidak dijamin. Lanjutkan?",
    .user_gguf_registered_title = L"Model terdaftar",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Model yang dipilih telah didaftarkan ke mesin lokal.\n"
    L"\n"
    L"Model ini digunakan untuk terjemahan saat Anda memilih \"Pemilihan mesin terjemahan > Pilihan pengguna (.gguf)\".\n"
    L"\n"
    L"Kapan berlaku: paling lama sekitar 1 menit setelah pendaftaran (setelah mesin berhenti karena tidak aktif)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Model ini sudah tertanam di Emebala Chat. Pendaftaran tidak diperlukan. Pilih langsung mesin penerjemahan lokal bawaan.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(belum terdaftar)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Kelola model…",
    .gguf_manager_title = L"Pengelola Model Pengguna",
    .gguf_manager_empty = L"Tidak ada model pengguna yang terdaftar.",
    .gguf_manager_rename = L"Ubah nama…",
    .gguf_manager_delete = L"Hapus…",
    .gguf_manager_close = L"Tutup",
    .gguf_manager_delete_confirm_title = L"Hapus registrasi model",
    .gguf_manager_delete_confirm_body = L"Registrasi model yang dipilih akan dihapus.\n"
    L"\n"
    L"File model (.gguf) tetap disimpan di disk dan tidak dihapus. Jika model ini sedang digunakan, pemilihan mesin penerjemah akan kembali ke Otomatis (Auto).\n"
    L"\n"
    L"Lanjutkan?",
    .gguf_manager_rename_title = L"Ubah nama model",
    .gguf_manager_rename_body = L"Masukkan nama baru (maks. 64 karakter, tanpa spasi).",
    .gguf_manager_rename_invalid = L"Nama itu tidak dapat digunakan. Masukkan nama yang tidak kosong, berbeda dari yang sudah ada, tanpa spasi atau pemisah jalur, maksimal 64 karakter.",
    .gguf_manager_done = L"Perubahan disimpan.",
    .gguf_manager_err_serialize = L"registry.json tidak dapat diserialisasi (nama file ditolak). Tidak ada yang berubah.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% tidak tersedia; folder model bersama tidak dapat ditemukan.",
    .gguf_manager_err_write = L"registry.json tidak dapat ditulis.",
    .gguf_manager_err_write_partial = L"registry.json tidak dapat ditulis sepenuhnya.",
    .gguf_manager_err_registry_damaged = L"registry.json rusak atau memiliki skema yang tidak didukung. File TIDAK diubah. Perbaiki atau hapus, lalu coba lagi.",
    .tooltip_translate_failed = L"Terjemahan gagal. Silakan coba lagi.",
    .repair_transient_body = L"Mesin terjemahan lokal sementara tidak tersedia (mungkin sedang mulai). Tunggu sebentar dan coba lagi.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Oke",
    .dialog_cancel = L"Batal",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Tambah dari file…",
    .gguf_manager_add_hf = L"Tambah dari Hugging Face…",
    .hf_add_title = L"Tambah model Hugging Face",
    .hf_url_label = L"URL model",
    .hf_downloading = L"Mengunduh…",
    .hf_failed = L"Unduhan gagal.",
    .hf_invalid_url = L"Masukkan URL model huggingface.co (resolve, blob, atau halaman model)",
    .hf_done = L"Terdaftar.",
    .openai_base_url_hint = L"mis.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Kunci API",
    // REQ-051: tombol [Hapus pengaturan].
    .openai_delete_settings = L"Hapus pengaturan",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Periksa pembaruan",
    .update_idle_note = L"Lihat apakah Anda menjalankan Emebala Chat versi terbaru.",
    .update_checking = L"Memeriksa pembaruan…",
    .update_checking_hint = L"Berjalan di latar belakang; Anda dapat menutup jendela ini.",
    .update_uptodate = L"Anda menggunakan versi terbaru",
    .update_dlg_title = L"Pembaruan tersedia",
    .update_dlg_version = L"Versi baru",
    .update_dlg_version_cur = L"Anda menggunakan {v}",
    .update_dlg_size = L"Ukuran",
    .update_dlg_size_approx = L"sekitar {s}",
    .update_dlg_privacy = L"Memeriksa dan mengunduh akan terhubung ke GitHub.",
    .update_dlg_download = L"Unduh",
    .update_dlg_later = L"Nanti",
    .update_dl_progress = L"Mengunduh…",
    .update_dl_cancel = L"Batal",
    .update_dl_cancelled = L"Unduhan dibatalkan.",
    .update_ready_title = L"Unduhan selesai — siap dipasang",
    .update_ready_warn = L"Aplikasi akan ditutup untuk memasang.",
    .update_ready_install = L"Pasang sekarang",
    .update_ready_later = L"Nanti",
    .update_err_offline = L"Anda sedang luring. Periksa koneksi Anda, lalu coba lagi.",
    .update_err_rate = L"Terlalu banyak permintaan. Silakan coba lagi sebentar lagi.",
    .update_err_hash = L"File yang diunduh tidak lolos verifikasi.",
    .update_err_retry = L"Coba lagi",
    .update_snoozed_line = L"Anda dapat memasang pembaruan nanti.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Unduh model",
    .model_dl_consent_body = L"Model terjemahan bawaan belum dipasang. Unduh sekarang? (sekitar {s})",
    .model_dl_privacy = L"Mengunduh akan terhubung ke huggingface.co.",
    .model_dl_download = L"Unduh sekarang",
    .model_dl_verifying = L"Memverifikasi unduhan…",
    .model_dl_placing = L"Memasang model…",
    .model_dl_done = L"Model siap. Mesin lokal akan menggunakannya mulai terjemahan berikutnya.",
    .model_dl_failed = L"Unduhan gagal. Periksa koneksi Anda dan coba lagi.",
    .model_dl_disk_space = L"Ruang disk bebas tidak cukup (memerlukan sekitar {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Pengaturan pemuatan model terjemahan…",
    .engine_load_settings_title = L"Pemuatan model terjemahan",
    .load_mode_on_use = L"Muat saat digunakan (disarankan)",
    .load_mode_warmup = L"Muat di awal",
    .load_mode_on_use_desc = L"Hemat memori. Terjemahan pertama mungkin dimulai beberapa detik lebih lambat.",
    .load_mode_warmup_desc = L"Terjemahan langsung dimulai. Model tetap berada di memori.",
    .gguf_policy_sticky = L"(Tetap di memori)",
    .gguf_policy_evict = L"(Muat saat digunakan)",
    .gguf_manager_toggle_policy = L"Ganti kebijakan",
    .gguf_duplicate_warning = L"File duplikat: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Bersihkan duplikat",
    .gguf_manager_dedupe_done = L"File duplikat telah dipindahkan ke Recycle Bin.",
    .gguf_manager_dedupe_failed = L"File duplikat tidak dapat dibersihkan. Periksa apakah file sedang digunakan oleh program lain.",
    .menu_ui_theme = L"Tema antarmuka",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editorial hangat",
    .theme_monolith = L"Monolit halus",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Tampilkan terjemahan balik mengambang (verifikasi)",
    .back_translate_header = L"🔄 Verifikasi terjemahan balik",
    .back_translate_match_exact = L"✓ Makna sesuai",
    .back_translate_match_check = L"⚡ Periksa nuansa",
    .back_translate_reference = L"↳ Asli: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Bukan berkas model GGUF yang valid; tidak didaftarkan.",
    .gguf_manager_active_suffix = L"(sedang digunakan)",
    .gguf_manager_activated = L"Model ini sekarang sedang digunakan.",
};
// 27. Malay (ms)
const LocalizedStrings kStringsMalay = {
    .menu_status_active = L"Status: aktif (F9: jeda)",
    .menu_status_paused = L"Status: dijeda (F9: sambung)",
    .menu_engine = L"Enjin penterjemah",
    .menu_engine_google = L"Google Terjemah (percuma / tanpa pemasangan)",
    .menu_engine_local = L"Enjin setempat terbina dalam (Hy-MT2-1.8B luar talian)",
    .menu_source_lang = L"Bahasa sumber (input)",
    .menu_target_lang = L"Bahasa sasaran (output)",
    .menu_swap_langs = L"Tukar sumber ⇄ sasaran (klik dua kali)",
    .menu_auto_send = L"Hantar automatik dengan Enter",
    .menu_sound_feedback = L"Maklum balas bunyi (nada)",
    .menu_show_badge = L"Lambang terapung kelihatan",
    .menu_start_with_windows = L"Mula dengan Windows",
    .menu_cheatsheet = L"Contehan pintasan dan bantuan...",
    .menu_exit = L"Keluar Emebala Chat",
    .menu_about = L"Perihal Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — pintasan dan panduan",
    .cheatsheet_body = L"Pintasan dan panduan penggunaan Emebala Chat:\n\n"
    L"  • F9 : aktif / jeda\n"
    L"  • Ctrl + F9 : tukar bahasa sasaran\n"
    L"  • Ctrl + Shift + Enter : togol hantar automatik\n"
    L"  • Shift + Enter : terjemah dan hantar serta-merta\n\n"
    L"Kawalan tetikus pada lambang:\n"
    L"  • Klik kiri : aktif / jeda\n"
    L"  • Klik dua kali : tukar sumber ⇄ sasaran\n"
    L"  • Klik kanan : buka menu tetapan\n\n"
    L"Mod penterjemahan:\n"
    L"  • Ganti sahaja (hantar-automatik mati): menggantikan baris dengan terjemahan untuk semakan.\n"
    L"  • Hantar automatik (hidup): menggantikan baris dan menekan Enter dengan serta-merta.",
    .about_title = L"Perihal Emebala Chat",
    .badge_active = L"Aktif",
    .badge_translating = L"Menterjemah...",
    .badge_paused = L"Dijeda",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Tidak dapat menyalin teks terpilih. Periksa apl sasaran dan cuba lagi.",
    .tooltip_no_selection = L"Tiada teks dipilih untuk diterjemah.",
    .auto_detect = L"Kesan automatik",
    .app_already_running = L"Emebala Chat sudah berjalan di latar belakang.\nPapar dulang pemberitahuan sistem.",
    .app_com_failed = L"Permulaan COM gagal.\nLambang terapung dan teks-ke-suara tidak tersedia,\ntetapi terjemahan, pintasan, dulang dan bunyi masih berfungsi.",
    .about_tagline = L"Berhenti menyalin dan menampal. Taip secara semula jadi dalam bahasa ibunda anda — terjemahan menggantikan taipan anda secara masa nyata dalam mana-mana apl Windows.",
    .about_feature0 = L"⚡ Seret-untuk-terjemah — pilih teks dalam mana-mana apl, ikon terapung serta-merta menterjemah.",
    .about_feature1 = L"🔊 TTS neural — menyebut kesemua 37 bahasa melalui pakej suara Windows yang dipasang.",
    .about_feature2 = L"🔒 100% pada peranti & persendirian — aktif hanya semasa pintasan ditekan; papan keratan tidak diusik.",
    .about_etymology = L"Pada 2000 SM, jurutulis Mesopotamia memanggil «Eme-bala» — mereka yang menjadikan bahasa jambatan antara dunia.",
    .about_link_website = L"Laman web",
    .about_link_contact = L"Hubungi",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Waktu pejabat 10:00–19:00 KST",
    .about_contact_lead = L"Arkitek utama: Yongtai Kim",
    .tooltip_copied = L"✓ Disalin!",
    .tooltip_button_copy = L"📋 Salin",
    .tooltip_button_tts = L"🔊 Suara",
    .menu_ui_language = L"Bahasa antara muka",
    .menu_ui_language_auto = L"Auto (bahasa sistem)",
    .about_reset_button = L"Set semula ke lalai sistem",
    .about_reset_done = L"Lalai dipulihkan",
    .menu_typing_group = L"Taipan papan kekunci",
    .menu_tooltip_group = L"Tooltip seret",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Tiada suara Windows dipasang untuk bahasa ini. Klik 🔊 sekali lagi untuk membuka tetapan Ucapan.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Pemberitahuan privasi",
    .privacy_notice_body = L"Berikut adalah prinsip privasi Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat tidak mengendalikan pelayan sendiri.\n"
    L"• Dengan model setempat, teks diterjemahkan tidak meninggalkan peranti anda.\n"
    L"• Jika anda memilih Google Terjemah atau bertukar ke awan secara automatik, teks yang dipilih atau ditaip dihantar terus kepada Google untuk diterjemahkan, bukan melalui Emebala.\n"
    L"• Log diagnostik dimatikan secara lalai; aktifkan dalam tetapan (pilihan).\n"
    L"• Jika anda tidak mahu menghantar teks ke awan (Google), buka menu ikon pada tray sistem, pilih “Enjin penterjemah” kemudian “Enjin setempat terbina dalam”. Tanpa model setempat dipasang dan dengan fallback awan dimatikan, terjemahan tidak berjalan — tiada apa dihantar.\n"
    L"\n"
    L"Butiran penuh terdapat dalam fail README, yang boleh dibaca semula pada bila-bila masa.\n",
    .cheatsheet_config_path = L"Fail konfigurasi: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Teks terlalu panjang: hanya bahagian awal dan akhir diterjemahkan.",
    .tooltip_untranslated_above = L"Terdapat teks baru yang belum diterjemahkan di atas. Letakkan kursor di hujung baris itu dan tekan Enter untuk menterjemah.",
    .repair_in_progress = L"Membaiki komponen enjin tempatan…",
    .repair_failed_title = L"Terjemahan tempatan tidak tersedia",
    .repair_failed_body = L"Fail enjin terjemahan tempatan tidak dijumpai, jadi terjemahan dijeda. Pasang semula Emebala Chat untuk memulihkan enjin tempatan, atau untuk bertukar ke terjemahan awan (Google), pilih “Google Terjemah” dari menu dulang, “Enjin terjemahan”.",
    .menu_engine_openai = L"Serasi OpenAI (pelayan tersuai)…",
    .openai_settings_title = L"Tetapan enjin serasi OpenAI",
    .openai_settings_action = L"Tetapan enjin serasi OpenAI…",
    .openai_base_url_label = L"URL asas",
    .openai_api_key_label = L"Kunci API",
    .openai_model_label = L"Model",
    .openai_fetch_models = L"Ambil senarai model",
    .openai_fetch_failed = L"Senarai model tidak dapat diambil. Anda boleh menaip nama model secara langsung.",
    .openai_http_warning_title = L"Sambungan tidak selamat (HTTP)",
    .openai_http_warning_body = L"URL asas menggunakan HTTP (tidak disulitkan). Kunci API dan teks anda akan dihantar tanpa sulitan. Teruskan?",
    .openai_saved = L"Tetapan serasi OpenAI disimpan.",
    .openai_key_masked = L"Kunci disimpan: ",
    .openai_invalid_base_url = L"URL asas tidak sah. Contoh: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Model yang ditentukan pengguna (.gguf)",
    .menu_browse_gguf_file = L"Daftarkan model .gguf lain…",
    .user_gguf_quality_title = L"Nota kualiti terjemahan",
    .user_gguf_quality_body = L"Model yang dipilih bukan Hy-MT2. Versi semasa menggunakan prompt khas Hy-MT2, jadi kualiti terjemahan dengan model ini tidak dijamin. Teruskan?",
    .user_gguf_registered_title = L"Model didaftarkan",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Model yang dipilih telah didaftarkan dengan enjin tempatan.\n"
    L"\n"
    L"Model ini digunakan untuk terjemahan apabila anda memilih \"Pemilihan enjin terjemahan > Pilihan pengguna (.gguf)\".\n"
    L"\n"
    L"Bila berkuat kuasa: paling lambat kira-kira 1 minit selepas pendaftaran (selepas enjin berhenti kerana tidak aktif)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Model ini sudah terbina dalam Emebala Chat. Pendaftaran tidak diperlukan. Pilih terus enjin terjemahan tempatan terbina dalam.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(belum didaftarkan)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Urus model…",
    .gguf_manager_title = L"Pengurus Model Pengguna",
    .gguf_manager_empty = L"Tiada model pengguna berdaftar.",
    .gguf_manager_rename = L"Tukar nama…",
    .gguf_manager_delete = L"Padam…",
    .gguf_manager_close = L"Tutup",
    .gguf_manager_delete_confirm_title = L"Padam pendaftaran model",
    .gguf_manager_delete_confirm_body = L"Pendaftaran model yang dipilih akan dipadamkan.\n"
    L"\n"
    L"Fail model (.gguf) kekal pada cakera dan tidak dipadamkan. Jika model ini sedang digunakan, pemilihan enjin terjemahan akan kembali kepada Automatik (Auto).\n"
    L"\n"
    L"Teruskan?",
    .gguf_manager_rename_title = L"Tukar nama model",
    .gguf_manager_rename_body = L"Masukkan nama baharu (maks. 64 aksara, tanpa ruang).",
    .gguf_manager_rename_invalid = L"Nama itu tidak boleh digunakan. Masukkan nama yang tidak kosong, berbeza daripada yang sedia ada, tanpa ruang atau pemisah laluan, sehingga 64 aksara.",
    .gguf_manager_done = L"Perubahan disimpan.",
    .gguf_manager_err_serialize = L"registry.json tidak dapat disirikan (nama fail ditolak). Tiada apa yang berubah.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% tidak tersedia; folder model kongsi tidak dapat ditemui.",
    .gguf_manager_err_write = L"registry.json tidak dapat ditulis.",
    .gguf_manager_err_write_partial = L"registry.json tidak dapat ditulis dengan lengkap.",
    .gguf_manager_err_registry_damaged = L"registry.json rosak atau mempunyai skema tidak disokong. Ia TIDAK diubah suai. Baiki atau padamkannya, kemudian cuba lagi.",
    .tooltip_translate_failed = L"Terjemahan gagal. Sila cuba lagi.",
    .repair_transient_body = L"Enjin terjemahan tempatan tidak tersedia buat sementara (mungkin sedang dimulakan). Tunggu seketika dan cuba lagi.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Setuju",
    .dialog_cancel = L"Batal",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Tambah daripada fail…",
    .gguf_manager_add_hf = L"Tambah daripada Hugging Face…",
    .hf_add_title = L"Tambah model Hugging Face",
    .hf_url_label = L"URL model",
    .hf_downloading = L"Memuat turun…",
    .hf_failed = L"Muat turun gagal.",
    .hf_invalid_url = L"Masukkan URL model huggingface.co (resolve, blob atau halaman model)",
    .hf_done = L"Telah didaftarkan.",
    .openai_base_url_hint = L"cth.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Kunci API",
    // REQ-051: butang [Padamkan tetapan].
    .openai_delete_settings = L"Padamkan tetapan",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Semak kemas kini",
    .update_idle_note = L"Lihat sama ada anda menggunakan Emebala Chat versi terkini.",
    .update_checking = L"Menyemak kemas kini…",
    .update_checking_hint = L"Berjalan di latar belakang; anda boleh menutup tetingkap ini.",
    .update_uptodate = L"Anda menggunakan versi terkini",
    .update_dlg_title = L"Kemas kini tersedia",
    .update_dlg_version = L"Versi baharu",
    .update_dlg_version_cur = L"anda menggunakan {v}",
    .update_dlg_size = L"Saiz",
    .update_dlg_size_approx = L"lebih kurang {s}",
    .update_dlg_privacy = L"Semakan dan muat turun akan menyambung ke GitHub.",
    .update_dlg_download = L"Muat turun",
    .update_dlg_later = L"Kemudian",
    .update_dl_progress = L"Memuat turun…",
    .update_dl_cancel = L"Batal",
    .update_dl_cancelled = L"Muat turun dibatalkan.",
    .update_ready_title = L"Muat turun selesai — sedia untuk dipasang",
    .update_ready_warn = L"Aplikasi akan ditutup untuk memasang.",
    .update_ready_install = L"Pasang sekarang",
    .update_ready_later = L"Kemudian",
    .update_err_offline = L"Anda luar talian. Periksa sambungan anda, kemudian cuba lagi.",
    .update_err_rate = L"Terlalu banyak permintaan. Sila cuba sebentar lagi.",
    .update_err_hash = L"Fail yang dimuat turun tidak lulus pengesahan.",
    .update_err_retry = L"Cuba lagi",
    .update_snoozed_line = L"Anda boleh memasang kemas kini kemudian.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"Muat turun model",
    .model_dl_consent_body = L"Model terjemahan terbina dalam belum dipasang. Muat turun sekarang? (lebih kurang {s})",
    .model_dl_privacy = L"Muat turun akan menyambung ke huggingface.co.",
    .model_dl_download = L"Muat turun sekarang",
    .model_dl_verifying = L"Mengesahkan muat turun…",
    .model_dl_placing = L"Memasang model…",
    .model_dl_done = L"Model sedia. Enjin tempatan akan menggunakannya mulai terjemahan seterusnya.",
    .model_dl_failed = L"Muat turun gagal. Periksa sambungan anda dan cuba lagi.",
    .model_dl_disk_space = L"Ruang cakera bebas tidak mencukupi (memerlukan lebih kurang {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Tetapan pemuatan model terjemahan…",
    .engine_load_settings_title = L"Pemuatan model terjemahan",
    .load_mode_on_use = L"Muat semasa digunakan (disyorkan)",
    .load_mode_warmup = L"Muat awal",
    .load_mode_on_use_desc = L"Menimatkan memori. Terjemahan pertama mungkin bermula beberapa saat lewat.",
    .load_mode_warmup_desc = L"Terjemahan bermula dengan serta-merta. Model kekal dalam memori.",
    .gguf_policy_sticky = L"(Kekal dalam memori)",
    .gguf_policy_evict = L"(Muat semasa digunakan)",
    .gguf_manager_toggle_policy = L"Tukar dasar",
    .gguf_duplicate_warning = L"Fail pendua: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Bersihkan pendua",
    .gguf_manager_dedupe_done = L"Fail pendua telah dialihkan ke Tong Kitar Semula.",
    .gguf_manager_dedupe_failed = L"Fail pendua tidak dapat dibersihkan. Periksa sama ada fail sedang digunakan oleh program lain.",
    .menu_ui_theme = L"Tema antara muka",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Editorial hangat",
    .theme_monolith = L"Monolit halus",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Paparkan terjemahan balik terapung (pengesahan)",
    .back_translate_header = L"🔄 Pengesahan terjemahan balik",
    .back_translate_match_exact = L"✓ Makna sepadan",
    .back_translate_match_check = L"⚡ Semak nuansa",
    .back_translate_reference = L"↳ Asal: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Bukan fail model GGUF yang sah; ia tidak didaftarkan.",
    .gguf_manager_active_suffix = L"(sedang digunakan)",
    .gguf_manager_activated = L"Model ini kini sedang digunakan.",
};
// 28. Filipino (fil)
// Note: "Website", "Google Translate" and "Reddit" are used in Philippine
// English-dominant UI practice; the surrounding copy is genuine Filipino.
const LocalizedStrings kStringsFilipino = {
    .menu_status_active = L"Katayuan: Aktibo (F9: I-pause)",
    .menu_status_paused = L"Katayuan: Nakahinto (F9: Ipagpatuloy)",
    .menu_engine = L"Makina ng pagsasalin",
    .menu_engine_google = L"Google Translate (Libre / walang i-install)",
    .menu_engine_local = L"Built-in na lokal na makina (Hy-MT2-1.8B offline)",
    .menu_source_lang = L"Pinagmulang wika (input)",
    .menu_target_lang = L"Target na wika (output)",
    .menu_swap_langs = L"Palitan ang pinagmulan ⇄ target (i-double click)",
    .menu_auto_send = L"Awtomatikong ipadala sa Enter",
    .menu_sound_feedback = L"Tugon ng tunog (mga tono)",
    .menu_show_badge = L"Ipakita ang lumulutang na badge",
    .menu_start_with_windows = L"Simulan kasama ang Windows",
    .menu_cheatsheet = L"Cheat sheet ng mga shortcut at tulong...",
    .menu_exit = L"Lumabas sa Emebala Chat",
    .menu_about = L"Tungkol sa Emebala Chat…",
    .cheatsheet_title = L"Emebala Chat — mga shortcut at gabay",
    .cheatsheet_body = L"Mga shortcut at gabay sa Emebala Chat:\n\n"
    L"  • F9 : i-aktibo / i-pause\n"
    L"  • Ctrl + F9 : palitan ang target na wika\n"
    L"  • Ctrl + Shift + Enter : i-toggle ang awtomatikong pagpadala\n"
    L"  • Shift + Enter : isalin at agad ipadala\n\n"
    L"Mga kontrol ng mouse sa badge:\n"
    L"  • Kaliwang click : i-aktibo / i-pause\n"
    L"  • Double click : palitan ang pinagmulan ⇄ target\n"
    L"  • Kanang click : buksan ang menu ng settings\n\n"
    L"Mga mode ng pagsasalin:\n"
    L"  • Palitan lamang (kasara ang auto-send): pinapalitan ang linya ng salin para sa pagsusuri.\n"
    L"  • Awtomatikong pagpadala (bukas): pinapalitan ang linya at agad pinipindot ang Enter.",
    .about_title = L"Tungkol sa Emebala Chat",
    .badge_active = L"Aktibo",
    .badge_translating = L"Nagsasalin...",
    .badge_paused = L"Nakahinto",
    .tooltip_title = L"Emebala Chat",
    .tooltip_copy_failed = L"Hindi maikopya ang napiling teksto. Suriin ang target app at subukang muli.",
    .tooltip_no_selection = L"Walang napiling teksto na isasalin.",
    .auto_detect = L"Kusang pagtukoy",
    .app_already_running = L"Gumagana na ang Emebala Chat sa background.\nTingnan ang notification tray.",
    .app_com_failed = L"Nabigong i-initialize ang COM.\nHindi magagamit ang lumulutang badge at text-to-speech,\nngunit patuloy pa ring gumagana ang pagsasalin, shortcuts, tray at mga tunog.",
    .about_tagline = L"Tigilan na ang kopya-at-idikit. Mag-type nang natural sa iyong katutubong wika — pinapalitan ng salin ang iyong pagta-type sa real time sa kahit aning app ng Windows.",
    .about_feature0 = L"⚡ I-drag para isalin — pumili ng teksto sa kahit aning app, agad isinasalin ng lumulutang icon.",
    .about_feature1 = L"🔊 Neural TTS — binibigkas ang lahat ng 37 wika sa pamamagitan ng naka-install na Windows voice packs.",
    .about_feature2 = L"🔒 100% sa device at pribado — aktibo lamang habang hawak ang shortcut; hindi hinahawakan ang clipboard.",
    .about_etymology = L"Noong 2000 BCE, tinatawag ng mga maysulat ng Mesopotamia na «Eme-bala» ang mga gumagawa ng wika na tulay sa mga mundo.",
    .about_link_website = L"Website",
    .about_link_contact = L"Kontak",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · Oras ng opisina 10:00–19:00 KST",
    .about_contact_lead = L"Namunang arkitekto: Yongtai Kim",
    .tooltip_copied = L"✓ Nakopya!",
    .tooltip_button_copy = L"📋 Kopyahin",
    .tooltip_button_tts = L"🔊 Boses",
    .menu_ui_language = L"Wika ng interface",
    .menu_ui_language_auto = L"Awtomatiko (wika ng sistema)",
    .about_reset_button = L"I-reset sa default ng sistema",
    .about_reset_done = L"Naibalik ang mga default",
    .menu_typing_group = L"Pagta-type ng keyboard",
    .menu_tooltip_group = L"Tooltip ng pagdrag",
    .app_name = L"Emebala Chat",
    .tooltip_no_tts_voice = L"Walang naka-install na Windows voice para sa wikang ito. I-click muli ang 🔊 upang buksan ang mga setting ng Pagsasalita.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"Paalala sa privacy",
    .privacy_notice_body = L"Narito ang mga prinsipyo sa privacy ng Emebala Chat.\n"
    L"\n"
    L"• Walang sariling server na pinapatakbo ang Emebala Chat.\n"
    L"• Sa local na model, hindi lumalabas sa iyong device ang isinalin.\n"
    L"• Kung pipiliin ang Google Translate o awtomatikong lilipat sa cloud, ang napili o ni-type na teksto ay ipapadala nang direkta sa Google para isalin, hindi sa pamamagitan ng Emebala.\n"
    L"• Ang diagnostic log ay OFF sa default; i-on sa settings (opt-in).\n"
    L"• Kung ayaw mong ipadala ang teksto sa cloud (Google), buksan ang menu ng icon sa system tray, piliin ang “Makina ng pagsasalin” at pagkatapos “Built-in na lokal na makina”. Kung walang naka-install na lokal na modelo at nakapatay ang cloud fallback, hindi tumatakbo ang pagsasalin — walang ipinapadala.\n"
    L"\n"
    L"Makikita ang buong detalye sa README file na maaaring babasahin anumang oras.\n",
    .cheatsheet_config_path = L"Config file: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"Masyadong mahaba ang teksto: simula at katapusan lang ang isinalin.",
    .tooltip_untranslated_above = L"May bagong hindi pa isinaling teksto sa itaas. Ilagay ang cursor sa dulo ng linyang iyon at pindutin ang Enter upang isalin.",
    .repair_in_progress = L"Kinukumpuni ang mga bahagi ng lokal na makina…",
    .repair_failed_title = L"Hindi available ang lokal na pagsasalin",
    .repair_failed_body = L"Nawawala ang mga file ng lokal na makina ng pagsasalin, kaya pansamantalang tumigil ang pagsasalin. I-install muli ang Emebala Chat para maibalik ang lokal na makina, o para lumipat sa cloud (Google) na pagsasalin, piliin ang “Google Translate” mula sa menu ng tray, “Makina ng pagsasalin”.",
    .menu_engine_openai = L"OpenAI-compatible (custom server)…",
    .openai_settings_title = L"Mga setting ng OpenAI-compatible na engine",
    .openai_settings_action = L"Mga setting ng OpenAI-compatible na engine…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"API Key",
    .openai_model_label = L"Modelo",
    .openai_fetch_models = L"Kunin ang listahan ng modelo",
    .openai_fetch_failed = L"Hindi nakuha ang listahan ng modelo. Maaari kang mag-type ng pangalan ng modelo nang direkta.",
    .openai_http_warning_title = L"Di-ligtas na koneksyon (HTTP)",
    .openai_http_warning_body = L"Ginagamit ng Base URL ang HTTP (hindi naka-encrypt). Ang iyong API key at teksto ay ipapadala nang walang encryption. Magpatuloy?",
    .openai_saved = L"Nai-save ang mga OpenAI-compatible na setting.",
    .openai_key_masked = L"Nai-save na key: ",
    .openai_invalid_base_url = L"Hindi wasto ang Base URL. Halimbawa: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"Modelong tinukoy ng user (.gguf)",
    .menu_browse_gguf_file = L"Magrehistro ng ibang .gguf model…",
    .user_gguf_quality_title = L"Paalala sa kalidad ng pagsasalin",
    .user_gguf_quality_body = L"Ang napiling modelo ay hindi Hy-MT2. Ang kasalukuyang bersyon ay gumagamit ng prompt na para lamang sa Hy-MT2, kaya hindi ginagarantiyahan ang kalidad ng pagsasalin ng modelong ito. Magpatuloy?",
    .user_gguf_registered_title = L"Nakarehistro ang modelo",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"Ang napiling modelo ay nakarehistro sa lokal na engine.\n"
    L"\n"
    L"Ang modelong ito ay ginagamit sa pagsasalin kapag pinili mo ang \"Pagpili ng engine ng pagsasalin > Pagpili ng gumagamit (.gguf)\".\n"
    L"\n"
    L"Kailan magiging epektibo: hindi hihigit sa mga 1 minuto pagkatapos ng rehistro (pagkatapos umalis ng engine dahil sa kawalan ng aktibidad)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"Ang modelong ito ay nakabuilt-in na sa Emebala Chat. Hindi na kailangan ng pagpaparehistro. Maaari mong direktang piliin ang built-in na lokal na translation engine.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(hindi pa rehistrado)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"Pamahalaan ang mga modelo…",
    .gguf_manager_title = L"Tagapamahala ng Mga Modelo ng Gumagamit",
    .gguf_manager_empty = L"Walang nakarehistrong modelong pang-gumagamit.",
    .gguf_manager_rename = L"Palitan ang pangalan…",
    .gguf_manager_delete = L"Tanggalin…",
    .gguf_manager_close = L"Isara",
    .gguf_manager_delete_confirm_title = L"Tanggalin ang rehistro ng modelo",
    .gguf_manager_delete_confirm_body = L"Ang rehistro ng napiling modelo ay tatanggalin.\n"
    L"\n"
    L"Ang file ng modelo (.gguf) ay mananatili sa disk at hindi tatanggalin. Kung ang modelong ito ay ginagamit, ang pagpili ng makina ng pagsasalin ay babalik sa Awtomatiko (Auto).\n"
    L"\n"
    L"Magpatuloy?",
    .gguf_manager_rename_title = L"Palitan ang pangalan ng modelo",
    .gguf_manager_rename_body = L"Maglagay ng bagong pangalan (hanggang 64 na karakter, walang espasyo).",
    .gguf_manager_rename_invalid = L"Hindi magamit ang pangalang iyan. Maglagay ng di-walang-laman na pangalan, naiiba sa mga umiiral, walang espasyo o pantahip ng landas, hanggang 64 na karakter.",
    .gguf_manager_done = L"Nai-save ang mga pagbabago.",
    .gguf_manager_err_serialize = L"Hindi mai-serialize ang registry.json (tinanggihan ang isang pangalan ng file). Walang nagbago.",
    .gguf_manager_err_no_localappdata = L"Hindi available ang %LOCALAPPDATA%; hindi mahanap ang ibinahaging folder ng mga modelo.",
    .gguf_manager_err_write = L"Hindi maisulat ang registry.json.",
    .gguf_manager_err_write_partial = L"Hindi lubos na nasulat ang registry.json.",
    .gguf_manager_err_registry_damaged = L"Sira ang registry.json o may hindi suportadong schema. HINDI ito binago. Ayusin o tanggalin ito, at subukan muli.",
    .tooltip_translate_failed = L"Nabigo ang pagsasalin. Subukan muli.",
    .repair_transient_body = L"Ang lokal na engine ng pagsasalin ay pansamantalang hindi available (maaaring nagsisimula). Maghintay sandali at subukan muli.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"Oo",
    .dialog_cancel = L"Kanselahin",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"Magdagdag mula sa file…",
    .gguf_manager_add_hf = L"Magdagdag mula sa Hugging Face…",
    .hf_add_title = L"Magdagdag ng Hugging Face model",
    .hf_url_label = L"URL ng model",
    .hf_downloading = L"Nagda-download…",
    .hf_failed = L"Nabigo ang pag-download.",
    .hf_invalid_url = L"Ilagay ang URL ng model mula sa huggingface.co (resolve, blob o pahina ng model)",
    .hf_done = L"Naka-register na.",
    .openai_base_url_hint = L"hal.: https://api.openai.com/v1",
    .openai_api_key_hint = L"Susi ng API",
    // REQ-051: button na [Tanggalin ang mga setting].
    .openai_delete_settings = L"Tanggalin ang mga setting",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"Suriin ang mga update",
    .update_idle_note = L"Tingnan kung pinakabagong bersyon ng Emebala Chat ang gamit mo.",
    .update_checking = L"Sinusuri ang mga update…",
    .update_checking_hint = L"Tumatakbo sa background; maaari mong isara ang window na ito.",
    .update_uptodate = L"Gamit mo ang pinakabagong bersyon",
    .update_dlg_title = L"May available na update",
    .update_dlg_version = L"Bagong bersyon",
    .update_dlg_version_cur = L"ang gamit mo ay {v}",
    .update_dlg_size = L"Laki",
    .update_dlg_size_approx = L"humigit-kumulang {s}",
    .update_dlg_privacy = L"Sinusuri at ini-download nito sa GitHub.",
    .update_dlg_download = L"I-download",
    .update_dlg_later = L"Mamaya na",
    .update_dl_progress = L"Nagda-download…",
    .update_dl_cancel = L"Kanselahin",
    .update_dl_cancelled = L"Kinansela ang download.",
    .update_ready_title = L"Tapos na ang download — handa nang i-install",
    .update_ready_warn = L"Isasara ang app para i-install.",
    .update_ready_install = L"I-install ngayon",
    .update_ready_later = L"Mamaya na",
    .update_err_offline = L"Wala kang koneksyon. Suriin ang koneksyon mo, at subukan muli.",
    .update_err_rate = L"Masyadong maraming kahilingan. Subukan muli mamaya.",
    .update_err_hash = L"Hindi pumasa sa beripikasyon ang na-download na file.",
    .update_err_retry = L"Subukan muli",
    .update_snoozed_line = L"Maaari mong i-install ang update mamaya.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"I-download ang model",
    .model_dl_consent_body = L"Hindi pa naka-install ang built-in na model ng pagsasalin. I-download ngayon? (humigit-kumulang {s})",
    .model_dl_privacy = L"Kumokonekta sa huggingface.co ang pag-download.",
    .model_dl_download = L"I-download ngayon",
    .model_dl_verifying = L"Bineberipika ang download…",
    .model_dl_placing = L"Ini-install ang model…",
    .model_dl_done = L"Handa na ang model. Gagamitin ito ng lokal na engine sa susunod na pagsasalin.",
    .model_dl_failed = L"Nabigo ang download. Suriin ang koneksyon mo at subukan muli.",
    .model_dl_disk_space = L"Kulang ang libreng espasyo ng disk (humigit-kumulang {s} ang kailangan).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"Mga setting ng pag-load ng translation model…",
    .engine_load_settings_title = L"Pag-load ng translation model",
    .load_mode_on_use = L"I-load kapag gagamitin (inirerekomenda)",
    .load_mode_warmup = L"I-load nang maaga",
    .load_mode_on_use_desc = L"Tipid sa memory. Maaaring maantal ang unang pagsasalin nang ilang segundo.",
    .load_mode_warmup_desc = L"Magsisimula agad ang pagsasalin. Mananatili sa memory ang model.",
    .gguf_policy_sticky = L"(Manatiling naka-load)",
    .gguf_policy_evict = L"(I-load kapag gagamitin)",
    .gguf_manager_toggle_policy = L"Palitan ang patakaran",
    .gguf_duplicate_warning = L"Mga duplicate na file: {n} {s} × {c}",
    .gguf_manager_dedupe = L"Linisin ang mga duplicate",
    .gguf_manager_dedupe_done = L"Ang mga duplicate na file ay inilipat sa Recycle Bin.",
    .gguf_manager_dedupe_failed = L"Hindi nalinis ang mga duplicate na file. Tingnan kung ginagamit ng ibang program.",
    .menu_ui_theme = L"Tema ng interface",
    .theme_fluent = L"Fluent Craft",
    .theme_editorial = L"Mainit na Editorial",
    .theme_monolith = L"Pinong Monolito",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"Ipakita ang lumulutang na pabalik na salin (pagpapatunay)",
    .back_translate_header = L"🔄 Pagpapatunay ng pabalik na salin",
    .back_translate_match_exact = L"✓ Tumutugma ang kahulugan",
    .back_translate_match_check = L"⚡ Suriin ang nuance",
    .back_translate_reference = L"↳ Orihinal: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"Hindi ito wastong GGUF model file; hindi ito narehistro.",
    .gguf_manager_active_suffix = L"(ginagamit)",
    .gguf_manager_activated = L"Ang modelong ito ay ginagamit na ngayon.",
};
// 29. Hindi (hi)
const LocalizedStrings kStringsHindi = {
    .menu_status_active = L"स्थिति: सक्रिय (F9: रोकें)",
    .menu_status_paused = L"स्थिति: रुका हुआ (F9: जारी रखें)",
    .menu_engine = L"अनुवाद इंजन",
    .menu_engine_google = L"Google अनुवाद (मुफ़्त / इंस्टॉलेशन नहीं)",
    .menu_engine_local = L"अंतर्निहित लोकल इंजन (Hy-MT2-1.8B ऑफ़लाइन)",
    .menu_source_lang = L"स्रोत भाषा (इनपुट)",
    .menu_target_lang = L"लक्ष्य भाषा (आउटपुट)",
    .menu_swap_langs = L"स्रोत ⇄ लक्ष्य अदला-बदली (डबल-क्लिक)",
    .menu_auto_send = L"Enter पर स्वतः भेजें",
    .menu_sound_feedback = L"ध्वनि प्रतिक्रिया (टोन)",
    .menu_show_badge = L"फ़्लोटिंग बैज दिखाएँ",
    .menu_start_with_windows = L"Windows के साथ प्रारंभ करें",
    .menu_cheatsheet = L"शॉर्टकट चीट-शीट और सहायता...",
    .menu_exit = L"एमेबाला चैट से बाहर निकलें",
    .menu_about = L"एमेबाला चैट के बारे में…",
    .cheatsheet_title = L"एमेबाला चैट — शॉर्टकट और उपयोग गाइड",
    .cheatsheet_body = L"एमेबाला चैट शॉर्टकट और उपयोग गाइड:\n\n"
    L"  • F9 : चालू / रोकें\n"
    L"  • Ctrl + F9 : लक्ष्य भाषा बदलें\n"
    L"  • Ctrl + Shift + Enter : स्वतः-भेजें मोड टॉगल करें\n"
    L"  • Shift + Enter : तुरंत अनुवाद कर भेजें\n\n"
    L"बैज पर माउस नियंत्रण:\n"
    L"  • बायाँ क्लिक : चालू / रोकें\n"
    L"  • डबल क्लिक : स्रोत ⇄ लक्ष्य अदला-बदली\n"
    L"  • दायाँ क्लिक : सेटिंग्स मेनू खोलें\n\n"
    L"अनुवाद मोड:\n"
    L"  • केवल प्रतिस्थापन (स्वतः-भेजें बंद): जाँच हेतु पंक्ति को अनुवाद से बदलता है।\n"
    L"  • स्वतः-भेजें (चालू): पंक्ति बदलकर तुरंत Enter दबाता है।",
    .about_title = L"एमेबाला चैट के बारे में",
    .badge_active = L"सक्रिय",
    .badge_translating = L"अनुवाद हो रहा...",
    .badge_paused = L"रुका हुआ",
    .tooltip_title = L"एमेबाला चैट",
    .tooltip_copy_failed = L"चयनित पाठ कॉपी नहीं हो सका। लक्ष्य ऐप जाँचें और फिर कोशिश करें।",
    .tooltip_no_selection = L"अनुवाद हेतु कोई पाठ चयनित नहीं है।",
    .auto_detect = L"स्वतः पहचान",
    .app_already_running = L"एमेबाला चैट पहले से पृष्ठभूमि में चल रहा है।\nसूचना ट्रे देखें।",
    .app_com_failed = L"COM आरंभ विफल।\nफ़्लोटिंग बैज और वाचन अनुपलब्ध रहेंगे,\nपर अनुवाद, शॉर्टकट, ट्रे और ध्वनियाँ काम करती रहेंगी।",
    .about_tagline = L"कापी-पेस्ट की ज़रूरत नहीं। अपनी मातृभाषा में सहज टाइप करें — अनुवाद किसी भी Windows ऐप में आपकी टाइपिंग को रीयल-टाइम में बदल देता है।",
    .about_feature0 = L"⚡ खींचें-और-अनुवाद — किसी भी ऐप में पाठ चुनें, फ़्लोटिंग आइकन तुरंत अनुवाद करता है।",
    .about_feature1 = L"🔊 न्यूरल TTS — इंस्टॉल Windows वॉइस पैक से सभी 37 भाषाओं का उच्चारण।",
    .about_feature2 = L"🔒 100% डिवाइस पर और निजी — जब तक शॉर्टकट दबा है तभी सक्रिय; क्लिपबोर्ड को छुए बिना।",
    .about_etymology = L"2000 ईसा पूर्व, मेसोपोटामिया के लेखकों ने भाषा को दुनियाओं के बीच पुल बनाने वालों को «Eme-bala» कहा करता था।",
    .about_link_website = L"वेबसाइट",
    .about_link_contact = L"संपर्क",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · सियोल येोंगदोंगपो (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · कार्यालय समय 10:00–19:00 KST",
    .about_contact_lead = L"मुख्य आर्किटेक्ट: Yongtai Kim",
    .tooltip_copied = L"✓ कॉपी हो गया!",
    .tooltip_button_copy = L"📋 कॉपी",
    .tooltip_button_tts = L"🔊 वाचन",
    .menu_ui_language = L"इंटरफ़ेस भाषा",
    .menu_ui_language_auto = L"स्वतः (सिस्टम भाषा)",
    .about_reset_button = L"सिस्टम पूर्वनिर्धारित पर रीसेट करें",
    .about_reset_done = L"पूर्वनिर्धारित बहाल",
    .menu_typing_group = L"कीबोर्ड टाइपिंग",
    .menu_tooltip_group = L"ड्रैग टूलटिप",
    .app_name = L"एमेबाला चैट",
    .tooltip_no_tts_voice = L"इस भाषा के लिए कोई Windows वॉइस इंस्टॉल नहीं है. वॉइस सेटिंग खोलने के लिए 🔊 पर फिर से क्लिक करें.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"गोपनीयता सूचना",
    .privacy_notice_body = L"Emebala Chat की गोपनीयता नीति इस प्रकार है:\n"
    L"\n"
    L"• Emebala Chat का कोई अपना सर्वर नहीं है।\n"
    L"• स्थानीय मॉडल पर अनुवादित पाठ आपके डिवाइस से बाहर नहीं जाता।\n"
    L"• यदि आप Google Translate चुनते हैं या स्वतः क्लाउड पर स्विच होता है, तो चयनित/लिखा पाठ अनुवाद हेतु सीधे Google भेजा जाता है — Emebala से नहीं।\n"
    L"• डायग्नोस्टिक लॉग डिफ़ॉल्ट बंद (OFF) हैं; सेटिंग में चालू करें (opt-in)।\n"
    L"• यदि आप टेक्स्ट क्लाउड (Google) में नहीं भेजना चाहते, तो सिस्टम ट्रे आइकन मेनू खोलें, “अनुवाद इंजन” चुनें और “अंतर्निहित लोकल इंजन” चुनें। यदि स्थानीय मॉडल इंस्टॉल नहीं है और क्लाउड फ़ॉलबैक बंद है, तो अनुवाद नहीं चलेगा — कुछ भी नहीं भेजा जाएगा।\n"
    L"\n"
    L"पूरा विवरण README फ़ाइल में है, जिसे आप कभी भी दोबारा पढ़ सकते हैं।\n",
    .cheatsheet_config_path = L"कॉन्फ़िग फ़ाइल: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"पाठ बहुत लंबा था: केवल शुरुआत और अंत का अनुवाद किया गया।",
    .tooltip_untranslated_above = L"ऊपर नया अनअनुवादित टेक्स्ट है। उसे अनुवाद करने के लिए कर्सर को उस पंक्ति के अंत में रखें और Enter दबाएं।",
    .repair_in_progress = L"लोकल इंजन घटकों की मरम्मत हो रही है…",
    .repair_failed_title = L"लोकल अनुवाद उपलब्ध नहीं है",
    .repair_failed_body = L"लोकल अनुवाद इंजन की फ़ाइलें नहीं मिलीं, इसलिए अनुवाद रुका हुआ है। लोकल इंजन को पुनर्स्थापित करने के लिए Emebala Chat को पुनः इंस्टॉल करें, या क्लाउड (Google) अनुवाद पर जाने के लिए, ट्रे मेनू से “अनुवाद इंजन” में “Google अनुवाद” चुनें।",
    .menu_engine_openai = L"OpenAI संगत (उपयोगकर्ता-परिभाषित सर्वर)…",
    .openai_settings_title = L"OpenAI-संगत इंजन सेटिंग्स",
    .openai_settings_action = L"OpenAI-संगत इंजन सेटिंग्स…",
    .openai_base_url_label = L"बेस URL",
    .openai_api_key_label = L"API कुंजी",
    .openai_model_label = L"मॉडल",
    .openai_fetch_models = L"मॉडल सूची प्राप्त करें",
    .openai_fetch_failed = L"मॉडल सूची प्राप्त नहीं हो सकी। आप मॉडल का नाम सीधे टाइप कर सकते हैं।",
    .openai_http_warning_title = L"असुरक्षित कनेक्शन (HTTP)",
    .openai_http_warning_body = L"बेस URL HTTP (अनएन्क्रिप्टेड) उपयोग करता है। आपकी API कुंजी और टेक्स्ट सादे टेक्स्ट में भेजे जाएंगे। जारी रखें?",
    .openai_saved = L"OpenAI-संगत सेटिंग्स सहेजी गईं।",
    .openai_key_masked = L"सहेजी गई कुंजी: ",
    .openai_invalid_base_url = L"बेस URL मान्य नहीं है। उदाहरण: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"उपयोगकर्ता-परिभाषित मॉडल (.gguf)",
    .menu_browse_gguf_file = L"कोई अन्य .gguf मॉडल पंजीकृत करें…",
    .user_gguf_quality_title = L"अनुवाद गुणवत्ता सूचना",
    .user_gguf_quality_body = L"चयनित मॉडल Hy-MT2 नहीं है। वर्तमान संस्करण केवल Hy-MT2 के लिए प्रॉम्प्ट का उपयोग करता है, इसलिए इस मॉडल से अनुवाद की गुणवत्ता की गारंटी नहीं है। जारी रखें?",
    .user_gguf_registered_title = L"मॉडल पंजीकृत",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"चयनित मॉडल स्थानीय इंजन में पंजीकृत हो गया है।\n"
    L"\n"
    L"इस मॉडल का उपयोग तब अनुवाद के लिए होता है जब आप \"अनुवाद इंजन चयन > उपयोगकर्ता चयन (.gguf)\" चुनते हैं।\n"
    L"\n"
    L"कब लागू: पंजीकरण के बाद अधिकतम लगभग 1 मिनट (इंजन के निष्क्रिय समापन के बाद)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"यह मॉडल Emebala Chat में पहले से ही अंतर्निहित है। पंजीकरण की आवश्यकता नहीं है। अंतर्निहित लोकल अनुवाद इंजन को सीधे चुनें।",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(पंजीकृत नहीं)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"मॉडल प्रबंधित करें…",
    .gguf_manager_title = L"उपयोगकर्ता मॉडल प्रबंधक",
    .gguf_manager_empty = L"कोई उपयोगकर्ता मॉडल पंजीकृत नहीं है।",
    .gguf_manager_rename = L"नाम बदलें…",
    .gguf_manager_delete = L"हटाएं…",
    .gguf_manager_close = L"बंद करें",
    .gguf_manager_delete_confirm_title = L"मॉडल पंजीकरण हटाएं",
    .gguf_manager_delete_confirm_body = L"चयनित मॉडल का पंजीकरण हटाया जाएगा।\n"
    L"\n"
    L"मॉडल फ़ाइल (.gguf) डिस्क पर बनी रहती है और नहीं हटती। यदि यह मॉडल प्रयोग में था, तो अनुवाद इंजन चयन स्वतः (Auto) पर लौट आएगा।\n"
    L"\n"
    L"जारी रखें?",
    .gguf_manager_rename_title = L"मॉडल का नाम बदलें",
    .gguf_manager_rename_body = L"नया नाम दर्ज करें (अधिकतम 64 वर्ण, बिना रिक्त स्थान)।",
    .gguf_manager_rename_invalid = L"वह नाम प्रयोग नहीं किया जा सकता। कोई खाली नहीं, मौजूदा से अलग, बिना रिक्त स्थान या पथ विभाजक, 64 वर्णों के भीतर नाम दर्ज करें।",
    .gguf_manager_done = L"परिवर्तन सहेजे गए।",
    .gguf_manager_err_serialize = L"registry.json को सीरियलाइज़ नहीं किया जा सका (फ़ाइल नाम अस्वीकृत)। कुछ भी नहीं बदला गया।",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% अनुपलब्ध है; साझा मॉडल फ़ोल्डर नहीं मिल सका।",
    .gguf_manager_err_write = L"registry.json नहीं लिखा जा सका।",
    .gguf_manager_err_write_partial = L"registry.json पूरी तरह नहीं लिखा जा सका।",
    .gguf_manager_err_registry_damaged = L"registry.json क्षतिग्रस्त है या उसका स्कीमा unsupported है। इसमें कोई बदलाव नहीं किया गया। इसे ठीक करें या हटाएं, फिर पुनः प्रयास करें।",
    .tooltip_translate_failed = L"अनुवाद विफल रहा। कृपया पुनः प्रयास करें।",
    .repair_transient_body = L"स्थानीय अनुवाद इंजन अस्थायी रूप से उपलब्ध नहीं है (शायद प्रारंभ हो रहा हो)। कृपया कुछ देर प्रतीक्षा करें और पुनः प्रयास करें।",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"ठीक है",
    .dialog_cancel = L"रद्द करें",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"फ़ाइल से जोड़ें…",
    .gguf_manager_add_hf = L"Hugging Face से जोड़ें…",
    .hf_add_title = L"Hugging Face मॉडल जोड़ें",
    .hf_url_label = L"मॉडल URL",
    .hf_downloading = L"डाउनलोड हो रहा है…",
    .hf_failed = L"डाउनलोड विफल रहा।",
    .hf_invalid_url = L"huggingface.co का मॉडल URL दर्ज करें (resolve, blob या मॉडल पेज)",
    .hf_done = L"पंजीकृत हो गया।",
    .openai_base_url_hint = L"उदा.: https://api.openai.com/v1",
    .openai_api_key_hint = L"API कुंजी",
    // REQ-051: [सेटिंग हटाएं] बटन।
    .openai_delete_settings = L"सेटिंग हटाएं",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"अपडेट जांचें",
    .update_idle_note = L"देखें कि क्या आप नवीनतम Emebala Chat चला रहे हैं।",
    .update_checking = L"अपडेट जांचा जा रहा है…",
    .update_checking_hint = L"यह पृष्ठभूमि में चलता है; आप इस विंडो को बंद कर सकते हैं।",
    .update_uptodate = L"आप नवीनतम संस्करण का उपयोग कर रहे हैं",
    .update_dlg_title = L"अपडेट उपलब्ध है",
    .update_dlg_version = L"नया संस्करण",
    .update_dlg_version_cur = L"आपके पास {v} है",
    .update_dlg_size = L"आकार",
    .update_dlg_size_approx = L"लगभग {s}",
    .update_dlg_privacy = L"जांच और डाउनलोड करने पर GitHub से कनेक्ट होता है।",
    .update_dlg_download = L"डाउनलोड करें",
    .update_dlg_later = L"बाद में",
    .update_dl_progress = L"डाउनलोड हो रहा है…",
    .update_dl_cancel = L"रद्द करें",
    .update_dl_cancelled = L"डाउनलोड रद्द कर दिया गया।",
    .update_ready_title = L"डाउनलोड पूर्ण — इंस्टॉल के लिए तैयार",
    .update_ready_warn = L"इंस्टॉल के लिए ऐप बंद हो जाएगा।",
    .update_ready_install = L"अभी इंस्टॉल करें",
    .update_ready_later = L"बाद में",
    .update_err_offline = L"आप ऑफ़लाइन हैं। अपना कनेक्शन जांचें और फिर पुनः प्रयास करें।",
    .update_err_rate = L"बहुत अधिक अनुरोध। कृपया थोड़ी देर बाद पुनः प्रयास करें।",
    .update_err_hash = L"डाउनलोड की गई फ़ाइल सत्यापित नहीं हुई।",
    .update_err_retry = L"पुनः प्रयास करें",
    .update_snoozed_line = L"आप इस अपडेट को बाद में इंस्टॉल कर सकते हैं।",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"मॉडल डाउनलोड करें",
    .model_dl_consent_body = L"बिल्ट-इन अनुवाद मॉडल इंस्टॉल नहीं है। अभी डाउनलोड करें? (लगभग {s})",
    .model_dl_privacy = L"डाउनलोड करने पर huggingface.co से कनेक्ट होता है।",
    .model_dl_download = L"अभी डाउनलोड करें",
    .model_dl_verifying = L"डाउनलोड की जांच हो रही है…",
    .model_dl_placing = L"मॉडल इंस्टॉल हो रहा है…",
    .model_dl_done = L"मॉडल तैयार है। लोकल इंजन इसे अगले अनुवाद से उपयोग करेगा।",
    .model_dl_failed = L"डाउनलोड विफल हुआ। अपना कनेक्शन जांचें और पुनः प्रयास करें।",
    .model_dl_disk_space = L"डिस्क पर पर्याप्त खाली स्थान नहीं है (लगभग {s} आवश्यक)।",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"अनुवाद मॉडल लोड सेटिंग…",
    .engine_load_settings_title = L"अनुवाद मॉडल लोडिंग",
    .load_mode_on_use = L"उपयोग पर लोड करें (अनुशंसित)",
    .load_mode_warmup = L"पहले से लोड करें",
    .load_mode_on_use_desc = L"मेमोरी बचाता है। पहला अनुवाद कुछ सेकंड देर से शुरू हो सकता है।",
    .load_mode_warmup_desc = L"अनुवाद तुरंत शुरू होता है। मॉडल मेमोरी में बना रहता है।",
    .gguf_policy_sticky = L"(मेमोरी में बनाए रखें)",
    .gguf_policy_evict = L"(उपयोग पर लोड करें)",
    .gguf_manager_toggle_policy = L"नीति बदलें",
    .gguf_duplicate_warning = L"डुप्लिकेट फ़ाइलें: {n} {s} × {c}",
    .gguf_manager_dedupe = L"डुप्लिकेट साफ़ करें",
    .gguf_manager_dedupe_done = L"डुप्लिकेट फ़ाइलों को रीसायकल बिन में ले जाया गया।",
    .gguf_manager_dedupe_failed = L"डुप्लिकेट फ़ाइलें साफ़ नहीं हो सकीं। जांचें कि कहीं कोई अन्य प्रोग्राम उनका उपयोग तो नहीं कर रहा।",
    .menu_ui_theme = L"इंटरफ़ेस थीम",
    .theme_fluent = L"फ़्लूएंट क्राफ्ट",
    .theme_editorial = L"वॉर्म एडिटोरियल",
    .theme_monolith = L"रिफ़ाइंड मोनोलिथ",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"फ़्लोटिंग पश्च-अनुवाद दिखाएँ (सत्यापन)",
    .back_translate_header = L"🔄 पश्च-अनुवाद जाँच",
    .back_translate_match_exact = L"✓ अर्थ मेल खाता है",
    .back_translate_match_check = L"⚡ भाव जाँचें",
    .back_translate_reference = L"↳ मूल: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"यह मान्य GGUF मॉडल फ़ाइल नहीं है; इसे पंजीकृत नहीं किया गया।",
    .gguf_manager_active_suffix = L"(उपयोग में)",
    .gguf_manager_activated = L"अब यह मॉडल उपयोग में है।",
};
// 30. Bengali (bn)
const LocalizedStrings kStringsBengali = {
    .menu_status_active = L"স্ট্যাটাস: সক্রিয় (F9: বিরতি)",
    .menu_status_paused = L"স্ট্যাটাস: বিরতিপ্রাপ্ত (F9: পুনরায় চালু)",
    .menu_engine = L"অনুবাদ ইঞ্জিন",
    .menu_engine_google = L"Google Translate (বিনামূল্যে / ইনস্টল ছাড়াই)",
    .menu_engine_local = L"অন্তর্নির্মিত লোকাল ইঞ্জিন (Hy-MT2-1.8B অফলাইন)",
    .menu_source_lang = L"উৎস ভাষা (ইনপুট)",
    .menu_target_lang = L"লক্ষ্য ভাষা (আউটপুট)",
    .menu_swap_langs = L"উৎস ⇄ লক্ষ্য অদলবদল (ডাবল-ক্লিক)",
    .menu_auto_send = L"Enter-এ স্বয়ংক্রিয় প্রেরণ",
    .menu_sound_feedback = L"শব্দ প্রতিক্রিয়া (টোন)",
    .menu_show_badge = L"ফ্লোটিং ব্যাজ দৃশ্যমান",
    .menu_start_with_windows = L"Windows-এর সাথে চালু",
    .menu_cheatsheet = L"শর্টকাট চিট-শিট ও সহায়তা...",
    .menu_exit = L"এমেবালা চ্যাট প্রস্থান",
    .menu_about = L"এমেবালা চ্যাট সম্পর্কে…",
    .cheatsheet_title = L"এমেবালা চ্যাট — শর্টকাট ও ব্যবহার গাইড",
    .cheatsheet_body = L"এমেবালা চ্যাট শর্টকাট ও ব্যবহার গাইড:\n\n"
    L"  • F9 : চালু / বিরতি\n"
    L"  • Ctrl + F9 : লক্ষ্য ভাষা পরিবর্তন\n"
    L"  • Ctrl + Shift + Enter : স্বয়ংক্রিয় প্রেরণ টগল\n"
    L"  • Shift + Enter : অনুবাদ করে সঙ্গে সঙ্গে পাঠান\n\n"
    L"ব্যাজে মাউস নিয়ন্ত্রণ:\n"
    L"  • বাম ক্লিক : চালু / বিরতি\n"
    L"  • ডাবল ক্লিক : উৎস ⇄ লক্ষ্য অদলবদল\n"
    L"  • ডান ক্লিক : সেটিংস মেনু খুলুন\n\n"
    L"অনুবাদ মোড:\n"
    L"  • শুধু প্রতিস্থাপন (স্বয়ং-প্রেরণ বন্ধ): পর্যালোচনার জন্য লাইনটি অনুবাদ দিয়ে বদলায়।\n"
    L"  • স্বয়ং-প্রেরণ (চালু): লাইন বদলে সঙ্গে সঙ্গে Enter চাপে।",
    .about_title = L"এমেবালা চ্যাট সম্পর্কে",
    .badge_active = L"সক্রিয়",
    .badge_translating = L"অনুবাদ হচ্ছে...",
    .badge_paused = L"বিরতি",
    .tooltip_title = L"এমেবালা চ্যাট",
    .tooltip_copy_failed = L"নির্বাচিত লেখা কপি করা যায়নি। লক্ষ্য অ্যাপ দেখে আবার চেষ্টা করুন।",
    .tooltip_no_selection = L"অনুবাদের জন্য কোনো লেখা নির্বাচন করা হয়নি।",
    .auto_detect = L"স্বয়ংক্রিয় শনাক্ত",
    .app_already_running = L"এমেবালা চ্যাট ইতিমধ্যে পটভূমিতে চলছে।\nসিস্টেম নোটিফিকেশন ট্রে দেখুন।",
    .app_com_failed = L"COM আরম্ভ বিফল।\nফ্লোটিং ব্যাজ ও টেক্সট-টু-স্পিচ পাওয়া যাবে না,\nতবে অনুবাদ, শর্টকাট, ট্রে ও শব্দ কাজ করবে।",
    .about_tagline = L"আর কপি-পেস্ট নয়। মাতৃভাষায় স্বাভাবিকভাবে লিখুন — যেকোনো Windows অ্যাপে আপনার টাইপিং রিয়েল-টাইমে অনুবাদে বদলে যায়।",
    .about_feature0 = L"⚡ ড্র্যাগ-করুন-অনুবাদ — যেকোনো অ্যাপে লেখা নির্বাচন করুন, ফ্লোটিং আইকন সঙ্গে সঙ্গে অনুবাদ করে।",
    .about_feature1 = L"🔊 নিউরাল TTS — ইনস্টল করা Windows ভয়েস প্যাক দিয়ে সব 37টি ভাষার উচ্চারণ।",
    .about_feature2 = L"🔒 ১০০% ডিভাইসে ও গোপন — শর্টকাট চেপে রাখা অবধিই সক্রিয়; ক্লিপবোর্ড অক্ষত।",
    .about_etymology = L"২০০০ খ্রিস্টপূর্বাব্দে মেসোপটেমীয় লেখকরা ভাষাকে জগতের সেতু বানানো লোকদের «Eme-bala» বলত।",
    .about_link_website = L"ওয়েবসাইট",
    .about_link_contact = L"যোগাযোগ",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · সিউল ইয়ংডেংপো (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · অফিস সময় 10:00–19:00 KST",
    .about_contact_lead = L"প্রধান আর্কিটেক্ট: Yongtai Kim",
    .tooltip_copied = L"✓ কপি হয়েছে!",
    .tooltip_button_copy = L"📋 কপি",
    .tooltip_button_tts = L"🔊 ভাষণ",
    .menu_ui_language = L"ইন্টারফেস ভাষা",
    .menu_ui_language_auto = L"অটো (সিস্টেম ভাষা)",
    .about_reset_button = L"সিস্টেম ডিফল্টে রিসেট",
    .about_reset_done = L"ডিফল্ট পুনরুদ্ধার",
    .menu_typing_group = L"কীবোর্ড টাইপিং",
    .menu_tooltip_group = L"ড্র্যাগ টুলটিপ",
    .app_name = L"এমেবালা চ্যাট",
    .tooltip_no_tts_voice = L"এই ভাষার জন্য কোনো Windows ভয়েস ইনস্টল করা নেই. ভয়েস সেটিংস খুলতে 🔊-এ আবার ক্লিক করুন.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"গোপনীয়তা বিজ্ঞপ্তি",
    .privacy_notice_body = L"Emebala Chat-এর গোপনীয়তা নীতি নিচে দেওয়া হলো:\n"
    L"\n"
    L"• Emebala Chat-এর নিজস্ব কোনো সার্ভার নেই।\n"
    L"• লোকাল মডেল ব্যবহার করলে অনুবাদিত টেক্সট আপনার ডিভাইস ছেড়ে যায় না।\n"
    L"• আপনি Google Translate নির্বাচন করলে বা স্বয়ংক্রিয়ভাবে ক্লাউডে গেলে, নির্বাচিত/লিখিত টেক্সট অনুবাদের জন্য সরাসরি Google-এ যায় — Emebala-র মাধ্যমে নয়।\n"
    L"• ডায়াগনস্টিক লগ ডিফল্ট বন্ধ (OFF); সেটিংসে চালু করতে হয় (opt-in)।\n"
    L"• আপনি যদি টেক্সট ক্লাউডে (Google) পাঠাতে না চান, সিস্টেম ট্রে আইকনের মেনু খুলুন, “অনুবাদ ইঞ্জিন” থেকে “অন্তর্নির্মিত লোকাল ইঞ্জিন” নির্বাচন করুন। লোকাল মডেল ইনস্টল না থাকলে এবং ক্লাউড ফলব্যাক বন্ধ থাকলে অনুবাদ চলবে না — কিছুই পাঠানো হবে না।\n"
    L"\n"
    L"পূর্ণ বিবরণ README ফাইলে আছে, যেটি যেকোনো সময় আবার পড়া যাবে।\n",
    .cheatsheet_config_path = L"কনফিগ ফাইল: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"পাঠ্যটি খুব দীর্ঘ ছিল: শুধু শুরু এবং শেষ অংশ অনুবাদ করা হয়েছে।",
    .tooltip_untranslated_above = L"উপরে নতুন অনুবাদহীন টেক্সট আছে। এটি অনুবাদ করতে কার্সারটি সেই লাইনের শেষে রাখুন এবং Enter চাপুন।",
    .repair_in_progress = L"লোকাল ইঞ্জিন উপাদান মেরামত হচ্ছে…",
    .repair_failed_title = L"লোকাল অনুবাদ পাওয়া যাচ্ছে না",
    .repair_failed_body = L"লোকাল অনুবাদ ইঞ্জিনের ফাইল পাওয়া যায়নি, তাই অনুবাদ স্থগিত হয়েছে। লোকাল ইঞ্জিন পুনরুদ্ধার করতে Emebala Chat পুনরায় ইনস্টল করুন, অথবা ক্লাউড (Google) অনুবাদে যেতে, ট্রে মেনু থেকে “অনুবাদ ইঞ্জিন”-এ “Google অনুবাদ” নির্বাচন করুন।",
    .menu_engine_openai = L"OpenAI সামঞ্জস্যপূর্ণ (ব্যবহারকারী-সংজ্ঞায়িত সার্ভার)…",
    .openai_settings_title = L"OpenAI-সংযুক্ত ইঞ্জিন সেটিংস",
    .openai_settings_action = L"OpenAI-সংযুক্ত ইঞ্জিন সেটিংস…",
    .openai_base_url_label = L"বেস URL",
    .openai_api_key_label = L"API কী",
    .openai_model_label = L"মডেল",
    .openai_fetch_models = L"মডেল তালিকা আনুন",
    .openai_fetch_failed = L"মডেল তালিকা আনা যায়নি। আপনি সরাসরি মডেলের নাম লিখতে পারেন।",
    .openai_http_warning_title = L"অনিরাপদ সংযোগ (HTTP)",
    .openai_http_warning_body = L"বেস URL HTTP (এনক্রিপ্ট করা নয়) ব্যবহার করে। আপনার API কী এবং টেক্সট সাদা টেক্সট হিসেবে পাঠানো হবে। চালিয়ে যাবেন?",
    .openai_saved = L"OpenAI-সংযুক্ত সেটিংস সংরক্ষণ করা হয়েছে।",
    .openai_key_masked = L"সংরক্ষিত কী: ",
    .openai_invalid_base_url = L"বেস URL অবৈধ। উদাহরণ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"ব্যবহারকারী-সংজ্ঞায়িত মডেল (.gguf)",
    .menu_browse_gguf_file = L"অন্য .gguf মডেল নিবন্ধন করুন…",
    .user_gguf_quality_title = L"অনুবাদের মান সম্পর্কে নোটিশ",
    .user_gguf_quality_body = L"নির্বাচিত মডেল Hy-MT2 নয়। বর্তমান সংস্করণ শুধু Hy-MT2-এর জন্য প্রম্পট ব্যবহার করে, তাই এই মডেলের অনুবাদের মান নিশ্চিত করা যায় না। চালিয়ে যাবেন?",
    .user_gguf_registered_title = L"মডেল নিবন্ধিত",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"নির্বাচিত মডেলটি লোকাল ইঞ্জিনে নিবন্ধিত হয়েছে।\n"
    L"\n"
    L"আপনি \"অনুবাদ ইঞ্জিন নির্বাচন > ব্যবহারকারীর পছন্দ (.gguf)\" বেছে নিলে এই মডেলটি অনুবাদে ব্যবহৃত হয়।\n"
    L"\n"
    L"কখন কার্যকর: নিবন্ধনের পর সর্বোচ্চ প্রায় ১ মিনিট (ইঞ্জিন নিষ্ক্রিয় সমাপ্তির পর)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"এই মডেলটি ইতিমধ্যে Emebala Chat-এ অন্তর্নির্মিত। নিবন্ধনের প্রয়োজন নেই। অন্তর্নির্মিত লোকাল অনুবাদ ইঞ্জিন সরাসরি নির্বাচন করুন।",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(নিবন্ধিত নয়)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"মডেল পরিচালনা…",
    .gguf_manager_title = L"ব্যবহারকারী মডেল পরিচালক",
    .gguf_manager_empty = L"কোনো ব্যবহারকারী মডেল নথিভুক্ত নেই।",
    .gguf_manager_rename = L"নাম পরিবর্তন…",
    .gguf_manager_delete = L"মুছুন…",
    .gguf_manager_close = L"বন্ধ করুন",
    .gguf_manager_delete_confirm_title = L"মডেল নিবন্ধন মুছুন",
    .gguf_manager_delete_confirm_body = L"নির্বাচিত মডেলের নিবন্ধন মুছে ফেলা হবে।\n"
    L"\n"
    L"মডেল ফাইল (.gguf) ডিস্কে থাকবে এবং মুছে ফেলা হবে না। এই মডেলটি ব্যবহারে থাকলে, অনুবাদ ইঞ্জিন নির্বাচন স্বয়ংক্রিয় (Auto)-তে ফিরে যাবে।\n"
    L"\n"
    L"চালিয়ে যাবেন?",
    .gguf_manager_rename_title = L"মডেলের নাম পরিবর্তন",
    .gguf_manager_rename_body = L"নতুন নাম লিখুন (সর্বোচ্চ ৬৪ অক্ষর, ফাঁক ছাড়া)।",
    .gguf_manager_rename_invalid = L"সেই নাম ব্যবহার করা যাবে না। খালি নয়, বিদ্যমানগুলো থেকে আলাদা, ফাঁক বা পথ বিভাজক ছাড়া, সর্বোচ্চ ৬৪ অক্ষরের নাম লিখুন।",
    .gguf_manager_done = L"পরিবর্তন সংরক্ষণ করা হয়েছে।",
    .gguf_manager_err_serialize = L"registry.json সিরিয়ালাইজ করা যায়নি (একটি ফাইলনাম প্রত্যাখ্যাত)। কিছুই পরিবর্তন করা হয়নি।",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% অনুপলব্ধ; ভাগ করা মডেল ফোল্ডার খুঁজে পাওয়া যায়নি।",
    .gguf_manager_err_write = L"registry.json লেখা যায়নি।",
    .gguf_manager_err_write_partial = L"registry.json সম্পূর্ণভাবে লেখা যায়নি।",
    .gguf_manager_err_registry_damaged = L"registry.json ক্ষতিগ্রস্ত বা এর স্কিমা সমর্থিত নয়। এটি পরিবর্তন করা হয়নি। এটি মেরামত করুন বা মুছুন, তারপর আবার চেষ্টা করুন।",
    .tooltip_translate_failed = L"অনুবাদ ব্যর্থ হয়েছে। আবার চেষ্টা করুন।",
    .repair_transient_body = L"লোকাল অনুবাদ ইঞ্জিনটি সাময়িকভাবে অনুপলব্ধ (হয়তো শুরু হচ্ছে)। একটু অপেক্ষা করে আবার চেষ্টা করুন।",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"ঠিক আছে",
    .dialog_cancel = L"বাতিল",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"ফাইল থেকে যোগ করুন…",
    .gguf_manager_add_hf = L"Hugging Face থেকে যোগ করুন…",
    .hf_add_title = L"Hugging Face মডেল যোগ করুন",
    .hf_url_label = L"মডেলের URL",
    .hf_downloading = L"ডাউনলোড হচ্ছে…",
    .hf_failed = L"ডাউনলোড ব্যর্থ হয়েছে।",
    .hf_invalid_url = L"huggingface.co মডেলের URL লিখুন (resolve, blob বা মডেল পেজ)",
    .hf_done = L"নিবন্ধিত হয়েছে।",
    .openai_base_url_hint = L"উদা.: https://api.openai.com/v1",
    .openai_api_key_hint = L"API কী",
    // REQ-051: [সেটিংস মুছুন] বোতাম।
    .openai_delete_settings = L"সেটিংস মুছুন",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"আপডেট যাচাই করুন",
    .update_idle_note = L"দেখুন আপনি সর্বশেষ Emebala Chat চালাচ্ছেন কিনা।",
    .update_checking = L"আপডেট যাচাই করা হচ্ছে…",
    .update_checking_hint = L"এটি পটভূমিতে চলে; আপনি এই উইন্ডোটি বন্ধ করতে পারেন।",
    .update_uptodate = L"আপনি সর্বশেষ সংস্করণ ব্যবহার করছেন",
    .update_dlg_title = L"আপডেট পাওয়া যাচ্ছে",
    .update_dlg_version = L"নতুন সংস্করণ",
    .update_dlg_version_cur = L"আপনার সংস্করণ {v}",
    .update_dlg_size = L"আকার",
    .update_dlg_size_approx = L"প্রায় {s}",
    .update_dlg_privacy = L"যাচাই এবং ডাউনলোড করতে GitHub-এ সংযোগ হয়।",
    .update_dlg_download = L"ডাউনলোড করুন",
    .update_dlg_later = L"পরে",
    .update_dl_progress = L"ডাউনলোড হচ্ছে…",
    .update_dl_cancel = L"বাতিল করুন",
    .update_dl_cancelled = L"ডাউনলোড বাতিল করা হয়েছে।",
    .update_ready_title = L"ডাউনলোড সম্পূর্ণ — ইনস্টলের জন্য প্রস্তুত",
    .update_ready_warn = L"ইনস্টল করতে অ্যাপটি বন্ধ হবে।",
    .update_ready_install = L"এখনই ইনস্টল করুন",
    .update_ready_later = L"পরে",
    .update_err_offline = L"আপনি অফলাইনে আছেন। আপনার সংযোগ পরীক্ষা করে আবার চেষ্টা করুন।",
    .update_err_rate = L"অনেক বেশি অনুরোধ। একটু পরে আবার চেষ্টা করুন।",
    .update_err_hash = L"ডাউনলোড করা ফাইলটি যাচাই পাস করেনি।",
    .update_err_retry = L"আবার চেষ্টা করুন",
    .update_snoozed_line = L"আপনি পরে আপডেটটি ইনস্টল করতে পারেন।",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"মডেল ডাউনলোড করুন",
    .model_dl_consent_body = L"বিল্ট-ইন অনুবাদ মডেল ইনস্টল করা নেই। এখনই ডাউনলোড করবেন? (প্রায় {s})",
    .model_dl_privacy = L"ডাউনলোড করতে huggingface.co-এ সংযোগ হয়।",
    .model_dl_download = L"এখনই ডাউনলোড করুন",
    .model_dl_verifying = L"ডাউনলোড করা ফাইল যাচাই করা হচ্ছে…",
    .model_dl_placing = L"মডেল ইনস্টল করা হচ্ছে…",
    .model_dl_done = L"মডেল প্রস্তুত। লোকাল ইঞ্জিন পরবর্তী অনুবাদ থেকে এটি ব্যবহার করবে।",
    .model_dl_failed = L"ডাউনলোড ব্যর্থ হয়েছে। আপনার সংযোগ পরীক্ষা করে আবার চেষ্টা করুন।",
    .model_dl_disk_space = L"ডিস্কে পর্যাপ্ত খালি জায়গা নেই (প্রায় {s} প্রয়োজন)।",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"অনুবাদ মডেল লোড সেটিংস…",
    .engine_load_settings_title = L"অনুবাদ মডেল লোডিং",
    .load_mode_on_use = L"ব্যবহারের সময় লোড করুন (প্রস্তাবিত)",
    .load_mode_warmup = L"আগে থেকে লোড করুন",
    .load_mode_on_use_desc = L"মেমোরি বাঁচায়। প্রথম অনুবাদ কয়েক সেকেন্ড দেরি হতে পারে।",
    .load_mode_warmup_desc = L"অনুবাদ তাৎক্ষণিক শুরু হয়। মডেলটি মেমোরিতে থাকে।",
    .gguf_policy_sticky = L"(মেমোরিতে রাখুন)",
    .gguf_policy_evict = L"(ব্যবহারের সময় লোড করুন)",
    .gguf_manager_toggle_policy = L"নীতি পরিবর্তন করুন",
    .gguf_duplicate_warning = L"ডুপ্লিকেট ফাইল: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ডুপ্লিকেট পরিষ্কার করুন",
    .gguf_manager_dedupe_done = L"ডুপ্লিকেট ফাইলগুলি রিসাইকেল বিনে সরানো হয়েছে।",
    .gguf_manager_dedupe_failed = L"ডুপ্লিকেট ফাইল পরিষ্কার করা যায়নি। অন্য কোনো প্রোগ্রাম ফাইলগুলি ব্যবহার করছে কিনা দেখুন।",
    .menu_ui_theme = L"ইন্টারফেস থিম",
    .theme_fluent = L"ফ্লুয়েন্ট ক্রাফ্ট",
    .theme_editorial = L"ওয়ার্ম এডিটোরিয়াল",
    .theme_monolith = L"রিফাইন্ড মনোলিথ",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"ভাসমান ব্যাক-অনুবাদ দেখান (যাচাই)",
    .back_translate_header = L"🔄 ব্যাক-অনুবাদ যাচাই",
    .back_translate_match_exact = L"✓ অর্থ মিলে গেছে",
    .back_translate_match_check = L"⚡ সূক্ষ্মতা যাচাই করুন",
    .back_translate_reference = L"↳ মূল: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"এটি বৈধ GGUF মডেল ফাইল নয়; তাই নিবন্ধন করা হয়নি।",
    .gguf_manager_active_suffix = L"(ব্যবহৃত হচ্ছে)",
    .gguf_manager_activated = L"এই মডেলটি এখন ব্যবহৃত হচ্ছে।",
};
// 31. Arabic (ar) — RTL language; string CONTENT is logical-order UTF-16, the
// UI chrome direction is decided by the render layer (bidi_utils).
const LocalizedStrings kStringsArabic = {
    .menu_status_active = L"الحالة: نشِط (F9: إيقاف مؤقت)",
    .menu_status_paused = L"الحالة: متوقف مؤقتًا (F9: استئناف)",
    .menu_engine = L"محرك الترجمة",
    .menu_engine_google = L"ترجمة Google (مجاني / بدون تثبيت)",
    .menu_engine_local = L"محرك الترجمة المحلي المدمج (Hy-MT2-1.8B دون اتصال)",
    .menu_source_lang = L"لغة المصدر (الإدخال)",
    .menu_target_lang = L"لغة الهدف (الإخراج)",
    .menu_swap_langs = L"تبديل المصدر ⇄ الهدف (نقر مزدوج)",
    .menu_auto_send = L"الإرسال التلقائي عند الضغط على Enter",
    .menu_sound_feedback = L"ردود الفعل الصوتية (نغمات)",
    .menu_show_badge = L"إظهار الشارة العائمة",
    .menu_start_with_windows = L"التشغيل عند بدء Windows",
    .menu_cheatsheet = L"مرجع اختصارات لوحة المفاتيح والمساعدة...",
    .menu_exit = L"إنهاء إيميبالا شات",
    .menu_about = L"حول إيميبالا شات…",
    .cheatsheet_title = L"إيميبالا شات — اختصارات ودليل الاستخدام",
    .cheatsheet_body = L"اختصارات إيميبالا شات ودليل الاستخدام:\n\n"
    L"  • F9 : تبديل التشغيل / الإيقاف المؤقت\n"
    L"  • Ctrl + F9 : تبديل لغة الهدف\n"
    L"  • Ctrl + Shift + Enter : تبديل الإرسال التلقائي\n"
    L"  • Shift + Enter : ترجمة وإرسال فوري\n\n"
    L"أزرار الفأرة على الشارة العائمة:\n"
    L"  • النقر الأيسر : التشغيل / الإيقاف المؤقت\n"
    L"  • النقر المزدوج : تبديل المصدر ⇄ الهدف\n"
    L"  • النقر الأيمن : فتح قائمة الإعدادات\n\n"
    L"أوضاع الترجمة:\n"
    L"  • الاستبدال فقط (الإرسال التلقائي متوقف): يستبدل السطر بالترجمة للمراجعة.\n"
    L"  • الإرسال التلقائي (مُفعّل): يستبدل السطر ويضغط Enter فورًا.",
    .about_title = L"حول إيميبالا شات",
    .badge_active = L"نشِط",
    .badge_translating = L"جارٍ الترجمة...",
    .badge_paused = L"متوقف مؤقتًا",
    .tooltip_title = L"إيميبالا شات",
    .tooltip_copy_failed = L"تعذّر نسخ النص المحدد. تحقق من التطبيق الهدف وأعد المحاولة.",
    .tooltip_no_selection = L"لم يتم تحديد أي نص لترجمته.",
    .auto_detect = L"كشف تلقائي",
    .app_already_running = L"إيميبالا شات يعمل بالفعل في الخلفية.\nتحقق من منطقة إشعارات النظام.",
    .app_com_failed = L"فشل تهيئة COM.\nلن تتوافر الشارة العائمة وقراءة النص بصوت مسموع،\nلكن الترجمة والاختصارات والإشعارات والأصوات ستواصل العمل.",
    .about_tagline = L"ودِّع النسخ واللصق. اكتب بلغتك الأم بطبيعتك — تحل الترجمة محل كتابتك في الوقت الفعلي داخل أي تطبيق من تطبيقات Windows.",
    .about_feature0 = L"⚡ ترجم بالسحب — حدّد النص في أي تطبيق، فتُترجمه الأيقونة العائمة فورًا.",
    .about_feature1 = L"🔊 تحويل نص إلى كلام عصبي — ينطق بجميع اللغات الـ37 عبر حزم الأصوات المثبتة في Windows.",
    .about_feature2 = L"🔒 يعمل محليًا وبخصوصية 100% — يتفعّل أثناء ضغط الاختصار فحسب، دون المساس بالحافظة.",
    .about_etymology = L"في عام 2000 قبل الميلاد، أطلق كتّاب بلاد الرافدين اسم «Eme-bala» على من يجعلون من اللغة جسرًا بين العوالم.",
    .about_link_website = L"الموقع الإلكتروني",
    .about_link_contact = L"اتصل بنا",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"فريق سان بلازا · سيول يونغدونغبو (الغرفة 219، 65 يونغجونغ-رو)",
    .about_contact_phone = L"+82 2 575 0414 · ساعات العمل 10:00–19:00 KST",
    .about_contact_lead = L"كبير المهندسين: Yongtai Kim",
    .tooltip_copied = L"✓ تم النسخ!",
    .tooltip_button_copy = L"📋 نسخ",
    .tooltip_button_tts = L"🔊 استماع",
    .menu_ui_language = L"لغة الواجهة",
    .menu_ui_language_auto = L"تلقائي (لغة النظام)",
    .about_reset_button = L"إعادة التعيين إلى إعدادات النظام الافتراضية",
    .about_reset_done = L"تمت استعادة الإعدادات الافتراضية",
    .menu_typing_group = L"الكتابة بلوحة المفاتيح",
    .menu_tooltip_group = L"تلميح السحب",
    .app_name = L"إيميبالا شات",
    .tooltip_no_tts_voice = L"لا يوجد صوت Windows مثبت لهذه اللغة. انقر فوق 🔊 مرة أخرى لفتح إعدادات الكلام.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"إشعار الخصوصية",
    .privacy_notice_body = L"هذه مبادئ الخصوصية في Emebala Chat.\n"
    L"\n"
    L"• لا يشغّل Emebala Chat أي خوادم خاصة به.\n"
    L"• عند استخدام النموذج المحلي، لا يغادر النص المترجم جهازك.\n"
    L"• إذا اخترت ترجمة Google أو تم التبديل التلقائي إلى السحابة، يُرسَل النص المحدَّد أو المكتوب مباشرة إلى Google للترجمة، وليس عبر Emebala.\n"
    L"• سجلات التشخيص معطّلة افتراضيًا؛ تفعّلها من الإعدادات (بموافقتك).\n"
    L"• إذا لم ترغب في إرسال النص إلى السحابة (Google)، افتح قائمة أيقونة شريط المهام واختر “محرك الترجمة” ثم “محرك الترجمة المحلي المدمج”. إذا لم يكن النموذج المحلي مثبتًا وكان التحويل السحابي معطلًا، فلن تعمل الترجمة ولن يُرسل أي شيء.\n"
    L"\n"
    L"التفاصيل الكاملة في ملف README ويمكن قراءته في أي وقت.\n",
    .cheatsheet_config_path = L"ملف الإعداد: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"النص طويل جدًا: تمت ترجمة البداية والنهاية فقط.",
    .tooltip_untranslated_above = L"يوجد نص جديد غير مترجم في الأعلى. ضع المؤشر في نهاية ذلك السطر واضغط Enter للترجمة.",
    .repair_in_progress = L"جارٍ إصلاح مكونات المحرك المحلي…",
    .repair_failed_title = L"الترجمة المحلية غير متاحة",
    .repair_failed_body = L"ملفات محرك الترجمة المحلي مفقودة، لذا تم إيقاف الترجمة مؤقتًا. أعد تثبيت Emebala Chat لاستعادة المحرك المحلي، أو للتبديل إلى ترجمة السحابة (Google)، اختر “Google ترجمة” من قائمة الشريط ضمن “محرك الترجمة”.",
    .menu_engine_openai = L"متوافق مع OpenAI (خادم مخصص)…",
    .openai_settings_title = L"إعدادات المحرك المتوافق مع OpenAI",
    .openai_settings_action = L"إعدادات المحرك المتوافق مع OpenAI…",
    .openai_base_url_label = L"عنوان URL الأساسي",
    .openai_api_key_label = L"مفتاح API",
    .openai_model_label = L"النموذج",
    .openai_fetch_models = L"جلب قائمة النماذج",
    .openai_fetch_failed = L"تعذر جلب قائمة النماذج. يمكنك كتابة اسم النموذج مباشرة.",
    .openai_http_warning_title = L"اتصال غير آمن (HTTP)",
    .openai_http_warning_body = L"يستخدم عنوان URL الأساسي HTTP (غير مشفر). سيتم إرسال مفتاح API والنص بشكل غير مشفر. هل تريد المتابعة؟",
    .openai_saved = L"تم حفظ الإعدادات المتوافقة مع OpenAI.",
    .openai_key_masked = L"المفتاح المحفوظ: ",
    .openai_invalid_base_url = L"عنوان URL الأساسي غير صالح. مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"النموذج المحدد من قبل المستخدم (.gguf)",
    .menu_browse_gguf_file = L"تسجيل نموذج .gguf آخر…",
    .user_gguf_quality_title = L"إشعار بشأن جودة الترجمة",
    .user_gguf_quality_body = L"النموذج المحدد ليس Hy-MT2. تستخدم النسخة الحالية موجه (prompt) خاصًا بـ Hy-MT2 فقط، لذا لا تُضمن جودة الترجمة بهذا النموذج. هل تريد المتابعة؟",
    .user_gguf_registered_title = L"تم تسجيل النموذج",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"تم تسجيل النموذج المحدد في المحرك المحلي.\n"
    L"\n"
    L"يُستخدم هذا النموذج للترجمة عند اختيار \"اختيار محرك الترجمة > اختيار المستخدم (.gguf)\".\n"
    L"\n"
    L"متى يسري: خلال دقيقة واحدة تقريبًا كحد أقصى بعد التسجيل (بعد خروج المحرك بسبب الخمول)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"هذا النموذج مدمج بالفعل في Emebala Chat. لا حاجة إلى التسجيل. يمكنك اختيار محرك الترجمة المحلي المدمج مباشرةً.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(غير مسجل)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"إدارة النماذج…",
    .gguf_manager_title = L"مدير نماذج المستخدم",
    .gguf_manager_empty = L"لا توجد نماذج مستخدم مسجّلة.",
    .gguf_manager_rename = L"إعادة تسمية…",
    .gguf_manager_delete = L"حذف…",
    .gguf_manager_close = L"إغلاق",
    .gguf_manager_delete_confirm_title = L"حذف تسجيل النموذج",
    .gguf_manager_delete_confirm_body = L"سيتم حذف تسجيل النموذج المحدد.\n"
    L"\n"
    L"ملف النموذج (.gguf) يبقى على القرص ولا يُحذف. إذا كان هذا النموذج قيد الاستخدام، فسيعود اختيار محرك الترجمة إلى تلقائي (Auto).\n"
    L"\n"
    L"هل تريد المتابعة؟",
    .gguf_manager_rename_title = L"إعادة تسمية النموذج",
    .gguf_manager_rename_body = L"أدخل اسمًا جديدًا (بحد أقصى 64 حرفًا، بدون مسافات).",
    .gguf_manager_rename_invalid = L"لا يمكن استخدام هذا الاسم. أدخل اسمًا غير فارغ ومختلف عن الأسماء الموجودة، بدون مسافات أو فواصل مسار، وضمن 64 حرفًا.",
    .gguf_manager_done = L"تم حفظ التغييرات.",
    .gguf_manager_err_serialize = L"تعذر تسلسل registry.json (تم رفض اسم ملف). لم يتغير شيء.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% غير متاح؛ لا يمكن تحديد موقع مجلد النماذج المشترك.",
    .gguf_manager_err_write = L"تعذر كتابة registry.json.",
    .gguf_manager_err_write_partial = L"تعذر كتابة registry.json بالكامل.",
    .gguf_manager_err_registry_damaged = L"registry.json تالف أو يحتوي على مخطط غير مدعوم. لم يتم تعديله. أصلحه أو احذفه ثم أعد المحاولة.",
    .tooltip_translate_failed = L"فشلت الترجمة. يرجى المحاولة مرة أخرى.",
    .repair_transient_body = L"محرك الترجمة المحلي غير متاح مؤقتًا (قد يكون قيد التشغيل). يرجى الانتظار قليلاً والمحاولة مرة أخرى.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"موافق",
    .dialog_cancel = L"إلغاء",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"إضافة من ملف…",
    .gguf_manager_add_hf = L"إضافة من Hugging Face…",
    .hf_add_title = L"إضافة نموذج من Hugging Face",
    .hf_url_label = L"عنوان URL للنموذج",
    .hf_downloading = L"جارٍ التنزيل…",
    .hf_failed = L"فشل التنزيل.",
    .hf_invalid_url = L"أدخل عنوان URL للنموذج من huggingface.co (resolve أو blob أو صفحة النموذج)",
    .hf_done = L"تم التسجيل.",
    .openai_base_url_hint = L"مثال: https://api.openai.com/v1",
    .openai_api_key_hint = L"مفتاح API",
    // REQ-051: زر [حذف الإعدادات].
    .openai_delete_settings = L"حذف الإعدادات",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"التحقق من التحديثات",
    .update_idle_note = L"تحقق مما إذا كنت تستخدم أحدث إصدار من Emebala Chat.",
    .update_checking = L"جارٍ التحقق من التحديثات…",
    .update_checking_hint = L"يعمل في الخلفية؛ يمكنك إغلاق هذه النافذة.",
    .update_uptodate = L"أنت تستخدم أحدث إصدار",
    .update_dlg_title = L"يتوفر تحديث",
    .update_dlg_version = L"الإصدار الجديد",
    .update_dlg_version_cur = L"إصدارك الحالي {v}",
    .update_dlg_size = L"الحجم",
    .update_dlg_size_approx = L"نحو {s}",
    .update_dlg_privacy = L"تتطلب التحقق والتنزيل الاتصال بـ GitHub.",
    .update_dlg_download = L"تنزيل",
    .update_dlg_later = L"لاحقًا",
    .update_dl_progress = L"جارٍ التنزيل…",
    .update_dl_cancel = L"إلغاء",
    .update_dl_cancelled = L"تم إلغاء التنزيل.",
    .update_ready_title = L"اكتمل التنزيل — جاهز للتثبيت",
    .update_ready_warn = L"سيُغلق التطبيق لبدء التثبيت.",
    .update_ready_install = L"تثبيت الآن",
    .update_ready_later = L"لاحقًا",
    .update_err_offline = L"أنت غير متصل. تحقق من اتصالك ثم حاول مرة أخرى.",
    .update_err_rate = L"طلبات كثيرة جدًا. يرجى المحاولة بعد قليل.",
    .update_err_hash = L"لم يجتز الملف الذي تم تنزيله عملية التحقق.",
    .update_err_retry = L"إعادة المحاولة",
    .update_snoozed_line = L"يمكنك تثبيت التحديث لاحقًا.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"تنزيل النموذج",
    .model_dl_consent_body = L"نموذج الترجمة المدمج غير مثبت. هل تريد تنزيله الآن؟ (نحو {s})",
    .model_dl_privacy = L"يتصل التنزيل بـ huggingface.co.",
    .model_dl_download = L"نزّل الآن",
    .model_dl_verifying = L"جارٍ التحقق من الملف الذي تم تنزيله…",
    .model_dl_placing = L"جارٍ تثبيت النموذج…",
    .model_dl_done = L"النموذج جاهز. سيعمل المحرك المحلي به بدءًا من الترجمة التالية.",
    .model_dl_failed = L"فشل التنزيل. تحقق من الاتصال وحاول مرة أخرى.",
    .model_dl_disk_space = L"مساحة القرص الحرة غير كافية (مطلوب نحو {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"إعدادات تحميل نموذج الترجمة…",
    .engine_load_settings_title = L"تحميل نموذج الترجمة",
    .load_mode_on_use = L"التحميل عند الاستخدام (موصى به)",
    .load_mode_warmup = L"التحميل المسبق",
    .load_mode_on_use_desc = L"يوفّر الذاكرة. قد تتأخر الترجمة الأولى بضع ثوانٍ.",
    .load_mode_warmup_desc = L"تبدأ الترجمة فورًا. يبقى النموذج محمّلًا في الذاكرة.",
    .gguf_policy_sticky = L"(يبقى محمّلًا)",
    .gguf_policy_evict = L"(التحميل عند الاستخدام)",
    .gguf_manager_toggle_policy = L"تبديل السياسة",
    .gguf_duplicate_warning = L"ملفات مكررة: {n} {s} × {c}",
    .gguf_manager_dedupe = L"تنظيف الملفات المكررة",
    .gguf_manager_dedupe_done = L"تم نقل الملفات المكررة إلى سلة المحذوفات.",
    .gguf_manager_dedupe_failed = L"تعذّر تنظيف الملفات المكررة. تحقق مما إذا كانت تُستخدم في برنامج آخر.",
    .menu_ui_theme = L"سمة الواجهة",
    .theme_fluent = L"فلووينت كرافت",
    .theme_editorial = L"وورم إيديتوريال",
    .theme_monolith = L"ريفايند مونوليث",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"إظهار الترجمة العكسية العائمة (تحقق)",
    .back_translate_header = L"🔄 تحقق من الترجمة العكسية",
    .back_translate_match_exact = L"✓ المعنى مطابق",
    .back_translate_match_check = L"⚡ تحقق من الفارق الدقيق",
    .back_translate_reference = L"↳ الأصل: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"الملف ليس نموذج GGUF صالحًا؛ لم يتم تسجيله.",
    .gguf_manager_active_suffix = L"(قيد الاستخدام)",
    .gguf_manager_activated = L"هذا النموذج قيد الاستخدام الآن.",
};
// 32. Persian (fa) — RTL
const LocalizedStrings kStringsPersian = {
    .menu_status_active = L"وضعیت: فعال (F9: توقف موقت)",
    .menu_status_paused = L"وضعیت: متوقف (F9: ادامه)",
    .menu_engine = L"موتور ترجمه",
    .menu_engine_google = L"Google Translate (رایگان / بدون نصب)",
    .menu_engine_local = L"موتور ترجمه محلی داخلی (Hy-MT2-1.8B آفلاین)",
    .menu_source_lang = L"زبان مبدأ (ورودی)",
    .menu_target_lang = L"زبان مقصد (خروجی)",
    .menu_swap_langs = L"تعویض مبدأ ⇄ مقصد (دوبار کلیک)",
    .menu_auto_send = L"ارسال خودکار با Enter",
    .menu_sound_feedback = L"بازخورد صوتی (تُن‌ها)",
    .menu_show_badge = L"نمایش نشان شناور",
    .menu_start_with_windows = L"شروع با ویندوز",
    .menu_cheatsheet = L"برگه تقلبی کلیدهای میان‌بر و راهنما...",
    .menu_exit = L"خروج از امبالا چت",
    .menu_about = L"دربارهٔ امبالا چت…",
    .cheatsheet_title = L"کلیدهای میان‌بر و راهنمای امبالا چت",
    .cheatsheet_body = L"کلیدهای میان‌بر و راهنمای استفاده از امبالا چت:\n\n"
    L"  • F9 : فعال / توقف موقت\n"
    L"  • Ctrl + F9 : تغییر زبان مقصد\n"
    L"  • Ctrl + Shift + Enter : تغییر حالت ارسال خودکار\n"
    L"  • Shift + Enter : ترجمه و ارسال فوری\n\n"
    L"کنترل‌های ماوس روی نشان:\n"
    L"  • کلیک چپ : فعال / توقف موقت\n"
    L"  • دوبار کلیک : تعویض مبدأ ⇄ مقصد\n"
    L"  • کلیک راست : باز کردن منوی تنظیمات\n\n"
    L"حالت‌های ترجمه:\n"
    L"  • فقط جای‌گزینی (ارسال خودکار خاموش): خط را برای بازبینی با ترجمه جای‌گزین می‌کند.\n"
    L"  • ارسال خودکار (روشن): خط را جای‌گزین و فوراً Enter را می‌فشارد.",
    .about_title = L"دربارهٔ امبالا چت",
    .badge_active = L"فعال",
    .badge_translating = L"در حال ترجمه...",
    .badge_paused = L"متوقف",
    .tooltip_title = L"امبالا چت",
    .tooltip_copy_failed = L"متن انتخابی کپی نشد. برنامهٔ مقصد را بررسی کنید و دوباره تلاش کنید.",
    .tooltip_no_selection = L"متنی برای ترجمه انتخاب نشده است.",
    .auto_detect = L"تشخیص خودکار",
    .app_already_running = L"امبالا چت هم‌اکنون در پس‌زمینه در حال اجراست.\nسینی اعلان‌های سیستم را بررسی کنید.",
    .app_com_failed = L"مقداردهی COM ناموفق بود.\nنشان شناور و متن‌به‌کلام در دسترس نخواهند بود،\nاما ترجمه، میان‌برها، سینی و صداها همچنان کار می‌کنند.",
    .about_tagline = L"کپی-پیست را فراموش کنید. به زبان مادری‌تان طبیعی تایپ کنید — ترجمه در هر برنامهٔ ویندوزی در زمان واقعی جای تایپ شما می‌نشیند.",
    .about_feature0 = L"⚡ بکشید تا ترجمه شود — متنی را در هر برنامه‌ای انتخاب کنید، نماد شناور فوراً ترجمه می‌کند.",
    .about_feature1 = L"🔊 TTS عصبی — تلفظ هر ۳۷ زبان از طریق بسته‌های صدای نصب‌شده Windows.",
    .about_feature2 = L"🔒 ۱۰۰٪ روی دستگاه و خصوصی — فقط هنگام فشردن میان‌بر فعال است؛ کلیپ‌بورد دست‌نخورده.",
    .about_etymology = L"در سال ۲۰۰۰ پیش از میلاد، کاتبان بین‌النهرین «Eme-bala» می‌خواندند آنان را که زبان را پل میان جهان‌ها می‌کردند.",
    .about_link_website = L"وب‌سایت",
    .about_link_contact = L"تماس",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"تیم سانپلازا · سئول یونگ‌دونگ‌پو (اتاق 219، 65 یونگ‌جونگ-رو)",
    .about_contact_phone = L"+82 2 575 0414 · ساعت کاری 10:00–19:00 KST",
    .about_contact_lead = L"معمار ارشد: Yongtai Kim",
    .tooltip_copied = L"✓ کپی شد!",
    .tooltip_button_copy = L"📋 کپی",
    .tooltip_button_tts = L"🔊 گفتار",
    .menu_ui_language = L"زبان رابط کاربری",
    .menu_ui_language_auto = L"خودکار (زبان سیستم)",
    .about_reset_button = L"بازنشانی به پیش‌فرض‌های سیستم",
    .about_reset_done = L"پیش‌فرض‌ها بازگردانده شدند",
    .menu_typing_group = L"تایپ با کیبورد",
    .menu_tooltip_group = L"راهنمای ابزار کشیدن",
    .app_name = L"امبالا چت",
    .tooltip_no_tts_voice = L"هیچ صدای Windows برای این زبان نصب نشده است. برای باز کردن تنظیمات گفتار، دوباره روی 🔊 کلیک کنید.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"اعلان حریم خصوصی",
    .privacy_notice_body = L"اصول حریم خصوصی در Emebala Chat:\n"
    L"\n"
    L"• Emebala Chat سرور اختصاصی ندارد.\n"
    L"• با مدل محلی، متن ترجمه‌شده از دستگاه شما خارج نمی‌شود.\n"
    L"• اگر Google Translate را انتخاب کنید یا تبدیل خودکار به ابری رخ دهد، متن انتخابی یا تایپ‌شده برای ترجمه مستقیماً به Google ارسال می‌شود؛ نه از طریق Emebala.\n"
    L"• گزارش‌های تشخیصی به‌طور پیش‌فرض خاموش‌اند؛ از تنظیمات روشن می‌شوند (با انتخاب شما).\n"
    L"• اگر نمی‌خواهید متن به ابر (Google) ارسال شود، منوی نماد نوار وظیفه را باز کنید، “موتور ترجمه” و سپس “موتور ترجمه محلی داخلی” را انتخاب کنید. بدون نصب مدل محلی و با غیرفعال بودن جایگزین ابری، ترجمه انجام نمی‌شود — چیزی ارسال نمی‌شود.\n"
    L"\n"
    L"جزئیات کامل در فایل README است و هر زمان قابل خواندن مجدد است.\n",
    .cheatsheet_config_path = L"فایل پیکربندی: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"متن بسیار بلند بود: فقط ابتدا و انتها ترجمه شد.",
    .tooltip_untranslated_above = L"در بالا متن جدیدی بدون ترجمه وجود دارد. برای ترجمه، مکان‌نما را در انتهای آن خط قرار دهید و Enter را بزنید.",
    .repair_in_progress = L"در حال تعمیر اجزای موتور محلی…",
    .repair_failed_title = L"ترجمه محلی در دسترس نیست",
    .repair_failed_body = L"فایل‌های موتور ترجمه محلی پیدا نشدند، بنابراین ترجمه متوقف شده است. برای بازیابی موتور محلی، Emebala Chat را دوباره نصب کنید، یا برای تغییر به ترجمه ابری (Google)، از منوی سینی، «موتور ترجمه»، «Google ترجمه» را انتخاب کنید.",
    .menu_engine_openai = L"سازگار با OpenAI (سرور سفارشی)…",
    .openai_settings_title = L"تنظیمات موتور سازگار با OpenAI",
    .openai_settings_action = L"تنظیمات موتور سازگار با OpenAI…",
    .openai_base_url_label = L"آدرس پایه",
    .openai_api_key_label = L"کلید API",
    .openai_model_label = L"مدل",
    .openai_fetch_models = L"دریافت فهرست مدل‌ها",
    .openai_fetch_failed = L"دریافت فهرست مدل‌ها ناموفق بود. می‌توانید نام مدل را مستقیماً تایپ کنید.",
    .openai_http_warning_title = L"اتصال ناامن (HTTP)",
    .openai_http_warning_body = L"آدرس پایه از HTTP (رمزنگاری‌نشده) استفاده می‌کند. کلید API و متن شما به‌صورت رمزنگاری‌نشده ارسال خواهد شد. ادامه می‌دهید؟",
    .openai_saved = L"تنظیمات سازگار با OpenAI ذخیره شد.",
    .openai_key_masked = L"کلید ذخیره‌شده: ",
    .openai_invalid_base_url = L"آدرس پایه معتبر نیست. مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"مدل تعیین شده توسط کاربر (.gguf)",
    .menu_browse_gguf_file = L"ثبت مدل .gguf دیگر…",
    .user_gguf_quality_title = L"اطلاعیه درباره کیفیت ترجمه",
    .user_gguf_quality_body = L"مدل انتخاب‌شده Hy-MT2 نیست. نسخه فعلی از پرامپت مخصوص Hy-MT2 استفاده می‌کند، بنابراین کیفیت ترجمه با این مدل تضمین نمی‌شود. ادامه می‌دهید؟",
    .user_gguf_registered_title = L"مدل ثبت شد",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"مدل انتخاب‌شده در موتور محلی ثبت شد.\n"
    L"\n"
    L"این مدل هنگامی برای ترجمه استفاده می‌شود که \"انتخاب موتور ترجمه > انتخاب کاربر (.gguf)\" را برگزینید.\n"
    L"\n"
    L"چه زمانی اعمال می‌شود: حداکثر حدود ۱ دقیقه پس از ثبت (پس از خروج موتور به دلیل عدم فعالیت)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"این مدل از قبل در Emebala Chat داخلی شده است. نیازی به ثبت‌نام نیست. می‌توانید موتور ترجمه محلی داخلی را مستقیماً انتخاب کنید.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ثبت نشده)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"مدیریت مدل‌ها…",
    .gguf_manager_title = L"مدیریت مدل‌های کاربر",
    .gguf_manager_empty = L"هیچ مدل کاربری ثبت نشده است.",
    .gguf_manager_rename = L"تغییر نام…",
    .gguf_manager_delete = L"حذف…",
    .gguf_manager_close = L"بستن",
    .gguf_manager_delete_confirm_title = L"حذف ثبت مدل",
    .gguf_manager_delete_confirm_body = L"ثبت مدل انتخاب‌شده حذف خواهد شد.\n"
    L"\n"
    L"فایل مدل (.gguf) روی دیسک باقی می‌ماند و حذف نمی‌شود. اگر از این مدل استفاده می‌شد، انتخاب موتور ترجمه به حالت خودکار (Auto) بازمی‌گردد.\n"
    L"\n"
    L"ادامه می‌دهید؟",
    .gguf_manager_rename_title = L"تغییر نام مدل",
    .gguf_manager_rename_body = L"نام جدید را وارد کنید (حداکثر ۶۴ نویسه، بدون فاصله).",
    .gguf_manager_rename_invalid = L"این نام قابل استفاده نیست. نامی غیرخالی، متفاوت از نام‌های موجود، بدون فاصله یا جداکنندهٔ مسیر و حداکثر ۶۴ نویسه وارد کنید.",
    .gguf_manager_done = L"تغییرات ذخیره شد.",
    .gguf_manager_err_serialize = L"registry.json قابل سریال‌سازی نیست (نام فایل رد شد). هیچ چیز تغییر نکرد.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% در دسترس نیست؛ پوشه مشترک مدل‌ها پیدا نشد.",
    .gguf_manager_err_write = L"registry.json نوشته نشد.",
    .gguf_manager_err_write_partial = L"registry.json به‌طور کامل نوشته نشد.",
    .gguf_manager_err_registry_damaged = L"registry.json آسیب دیده یا دارای طرح پشتیبانی‌نشده است. تغییر داده نشده است. آن را تعمیر یا حذف کنید و دوباره تلاش کنید.",
    .tooltip_translate_failed = L"ترجمه ناموفق بود. دوباره تلاش کنید.",
    .repair_transient_body = L"موتور ترجمه محلی به‌طور موقت در دسترس نیست (ممکن است در حال راه‌اندازی باشد). کمی صبر کنید و دوباره تلاش کنید.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"تأیید",
    .dialog_cancel = L"انصراف",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"افزودن از فایل…",
    .gguf_manager_add_hf = L"افزودن از Hugging Face…",
    .hf_add_title = L"افزودن مدل Hugging Face",
    .hf_url_label = L"URL مدل",
    .hf_downloading = L"در حال دانلود…",
    .hf_failed = L"دانلود ناموفق بود.",
    .hf_invalid_url = L"URL مدل huggingface.co را وارد کنید (resolve، blob یا صفحه مدل)",
    .hf_done = L"ثبت شد.",
    .openai_base_url_hint = L"مثلاً: https://api.openai.com/v1",
    .openai_api_key_hint = L"کلید API",
    // REQ-051: دکمه [حذف تنظیمات].
    .openai_delete_settings = L"حذف تنظیمات",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"بررسی به‌روزرسانی‌ها",
    .update_idle_note = L"ببینید آیا از آخرین نسخه Emebala Chat استفاده می‌کنید.",
    .update_checking = L"در حال بررسی به‌روزرسانی‌ها…",
    .update_checking_hint = L"در پس‌زمینه اجرا می‌شود؛ می‌توانید این پنجره را ببندید.",
    .update_uptodate = L"از آخرین نسخه استفاده می‌کنید",
    .update_dlg_title = L"به‌روزرسانی در دسترس است",
    .update_dlg_version = L"نسخه جدید",
    .update_dlg_version_cur = L"نسخه فعلی شما {v}",
    .update_dlg_size = L"اندازه",
    .update_dlg_size_approx = L"حدود {s}",
    .update_dlg_privacy = L"بررسی و دانلود به GitHub متصل می‌شود.",
    .update_dlg_download = L"دانلود",
    .update_dlg_later = L"بعداً",
    .update_dl_progress = L"در حال دانلود…",
    .update_dl_cancel = L"لغو",
    .update_dl_cancelled = L"دانلود لغو شد.",
    .update_ready_title = L"دانلود کامل شد — آماده نصب",
    .update_ready_warn = L"برنامه برای نصب بسته خواهد شد.",
    .update_ready_install = L"اکنون نصب کنید",
    .update_ready_later = L"بعداً",
    .update_err_offline = L"آفلاین هستید. اتصال خود را بررسی کنید و دوباره تلاش کنید.",
    .update_err_rate = L"درخواست‌ها بیش از حد است. لطفاً کمی بعد دوباره تلاش کنید.",
    .update_err_hash = L"فایل دانلود‌شده تأیید نشد.",
    .update_err_retry = L"تلاش مجدد",
    .update_snoozed_line = L"می‌توانید به‌روزرسانی را بعداً نصب کنید.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"دانلود مدل",
    .model_dl_consent_body = L"مدل ترجمه داخلی نصب نشده است. اکنون دانلود شود؟ (حدود {s})",
    .model_dl_privacy = L"دانلود به huggingface.co متصل می‌شود.",
    .model_dl_download = L"اکنون دانلود کنید",
    .model_dl_verifying = L"در حال بررسی فایل دانلودشده…",
    .model_dl_placing = L"در حال نصب مدل…",
    .model_dl_done = L"مدل آماده است. موتور محلی از ترجمه بعدی از آن استفاده می‌کند.",
    .model_dl_failed = L"دانلود ناموفق بود. اتصال خود را بررسی کنید و دوباره تلاش کنید.",
    .model_dl_disk_space = L"فضای خالی دیسک کافی نیست (حدود {s} لازم است).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"تنظیمات بارگذاری مدل ترجمه…",
    .engine_load_settings_title = L"بارگذاری مدل ترجمه",
    .load_mode_on_use = L"بارگذاری هنگام استفاده (توصیه‌شده)",
    .load_mode_warmup = L"بارگذاری از قبل",
    .load_mode_on_use_desc = L"حافظه را ذخیره می‌کند. اولین ترجمه ممکن است چند ثانیه دیرتر شروع شود.",
    .load_mode_warmup_desc = L"ترجمه بلافاصله شروع می‌شود. مدل در حافظه باقی می‌ماند.",
    .gguf_policy_sticky = L"(ماندن در حافظه)",
    .gguf_policy_evict = L"(بارگذاری هنگام استفاده)",
    .gguf_manager_toggle_policy = L"تغییر سیاست",
    .gguf_duplicate_warning = L"فایل‌های تکراری: {n} {s} × {c}",
    .gguf_manager_dedupe = L"پاک‌سازی تکراری‌ها",
    .gguf_manager_dedupe_done = L"فایل‌های تکراری به سطل بازیافت منتقل شدند.",
    .gguf_manager_dedupe_failed = L"پاک‌سازی فایل‌های تکراری ممکن نشد. بررسی کنید که برنامه دیگری از آن‌ها استفاده نمی‌کند.",
    .menu_ui_theme = L"تم رابط کاربری",
    .theme_fluent = L"فلوئنت کرافت",
    .theme_editorial = L"وارم ادیتوریال",
    .theme_monolith = L"ریفایند مونولیت",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"نمایش ترجمهٔ معکوس شناور (بررسی)",
    .back_translate_header = L"🔄 بررسی ترجمهٔ معکوس",
    .back_translate_match_exact = L"✓ معنا مطابقت دارد",
    .back_translate_match_check = L"⚡ بررسی ظرافت معنا",
    .back_translate_reference = L"↳ اصلی: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"فایل مدل GGUF معتبری نیست؛ ثبت نشد.",
    .gguf_manager_active_suffix = L"(در حال استفاده)",
    .gguf_manager_activated = L"این مدل اکنون در حال استفاده است.",
};
// 33. Urdu (ur) — RTL
const LocalizedStrings kStringsUrdu = {
    .menu_status_active = L"حالت: فعال (F9: وقفہ)",
    .menu_status_paused = L"حالت: وقفے میں (F9: جاری رکھیں)",
    .menu_engine = L"ترجمہ انجن",
    .menu_engine_google = L"Google ٹرانسلیٹ (مفت / بغیر تنصیب)",
    .menu_engine_local = L"بلٹ اِن مقامی انجن (Hy-MT2-1.8B آف لائن)",
    .menu_source_lang = L"ماخذ زبان (ان پٹ)",
    .menu_target_lang = L"ہدف زبان (آؤٹ پٹ)",
    .menu_swap_langs = L"ماخذ ⇄ ہدف بدلیں (ڈبل کلک)",
    .menu_auto_send = L"Enter پر خودکار ارسال",
    .menu_sound_feedback = L"آواز کا ردعمل (ٹونز)",
    .menu_show_badge = L"فلوٹنگ بیج دکھائیں",
    .menu_start_with_windows = L"Windows کے ساتھ شروع",
    .menu_cheatsheet = L"شارٹ کٹ چِٹ شیٹ اور مدد...",
    .menu_exit = L"ایمیبالا چیٹ سے باہر نکلیں",
    .menu_about = L"ایمیبالا چیٹ کے بارے میں…",
    .cheatsheet_title = L"ایمیبالا چیٹ — شارٹ کٹس اور استعمال گائیڈ",
    .cheatsheet_body = L"ایمیبالا چیٹ شارٹ کٹس اور استعمال گائیڈ:\n\n"
    L"  • F9 : فعال / وقفہ سوئچ کریں\n"
    L"  • Ctrl + F9 : ہدف زبان بدلیں\n"
    L"  • Ctrl + Shift + Enter : خودکار ارسال ٹوگل کریں\n"
    L"  • Shift + Enter : فوراً ترجمہ اور ارسال کریں\n\n"
    L"بیج پر ماؤس کنٹرول:\n"
    L"  • بائیں کلک : فعال / وقفہ\n"
    L"  • ڈبل کلک : ماخذ ⇄ ہدف بدلیں\n"
    L"  • دائیں کلک : سیٹنگز مینو کھولیں\n\n"
    L"ترجمہ موڈز:\n"
    L"  • صرف متبادل (خودکار ارسال بند): سطر کو جائزے کے لیے ترجمے سے بدل دیتا ہے۔\n"
    L"  • خودکار ارسال (چالو): سطر بدل کر فوراً Enter دبا دیتا ہے۔",
    .about_title = L"ایمیبالا چیٹ کے بارے میں",
    .badge_active = L"فعال",
    .badge_translating = L"ترجمہ جاری...",
    .badge_paused = L"وقفہ",
    .tooltip_title = L"ایمیبالا چیٹ",
    .tooltip_copy_failed = L"منتخب متن کاپی نہیں ہو سکا۔ ہدف ایپ چیک کریں اور دوبارہ کوشش کریں۔",
    .tooltip_no_selection = L"ترجمے کے لیے کوئی متن منتخب نہیں ہے۔",
    .auto_detect = L"خودکار شناخت",
    .app_already_running = L"ایمیبالا چیٹ پہلے سے پس منظر میں چل رہا ہے۔\nسسٹم نوٹیفکیشن ٹرے کو دیکھیں۔",
    .app_com_failed = L"COM آغاز ناکام۔\nفلوٹنگ بیج اور متن از آواز دستیاب نہیں ہوں گے،\nلیکن ترجمہ، شارٹ کٹس، ٹرے اور آوازیں چلتی رہیں گی۔",
    .about_tagline = L"کاپی پیسٹ کی ضرورت ختم۔ اپنی مادری زبان میں قدرتی ٹائپ کریں — ترجمہ کسی بھی Windows ایپ میں آپ کی ٹائپنگ کو وقتی طور پر بدل دیتا ہے۔",
    .about_feature0 = L"⚡ کھینچیں اور ترجمہ کریں — کسی بھی ایپ میں متن منتخب کریں، فلوٹنگ آئیکن فوراً ترجمہ کر دیتا ہے۔",
    .about_feature1 = L"🔊 نیورل TTS — انسٹال شد Windows وائس پیکس کے ذریعے تمام 37 زبانوں کا تلفظ۔",
    .about_feature2 = L"🔒 100% ڈیوائس پر اور نجی — جب تک شارٹ کٹ دبا رہے تب تک فعال؛ کلپ بورڈ کو ہاتھ نہیں لگاتا۔",
    .about_etymology = L"2000 ق م، میسوپوٹیمیا کے کاتبوں نے انہیں «Eme-bala» کہا جنہوں نے زبان کو دنیاؤں کے درمیان پل بنایا۔",
    .about_link_website = L"ویب سائٹ",
    .about_link_contact = L"رابطہ",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"ٹیم سنپلازہ · سول یونگڈونگپو (کمرہ 219، 65 یونگجنگ-رو)",
    .about_contact_phone = L"+82 2 575 0414 · آفس اوقات 10:00–19:00 KST",
    .about_contact_lead = L"لیڈ آرکیٹیکٹ: Yongtai Kim",
    .tooltip_copied = L"✓ کاپی ہو گیا!",
    .tooltip_button_copy = L"📋 کاپی",
    .tooltip_button_tts = L"🔊 آواز",
    .menu_ui_language = L"انٹرفیس زبان",
    .menu_ui_language_auto = L"خودکار (سسٹم زبان)",
    .about_reset_button = L"سسٹم ڈیفالٹس پر ری سیٹ",
    .about_reset_done = L"ڈیفالٹس بحال ہو گئے",
    .menu_typing_group = L"کی بورڈ ٹائپنگ",
    .menu_tooltip_group = L"ڈریگ ٹول ٹپ",
    .app_name = L"ایمیبالا چیٹ",
    .tooltip_no_tts_voice = L"اس زبان کے لیے کوئی Windows وائس انسٹال نہیں ہے۔ اسپیچ سیٹنگز کھولنے کے لیے 🔊 پر دوبارہ کلک کریں۔",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"رازداری کا اعلان",
    .privacy_notice_body = L"Emebala Chat کے رازداری کے اصول:\n"
    L"\n"
    L"• Emebala Chat کا اپنا کوئی سرور نہیں ہے۔\n"
    L"• لوکل ماڈل پر ترجمہ شدہ متن آپ کے ڈیوائس سے باہر نہیں جاتا۔\n"
    L"• اگر آپ Google Translate منتخب کریں یا خودکار طور پر کلاؤڈ پر سوئچ ہو، تو منتخب/ٹائپ شدہ متن ترجمے کے لیے براہ راست Google کو جاتا ہے — Emebala کے ذریعے نہیں۔\n"
    L"• ڈائگناسٹک لاگ ڈیفالٹ طور پر بند (OFF) ہیں؛ سیٹنگز میں آن کریں (opt-in)۔\n"
    L"• اگر آپ متن کلاؤڈ (Google) میں نہیں بھیجنا چاہتے تو سسٹم ٹری آئیکن کا مینو کھولیں، “ترجمہ انجن” میں سے “بلٹ اِن مقامی انجن” منتخب کریں۔ اگر مقامی ماڈل انسٹال نہیں ہے اور کلاؤڈ فال بیک بند ہے تو ترجمہ نہیں چلے گا — کچھ بھی نہیں بھیجا جائے گا۔\n"
    L"\n"
    L"مکمل تفصیل README فائل میں ہے، جسے آپ کسی بھی وقت دوبارہ پڑھ سکتے ہیں۔\n",
    .cheatsheet_config_path = L"کنفیگ فائل: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"متن بہت لمبا تھا: صرف آغاز اور انجام کا ترجمہ کیا گیا۔",
    .tooltip_untranslated_above = L"اوپر نیا غیر ترجمہ شدہ متن ہے۔ اس کا ترجمہ کرنے کے لیے کرسر کو اس لائن کے آخر میں رکھیں اور Enter دبائیں۔",
    .repair_in_progress = L"مقامی انجن اجزا کی مرمت جاری ہے…",
    .repair_failed_title = L"مقامی ترجمہ دستیاب نہیں ہے",
    .repair_failed_body = L"مقامی ترجمہ انجن کی فائلیں نہیں ملیں، اس لیے ترجمہ روک دیا گیا ہے۔ مقامی انجن کو بحال کرنے کے لیے Emebala Chat کو دوبارہ انسٹال کریں، یا کلاؤڈ (Google) ترجمے پر جانے کے لیے، ٹرے مینیو سے “ترجمہ انجن” میں “Google ترجمہ” منتخب کریں۔",
    .menu_engine_openai = L"OpenAI مطابقت (صارف کی وضاحت کردہ سرور)…",
    .openai_settings_title = L"OpenAI مطابق انجن سیٹنگز",
    .openai_settings_action = L"OpenAI مطابق انجن سیٹنگز…",
    .openai_base_url_label = L"بیس URL",
    .openai_api_key_label = L"API کلید",
    .openai_model_label = L"ماڈل",
    .openai_fetch_models = L"ماڈل فہرست حاصل کریں",
    .openai_fetch_failed = L"ماڈل فہرست حاصل نہ ہو سکی۔ آپ ماڈل کا نام براہ راست لکھ سکتے ہیں۔",
    .openai_http_warning_title = L"غیر محفوظ کنکشن (HTTP)",
    .openai_http_warning_body = L"بیس URL HTTP (غیر خفیہ) استعمال کرتا ہے۔ آپ کی API کلید اور متن سادہ متن میں بھیجا جائے گا۔ جاری رکھیں؟",
    .openai_saved = L"OpenAI مطابق سیٹنگز محفوظ کر دی گئیں۔",
    .openai_key_masked = L"محفوظ کلید: ",
    .openai_invalid_base_url = L"بیس URL درست نہیں۔ مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"صارف کی وضاحت کردہ ماڈل (.gguf)",
    .menu_browse_gguf_file = L"کوئی اور .gguf ماڈل رجسٹر کریں…",
    .user_gguf_quality_title = L"ترجمے کے معیار کا نوٹس",
    .user_gguf_quality_body = L"منتخب ماڈل Hy-MT2 نہیں ہے۔ موجودہ ورژن صرف Hy-MT2 کے لیے پرامپٹ استعمال کرتا ہے، اس لیے اس ماڈل کے ترجمے کی کوالٹی ضمانت نہیں۔ جاری رکھیں؟",
    .user_gguf_registered_title = L"ماڈل رجسٹر ہو گیا",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"منتخب ماڈل لوکل انجن میں رجسٹر کر دیا گیا ہے۔\n"
    L"\n"
    L"جب آپ \"ترجمہ انجن کا انتخاب > صارف کا انتخاب (.gguf)\" چنتے ہیں تو اس ماڈل کا ترجمے میں استعمال ہوتا ہے۔\n"
    L"\n"
    L"کب لاگو: رجسٹریشن کے بعد زیادہ سے زیادہ تقریباً 1 منٹ (انجن کے غیر فعال ہونے کے بعد)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"یہ ماڈل پہلے سے ہی Emebala Chat میں شامل ہے۔ رجسٹریشن کی ضرورت نہیں ہے۔ بلٹ اِن لوکل ترجمہ انجن کو براہ راست منتخب کریں۔",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(رجسٹرڈ نہیں)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"ماڈلز منظم کریں…",
    .gguf_manager_title = L"صارف ماڈل منیجر",
    .gguf_manager_empty = L"کوئی صارف ماڈل رجسٹرڈ نہیں۔",
    .gguf_manager_rename = L"نام تبدیل کریں…",
    .gguf_manager_delete = L"حذف کریں…",
    .gguf_manager_close = L"بند کریں",
    .gguf_manager_delete_confirm_title = L"ماڈل رجسٹریشن حذف کریں",
    .gguf_manager_delete_confirm_body = L"منتخب ماڈل کی رجسٹریشن حذف کر دی جائے گی۔\n"
    L"\n"
    L"ماڈل فائل (.gguf) ڈسک پر محفوظ رہتی ہے اور حذف نہیں ہوتی۔ اگر یہ ماڈل استعمال ہو رہا تھا، تو ترجمہ انجن کا انتخاب خودکار (Auto) پر واپس آ جائے گا۔\n"
    L"\n"
    L"جاری رکھیں؟",
    .gguf_manager_rename_title = L"ماڈل کا نام تبدیل کریں",
    .gguf_manager_rename_body = L"نیا نام درج کریں (زیادہ سے زیادہ 64 حروف، بغیر خالی جگہ کے)۔",
    .gguf_manager_rename_invalid = L"وہ نام استعمال نہیں ہو سکتا۔ کوئی خالی نہیں، موجودہ ناموں سے مختلف، بغیر خالی جگہ یا راستہ جداکننے والے، 64 حروف کے اندر نام درج کریں۔",
    .gguf_manager_done = L"تبدیلیاں محفوظ کر دی گئیں۔",
    .gguf_manager_err_serialize = L"registry.json کو سیریلائز نہیں کیا جا سکا (فائل کا نام مسترد کر دیا گیا)۔ کچھ نہیں بدلا۔",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% دستیاب نہیں؛ مشترکہ ماڈل فولڈر نہیں مل سکا۔",
    .gguf_manager_err_write = L"registry.json نہیں لکھی جا سکی۔",
    .gguf_manager_err_write_partial = L"registry.json مکمل نہیں لکھی جا سکی۔",
    .gguf_manager_err_registry_damaged = L"registry.json خراب ہے یا اس کا اسکیما معاونت یافتہ نہیں۔ اسے تبدیل نہیں کیا گیا۔ اسے ٹھیک کریں یا حذف کریں اور دوبارہ کوشش کریں۔",
    .tooltip_translate_failed = L"ترجمہ ناکام رہا۔ دوبارہ کوشش کریں۔",
    .repair_transient_body = L"لوکل ترجمہ انجن عارضی طور پر دستیاب نہیں (ہو سکتا ہے شروع ہو رہا ہو)۔ تھوڑی دیر انتظار کریں اور دوبارہ کوشش کریں۔",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"ٹھیک ہے",
    .dialog_cancel = L"منسوخ",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"فائل سے شامل کریں…",
    .gguf_manager_add_hf = L"Hugging Face سے شامل کریں…",
    .hf_add_title = L"Hugging Face ماڈل شامل کریں",
    .hf_url_label = L"ماڈل کا URL",
    .hf_downloading = L"ڈاؤن لوڈ ہو رہا ہے…",
    .hf_failed = L"ڈاؤن لوڈ ناکام ہوا۔",
    .hf_invalid_url = L"huggingface.co کا ماڈل URL درج کریں (resolve، blob یا ماڈل صفحہ)",
    .hf_done = L"رجسٹر ہو گیا۔",
    .openai_base_url_hint = L"مثال: https://api.openai.com/v1",
    .openai_api_key_hint = L"API کلید",
    // REQ-051: [ترتیبات حذف کریں] بٹن۔
    .openai_delete_settings = L"ترتیبات حذف کریں",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"اپڈیٹس کی جانچ کریں",
    .update_idle_note = L"دیکھیں کہ آیا آپ تازہ ترین Emebala Chat چلا رہے ہیں۔",
    .update_checking = L"اپڈیٹس کی جانچ ہو رہی ہے…",
    .update_checking_hint = L"یہ پس منظر میں چلتا ہے؛ آپ اس ونڈو کو بند کر سکتے ہیں۔",
    .update_uptodate = L"آپ تازہ ترین ورژن استعمال کر رہے ہیں",
    .update_dlg_title = L"اپڈیٹ دستیاب ہے",
    .update_dlg_version = L"نیا ورژن",
    .update_dlg_version_cur = L"آپ کے پاس {v} ہے",
    .update_dlg_size = L"سائز",
    .update_dlg_size_approx = L"تقریباً {s}",
    .update_dlg_privacy = L"جانچ اور ڈاؤن لوڈ GitHub سے جڑتا ہے۔",
    .update_dlg_download = L"ڈاؤن لوڈ کریں",
    .update_dlg_later = L"بعد میں",
    .update_dl_progress = L"ڈاؤن لوڈ ہو رہا ہے…",
    .update_dl_cancel = L"منسوخ کریں",
    .update_dl_cancelled = L"ڈاؤن لوڈ منسوخ کر دیا گیا۔",
    .update_ready_title = L"ڈاؤن لوڈ مکمل — انسٹال کے لیے تیار",
    .update_ready_warn = L"انسٹال کرنے کے لیے ایپ بند ہو جائے گی۔",
    .update_ready_install = L"ابھی انسٹال کریں",
    .update_ready_later = L"بعد میں",
    .update_err_offline = L"آپ آف لائن ہیں۔ اپنا کنکشن چیک کریں اور دوبارہ کوشش کریں۔",
    .update_err_rate = L"بہت زیادہ درخواستیں ہیں۔ براہ کرم کچھ دیر بعد دوبارہ کوشش کریں۔",
    .update_err_hash = L"ڈاؤن لوڈ کی گئی فائل تصدیق نہیں ہو سکی۔",
    .update_err_retry = L"دوبارہ کوشش کریں",
    .update_snoozed_line = L"آپ اپڈیٹ بعد میں انسٹال کر سکتے ہیں۔",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"ماڈل ڈاؤن لوڈ کریں",
    .model_dl_consent_body = L"بلٹ اِن ترجمہ ماڈل انسٹال نہیں ہے۔ ابھی ڈاؤن لوڈ کریں؟ (تقریباً {s})",
    .model_dl_privacy = L"ڈاؤن لوڈ huggingface.co سے جڑتا ہے۔",
    .model_dl_download = L"ابھی ڈاؤن لوڈ کریں",
    .model_dl_verifying = L"ڈاؤن لوڈ کی گئی فائل کی تصدیق ہو رہی ہے…",
    .model_dl_placing = L"ماڈل انسٹال ہو رہا ہے…",
    .model_dl_done = L"ماڈل تیار ہے۔ مقامی انجن اگلے ترجمے سے اسے استعمال کرے گا۔",
    .model_dl_failed = L"ڈاؤن لوڈ ناکام رہا۔ اپنا کنکشن چیک کریں اور دوبارہ کوشش کریں۔",
    .model_dl_disk_space = L"ڈسک پر کافی خالی جگہ نہیں ہے (تقریباً {s} درکار ہے)۔",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"ترجمہ ماڈل لوڈ سیٹنگز…",
    .engine_load_settings_title = L"ترجمہ ماڈل لوڈنگ",
    .load_mode_on_use = L"استعمال پر لوڈ کریں (تجویز کردہ)",
    .load_mode_warmup = L"پہلے سے لوڈ کریں",
    .load_mode_on_use_desc = L"میموری بچاتا ہے۔ پہلا ترجمہ چند سیکنڈ دیر سے شروع ہو سکتا ہے۔",
    .load_mode_warmup_desc = L"ترجمہ فوراً شروع ہوتا ہے۔ ماڈل میموری میں رہتا ہے۔",
    .gguf_policy_sticky = L"(میموری میں رہے)",
    .gguf_policy_evict = L"(استعمال پر لوڈ کریں)",
    .gguf_manager_toggle_policy = L"پالیسی بدلیں",
    .gguf_duplicate_warning = L"ڈپلیکیٹ فائلیں: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ڈپلیکیٹ صاف کریں",
    .gguf_manager_dedupe_done = L"ڈپلیکیٹ فائلیں ریسائیکل بِن میں منتقل کر دی گئیں۔",
    .gguf_manager_dedupe_failed = L"ڈپلیکیٹ فائلیں صاف نہیں ہو سکیں۔ چیک کریں کہ کوئی اور پروگرام ان کا استعمال تو نہیں کر رہا۔",
    .menu_ui_theme = L"یو آئی تھیم",
    .theme_fluent = L"فلوئنٹ کرافٹ",
    .theme_editorial = L"وارم ایڈیٹوریل",
    .theme_monolith = L"ریفائنڈ مونولٹ",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"فلوٹنگ معکوس ترجمہ دکھائیں (تصدیق)",
    .back_translate_header = L"🔄 معکوس ترجمے کی تصدیق",
    .back_translate_match_exact = L"✓ معنی مطابق ہے",
    .back_translate_match_check = L"⚡ باریکی کی جانچ کریں",
    .back_translate_reference = L"↳ اصل: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"یہ درست GGUF ماڈل فائل نہیں ہے؛ اسے رجسٹر نہیں کیا گیا۔",
    .gguf_manager_active_suffix = L"(استعمال میں)",
    .gguf_manager_activated = L"اب یہ ماڈل استعمال میں ہے۔",
};
// 34. Hebrew (he) — RTL
const LocalizedStrings kStringsHebrew = {
    .menu_status_active = L"סטטוס: פעיל (F9: השהיה)",
    .menu_status_paused = L"סטטוס: מושהה (F9: המשך)",
    .menu_engine = L"מנוע תרגום",
    .menu_engine_google = L"Google Translate (חינם / ללא התקנה)",
    .menu_engine_local = L"מנוע תרגום מקומי מובנה (Hy-MT2-1.8B לא מקוון)",
    .menu_source_lang = L"שפת מקור (קלט)",
    .menu_target_lang = L"שפת יעד (פלט)",
    .menu_swap_langs = L"החלפת מקור ⇄ יעד (לחיצה כפולה)",
    .menu_auto_send = L"שליחה אוטומטית ב-Enter",
    .menu_sound_feedback = L"משוב קולי (צלילים)",
    .menu_show_badge = L"תג צף גלוי",
    .menu_start_with_windows = L"הפעל עם Windows",
    .menu_cheatsheet = L"מדריך קיצורי מקשים ועזרה...",
    .menu_exit = L"צא מ-אמבאלה צ'אט",
    .menu_about = L"אודות אמבאלה צ'אט…",
    .cheatsheet_title = L"אמבאלה צ'אט — קיצורי מקשים ומדריך שימוש",
    .cheatsheet_body = L"קיצורי המקשים ומדריך השימוש של אמבאלה צ'אט:\n\n"
    L"  • F9 : הפעלה / השהיה\n"
    L"  • Ctrl + F9 : החלפת שפת יעד\n"
    L"  • Ctrl + Shift + Enter : החלפת מצב שליחה אוטומטית\n"
    L"  • Shift + Enter : תרגום ושליחה מיידית\n\n"
    L"פקידי העכבר על התג הצף:\n"
    L"  • לחיצה שמאלית : הפעלה / השהיה\n"
    L"  • לחיצה כפולה : החלפת מקור ⇄ יעד\n"
    L"  • לחיצה ימנית : פתיחת תפריט ההגדרות\n\n"
    L"מצבי תרגום:\n"
    L"  • החלפה בלבד (שליחה אוטומטית כבויה): מחליף את השורה בתרגום לסקירה.\n"
    L"  • שליחה אוטומטית (דלוקה): מחליף את השורה ולוחץ Enter מיד.",
    .about_title = L"אודות אמבאלה צ'אט",
    .badge_active = L"פעיל",
    .badge_translating = L"מתרגם...",
    .badge_paused = L"מושהה",
    .tooltip_title = L"אמבאלה צ'אט",
    .tooltip_copy_failed = L"לא ניתן להעתיק את הטקסט הנבחר. בדוק את אפליקציית היעד ונסה שוב.",
    .tooltip_no_selection = L"לא נבחר טקסט לתרגום.",
    .auto_detect = L"זיהוי אוטומטי",
    .app_already_running = L"אמבאלה צ'אט כבר פועל ברקע.\nבדוק את מגש ההודעות.",
    .app_com_failed = L"אתחול COM נכשל.\nהתג הצף והקראת טקסט לא יהיו זמינים,\nאך תרגום, קיצורי מקשים, מגש וצלילים ימשיכו לפעול.",
    .about_tagline = L"להיפרד מהעתקה והדבקה. הקלדו בטבעיות בשפת האם — התרגום מחליף את ההקלדה בזמן אמת בכל אפליקציית Windows.",
    .about_feature0 = L"⚡ גררו ותרגמו — סמנו טקסט בכל אפליקציה, הסמל הצף מתרגם מיד.",
    .about_feature1 = L"🔊 TTS עצבי — מדבר את כל 37 השפות באמצעות חבילות קול של Windows מותקנות.",
    .about_feature2 = L"🔒 100% מקומי ופרטי — פעיל רק בעוד קיצור המקשים מוחזק; לוח גזירים נשאר נקי.",
    .about_etymology = L"בשנת 2000 לפנה\"ס כינו סופרי מסופוטמיה «Eme-bala» — אלה שהפכו שפה לגשר בין עולמות.",
    .about_link_website = L"אתר אינטרנט",
    .about_link_contact = L"יצירת קשר",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · סאול יונגדונגפו (Room 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · שעות משרד 10:00–19:00 KST",
    .about_contact_lead = L"ארכיטקט מוביל: Yongtai Kim",
    .tooltip_copied = L"✓ הועתק!",
    .tooltip_button_copy = L"📋 העתק",
    .tooltip_button_tts = L"🔊 הקראה",
    .menu_ui_language = L"שפת הממשק",
    .menu_ui_language_auto = L"אוטומטי (שפת המערכת)",
    .about_reset_button = L"איפוס לברירות מחדל של המערכת",
    .about_reset_done = L"ברירות המחדל שוחזרו",
    .menu_typing_group = L"הקלדה במקלדת",
    .menu_tooltip_group = L"רמז גרירה",
    .app_name = L"אמבאלה צ'אט",
    .tooltip_no_tts_voice = L"אין קול Windows מותקן עבור שפה זו. לחץ שוב על 🔊 כדי לפתוח את הגדרות הדיבור.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"הודעת פרטיות",
    .privacy_notice_body = L"עקרונות הפרטיות של Emebala Chat:\n"
    L"\n"
    L"• ל־Emebala Chat אין שרתים משלה.\n"
    L"• במודל מקומי, הטקסט המתורגם אינו עוזב את המכשיר שלך.\n"
    L"• אם תבחר ב־Google Translate או שהמעבר לענן יתבצע אוטומטית, הטקסט הנבחר או המוקלד נשלח ישירות ל־Google לצורך תרגום, ולא דרך Emebala.\n"
    L"• יומני אבחון כבויים כברירת מחדל; הפעל אותם בהגדרות (הסכמה מפורשת).\n"
    L"• אם אינך רוצה לשלוח טקסט לענן (Google), פתח את תפריט סמל מגש המערכת, בחר “מנוע תרגום” ואז “מנוע תרגום מקומי מובנה”. ללא מודל מקומי מותקן ועם גיבוי ענן מושבת, התרגום לא יפעל — שום דבר לא נשלח.\n"
    L"\n"
    L"הפירוט המלא בקובץ README, הניתן לקריאה חוזרת בכל עת.\n",
    .cheatsheet_config_path = L"קובץ ההגדרות: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"הטקסט ארוך מדי: רק הפתיח והסיום תורגמו.",
    .tooltip_untranslated_above = L"יש טקסט חדש שלא תורגם למעלה. מקם את הסמן בסוף השורה ההיא ולחץ Enter כדי לתרגם.",
    .repair_in_progress = L"מתקן את רכיבי המנוע המקומי…",
    .repair_failed_title = L"התרגום המקומי אינו זמין",
    .repair_failed_body = L"קבצי מנוע התרגום המקומי חסרים, לכן התרגום הושהה. התקינו מחדש את Emebala Chat כדי לשחזר את המנוע המקומי, או כדי לעבור לתרגום בענן (Google), בחרו “Google תרגום” בתפריט השורה, “מנוע תרגום”.",
    .menu_engine_openai = L"תואם OpenAI (שרת מותאם אישית)…",
    .openai_settings_title = L"הגדרות מנוע תואם OpenAI",
    .openai_settings_action = L"הגדרות מנוע תואם OpenAI…",
    .openai_base_url_label = L"כתובת בסיס",
    .openai_api_key_label = L"מפתח API",
    .openai_model_label = L"מודל",
    .openai_fetch_models = L"משוך רשימת מודלים",
    .openai_fetch_failed = L"לא ניתן למשוך את רשימת המודלים. אפשר להקליד שם מודל ישירות.",
    .openai_http_warning_title = L"חיבור לא מאובטח (HTTP)",
    .openai_http_warning_body = L"כתובת הבסיס משתמשת ב־HTTP (לא מוצפן). מפתח ה־API והטקסט שלך יישלחו כטקסט גלוי. להמשיך?",
    .openai_saved = L"הגדרות תואמות OpenAI נשמרו.",
    .openai_key_masked = L"מפתח שמור: ",
    .openai_invalid_base_url = L"כתובת הבסיס אינה חוקית. דוגמה: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"מודל שצוין על ידי המשתמש (.gguf)",
    .menu_browse_gguf_file = L"רישום מודל .gguf אחר…",
    .user_gguf_quality_title = L"הודעה על איכות התרגום",
    .user_gguf_quality_body = L"המודל שנבחר אינו Hy-MT2. הגרסה הנוכחית משתמשת בפרומפט ייעודי ל־Hy-MT2 בלבד, ולכן איכות התרגום עם מודל זה אינה מובטחת. להמשיך?",
    .user_gguf_registered_title = L"המודל נרשם",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"המודל שנבחר נרשם במנוע המקומי.\n"
    L"\n"
    L"מודל זה משמש לתרגום כאשר בוחרים \"בחירת מנוע תרגום > בחירת משתמש (.gguf)\".\n"
    L"\n"
    L"מתי זה נכנס לתוקף: עד כדקה אחת לאחר ההרשמה (לאחר סיום פעולת המנוע עקב חוסר פעילות)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"מודל זה כבר מובנה בתוך Emebala Chat. אין צורך ברישום. ניתן לבחור ישירות את מנוע התרגום המקומי המובנה.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(לא רשום)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"ניהול מודלים…",
    .gguf_manager_title = L"מנהל מודלים של המשתמש",
    .gguf_manager_empty = L"אין מודלים של משתמש רשומים.",
    .gguf_manager_rename = L"שינוי שם…",
    .gguf_manager_delete = L"מחיקה…",
    .gguf_manager_close = L"סגירה",
    .gguf_manager_delete_confirm_title = L"מחיקת רישום המודל",
    .gguf_manager_delete_confirm_body = L"רישום המודל שנבחר יימחק.\n"
    L"\n"
    L"קובץ המודל (.gguf) נשמר על הדיסק ואינו נמחק. אם המודל היה בשימוש, הבחירה במנוע התרגום תחזור ל־אוטומטי (Auto).\n"
    L"\n"
    L"להמשיך?",
    .gguf_manager_rename_title = L"שינוי שם המודל",
    .gguf_manager_rename_body = L"הזן שם חדש (עד 64 תווים, ללא רווחים).",
    .gguf_manager_rename_invalid = L"אי אפשר להשתמש בשם הזה. הזן שם שאינו ריק, שונה מהקיימים, ללא רווחים או מפרידי נתיב, בעד 64 תווים.",
    .gguf_manager_done = L"השינויים נשמרו.",
    .gguf_manager_err_serialize = L"לא ניתן היה לסריאליזם את registry.json (שם קובץ נדחה). לא שונה דבר.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% אינו זמין; לא ניתן לאתר את תיקיית המודלים המשותפת.",
    .gguf_manager_err_write = L"לא ניתן היה לכתוב את registry.json.",
    .gguf_manager_err_write_partial = L"registry.json לא נכתב במלואו.",
    .gguf_manager_err_registry_damaged = L"registry.json פגום או בעל סכימה לא נתמכת. הוא לא שונה. תקנו או הסירו אותו ונסו שוב.",
    .tooltip_translate_failed = L"התרגום נכשל. נסה שוב.",
    .repair_transient_body = L"מנוע התרגום המקומי אינו זמין זמנית (ייתכן שהוא בתהליך הפעלה). המתן רגע ונסה שוב.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"אישור",
    .dialog_cancel = L"ביטול",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"הוספה מקובץ…",
    .gguf_manager_add_hf = L"הוספה מ-Hugging Face…",
    .hf_add_title = L"הוספת מודל Hugging Face",
    .hf_url_label = L"כתובת URL של המודל",
    .hf_downloading = L"מוריד…",
    .hf_failed = L"ההורדה נכשלה.",
    .hf_invalid_url = L"הזן את כתובת ה-URL של המודל מ-huggingface.co (resolve, blob או דף המודל)",
    .hf_done = L"נרשם.",
    .openai_base_url_hint = L"לדוגמה: https://api.openai.com/v1",
    .openai_api_key_hint = L"מפתח API",
    // REQ-051: לחצן [מחיקת הגדרות].
    .openai_delete_settings = L"מחיקת הגדרות",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"בדיקת עדכונים",
    .update_idle_note = L"בדוק אם אתה משתמש בגרסה העדכנית ביותר של Emebala Chat.",
    .update_checking = L"מבצע בדיקת עדכונים…",
    .update_checking_hint = L"פועל ברקע; אתה יכול לסגור את החלון הזה.",
    .update_uptodate = L"הגרסה שלך עדכנית",
    .update_dlg_title = L"עדכון זמין",
    .update_dlg_version = L"גרסה חדשה",
    .update_dlg_version_cur = L"הגרסה שלך {v}",
    .update_dlg_size = L"גודל",
    .update_dlg_size_approx = L"כ-{s}",
    .update_dlg_privacy = L"הבדיקה וההורדה מתחברות אל GitHub.",
    .update_dlg_download = L"הורדה",
    .update_dlg_later = L"מאוחר יותר",
    .update_dl_progress = L"מוריד…",
    .update_dl_cancel = L"ביטול",
    .update_dl_cancelled = L"ההורדה בוטלה.",
    .update_ready_title = L"ההורדה הושלמה — מוכן להתקנה",
    .update_ready_warn = L"האפליקציה תיסגר כדי להתקין.",
    .update_ready_install = L"התקן עכשיו",
    .update_ready_later = L"מאוחר יותר",
    .update_err_offline = L"אתה לא מחובר. בדוק את החיבור ונסה שוב.",
    .update_err_rate = L"יותר מדי בקשות. נסה שוב בעוד מעט.",
    .update_err_hash = L"הקובץ שהורד לא עבר אימות.",
    .update_err_retry = L"נסה שוב",
    .update_snoozed_line = L"ניתן להתקין את העדכון מאוחר יותר.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"הורדת המודל",
    .model_dl_consent_body = L"מודל התרגום המובנה אינו מותקן. להוריד אותו עכשיו? (כ-{s})",
    .model_dl_privacy = L"ההורדה מתחברת אל huggingface.co.",
    .model_dl_download = L"הורד עכשיו",
    .model_dl_verifying = L"הקובץ שהורד נבדק…",
    .model_dl_placing = L"המודל מותקן…",
    .model_dl_done = L"המודל מוכן. המנוע המקומי ישתמש בו מהתרגום הבא.",
    .model_dl_failed = L"ההורדה נכשלה. בדוק את החיבור ונסה שוב.",
    .model_dl_disk_space = L"אין מספיק מקום פנוי בדיסק (נדרשים כ-{s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"הגדרות טעינת מודל התרגום…",
    .engine_load_settings_title = L"טעינת מודל התרגום",
    .load_mode_on_use = L"טעינה בעת שימוש (מומלץ)",
    .load_mode_warmup = L"טעינה מראש",
    .load_mode_on_use_desc = L"חוסך זיכרון. התרגום הראשון עשוי להתחיל כמה שניות מאוחר יותר.",
    .load_mode_warmup_desc = L"התרגום מתחיל מיד. המודל נשאר בזיכרון.",
    .gguf_policy_sticky = L"(להשאיר בזיכרון)",
    .gguf_policy_evict = L"(טעינה בעת שימוש)",
    .gguf_manager_toggle_policy = L"החלפת מדיניות",
    .gguf_duplicate_warning = L"קבצים כפולים: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ניקוי כפילויות",
    .gguf_manager_dedupe_done = L"הקבצים הכפולים הועברו לסל המיחזור.",
    .gguf_manager_dedupe_failed = L"לא ניתן היה לנקות את הקבצים הכפולים. בדוק אם תוכנית אחרת משתמשת בהם.",
    .menu_ui_theme = L"ערכת עיצוב",
    .theme_fluent = L"פלואנט קראפט",
    .theme_editorial = L"ווארם אדיטוריאל",
    .theme_monolith = L"ריפיינד מונוליט",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"הצגת תרגום חוזר צף (אימות)",
    .back_translate_header = L"🔄 אימות תרגום חוזר",
    .back_translate_match_exact = L"✓ המשמעות תואמת",
    .back_translate_match_check = L"⚡ בדיקת ניואנס",
    .back_translate_reference = L"↳ מקור: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"זהו לא קובץ מודל GGUF תקין; הוא לא נרשם.",
    .gguf_manager_active_suffix = L"(בשימוש)",
    .gguf_manager_activated = L"מודל זה עכשיו בשימוש.",
};
// 35. Khmer (km)
const LocalizedStrings kStringsKhmer = {
    .menu_status_active = L"ស្ថានភាព៖ សកម្ម (F9៖ ផ្អាក)",
    .menu_status_paused = L"ស្ថានភាព៖ បានផ្អាក (F9៖ បន្ត)",
    .menu_engine = L"ម៉ាស៊ីនបកប្រែ",
    .menu_engine_google = L"Google បកប្រែ (ឥតគិតថ្លៃ / គ្មានការដំឡើង)",
    .menu_engine_local = L"ម៉ាស៊ីនក្នុងស្រាប់ (Hy-MT2-1.8B ក្រៅបណ្តាញ)",
    .menu_source_lang = L"ភាសាប្រភព (បញ្ចូល)",
    .menu_target_lang = L"ភាសាគោលដៅ (បញ្ចេញ)",
    .menu_swap_langs = L"ប្ដូរប្រភព ⇄ គោលដៅ (ចុចទ្វេដង)",
    .menu_auto_send = L"ផ្ញើស្វ័យប្រវត្តិពេលចុច Enter",
    .menu_sound_feedback = L"ការឆ្លើយតបជាសំឡេង (តូន)",
    .menu_show_badge = L"បង្ហាញផ្លាកអណ្ដែត",
    .menu_start_with_windows = L"ដំណើរការជាមួយ Windows",
    .menu_cheatsheet = L"តារាងគន្លឹះក្ដារចុច និងជំនួយ...",
    .menu_exit = L"ចាកចេញពី អេមេបាឡា ឆាត",
    .menu_about = L"អំពី អេមេបាឡា ឆាត…",
    .cheatsheet_title = L"គន្លឹះ និងការណ៍នាំប្រើ អេមេបាឡា ឆាត",
    .cheatsheet_body = L"គន្លឹះ និងការណ៍នាំប្រើ អេមេបាឡា ឆាត:\n\n"
    L"  • F9 : បើក / ផ្អាក\n"
    L"  • Ctrl + F9 : ប្ដូរភាសាគោលដៅ\n"
    L"  • Ctrl + Shift + Enter : បិទ/បើកការផ្ញើស្វ័យប្រវត្តិ\n"
    L"  • Shift + Enter : បកប្រែរួចផ្ញើភ្លាម\n\n"
    L"ការគ្រប់គ្រងដោយកណ្ដុលលើផ្លាក៖\n"
    L"  • ចុចឆ្វេង : បើក / ផ្អាក\n"
    L"  • ចុចទ្វេដង : ប្ដូរប្រភព ⇄ គោលដៅ\n"
    L"  • ចុចខ្វា : បើកម៉ឺនុយការកំណត់\n\n"
    L"របៀបបកប្រែ៖\n"
    L"  • ជំនួសតែប៉ុណ្ណោះ (ផ្ញើស្វ័យប្រវត្តិបិទ)៖ ជំនួសបន្ទាត់ដោយការបកប្រែសម្រាប់ពិនិត្យ។\n"
    L"  • ផ្ញើស្វ័យប្រវត្តិ (បើក)៖ ជំនួសបន្ទាត់ រួចចុច Enter ភ្លាម។",
    .about_title = L"អំពី អេមេបាឡា ឆាត",
    .badge_active = L"សកម្ម",
    .badge_translating = L"កំពុងបកប្រែ...",
    .badge_paused = L"បានផ្អាក",
    .tooltip_title = L"អេមេបាឡា ឆាត",
    .tooltip_copy_failed = L"មិនអាចចម្លងអត្ថបទដែលបានជ្រើសរើសទេ។ សូមពិនិត្យកម្មវិធីគោលដៅ ហើយព្យាយាមម្ដងទៀត។",
    .tooltip_no_selection = L"គ្មានអត្ថបទត្រូវបានជ្រើសសម្រាប់បកប្រែទេ។",
    .auto_detect = L"កំណត់ស្វ័យប្រវត្តិ",
    .app_already_running = L"អេមេបាឡា ឆាត កំពុងដំណើរការក្នុងផ្ទៃខាងក្រោយរួចជាស្រេច។\nសូមពិនិត្យតំបន់ជូនដំណឹងរបស់ប្រព័ន្ធ។",
    .app_com_failed = L"ការដំណើរការ COM បានបរាជ័យ។\nផ្លាកអណ្ដែត និងការអានអត្ថបទជាសំឡេងនឹងមិនអាចប្រើបានទេ\nប៉ុន្តែការបកប្រែ គន្លឹះ តំបន់ជូនដំណឹង និងសំឡេង នៅតែដំណើរការ។",
    .about_tagline = L"ឈប់ចម្លងបិទភ្ជាប់។ សរសេរដោយធម្មជាតិជាភាសាកំណើតរបស់អ្នក — ការបកប្រែនឹងជំនួសអក្សរដែលអ្នកបោះ ជានិច្ចកាលនៅក្នុងកម្មវិធី Windows ណាក៏បាន។",
    .about_feature0 = L"⚡ អូសដើម្បីបកប្រែ — ជ្រើសរើសអត្ថបទក្នុងកម្មវិធីណាក៏បាន សញ្ញាអណ្ដែតបកប្រែភ្លាម។",
    .about_feature1 = L"🔊 TTS បណ្ដាញប្រសាទ — បញ្ចេញសំឡេងគ្រប់ភាសាទាំង 37 តាមរយៈកញ្ចប់សំឡេង Windows ដែលបានដំឡើង។",
    .about_feature2 = L"🔒 100% នៅលើឧបករណ៍ និងឯកជន — សកម្មតែពេលគន្លឹះកំពុងចុច; ក្ដារតម្កេញមិនត្រូវប៉ះទេ។",
    .about_etymology = L"នៅឆ្នាំ 2000 មុនគ្រិស្តសករាជ កាតិកាម៉េសូប៉ូតាមីយាបានហៅ «Eme-bala» ចំពោះមនុស្សដែលធ្វើឱ្យភាសាក្លាយជាស្ពានភ្ជាប់ពិភព។",
    .about_link_website = L"គេហទំព័រ",
    .about_link_contact = L"ទំនាក់ទំនង",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"ក្រុម Team Sunplaza · សេអ៊ូល Yeongdeungpo (បន្ទប់ 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · ម៉ោងធ្វើការ 10:00–19:00 KST",
    .about_contact_lead = L"ស្ថាបតិករចម្បង៖ Yongtai Kim",
    .tooltip_copied = L"✓ បានចម្លង!",
    .tooltip_button_copy = L"📋 ចម្លង",
    .tooltip_button_tts = L"🔊 សំឡេង",
    .menu_ui_language = L"ភាសាចំណុចប្រទាក់",
    .menu_ui_language_auto = L"ស្វ័យប្រវត្តិ (ភាសាប្រព័ន្ធ)",
    .about_reset_button = L"កំណត់ឡើងវិញទៅលំនាំដើមប្រព័ន្ធ",
    .about_reset_done = L"លំនាំដើមត្រូវបានត្រឡប់មកវិញ",
    .menu_typing_group = L"ការវាយអក្សរលើក្ដារចុច",
    .menu_tooltip_group = L"បន្ទាត់ណែនាំពេលអូស",
    .app_name = L"អេមេបាឡា ឆាត",
    .tooltip_no_tts_voice = L"មិនមានសំឡេង Windows ត្រូវបានដំឡើងសម្រាប់ភាសានេះទេ។ ចុច 🔊 ម្តងទៀតដើម្បីបើកការកំណត់ការនិយាយ។",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"ការជូនដំណឹងអំពីឯកជនភាព",
    .privacy_notice_body = L"គោលការណ៍ឯកជនភាពរបស់ Emebala Chat មានដូចខាងក្រោម៖\n"
    L"\n"
    L"• Emebala Chat មិនដំណើរការសាឺវឺរបស់ខ្លួនទេ។\n"
    L"• ពេលប្រើម៉ូដែលក្នុងឧបករណ៍ អត្ថបទបកប្រែមិនចេញពីឧបករណ៍របស់អ្នកទេ។\n"
    L"• បើអ្នកជ្រើស Google Translate ឬប្ដូរទៅពពកដោយស្វ័យប្រវត្តិ អត្ថបទដែលជ្រើស ឬបោះពុម្ពត្រូវផ្ញើទៅ Google ដោយផ្ទាល់ដើម្បីបកប្រែ ដោយមិនឆ្លងកាត់ Emebala ទេ។\n"
    L"• កំណត់ហេតុធ្វើរោគវិនិច្ឆ័យត្រូវបានបិទដោយលំនាំដើម ហើយត្រូវបើកក្នុងការកំណត់ (ជ្រើសរើសចូលរួម)។\n"
    L"• ប្រសិនបើអ្នកមិនចង់ផ្ញើអត្ថបទទៅ cloud (Google) សូមបើកម៉ឺនុយរូបតំណាងនៅថតការងារ ហើយជ្រើសរើស “ម៉ាស៊ីនបកប្រែ” រួច “ម៉ាស៊ីនក្នុងស្រាប់”។ ប្រសិនបើគ្មានគំរូក្នុងម៉ាស៊ីនត្រូវបានដំឡើង ហើយការបម្រុងទុក cloud ត្រូវបានបិទ ការបកប្រែនឹងមិនដំណើរការទេ — គ្មានអ្វីត្រូវបានផ្ញើឡើយ។\n"
    L"\n"
    L"ព័ត៌មានលម្អិតស្ថិតក្នុងឯកសារ README ដែលអាចអានឡើងវិញពេលណាក៏បាន។\n",
    .cheatsheet_config_path = L"ឯកសារកំណត់រចនាសម្ព័ន្ធ: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"អត្ថបទវែងពេក៖ បានបកប្រែតែផ្នែកដើម និងបញ្ចប់ប៉ុណ្ណោះ។",
    .tooltip_untranslated_above = L"មានអត្ថបទថ្មីមិនទាន់បកប្រែខាងលើ។ ដើម្បីបកប្រែ សូមដាក់កូរ៉េស័រនៅចុងបន្ទាត់នោះ ហើយចុច Enter។",
    .repair_in_progress = L"កំពុងជួសជុលសមាសភាគម៉ាស៊ីនក្នុងម៉ាស៊ីន…",
    .repair_failed_title = L"ការបកប្រែក្នុងម៉ាស៊ីនមិនអាចប្រើបានទេ",
    .repair_failed_body = L"រកមិនឃើញឯកសារម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនទេ ដូច្នេះការបកប្រែត្រូវបានផ្អាកជាបណ្តោះអាសន្ន។ ដើម្បីស្តារម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនឡើងវិញ សូមដំឡើង Emebala Chat ម្តងទៀត ឬដើម្បីប្តូរទៅការបកប្រែក្នុងពពក (Google) សូមជ្រើសរើស “Google បកប្រែ” ពីម៉ឺនុយ tray នៅ “ម៉ាស៊ីនបកប្រែ”។",
    .menu_engine_openai = L"ស្រប OpenAI (ម៉ាស៊ីនមេដែលអ្នកប្រើប្រាស់កំណត់)…",
    .openai_settings_title = L"ការកំណត់ម៉ាស៊ីនឆបែល OpenAI",
    .openai_settings_action = L"ការកំណត់ម៉ាស៊ីនឆបែល OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"កូនសោ API",
    .openai_model_label = L"ម៉ូដែល",
    .openai_fetch_models = L"ទាញយកបញ្ជីម៉ូដែល",
    .openai_fetch_failed = L"មិនអាចទាញយកបញ្ជីម៉ូដែលបានទេ។ អ្នកអាចវាយឈ្មោះម៉ូដែលដោយផ្ទាល់។",
    .openai_http_warning_title = L"ការតភ្ជាប់មិនសុវត្ថិភាព (HTTP)",
    .openai_http_warning_body = L"Base URL ប្រើ HTTP (មិនបានអ៊ិនគ្រីប)។ កូនសោ API និងអត្ថបទរបស់អ្នកនឹងត្រូវផ្ញើជាអក្សរធម្មតា។ បន្ត?",
    .openai_saved = L"បានរក្សាទុកការកំណត់ឆបែល OpenAI។",
    .openai_key_masked = L"កូនសោដែលបានរក្សាទុក៖ ",
    .openai_invalid_base_url = L"Base URL មិនត្រឹមត្រូវ។ ឧទាហរណ៍៖ https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"ម៉ូដែលដែលអ្នកប្រើប្រាស់កំណត់ (.gguf)",
    .menu_browse_gguf_file = L"ចុះឈ្មោះម៉ូដែល .gguf ផ្សេងទៀត…",
    .user_gguf_quality_title = L"សេចក្ដីជូនដំណឹងអំពីគុណភាពបកប្រែ",
    .user_gguf_quality_body = L"ម៉ូដែលដែលបានជ្រើសរើសមិនមែនជា Hy-MT2 ទេ។ កំណែបច្ចុប្បន្នប្រើប្រាស់ prompt សម្រាប់តែ Hy-MT2 ដូច្នេះគុណភាពបកប្រែជាមួយម៉ូដែលនេះមិនត្រូវបានធានាទេ។ បន្ត?",
    .user_gguf_registered_title = L"បានចុះឈ្មោះម៉ូដែល",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"បានចុះឈ្មោះម៉ូដែលដែលបានជ្រើសរើសជាមួយម៉ាស៊ីនក្នុងសៀ។\n"
    L"\n"
    L"ម៉ូដែលនេះត្រូវបានប្រើសម្រាប់បកប្រែនៅពេលអ្នកជ្រើសរើស \"ការជ្រើសរើសម៉ាស៊ីនបកប្រែ > ជម្រើសរបស់អ្នកប្រើ (.gguf)\"។\n"
    L"\n"
    L"ពេលវេលាមישប្រើៈ យ៉ាងយូរប្រហែល 1 នាទីក្រោយពេលចុះឈ្មោះ (បន្ទាប់ពីម៉ាស៊ីនបញ្ចប់ដោយសារអសកម្ម)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"ម៉ូដែលនេះមានស្រាប់ក្នុង Emebala Chat រួចហើយ។ មិនចាំបាច់ចុះឈ្មោះទេ។ អ្នកអាចជ្រើសរើសម៉ាស៊ីនបកប្រែក្នុងស្រុកដែលមានស្រាប់ដោយផ្ទាល់។",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(មិនទាន់ចុះឈ្មោះ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"គ្រប់គ្រងម៉ូដែល…",
    .gguf_manager_title = L"កម្មវិធីគ្រប់គ្រងម៉ូដែលអ្នកប្រើ",
    .gguf_manager_empty = L"មិនមានម៉ូដែលអ្នកប្រើដែលបានចុះឈ្មោះទេ។",
    .gguf_manager_rename = L"ប្ដូរឈ្មោះ…",
    .gguf_manager_delete = L"លុប…",
    .gguf_manager_close = L"បិទ",
    .gguf_manager_delete_confirm_title = L"លុបការចុះឈ្មោះម៉ូដែល",
    .gguf_manager_delete_confirm_body = L"ការចុះឈ្មោះនៃម៉ូដែលដែលបានជ្រើសរើស នឹងត្រូវលុប។\n"
    L"\n"
    L"ឯកសារម៉ូដែល (.gguf) នៅតែស្ថិតនៅលើថាស ហើយមិនត្រូវបានលុបទេ។ ប្រសិនបើម៉ូដែលនេះកំពុងប្រើប្រាស់ ការជ្រើសរើសម៉ាស៊ីនបកប្រែ នឹងត្រលប់ទៅស្វ័យប្រវត្តិ (Auto)។\n"
    L"\n"
    L"បន្ត?",
    .gguf_manager_rename_title = L"ប្ដូរឈ្មោះម៉ូដែល",
    .gguf_manager_rename_body = L"បញ្ចូលឈ្មោះថ្មី (អតិបរមា ៦៤ តួអក្សរ គ្មានដកឃ្លា)។",
    .gguf_manager_rename_invalid = L"មិនអាចប្រើឈ្មោះនោះបានទេ។ សូមបញ្ចូលឈ្មោះមិនទទេ ខុសពីឈ្មោះដែលមានស្រាប់ គ្មានដកឃ្លា ឬសញ្ញាបំបែកផ្លូវ ក្នុងចំណោម ៦៤ តួអក្សរ។",
    .gguf_manager_done = L"បានរក្សាទុកការផ្លាស់ប្ដូរ។",
    .gguf_manager_err_serialize = L"មិនអាច serialize registry.json បានទេ (ឈ្មោះឯកសារត្រូវបានបដិសេធ)។ មិនមានអ្វីត្រូវបានផ្លាស់ប្ដូរទេ។",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% មិនអាចប្រើប្រាស់បានទេ; រកថតម៉ូដែលរួមមិនឃើញ។",
    .gguf_manager_err_write = L"មិនអាចសរសេរ registry.json បានទេ។",
    .gguf_manager_err_write_partial = L"registry.json មិនត្រូវបានសរសេរពេញលេញទេ។",
    .gguf_manager_err_registry_damaged = L"registry.json ខូចឬមានស្គីមាមិនត្រូវបានគាំទ្រ។ វាមិនត្រូវបានកែប្រាងទេ។ ជួសជុលឬលុបវាចេញ រួចព្យាយាមម្ដងទៀត។",
    .tooltip_translate_failed = L"ការបកប្រែបរាជ័យ។ សូមព្យាយាមម្ដងទៀត។",
    .repair_transient_body = L"ម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនមិនអាចប្រើប្រាស់បណ្តោះអាសន្ន (អាចកំពុងចាប់ផ្តើម)។ សូមរង់ចាំបន្តិចហើយព្យាយាមម្ដងទៀត។",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"យល់ព្រម",
    .dialog_cancel = L"បោះបង់",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"បន្ថែមពីឯកសារ…",
    .gguf_manager_add_hf = L"បន្ថែមពី Hugging Face…",
    .hf_add_title = L"បន្ថែមម៉ូដែល Hugging Face",
    .hf_url_label = L"URL របស់ម៉ូដែល",
    .hf_downloading = L"កំពុងទាញយក…",
    .hf_failed = L"ការទាញយកបរាជ័យ។",
    .hf_invalid_url = L"បញ្ចូល URL ម៉ូដែលពី huggingface.co (resolve, blob ឬទំព័រម៉ូដែល)",
    .hf_done = L"បានចុះឈ្មោះ។",
    .openai_base_url_hint = L"ឧ. https://api.openai.com/v1",
    .openai_api_key_hint = L"កូនសោ API",
    // REQ-051: ប៊ូតុង [លុបការកំណត់]។
    .openai_delete_settings = L"លុបការកំណត់",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"ពិនិត្យការធ្វើបច្ចុប្បន្នភាព",
    .update_idle_note = L"សូមពិនិត្យថាអ្នកកំពុងប្រើ Emebala Chat ជំនាន់ចុងក្រោយឬទេ។",
    .update_checking = L"កំពុងពិនិត្យការធ្វើបច្ចុប្បន្នភាព…",
    .update_checking_hint = L"ដំណើរការនៅផ្នែកខាងក្រោយ អ្នកអាចបិទបង្អួចនេះបាន។",
    .update_uptodate = L"អ្នកកំពុងប្រើជំនាន់ចុងក្រោយហើយ",
    .update_dlg_title = L"មានបច្ចុប្បន្នភាពថ្មី",
    .update_dlg_version = L"ជំនាន់ថ្មី",
    .update_dlg_version_cur = L"អ្នកកំពុងប្រើ {v}",
    .update_dlg_size = L"ទំហំ",
    .update_dlg_size_approx = L"ប្រហែល {s}",
    .update_dlg_privacy = L"ការពិនិត្យនិងទាញយកភ្ជាប់ទៅ GitHub។",
    .update_dlg_download = L"ទាញយក",
    .update_dlg_later = L"ពេលក្រោយ",
    .update_dl_progress = L"កំពុងទាញយក…",
    .update_dl_cancel = L"បោះបង់",
    .update_dl_cancelled = L"បានបោះបង់ការទាញយកហើយ",
    .update_ready_title = L"ទាញយករួចរាល់ — រួចរាល់សម្រាប់ដំឡើង",
    .update_ready_warn = L"កម្មវិធីនឹងបិទដើម្បីដំឡើង។",
    .update_ready_install = L"ដំឡើងឥឡូវនេះ",
    .update_ready_later = L"ពេលក្រោយ",
    .update_err_offline = L"អ្នកកំពុងក្រៅបណ្ដាញ។ សូមពិនិត្យការភ្ជាប់ រួចព្យាយាមម្ដងទៀត។",
    .update_err_rate = L"សំណើច្រើនពេក។ សូមព្យាយាមម្ដងទៀតបន្តិចទៀត។",
    .update_err_hash = L"ឯកសារដែលបានទាញយកមិនបានឆ្លងការផ្ទៀងផ្ទាត់។",
    .update_err_retry = L"ព្យាយាមម្ដងទៀត",
    .update_snoozed_line = L"អ្នកអាចដំឡើងបច្ចុប្បន្នភាពនេះពេលក្រោយបាន។",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"ទាញយកម៉ូដែល",
    .model_dl_consent_body = L"ម៉ូដែលបកប្រែដែលបានបង្កប់មិនត្រូវបានដំឡើងទេ។ ទាញយកឥឡូវនេះ? (ប្រហែល {s})",
    .model_dl_privacy = L"ការទាញយកភ្ជាប់ទៅ huggingface.co។",
    .model_dl_download = L"ទាញយកឥឡូវនេះ",
    .model_dl_verifying = L"កំពុងផ្ទៀងផ្ទាត់ឯកសារដែលបានទាញយក…",
    .model_dl_placing = L"កំពុងដំឡើងម៉ូដែល…",
    .model_dl_done = L"ម៉ូដែលត្រៀមរួចរាល់។ ម៉ាស៊ីនក្នុងស្រុកនឹងប្រើវាចាប់ពីការបកប្រែបន្ទាប់។",
    .model_dl_failed = L"ការទាញយកបរាជ័យ។ សូមពិនិត្យការភ្ជាប់ ហើយព្យាយាមម្តងទៀត។",
    .model_dl_disk_space = L"ទំហំទីតាំងទំនេរលើថាសមិនគ្រប់គ្រាន់ (ត្រូវការប្រហែល {s})។",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"ការកំណត់ការផ្ទុកម៉ូដែលបកប្រែ…",
    .engine_load_settings_title = L"ការផ្ទុកម៉ូដែលបកប្រែ",
    .load_mode_on_use = L"ផ្ទុកពេលប្រើប្រាស់ (ណែនាំ)",
    .load_mode_warmup = L"ផ្ទុកជាមុន",
    .load_mode_on_use_desc = L"សន្សំសំណំ។ ការបកប្រែដំបូងអាចយឺតរយៈពេលប៉ុន្មានវិនាទី។",
    .load_mode_warmup_desc = L"ការបកប្រែចាប់ផ្តើមភ្លាមៗ។ ម៉ូដែលនៅតែស្ថិតក្នុងសំណំ។",
    .gguf_policy_sticky = L"(រក្សាទុកក្នុងសំណំ)",
    .gguf_policy_evict = L"(ផ្ទុកពេលប្រើប្រាស់)",
    .gguf_manager_toggle_policy = L"ប្តូរគោលការណ៍",
    .gguf_duplicate_warning = L"ឯកសារស្ទួន: {n} {s} × {c}",
    .gguf_manager_dedupe = L"សម្អាតឯកសារស្ទួន",
    .gguf_manager_dedupe_done = L"ឯកសារស្ទួនត្រូវបានផ្លាស់ទីទៅក្នុងធុងសំរាម។",
    .gguf_manager_dedupe_failed = L"មិនអាចសម្អាតឯកសារស្ទួនបានទេ។ សូមពិនិត្យមើលថាកម្មវិធីផ្សេងកំពុងប្រើប្រាស់ពួកវាឬទេ។",
    .menu_ui_theme = L"ធីមចំណុច",
    .theme_fluent = L"ហ្វ៉លូអង់គ្រាហ្វ",
    .theme_editorial = L"វ៉ាមអេឌីទូរៀល",
    .theme_monolith = L"រីហ្វែយរិនម៉ូណូលីត",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"បង្ហាញការបកប្រែត្រឡប់អណ្តែត (ផ្ទៀងផ្ទាត់)",
    .back_translate_header = L"🔄 ផ្ទៀងផ្ទាត់ការបកប្រែត្រឡប់",
    .back_translate_match_exact = L"✓ អត្ថន័យត្រូវគ្នា",
    .back_translate_match_check = L"⚡ ពិនិត្យអត្ថរស",
    .back_translate_reference = L"↳ ដើម: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"ឯកសារម៉ូដែល GGUF មិនត្រឹមត្រូវ ទើបមិនបានចុះឈ្មោះ។",
    .gguf_manager_active_suffix = L"(កំពុងប្រើ)",
    .gguf_manager_activated = L"ម៉ូដែលនេះត្រូវបានកំណត់ឱ្យកំពុងប្រើ។",
};
// 36. Lao (lo)
const LocalizedStrings kStringsLao = {
    .menu_status_active = L"ສະຖານະ: ເປີດໃຊ້ (F9: ຢຸດຊົ່ວຄາວ)",
    .menu_status_paused = L"ສະຖານະ: ຢຸດຊົ່ວຄາວ (F9: ສືບຕໍ່)",
    .menu_engine = L"ເຄື່ອງຈັກແປ",
    .menu_engine_google = L"Google ແປ (ຟຣີ / ບໍ່ຕ້ອງຕິດຕັ້ງ)",
    .menu_engine_local = L"ເຄື່ອງຈັກພາຍໃນທ້ອງຖິ່ນ (Hy-MT2-1.8B ອັອບໄລນ໌)",
    .menu_source_lang = L"ພາສາຕົ້ນສະບັບ (ຂໍ້ມູນເຂົ້າ)",
    .menu_target_lang = L"ພາສາເປົ້າໝາຍ (ຂໍ້ມູນອອກ)",
    .menu_swap_langs = L"ສັບປ່ຽນຕົ້ນ ⇄ ເປົ້າໝາຍ (ກລິກສອງຄັ້ງ)",
    .menu_auto_send = L"ສົ່ງອັດຕະໂນມັດເມື່ອກົດ Enter",
    .menu_sound_feedback = L"ສຽງຕອບກັບ (ໂທນ)",
    .menu_show_badge = L"ສະແດງແຖບປ້າຍລອຍ",
    .menu_start_with_windows = L"ເປີດຄູ່ກັບ Windows",
    .menu_cheatsheet = L"ຄີດທາງລັດ ແລະ ຄູ່ມືຊ່ວຍເຫຼືອ...",
    .menu_exit = L"ອອກຈາກ ເອເມບາລາ ແຊັດ",
    .menu_about = L"ກ່ຽວກັບ ເອເມບາລາ ແຊັດ…",
    .cheatsheet_title = L"ຄີດທາງລັດ ແລະ ຄູ່ມືການໃຊ້ ເອເມບາລາ ແຊັດ",
    .cheatsheet_body = L"ຄີດທາງລັດ ແລະ ຄູ່ມືການໃຊ້ ເອເມບາລາ ແຊັດ:\n\n"
    L"  • F9 : ເປີດ / ຢຸດຊົ່ວຄາວ\n"
    L"  • Ctrl + F9 : ປ່ຽນພາສາເປົ້າໝາຍ\n"
    L"  • Ctrl + Shift + Enter : ສະຫຼັບໂໝດສົ່ງອັດຕະໂນມັດ\n"
    L"  • Shift + Enter : ແປແລ້ວສົ່ງທັນທີ\n\n"
    L"ການຄວບຄຸມດ້ວຍເມົາສ໌ເທິງປ້າຍ:\n"
    L"  • ກລິກຊ້າຍ : ເປີດ / ຢຸດຊົ່ວຄາວ\n"
    L"  • ກລິກສອງຄັ້ງ : ສັບປ່ຽນຕົ້ນ ⇄ ເປົ້າໝາຍ\n"
    L"  • ກລິກຂວາ : ເປີດເມນູການຕັ້ງຄ່າ\n\n"
    L"ໂໝດການແປ:\n"
    L"  • ແທນທີ່ເທົ່ານັ້ນ (ສົ່ງອັດຕະໂນມັດປິດ): ແທນອັນລະເພາະດ້ວຍຄຳແປເພື່ອກວດສອບ.\n"
    L"  • ສົ່ງອັດຕະໂນມັດ (ເປີດ): ແທນແລ້ວກົດ Enter ທັນທີ.",
    .about_title = L"ກ່ຽວກັບ ເອເມບາລາ ແຊັດ",
    .badge_active = L"ເປີດໃຊ້",
    .badge_translating = L"ກຳລັງແປ...",
    .badge_paused = L"ຢຸດຊົ່ວຄາວ",
    .tooltip_title = L"ເອເມບາລາ ແຊັດ",
    .tooltip_copy_failed = L"ບໍ່ສາມາດລອກເອົາຂໍ້ຄວາມທີ່ເລືອກໄດ້. ກະລຸນາກວດສອບແອັບເປົ້າໝາຍ ແລ້ວລອງໃໝ່.",
    .tooltip_no_selection = L"ຍັງບໍ່ໄດ້ເລືອກຂໍ້ຄວາມສຳລັບແປ.",
    .auto_detect = L"ກວດຈັບອັດຕະໂນມັດ",
    .app_already_running = L"ເອເມບາລາ ແຊັດ ກຳລັງປະຕິບັດຢູ່ພື້ນຫຼັງແລ້ວ.\nກະລຸນາກວດເບິ່ງຖາດແຈ້ງເຕືອນຂອງລະບົບ.",
    .app_com_failed = L"ການເລີ່ມຕົ້ນ COM ລົ້ມເຫຼວ.\nແຖບປ້າຍລອຍ ແລະ ການອ່ານຂໍ້ຄວາມຈະໃຊ້ບໍ່ໄດ້,\nແຕ່ການແປ, ຄີດທາງລັດ, ຖາດ ແລະ ສຽງຍັງເຮັດວຽກຕາມປົກກະຕິ.",
    .about_tagline = L"ພໍໄດ້ກັບການລອກແຜ່. ພິມຢ່າງທຳມະຊາດເປັນພາສາແມ່ຂອງທ່ານ — ຄຳແປຈະແທນການພິມຂອງທ່ານທັນທີໃນທຸກແອັບ Windows.",
    .about_feature0 = L"⚡ ລາກເພື່ອແປ — ເລືອກຂໍ້ຄວາມໃນແອັບໃດກໍໄດ້ ຮູບສັນຍາລັກລອຍຈະແປທັນທີ.",
    .about_feature1 = L"🔊 TTS ແບບປະສາດ — ອອກສຽງຄົບທັງ 37 ພາສາຜ່ານຊຸດສຽງ Windows ທີ່ຕິດຕັ້ງ.",
    .about_feature2 = L"🔒 100% ໃນອຸປະກອນ ແລະ ເປັນສ່ວນຕົວ — ເຮັດວຽກຕອນກົດຄີດທາງລັດເທົ່ານັ້ນ; ບໍ່ແຕະກະດານຂັບ.",
    .about_etymology = L"ໃນປີ 2000 ກ່ອນຄຣິດສັກກະລາດ, ຜູ້ຂຽນແຫ່ງເມໂຊໂປເຕເມຍເອີ້ນຜູ້ທີ່ປ່ຽນພາສາເປັນຂົວເຊື່ອມໂລກວ່າ «Eme-bala».",
    .about_link_website = L"ເວັບໄຊ",
    .about_link_contact = L"ຕິດຕໍ່",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"ທີມ Team Sunplaza · ຊຽວລ Yeongdeungpo (ຫ້ອງ 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · ເວລາເຮັດວຽກ 10:00–19:00 KST",
    .about_contact_lead = L"ສະຖາປະນິກຫຼັກ: Yongtai Kim",
    .tooltip_copied = L"✓ ລອກເອົາແລ້ວ!",
    .tooltip_button_copy = L"📋 ລອກເອົາ",
    .tooltip_button_tts = L"🔊 ອ່ານດັງ",
    .menu_ui_language = L"ພາສາຜູ້ໃຊ້ງານ",
    .menu_ui_language_auto = L"ອັດຕະໂນມັດ (ພາສາລະບົບ)",
    .about_reset_button = L"ຣີເຊັດຄືນຄ່າເລີ່ມຕົ້ນຂອງລະບົບ",
    .about_reset_done = L"ກູ້ຄືນຄ່າເລີ່ມຕົ້ນແລ້ວ",
    .menu_typing_group = L"ການພິມດ້ວຍແປ້ນພິມ",
    .menu_tooltip_group = L"ຄຳແນະນຳເມື່ອລາກ",
    .app_name = L"ເອເມບາລາ ແຊັດ",
    .tooltip_no_tts_voice = L"ບໍ່ມີສຽງ Windows ຕິດຕັ້ງສຳລັບພາສານີ້. ຄລິກ 🔊 ອີກເທື່ອໜຶ່ງເພື່ອເປີດການຕັ້ງຄ່າການເວົ້າ.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"ການແຈ້ງເຕືອນດ້ານຄວາມເປັນສ່ວນຕົວ",
    .privacy_notice_body = L"ຫຼັກການດ້ານຄວາມເປັນສ່ວນຕົວຂອງ Emebala Chat ມີດັ່ງນີ້:\n"
    L"\n"
    L"• Emebala Chat ບໍ່ມີເຊີເວີຂອງຕົນເອງ.\n"
    L"• ເມື່ອໃຊ້ແບບຈຳລອງພາຍໃນເຄື່ອງ, ຂໍ້ຄວາມແປຈະບໍ່ອອກຈາກອຸປະກອນຂອງທ່ານ.\n"
    L"• ຖ້າທ່ານເລືອກ Google Translate ຫຼືສະຫຼັບໄປຄລາວອັດຕະໂນມັດ, ຂໍ້ຄວາມທີ່ເລືອກ ຫຼືພິມ ຈະຖືກສົ່ງໄປ Google ໂດຍກົງ ເພື່ອແປ, ບໍ່ຜ່ານ Emebala.\n"
    L"• ໄຟລ໌ບັນທຶກການວິນິດໄສຖືກປິດໄວ້ເປັນຄ່າເລີ່ມຕົ້ນ; ເປີດໄດ້ໃນການຕັ້ງຄ່າ (ເລືອກເຂົ້າຮ່ວມ).\n"
    L"• ຖ້າທ່ານບໍ່ຕ້ອງສົ່ງຂໍ້ຄວາມໄປຍັງຄລາວ (Google), ໃຫ້ເປີດເມນູໄອຄອນຢູ່ແຖບຮາບພຽງລະບົບ ແລ້ວເລືອກ “ເຄື່ອງຈັກແປ” ຈາກນັ້ນ “ເຄື່ອງຈັກພາຍໃນທ້ອງຖິ່ນ”. ຖ້າບໍ່ມີແບບຈຳລອງໃນເຄື່ອງ ແລະປິດການສະຫຼັບໄປຄລາວໄວ້, ການແປຈະບໍ່ເຮັດວຽກ ໂດຍບໍ່ມີການສົ່ງຂໍ້ມູນ.\n"
    L"\n"
    L"ລາຍລະອຽດເຕັມຢູ່ໃນໄຟລ໌ README ທີ່ສາມາດອ່ານຄືນໄດ້ທຸກເວລາ.\n",
    .cheatsheet_config_path = L"ໄຟລ໌ config: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"ຂໍ້ຄວາມຍາວເກີນໄປ: ແປສະເພາະສ່ວນຕົ້ນແລະສ່ວນທ້າຍເທົ່ານັ້ນ.",
    .tooltip_untranslated_above = L"ມີຂໍ້ຄວາມໃໝ່ທີ່ຍັງບໍ່ໄດ້ແປຢູ່ຂ້າງເທິງ. ເພື່ອແປ, ກະລຸນາວາງເຄີເຊີທີ່ທ້າຍບັນທັດນັ້ນ ແລ້ວກົດ Enter.",
    .repair_in_progress = L"ກຳລັງສ້ອມແປງອົງປະກອບເຄື່ອງຈັກທ້ອງຖິ່ນ…",
    .repair_failed_title = L"ການແປພາສາທ້ອງຖິ່ນບໍ່ສາມາດໃຊ້ໄດ້",
    .repair_failed_body = L"ບໍ່ພົບໄຟລ໌ເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນ, ຈຶ່ງຢຸດການແປພາສາຊົ່ວຄາວ. ເພື່ອຟື້ນຟູເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນ, ກະລຸນາຕິດຕັ້ງ Emebala Chat ອີກຄັ້ງ, ຫຼື ເພື່ອປ່ຽນໄປໃຊ້ການແປພາສາຄລາວ (Google), ກະລຸນາເລືອກ “Google ແປພາສາ” ຈາກເມນູ tray, “ເຄື່ອງຈັກແປພາສາ”.",
    .menu_engine_openai = L"ຖັດກັນກັບ OpenAI (ເຄື່ອງແມ່ຂ່າຍທີ່ຜູ້ໃຊ້ກຳນົດ)…",
    .openai_settings_title = L"ການຕັ້ງຄ່າເຄື່ອງຈັກທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI",
    .openai_settings_action = L"ການຕັ້ງຄ່າເຄື່ອງຈັກທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI…",
    .openai_base_url_label = L"Base URL",
    .openai_api_key_label = L"ລະຫັດ API",
    .openai_model_label = L"ໂມເດລ",
    .openai_fetch_models = L"ດຶງລາຍການໂມເດລ",
    .openai_fetch_failed = L"ບໍ່ສາມາດດຶງລາຍການໂມເດລໄດ້. ທ່ານສາມາດພິມຊື່ໂມເດລໂດຍກົງໄດ້.",
    .openai_http_warning_title = L"ການເຊື່ອມຕໍ່ທີ່ບໍ່ປອດໄພ (HTTP)",
    .openai_http_warning_body = L"Base URL ໃຊ້ HTTP (ບໍ່ໄດ້ເຂົ້າລະຫັດ). ລະຫັດ API ແລະ ຂໍ້ຄວາມຂອງທ່ານຈະຖືກສົ່ງແບບບໍ່ເຂົ້າລະຫັດ. ສືບຕໍ່?",
    .openai_saved = L"ບັນທຶກການຕັ້ງຄ່າທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI ແລ້ວ.",
    .openai_key_masked = L"ລະຫັດທີ່ບັນທຶກ: ",
    .openai_invalid_base_url = L"Base URL ບໍ່ຖືກຕ້ອງ. ຕົວຢ່າງ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"ໂມເດວທີ່ຜູ້ໃຊ້ກຳນົດ (.gguf)",
    .menu_browse_gguf_file = L"ລົງທະບຽນໂມເດວ .gguf ອື່ນ…",
    .user_gguf_quality_title = L"ແຈ້ງການກ່ຽວກັບຄຸນນະພາບການແປ",
    .user_gguf_quality_body = L"ໂມເດລທີ່ເລືອກບໍ່ແມ່ນ Hy-MT2. ເວີຊັນປັດຈຸບັນໃຊ້ prompt ສຳລັບສະເພາະ Hy-MT2 ເທົ່ານັ້ນ, ດັ່ງນັ້ນຄຸນນະພາບການແປດ້ວຍໂມເດລນີ້ຈຶ່ງບໍ່ໄດ້ຮັບການຮັບປະກັນ. ສືບຕໍ່?",
    .user_gguf_registered_title = L"ລົງທະບຽນໂມເດລແລ້ວ",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"ໂມເດລທີ່ເລືອກໄດ້ຖືກລົງທະບຽນກັບເຄື່ອງຈັກທ້ອງຖິ່ນແລ້ວ.\n"
    L"\n"
    L"ໂມເດລນີ້ຖືກໃຊ້ສຳລັບການແປເມື່ອທ່ານເລືອກ \"ການເລືອກເຄື່ອງຈັກແປ > ທາງເລືອກຂອງຜູ້ໃຊ້ (.gguf)\".\n"
    L"\n"
    L"ເມື່ອໃຊ້ໄດ້: ພາຍໃນ 1 ນາທີຫຼັງຈາກລົງທະບຽນ (ຫຼັງຈາກເຄື່ອງຈັກຢຸດເພາະບໍ່ໄດ້ໃຊ້ງານ)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"ໂມເດວນີ້ແມ່ນມີຢູ່ໃນ Emebala Chat ແລ້ວ. ບໍ່ຈຳເປັນຕ້ອງລົງທະບຽນ. ທ່ານສາມາດເລືອກເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນທີ່ມີຢູ່ໂດຍກົງໄດ້.",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(ຍັງບໍ່ໄດ້ລົງທະບຽນ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"ຈັດການໂມເດລ…",
    .gguf_manager_title = L"ຜູ້ຈັດການໂມເດລຜູ້ໃຊ້",
    .gguf_manager_empty = L"ບໍ່ມີໂມເດລຜູ້ໃຊ້ທີ່ລົງທະບຽນ.",
    .gguf_manager_rename = L"ປ່ຽນຊື່…",
    .gguf_manager_delete = L"ລົບ…",
    .gguf_manager_close = L"ປິດ",
    .gguf_manager_delete_confirm_title = L"ລົບການລົງທະບຽນໂມເດລ",
    .gguf_manager_delete_confirm_body = L"ການລົງທະບຽນຂອງໂມເດລທີ່ເລືອກ ຈະຖືກລົບ.\n"
    L"\n"
    L"ໄຟລ໌ໂມເດລ (.gguf) ຍັງຄົງຢູ່ໃນດິສກ໌ ແລະ ຈະບໍ່ຖືກລົບ. ຖ້າໂມເດລນີ້ຖືກນໍາໃຊ້ຢູ່, ການເລືອກເຄື່ອງຈັກຮຽກຈະກັບໄປທີ່ອັດຕະໂນມັດ (Auto).\n"
    L"\n"
    L"ສືບຕໍ່?",
    .gguf_manager_rename_title = L"ປ່ຽນຊື່ໂມເດລ",
    .gguf_manager_rename_body = L"ໃສ່ຊື່ໃໝ່ (ສູງສຸດ 64 ຕົວອັກສອນ, ບໍ່ມີຊ່ອງຫວ່າງ).",
    .gguf_manager_rename_invalid = L"ບໍ່ສາມາດໃຊ້ຊື່ນັ້ນໄດ້. ກະລຸນາໃສ່ຊື່ທີ່ບໍ່ວ່າງ, ຕ່າງຈາກທີ່ມີຢູ່, ບໍ່ມີຊ່ອງຫວ່າງ ຫຼື ຕົວແຍກເສັ້ນທາງ, ບໍ່ເກີນ 64 ຕົວອັກສອນ.",
    .gguf_manager_done = L"ບັນທຶກການປ່ຽນແປງແລ້ວ.",
    .gguf_manager_err_serialize = L"ບໍ່ສາມາດ serialize registry.json ໄດ້ (ຊື່ໄຟລ໌ຖືກປະຕິເສດ). ບໍ່ມີຫຍັງປ່ຽນແປງ.",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% ບໍ່ສາມາດໃຊ້ໄດ້; ບໍ່ພົບໂຟນເດີໂມເດລທີ່ແບ່ງປັນ.",
    .gguf_manager_err_write = L"ບໍ່ສາມາດຂຽນ registry.json ໄດ້.",
    .gguf_manager_err_write_partial = L"registry.json ບໍ່ໄດ້ຖືກຂຽນໃຫ້ສົມບູນ.",
    .gguf_manager_err_registry_damaged = L"registry.json ເສຍຫາຍ ຫຼື ມີ schema ທີ່ບໍ່ຮອງຮັບ. ມັນບໍ່ໄດ້ຖືກດັດແປງ. ສ້ອມແປງ ຫຼື ລຶບມັນອອກ ແລ້ວລອງໃໝ່.",
    .tooltip_translate_failed = L"ການແປລົ້ມເຫຼວ. ກະລຸນາລອງໃໝ່.",
    .repair_transient_body = L"ເຄື່ອງຈັກແປທ້ອງຖິ່ນບໍ່ສາມາດໃຊ້ໄດ້ຊົ່ວຄາວ (ອາດກຳລັງເລີ່ມຕົ້ນ). ກະລຸນາลໍຖ້າຊົວຄູ່ແລ້ວລອງໃໝ່.",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"ຕົກລົງ",
    .dialog_cancel = L"ຍົກເລີກ",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"ເພີ່ມຈາກໄຟລ໌…",
    .gguf_manager_add_hf = L"ເພີ່ມຈາກ Hugging Face…",
    .hf_add_title = L"ເພີ່ມໂມເດລ Hugging Face",
    .hf_url_label = L"URL ຂອງໂມເດລ",
    .hf_downloading = L"ກຳລັງດາວໂຫລດ…",
    .hf_failed = L"ການດາວໂຫລດລົ້ມເຫຼວ.",
    .hf_invalid_url = L"ໃສ່ URL ໂມເດລຈາກ huggingface.co (resolve, blob ຫຼື ໜ້າໂມເດລ)",
    .hf_done = L"ລົງທະບຽນແລ້ວ.",
    .openai_base_url_hint = L"ຕົວຢ່າງ: https://api.openai.com/v1",
    .openai_api_key_hint = L"ລະຫັດ API",
    // REQ-051: ປຸ່ມ [ລົບການຕັ້ງຄ່າ].
    .openai_delete_settings = L"ລົບການຕັ້ງຄ່າ",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"ກວດສອບການອັບເດດ",
    .update_idle_note = L"ກວດເບິ່ງວ່າທ່ານໃຊ້ Emebala Chat ລຸ້ນຫຼ້າສຸດບໍ່.",
    .update_checking = L"ກຳລັງກວດສອບການອັບເດດ…",
    .update_checking_hint = L"ເຮັດວຽກໃນພື້ນຫຼັງ; ທ່ານສາມາດປິດໜ້າຕ່າງນີ້ໄດ້.",
    .update_uptodate = L"ທ່ານໃຊ້ລຸ້ນຫຼ້າສຸດແລ້ວ",
    .update_dlg_title = L"ມີການອັບເດດໃຫມ່",
    .update_dlg_version = L"ລຸ້ນໃຫມ່",
    .update_dlg_version_cur = L"ທ່ານໃຊ້ {v}",
    .update_dlg_size = L"ຂະໜາດ",
    .update_dlg_size_approx = L"ປະມານ {s}",
    .update_dlg_privacy = L"ການກວດສອບ ແລະ ດາວໂຫຼດ ຈະເຊື່ອມຕໍ່ຫາ GitHub.",
    .update_dlg_download = L"ດາວໂຫຼດ",
    .update_dlg_later = L"ພາຍຫຼັງ",
    .update_dl_progress = L"ກຳລັງດາວໂຫຼດ…",
    .update_dl_cancel = L"ຍົກເລີກ",
    .update_dl_cancelled = L"ຍົກເລີກການດາວໂຫຼດແລ້ວ",
    .update_ready_title = L"ດາວໂຫຼດສຳເລັດ — ພ້ອມຕິດຕັ້ງ",
    .update_ready_warn = L"ແອັບຈະປິດເພື່ອຕິດຕັ້ງ.",
    .update_ready_install = L"ຕິດຕັ້ງດຽວນີ້",
    .update_ready_later = L"ພາຍຫຼັງ",
    .update_err_offline = L"ທ່ານອອບລາຍ. ກວດເບິ່ງການເຊື່ອມຕໍ່ ແລ້ວລອງໃໝ່.",
    .update_err_rate = L"ຄຳຂໍຫຼາຍເກີນໄປ. ກະລຸນາລອງໃໝ່ອີກຄັ້ງໃນອີກຄືນໜ້ອຍ.",
    .update_err_hash = L"ໄຟລ໌ທີ່ດາວໂຫຼດບໍ່ຜ່ານການຢັ້ງຢືນ.",
    .update_err_retry = L"ລອງໃໝ່ອີກຄັ້ງ",
    .update_snoozed_line = L"ທ່ານສາມາດຕິດຕັ້ງອັບເດດນີ້ພາຍຫຼັງໄດ້.",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"ດາວໂຫຼດໂມເດວ",
    .model_dl_consent_body = L"ໂມເດວແປພາສາໃນຕົວຍັງບໍ່ໄດ້ຕິດຕັ້ງ. ດາວໂຫຼດຕອນນີ້ບໍ? (ປະມານ {s})",
    .model_dl_privacy = L"ການດາວໂຫຼດຈະເຊື່ອມຕໍ່ຫາ huggingface.co.",
    .model_dl_download = L"ດາວໂຫຼດດຽວນີ້",
    .model_dl_verifying = L"ກຳລັງກວດສອບໄຟລ໌ທີ່ດາວໂຫຼດ…",
    .model_dl_placing = L"ກຳລັງຕິດຕັ້ງໂມເດວ…",
    .model_dl_done = L"ໂມເດວພ້ອມແລ້ວ. ເຄື່ອງຈັກທ້ອງຖິ່ນຈະໃຊ້ມັນຕັ້ງແຕ່ການແປຄັ້ງຕໍ່ໄປ.",
    .model_dl_failed = L"ດາວໂຫຼດບໍ່ສຳເລັດ. ກວດເບິ່ງການເຊື່ອມຕໍ່ ແລ້ວລອງໃໝ່.",
    .model_dl_disk_space = L"ບໍ່ມີພື້ນທີ່ວ່າງຢູ່ໃນດິສພໍ (ຕ້ອງການປະມານ {s}).",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"ການຕັ້ງຄ່າການໂຫຼດໂມເດວແປ…",
    .engine_load_settings_title = L"ການໂຫຼດໂມເດວແປ",
    .load_mode_on_use = L"ໂຫຼດເມື່ອນຳໃຊ້ (ແນະນຳ)",
    .load_mode_warmup = L"ໂຫຼດລ່ວງໜ້າ",
    .load_mode_on_use_desc = L"ປະຢັດໜ່ວຍຄວາມຈຳ. ການແປຄັ້ງທຳອິດອາດເລີ່ມຊ້າລົງໄປໜຶ່ງວິນາທີ.",
    .load_mode_warmup_desc = L"ການແປເລີ່ມທັນທີ. ໂມເດວຍັງຄົງຢູ່ໃນໜ່ວຍຄວາມຈຳ.",
    .gguf_policy_sticky = L"(ຄົງໃນໜ່ວຍຄວາມຈຳ)",
    .gguf_policy_evict = L"(ໂຫຼດເມື່ອນຳໃຊ້)",
    .gguf_manager_toggle_policy = L"ສະຫຼັບນະໂຍບາຍ",
    .gguf_duplicate_warning = L"ໄຟລ໌ຊ້ຳກັນ: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ທຳຄວາມສະອາດໄຟລ໌ຊ້ຳ",
    .gguf_manager_dedupe_done = L"ໄຟລ໌ຊ້ຳໄດ້ຖືກຍ້າຍໄປຖັງຂີ້ເຫຍື້ອແລ້ວ.",
    .gguf_manager_dedupe_failed = L"ບໍ່ສາມາດທຳຄວາມສະອາດໄຟລ໌ຊ້ຳໄດ້. ກວດເບິ່ງວ່າມີໂປຣແກຣມອື່ນໃຊ້ງານໄຟລ໌ນັ້ນຢູ່ບໍ່.",
    .menu_ui_theme = L"ທີມສະໜອງ",
    .theme_fluent = L"ຟລູເອນກຣາບ",
    .theme_editorial = L"ວໍມເອດິໂຕຣຽວ",
    .theme_monolith = L"ຣີໄຟນດມອນໂລລິດ",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"ສະແດງການແປຍ້ອນກັບແບບລອຍ (ກວດສອບ)",
    .back_translate_header = L"🔄 ກວດສອບການແປຍ້ອນກັບ",
    .back_translate_match_exact = L"✓ ຄວາມໝາຍກົງກັນ",
    .back_translate_match_check = L"⚡ ກວດສອບນ້ຳສຽງ",
    .back_translate_reference = L"↳ ຕົ້ນສະບັບ: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"ບໍ່ແມ່ນໄຟລ໌ໂມເດວ GGUF ທີ່ຖືກຕ້ອງ ຈຶ່ງບໍ່ໄດ້ລົງທະບຽນ.",
    .gguf_manager_active_suffix = L"(ກຳລັງໃຊ້)",
    .gguf_manager_activated = L"ໂມເດວນີ້ແມ່ນໂມເດວທີ່ກຳລັງໃຊ້ແລ້ວ.",
};
// 37. Burmese (my)
const LocalizedStrings kStringsBurmese = {
    .menu_status_active = L"အခြေအနေ: လုပ်ဆောင်နေ (F9: ခေတ္တရပ်)",
    .menu_status_paused = L"အခြေအနေ: ခေတ္တရပ်ထား (F9: ပြန်စ)",
    .menu_engine = L"ဘာသာပြန် အင်ဂျင်",
    .menu_engine_google = L"Google Translate (အခမဲ့ / မတပ်ဆင်ရ)",
    .menu_engine_local = L"အတွင်းသွင်းပြင်ပအင်ဂျင် (Hy-MT2-1.8B အင်တာနက်မဲ့)",
    .menu_source_lang = L"မူလဘာသာစကား (ထည့်သွင်းမှု)",
    .menu_target_lang = L"ပစ်မှတ်ဘာသာစကား (ထုတ်လွှင့်မှု)",
    .menu_swap_langs = L"မူလ ⇄ ပစ်မှတ် လှလှယ် (နှစ်ချက်နှိပ်)",
    .menu_auto_send = L"Enter နှိပ်လျှင် အလိုအလျောက်ပို့ရန်",
    .menu_sound_feedback = L"အသံ တုံ့ပြန်ချက် (တုန်များ)",
    .menu_show_badge = L"ပေါ်လောတံဆိပ် ပြရန်",
    .menu_start_with_windows = L"Windows နှင့်အတူ စတင်မည်",
    .menu_cheatsheet = L"ရှော့တ်ကတ် လမ်းညွှန်နှင့် အကူအညီ...",
    .menu_exit = L"အီမီဘာလာ ချက် မှ ထွက်မည်",
    .menu_about = L"အီမီဘာလာ ချက် အကြောင်း…",
    .cheatsheet_title = L"အီမီဘာလာ ချက် ရှော့တ်ကတ်နှင့် အသုံးပြုနည်း လမ်းညွှန်",
    .cheatsheet_body = L"အီမီဘာလာ ချက် ရှော့တ်ကတ်နှင့် အသုံးပြုနည်း လမ်းညွှန်:\n\n"
    L"  • F9 : ဖွင့် / ခေတ္တရပ် ပြောင်းလဲရန်\n"
    L"  • Ctrl + F9 : ပစ်မှတ်ဘာသာစကား ပြောင်းရန်\n"
    L"  • Ctrl + Shift + Enter : အလိုအလျောက်ပို့မှု ပြောင်းလဲရန်\n"
    L"  • Shift + Enter : ပြန်ဆိုပြီး ချက်ချင်းပို့ရန်\n\n"
    L"တံဆိပ်ပေါ်တွင် မောက်စ် ထိန်းချုပ်မှုများ:\n"
    L"  • ဘယ်ဘက်နှိပ် : ဖွင့် / ခေတ္တရပ်\n"
    L"  • နှစ်ချက်နှိပ် : မူလ ⇄ ပစ်မှတ် လှလှယ်\n"
    L"  • ညာဘက်နှိပ် : ဆက်တင်မီနူး ဖွင့်ရန်\n\n"
    L"ဘာသာပြန် မုဒ်များ:\n"
    L"  • အစားထိုးသာ (အလိုအလျောက်ပို့ ပိတ်): စာကြောင်းကို ပြန်ဆိုချက်ဖြင့် စိစစ်ရန် အစားထိုးသည်။\n"
    L"  • အလိုအလျောက်ပို့ (ပွင့်): အစားထိုးပြီး ချက်ချင်း Enter နှိပ်သည်။",
    .about_title = L"အီမီဘာလာ ချက် အကြောင်း",
    .badge_active = L"လုပ်ဆောင်နေ",
    .badge_translating = L"ပြန်ဆိုနေ...",
    .badge_paused = L"ခေတ္တရပ်",
    .tooltip_title = L"အီမီဘာလာ ချက်",
    .tooltip_copy_failed = L"ရွေးချယ်ထားသော စာသားကို ကူးယူ၍မရပါ။ ပစ်မှတ်အက်ပ်ကို စစ်ဆေးပြီး ပြန်ကြိုးစားပါ။",
    .tooltip_no_selection = L"ပြန်ဆိုရန် စာသား မရွေးချယ်ရသေးပါ။",
    .auto_detect = L"အလိုအလျောက် ဖော်ထုတ်",
    .app_already_running = L"အီမီဘာလာ ချက် သည် နောက်ခံတွင် စတင်လည်ပတ်နေပြီ ဖြစ်သည်။\nစနစ်အသိပေးချက် တရေးကို ကြည့်ပါ။",
    .app_com_failed = L"COM စတင်ခြင်း မအောင်မြင်ပါ။\nပေါ်လောတံဆိပ်နှင့် စာသားအသံဖတ်ခြင်း မရတော့သော်လည်း၊\nဘာသာပြန်ခြင်း၊ ရှော့တ်ကတ်များ၊ တရေးနှင့် အသံများ ဆက်လက်လည်ပတ်နေမည်။",
    .about_tagline = L"ကူးတိပ် ပွဲတိပ်များကို မလုပ်တော့ပါနဲ့။ သင့်မိခင်ဘာသာဖြင့် သဘာဝကျ ရိုက်နှိပ်ပါ — Windows အက်ပ်များအတွင်း သင့်ရိုက်နှိပ်မှုများကို ဘာသာပြန်က ချက်ချင်း အစားထိုးပေးမည်။",
    .about_feature0 = L"⚡ ဆွဲပြန်ဆို — မည်သည့်အက်ပ်တွင်မဆို စာသားရွေးချယ်လိုက်လျှင် ပေါ်လောသင်္ကေတက ချက်ချင်း ပြန်ဆိုပေးသည်။",
    .about_feature1 = L"🔊 Neural TTS — တပ်ဆင်ထားသော Windows အသံပက်ကေ့ဂျ်များဖြင့် ဘာသာစကား 37 မျိုးလုံးကို အသံထွက်ဖတ်သည်။",
    .about_feature2 = L"🔒 100% စက်ပေါ်တွင်သာ၊ လုံခြုံစွာ — ရှော့တ်ကတ် ဖိထားချိန်၌သာ လုပ်ဆောင်; ကလစ်ပွိုဒ်ကို မထိတွေ့ပါ။",
    .about_etymology = L"ခရစ်တော် မပေါ်မီ နှစ်ပေါင်း ၂၀၀၀ က မက်ဆိုပိုတေးမီးယား စာရေးဆရာများက ဘာသာစကားကို ကမ္ဘာ့တံတားအဖြစ် ပြောင်းလဲသူများကို «Eme-bala» ဟု ခေါ်ကြသည်။",
    .about_link_website = L"ဝက်ဘ်ဆိုက်",
    .about_link_contact = L"ဆက်သွယ်ရန်",
    .about_link_reddit = L"Reddit",
    .about_contact_org = L"Team Sunplaza · ဆီအူးလ် Yeongdeungpo (အခန်း 219, 65 Yeongjung-ro)",
    .about_contact_phone = L"+82 2 575 0414 · ရုံးချိန် 10:00–19:00 KST",
    .about_contact_lead = L"ဦးဆောင်ဗိသုကာ: Yongtai Kim",
    .tooltip_copied = L"✓ ကူးယူပြီး!",
    .tooltip_button_copy = L"📋 ကူးယူရန်",
    .tooltip_button_tts = L"🔊 အသံဖတ်",
    .menu_ui_language = L"အင်တာဖေ့စ် ဘာသာစကား",
    .menu_ui_language_auto = L"အလိုအလျောက် (စနစ်ဘာသာစကား)",
    .about_reset_button = L"စနစ်ပုံသေများသို့ ပြန်လည်သတ်မှတ်ရန်",
    .about_reset_done = L"ပုံသေများ ပြန်လည်ရရှိပါသည်",
    .menu_typing_group = L"ကီးဘုတ် ရိုက်နှိပ်ခြင်း",
    .menu_tooltip_group = L"ဆွဲယူမှု ToolTip",
    .app_name = L"အီမီဘာလာ ချက်",
    .tooltip_no_tts_voice = L"ဤဘာသာစကားအတွက် Windows အသံ တပ်ဆင်ထားခြင်း မရှိပါ။ စပီခ် ဆက်တင်များ ဖွင့်ရန် 🔊 ကို ထပ်မံနှိပ်ပါ။",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    .privacy_notice_title = L"ကိုယ်ရေးအချက်အလက် လုံခြုံရေးကြေညာချက်",
    .privacy_notice_body = L"Emebala Chat ၏ ကိုယ်ရေးလုံခြုံမှု မူဝါဒများမှာ-\n"
    L"\n"
    L"• Emebala Chat သည် ကိုယ်ပိုင်ဆာဗာ မလည်ပတ်ပါ။\n"
    L"• ဒေသတွင်းမော်ဒယ်သုံးပါက ဘာသာပြန်စာသားသည် သင့်စက်မှ မထွက်ပါ။\n"
    L"• Google Translate ကို ရွေးလိုက်ပါက သို့မဟုတ် တစ်ဆက်တည်း cloud သို့ ပြောင်းလိုက်ပါက ရွေးထား/ရိုက်ထားသော စာသားသည် ဘာသာပြန်ရန် Google သို့ တိုက်ရိုက်ပို့ပြီး Emebala မဖြတ်ပါ။\n"
    L"• ရောဂါရှာဖွေ log များသည် မူလပိတ်ထားသည်။ ဆက်တင်တွင် ပွင့်အပ် (opt-in)။\n"
    L"• အကယ်၍ သင်သည် စာသားကို cloud (Google) သို့ မပို့လိုပါက စနစ်တရေး သင်္ကေတမီနူးကို ဖွင့်၍ “ဘာသာပြန် အင်ဂျင်” တွင် “အတွင်းသွင်းပြင်ပအင်ဂျင်” ကို ရွေးချယ်ပါ။ ဒေသတွင်းမော်ဒယ် မတပ်ဆင်ထားပါကနှင့် cloud ပြောင်းလဲခြင်း ပိတ်ထားပါက ဘာသာပြန်သည် စာသားမပို့ဘဲ အလုပ်မလုပ်ပါ။\n"
    L"\n"
    L"အသေးစိတ်ကို README ဖိုင်တွင် ဖတ်နိုင်ပြီး အချိန်မရွေး ပြန်ဖတ်နိုင်သည်။\n",
    .cheatsheet_config_path = L"config ဖိုင်: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    .translate_truncated_notice = L"စာသား အလွန်ရှည်သဖြင့် အစနှင့် အဆုံးကိုသာ ဘာသာပြန်ဆိုပါသည်။",
    .tooltip_untranslated_above = L"အပေါ်တွင် ဘာသာပြန်မထားသော စာသားအသစ်ရှိသည်။ ဘာသာပြန်ရန် ကာဆာကို ထိုစာကြောင်းအဆုံးသို့ ထားပြီး Enter နှိပ်ပါ။",
    .repair_in_progress = L"ပရိုဂရမ်အင်ဂျင်အစိတ်အပိုင်းများကို ပြုပြင်နေသည်…",
    .repair_failed_title = L"ဒေသန္တရ ဘာသာပြန်ချက် မရနိုင်ပါ",
    .repair_failed_body = L"ဒေသန္တရ ဘာသာပြန်အင်ဂျင် ဖိုင်များ မတွေ့ပါသဖြင့် ဘာသာပြန်မှုကို ယာယီရပ်နားထားပါသည်။ ဒေသန္တရ အင်ဂျင်ကို ပြန်လည်ရရှိရန် Emebala Chat ကို ပြန်လည်ထည့်သွင်းပါ၊ သို့မဟုတ် ကလောင်(Google) ဘာသာပြန်သို့ ပြောင်းလဲရန်၊ tray မီနူး၏ “ဘာသာပြန်အင်ဂျင်” မှ “Google ဘာသာပြန်” ကို ရွေးပါ။",
    .menu_engine_openai = L"OpenAI နှင့်သဟဇာတ (အသုံးပြုသူသတ်မှတ်ချက်စက်များ)…",
    .openai_settings_title = L"OpenAI နှင့် ကိုက်ညီသော အင်ဂျင် ဆက်တင်များ",
    .openai_settings_action = L"OpenAI နှင့် ကိုက်ညီသော အင်ဂျင် ဆက်တင်များ…",
    .openai_base_url_label = L"ပင်မ URL",
    .openai_api_key_label = L"API ကီး",
    .openai_model_label = L"မော်ဒယ်",
    .openai_fetch_models = L"မော်ဒယ် စာရင်း ရယူပါ",
    .openai_fetch_failed = L"မော်ဒယ် စာရင်းကို ရယူ၍ မရပါ။ မော်ဒယ်အမည်ကို တိုက်ရိုက် ရိုက်ထည့်နိုင်ပါသည်။",
    .openai_http_warning_title = L"လုံခြုံမှုမရှိသော ချိတ်ဆက်မှု (HTTP)",
    .openai_http_warning_body = L"ပင်မ URL သည် HTTP (စာဝှက်မထားပါ) ကို အသုံးပြုပါသည်။ သင်၏ API ကီးနှင့် စာသားကို စာသားအဖြစ် ပေးပို့ပါမည်။ ဆက်လက်ပါသလား?",
    .openai_saved = L"OpenAI နှင့် ကိုက်ညီသော ဆက်တင်များကို သိမ်းဆည်းပြီး။",
    .openai_key_masked = L"သိမ်းဆည်းထားသော ကီး: ",
    .openai_invalid_base_url = L"ပင်မ URL မမှန်ကန်ပါ။ ဥပမာ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    .menu_engine_user_gguf = L"အသုံးပြုသူသတ်မှတ်ထားသော မော်ဒယ် (.gguf)",
    .menu_browse_gguf_file = L"အခြား .gguf မော်ဒယ်ကို မှတ်ပုံတင်ပါ…",
    .user_gguf_quality_title = L"ဘာသာပြန် အရည်အသွေး သတိပေးချက်",
    .user_gguf_quality_body = L"ရွေးချယ်ထားသော မော်ဒယ်သည် Hy-MT2 မဟုတ်ပါ။ လက်ရှိ ဗားရှင်းသည် Hy-MT2 အတွက်သာ prompt ကို အသုံးပြုပါသည်၊ ထို့ကြောင့် ဒီမော်ဒယ်ဖြင့် ဘာသာပြန် အရည်အသွေးကို အာမခံ၍ မရပါ။ ဆက်လက်ပါသလား?",
    .user_gguf_registered_title = L"မော်ဒယ်စာရင်း သွင်းပြီး",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
    .user_gguf_registered_body = L"ရွေးချယ်ထားသော မော်ဒယ်ကို ဒေသတွင်း အင်ဂျင်သို့ စာရင်းသွင်းပြီးပါပြီ။\n"
    L"\n"
    L"သင် \"ဘာသာပြန် အင်ဂျင် ရွေးချယ်မှု > အသုံးပြုသူ ရွေးချယ်မှု (.gguf)\" ကို ရွေးချယ်သောအခါ ဒီမော်ဒယ်ကို ဘာသာပြန်ရန် အသုံးပြုပါတယ်။\n"
    L"\n"
    L"ဘယ်အချိန် သက်ရောက်သလဲ: စာရင်းသွင်းပြီးနောက် အများဆုံး ခန့် 1 မိနစ် (အင်ဂျင် အလုပ်မလုပ်ဘဲ အဆုံးသတ်ပြီးနောက်)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    .user_gguf_bundled_duplicate_body = L"ဒီမော်ဒယ်က Emebala Chat ထဲမှာ အသင့်ပါပြီးသားဖြစ်ပါတယ်။ မှတ်ပုံတင်စရာမလိုပါဘူး။ ပါရှိပြီးသား ပြည်တွင်းဘာသာပြန်အင်ဂျင်ကို တိုက်ရိုက်ရွေးချယ်နိုင်ပါတယ်။",
    // REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    .menu_engine_user_gguf_empty = L"(မှတ်ပုံတင်မထားပါ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    .menu_manage_gguf_models = L"မော်ဒယ်များကို စီမံခန့်ခွဲပါ…",
    .gguf_manager_title = L"အသုံးပြုသူ မော်ဒယ် စီမံခန့်ခွဲခန်း",
    .gguf_manager_empty = L"စာရင်းသွင်းထားသော အသုံးပြုသူ မော်ဒယ် မရှိပါ။",
    .gguf_manager_rename = L"နာမည်ပြောင်းပါ…",
    .gguf_manager_delete = L"ဖျက်ပါ…",
    .gguf_manager_close = L"ပိတ်ပါ",
    .gguf_manager_delete_confirm_title = L"မော်ဒယ် စာရင်းသွင်းမှုကို ဖျက်ပါ",
    .gguf_manager_delete_confirm_body = L"ရွေးချယ်ထားသော မော်ဒယ်၏ စာရင်းသွင်းမှုကို ဖျက်လိုက်ပါမည်။\n"
    L"\n"
    L"မော်ဒယ် ဖိုင် (.gguf) ကို ဒစ်စ်ပေါ်တွင် ထားရှိပြီး မဖျက်ပါ။ ဒီ မော်ဒယ်ကို အသုံးပြုနေပါက ဘာသာပြန် အင်ဂျင်ရွေးချယ်မှုကို အလိုအလျောက် (Auto) သို့ ပြန်သွားပါမည်။\n"
    L"\n"
    L"ဆက်လက်ပါသလား?",
    .gguf_manager_rename_title = L"မော်ဒယ် နာမည်ပြောင်းပါ",
    .gguf_manager_rename_body = L"နာမည် အသစ်ထည့်ပါ (အများဆုံး 64 အက္ခရာ၊ ဘေးလ်မပါ)။",
    .gguf_manager_rename_invalid = L"အဲဒီနာမည် အသုံးမပြုနိုင်ပါ။ ဗလာမဟုတ်ပြီး ၆၄ အက္ခရာ အတွင်းနာမည် တစ်ခု ထည့်ပါ။",
    .gguf_manager_done = L"ပြောင်းလဲမှုများ သိမ်းဆည်းပြီး။",
    .gguf_manager_err_serialize = L"registry.json ကို serialize မလုပ်နိုင်ပါ (ဖိုင်နစ်မည်တစ်ခုကို ပယ်ချခံရ). ပြောင်းလဲမှုမရှိပါ။",
    .gguf_manager_err_no_localappdata = L"%LOCALAPPDATA% မရနိုင်ပါ။ မော်ဒယ် ဖိုင် မတွေ့ပါ။",
    .gguf_manager_err_write = L"registry.json ကို မရေးနိုင်ပါ။",
    .gguf_manager_err_write_partial = L"registry.json ကို အပြည့်အဝ မရေးနိုင်ပါ။",
    .gguf_manager_err_registry_damaged = L"registry.json ပျက်စီးနေပါသည် သို့မဟုတ် မပံ့ပိုးသော schema ရှိပါသည်။ ၎င်းကို မပြောင်းလဲပါ။ ပြုပြင်ပါ သို့မဟုတ် ဖျက်ပြီး ထပ်စဉ်းစားပါ။",
    .tooltip_translate_failed = L"ဘာသာပြန် မအောင်မြင်ပါ။ ထပ်စဉ်းစားပါ။",
    .repair_transient_body = L"ဒေသတွင်း ဘာသာပြန် အင်ဂျင်ကို ယာယီ အသုံးမပြုနိုင်ပါ (စတင်နေပါရန် ဖြစ်နိုင်ပါသည်)။ ခဏစောင့်ပြီး ထပ်စဉ်းစားပါ။",
    // REQ-050: dialog OK/Cancel push-button labels.
    .dialog_ok = L"အိုကေ",
    .dialog_cancel = L"ပယ်ဖျက်ပါ",
    // REQ-050: merged manager add-methods + Hugging Face download dialog.
    .gguf_manager_add_file = L"ဖိုင်မှ ထည့်ရန်…",
    .gguf_manager_add_hf = L"Hugging Face မှ ထည့်ရန်…",
    .hf_add_title = L"Hugging Face မော်ဒယ်ထည့်ရန်",
    .hf_url_label = L"မော်ဒယ် URL",
    .hf_downloading = L"ဒေါင်းလုဒ်လုပ်နေသည်…",
    .hf_failed = L"ဒေါင်းလုဒ်လုပ်ရန်မအောင်မြင်ပါ။",
    .hf_invalid_url = L"huggingface.co မှ မော်ဒယ် URL ထည့်ပါ (resolve, blob သို့မဟုတ် မော်ဒယ်စာမျက်နှာ)",
    .hf_done = L"စာရင်းသွင်းပြီးပါပြီ။",
    .openai_base_url_hint = L"ဥပမာ: https://api.openai.com/v1",
    .openai_api_key_hint = L"API ကီးပါ",
    // REQ-051: [ဆက်တင်ဖျက်ပါ] ခလုတ်။
    .openai_delete_settings = L"ဆက်တင်ဖျက်ပါ",
    // REQ-UC (260930_0004, update-checker design §12): update-checker strings.
    .update_check = L"အပ်ဒိတ်များ စစ်ဆေးရန်",
    .update_idle_note = L"သင်အသစ်ဆုံး Emebala Chat ကို သုံးဆဲဖြစ်မဖြစ် စစ်ဆေးပါ။",
    .update_checking = L"အပ်ဒိတ်များ စစ်ဆေးနေပါသည်…",
    .update_checking_hint = L"ဘက်ကွက်တွင် လုပ်ဆောင်နေပါသည်။ ဤဝင်းဒိုးကို ပိတ်နိုင်ပါသည်။",
    .update_uptodate = L"သင် အသစ်ဆုံးဗားရှင်းကို သုံးနေပါပြီ",
    .update_dlg_title = L"အပ်ဒိတ်အသစ် ရရှိနိုင်ပါပြီ",
    .update_dlg_version = L"ဗားရှင်အသစ်",
    .update_dlg_version_cur = L"သင် အသုံးပြုနေသည်မှာ {v}",
    .update_dlg_size = L"အရွယ်အစား",
    .update_dlg_size_approx = L"ခန့်မှန်း {s}",
    .update_dlg_privacy = L"စစ်ဆေးမှုနှင့် ဒေါင်းလုဒ်မှာ GitHub နှင့် ချိတ်ဆက်ပါသည်။",
    .update_dlg_download = L"ဒေါင်းလုဒ်လုပ်ရန်",
    .update_dlg_later = L"နောက်တွင်",
    .update_dl_progress = L"ဒေါင်းလုဒ်ဆွဲနေပါသည်…",
    .update_dl_cancel = L"ပယ်ဖျက်ရန်",
    .update_dl_cancelled = L"ဒေါင်းလုဒ်ကို ပယ်ဖျက်ခဲ့ပါပြီ။",
    .update_ready_title = L"ဒေါင်းလုဒ်ပြီးဆုံးပါပြီ — ထည့်သွင်းရန်အဆင်သင့်",
    .update_ready_warn = L"ထည့်သွင်းရန် အက်ပ်ကို ပိတ်ပါမည်။",
    .update_ready_install = L"ယခုပင် ထည့်သွင်းရန်",
    .update_ready_later = L"နောက်တွင်",
    .update_err_offline = L"သင်အင်တာနက်မချိတ်ဆက်နိုင်ပါ။ သင့်ချိတ်ဆက်မှုကို စစ်ဆေးပြီး ပြန်လည်ကြိုးစားပါ။",
    .update_err_rate = L"တောင်းဆိုမှုများ အလွန်များနေပါသည်။ ခဏကြာပြီး ပြန်လည်ကြိုးစားပါ။",
    .update_err_hash = L"ဒေါင်းလုဒ်လုပ်ထားသော ဖိုင်ကို အတည်မပြုနိုင်ပါ။",
    .update_err_retry = L"ပြန်လည်ကြိုးစားရန်",
    .update_snoozed_line = L"သင် အပ်ဒိတ်ကို နောက်မှ ထည့်သွင်းနိုင်ပါသည်။",
    // REQ-MD (260930_0004, update-checker design §12 계열): model download dialog.
    .model_dl_caption = L"မော်ဒယ်ဒေါင်းလုဒ်လုပ်ရန်",
    .model_dl_consent_body = L"တပ်ဆင်ထားသော ဘာသာပြန်မော်ဒယ် မတပ်ဆင်ရသေးပါ။ ယခုဒေါင်းလုဒ်လုပ်ပါသလား? (ခန့်မှန်း {s})",
    .model_dl_privacy = L"ဒေါင်းလုဒ်လုပ်ရန် huggingface.co နှင့် ချိတ်ဆက်ပါသည်။",
    .model_dl_download = L"ယခုဒေါင်းလုဒ်လုပ်ရန်",
    .model_dl_verifying = L"ဒေါင်းလုဒ်လုပ်ထားသည့်ဖိုင်ကို စစ်ဆေးနေပါသည်…",
    .model_dl_placing = L"မော်ဒယ်ကို တပ်ဆင်နေပါသည်…",
    .model_dl_done = L"မော်ဒယ်အဆင်သင့်ဖြစ်ပါပြီ။ ဒေသန္တရအင်ဂျင်က နောက် ဘာသာပြန်ချက်ကတည်းက အသုံးပြုပါမည်။",
    .model_dl_failed = L"ဒေါင်းလုဒ်လုပ်ခြင်း မအောင်မြင်ပါ။ သင့်ချိတ်ဆက်မှုကို စစ်ဆေးပြီး ပြန်လည်ကြိုးစားပါ။",
    .model_dl_disk_space = L"ဒစ်စ်ပေါ်တွင် လုံလောက်သော နေရာလွတ် မရှိပါ (ခန့်မှန်း {s} လိုအပ်ပါသည်)။",
    // REQ-060 (session 261009_0001): load-mode + manager policy/duplicate block.
    .engine_load_settings_menu_row = L"ဘာသာပြန် မော်ဒယ် ဖွင့်စဉ် ဆက်တင်များ…",
    .engine_load_settings_title = L"ဘာသာပြန် မော်ဒယ် ဖွင့်စဉ်",
    .load_mode_on_use = L"သုံးတဲ့အခါ ဖွင့်ပါ (အကြံပြုပါသည်)",
    .load_mode_warmup = L"အရင်ကတည်းက ဖွင့်ထားပါ",
    .load_mode_on_use_desc = L"မေမွရီ သက်သာစေပါတယ်။ ပထမ ဘာသာပြန်ချက်က စက္ကန့်ပိုင်းနောက်ကျနိုင်ပါတယ်။",
    .load_mode_warmup_desc = L"ဘာသာပြန်ချက် ချက်ချင်းစတင်ပါတယ်။ မော်ဒယ်က မေမွရီထဲမှာ ဆက်ရှိနေပါတယ်။",
    .gguf_policy_sticky = L"(ဆက်ရှိနေစေ)",
    .gguf_policy_evict = L"(သုံးတဲ့အခါ ဖွင့်ပါ)",
    .gguf_manager_toggle_policy = L"စည်းမျဉ်း ပြောင်းပါ",
    .gguf_duplicate_warning = L"ထပ်တူ ဖိုင်များ: {n} {s} × {c}",
    .gguf_manager_dedupe = L"ထပ်တူ ရှင်းလင်းရန်",
    .gguf_manager_dedupe_done = L"ထပ်တူ ဖိုင်များကို recycle bin သို့ ရွှေ့ပြီးပါပြီ။",
    .gguf_manager_dedupe_failed = L"ထပ်တူ ဖိုင်များ ရှင်းလင်း မရပါ။ တခြား ပရိုဂရမ် သုံးနေသလား စစ်ဆေးပါ။",
    .menu_ui_theme = L"UI ဒီဇိုင်း",
    .theme_fluent = L"ဖလူးအန့်ကရွတ်",
    .theme_editorial = L"ဝမ်အက်ဒီတိုရီရယ်",
    .theme_monolith = L"ရီဖိုင်န့်မိုနိုလစ်",
    // 261108_0002 (WS3, v0.10.6): floating back-translation card + tray toggle.
    .menu_back_translation = L"မျောနေသော ပြန်လည်ဘာသာပြန်ပြသမှု (စစ်ဆေးရန်)",
    .back_translate_header = L"🔄 ပြန်လည်ဘာသာပြန် စစ်ဆေးမှု",
    .back_translate_match_exact = L"✓ အဓိပ္ပာယ် ကိုက်ညီသည်",
    .back_translate_match_check = L"⚡ အဓိပ္ပာယ်ကွဲပြားမှု စစ်ဆေးရန်",
    .back_translate_reference = L"↳ မူရင်း: ",
    // 261111_0001 U6 (v0.10.7, R1 P1): registration gate rejection,
    // the active-binding list marker, and the activation notice.
    .gguf_invalid_model_body = L"မှန်ကန်တဲ့ GGUF မော်ဒယ်ဖိုင် မဟုတ်ဘူးဆိုတော့ မှတ်ပုံတင် မထားပါဘူး။",
    .gguf_manager_active_suffix = L"(အသုံးပြုနေတယ်)",
    .gguf_manager_activated = L"ဒီ မော်ဒယ်ကို အသုံးပြုနေတဲ့ မော်ဒယ်အဖြစ် သတ်မှတ်ပြီးပါပြီ။",
};
const LocalizedStrings& GetStrings(UiLocale loc) {
    // REQ-037/B-3: 38-case switch (design §2.1.2). The default branch keeps the
    // Auto/unknown -> English explicit-fallback contract (design §2-Q4.3): a
    // SELECTABLE locale never reaches here half-wired, because the authenticity
    // gate withholds non-authored tables from the selector and PlanUiLocaleChange.
    switch (loc) {
        case UiLocale::Korean:               return kStringsKorean;
        case UiLocale::English:              return kStringsEnglish;
        case UiLocale::Vietnamese:           return kStringsVietnamese;
        case UiLocale::ChineseSimplified:    return kStringsChineseSimp;
        case UiLocale::ChineseTraditional:   return kStringsChineseTrad;
        case UiLocale::Japanese:             return kStringsJapanese;
        case UiLocale::Spanish:              return kStringsSpanish;
        case UiLocale::French:               return kStringsFrench;
        case UiLocale::German:               return kStringsGerman;
        case UiLocale::Russian:              return kStringsRussian;
        case UiLocale::Thai:                 return kStringsThai;
        case UiLocale::Arabic:               return kStringsArabic;
        case UiLocale::Portuguese:           return kStringsPortuguese;
        case UiLocale::Italian:              return kStringsItalian;
        case UiLocale::Indonesian:           return kStringsIndonesian;
        case UiLocale::Malay:                return kStringsMalay;
        case UiLocale::Filipino:             return kStringsFilipino;
        case UiLocale::Khmer:                return kStringsKhmer;
        case UiLocale::Lao:                  return kStringsLao;
        case UiLocale::Hindi:                return kStringsHindi;
        case UiLocale::Bengali:              return kStringsBengali;
        case UiLocale::Turkish:              return kStringsTurkish;
        case UiLocale::Polish:               return kStringsPolish;
        case UiLocale::Dutch:                return kStringsDutch;
        case UiLocale::Ukrainian:            return kStringsUkrainian;
        case UiLocale::Persian:              return kStringsPersian;
        case UiLocale::Urdu:                 return kStringsUrdu;
        case UiLocale::Hebrew:               return kStringsHebrew;
        case UiLocale::Czech:                return kStringsCzech;
        case UiLocale::Hungarian:            return kStringsHungarian;
        case UiLocale::Swedish:              return kStringsSwedish;
        case UiLocale::Greek:                return kStringsGreek;
        case UiLocale::Romanian:             return kStringsRomanian;
        case UiLocale::Danish:               return kStringsDanish;
        case UiLocale::Finnish:              return kStringsFinnish;
        case UiLocale::Norwegian:            return kStringsNorwegian;
        case UiLocale::Burmese:              return kStringsBurmese;
        case UiLocale::Auto:
        default:
            return kStringsEnglish;
    }
}

} // namespace

// ---- REQ-037 (design §2.1.3): the single LocaleMapping table -------------
//
// 37 rows in UiLocale enum order (== kAllLanguages registry order, design
// §2.1.1). StringToLocale, LocaleToString, GetLocaleCode and BOTH phases of
// DetectSystemLocale are data-driven from here, so adding a future locale is
// a one-row change plus a translation table. The table is exposed via
// GetLocaleMappings() for the headless TestReq037LocaleMapping round-trips.
//
// The two Chinese rows carry prefix L"zh" for the completeness assertion but
// never participate in generic prefix matching: Chinese is resolved by the
// explicit script-subtag pre-check in LocaleFromBcp47Tag (design §2.1.3:
// script disambiguation is not prefix-matchable), and the zh rows are skipped
// in the loop below. The LANGID phase keeps the SUBLANG disambiguation as an
// explicit pre-check too (legacy zh-TW/HK/MO behavior preserved verbatim).
namespace {

// SDK 10.0.26100 winnt.h defines no LANG_BURMESE symbol; the primary LANGID
// for Burmese (my-MM) is 0x0055. Named here once to keep the table readable.
constexpr WORD kLangidBurmese = 0x0055;

const LocaleMapping kLocaleMappings[] = {
    // locale                             config   prefix   alt      full       primary LANGID
    { UiLocale::Korean,               "ko",   L"ko",  nullptr,  L"ko-KR",  LANG_KOREAN,      true },
    { UiLocale::English,              "en",   L"en",  nullptr,  L"en-US",  LANG_ENGLISH,     true },
    { UiLocale::Vietnamese,           "vi",   L"vi",  nullptr,  L"vi-VN",  LANG_VIETNAMESE,  true },
    { UiLocale::ChineseSimplified,    "zh-CN", L"zh", nullptr,  L"zh-CN",  LANG_CHINESE,     true },
    { UiLocale::ChineseTraditional,   "zh-TW", L"zh", nullptr,  L"zh-TW",  LANG_CHINESE,     true },
    { UiLocale::Japanese,             "ja",   L"ja",  nullptr,  L"ja-JP",  LANG_JAPANESE,    true },
    { UiLocale::Spanish,              "es",   L"es",  nullptr,  L"es-ES",  LANG_SPANISH,     true },
    { UiLocale::French,               "fr",   L"fr",  nullptr,  L"fr-FR",  LANG_FRENCH,      true },
    { UiLocale::German,               "de",   L"de",  nullptr,  L"de-DE",  LANG_GERMAN,      true },
    { UiLocale::Russian,              "ru",   L"ru",  nullptr,  L"ru-RU",  LANG_RUSSIAN,     true },
    { UiLocale::Thai,                 "th",   L"th",  nullptr,  L"th-TH",  LANG_THAI,        true },
    { UiLocale::Arabic,               "ar",   L"ar",  nullptr,  L"ar-SA",  LANG_ARABIC,      true },
    { UiLocale::Portuguese,           "pt",   L"pt",  nullptr,  L"pt-PT",  LANG_PORTUGUESE,  true },
    { UiLocale::Italian,              "it",   L"it",  nullptr,  L"it-IT",  LANG_ITALIAN,     true },
    { UiLocale::Indonesian,           "id",   L"id",  nullptr,  L"id-ID",  LANG_INDONESIAN,  true },
    { UiLocale::Malay,                "ms",   L"ms",  nullptr,  L"ms-MY",  LANG_MALAY,       true },
    { UiLocale::Filipino,             "fil",  L"fil", nullptr,  L"fil-PH", LANG_FILIPINO,    true },
    { UiLocale::Khmer,                "km",   L"km",  nullptr,  L"km-KH",  LANG_KHMER,       true },
    { UiLocale::Lao,                  "lo",   L"lo",  nullptr,  L"lo-LA",  LANG_LAO,         true },
    { UiLocale::Hindi,                "hi",   L"hi",  nullptr,  L"hi-IN",  LANG_HINDI,       true },
    { UiLocale::Bengali,              "bn",   L"bn",  nullptr,  L"bn-BD",  LANG_BENGALI,     true },
    { UiLocale::Turkish,              "tr",   L"tr",  nullptr,  L"tr-TR",  LANG_TURKISH,     true },
    { UiLocale::Polish,               "pl",   L"pl",  nullptr,  L"pl-PL",  LANG_POLISH,      true },
    { UiLocale::Dutch,                "nl",   L"nl",  nullptr,  L"nl-NL",  LANG_DUTCH,       true },
    { UiLocale::Ukrainian,            "uk",   L"uk",  nullptr,  L"uk-UA",  LANG_UKRAINIAN,   true },
    { UiLocale::Persian,              "fa",   L"fa",  nullptr,  L"fa-IR",  LANG_PERSIAN,     true },
    { UiLocale::Urdu,                 "ur",   L"ur",  nullptr,  L"ur-PK",  LANG_URDU,        true },
    { UiLocale::Hebrew,               "he",   L"he",  nullptr,  L"he-IL",  LANG_HEBREW,      true },
    { UiLocale::Czech,                "cs",   L"cs",  nullptr,  L"cs-CZ",  LANG_CZECH,       true },
    { UiLocale::Hungarian,            "hu",   L"hu",  nullptr,  L"hu-HU",  LANG_HUNGARIAN,   true },
    { UiLocale::Swedish,              "sv",   L"sv",  nullptr,  L"sv-SE",  LANG_SWEDISH,     true },
    { UiLocale::Greek,                "el",   L"el",  nullptr,  L"el-GR",  LANG_GREEK,       true },
    { UiLocale::Romanian,             "ro",   L"ro",  nullptr,  L"ro-RO",  LANG_ROMANIAN,    true },
    { UiLocale::Danish,               "da",   L"da",  nullptr,  L"da-DK",  LANG_DANISH,      true },
    { UiLocale::Finnish,              "fi",   L"fi",  nullptr,  L"fi-FI",  LANG_FINNISH,     true },
    // Norwegian: Windows reports nb-NO (Bokmål); the legacy "no" tag is kept
    // as the alternate prefix (design §2.1.3 edge case). Both resolve to the
    // single Norwegian locale.
    { UiLocale::Norwegian,            "no",   L"nb",  L"no",   L"nb-NO",   LANG_NORWEGIAN,   true },
    { UiLocale::Burmese,              "my",   L"my",  nullptr,  L"my-MM",  kLangidBurmese,   true },
};

} // namespace

const std::vector<LocaleMapping>& GetLocaleMappings() {
    static const std::vector<LocaleMapping> kMappings(kLocaleMappings,
                                                      kLocaleMappings + sizeof(kLocaleMappings) / sizeof(kLocaleMappings[0]));
    return kMappings;
}

namespace {
// ASCII, case-insensitive equality for config locale codes ("zh-CN" == "zh_cn"
// handled explicitly by StringToLocale's alias list).
bool IEqualsAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}
} // namespace

void I18n::Initialize(std::string_view config_ui_lang) {
    if (config_ui_lang == "auto" || config_ui_lang.empty()) {
        s_current_locale.store(DetectSystemLocale(), std::memory_order_relaxed);
    } else {
        s_current_locale.store(StringToLocale(config_ui_lang), std::memory_order_relaxed);
    }
}

void I18n::SetLocale(UiLocale locale) {
    s_current_locale.store(locale, std::memory_order_relaxed);
}

UiLocale I18n::GetCurrentLocale() {
    return s_current_locale.load(std::memory_order_relaxed);
}

std::string_view I18n::GetLocaleCode() {
    // REQ-037/B-3 (design §2.1.3): data-driven from LocaleMapping. The Auto
    // sentinel is never stored past Initialize() (it maps to DetectSystemLocale
    // at startup and at apply time), and its "en" fallback branch is kept
    // explicit - same contract as the legacy switch, now table-fed.
    const UiLocale loc = GetCurrentLocale();
    if (loc != UiLocale::Auto) {
        for (const LocaleMapping& m : kLocaleMappings) {
            if (m.locale == loc) return m.config_code;
        }
    }
    return "en";
}

std::wstring I18n::Get(StringId id) {
    const auto& s = GetStrings(GetCurrentLocale());
    switch (id) {
        case StringId::MenuStatusActive: return s.menu_status_active;
        case StringId::MenuStatusPaused: return s.menu_status_paused;
        case StringId::MenuEngine: return s.menu_engine;
        case StringId::MenuEngineGoogle: return s.menu_engine_google;
        case StringId::MenuEngineLocal: return s.menu_engine_local;
        case StringId::MenuSourceLang: return s.menu_source_lang;
        case StringId::MenuTargetLang: return s.menu_target_lang;
        case StringId::MenuSwapLangs: return s.menu_swap_langs;
        case StringId::MenuAutoSend: return s.menu_auto_send;
        case StringId::MenuSoundFeedback: return s.menu_sound_feedback;
        case StringId::MenuShowBadge: return s.menu_show_badge;
        case StringId::MenuStartWithWindows: return s.menu_start_with_windows;
        case StringId::MenuCheatSheet: return s.menu_cheatsheet;
        case StringId::MenuExit: return s.menu_exit;
        case StringId::MenuAbout: return s.menu_about;

        case StringId::CheatSheetTitle: return s.cheatsheet_title;
        case StringId::CheatSheetBody: return s.cheatsheet_body;
        case StringId::AboutTitle: return s.about_title;

        case StringId::BadgeActive: return s.badge_active;
        case StringId::BadgeTranslating: return s.badge_translating;
        case StringId::BadgePaused: return s.badge_paused;

        case StringId::TooltipTitle: return s.tooltip_title;
        case StringId::TooltipCopyFailed: return s.tooltip_copy_failed;
        case StringId::TooltipNoSelection: return s.tooltip_no_selection;
        case StringId::AutoDetect: return s.auto_detect;

        case StringId::AppAlreadyRunning: return s.app_already_running;
        case StringId::AppComFailed: return s.app_com_failed;
        case StringId::AboutTagline: return s.about_tagline;
        case StringId::AboutFeature0: return s.about_feature0;
        case StringId::AboutFeature1: return s.about_feature1;
        case StringId::AboutFeature2: return s.about_feature2;
        case StringId::AboutEtymology: return s.about_etymology;
        case StringId::AboutLinkWebsite: return s.about_link_website;
        case StringId::AboutLinkContact: return s.about_link_contact;
        case StringId::AboutLinkReddit: return s.about_link_reddit;
        case StringId::AboutContactOrg: return s.about_contact_org;
        case StringId::AboutContactPhone: return s.about_contact_phone;
        case StringId::AboutContactLead: return s.about_contact_lead;
        case StringId::TooltipCopied: return s.tooltip_copied;
        case StringId::TooltipButtonCopy: return s.tooltip_button_copy;
        case StringId::TooltipButtonTts: return s.tooltip_button_tts;
        case StringId::MenuUiLanguage: return s.menu_ui_language;
        case StringId::MenuUiLanguageAuto: return s.menu_ui_language_auto;
        case StringId::AboutResetButton: return s.about_reset_button;
        case StringId::AboutResetDone: return s.about_reset_done;
        case StringId::MenuTypingGroup: return s.menu_typing_group;
        case StringId::MenuTooltipGroup: return s.menu_tooltip_group;
        // REQ-B-001 / REQ-C-004 (session 260909_0001 Batch-1)
        case StringId::AppName: return s.app_name;
        case StringId::TooltipNoTtsVoice: return s.tooltip_no_tts_voice;
        // REQ-206/208 (session 260911_0002 T2, P3 design §2.3/§2.4)
        case StringId::PrivacyNoticeTitle: return s.privacy_notice_title;
        case StringId::PrivacyNoticeBody: return s.privacy_notice_body;
        case StringId::CheatSheetConfigPath: return s.cheatsheet_config_path;
        case StringId::TranslateTruncatedNotice: return s.translate_truncated_notice;
        case StringId::TooltipUntranslatedAbove: return s.tooltip_untranslated_above;
        // REQ-005 (M6 T6, design §6.4): engine-host repair guidance.
        case StringId::RepairInProgress:  return s.repair_in_progress;
        case StringId::RepairFailedTitle: return s.repair_failed_title;
        case StringId::RepairFailedBody:  return s.repair_failed_body;

        // REQ-045 P4-3 (design §3b): OpenAI Compatible engine UI strings.
        case StringId::MenuEngineOpenAi:        return s.menu_engine_openai;
        case StringId::OpenAiSettingsTitle:     return s.openai_settings_title;
        case StringId::OpenAiSettingsAction:    return s.openai_settings_action;
        case StringId::OpenAiBaseUrlLabel:      return s.openai_base_url_label;
        case StringId::OpenAiApiKeyLabel:       return s.openai_api_key_label;
        case StringId::OpenAiModelLabel:        return s.openai_model_label;
        case StringId::OpenAiFetchModels:       return s.openai_fetch_models;
        case StringId::OpenAiFetchFailed:       return s.openai_fetch_failed;
        case StringId::OpenAiHttpWarningTitle:  return s.openai_http_warning_title;
        case StringId::OpenAiHttpWarningBody:   return s.openai_http_warning_body;
        case StringId::OpenAiSaved:             return s.openai_saved;
        case StringId::OpenAiKeyMasked:         return s.openai_key_masked;
        case StringId::OpenAiInvalidBaseUrl:    return s.openai_invalid_base_url;

        // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
        case StringId::MenuEngineUserGguf:      return s.menu_engine_user_gguf;
        case StringId::MenuBrowseGgufFile:      return s.menu_browse_gguf_file;
        case StringId::UserGgufQualityTitle:    return s.user_gguf_quality_title;
        case StringId::UserGgufQualityBody:     return s.user_gguf_quality_body;
        case StringId::UserGgufRegisteredTitle: return s.user_gguf_registered_title;
        case StringId::UserGgufRegisteredBody:  return s.user_gguf_registered_body;
        // REQ-047 D2 (design §B.3): bundled-origin reuse-path rejection notice.
        case StringId::UserGgufBundledDuplicateBody:
            return s.user_gguf_bundled_duplicate_body;

        // REQ-047 U1 (designer 164500 §5.3): "(미등록)" placeholder for the
        // user-model tray entry when no .gguf model has been registered yet.
        case StringId::MenuEngineUserGgufEmpty:
            return s.menu_engine_user_gguf_empty;

        // REQ-048 R2-D: registered user-.gguf model manager (rename/delete).
        case StringId::MenuManageGgufModels:          return s.menu_manage_gguf_models;
        case StringId::GgufManagerTitle:              return s.gguf_manager_title;
        case StringId::GgufManagerEmpty:              return s.gguf_manager_empty;
        case StringId::GgufManagerRename:             return s.gguf_manager_rename;
        case StringId::GgufManagerDelete:             return s.gguf_manager_delete;
        case StringId::GgufManagerClose:              return s.gguf_manager_close;
        case StringId::GgufManagerDeleteConfirmTitle: return s.gguf_manager_delete_confirm_title;
        case StringId::GgufManagerDeleteConfirmBody:  return s.gguf_manager_delete_confirm_body;
        case StringId::GgufManagerRenameTitle:        return s.gguf_manager_rename_title;
        case StringId::GgufManagerRenameBody:         return s.gguf_manager_rename_body;
        case StringId::GgufManagerRenameInvalid:      return s.gguf_manager_rename_invalid;
        case StringId::GgufManagerDone:               return s.gguf_manager_done;
        case StringId::GgufManagerErrSerialize: return s.gguf_manager_err_serialize;
        case StringId::GgufManagerErrNoLocalappdata: return s.gguf_manager_err_no_localappdata;
        case StringId::GgufManagerErrWrite: return s.gguf_manager_err_write;
        case StringId::GgufManagerErrWritePartial: return s.gguf_manager_err_write_partial;
        case StringId::GgufManagerErrRegistryDamaged: return s.gguf_manager_err_registry_damaged;
        case StringId::TooltipTranslateFailed:      return s.tooltip_translate_failed;
        case StringId::RepairTransientBody:         return s.repair_transient_body;
        // REQ-050: localized OK/Cancel push-button labels (OpenAI settings
        // dialog and future dialogs); real translations in all 37 locales.
        case StringId::DialogOk:                    return s.dialog_ok;
        case StringId::DialogCancel:                return s.dialog_cancel;

        // REQ-050: merged gguf-model manager — the two top-row add methods
        // plus the Hugging Face add-dialog strings (cancel reuses
        // DialogCancel; write/dir failures reuse the manager-error strings).
        case StringId::GgufManagerAddFile: return s.gguf_manager_add_file;
        case StringId::GgufManagerAddHf:   return s.gguf_manager_add_hf;
        case StringId::HfAddTitle:         return s.hf_add_title;
        case StringId::HfUrlLabel:         return s.hf_url_label;
        case StringId::HfDownloading:      return s.hf_downloading;
        case StringId::HfFailed:           return s.hf_failed;
        case StringId::HfInvalidUrl:       return s.hf_invalid_url;
        case StringId::HfDone:             return s.hf_done;

        // REQ-050: OpenAI settings cue banners — gray placeholder hints on
        // the base-URL / API-key single-line edits (EM_SETCUEBANNER).
        case StringId::OpenAiBaseUrlHint:  return s.openai_base_url_hint;
        case StringId::OpenAiApiKeyHint:   return s.openai_api_key_hint;

        // REQ-051 (Symptom D): the settings-dialog [삭제] push-button caption.
        case StringId::OpenAiDeleteSettings: return s.openai_delete_settings;

        // REQ-UC (260930_0004, update-checker design §12): About update zone.
        case StringId::UpdateCheck:         return s.update_check;
        case StringId::UpdateIdleNote:      return s.update_idle_note;
        case StringId::UpdateChecking:      return s.update_checking;
        case StringId::UpdateCheckingHint:  return s.update_checking_hint;
        case StringId::UpdateUptodate:      return s.update_uptodate;
        case StringId::UpdateDlgTitle:      return s.update_dlg_title;
        case StringId::UpdateDlgVersion:    return s.update_dlg_version;
        case StringId::UpdateDlgVersionCur: return s.update_dlg_version_cur;
        case StringId::UpdateDlgSize:       return s.update_dlg_size;
        case StringId::UpdateDlgSizeApprox: return s.update_dlg_size_approx;
        case StringId::UpdateDlgPrivacy:    return s.update_dlg_privacy;
        case StringId::UpdateDlgDownload:   return s.update_dlg_download;
        case StringId::UpdateDlgLater:      return s.update_dlg_later;
        case StringId::UpdateDlProgress:    return s.update_dl_progress;
        case StringId::UpdateDlCancel:      return s.update_dl_cancel;
        case StringId::UpdateDlCancelled:   return s.update_dl_cancelled;
        case StringId::UpdateReadyTitle:    return s.update_ready_title;
        case StringId::UpdateReadyWarn:     return s.update_ready_warn;
        case StringId::UpdateReadyInstall:  return s.update_ready_install;
        case StringId::UpdateReadyLater:    return s.update_ready_later;
        case StringId::UpdateErrOffline:    return s.update_err_offline;
        case StringId::UpdateErrRate:       return s.update_err_rate;
        case StringId::UpdateErrHash:       return s.update_err_hash;
        case StringId::UpdateErrRetry:      return s.update_err_retry;
        case StringId::UpdateSnoozedLine:   return s.update_snoozed_line;

        // REQ-MD (260930_0004 D9): on-demand bundled-model download dialog.
        case StringId::ModelDlCaption:      return s.model_dl_caption;
        case StringId::ModelDlConsentBody:  return s.model_dl_consent_body;
        case StringId::ModelDlPrivacy:      return s.model_dl_privacy;
        case StringId::ModelDlDownload:     return s.model_dl_download;
        case StringId::ModelDlVerifying:    return s.model_dl_verifying;
        case StringId::ModelDlPlacing:      return s.model_dl_placing;
        case StringId::ModelDlDone:         return s.model_dl_done;
        case StringId::ModelDlFailed:       return s.model_dl_failed;
        case StringId::ModelDlDiskSpace:    return s.model_dl_disk_space;

        // REQ-060 (session 261009_0001): translation-model load-mode settings
        // dialog + gguf-manager resident-policy / duplicate-file UI.
        case StringId::EngineLoadSettingsMenuRow: return s.engine_load_settings_menu_row;
        case StringId::EngineLoadSettingsTitle:   return s.engine_load_settings_title;
        case StringId::LoadModeOnUse:             return s.load_mode_on_use;
        case StringId::LoadModeWarmup:            return s.load_mode_warmup;
        case StringId::LoadModeOnUseDesc:         return s.load_mode_on_use_desc;
        case StringId::LoadModeWarmupDesc:        return s.load_mode_warmup_desc;
        case StringId::GgufPolicySticky:          return s.gguf_policy_sticky;
        case StringId::GgufPolicyEvict:           return s.gguf_policy_evict;
        case StringId::GgufManagerTogglePolicy:   return s.gguf_manager_toggle_policy;
        case StringId::GgufDuplicateWarning:      return s.gguf_duplicate_warning;
        case StringId::GgufManagerDedupe:         return s.gguf_manager_dedupe;
        case StringId::GgufManagerDedupeDone:     return s.gguf_manager_dedupe_done;
        case StringId::GgufManagerDedupeFailed:   return s.gguf_manager_dedupe_failed;

        case StringId::MenuUiTheme:               return s.menu_ui_theme;
        case StringId::ThemeFluent:               return s.theme_fluent;
        case StringId::ThemeEditorial:            return s.theme_editorial;
        case StringId::ThemeMonolith:             return s.theme_monolith;

        // 261108_0002 (WS3): floating back-translation verification card +
        // its tray toggle (same trailing-field wiring as the theme block).
        case StringId::MenuBackTranslation:       return s.menu_back_translation;
        case StringId::BackTranslateHeader:       return s.back_translate_header;
        case StringId::BackTranslateMatchExact:   return s.back_translate_match_exact;
        case StringId::BackTranslateMatchCheck:   return s.back_translate_match_check;
        // 261108_0002 (FIX-WS3B): the card's reference-line label.
        case StringId::BackTranslateReference:    return s.back_translate_reference;

        // 261111_0001 U6 (v0.10.7, R1 P1): user-model surface completeness
        // (same trailing-field wiring as the blocks above).
        case StringId::GgufInvalidModelBody:      return s.gguf_invalid_model_body;
        case StringId::GgufManagerActiveSuffix:   return s.gguf_manager_active_suffix;
        case StringId::GgufManagerActivated:      return s.gguf_manager_activated;

        case StringId::EnumCount:
        default: return L""; // empty by design - the completeness test skips it
    }
    // unreachable
}

I18nStrings I18n::GetThemeStrings() {
    const auto& s = GetStrings(GetCurrentLocale());
    return I18nStrings{
        s.menu_ui_theme,
        s.theme_fluent,
        s.theme_editorial,
        s.theme_monolith
    };
}

std::wstring I18n::GetLanguageDisplayName(std::string_view lang_code) {
    if (lang_code == "AUTO" || lang_code == "Auto Detect") {
        return Get(StringId::AutoDetect);
    }

    // Look up in 38-language registry
    for (const auto& lang : GetSupportedLanguages()) {
        if (lang.code == lang_code || lang.name_en == lang_code) {
            // Form display name: Native Name (English Name)
            // e.g. "한국어 (Korean)", "日本語 (Japanese)", "Tiếng Việt (Vietnamese)"
            if (GetCurrentLocale() == UiLocale::Korean) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else if (GetCurrentLocale() == UiLocale::Japanese) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else if (GetCurrentLocale() == UiLocale::ChineseSimplified || GetCurrentLocale() == UiLocale::ChineseTraditional) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else {
                return ToUtf16(lang.name_en) + L" (" + ToUtf16(lang.name_native) + L")";
            }
        }
    }

    return ToUtf16(lang_code);
}

UiLocale I18n::DetectSystemLocale() {
    // Phase 1 (design §2.1.3): BCP-47 tag -> table-driven prefix match.
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        const UiLocale byTag = LocaleFromBcp47Tag(localeName);
        if (byTag != UiLocale::Auto) { // Auto = the "no match" sentinel
            return byTag;
        }
    }

    // Phase 2: LANGID fallback, also table-driven. Chinese keeps the legacy
    // SUBLANG script disambiguation as an explicit pre-check (the two zh rows
    // share LANG_CHINESE, so first-match-on-primary would flatten it).
    const LANGID langId = ::GetUserDefaultUILanguage();
    const WORD primary = PRIMARYLANGID(langId);
    const WORD sub = SUBLANGID(langId);
    if (primary == LANG_CHINESE) {
        if (sub == SUBLANG_CHINESE_SIMPLIFIED || sub == SUBLANG_CHINESE_SINGAPORE) {
            return UiLocale::ChineseSimplified;
        }
        return UiLocale::ChineseTraditional;
    }
    for (const LocaleMapping& m : kLocaleMappings) {
        if (m.locale == UiLocale::ChineseSimplified || m.locale == UiLocale::ChineseTraditional) {
            continue; // handled by the SUBLANG pre-check above
        }
        if (primary == m.langid_primary) {
            return m.locale;
        }
    }
    // Explicit English fallback (design §2.1.5): an unsupported OS language
    // (e.g. sw-KE) gets the English UI - never a silent half-wired locale.
    return UiLocale::English;
}

UiLocale LocaleFromBcp47Tag(std::wstring_view tag) {
    // Phase-1 half of DetectSystemLocale, exposed for headless tests
    // (design §2.1.6). Returns UiLocale::Auto as the "no match" sentinel so
    // the caller can fall through to the LANGID phase.

    // Chinese: script-subtag disambiguation is NOT prefix-matchable, so the
    // legacy exact-match pre-checks are preserved verbatim (design §2.1.3).
    // A zh-* tag outside these lists deliberately falls through to the LANGID
    // phase, which disambiguates by SUBLANG (legacy behavior).
    auto tagEquals = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        }
        return true;
    };
    if (tag.size() >= 2 && tagEquals(tag.substr(0, 2), L"zh")) {
        if (tagEquals(tag, L"zh-CN") || tagEquals(tag, L"zh-Hans") || tagEquals(tag, L"zh-SG")) {
            return UiLocale::ChineseSimplified;
        }
        if (tagEquals(tag, L"zh-TW") || tagEquals(tag, L"zh-Hant") || tagEquals(tag, L"zh-HK") ||
            tagEquals(tag, L"zh-MO")) {
            return UiLocale::ChineseTraditional;
        }
        return UiLocale::Auto;
    }

    // Hyphen/underscore-boundary prefix match (design §5.1 R6): the tag must
    // equal the prefix or continue with '-'/'_' - never a raw starts_with, so
    // L"fi" cannot swallow fil-PH and L"no" cannot swallow "nob"/"nobil".
    // Takes the RAW pointer (not a string_view): wstring_view(nullptr) would
    // strlen a null pointer - the 35 rows without an alternate prefix pass
    // nullptr here by design and must short-circuit BEFORE view construction.
    auto prefixHit = [&](const wchar_t* prefixRaw) {
        if (prefixRaw == nullptr || prefixRaw[0] == L'\0') return false;
        const std::wstring_view prefix{prefixRaw};
        if (tag.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(tag[i]) != std::towlower(prefix[i])) return false;
        }
        return tag.size() == prefix.size() || tag[prefix.size()] == L'-' || tag[prefix.size()] == L'_';
    };
    for (const LocaleMapping& m : kLocaleMappings) {
        if (m.locale == UiLocale::ChineseSimplified || m.locale == UiLocale::ChineseTraditional) {
            continue; // zh rows resolve via the script pre-check only
        }
        if (prefixHit(m.bcp47_prefix) || prefixHit(m.bcp47_prefix_alt)) {
            return m.locale;
        }
    }
    return UiLocale::Auto;
}

UiLocale I18n::StringToLocale(std::string_view str) {
    // Table-driven (design §2.1.3) with the legacy aliases preserved verbatim:
    // "zh_cn"/"zh_tw" (underscore spellings) and bare "zh" -> Simplified.
    if (IEqualsAscii(str, "auto")) return UiLocale::Auto;
    if (IEqualsAscii(str, "zh_cn")) return UiLocale::ChineseSimplified;
    if (IEqualsAscii(str, "zh_tw")) return UiLocale::ChineseTraditional;
    if (IEqualsAscii(str, "zh")) return UiLocale::ChineseSimplified;
    for (const LocaleMapping& m : kLocaleMappings) {
        if (IEqualsAscii(str, m.config_code)) return m.locale;
    }
    // Unknown value -> explicit English fallback (design §2.1.5, R7-safe:
    // persisted config stores these same canonical codes, so every old value
    // still resolves; only truly unknown strings degrade to English).
    return UiLocale::English;
}

std::string I18n::LocaleToString(UiLocale locale) {
    if (locale == UiLocale::Auto) return "auto";
    for (const LocaleMapping& m : kLocaleMappings) {
        if (m.locale == locale) return m.config_code;
    }
    return "en"; // defensive: an enum value without a mapping row
}

const std::vector<UiLocaleEntry>& GetSupportedUiLocales() {
    // REQ-037 (design §2.1.4): built ONCE by joining LocaleMapping against the
    // kAllLanguages registry (name_native = endonym source of truth, so the
    // two tables cannot drift) through the authenticity gate (§2-Q4.2): rows
    // with authored=false stay out of the selector, and PlanUiLocaleChange
    // therefore refuses their codes - the proven half-wired guard, kept as the
    // gate knob even though B-3 authors all 37 rows.
    //
    // Display order per design §2-Q3: legacy muscle-memory front block
    // (KO, JA, zh-CN, zh-TW, VI, ES), then the remaining locales in registry
    // order, English last.
    static const std::vector<UiLocaleEntry> kEntries = [] {
        std::vector<UiLocaleEntry> out;
        out.reserve(sizeof(kLocaleMappings) / sizeof(kLocaleMappings[0]));
        // deque (not vector): its elements keep stable addresses when more are
        // appended, so the const wchar_t* we hand out never dangles.
        static std::deque<std::wstring> storage;
        const auto& registry = GetSupportedLanguages();

        auto append = [&](UiLocale locale) {
            for (const LocaleMapping& m : kLocaleMappings) {
                if (m.locale != locale) continue;
                if (!m.authored) return; // gate: withheld locale never listed
                for (const LanguageInfo& lang : registry) {
                    if (lang.code == "AUTO") continue;
                    // config_code ("ko", "zh-CN", "fil", ...) matches the
                    // registry code ("KO", "ZH-CN", "FIL", ...) ASCII
                    // case-insensitively (the IEqualsAscii idiom above).
                    if (!IEqualsAscii(m.config_code, lang.code)) continue;
                    storage.push_back(ToUtf16(lang.name_native));
                    out.push_back({ locale, storage.back().c_str() });
                    return;
                }
                return; // no registry row: fail closed (entry withheld)
            }
        };
        append(UiLocale::Korean);
        append(UiLocale::Japanese);
        append(UiLocale::ChineseSimplified);
        append(UiLocale::ChineseTraditional);
        append(UiLocale::Vietnamese);
        append(UiLocale::Spanish);
        for (const LocaleMapping& m : kLocaleMappings) {
            if (m.locale == UiLocale::Korean || m.locale == UiLocale::Japanese ||
                m.locale == UiLocale::ChineseSimplified || m.locale == UiLocale::ChineseTraditional ||
                m.locale == UiLocale::Vietnamese || m.locale == UiLocale::Spanish ||
                m.locale == UiLocale::English) {
                continue; // front block already appended; English closes the list
            }
            append(m.locale);
        }
        append(UiLocale::English);
        return out;
    }();
    return kEntries;
}

UiLocaleChangePlan PlanUiLocaleChangeWithSelector(const std::vector<UiLocaleEntry>& selector,
                                                  std::string_view current_persisted,
                                                  std::string_view requested) {
    UiLocaleChangePlan plan;
    const std::string req(requested);

    // Reject at the selector boundary: "auto" or an exact AUTHORED-locale code
    // (any ASCII case - normalized to the canonical table spelling). The
    // gate's ENFORCEMENT POINT is this loop: a mapping row withheld from the
    // selector (authored=false) has no entry here, so its code is refused and
    // can never be persisted half-wired. Split out as a pure overload so the
    // unit test can pin the refusal with a synthetic reduced selector even
    // while every real locale is authored (design §2-Q4.2).
    std::string canonical;
    if (IEqualsAscii(req, "auto")) {
        canonical = "auto";
    } else {
        for (const auto& entry : selector) {
            const std::string code = I18n::LocaleToString(entry.locale);
            if (IEqualsAscii(req, code)) {
                canonical = code;
                break;
            }
        }
    }
    if (canonical.empty()) {
        plan.valid = false; // unknown/unauthored locale: apply NOTHING (half-wired guard)
        return plan;
    }

    plan.applied = IEqualsAscii(canonical, "auto")
                       ? UiLocale::Auto
                       : I18n::StringToLocale(canonical);
    plan.valid = true;
    plan.changed = !IEqualsAscii(current_persisted, canonical);
    plan.persisted_value = std::move(canonical);
    // Plan §5.4 propagation order: tray -> badge -> tooltip -> About.
    plan.surfaces = { LocaleSurface::Tray, LocaleSurface::Badge,
                      LocaleSurface::Tooltip, LocaleSurface::About };
    return plan;
}

UiLocaleChangePlan PlanUiLocaleChange(std::string_view current_persisted,
                                      std::string_view requested) {
    return PlanUiLocaleChangeWithSelector(GetSupportedUiLocales(), current_persisted, requested);
}

bool I18n::IsStartWithWindowsEnabled() {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = sizeof(path);
        DWORD type = 0;
        LSTATUS status = ::RegQueryValueExW(hKey, kRunValueName, nullptr, &type, reinterpret_cast<LPBYTE>(path), &size);
        ::RegCloseKey(hKey);
        return (status == ERROR_SUCCESS && size > 0);
    }
    return false;
}

void I18n::SetStartWithWindows(bool enable) {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring quotedPath = L"\"" + std::wstring(exePath) + L"\"";
            ::RegSetValueExW(
                hKey,
                kRunValueName,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(quotedPath.c_str()),
                static_cast<DWORD>((quotedPath.size() + 1) * sizeof(wchar_t))
            );
        } else {
            ::RegDeleteValueW(hKey, kRunValueName);
        }
        ::RegCloseKey(hKey);
    }
}

} // namespace emebalachat
