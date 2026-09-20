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
    X(dialog_cancel)

struct LocalizedStrings {
#define EMEBALA_LSTR_FIELD(name) const wchar_t* name;
    EMEBALA_LSTR_FIELDS(EMEBALA_LSTR_FIELD)
#undef EMEBALA_LSTR_FIELD
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
// REQ-044: 57 == StringId::EnumCount. The design doc/Tech Gate cited 53,
// but that predates REQ-042's tooltip_untranslated_above (field 54); the
// enum's own running-total comment reads "57x37 with the REQ-005 repair
// trio". REQ-045 P4-3 (design §3b) appended 13 OpenAI fields (70), and
// REQ-045 P4-5 (item 3a-2) appended 6 user-.gguf fields, bringing the total
// to 76. REQ-047 D2 (design §B.3) appended the bundled-duplicate notice
// body (77). REQ-047 U1 (designer 164500 §5.3) appended the tray
// "(미등록)" empty-slot marker (78). REQ-048 R2-D appended the 12
// gguf-manager fields (97). REQ-050 appended the dialog OK/Cancel button
// labels (99). The Get() switch maps exactly these 99
// named fields.
static_assert(kLocalizedStringsFieldCount == 99,
    "LocalizedStrings field count changed - update all 37 locale tables");

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
};

// 2. Japanese (ja)
const LocalizedStrings kStringsJapanese = {
    L"状態: 有効 (F9: 一時停止)",
    L"状態: 一時停止 (F9: 再開)",
    L"翻訳エンジンの選択",
    L"Google 翻訳 (無料 / インストール不要)",
    L"内蔵ローカルエンジン (Hy-MT2-1.8B オフライン)",
    L"元の言語 (入力)",
    L"翻訳先言語 (ターゲット)",
    L"言語を入れ替える (ダブルクリック)",
    L"Enterで自動送信 (Auto-Send)",
    L"効果音 (Sound)",
    L"フローティングバッジを表示",
    L"Windows 起動時に自動実行",
    L"ショートカット案内とヘルプ...",
    L"エメバラチャット を終了",
    L"エメバラチャット について…",
    L"エメバラチャット ショートカットと使用案内",
    L"エメバラチャット ショートカットと使用案内:\n\n"
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
    L"エメバラチャット について",
    L"有効",
    L"翻訳中...",
    L"一時停止",
    L"エメバラチャット",
    L"選択したテキストをコピーできませんでした。対象アプリを確認して再試行してください。",
    L"翻訳するテキストが選択されていません。",
    L"自動検出",
    L"エメバラチャット はすでにバックグラウンドで実行中です。\nシステムトレイを確認してください。",
    L"COM の初期化に失敗しました。\nフローティングバッジと音声読み上げは利用できませんが、\n翻訳・ショートカット・トレイ・効果音は引き続き動作します。",
    L"コピー＆ペーストはもう不要。母語で自然に入力すると、あらゆる Windows アプリの中で打鍵がリアルタイムに翻訳へ置きわります。",
    L"⚡ ドラッグ翻訳 — 任意のアプリでテキストを選択すると、フローティングアイコンが即座に翻訳。",
    L"🔊 ニューラルTTS — Windows 音声パック導入時、37言語すべての発音に対応。",
    L"🔒 100% ローカル・プライバシー — ショートカットを押している間だけ作動し、クリップボードは使いません。",
    L"紀元前2000年、メソポタミアの書記たちは「エメ＝バラ」——言語で世界を結ぶ者——と呼びました。",
    L"ウェブサイト",
    L"お問い合わせ",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · 営業時間 10:00–19:00 KST",
    L"リードアーキテクト: Yongtai Kim",
    L"✓ コピーしました!",
    L"📋 コピー",
    L"🔊 読み上げ",
    L"表示言語",
    L"自動 (システム言語)",
    L"システム既定値にリセット",
    L"既定値に復元しました",
    L"キーボード入力",
    L"ドラッグ翻訳ツールチップ",
    L"エメバラチャット",
    L"この言語用の Windows 音声がインストールされていません。🔊 をもう一度クリックすると音声設定が開きます。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"プライバシーに関するご案内",
    L"Emebala Chat のプライバシー方針をご案内します。\n"
    L"\n"
    L"• Emebala Chat に独自サーバーはありません。\n"
    L"• ローカルモデル使用時、翻訳内容はデバイスの外に出ません。\n"
    L"• Google 翻訳エンジンを選択した場合、または自動切り替えで作動中は、選択・入力したテキストは Emebala を経由せず Google に直接送信され翻訳に使われます。\n"
    L"• 診断ログは既定で OFF で、設定で ON にするオプトイン機能です。\n"
    L"• テキストをクラウド(Google)に送信したくない場合は、トレイアイコンのメニューで「翻訳エンジンの選択」から「内蔵ローカルエンジン」を選択してください。ローカルモデル未インストールでクラウド切替が無効の場合、翻訳は送信されず動作しません。\n"
    L"\n"
    L"詳細は README ファイルをご覧ください。いつでも再読できます。\n",
    L"設定ファイル: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"テキストが長すぎるため、冒頭と末尾のみを翻訳しました。",
    L"上に翻訳されていない新しいテキストがあります。その行の末尾にカーソルを置いて Enter を押すと翻訳されます。",
    L"ローカルエンジンコンポーネントを修復しています…",
    L"ローカル翻訳を利用できません",
    L"ローカル翻訳エンジンのファイルが見つからないため、翻訳は一時的に停止しています。Emebala Chat を再インストールするとローカルエンジンを復元できます。または、クラウド（Google）翻訳に切り替えるには、トレイメニューの「翻訳エンジンの選択」から「Google 翻訳」を選んでください。",
    L"OpenAI 互換 (ユーザー指定サーバー)…",
        L"OpenAI 互換エンジン設定",
        L"OpenAI 互換エンジン設定…",
        L"ベース URL",
        L"API キー",
        L"モデル",
        L"モデル一覧を取得",
        L"モデル一覧を取得できませんでした。モデル名を直接入力できます。",
        L"安全でない接続 (HTTP)",
        L"ベース URL が HTTP (暗号化なし) です。API キーとテキストが平文で送信されます。続行しますか？",
        L"OpenAI 互換設定が保存されました。",
        L"保存済みキー: ",
        L"ベース URL が正しくありません。例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"ユーザー指定モデル (.gguf)",
    L"別の .gguf モデルを登録…",
        L"翻訳品質の案内",
        L"選択したモデルは Hy-MT2 ではありません。現在のバージョンは Hy-MT2 専用のプロンプトを使用するため、このモデルの翻訳品質は保証されません。続行しますか？",
        L"モデル登録完了",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"選択したモデルがローカルエンジンに登録されました。\n"
    L"\n"
    L"このモデルは「翻訳エンジン選択 > ユーザー選択 (.gguf)」で選択すると翻訳に使用されます。\n"
    L"\n"
    L"反映時刻: 登録後、最大約 1 分 (エンジンがアイドル終了してから)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"このモデルはEmebala Chatにすでに内蔵されています。登録は必要ありません。内蔵のローカル翻訳エンジンを直接選択してご利用ください。",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(未登録)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"モデルの管理…",
    L"ユーザーモデル管理",
    L"登録されたユーザーモデルはありません。",
    L"名前の変更…",
    L"削除…",
    L"閉じる",
    L"モデル登録の削除",
    L"選択したモデルの登録が削除されます。\n"
    L"\n"
    L"モデルファイル(.gguf)はディスクに保持され、削除されません。このモデルを使用中だった場合、翻訳エンジンの選択は自動(Auto)に戻ります。\n"
    L"\n"
    L"続行しますか？",
    L"モデル名の変更",
    L"新しい名前を入力してください (空白なし64文字以内)。",
    L"その名前は使用できません。空白やパス区切り文字を含まず、64文字以内で、既存のものと異なる空でない名前を入力してください。",
    L"変更が保存されました。",

    L"registry.json をシリアライズできません(ファイル名が拒否されました)。変更はありません。",
    L"%LOCALAPPDATA% を使用できず、共有モデルフォルダーが見つかりません。",
    L"registry.json を書き込めません。",
    L"registry.json の書き込みが完了しませんでした。",
    L"registry.json が破損しているか、サポートされていないスキーマです。変更は加えていません。修復するか削除してから再試行してください。",
    L"翻訳に失敗しました。もう一度お試しください。",
    L"ローカル翻訳エンジンを一時的に使用できません(起動中の可能性があります)。しばらくしてからもう一度お試しください。",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"決定",
    L"キャンセル",
};

// 3. Chinese Simplified (zh-CN)
const LocalizedStrings kStringsChineseSimp = {
    L"状态: 运行中 (F9: 暂停)",
    L"状态: 已暂停 (F9: 启用)",
    L"选择翻译引擎",
    L"Google 翻译 (免费 / 免安装 / 极速)",
    L"内置本地引擎 (Hy-MT2-1.8B 离线)",
    L"源语言 (输入语言)",
    L"目标语言 (翻译目标)",
    L"源语言 ⇄ 目标语言 互换 (双击)",
    L"回车自动发送 (Auto-Send)",
    L"声音提示反馈 (Tones)",
    L"显示桌面悬浮徽章",
    L"开机自动启动 (Start with Windows)",
    L"快捷键与使用说明 (帮助)...",
    L"退出 埃梅巴拉 翻译",
    L"关于 埃梅巴拉 翻译…",
    L"埃梅巴拉 翻译 快捷键与使用说明",
    L"埃梅巴拉 翻译 快捷键与使用说明:\n\n"
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
    L"关于 埃梅巴拉 翻译",
    L"运行中",
    L"翻译中...",
    L"已暂停",
    L"埃梅巴拉 翻译",
    L"无法复制所选文本。请检查目标应用后重试。",
    L"未选择要翻译的文本。",
    L"自动检测",
    L"埃梅巴拉 翻译 已在后台运行。\n请查看系统通知托盘。",
    L"COM 初始化失败。\n悬浮徽章和语音朗读将不可用，\n但翻译、快捷键、托盘和提示音仍可正常使用。",
    L"告别复制粘贴。用母语自然输入，译文会在任何 Windows 应用中实时替换你的键入。",
    L"⚡ 拖拽翻译 — 在任意应用中选中文本，悬浮图标即刻翻译。",
    L"🔊 神经 TTS — 接入 Windows 语音包后支持全部 37 种语言发音。",
    L"🔒 100% 本地运行且私密 — 仅在按住快捷键时生效，不触碰剪贴板。",
    L"公元前 2000 年，美索不达米亚的书吏称那些以语言连通世界的人为 Eme-bala。",
    L"官网",
    L"联系",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · 办公时间 10:00–19:00 KST",
    L"首席架构师：Yongtai Kim",
    L"✓ 已复制！",
    L"📋 复制",
    L"🔊 朗读",
    L"界面语言",
    L"自动（系统语言）",
    L"重置为系统默认值",
    L"已恢复默认值",
    L"键盘输入",
    L"划译工具提示",
    L"埃梅巴拉 翻译",
    L"此语言未安装 Windows 语音。再次点击 🔊 可打开语音设置。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"隐私保护说明",
    L"请阅读 Emebala Chat 的隐私处理原则。\n"
    L"\n"
    L"• Emebala Chat 不运营任何自有服务器。\n"
    L"• 使用本地模型时，翻译内容不会离开您的设备。\n"
    L"• 选择 Google 翻译引擎或自动切换到云端时，所选或输入的文本将直接发送给 Google 进行翻译，不经过 Emebala。\n"
    L"• 诊断日志默认关闭（OFF），需在设置中手动开启（选择性加入）。\n"
    L"• 如不希望将文本发送至云端 (Google)，请在系统托盘图标菜单的“选择翻译引擎”中选择“内置本地引擎”。若未安装本地模型且已关闭云端回退，翻译将不会被发送，也不会运行。\n"
    L"\n"
    L"完整说明请查看 README 文件，您可随时重新阅读。\n",
    L"配置文件: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"文本过长，仅翻译了开头和结尾部分。",
    L"上方有未翻译的新文本。将光标置于该行末尾并按 Enter 即可翻译。",
    L"正在修复本地引擎组件…",
    L"本地翻译不可用",
    L"找不到本地翻译引擎文件，翻译已暂时停止。重新安装 Emebala Chat 可恢复本地引擎，或者要切换到云（Google）翻译，请在托盘菜单的“选择翻译引擎”中选择“Google 翻译”。",
    L"OpenAI 兼容 (用户自定义服务器)…",
        L"OpenAI 兼容引擎设置",
        L"OpenAI 兼容引擎设置…",
        L"Base URL",
        L"API 密钥",
        L"模型",
        L"获取模型列表",
        L"无法获取模型列表。您可以直接输入模型名称。",
        L"不安全的连接 (HTTP)",
        L"Base URL 使用 HTTP(未加密)。您的 API 密钥和文本将以明文发送。是否继续？",
        L"OpenAI 兼容设置已保存。",
        L"已保存的密钥: ",
        L"Base URL 无效。示例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"用户自定义模型 (.gguf)",
    L"注册其他 .gguf 模型…",
        L"翻译质量提示",
        L"所选模型不是 Hy-MT2。当前版本使用 Hy-MT2 专用提示词,因此此模型的翻译质量不予保证。是否继续？",
        L"模型注册完成",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"所选模型已注册到本地引擎。\n"
    L"\n"
    L"在“翻译引擎选择 > 用户选择 (.gguf)”中选择此模型后,即可用于翻译。\n"
    L"\n"
    L"生效时间: 注册后最多约 1 分钟(引擎空闲退出后)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"该模型已内置在 Emebala Chat 中，无需注册。直接选择内置的本地翻译引擎即可使用。",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(未注册)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"管理模型…",
    L"用户模型管理",
    L"没有已注册的用户模型。",
    L"重命名…",
    L"删除…",
    L"关闭",
    L"删除模型注册",
    L"所选模型的注册将被删除。\n"
    L"\n"
    L"模型文件(.gguf)会保留在磁盘上,不会被删除。如果正在使用该模型,翻译引擎选择将自动返回“自动(Auto)”。\n"
    L"\n"
    L"是否继续?",
    L"重命名模型",
    L"输入新名称(不超过64个字符,不含空格)。",
    L"该名称无法使用。请输入一个非空、与现有名称不同、不含空格或路径分隔符且不超过64个字符的名称。",
    L"更改已保存。",

    L"无法序列化 registry.json(文件名被拒绝)。未做任何更改。",
    L"%LOCALAPPDATA% 不可用,找不到共享模型文件夹。",
    L"无法写入 registry.json。",
    L"registry.json 未能完整写入。",
    L"registry.json 已损坏或架构不受支持。未做修改。请修复或删除后重试。",
    L"翻译失败。请重试。",
    L"本地翻译引擎暂时不可用(可能正在启动)。请稍后重试。",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"确定",
    L"取消",
};

// 4. Chinese Traditional (zh-TW)
const LocalizedStrings kStringsChineseTrad = {
    L"狀態: 運行中 (F9: 暫停)",
    L"狀態: 已暫停 (F9: 啟用)",
    L"選擇翻譯引擎",
    L"Google 翻譯 (免費 / 免安裝 / 線上)",
    L"內建本機引擎 (Hy-MT2-1.8B 離線)",
    L"來源語言 (輸入語言)",
    L"目標語言 (翻譯目標)",
    L"來源語言 ⇄ 目標語言 對調 (雙擊)",
    L"Enter 自動發送 (Auto-Send)",
    L"音效回饋 (Sound)",
    L"顯示桌面懸浮徽章",
    L"開機時自動啟動",
    L"快捷鍵與使用說明 (說明)...",
    L"結束 埃梅巴拉 翻譯",
    L"關於 埃梅巴拉 翻譯…",
    L"埃梅巴拉 翻譯 快捷鍵與使用說明",
    L"埃梅巴拉 翻譯 快捷鍵與使用說明:\n\n"
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
    L"關於 埃梅巴拉 翻譯",
    L"運行中",
    L"翻譯中...",
    L"已暫停",
    L"埃梅巴拉 翻譯",
    L"無法複製所選文字。請檢查目標應用程式後重試。",
    L"尚未選取要翻譯的文字。",
    L"自動檢測",
    L"埃梅巴拉 翻譯 已在背景執行。\n請檢視系統通知列。",
    L"COM 初始化失敗。\n懸浮徽章與語音朗讀將不可用，\n但翻譯、快捷鍵、系統匣與提示音仍可正常使用。",
    L"告別複製貼上。用母語自然輸入，譯文會在任何 Windows 應用程式中即時取代你的鍵入。",
    L"⚡ 拖曳翻譯 — 在任何應用程式中選取文字，懸浮圖示立即翻譯。",
    L"🔊 神經 TTS — 接入 Windows 語音包後支援全部 37 種語言發音。",
    L"🔒 100% 本機執行且私密 — 僅在按住快捷鍵時生效，不觸碰剪貼簿。",
    L"西元前 2000 年，美索不達米亞的書吏稱那些以語言連結世界的人為 Eme-bala。",
    L"官網",
    L"聯絡",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · 辦公時間 10:00–19:00 KST",
    L"首席架構師：Yongtai Kim",
    L"✓ 已複製！",
    L"📋 複製",
    L"🔊 朗讀",
    L"介面語言",
    L"自動（系統語言）",
    L"重設為系統預設值",
    L"已還原預設值",
    L"鍵盤輸入",
    L"划譯工具提示",
    L"埃梅巴拉 翻譯",
    L"此語言未安裝 Windows 語音。再次點擊 🔊 可開啟語音設定。",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"隱私保護說明",
    L"請閱讀 Emebala Chat 的隱私處理原則。\n"
    L"\n"
    L"• Emebala Chat 不營運任何自有伺服器。\n"
    L"• 使用本機模型時，翻譯內容不會離開您的裝置。\n"
    L"• 選擇 Google 翻譯引擎或自動切換至雲端時，所選或輸入的文字將直接傳送至 Google 進行翻譯，不經過 Emebala。\n"
    L"• 診斷記錄預設關閉（OFF），需在設定中手動開啟（選擇性加入）。\n"
    L"• 若您不希望將文字傳送至雲端 (Google)，請在系統匣圖示選單的「選擇翻譯引擎」中選取「內建本機引擎」。若未安裝本地模型且已關閉雲端退回，翻譯將不會傳送任何資料，也不會執行。\n"
    L"\n"
    L"完整說明請查閱 README 檔案，您可隨時重新閱讀。\n",
    L"設定檔: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"文字過長，僅翻譯了開頭和結尾部分。",
    L"上方有未翻譯的新文字。將游標置於該行末尾並按 Enter 即可翻譯。",
    L"正在修復本機引擎元件…",
    L"本機翻譯無法使用",
    L"找不到本機翻譯引擎檔案，翻譯已暫時停止。重新安裝 Emebala Chat 可還原本機引擎，或者若要切換到雲端（Google）翻譯，請在系統匣選單的「選擇翻譯引擎」中選取「Google 翻譯」。",
    L"OpenAI 相容 (使用者自訂伺服器)…",
        L"OpenAI 相容引擎設定",
        L"OpenAI 相容引擎設定…",
        L"Base URL",
        L"API 金鑰",
        L"模型",
        L"取得模型列表",
        L"無法取得模型列表。您可以直接輸入模型名稱。",
        L"不安全的連線 (HTTP)",
        L"Base URL 使用 HTTP(未加密)。您的 API 金鑰和文字將以明文傳送。是否繼續？",
        L"OpenAI 相容設定已儲存。",
        L"已儲存的金鑰: ",
        L"Base URL 無效。範例: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"使用者自訂模型 (.gguf)",
    L"註冊其他 .gguf 模型…",
        L"翻譯品質提示",
        L"所選模型不是 Hy-MT2。目前版本使用 Hy-MT2 專用提示詞,因此此模型的翻譯品質不予保證。是否繼續？",
        L"模型註冊完成",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"所選模型已註冊到本機引擎。\n"
    L"\n"
    L"在「翻譯引擎選擇 > 使用者選擇 (.gguf)」中選擇此模型後,即可用於翻譯。\n"
    L"\n"
    L"生效時間: 註冊後最多約 1 分鐘(引擎閒置結束後)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"此模型已內建於 Emebala Chat，無需註冊。直接選擇內建的本機翻譯引擎即可使用。",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(未註冊)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"管理模型…",
    L"使用者模型管理",
    L"沒有已註冊的使用者模型。",
    L"重新命名…",
    L"刪除…",
    L"關閉",
    L"刪除模型註冊",
    L"所選模型的註冊將被刪除。\n"
    L"\n"
    L"模型檔案(.gguf)會保留在磁碟上,不會被刪除。如果正在使用該模型,翻譯引擎選擇將自動返回「自動(Auto)」。\n"
    L"\n"
    L"是否繼續?",
    L"重新命名模型",
    L"輸入新名稱(不超過64個字元,不含空格)。",
    L"該名稱無法使用。請輸入一個非空、與現有名稱不同、不含空格或路徑分隔字元且不超過64個字元的名稱。",
    L"變更已儲存。",

    L"無法序列化 registry.json(檔名被拒絕)。未做任何變更。",
    L"%LOCALAPPDATA% 不可用,找不到共用模型資料夾。",
    L"無法寫入 registry.json。",
    L"registry.json 未能完整寫入。",
    L"registry.json 已損毀或結構不受支援。未做修改。請修復或刪除後重試。",
    L"翻譯失敗。請重試。",
    L"本機翻譯引擎暫時不可用(可能正在啟動)。請稍後重試。",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"確定",
    L"取消",
};

// 5. Vietnamese (vi)
const LocalizedStrings kStringsVietnamese = {
    L"Trạng thái: Đang bật (F9: Tạm dừng)",
    L"Trạng thái: Tạm dừng (F9: Bật lại)",
    L"Chọn công cụ dịch",
    L"Google Dịch (Miễn phí / Trực tuyến)",
    L"Công cụ cục bộ tích hợp (Hy-MT2-1.8B ngoại tuyến)",
    L"Ngôn ngữ nguồn (Nhập)",
    L"Ngôn ngữ đích (Dịch sang)",
    L"Hoán đổi nguồn ⇄ đích (Nhấp đúp)",
    L"Tự động gửi khi nhấn Enter (Auto-Send)",
    L"Âm thanh thông báo (Sound)",
    L"Hiển thị huy hiệu nổi",
    L"Khởi động cùng Windows",
    L"Hướng dẫn phím tắt (Trợ giúp)...",
    L"Thoát Emebala Chat",
    L"Giới thiệu Emebala Chat…",
    L"Hướng dẫn sử dụng Emebala Chat",
    L"Phím tắt & Hướng dẫn sử dụng Emebala Chat:\n\n"
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
    L"Giới thiệu Emebala Chat",
    L"Đang bật",
    L"Đang dịch...",
    L"Tạm dừng",
    L"Emebala Chat",
    L"Không thể sao chép văn bản đã chọn. Hãy kiểm tra ứng dụng đích rồi thử lại.",
    L"Chưa chọn văn bản nào để dịch.",
    L"Tự động phát hiện",
    L"Emebala Chat đang chạy ngầm.\nHãy kiểm tra khay thông báo hệ thống.",
    L"Khởi tạo COM thất bại.\nHuy hiệu nổi và đọc văn bản sẽ không khả dụng,\nnhưng dịch, phím tắt, khay hệ thống và âm thanh vẫn hoạt động.",
    L"Không còn copy-paste. Gõ tự nhiên bằng tiếng mẹ đẻ — bản dịch thay thế ngay câu bạn gõ trong mọi ứng dụng Windows.",
    L"⚡ Kéo để dịch — chọn văn bản trong bất kỳ ứng dụng nào, biểu tượng nổi dịch ngay lập tức.",
    L"🔊 TTS thần kinh — phát âm tất cả 37 ngôn ngữ khi có gói giọng nói Windows.",
    L"🔒 100% trên máy & riêng tư — chỉ hoạt động khi giữ phím tắt; không đụng tới clipboard.",
    L"Năm 2000 TCN, các thư lại Lưỡng Hà gọi 'Eme-bala' — những người dùng ngôn ngữ bắc nhịp nối các thế giới.",
    L"Website",
    L"Liên hệ",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Giờ làm việc 10:00–19:00 KST",
    L"Kiến trúc sư chính: Yongtai Kim",
    L"✓ Đã sao chép!",
    L"📋 Sao chép",
    L"🔊 Đọc",
    L"Ngôn ngữ giao diện",
    L"Tự động (ngôn ngữ hệ thống)",
    L"Đặt lại về mặc định hệ thống",
    L"Đã khôi phục mặc định",
    L"Gõ phím",
    L"Dịch khi chọn văn bản",
    L"Emebala Chat",
    L"Chưa cài đặt giọng nói Windows cho ngôn ngữ này. Nhấp lại 🔊 để mở Cài đặt Nhận dạng và giọng nói.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Thông báo về quyền riêng tư",
    L"Đây là nguyên tắc xử lý thông tin cá nhân của Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat không vận hành bất kỳ máy chủ riêng nào.\n"
    L"• Khi dùng mô hình cục bộ, nội dung dịch không rời khỏi máy của bạn.\n"
    L"• Khi chọn Google Dịch hoặc được tự động chuyển sang đám mây, văn bản bạn chọn hoặc nhập được gửi trực tiếp tới Google để dịch, không qua Emebala.\n"
    L"• Nhật ký chẩn đoán mặc định TẮT; bạn phải bật trong cài đặt (chọn tham gia).\n"
    L"• Nếu không muốn gửi văn bản lên đám mây (Google), hãy mở menu biểu tượng ở khay hệ thống, chọn “Chọn công cụ dịch” rồi chọn “Công cụ cục bộ tích hợp”. Khi chưa cài mô hình cục bộ và tùy chọn chuyển lên đám mây đang tắt, bản dịch sẽ không chạy — không có gì được gửi đi.\n"
    L"\n"
    L"Xem toàn bộ nội dung trong tệp README. Bạn có thể đọc lại bất cứ lúc nào.\n",
    L"Tệp cấu hình: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Văn bản quá dài nên chỉ phần đầu và phần cuối được dịch.",
    L"Có văn bản mới chưa dịch ở trên. Đặt con trỏ ở cuối dòng đó và nhấn Enter để dịch.",
    L"Đang sửa chữa các thành phần cục bộ…",
    L"Không thể dùng bản dịch cục bộ",
    L"Không tìm thấy tệp của cục bộ nên bản dịch tạm dừng. Cài đặt lại Emebala Chat để khôi phục cục bộ, hoặc để chuyển sang bản dịch đám mây (Google), hãy chọn “Google Dịch” trong menu khay “Chọn công cụ dịch”.",
    L"Tương thích OpenAI (máy chủ do ngườ dùng chỉ định)…",
        L"Cài đặt công cụ tương thích OpenAI",
        L"Cài đặt công cụ tương thích OpenAI…",
        L"Base URL",
        L"API Key",
        L"Mô hình",
        L"Lấy danh sách mô hình",
        L"Không thể lấy danh sách mô hình. Bạn có thể nhập tên mô hình trực tiếp.",
        L"Kết nối không an toàn (HTTP)",
        L"Base URL sử dụng HTTP (không mã hóa). API key và văn bản của bạn sẽ được gửi dạng plaintext. Tiếp tục?",
        L"Đã lưu cài đặt tương thích OpenAI.",
        L"Khóa đã lưu: ",
        L"Base URL không hợp lệ. Ví dụ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Mô hình do ngườ dùng chỉ định (.gguf)",
    L"Đăng ký mô hình .gguf khác…",
        L"Thông báo chất lượng dịch",
        L"Mô hình đã chọn không phải Hy-MT2. Phiên bản hiện tại chỉ dùng prompt dành riêng cho Hy-MT2, vì vậy chất lượng dịch của mô hình này không được đảm bảo. Tiếp tục?",
        L"Đăng ký mô hình hoàn tất",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Mô hình đã chọn đã được đăng ký với công cụ cục bộ.\n"
    L"\n"
    L"Mô hình này được dùng để dịch khi bạn chọn \"Chọn công cụ dịch > Người dùng tự chọn (.gguf)\".\n"
    L"\n"
    L"Thời điểm áp dụng: tối đa khoảng 1 phút sau khi đăng ký (sau khi công cụ kết thúc nhàn rỗi)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Mô hình này đã được tích hợp sẵn trong Emebala Chat. Bạn không cần đăng ký. Hãy chọn trực tiếp công cụ dịch nội bộ để sử dụng.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(chưa đăng ký)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Quản lý mô hình…",
    L"Quản lý mô hình người dùng",
    L"Chưa có mô hình người dùng nào được đăng ký.",
    L"Đổi tên…",
    L"Xóa…",
    L"Đóng",
    L"Xóa đăng ký mô hình",
    L"Đăng ký của mô hình đã chọn sẽ bị xóa.\n"
    L"\n"
    L"Tệp mô hình (.gguf) vẫn được giữ trên ổ đĩa và không bị xóa. Nếu đang sử dụng mô hình này, lựa chọn công cụ dịch sẽ quay về Tự động (Auto).\n"
    L"\n"
    L"Tiếp tục?",
    L"Đổi tên mô hình",
    L"Nhập tên mới (tối đa 64 ký tự, không dấu cách).",
    L"Không thể sử dụng tên đó. Vui lòng nhập tên không trống, khác với tên hiện có, không chứa dấu cách hoặc dấu phân cách đường dẫn, trong 64 ký tự.",
    L"Đã lưu thay đổi.",

    L"Không thể tuần tự hóa registry.json (tên tệp bị từ chối). Không có gì thay đổi.",
    L"%LOCALAPPDATA% không khả dụng; không thể định vị thư mục mô hình dùng chung.",
    L"Không thể ghi registry.json.",
    L"Không thể ghi registry.json hoàn chỉnh.",
    L"registry.json bị hỏng hoặc có lược đồ không được hỗ trợ. Nó KHÔNG bị sửa đổi. Hãy khắc phục hoặc xóa rồi thử lại.",
    L"Dịch thất bại. Vui lòng thử lại.",
    L"Không thể sử dụng tạm thời công cụ dịch cục bộ (có thể đang khởi động). Vui lòng đợi một chút và thử lại.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Đồng ý",
    L"Hủy",
};

// 6. Spanish (es)
const LocalizedStrings kStringsSpanish = {
    L"Estado: Activo (F9: Pausar)",
    L"Estado: Pausado (F9: Activar)",
    L"Motor de traducción",
    L"Google Translate (Gratuito / En línea)",
    L"Motor local integrado (Hy-MT2-1.8B sin conexión)",
    L"Idioma de origen (Entrada)",
    L"Idioma de destino (Traducción)",
    L"Intercambiar idiomas (Doble clic)",
    L"Enviar automáticamente con Enter",
    L"Sonidos de notificación",
    L"Mostrar insignia flotante",
    L"Iniciar con Windows",
    L"Guía de atajos de teclado...",
    L"Salir de Emebala Chat",
    L"Acerca de Emebala Chat…",
    L"Guía de atajos de Emebala Chat",
    L"Guía de uso y atajos de Emebala Chat:\n\n"
    L"  • F9 : Activar / Pausar\n"
    L"  • Ctrl + F9 : Cambiar idioma de destino\n"
    L"  • Ctrl + Shift + Enter : Alternar envío automático\n"
    L"  • Shift + Enter : Traducir y enviar de inmediato\n\n"
    L"Acciones de ratón en la insignia:\n"
    L"  • Clic izquierdo : Activar / Pausar\n"
    L"  • Doble clic : Intercambiar origen ⇄ destino\n"
    L"  • Clic derecho : Abrir menú de opciones",
    L"Acerca de Emebala Chat",
    L"Activo",
    L"Traduciendo...",
    L"Pausado",
    L"Emebala Chat",
    L"No se pudo copiar el texto seleccionado. Revisa la aplicación de destino e inténtalo de nuevo.",
    L"No hay texto seleccionado para traducir.",
    L"Detectar automáticamente",
    L"Emebala Chat ya se está ejecutando en segundo plano.\nRevisa la bandeja de notificaciones.",
    L"Error al iniciar COM.\nLa insignia flotante y la voz no estarán disponibles,\npero la traducción, los atajos, la bandeja y los sonidos siguen funcionando.",
    L"Nunca más copiar y pegar. Escribe con naturalidad en tu idioma: la traducción reemplaza tu texto en tiempo real en cualquier aplicación de Windows.",
    L"⚡ Arrastrar y traducir — selecciona texto en cualquier app y el icono flotante lo traduce al instante.",
    L"🔊 TTS neuronal — pronuncia los 37 idiomas con los paquetes de voz de Windows instalados.",
    L"🔒 100% local y privado — solo activo mientras mantienes el atajo; sin tocar el portapapeles.",
    L"En el 2000 a. C., los escribas mesopotámicos llamaban «Eme-bala» a quienes convierten el lenguaje en un puente entre mundos.",
    L"Sitio web",
    L"Contacto",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Horario 10:00–19:00 KST",
    L"Arquitecto principal: Yongtai Kim",
    L"✓ ¡Copiado!",
    L"📋 Copiar",
    L"🔊 Voz",
    L"Idioma de la interfaz",
    L"Automático (idioma del sistema)",
    L"Restablecer valores predeterminados",
    L"Valores restaurados",
    L"Escritura con teclado",
    L"Tooltip de selección",
    L"Emebala Chat",
    L"No hay una voz de Windows instalada para este idioma. Haz clic de nuevo en 🔊 para abrir la configuración de Voz.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Aviso de privacidad",
    L"Estos son los principios de privacidad de Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat no opera ningún servidor propio.\n"
    L"• Con el modelo local, el texto traducido nunca sale de tu dispositivo.\n"
    L"• Si eliges Google Translate o se activa la nube automáticamente, el texto seleccionado o escrito se envía directamente a Google para su traducción, sin pasar por Emebala.\n"
    L"• Los registros de diagnóstico están DESACTIVADOS por defecto; debes activarlos en la configuración (opt-in).\n"
    L"• Si no quieres enviar texto a la nube (Google), abre el menú del icono de la bandeja, elige “Motor de traducción” y selecciona “Motor local integrado”. Sin modelo local instalado y con el modo cloud desactivado, la traducción no se ejecuta y no se envía nada.\n"
    L"\n"
    L"Puedes leer el detalle completo en el archivo README cuando quieras.\n",
    L"Archivo de configuración: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"El texto era demasiado largo: solo se tradujeron el principio y el final.",
    L"Hay texto nuevo sin traducir arriba. Coloca el cursor al final de esa línea y pulsa Enter para traducirlo.",
    L"Reparando los componentes del motor local…",
    L"Traducción local no disponible",
    L"No se encuentran los archivos del motor de traducción local, por lo que la traducción se detuvo temporalmente. Reinstala Emebala Chat para restaurar el motor local, o para cambiar a la traducción en la nube (Google), elige “Google Translate” en el menú de la bandeja, “Motor de traducción”.",
    L"Compatible con OpenAI (servidor personalizado)…",
        L"Ajustes del motor compatible con OpenAI",
        L"Ajustes del motor compatible con OpenAI…",
        L"URL base",
        L"Clave de API",
        L"Modelo",
        L"Obtener lista de modelos",
        L"No se pudo obtener la lista de modelos. Puede escribir el nombre del modelo directamente.",
        L"Conexión no segura (HTTP)",
        L"La URL base usa HTTP (sin cifrar). Su clave de API y el texto se enviarán en texto plano. ¿Continuar?",
        L"Ajustes compatibles con OpenAI guardados.",
        L"Clave guardada: ",
        L"La URL base no es válida. Ejemplo: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Modelo personalizado (.gguf)",
    L"Registrar otro modelo .gguf…",
        L"Aviso sobre la calidad de la traducción",
        L"El modelo seleccionado no es Hy-MT2. La versión actual usa el prompt exclusivo de Hy-MT2, por lo que la calidad de traducción con este modelo no está garantizada. ¿Continuar?",
        L"Modelo registrado",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"El modelo seleccionado se ha registrado en el motor local.\n"
    L"\n"
    L"Este modelo se usa para traducir cuando elige \"Selección de motor de traducción > Selección del usuario (.gguf)\".\n"
    L"\n"
    L"Cuándo surte efecto: hasta aproximadamente 1 minuto después del registro (tras la salida por inactividad del motor)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Este modelo ya está integrado en Emebala Chat. No es necesario registrarlo. Puedes seleccionar directamente el motor de traducción local integrado.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(sin registrar)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Administrar modelos…",
    L"Administrador de modelos de usuario",
    L"No hay modelos de usuario registrados.",
    L"Cambiar nombre…",
    L"Eliminar…",
    L"Cerrar",
    L"Eliminar registro del modelo",
    L"Se eliminará el registro del modelo seleccionado.\n"
    L"\n"
    L"El archivo del modelo (.gguf) se conserva en el disco y no se elimina. Si este modelo estaba en uso, la selección del motor de traducción volverá a Automático (Auto).\n"
    L"\n"
    L"¿Continuar?",
    L"Cambiar nombre del modelo",
    L"Introduzca un nombre nuevo (máx. 64 caracteres, sin espacios).",
    L"Ese nombre no se puede usar. Introduzca un nombre no vacío, distinto de los existentes, sin espacios ni separadores de ruta, de hasta 64 caracteres.",
    L"Cambios guardados.",

    L"No se pudo serializar registry.json (se rechazó un nombre de archivo). No se cambió nada.",
    L"%LOCALAPPDATA% no está disponible; no se puede ubicar el directorio de modelos compartido.",
    L"No se pudo escribir registry.json.",
    L"No se pudo escribir registry.json por completo.",
    L"registry.json está dañado o tiene un esquema no compatible. NO se modificó. Repárelo o elimínelo y vuelva a intentarlo.",
    L"La traducción falló. Inténtelo de nuevo.",
    L"El motor de traducción local no está disponible temporalmente (puede estar iniciándose). Espere un momento e inténtelo de nuevo.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Aceptar",
    L"Cancelar",
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
    L"Statut : actif (F9 : pause)",
    L"Statut : en pause (F9 : reprendre)",
    L"Moteur de traduction",
    L"Google Traduction (gratuit / sans installation)",
    L"Moteur local intégré (Hy-MT2-1.8B hors ligne)",
    L"Langue source (entrée)",
    L"Langue cible (sortie)",
    L"Inverser source ⇄ cible (double-clic)",
    L"Envoi auto avec Entrée",
    L"Retour sonore (tonalités)",
    L"Badge flottant visible",
    L"Démarrer avec Windows",
    L"Aide-mémoire des raccourcis et aide...",
    L"Quitter Emebala Chat",
    L"À propos d'Emebala Chat…",
    L"Emebala Chat — Raccourcis et guide d'utilisation",
    L"Raccourcis et guide d'Emebala Chat :\n\n"
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
    L"À propos d'Emebala Chat",
    L"Actif",
    L"Traduction...",
    L"En pause",
    L"Emebala Chat",
    L"Impossible de copier le texte sélectionné. Vérifiez l'application cible et réessayez.",
    L"Aucun texte sélectionné à traduire.",
    L"Détection auto",
    L"Emebala Chat est déjà exécuté en arrière-plan.\nVérifiez la zone de notification système.",
    L"L'initialisation COM a échoué.\nLe badge flottant et la synthèse vocale seront indisponibles,\nmais la traduction, les raccourcis, la barre d'état et les sons fonctionnent toujours.",
    L"Fini le copier-coller. Tapez naturellement dans votre langue maternelle — la traduction remplace votre frappe en temps réel dans n'importe quelle application Windows.",
    L"⚡ Glisser-traduire — sélectionnez un texte dans n'importe quelle app, l'icône flottante traduit instantanément.",
    L"🔊 TTS neuronal — prononce les 37 langues via les packs de voix Windows installés.",
    L"🔒 100 % local et privé — actif uniquement pendant que le raccourci est maintenu ; presse-papiers intact.",
    L"En 2000 av. J.-C., les scribes mésopotamiens appelaient « Eme-bala » ceux qui font de la langue un pont entre les mondes.",
    L"Site web",
    L"Contact",
    L"Reddit",
    L"Team Sunplaza · Séoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Heures ouvrables 10:00–19:00 KST",
    L"Architecte principal : Yongtai Kim",
    L"✓ Copié !",
    L"📋 Copier",
    L"🔊 Voix",
    L"Langue de l'interface",
    L"Auto (langue système)",
    L"Réinitialiser aux valeurs système",
    L"Valeurs restaurées",
    L"Saisie au clavier",
    L"Infobulle de glissement",
    L"Emebala Chat",
    L"Aucune voix Windows installée pour cette langue. Cliquez à nouveau sur 🔊 pour ouvrir les paramètres de la Voix.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Avertissement de confidentialité",
    L"Voici les principes de confidentialité d’Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat n’exploite aucun serveur propre.\n"
    L"• Avec le modèle local, le texte traduit ne quitte jamais votre appareil.\n"
    L"• Si vous choisissez Google Traduction ou si le basculement cloud est automatique, le texte sélectionné ou saisi est envoyé directement à Google pour la traduction, sans transiter par Emebala.\n"
    L"• Les journaux de diagnostic sont DÉSACTIVÉS par défaut ; activez-les dans la configuration (consentement explicite).\n"
    L"• Si vous ne souhaitez pas envoyer de texte vers le cloud (Google), ouvrez le menu de l'icône de la barre des tâches, choisissez « Moteur de traduction » puis « Moteur local intégré ». Sans modèle local installé et avec le repli cloud désactivé, la traduction ne s'exécute pas : rien n'est envoyé.\n"
    L"\n"
    L"Le détail complet est dans le fichier README, relisible à tout moment.\n",
    L"Fichier de configuration : %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Texte trop long : seul le début et la fin ont été traduits.",
    L"Il y a un nouveau texte non traduit au-dessus. Placez le curseur à la fin de cette ligne et appuyez sur Entrée pour le traduire.",
    L"Restauration des composants du moteur local…",
    L"Traduction locale indisponible",
    L"Les fichiers du moteur de traduction locale sont introuvables, la traduction est donc suspendue. Réinstallez Emebala Chat pour restaurer le moteur local, ou passez à la traduction cloud (Google) en choisissant « Google Traduction » dans le menu de la barre d’état, « Moteur de traduction ».",
    L"Compatible OpenAI (serveur personnalisé)…",
        L"Paramètres du moteur compatible OpenAI",
        L"Paramètres du moteur compatible OpenAI…",
        L"URL de base",
        L"Clé API",
        L"Modèle",
        L"Récupérer la liste des modèles",
        L"Impossible de récupérer la liste des modèles. Vous pouvez saisir un nom de modèle directement.",
        L"Connexion non sécurisée (HTTP)",
        L"L'URL de base utilise HTTP (non chiffré). Votre clé API et votre texte seront envoyés en clair. Continuer ?",
        L"Paramètres compatibles OpenAI enregistrés.",
        L"Clé enregistrée : ",
        L"L'URL de base n'est pas valide. Exemple : https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Modèle spécifié par l'utilisateur (.gguf)",
    L"Enregistrer un autre modèle .gguf…",
        L"Avis sur la qualité de la traduction",
        L"Le modèle sélectionné n'est pas Hy-MT2. La version actuelle utilise le prompt réservé à Hy-MT2 ; la qualité de traduction avec ce modèle n'est donc pas garantie. Continuer ?",
        L"Modèle enregistré",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Le modèle sélectionné a été enregistré auprès du moteur local.\n"
    L"\n"
    L"Ce modèle est utilisé pour la traduction lorsque vous choisissez « Sélection du moteur de traduction > Choix de l'utilisateur (.gguf) ».\n"
    L"\n"
    L"Prise d'effet : au maximum environ 1 minute après l'enregistrement (après la sortie du moteur par inactivité)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Ce modèle est déjà intégré à Emebala Chat. Aucune inscription n'est nécessaire. Vous pouvez sélectionner directement le moteur de traduction local intégré.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(non enregistré)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Gérer les modèles…",
    L"Gestionnaire de modèles utilisateur",
    L"Aucun modèle utilisateur enregistré.",
    L"Renommer…",
    L"Supprimer…",
    L"Fermer",
    L"Supprimer l'inscription du modèle",
    L"L'inscription du modèle sélectionné sera supprimée.\n"
    L"\n"
    L"Le fichier du modèle (.gguf) est conservé sur le disque et n'est pas supprimé. Si ce modèle était utilisé, le choix du moteur de traduction reviendra à Auto.\n"
    L"\n"
    L"Continuer ?",
    L"Renommer le modèle",
    L"Saisissez un nouveau nom (64 caractères max, sans espaces).",
    L"Ce nom ne peut pas être utilisé. Saisissez un nom non vide, différent des noms existants, sans espaces ni séparateurs de chemin, de 64 caractères maximum.",
    L"Modifications enregistrées.",

    L"Impossible de sérialiser registry.json (un nom de fichier a été refusé). Rien n'a été modifié.",
    L"%LOCALAPPDATA% est indisponible ; impossible de localiser le dossier de modèles partagé.",
    L"Impossible d'écrire registry.json.",
    L"registry.json n'a pas pu être écrit complètement.",
    L"registry.json est endommagé ou utilise un schéma non pris en charge. Il n'a PAS été modifié. Réparez-le ou supprimez-le, puis réessayez.",
    L"Échec de la traduction. Veuillez réessayer.",
    L"Le moteur de traduction local est temporairement indisponible (il est peut-être en cours de démarrage). Patientez un instant puis réessayez.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Valider",
    L"Annuler",
};

// 9. German (de)
const LocalizedStrings kStringsGerman = {
    L"Status: Aktiv (F9: Pause)",
    L"Status: Pausiert (F9: Fortsetzen)",
    L"Übersetzungsengine",
    L"Google Übersetzen (kostenlos / ohne Installation)",
    L"Eingebaute lokale Engine (Hy-MT2-1.8B offline)",
    L"Ausgangssprache (Eingabe)",
    L"Zielsprache (Ausgabe)",
    L"Ausgangssprache ⇄ Zielsprache tauschen (Doppelklick)",
    L"Automatisch mit Enter senden",
    L"Sound-Rückmeldung (Töne)",
    L"Schwebendes Badge sichtbar",
    L"Mit Windows starten",
    L"Tastenkürzel-Spickzettel und Hilfe...",
    L"Emebala Chat beenden",
    L"Über Emebala Chat…",
    L"Emebala Chat — Tastenkürzel und Anleitung",
    L"Tastenkürzel und Anleitung für Emebala Chat:\n\n"
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
    L"Über Emebala Chat",
    L"Aktiv",
    L"Übersetze...",
    L"Pausiert",
    L"Emebala Chat",
    L"Der markierte Text konnte nicht kopiert werden. Prüfen Sie die Ziel-App und versuchen Sie es erneut.",
    L"Es ist kein Text zum Übersetzen ausgewählt.",
    L"Automatisch erkennen",
    L"Emebala Chat läuft bereits im Hintergrund.\nPrüfen Sie das Benachrichtigungsfeld.",
    L"COM-Initialisierung fehlgeschlagen.\nSchwebendes Badge und Sprachausgabe sind nicht verfügbar,\naber Übersetzung, Tastenkürzel, Tray und Sounds funktionieren weiterhin.",
    L"Schluss mit Kopieren und Einfügen. Tippen Sie natürlich in Ihrer Muttersprache — die Übersetzung ersetzt Ihre Eingabe in Echtzeit in jeder Windows-Anwendung.",
    L"⚡ Ziehen-und-Übersetzen — Text in einer beliebigen App markieren, das schwebende Symbol übersetzt sofort.",
    L"🔊 Neuronales TTS — spricht alle 37 Sprachen über installierte Windows-Sprachpakete.",
    L"🔒 100 % lokal und privat — nur aktiv, während das Tastenkürzel gehalten wird; Zwischenablage unberührt.",
    L"Im Jahr 2000 v. Chr. nannten mesopotamische Schreiber „Eme-bala“ — jene, die Sprache zur Brücke zwischen Welten machen.",
    L"Webseite",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Bürozeiten 10:00–19:00 KST",
    L"Leitender Architekt: Yongtai Kim",
    L"✓ Kopiert!",
    L"📋 Kopieren",
    L"🔊 Stimme",
    L"Oberflächensprache",
    L"Auto (Systemsprache)",
    L"Auf Systemstandards zurücksetzen",
    L"Standards wiederhergestellt",
    L"Tastatureingabe",
    L"Drag-Tooltip",
    L"Emebala Chat",
    L"Für diese Sprache ist keine Windows-Stimme installiert. Klicken Sie erneut auf 🔊, um die Spracheinstellungen zu öffnen.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Datenschutzhinweis",
    L"Dies sind die Datenschutzgrundsätze von Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat betreibt keine eigenen Server.\n"
    L"• Bei Nutzung des lokalen Modells verlässt der übersetzte Text Ihr Gerät nicht.\n"
    L"• Wenn Sie Google Übersetzen wählen oder automatisch in den Cloud-Modus gewechselt wird, wird der markierte oder eingegebene Text direkt an Google zur Übersetzung gesendet – nicht über Emebala.\n"
    L"• Diagnostic-Protokolle sind standardmäßig AUS; aktivieren Sie sie in den Einstellungen (Opt-in).\n"
    L"• Wenn Sie keinen Text in die Cloud (Google) senden möchten, öffnen Sie das Menü des Taskleistensymbols, wählen Sie “Übersetzungsengine” und dann “Eingebaute lokale Engine”. Ohne installiertes lokales Modell und mit deaktiviertem Cloud-Fallback wird nicht übersetzt — es wird nichts gesendet.\n"
    L"\n"
    L"Details stehen in der README-Datei – jederzeit erneut lesbar.\n",
    L"Konfigurationsdatei: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Der Text war zu lang: Es wurden nur Anfang und Ende übersetzt.",
    L"Oben gibt es neuen unübersetzten Text. Setzen Sie den Cursor an das Ende dieser Zeile und drücken Sie die Eingabetaste, um ihn zu übersetzen.",
    L"Lokale Engine-Komponenten werden repariert…",
    L"Lokale Übersetzung nicht verfügbar",
    L"Die Dateien der lokalen Übersetzungsengine fehlen, daher ist die Übersetzung vorübergehend angehalten. Installieren Sie Emebala Chat erneut, um die lokale Engine wiederherzustellen, oder wechseln Sie im Tray-Menü unter „Übersetzungsengine“ zu „Google Übersetzer“.",
    L"OpenAI-kompatibel (benutzerdefinierter Server)…",
        L"OpenAI-kompatible Engine-Einstellungen",
        L"OpenAI-kompatible Engine-Einstellungen…",
        L"Basis-URL",
        L"API-Schlüssel",
        L"Modell",
        L"Modellliste abrufen",
        L"Die Modellliste konnte nicht abgerufen werden. Sie können einen Modellnamen direkt eingeben.",
        L"Unsichere Verbindung (HTTP)",
        L"Die Basis-URL verwendet HTTP (unverschlüsselt). Ihr API-Schlüssel und Ihr Text werden im Klartext gesendet. Fortfahren?",
        L"OpenAI-kompatible Einstellungen gespeichert.",
        L"Gespeicherter Schlüssel: ",
        L"Die Basis-URL ist ungültig. Beispiel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Benutzerdefiniertes Modell (.gguf)",
    L"Anderes .gguf-Modell registrieren…",
        L"Hinweis zur Übersetzungsqualität",
        L"Das ausgewählte Modell ist nicht Hy-MT2. Die aktuelle Version verwendet den Hy-MT2-spezifischen Prompt, daher ist die Übersetzungsqualität mit diesem Modell nicht garantiert. Fortfahren?",
        L"Modell registriert",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Das ausgewählte Modell wurde bei der lokalen Engine registriert.\n"
    L"\n"
    L"Dieses Modell wird für die Übersetzung verwendet, wenn Sie \"Übersetzungsengine wählen > Benutzerauswahl (.gguf)\" wählen.\n"
    L"\n"
    L"Wann wirksam: bis zu etwa 1 Minute nach der Registrierung (nach dem Leerlaufende der Engine)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Dieses Modell ist bereits in Emebala Chat integriert. Eine Registrierung ist nicht erforderlich. Sie können die integrierte lokale Übersetzungsengine direkt auswählen.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(nicht registriert)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Modelle verwalten…",
    L"Benutzermodell-Verwaltung",
    L"Keine Benutzermodelle registriert.",
    L"Umbenennen…",
    L"Löschen…",
    L"Schließen",
    L"Modellregistrierung löschen",
    L"Die Registrierung des ausgewählten Modells wird entfernt.\n"
    L"\n"
    L"Die Modelldatei (.gguf) bleibt auf dem Datenträger erhalten und wird nicht gelöscht. Wenn dieses Modell verwendet wurde, kehrt die Auswahl der Übersetzungsengine zu Automatisch (Auto) zurück.\n"
    L"\n"
    L"Fortfahren?",
    L"Modell umbenennen",
    L"Neuen Namen eingeben (max. 64 Zeichen, ohne Leerzeichen).",
    L"Dieser Name kann nicht verwendet werden. Bitte einen nicht leeren Namen eingeben, der von vorhandenen abweicht und keine Leerzeichen oder Pfadtrennzeichen enthält (max. 64 Zeichen).",
    L"Änderungen gespeichert.",

    L"registry.json konnte nicht serialisiert werden (ein Dateiname wurde abgelehnt). Es wurde nichts geändert.",
    L"%LOCALAPPDATA% ist nicht verfügbar; das freigegebene Modellverzeichnis kann nicht gefunden werden.",
    L"registry.json konnte nicht geschrieben werden.",
    L"registry.json konnte nicht vollständig geschrieben werden.",
    L"registry.json ist beschädigt oder hat ein nicht unterstütztes Schema. Es wurde NICHT geändert. Reparieren oder entfernen Sie es und versuchen Sie es erneut.",
    L"Übersetzung fehlgeschlagen. Bitte versuchen Sie es erneut.",
    L"Die lokale Übersetzungsengine ist vorübergehend nicht verfügbar (möglicherweise beim Start). Warten Sie einen Moment und versuchen Sie es erneut.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Bestätigen",
    L"Abbrechen",
};

// 10. Russian (ru)
const LocalizedStrings kStringsRussian = {
    L"Состояние: активно (F9: пауза)",
    L"Состояние: пауза (F9: возобновить)",
    L"Движок перевода",
    L"Google Переводчик (бесплатно / без установки)",
    L"Встроенный локальный движок (Hy-MT2-1.8B офлайн)",
    L"Исходный язык (ввод)",
    L"Язык перевода (вывод)",
    L"Поменять исходный ⇄ целевой (двойной клик)",
    L"Автоотправка по Enter",
    L"Звуковая обратная связь",
    L"Показывать плавающий бейдж",
    L"Запускать с Windows",
    L"Шпаргалка горячих клавиш и помощь...",
    L"Выйти из Эмебала Чат",
    L"Об Эмебала Чат…",
    L"Эмебала Чат: горячие клавиши и руководство",
    L"Горячие клавиши и руководство Эмебала Чат:\n\n"
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
    L"Об Эмебала Чат",
    L"Активно",
    L"Перевод...",
    L"Пауза",
    L"Эмебала Чат",
    L"Не удалось скопировать выделенный текст. Проверьте целевое приложение и повторите попытку.",
    L"Текст для перевода не выбран.",
    L"Определять автоматически",
    L"Эмебала Чат уже запущен в фоновом режиме.\nПроверьте область уведомлений.",
    L"Сбой инициализации COM.\nПлавающий бейдж и озвучивание текста будут недоступны,\nно перевод, горячие клавиши, трей и звуки продолжат работать.",
    L"Хватит копировать и вставлять. Печатайте естественно на родном языке — перевод заменяет ваш ввод в реальном времени в любом приложении Windows.",
    L"⚡ Перевод перетаскиванием — выделите текст в любом приложении, плавающий значок переведёт мгновенно.",
    L"🔊 Нейронное TTS — озвучивает все 37 языков через установленные голосовые пакеты Windows.",
    L"🔒 100 % на устройстве и приватно — активно, только пока удерживается горячая клавиша; буфер обмена не затрагивается.",
    L"В 2000 г. до н. э. месопотамские писцы называли «Eme-bala» тех, кто превращает язык в мост между мирами.",
    L"Сайт",
    L"Контакты",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Рабочие часы 10:00–19:00 KST",
    L"Ведущий архитектор: Yongtai Kim",
    L"✓ Скопировано!",
    L"📋 Копировать",
    L"🔊 Озвучка",
    L"Язык интерфейса",
    L"Авто (язык системы)",
    L"Сброс к системным значениям",
    L"Значения восстановлены",
    L"Ввод с клавиатуры",
    L"Всплывающая подсказка",
    L"Эмебала Чат",
    L"Для этого языка не установлен голос Windows. Нажмите 🔊 ещё раз, чтобы открыть параметры распознавания речи.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Уведомление о конфиденциальности",
    L"Принципы обработки данных в Emebala Chat:\n"
    L"\n"
    L"• Emebala Chat не использует собственные серверы.\n"
    L"• При локальной модели переводимый текст не покидает устройство.\n"
    L"• Если выбран Google Переводчик или включён автопереход в облако, выделенный или введённый текст отправляется напрямую в Google для перевода, минуя Emebala.\n"
    L"• Диагностические журналы по умолчанию ВЫКЛЮЧЕНЫ; их можно включить в настройках (по согласию).\n"
    L"• Если вы не хотите отправлять текст в облако (Google), откройте меню значка в системном трее, выберите «Движок перевода» и укажите «Встроенный локальный движок». Без установленной локальной модели и при отключённом облачном резерве перевод не выполняется — ничего не отправляется.\n"
    L"\n"
    L"Полные сведения — в файле README, доступном для чтения в любое время.\n",
    L"Файл конфигурации: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Текст слишком длинный: переведены только начало и конец.",
    L"Выше есть новый непереведенный текст. Поместите курсор в конец этой строки и нажмите Enter, чтобы перевести.",
    L"Восстановление компонентов локального движка…",
    L"Локальный перевод недоступен",
    L"Файлы локального движка перевода не найдены, поэтому перевод приостановлен. Переустановите Emebala Chat, чтобы восстановить локальный движок, или переключитесь на облачный (Google) перевод, выбрав «Google Переводчик» в меню области уведомлений, «Движок перевода».",
    L"Совместимо с OpenAI (пользовательский сервер)…",
        L"Настройки совместимого с OpenAI движка",
        L"Настройки совместимого с OpenAI движка…",
        L"Base URL",
        L"API-ключ",
        L"Модель",
        L"Получить список моделей",
        L"Не удалось получить список моделей. Вы можете ввести имя модели вручную.",
        L"Незащищённое соединение (HTTP)",
        L"Base URL использует HTTP (без шифрования). Ваш API-ключ и текст будут отправлены в открытом виде. Продолжить?",
        L"Совместимые с OpenAI настройки сохранены.",
        L"Сохранённый ключ: ",
        L"Недопустимый Base URL. Пример: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Модель, указанная пользователем (.gguf)",
    L"Зарегистрировать другую модель .gguf…",
        L"Уведомление о качестве перевода",
        L"Выбранная модель не Hy-MT2. Текущая версия использует промпт только для Hy-MT2, поэтому качество перевода этой моделью не гарантируется. Продолжить?",
        L"Модель зарегистрирована",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Выбранная модель зарегистрирована в локальном движке.\n"
    L"\n"
    L"Эта модель используется для перевода, когда вы выбираете «Выбор движка перевода > Выбор пользователя (.gguf)».\n"
    L"\n"
    L"Когда вступает в силу: не более примерно 1 минуты после регистрации (после выхода движка из простоя)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Эта модель уже встроена в Emebala Chat. Регистрация не требуется. Выберите встроенный локальный движок перевода напрямую.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(не зарегистрировано)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Управление моделями…",
    L"Управление моделями пользователя",
    L"Нет зарегистрированных пользовательских моделей.",
    L"Переименовать…",
    L"Удалить…",
    L"Закрыть",
    L"Удаление регистрации модели",
    L"Регистрация выбранной модели будет удалена.\n"
    L"\n"
    L"Файл модели (.gguf) остаётся на диске и не удаляется. Если эта модель использовалась, выбор движка перевода вернётся к Авто (Auto).\n"
    L"\n"
    L"Продолжить?",
    L"Переименование модели",
    L"Введите новое имя (до 64 символов, без пробелов).",
    L"Такое имя использовать нельзя. Введите непустое имя, отличающееся от существующих, без пробелов и разделителей пути, длиной до 64 символов.",
    L"Изменения сохранены.",

    L"Не удалось сериализовать registry.json (имя файла отклонено). Ничего не изменено.",
    L"%LOCALAPPDATA% недоступен; не удаётся найти общий каталог моделей.",
    L"Не удалось записать registry.json.",
    L"Не удалось полностью записать registry.json.",
    L"registry.json повреждён или имеет неподдерживаемую схему. Он НЕ был изменён. Исправьте или удалите его и повторите попытку.",
    L"Перевод не удался. Попробуйте ещё раз.",
    L"Локальный движок перевода временно недоступен (возможно, запускается). Подождите немного и повторите попытку.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Подтвердить",
    L"Отмена",
};

// 11. Portuguese (pt)
const LocalizedStrings kStringsPortuguese = {
    L"Estado: ativo (F9: pausar)",
    L"Estado: pausado (F9: retomar)",
    L"Mecanismo de tradução",
    L"Google Tradutor (grátis / sem instalação)",
    L"Motor local integrado (Hy-MT2-1.8B offline)",
    L"Idioma de origem (entrada)",
    L"Idioma de destino (saída)",
    L"Trocar origem ⇄ destino (clique duplo)",
    L"Envio automático com Enter",
    L"Feedback sonoro (tons)",
    L"Mostrar selo flutuante",
    L"Iniciar com o Windows",
    L"Resumo de atalhos e ajuda...",
    L"Sair do Emebala Chat",
    L"Sobre o Emebala Chat…",
    L"Emebala Chat — Atalhos e guia de uso",
    L"Atalhos e guia de uso do Emebala Chat:\n\n"
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
    L"Sobre o Emebala Chat",
    L"Ativo",
    L"Traduzindo...",
    L"Pausado",
    L"Emebala Chat",
    L"Não foi possível copiar o texto selecionado. Verifique o aplicativo de destino e tente novamente.",
    L"Nenhum texto selecionado para traduzir.",
    L"Detecção automática",
    L"O Emebala Chat já está em execução em segundo plano.\nVerifique a bandeja de notificações.",
    L"Falha na inicialização do COM.\nO selo flutuante e a leitura de voz ficarão indisponíveis,\nmas tradução, atalhos, bandeja e sons continuam funcionando.",
    L"Chega de copiar e colar. Digite naturalmente no seu idioma nativo — a tradução substitui sua digitação em tempo real em qualquer aplicativo do Windows.",
    L"⚡ Arrastar para traduzir — selecione texto em qualquer app e o ícone flutuante traduz na hora.",
    L"🔊 TTS neural — fala todos os 37 idiomas via pacotes de voz do Windows instalados.",
    L"🔒 100% no dispositivo e privado — ativo apenas enquanto o atalho é pressionado; área de transferência intacta.",
    L"Em 2000 a.C., escribas mesopotâmicos chamavam de «Eme-bala» aqueles que transformam a linguagem em ponte entre mundos.",
    L"Site",
    L"Contato",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Horário comercial 10:00–19:00 KST",
    L"Arquiteto principal: Yongtai Kim",
    L"✓ Copiado!",
    L"📋 Copiar",
    L"🔊 Voz",
    L"Idioma da interface",
    L"Automático (idioma do sistema)",
    L"Restaurar padrões do sistema",
    L"Padrões restaurados",
    L"Digitação no teclado",
    L"Dica de ferramenta",
    L"Emebala Chat",
    L"Não há voz do Windows instalada para este idioma. Clique novamente em 🔊 para abrir as configurações de Fala.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Aviso de privacidade",
    L"Estes são os princípios de privacidade do Emebala Chat.\n"
    L"\n"
    L"• O Emebala Chat não opera servidores próprios.\n"
    L"• Com o modelo local, o texto traduzido nunca sai do seu dispositivo.\n"
    L"• Se escolher o Google Tradutor ou houver mudança automática para a nuvem, o texto selecionado ou digitado é enviado diretamente ao Google para tradução, sem passar pelo Emebala.\n"
    L"• Os registros de diagnóstico estão DESATIVADOS por padrão; ative-os nas configurações (opt-in).\n"
    L"• Se não quiser enviar texto para a nuvem (Google), abra o menu do ícone da bandeja, escolha “Mecanismo de tradução” e selecione “Motor local integrado”. Sem modelo local instalado e com o recurso de nuvem desativado, a tradução não é executada — nada é enviado.\n"
    L"\n"
    L"O detalhamento completo está no arquivo README, para reler quando quiser.\n",
    L"Arquivo de configuração: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Texto longo demais: apenas o início e o fim foram traduzidos.",
    L"Há texto novo não traduzido acima. Coloque o cursor no final dessa linha e prima Enter para traduzir.",
    L"A reparar os componentes do motor local…",
    L"Tradução local indisponível",
    L"Os ficheiros do motor de tradução local não foram encontrados, pelo que a tradução foi suspensa. Reinstale o Emebala Chat para restaurar o motor local, ou mude para a tradução na nuvem (Google) escolhendo “Google Tradutor” no menu da bandeja, em “Motor de tradução”.",
    L"Compatível com OpenAI (servidor personalizado)…",
        L"Definições do motor compatível com OpenAI",
        L"Definições do motor compatível com OpenAI…",
        L"URL base",
        L"Chave de API",
        L"Modelo",
        L"Obter lista de modelos",
        L"Não foi possível obter a lista de modelos. Pode introduzir o nome do modelo diretamente.",
        L"Ligação insegura (HTTP)",
        L"O URL base utiliza HTTP (não encriptado). A sua chave de API e o texto serão enviados em texto simples. Continuar?",
        L"Definições compatíveis com OpenAI guardadas.",
        L"Chave guardada: ",
        L"O URL base não é válido. Exemplo: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Modelo especificado pelo usuário (.gguf)",
    L"Registrar outro modelo .gguf…",
        L"Aviso sobre a qualidade da tradução",
        L"O modelo selecionado não é Hy-MT2. A versão atual utiliza o prompt exclusivo do Hy-MT2, pelo que a qualidade da tradução com este modelo não é garantida. Continuar?",
        L"Modelo registado",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"O modelo selecionado foi registado no motor local.\n"
    L"\n"
    L"Este modelo é utilizado para tradução quando escolhe \"Seleção do motor de tradução > Escolha do utilizador (.gguf)\".\n"
    L"\n"
    L"Quando surte efeito: até cerca de 1 minuto após o registo (após a saída do motor por inatividade)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Este modelo já está integrado no Emebala Chat. Não é necessário registrá-lo. Selecione diretamente o mecanismo de tradução local integrado.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(não registrado)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Gerir modelos…",
    L"Gestor de modelos do utilizador",
    L"Sem modelos de utilizador registados.",
    L"Mudar o nome…",
    L"Eliminar…",
    L"Fechar",
    L"Eliminar registo do modelo",
    L"O registo do modelo selecionado será eliminado.\n"
    L"\n"
    L"O ficheiro do modelo (.gguf) é mantido no disco e não é eliminado. Se este modelo estava em utilização, a seleção do motor de tradução voltará a Automático (Auto).\n"
    L"\n"
    L"Continuar?",
    L"Mudar o nome do modelo",
    L"Introduza um novo nome (máx. 64 caracteres, sem espaços).",
    L"Esse nome não pode ser utilizado. Introduza um nome não vazio, diferente dos existentes, sem espaços nem separadores de caminho, até 64 caracteres.",
    L"Alterações guardadas.",

    L"Não foi possível serializar registry.json (um nome de ficheiro foi rejeitado). Nada foi alterado.",
    L"%LOCALAPPDATA% está indisponível; não foi possível localizar a pasta de modelos partilhada.",
    L"Não foi possível escrever registry.json.",
    L"registry.json não foi completamente escrito.",
    L"registry.json está danificado ou tem um esquema não suportado. NÃO foi modificado. Corrija-o ou remova-o e tente novamente.",
    L"Falha na tradução. Tente novamente.",
    L"O motor de tradução local está temporariamente indisponível (pode estar a iniciar). Aguarde um momento e tente novamente.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Confirmar",
    L"Cancelar",
};

// 12. Italian (it)
const LocalizedStrings kStringsItalian = {
    L"Stato: attivo (F9: pausa)",
    L"Stato: in pausa (F9: riprendi)",
    L"Motore di traduzione",
    L"Google Traduttore (gratis / senza installazione)",
    L"Motore locale integrato (Hy-MT2-1.8B offline)",
    L"Lingua di origine (input)",
    L"Lingua di destinazione (output)",
    L"Scambia origine ⇄ destinazione (doppio clic)",
    L"Invio automatico con Enter",
    L"Feedback sonoro (toni)",
    L"Mostra badge fluttuante",
    L"Avvia con Windows",
    L"Promemoria scorciatoie e guida...",
    L"Esci da Emebala Chat",
    L"Informazioni su Emebala Chat…",
    L"Emebala Chat — scorciatoie e guida",
    L"Scorciatoie e guida all'uso di Emebala Chat:\n\n"
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
    L"Informazioni su Emebala Chat",
    L"Attivo",
    L"Traduzione...",
    L"In pausa",
    L"Emebala Chat",
    L"Impossibile copiare il testo selezionato. Controlla l'app di destinazione e riprova.",
    L"Nessun testo selezionato da tradurre.",
    L"Rilevamento automatico",
    L"Emebala Chat è già in esecuzione in background.\nControlla l'area di notifica.",
    L"Inizializzazione COM non riuscita.\nIl badge fluttuante e la sintesi vocale non saranno disponibili,\nma traduzione, scorciatoie, tray e suoni continueranno a funzionare.",
    L"Basta copiare e incollare. Scrivi naturalmente nella tua lingua madre: la traduzione sostituisce la tua digitazione in tempo reale in qualsiasi app Windows.",
    L"⚡ Trascina e traduci — seleziona il testo in qualsiasi app, l'icona fluttuante traduce all'istante.",
    L"🔊 TTS neurale — pronuncia tutte le 37 lingue tramite i pacchetti vocali di Windows installati.",
    L"🔒 100% sul dispositivo e privato — attivo solo mentre la scorciatoia è premuta; appunti intatti.",
    L"Nel 2000 a.C., gli scribi mesopotamici chiamavano «Eme-bala» coloro che fanno del linguaggio un ponte tra mondi.",
    L"Sito web",
    L"Contatti",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Orari d'ufficio 10:00–19:00 KST",
    L"Architetto principale: Yongtai Kim",
    L"✓ Copiato!",
    L"📋 Copia",
    L"🔊 Voce",
    L"Lingua dell'interfaccia",
    L"Auto (lingua di sistema)",
    L"Ripristina valori predefiniti di sistema",
    L"Valori ripristinati",
    L"Digitazione da tastiera",
    L"Tooltip di trascinamento",
    L"Emebala Chat",
    L"Nessuna voce Windows installata per questa lingua. Fai di nuovo clic su 🔊 per aprire le impostazioni di Riconoscimento vocale.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Informativa sulla privacy",
    L"Ecco i principi sulla privacy di Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat non gestisce alcun server proprio.\n"
    L"• Con il modello locale, il testo tradotto non lascia il dispositivo.\n"
    L"• Se scegli Google Traduttore o il passaggio al cloud è automatico, il testo selezionato o digitato viene inviato direttamente a Google per la traduzione, senza passare da Emebala.\n"
    L"• I log di diagnostica sono DISATTIVATI per impostazione predefinita; attivali nelle impostazioni (opt-in).\n"
    L"• Se non vuoi inviare testo al cloud (Google), apri il menu dell'icona nella barra delle applicazioni, scegli “Motore di traduzione” e seleziona “Motore locale integrato”. Senza modello locale installato e con il fallback cloud disattivato, la traduzione non viene eseguita e non viene inviato nulla.\n"
    L"\n"
    L"Il dettaglio completo è nel file README, rileggibile in qualsiasi momento.\n",
    L"File di configurazione: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Testo troppo lungo: sono stati tradotti solo l'inizio e la fine.",
    L"C'è nuovo testo non tradotto sopra. Posiziona il cursore alla fine di quella riga e premi Invio per tradurlo.",
    L"Riparazione dei componenti del motore locale…",
    L"Traduzione locale non disponibile",
    L"I file del motore di traduzione locale non sono stati trovati, quindi la traduzione è sospesa. Reinstalla Emebala Chat per ripristinare il motore locale, oppure passa alla traduzione cloud (Google) scegliendo “Google Traduttore” dal menu dell’area di notifica, “Motore di traduzione”.",
    L"Compatibile con OpenAI (server personalizzato)…",
        L"Impostazioni motore compatibile OpenAI",
        L"Impostazioni motore compatibile OpenAI…",
        L"URL base",
        L"Chiave API",
        L"Modello",
        L"Recupera elenco modelli",
        L"Impossibile recuperare l'elenco dei modelli. Puoi digitare direttamente il nome del modello.",
        L"Connessione non sicura (HTTP)",
        L"L'URL base usa HTTP (non crittografato). La chiave API e il testo verranno inviati in chiaro. Continuare?",
        L"Impostazioni compatibili OpenAI salvate.",
        L"Chiave salvata: ",
        L"L'URL base non è valido. Esempio: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Modello specificato dall'utente (.gguf)",
    L"Registra un altro modello .gguf…",
        L"Avviso sulla qualità della traduzione",
        L"Il modello selezionato non è Hy-MT2. La versione attuale usa il prompt esclusivo di Hy-MT2, quindi la qualità della traduzione con questo modello non è garantita. Continuare?",
        L"Modello registrato",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Il modello selezionato è stato registrato nel motore locale.\n"
    L"\n"
    L"Questo modello viene usato per la traduzione quando scegli \"Selezione motore di traduzione > Scelta utente (.gguf)\".\n"
    L"\n"
    L"Quando ha effetto: al massimo circa 1 minuto dopo la registrazione (dopo l'uscita del motore per inattività)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Questo modello è già integrato in Emebala Chat. Non è necessario registrarlo. Puoi selezionare direttamente il motore di traduzione locale integrato.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(non registrato)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Gestisci modelli…",
    L"Gestione modelli utente",
    L"Nessun modello utente registrato.",
    L"Rinomina…",
    L"Elimina…",
    L"Chiudi",
    L"Elimina registrazione modello",
    L"La registrazione del modello selezionato verrà eliminata.\n"
    L"\n"
    L"Il file del modello (.gguf) viene mantenuto sul disco e non viene eliminato. Se questo modello era in uso, la selezione del motore di traduzione tornerà a Automatico (Auto).\n"
    L"\n"
    L"Continuare?",
    L"Rinomina modello",
    L"Inserisci un nuovo nome (max 64 caratteri, senza spazi).",
    L"Questo nome non può essere usato. Inserisci un nome non vuoto, diverso da quelli esistenti, senza spazi né separatori di percorso, entro 64 caratteri.",
    L"Modifiche salvate.",

    L"Impossibile serializzare registry.json (un nome file è stato rifiutato). Non è stato modificato nulla.",
    L"%LOCALAPPDATA% non disponibile; impossibile trovare la cartella dei modelli condivisa.",
    L"Impossibile scrivere registry.json.",
    L"registry.json non è stato scritto completamente.",
    L"registry.json è danneggiato o ha uno schema non supportato. NON è stato modificato. Riparalo o rimuovilo e riprova.",
    L"Traduzione non riuscita. Riprova.",
    L"Il motore di traduzione locale è temporaneamente non disponibile (potrebbe essere in avvio). Attendi un momento e riprova.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Conferma",
    L"Annulla",
};

// 13. Dutch (nl)
const LocalizedStrings kStringsDutch = {
    L"Status: actief (F9: pauze)",
    L"Status: gepauzeerd (F9: hervatten)",
    L"Vertaalengine",
    L"Google Vertalen (gratis / zonder installatie)",
    L"Ingebouwde lokale engine (Hy-MT2-1.8B offline)",
    L"Brontaal (invoer)",
    L"Doeltaal (uitvoer)",
    L"Bron ⇄ doel wisselen (dubbelklik)",
    L"Automatisch verzenden met Enter",
    L"Geluidsfeedback (tonen)",
    L"Zwevend badge zichtbaar",
    L"Starten met Windows",
    L"Sneltoetsenoverzicht en hulp...",
    L"Emebala Chat afsluiten",
    L"Over Emebala Chat…",
    L"Emebala Chat — sneltoetsen en handleiding",
    L"Sneltoetsen en handleiding voor Emebala Chat:\n\n"
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
    L"Over Emebala Chat",
    L"Actief",
    L"Vertalen...",
    L"Gepauzeerd",
    L"Emebala Chat",
    L"Kon de geselecteerde tekst niet kopiëren. Controleer de doel-app en probeer het opnieuw.",
    L"Er is geen tekst geselecteerd om te vertalen.",
    L"Automatisch herkennen",
    L"Emebala Chat wordt al uitgevoerd op de achtergrond.\nControleer het meldingsvak.",
    L"COM-initialisatie mislukt.\nHet zwevende badge en voorlezen zijn niet beschikbaar,\nmaar vertalen, sneltoetsen, tray en geluiden werken nog steeds.",
    L"Nooit meer kopiëren en plakken. Typ natuurlijk in je moedertaal — de vertaling vervangt je getynde tekst realtime in elke Windows-app.",
    L"⚡ Sleep-om-te-vertalen — selecteer tekst in elke app, het zwevende pictogram vertaalt meteen.",
    L"🔊 Neurale TTS — spreekt alle 37 talen via geïnstalleerde Windows-spraakpakketten.",
    L"🔒 100% lokaal en privé — alleen actief zolang de sneltoets wordt ingehouden; klembord onaangeroerd.",
    L"In 2000 v.Chr. noemden Mesopotamische schrijvers «Eme-bala» — hen die taal omzetten in een brug tussen werelden.",
    L"Website",
    L"Contact",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Kantooruren 10:00–19:00 KST",
    L"Hoofdarchitect: Yongtai Kim",
    L"✓ Gekopieerd!",
    L"📋 Kopiëren",
    L"🔊 Stem",
    L"Interfacetaal",
    L"Automatisch (systeemtaal)",
    L"Reset naar systeemstandaarden",
    L"Standaarden hersteld",
    L"Typen op toetsenbord",
    L"Sleep-tooltip",
    L"Emebala Chat",
    L"Er is geen Windows-stem geïnstalleerd voor deze taal. Klik nogmaals op 🔊 om de Spraak-instellingen te openen.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Privacymelding",
    L"Dit zijn de privacyprincipes van Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat beheert geen eigen servers.\n"
    L"• Met het lokale model verlaat de vertaalde tekst uw apparaat niet.\n"
    L"• Als u Google Translate kiest of automatisch naar de cloud schakelt, wordt de geselecteerde of getypte tekst rechtstreeks naar Google verzonden voor vertaling, niet via Emebala.\n"
    L"• Diagnosticelogboeken zijn standaard UIT; schakel ze in via de instellingen (opt-in).\n"
    L"• Als u geen tekst naar de cloud (Google) wilt sturen, opent u het menu van het systeemvakpictogram, kiest u “Vertaalengine” en vervolgens “Ingebouwde lokale engine”. Zonder lokaal model en met cloud-omleiding uitgeschakeld wordt er niet vertaald — er wordt niets verzonden.\n"
    L"\n"
    L"Alle details staan in het README-bestand, dat u altijd opnieuw kunt lezen.\n",
    L"Configuratiebestand: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"De tekst was te lang: alleen het begin en het einde zijn vertaald.",
    L"Er staat nieuwe onvertaalde tekst hierboven. Plaats de cursor aan het einde van die regel en druk op Enter om te vertalen.",
    L"Lokale engine-onderdelen herstellen…",
    L"Lokale vertaling niet beschikbaar",
    L"De bestanden van de lokale vertaalengine ontbreken, dus de vertaling is onderbroken. Installeer Emebala Chat opnieuw om de lokale engine te herstellen, of schakel over naar cloud(Google)-vertaling door “Google Vertalen” te kiezen in het menubalkmenu bij “Vertaalengine”.",
    L"OpenAI-compatibel (aangepaste server)…",
        L"OpenAI-compatibele engine-instellingen",
        L"OpenAI-compatibele engine-instellingen…",
        L"Basis-URL",
        L"API-sleutel",
        L"Model",
        L"Modellenlijst ophalen",
        L"De modellenlijst kon niet worden opgehaald. U kunt een modelnaam direct typen.",
        L"Onbeveiligde verbinding (HTTP)",
        L"De basis-URL gebruikt HTTP (niet versleuteld). Uw API-sleutel en tekst worden als leesbare tekst verzonden. Doorgaan?",
        L"OpenAI-compatibele instellingen opgeslagen.",
        L"Opgeslagen sleutel: ",
        L"De basis-URL is ongeldig. Voorbeeld: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Door gebruiker gespecificeerd model (.gguf)",
    L"Ander .gguf-model registreren…",
        L"Mededeling over vertaalkwaliteit",
        L"Het geselecteerde model is geen Hy-MT2. De huidige versie gebruikt de alleen-voor-Hy-MT2 prompt, dus de vertaalkwaliteit met dit model is niet gegarandeerd. Doorgaan?",
        L"Model geregistreerd",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Het geselecteerde model is geregistreerd bij de lokale engine.\n"
    L"\n"
    L"Dit model wordt gebruikt voor vertaling wanneer u \"Vertaalengine selecteren > Keuze van gebruiker (.gguf)\" kiest.\n"
    L"\n"
    L"Wanneer van kracht: tot ongeveer 1 minuut na registratie (na het beëindigen van de engine door inactiviteit)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Dit model is al ingebouwd in Emebala Chat. Registratie is niet nodig. Selecteer de ingebouwde lokale vertaalengine direct.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(niet geregistreerd)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Modellen beheren…",
    L"Gebruikersmodelbeheer",
    L"Geen gebruikersmodellen geregistreerd.",
    L"Hernoemen…",
    L"Verwijderen…",
    L"Sluiten",
    L"Modelregistratie verwijderen",
    L"De registratie van het geselecteerde model wordt verwijderd.\n"
    L"\n"
    L"Het modelbestand (.gguf) blijft op de schijf staan en wordt niet verwijderd. Als dit model in gebruik was, keert de keuze van de vertaalengine terug naar Auto.\n"
    L"\n"
    L"Doorgaan?",
    L"Model hernoemen",
    L"Voer een nieuwe naam in (max. 64 tekens, geen spaties).",
    L"Die naam kan niet worden gebruikt. Voer een niet-lege naam in die afwijkt van bestaande namen, zonder spaties of padscheidingstekens, van maximaal 64 tekens.",
    L"Wijzigingen opgeslagen.",

    L"registry.json kon niet worden geserialiseerd (een bestandsnaam is afgewezen). Er is niets gewijzigd.",
    L"%LOCALAPPDATA% is niet beschikbaar; de gedeelde modellenmap kan niet worden gevonden.",
    L"registry.json kon niet worden weggeschreven.",
    L"registry.json kon niet volledig worden weggeschreven.",
    L"registry.json is beschadigd of heeft een niet-ondersteund schema. Het is NIET gewijzigd. Herstel of verwijder het en probeer het opnieuw.",
    L"Vertaling mislukt. Probeer het opnieuw.",
    L"De lokale vertaalengine is tijdelijk niet beschikbaar (mogelijk aan het starten). Wacht even en probeer het opnieuw.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Bevestigen",
    L"Annuleren",
};

// 14. Polish (pl)
const LocalizedStrings kStringsPolish = {
    L"Status: aktywny (F9: pauza)",
    L"Status: wstrzymany (F9: wznow)",
    L"Silnik tłumaczenia",
    L"Tłumacz Google (darmowy / bez instalacji)",
    L"Wbudowany silnik lokalny (Hy-MT2-1.8B offline)",
    L"Język źródłowy (wejście)",
    L"Język docelowy (wyjście)",
    L"Zamień źródło ⇄ cel (dwuklik)",
    L"Automatyczne wysyłanie po Enter",
    L"Dźwięki potwierdzenia",
    L"Pływająca odznaka widoczna",
    L"Uruchamiaj z Windowsem",
    L"Ściąga skrótów i pomoc...",
    L"Zamknij Emebala Chat",
    L"O Emebala Chat…",
    L"Emebala Chat — skróty i przewodnik",
    L"Skróty i przewodnik po Emebala Chat:\n\n"
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
    L"O Emebala Chat",
    L"Aktywny",
    L"Tłumaczenie...",
    L"Wstrzymany",
    L"Emebala Chat",
    L"Nie udało się skopiować zaznaczonego tekstu. Sprawdź aplikację docelową i spróbuj ponownie.",
    L"Nie wybrano tekstu do przetłumaczenia.",
    L"Wykryj automatycznie",
    L"Emebala Chat jest już uruchomiony w tle.\nSprawdź zasobnik powiadomień.",
    L"Inicjalizacja COM nie powiodła się.\nPływająca odznaka i czytanie na głos będą niedostępne,\nale tłumaczenie, skróty, zasobnik i dźwięki nadal działają.",
    L"Koniec z kopiowaniem i wklejaniem. Pis naturalnie w swoim języku ojczystym — tłumaczenie zastępuje Twoje wpisy w czasie rzeczywistym w każdej aplikacji Windows.",
    L"⚡ Przeciągnij, aby przetłumaczyć — zaznacz tekst w dowolnej aplikacji, pływająca ikona tłumaczy natychmiast.",
    L"🔊 Neuralne TTS — wymawia wszystkie 37 języków dzięki zainstalowanym pakietom głosowym Windows.",
    L"🔒 100% na urządzeniu i prywatne — aktywne tylko podczas trzymania skrótu; schowek nienaruszony.",
    L"W 2000 r. p.n.e. mezopotamscy skrybowie nazywali «Eme-bala» tych, którzy zamieniają język w most między światami.",
    L"Strona internetowa",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Godziny pracy 10:00–19:00 KST",
    L"Główny architekt: Yongtai Kim",
    L"✓ Skopiowano!",
    L"📋 Kopiuj",
    L"🔊 Głos",
    L"Język interfejsu",
    L"Auto (język systemu)",
    L"Przywróć ustawienia systemowe",
    L"Przywrócono wartości domyślne",
    L"Pisanie na klawiaturze",
    L"Etykieta przeciągania",
    L"Emebala Chat",
    L"Dla tego języka nie zainstalowano głosu Windows. Kliknij ponownie 🔊, aby otworzyć ustawienia Mowy.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Informacja o prywatności",
    L"Oto zasady prywatności w Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat nie prowadzi własnych serwerów.\n"
    L"• Przy modelu lokalnym tłumaczony tekst nie opuszcza urządzenia.\n"
    L"• Jeśli wybierzesz Tłumacz Google lub nastąpi automatyczne przełączenie w chmurę, zaznaczony lub wpisany tekst jest wysyłany bezpośrednio do Google w celu tłumaczenia, bez pośrednictwa Emebala.\n"
    L"• Logi diagnostyczne są domyślnie WYŁĄCZONE; włącz je w konfiguracji (zgoda).\n"
    L"• Jeśli nie chcesz wysyłać tekstu do chmury (Google), otwórz menu ikony w zasobniku systemowym, wybierz “Silnik tłumaczenia” i zaznacz “Wbudowany silnik lokalny”. Bez zainstalowanego modelu lokalnego i przy wyłączonym przejściu do chmury tłumaczenie nie działa — nic nie jest wysyłane.\n"
    L"\n"
    L"Szczegóły znajdziesz w pliku README — możesz go przeczytać w dowolnej chwili.\n",
    L"Plik konfiguracji: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Tekst był zbyt długi: przetłumaczono tylko początek i koniec.",
    L"Powyżej znajduje się nowy nieprzetłumaczony tekst. Umieść kursor na końcu tego wiersza i naciśnij Enter, aby przetłumaczyć.",
    L"Naprawianie lokalnych składników silnika…",
    L"Tłumaczenie lokalne niedostępne",
    L"Nie znaleziono plików lokalnego silnika tłumaczenia, więc tłumaczenie zostało wstrzymane. Zainstaluj ponownie Emebala Chat, aby przywrócić lokalny silnik, lub przełącz się na tłumaczenie w chmurze (Google), wybierając „Google Translate” w menu zasobnika, „Silnik tłumaczenia”.",
    L"Zgodne z OpenAI (serwer użytkownika)…",
        L"Ustawienia silnika zgodnego z OpenAI",
        L"Ustawienia silnika zgodnego z OpenAI…",
        L"Base URL",
        L"Klucz API",
        L"Model",
        L"Pobierz listę modeli",
        L"Nie udało się pobrać listy modeli. Możesz wpisać nazwę modelu bezpośrednio.",
        L"Niezabezpieczone połączenie (HTTP)",
        L"Base URL używa HTTP (bez szyfrowania). Twój klucz API i tekst zostaną wysłane jako zwykły tekst. Kontynuować?",
        L"Zapisano ustawienia zgodne z OpenAI.",
        L"Zapisany klucz: ",
        L"Base URL jest nieprawidłowy. Przykład: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Model określony przez użytkownika (.gguf)",
    L"Zarejestruj inny model .gguf…",
        L"Informacja o jakości tłumaczenia",
        L"Wybrany model nie jest Hy-MT2. Bieżąca wersja używa promptu wyłącznie dla Hy-MT2, więc jakość tłumaczenia tym modelem nie jest gwarantowana. Kontynuować?",
        L"Model zarejestrowany",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Wybrany model został zarejestrowany w lokalnym silniku.\n"
    L"\n"
    L"Model ten jest używany do tłumaczenia po wybraniu \"Wybór silnika tłumaczenia > Wybór użytkownika (.gguf)\".\n"
    L"\n"
    L"Kiedy zaczyna obowiązywać: do około 1 minuty po rejestracji (po zakończeniu działania silnika z powodu bezczynności)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Ten model jest już wbudowany w Emebala Chat. Rejestracja nie jest potrzebna. Wystarczy bezpośrednio wybrać wbudowany lokalny silnik tłumaczenia.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(niezarejestrowany)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Zarządzaj modelami…",
    L"Menedżer modeli użytkownika",
    L"Brak zarejestrowanych modeli użytkownika.",
    L"Zmień nazwę…",
    L"Usuń…",
    L"Zamknij",
    L"Usuń rejestrację modelu",
    L"Rejestracja wybranego modelu zostanie usunięta.\n"
    L"\n"
    L"Plik modelu (.gguf) pozostaje na dysku i nie zostanie usunięty. Jeśli ten model był używany, wybór silnika tłumaczenia powróci do Automatyczny (Auto).\n"
    L"\n"
    L"Kontynuować?",
    L"Zmień nazwę modelu",
    L"Wpisz nową nazwę (maks. 64 znaki, bez spacji).",
    L"Tej nazwy nie można użyć. Wpisz niepustą nazwę, różną od istniejących, bez spacji i separatorów ścieżki, do 64 znaków.",
    L"Zapisano zmiany.",

    L"Nie można zserializować registry.json (nazwa pliku została odrzucona). Nic nie zmieniono.",
    L"%LOCALAPPDATA% jest niedostępny; nie można zlokalizować folderu wspólnych modeli.",
    L"Nie można zapisać registry.json.",
    L"Nie można w pełni zapisać registry.json.",
    L"registry.json jest uszkodzony lub ma nieobsługiwaną strukturę. NIE został zmieniony. Napraw go lub usuń i spróbuj ponownie.",
    L"Tłumaczenie nie powiodło się. Spróbuj ponownie.",
    L"Lokalny silnik tłumaczenia jest tymczasowo niedostępny (może się uruchamiać). Poczekaj chwilę i spróbuj ponownie.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Potwierdź",
    L"Anuluj",
};

// 15. Czech (cs)
const LocalizedStrings kStringsCzech = {
    L"Stav: aktivní (F9: pauza)",
    L"Stav: pozastaveno (F9: pokračovat)",
    L"Překladový engine",
    L"Google Překladač (zdarma / bez instalace)",
    L"Vestavěný lokální engine (Hy-MT2-1.8B offline)",
    L"Zdrojový jazyk (vstup)",
    L"Cílový jazyk (výstup)",
    L"Prohodit zdroj ⇄ cíl (dvojklik)",
    L"Automatické odeslání klávesou Enter",
    L"Zvuková zpětná vazba",
    L"Plovoucí odznak viditelný",
    L"Spouštět s Windows",
    L"Přehled klávesových zkratek a nápověda...",
    L"Ukončit Emebala Chat",
    L"O Emebala Chat…",
    L"Emebala Chat — klávesové zkratky a průvodce",
    L"Klávesové zkratky a průvodce Emebala Chat:\n\n"
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
    L"O Emebala Chat",
    L"Aktivní",
    L"Překládám...",
    L"Pozastaveno",
    L"Emebala Chat",
    L"Vybraný text nelze zkopírovat. Zkontrolujte cílovou aplikaci a zkuste to znovu.",
    L"Nebyl vybrán žádný text k překladu.",
    L"Automaticky rozpoznat",
    L"Emebala Chat již běží na pozadí.\nZkontrolujte oznamovací oblast.",
    L"Inicializace COM se nezdařila.\nPlovoucí odznak a převod textu na řeč budou nedostupné,\nale překlad, klávesové zkratky, tray a zvuky stále fungují.",
    L"Konec kopírování a vkládání. Pište přirozeně ve svém rodném jazyce — překlad nahrazuje váš psaní v reálném čase v libovolné aplikaci Windows.",
    L"⚡ Přetáhněte pro překlad — vyberte text v libovolné aplikaci, plovoucí ikna přeloží okamžitě.",
    L"🔊 Neuronové TTS — vyslovuje všech 37 jazyků pomocí nainstalovaných hlasových balíčků Windows.",
    L"🔒 100% na zařízení a soukromé — aktivní pouze při podržení zkratky; schránka nedotčena.",
    L"V roce 2000 př. n. l. mezopotámští písaři nazývaly «Eme-bala» ty, kdo mění jazyk v most mezi světy.",
    L"Web",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Pracovní doba 10:00–19:00 KST",
    L"Hlavní architekt: Yongtai Kim",
    L"✓ Zkopírováno!",
    L"📋 Kopírovat",
    L"🔊 Hlas",
    L"Jazyk rozhraní",
    L"Auto (systémový jazyk)",
    L"Obnovit výchozí nastavení systému",
    L"Výchozí hodnoty obnoveny",
    L"Psaní na klávesnici",
    L"Tooltip tažení",
    L"Emebala Chat",
    L"Pro tento jazyk není nainstalován žádný hlas Windows. Klikněte znovu na 🔊 pro otevření nastavení Rozpoznávání řeči.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Oznámení o ochraně soukromí",
    L"Zde jsou zásady ochrany soukromí aplikace Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat neprovožuje žádné vlastní servery.\n"
    L"• Při lokálním modelu překládaný text neopouští vaše zařízení.\n"
    L"• Pokud zvolíte Google Translate nebo dojde k automatickému přepnutí do cloudu, vybraný nebo zadaný text je posílán přímo společnosti Google k přeložení, bez prochzení přes Emebala.\n"
    L"• Diagnostické protokoly jsou ve výchozím nastavení VYPNUTÉ; zapnete je v nastavení (opt-in).\n"
    L"• Pokud nechcete odesílat text do cloudu (Google), otevřete nabídku ikony v oznamovací oblasti, zvolte “Překladový engine” a vyberte “Vestavěný lokální engine”. Bez nainstalovaného lokálního modelu a se zakázaným cloudovým zálohováním překlad neběží — nic se neodesílá.\n"
    L"\n"
    L"Podrobnosti najdete v souboru README, který lze kdykoli znovu přečíst.\n",
    L"Konfigurační soubor: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Text byl příliš dlouhý: přeloženy byly pouze začátek a konec.",
    L"Nahoře je nový nepřeložený text. Umístěte kurzor na konec tohoto řádku a stisknutím Enteru jej přeložte.",
    L"Oprava místních součástí enginu…",
    L"Místní překlad není dostupný",
    L"Soubory místního překladového enginu chybí, takže je překlad pozastaven. Pro obnovení místního enginu přeinstalujte Emebala Chat, nebo pro přechod na cloudový (Google) překlad vyberte v nabídce oznamovací oblasti „Google Překladač“ v části „Překladový engine“.",
    L"Kompatibilní s OpenAI (uživatelský server)…",
        L"Nastavení enginu kompatibilního s OpenAI",
        L"Nastavení enginu kompatibilního s OpenAI…",
        L"Základní URL",
        L"API klíč",
        L"Model",
        L"Načíst seznam modelů",
        L"Seznam modelů se nepodařilo načíst. Název modelu můžete zadat přímo.",
        L"Nezabezpečené připojení (HTTP)",
        L"Základní URL používá HTTP (nešifrované). Váš API klíč a text budou odeslány v nešifrované podobě. Pokračovat?",
        L"Nastavení kompatibilní s OpenAI uložena.",
        L"Uložený klíč: ",
        L"Základní URL není platná. Příklad: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Model určený uživatelem (.gguf)",
    L"Zaregistrovat jiný model .gguf…",
        L"Upozornění na kvalitu překladu",
        L"Vybraný model není Hy-MT2. Aktuální verze používá výhradně prompt pro Hy-MT2, takže kvalita překladu s tímto modelem není zaručena. Pokračovat?",
        L"Model zaregistrován",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Vybraný model byl zaregistrován v lokálním enginu.\n"
    L"\n"
    L"Tento model se používá k překladu, když zvolíte \"Výběr překladového enginu > Volba uživatele (.gguf)\".\n"
    L"\n"
    L"Kdy se projeví: nejpozději přibližně 1 minutu po registraci (po ukončení enginu z důvodu nečinnosti)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Tento model je již vestavěný v Emebala Chat. Registrace není potřeba. Vestavěný lokální překladový engine můžete vybrat přímo.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(není registrován)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Spravovat modely…",
    L"Správa uživatelských modelů",
    L"Nejsou zaregistrovány žádné uživatelské modely.",
    L"Přejmenovat…",
    L"Odstranit…",
    L"Zavřít",
    L"Odstranit registraci modelu",
    L"Registrace vybraného modelu bude odstraněna.\n"
    L"\n"
    L"Soubor modelu (.gguf) zůstane na disku a nebude odstraněn. Pokud byl tento model používán, výběr překladového enginu se vrátí na Automaticky (Auto).\n"
    L"\n"
    L"Pokračovat?",
    L"Přejmenovat model",
    L"Zadejte nový název (max. 64 znaků, bez mezer).",
    L"Tento název nelze použít. Zadejte neprázdný název, který se liší od existujících, bez mezer a oddělovačů cesty, do 64 znaků.",
    L"Změny byly uloženy.",

    L"registry.json se nepodařilo serializovat (název souboru byl odmítnut). Nic se nezměnilo.",
    L"%LOCALAPPDATA% není k dispozici; nelze najít sdílenou složku modelů.",
    L"registry.json se nepodařilo zapsat.",
    L"registry.json se nepodařilo zapsat celý.",
    L"registry.json je poškozený nebo má nepodporované schéma. Nebyl změněn. Opravte ho nebo ho odstraňte a zkuste to znovu.",
    L"Překlad se nezdařil. Zkuste to znovu.",
    L"Místní překladový engine je dočasně nedostupný (možná se spouští). Počkejte chvíli a zkuste to znovu.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Potvrdit",
    L"Storno",
};

// 16. Hungarian (hu)
const LocalizedStrings kStringsHungarian = {
    L"Állapot: aktív (F9: szünet)",
    L"Állapot: szünetel (F9: folytatás)",
    L"Fordítómotor",
    L"Google Fordító (ingyenes / telepítés nélkül)",
    L"Beépített helyi motor (Hy-MT2-1.8B offline)",
    L"Forrásnyelv (bemenet)",
    L"Célnyelv (kimenet)",
    L"Forrás ⇄ cél felcserélése (dupla kattintás)",
    L"Automatikus küldés Enterrel",
    L"Hangvisszajelzés (hangnemek)",
    L"Lebegő jelvény látható",
    L"Indítás Windows-szal",
    L"Gyorsbillentyű-súgó és súgó...",
    L"Emebala Chat bezárása",
    L"Az Emebala Chat névjegye…",
    L"Emebala Chat — gyorsbillentyűk és útmutató",
    L"Gyorsbillentyűk és útmutató az Emebala Chathez:\n\n"
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
    L"Az Emebala Chat névjegye",
    L"Aktív",
    L"Fordítás...",
    L"Szünetel",
    L"Emebala Chat",
    L"Nem sikerült kimásolni a kijelölt szöveget. Ellenőrizze a célalkalmazást, és próbálja újra.",
    L"Nincs fordításra kijelölt szöveg.",
    L"Automatikus felismerés",
    L"Az Emebala Chat már fut a háttérben.\nEllenőrizze az értesítési tálcát.",
    L"A COM inicializálása sikertelen.\nA lebegő jelvény és a felolvasás nem lesz elérhető,\nde a fordítás, gyorsbillentyűk, tálca és hangok tovább működnek.",
    L"Kopírozás és beillesztés többé nem kell. Gépeljen természetesen az anyanyelvén — a fordítás valós időben felváltja a gépelést bármely Windows-alkalmazásban.",
    L"⚡ Húzássos fordítás — jelöljön ki szöveget bármely appban, a lebegő ikon azonnal fordít.",
    L"🔊 Neurális TTS — mind a 37 nyelvet kimondja a telepített Windows hangcsomagokkal.",
    L"🔒 100% eszközön és privát — csak a gyorsbillentyű tartása közben aktív; vágólap érintetlen.",
    L"Kr. e. 2000-ben a mezopotámiai írnokok „Eme-bala”-nak nevezték azokat, akik a nyelvet a világok hidává alakítják.",
    L"Weboldal",
    L"Kapcsolat",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Munkaidő 10:00–19:00 KST",
    L"Vezető építész: Yongtai Kim",
    L"✓ Kimásolva!",
    L"📋 Másolás",
    L"🔊 Hang",
    L"Felhasználói felület nyelve",
    L"Automatikus (rendszernyelv)",
    L"Visszaállítás rendszeralapértelmezettekre",
    L"Alapértelmezések visszaállítva",
    L"Billentyűzetes gépelés",
    L"Húzás tippablak",
    L"Emebala Chat",
    L"Ehhez a nyelvhez nincs telepítve Windows-hang. Kattintson újra a 🔊 gombra a Beszéd beállításainak megnyitásához.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Adatvédelmi tájékoztató",
    L"Az Emebala Chat adatvédelmi elvei:\n"
    L"\n"
    L"• Az Emebala Chat nem üzemeltet saját szervert.\n"
    L"• Helyi modell használatakor a fordított szöveg nem hagyja el az eszközt.\n"
    L"• Ha a Google Fordítót választja, vagy automatikusan felhő üzemmódra vált, a kijelölt vagy bevitt szöveg közvetlenül a Google-höz megy a fordításhoz – nem az Emebala-n keresztül.\n"
    L"• A diagnosztikai naplók alapértelmezés szerint KI vannak kapcsolva; a beállításokban kapcsolhatja be (hozzájulás).\n"
    L"• Ha nem szeretné szöveget a felhőbe (Google) küldeni, nyissa meg a tálcaikon menüjét, válassza a “Fordítómotor” pontot, majd a “Beépített helyi motor” lehetőséget. Ha nincs telepített helyi modell és a felhőtartalék ki van kapcsolva, a fordítás nem fut — semmi sem kerül küldésre.\n"
    L"\n"
    L"A teljes részletezés a README fájlban olvasható, bármikor újra.\n",
    L"Konfigurációs fájl: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"A szöveg túl hosszú: csak az eleje és a vége lett lefordítva.",
    L"Új, le nem fordított szöveg van fent. Helyezze a kurzort a sor végére, és az Enter megnyomásával fordítsa le.",
    L"A helyi motor összetevőinek javítása…",
    L"A helyi fordítás nem érhető el",
    L"A helyi fordítómotor fájljai hiányoznak, ezért a fordítás szünetel. A helyi motor helyreállításához telepítse újra az Emebala Chatet, vagy váltson a felhőalapú (Google) fordításra a „Google Fordító” választásával a tálca „Fordítómotor” menüjében.",
    L"OpenAI-kompatibilis (egyéni szerver)…",
        L"OpenAI-kompatibilis motor beállításai",
        L"OpenAI-kompatibilis motor beállításai…",
        L"Alap-URL",
        L"API-kulcs",
        L"Modell",
        L"Modelllista lekérése",
        L"A modelllista lekérése nem sikerült. Modellnevet közvetlenül is beírhat.",
        L"Nem biztonságos kapcsolat (HTTP)",
        L"Az alap-URL HTTP-et használ (titkosítás nélkül). API-kulcsa és szövege titkosítatlanul lesz elküldve. Folytatja?",
        L"OpenAI-kompatibilis beállítások mentve.",
        L"Mentett kulcs: ",
        L"Az alap-URL érvénytelen. Példa: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Felhasználó által megadott modell (.gguf)",
    L"Más .gguf modell regisztrálása…",
        L"Tájékoztatás a fordítás minőségéről",
        L"A kiválasztott modell nem Hy-MT2. A jelenlegi verzió csak Hy-MT2-höz készült promptot használ, ezért a fordítás minősége ezzel a modellel nem garantált. Folytatja?",
        L"Modell regisztrálva",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"A kiválasztott modell regisztrálva lett a helyi motorban.\n"
    L"\n"
    L"Ez a modell akkor használható fordításhoz, amikor a \"Fordítómotor választása > Felhasználói választás (.gguf)\" lehetőséget választja.\n"
    L"\n"
    L"Mikor lép hatályba: regisztráció után legfeljebb kb. 1 perc (a motor inaktivitás miatti leállása után)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Ez a modell már beépítve van az Emebala Chatbe. Regisztrációra nincs szükség. A beépített helyi fordítómotort közvetlenül kiválaszthatod.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(nincs regisztrálva)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Modellek kezelése…",
    L"Felhasználói modellek kezelése",
    L"Nincsenek regisztrált felhasználói modellek.",
    L"Átnevezés…",
    L"Törlés…",
    L"Bezárás",
    L"Modellregisztráció törlése",
    L"A kiválasztott modell regisztrációja törlődik.\n"
    L"\n"
    L"A modellfájl (.gguf) a lemezen marad és nem törlődik. Ha ezt a modellt használta, a fordítómotor választása visszaáll Automatikus (Auto) értékre.\n"
    L"\n"
    L"Folytatja?",
    L"Modell átnevezése",
    L"Adjon meg új nevet (legfeljebb 64 karakter, szóköz nélkül).",
    L"Ez a név nem használható. Adj meg egy nem üres, a meglévőktől eltérő, szóközöket és elválasztókat nem tartalmazó, legfeljebb 64 karakteres nevet.",
    L"Változások mentve.",

    L"A registry.json nem szerializálható (egy fájlnév elutasítva). Semmi sem változott.",
    L"A %LOCALAPPDATA% nem érhető el; a megosztott modellmappa nem található.",
    L"A registry.json nem írható.",
    L"A registry.json nem írható teljesen.",
    L"A registry.json sérült vagy nem támogatott sémájú. NEM lett módosítva. Javítsa ki vagy távolítsa el, majd próbálja újra.",
    L"A fordítás nem sikerült. Próbálja újra.",
    L"A helyi fordítómotor átmenetileg nem érhető el (előfordulhat, hogy éppen indul). Várjon egy pillanatot, és próbálja újra.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Rendben",
    L"Mégsem",
};

// 17. Romanian (ro)
const LocalizedStrings kStringsRomanian = {
    L"Stare: activ (F9: pauză)",
    L"Stare: întrerupt (F9: reia)",
    L"Motor de traducere",
    L"Google Translate (gratuit / fără instalare)",
    L"Motor local încorporat (Hy-MT2-1.8B offline)",
    L"Limba sursă (intrare)",
    L"Limba țintă (ieșire)",
    L"Schimbă sursa ⇄ ținta (dublu clic)",
    L"Trimitere automată cu Enter",
    L"Semnal sonor (tonuri)",
    L"Insignă flotantă vizibilă",
    L"Pornește cu Windows",
    L"Ghid de scurtături și ajutor...",
    L"Închide Emebala Chat",
    L"Despre Emebala Chat…",
    L"Emebala Chat — scurtături și ghid",
    L"Scurtături și ghid de utilizare pentru Emebala Chat:\n\n"
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
    L"Despre Emebala Chat",
    L"Activ",
    L"Se traduce...",
    L"Întrerupt",
    L"Emebala Chat",
    L"Nu s-a putut copia textul selectat. Verificați aplicația țintă și încercați din nou.",
    L"Nu este selectat niciun text de tradus.",
    L"Detectare automată",
    L"Emebala Chat rulează deja în fundal.\nVerificați zona de notificări.",
    L"Inicializarea COM a eșuat.\nInsigna flotantă și citirea textului vor fi indisponibile,\ndar traducerea, scurtăturile, tava și sunetele continuă să funcționeze.",
    L"Gata cu copierea și lipirea. Tastați natural în limba maternă — traducerea vă înlocuiește tastarea în timp real în orice aplicație Windows.",
    L"⚡ Trage pentru a traduce — selectează text în orice aplicație, iconița flotantă traduce instant.",
    L"🔊 TTS neuronal — pronunță toate cele 37 de limbi prin pachetele de voci Windows instalate.",
    L"🔒 100% pe dispozitiv și privat — activ doar cât timp este ținută scurtătura; clipboard neatins.",
    L"În anul 2000 î.Hr., scribii mesopotamieni numeau «Eme-bala» pe aceia care prefac limba în punte între lumi.",
    L"Site web",
    L"Contact",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Program 10:00–19:00 KST",
    L"Arhitect principal: Yongtai Kim",
    L"✓ Copiat!",
    L"📋 Copiază",
    L"🔊 Voce",
    L"Limba interfeței",
    L"Auto (limba sistemului)",
    L"Resetează la valorile implicite de sistem",
    L"Valori implicite restaurate",
    L"Tastare de la tastatură",
    L"Sfat de glisare",
    L"Emebala Chat",
    L"Nu există o voce Windows instalată pentru această limbă. Faceți clic din nou pe 🔊 pentru a deschide setările Vocii.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Notificare de confidențialitate",
    L"Iată principiile de confidențialitate ale Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat nu operează niciun server propriu.\n"
    L"• Cu modelul local, textul tradus nu părăsește dispozitivul.\n"
    L"• Dacă alegi Google Translate sau se comută automat în cloud, textul selectat sau tastat este trimis direct la Google pentru traducere, fără a trece prin Emebala.\n"
    L"• Jurnalele de diagnostic sunt IMPLICIT DEZACTIVATE; le activezi din setări (consimțământ).\n"
    L"• Dacă nu doriți să trimiteți text în cloud (Google), deschideți meniul pictogramei din bara de sistem, alegeți “Motor de traducere” și apoi “Motor local încorporat”. Fără model local instalat și cu preluarea în cloud dezactivată, traducerea nu rulează — nimic nu este trimis.\n"
    L"\n"
    L"Detaliile complete sunt în fișierul README, recitibil oricând.\n",
    L"Fișier de configurare: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Textul era prea lung: au fost traduse doar începutul și sfârșitul.",
    L"Există text nou netradus mai sus. Plasați cursorul la sfârșitul acelui rând și apăsați Enter pentru a traduce.",
    L"Se repară componentele motorului local…",
    L"Traducerea locală nu este disponibilă",
    L"Fișierele motorului de traducere local lipsesc, deci traducerea este întreruptă. Reinstalați Emebala Chat pentru a restabili motorul local, sau treceți la traducerea în cloud (Google) alegând „Google Translate” din meniul barei de sistem, „Motor de traducere”.",
    L"Compatibil OpenAI (server personalizat)…",
        L"Setări motor compatibil OpenAI",
        L"Setări motor compatibil OpenAI…",
        L"URL de bază",
        L"Cheie API",
        L"Model",
        L"Preia lista de modele",
        L"Lista de modele nu a putut fi preluată. Puteți tasta direct numele modelului.",
        L"Conexiune nesecurizată (HTTP)",
        L"URL-ul de bază folosește HTTP (necriptat). Cheia API și textul dvs. vor fi trimise în clar. Continuați?",
        L"Setări compatibile OpenAI salvate.",
        L"Cheie salvată: ",
        L"URL-ul de bază nu este valid. Exemplu: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Model specificat de utilizator (.gguf)",
    L"Înregistrează alt model .gguf…",
        L"Notă despre calitatea traducerii",
        L"Modelul selectat nu este Hy-MT2. Versiunea actuală folosește promptul exclusiv Hy-MT2, deci calitatea traducerii cu acest model nu este garantată. Continuați?",
        L"Model înregistrat",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Modelul selectat a fost înregistrat la motorul local.\n"
    L"\n"
    L"Acest model este folosit pentru traducere când alegeți \"Selectarea motorului de traducere > Alegerea utilizatorului (.gguf)\".\n"
    L"\n"
    L"Când intră în vigoare: cel mult aproximativ 1 minut după înregistrare (după ieșirea motorului din inactivitate)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Acest model este deja integrat în Emebala Chat. Înregistrarea nu este necesară. Poți selecta direct motorul de traducere local integrat.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(neregistrat)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Gestionare modele…",
    L"Manager modele utilizator",
    L"Nu există modele de utilizator înregistrate.",
    L"Redenumește…",
    L"Șterge…",
    L"Închide",
    L"Ștergere înregistrare model",
    L"Înregistrarea modelului selectat va fi ștearsă.\n"
    L"\n"
    L"Fișierul modelului (.gguf) rămâne pe disc și nu este șters. Dacă acest model era utilizat, selectarea motorului de traducere revine la Automat (Auto).\n"
    L"\n"
    L"Continuați?",
    L"Redenumește modelul",
    L"Introduceți un nume nou (max. 64 de caractere, fără spații).",
    L"Acest nume nu poate fi folosit. Introduceți un nume nevid, diferit de cele existente, fără spații sau separatoare de cale, de maximum 64 de caractere.",
    L"Modificări salvate.",

    L"registry.json nu a putut fi serializat (un nume de fișier a fost respins). Nu s-a modificat nimic.",
    L"%LOCALAPPDATA% nu este disponibil; nu se poate localiza directorul de modele partajat.",
    L"registry.json nu a putut fi scris.",
    L"registry.json nu a putut fi scris complet.",
    L"registry.json este deteriorat sau are un schemă neacceptată. NU a fost modificat. Reparați-l sau eliminați-l și încercați din nou.",
    L"Traducerea a eșuat. Încercați din nou.",
    L"Motorul de traducere local este temporar indisponibil (se poate inițializa). Așteptați un moment și încercați din nou.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Confirmare",
    L"Anulare",
};

// 18. Swedish (sv)
const LocalizedStrings kStringsSwedish = {
    L"Status: aktiv (F9: paus)",
    L"Status: pausad (F9: återuppta)",
    L"Översättningsmotor",
    L"Google Översätt (gratis / utan installation)",
    L"Inbyggd lokal motor (Hy-MT2-1.8B offline)",
    L"Källspråk (inmatning)",
    L"Målspråk (utmatning)",
    L"Byt källa ⇄ mål (dubbelklick)",
    L"Skicka automatiskt med Enter",
    L"Ljudåterkoppling (toner)",
    L"Flytande bricka synlig",
    L"Starta med Windows",
    L"Snabbtangentsguide och hjälp...",
    L"Avsluta Emebala Chat",
    L"Om Emebala Chat…",
    L"Emebala Chat — snabbtangenter och guide",
    L"Snabbtangenter och guide för Emebala Chat:\n\n"
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
    L"Om Emebala Chat",
    L"Aktiv",
    L"Översätter...",
    L"Pausad",
    L"Emebala Chat",
    L"Kunde inte kopiera den markerade texten. Kontrollera målappen och försök igen.",
    L"Ingen text är markerad att översätta.",
    L"Identifiera automatiskt",
    L"Emebala Chat körs redan i bakgrunden.\nKontrollera meddelandefältet.",
    L"COM-initieringen misslyckades.\nDen flytande brickan och uppläsning är inte tillgängliga,\nmen översättning, snabbtangenter, aktivitetsfält och ljud fungerar fortfarande.",
    L"Inget mer kopierande. Skriv naturligt på ditt modersmål — översättningen ersätter din inskrift i realtid i alla Windows-appar.",
    L"⚡ Dra-för-att-översätta — markera text i vilken app som helst, den flytande ikonen översätter direkt.",
    L"🔊 Neuronalt TTS — talar alla 37 språk via installerade Windows-röstpaket.",
    L"🔒 100 % på enheten och privat — aktivt endast medan snabbtangenten hås; urklipp orört.",
    L"År 2000 f.Kr. kallade mesopotamiska skribenter «Eme-bala» — dem som gör språket till en bro mellan världar.",
    L"Webbplats",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Kontorstider 10:00–19:00 KST",
    L"Leadande arkitekt: Yongtai Kim",
    L"✓ Kopierad!",
    L"📋 Kopiera",
    L"🔊 Röst",
    L"Gränssnittsspråk",
    L"Auto (systemspråk)",
    L"Återställ till systemstandard",
    L"Standardvärden återställda",
    L"Tangentbordsskrivning",
    L"Dra-tooltip",
    L"Emebala Chat",
    L"Ingen Windows-röst är installerad för det här språket. Klicka på 🔊 igen för att öppna Tal-inställningarna.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Integritetsinformation",
    L"Så här hanterar Emebala Chat din integritet.\n"
    L"\n"
    L"• Emebala Chat driver inga egna servrar.\n"
    L"• Med lokal modell lämnar den översatta texten aldrig enheten.\n"
    L"• Om du väljer Google Översätt, eller om molnläge aktiveras automatiskt, skickas den markerade eller inmatade texten direkt till Google för översättning – inte via Emebala.\n"
    L"• Diagnossloggar är AV som standard; du slår på dem i inställningarna (opt-in).\n"
    L"• Om du inte vill skicka text till molnet (Google) öppnar du menyn från aktivitetsikonet, väljer “Översättningsmotor” och sedan “Inbyggd lokal motor”. Utan lokal modell och med molnfallback inaktiverat körs ingen översättning — ingenting skickas.\n"
    L"\n"
    L"Alla detaljer finns i README-filen, som du kan läsa om när som helst.\n",
    L"Konfigurationsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Texten var för lång: endast början och slutet översattes.",
    L"Det finns ny oöversatt text ovanför. Placera markören i slutet av den raden och tryck på Enter för att översätta.",
    L"Reparerar de lokala motorkomponenterna…",
    L"Lokal översättning är inte tillgänglig",
    L"Filerna för den lokala översättningsmotorn saknas, så översättningen har pausats. Installera om Emebala Chat för att återställa den lokala motorn, eller byt till molnöversättning (Google) genom att välja “Google Översätt” i menyn för systemfältet, “Översättningsmotor”.",
    L"OpenAI-kompatibel (anpassad server)…",
        L"Inställningar för OpenAI-kompatibel motor",
        L"Inställningar för OpenAI-kompatibel motor…",
        L"Bas-URL",
        L"API-nyckel",
        L"Modell",
        L"Hämta modellista",
        L"Modellistan kunde inte hämtas. Du kan skriva modellnamnet direkt.",
        L"Oskyddad anslutning (HTTP)",
        L"Bas-URL:en använder HTTP (okrypterat). Din API-nyckel och text skickas i klartext. Fortsätt?",
        L"OpenAI-kompatibla inställningar sparade.",
        L"Sparad nyckel: ",
        L"Bas-URL:en är ogiltig. Exempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Användarspecificerad modell (.gguf)",
    L"Registrera en annan .gguf-modell…",
        L"Meddelande om översättningskvalitet",
        L"Den valda modellen är inte Hy-MT2. Den aktuella versionen använder prompten endast för Hy-MT2, så översättningskvaliteten med den här modellen garanteras inte. Fortsätt?",
        L"Modell registrerad",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Den valda modellen har registrerats hos den lokala motorn.\n"
    L"\n"
    L"Den här modellen används för översättning när du väljer \"Välj översättningsmotor > Användarval (.gguf)\".\n"
    L"\n"
    L"När den träder i kraft: högst cirka 1 minut efter registreringen (efter att motorn avslutats vid inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Den här modellen är redan inbyggd i Emebala Chat. Ingen registrering behövs. Välj den inbyggda lokala översättningsmotorn direkt.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ej registrerad)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Hantera modeller…",
    L"Hanterare av användarmodeller",
    L"Inga användarmodeller registrerade.",
    L"Byt namn…",
    L"Ta bort…",
    L"Stäng",
    L"Ta bort modellregistrering",
    L"Registreringen av den valda modellen tas bort.\n"
    L"\n"
    L"Modellfilen (.gguf) kvarstår på disken och tas inte bort. Om den här modellen användes återgår valet av översättningsmotor till Automatiskt (Auto).\n"
    L"\n"
    L"Fortsätta?",
    L"Byt namn på modell",
    L"Ange ett nytt namn (max 64 tecken, utan mellanslag).",
    L"Det namnet kan inte användas. Ange ett icke-tomt namn som skiljer sig från befintliga, utan mellanslag eller sökvägsavgränsare, på högst 64 tecken.",
    L"Ändringar sparade.",

    L"registry.json kunde inte serialiseras (ett filnamn avvisades). Inget har ändrats.",
    L"%LOCALAPPDATA% är inte tillgängligt; den delade modellmappen kunde inte hittas.",
    L"registry.json kunde inte skrivas.",
    L"registry.json kunde inte skrivas fullständigt.",
    L"registry.json är skadad eller har ett schema som inte stöds. Den har INTE ändrats. Reparera eller ta bort den och försök igen.",
    L"Översättningen misslyckades. Försök igen.",
    L"Den lokala översättningsmotorn är tillfälligt otillgänglig (den kan starta). Vänta en stund och försök igen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Bekräfta",
    L"Avbryt",
};

// 19. Danish (da)
const LocalizedStrings kStringsDanish = {
    L"Status: aktiv (F9: pause)",
    L"Status: pause (F9: genoptag)",
    L"Oversættelsesmotor",
    L"Google Oversæt (gratis / uden installation)",
    L"Indbygget lokal motor (Hy-MT2-1.8B offline)",
    L"Kildesprog (input)",
    L"Formsprog (output)",
    L"Byt kilde ⇄ mål (dobbeltklik)",
    L"Automatisk afsendelse med Enter",
    L"Lydfeedback (toner)",
    L"Flydende badge synlig",
    L"Start med Windows",
    L"Genvejs oversigt og hjælp...",
    L"Afslut Emebala Chat",
    L"Om Emebala Chat…",
    L"Emebala Chat — genveje og guide",
    L"Genveje og guide til Emebala Chat:\n\n"
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
    L"Om Emebala Chat",
    L"Aktiv",
    L"Oversætter...",
    L"Pause",
    L"Emebala Chat",
    L"Kunne ikke kopiere den markerede tekst. Tjek målappen, og prøv igen.",
    L"Der er ikke markeret tekst at oversætte.",
    L"Registrér automatisk",
    L"Emebala Chat kører allerede i baggrunden.\nTjek notifikationsområdet.",
    L"COM-initialisering mislykkedes.\nDen flydende badge og oplæsning er utilgængelige,\nmen oversættelse, genveje, bakke og lyde fungerer stadig.",
    L"Stop med at kopiere og indsætte. Skriv naturligt på dit modersmål — oversættelsen erstatter din indtastning i realtid i enhver Windows-app.",
    L"⚡ Træk-for-at-oversætte — markér tekst i en vilkårlig app, det flydende ikon oversætter med det samme.",
    L"🔊 Neuralt TTS — udtaler alle 37 sprog via installerede Windows-talepakker.",
    L"🔒 100 % på enheden og privat — aktiv kun mens genvejen holdes; udklipsholder urørt.",
    L"I år 2000 f.Kr. kaldte mesopotamiske skriver «Eme-bala» — dem der gør sproget til en bro mellem verdener.",
    L"Hjemmeside",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Åbningstider 10:00–19:00 KST",
    L"Leadende arkitekt: Yongtai Kim",
    L"✓ Kopieret!",
    L"📋 Kopiér",
    L"🔊 Læs op",
    L"Grænsefladesprog",
    L"Auto (system sprog)",
    L"Nulstil til systemstandarder",
    L"Standarder gendannet",
    L"Tastaturindtastning",
    L"Drag-værktøjstip",
    L"Emebala Chat",
    L"Der er ikke installeret en Windows-tale til dette sprog. Klik på 🔊 igen for at åbne Tale-indstillingerne.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Privatlivspolitik",
    L"Sådan behandler Emebala Chat dine data.\n"
    L"\n"
    L"• Emebala Chat driver ingen egen server.\n"
    L"• Ved lokal model forlader den oversatte tekst ikke din enhed.\n"
    L"• Hvis du vælger Google Translate, eller der skiftes automatisk til cloud, sendes den markerede eller indtastede tekst direkte til Google til oversættelse – ikke via Emebala.\n"
    L"• Diagnosedokumentation er SOM STANDARD FRA; du slår den til i indstillingerne (tilvalg).\n"
    L"• Hvis du ikke vil sende tekst til skyen (Google), skal du åbne menuen fra statusfeltikonet, vælge “Oversættelsesmotor” og derefter “Indbygget lokal motor”. Uden en lokal model og med cloud-backup deaktiveret kører oversættelsen ikke — intet sendes.\n"
    L"\n"
    L"Detaljerne står i README-filen, som du kan læse når som helst.\n",
    L"Konfigurationsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Teksten var for lang: kun starten og slutningen blev oversat.",
    L"Der er ny uoversat tekst ovenfor. Placer markøren i slutningen af den linje, og tryk på Enter for at oversætte.",
    L"Reparerer de lokale motorkomponenter…",
    L"Lokal oversættelse er ikke tilgængelig",
    L"Filerne til den lokale oversættelsesmotor mangler, så oversættelsen er sat på pause. Geninstaller Emebala Chat for at gendanne den lokale motor, eller skift til sky-oversættelse (Google) ved at vælge “Google Oversæt” fra bakkemenuen under “Oversættelsesmotor”.",
    L"OpenAI-kompatibel (brugerdefineret server)…",
        L"Indstillinger for OpenAI-kompatibel motor",
        L"Indstillinger for OpenAI-kompatibel motor…",
        L"Basis-URL",
        L"API-nøgle",
        L"Model",
        L"Hent modelliste",
        L"Modellisten kunne ikke hentes. Du kan skrive modelnavnet direkte.",
        L"Usikker forbindelse (HTTP)",
        L"Basis-URL'en bruger HTTP (ukrypteret). Din API-nøgle og tekst sendes i klartekst. Fortsæt?",
        L"OpenAI-kompatible indstillinger gemt.",
        L"Gemt nøgle: ",
        L"Basis-URL'en er ugyldig. Eksempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Brugerspecificeret model (.gguf)",
    L"Registrér en anden .gguf-model…",
        L"Meddelelse om oversættelseskvalitet",
        L"Den valgte model er ikke Hy-MT2. Den aktuelle version bruger kun prompten til Hy-MT2, så oversættelseskvaliteten med denne model kan ikke garanteres. Fortsæt?",
        L"Model registreret",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Den valgte model er registreret hos den lokale motor.\n"
    L"\n"
    L"Denne model bruges til oversættelse, når du vælger \"Valg af oversættelsesmotor > Brugervalgt (.gguf)\".\n"
    L"\n"
    L"Hvornår den træder i kraft: senest ca. 1 minut efter registreringen (efter motorens afslutning ved inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Denne model er allerede indbygget i Emebala Chat. Registrering er ikke nødvendig. Vælg den indbyggede lokale oversættelsesmotor direkte.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ikke registreret)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Administrer modeller…",
    L"Administration af brugermodeller",
    L"Ingen brugermodeller registreret.",
    L"Omdøb…",
    L"Slet…",
    L"Luk",
    L"Slet modelregistrering",
    L"Registreringen af den valgte model slettes.\n"
    L"\n"
    L"Modelfilen (.gguf) beholdes på disken og slettes ikke. Hvis denne model var i brug, vender valget af oversættelsesmotor tilbage til Automatisk (Auto).\n"
    L"\n"
    L"Fortsæt?",
    L"Omdøb model",
    L"Indtast et nyt navn (maks. 64 tegn, uden mellemrum).",
    L"Det navn kan ikke bruges. Indtast et ikke-tomt navn, der adskiller sig fra eksisterende, uden mellemrum eller stiangrænsesymboler, på højst 64 tegn.",
    L"Ændringer gemt.",

    L"registry.json kunne ikke serialiseres (et filnavn blev afvist). Intet blev ændret.",
    L"%LOCALAPPDATA% er ikke tilgængelig; den delte modelmappe kunne ikke findes.",
    L"registry.json kunne ikke skrives.",
    L"registry.json kunne ikke skrives færdig.",
    L"registry.json er beskadiget eller har et ikke-understøttet skema. Den er IKKE ændret. Reparér eller fjern den, og prøv igen.",
    L"Oversættelsen mislykkedes. Prøv igen.",
    L"Den lokale oversættelsesmotor er midlertidigt utilgængelig (den er muligvis ved at starte). Vent et øjeblik og prøv igen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Bekræft",
    L"Annuller",
};

// 20. Finnish (fi)
const LocalizedStrings kStringsFinnish = {
    L"Tila: aktiivinen (F9: tauko)",
    L"Tila: keskeytetty (F9: jatka)",
    L"Käännösmoottori",
    L"Google Kääntäjä (ilmainen / ei asennusta)",
    L"Sisäänrakennettu paikallinen moottori (Hy-MT2-1.8B offline)",
    L"Lähd kieli (syöte)",
    L"Koh kieli (tuloste)",
    L"Vaihda lähde ⇄ kohde (kaksoiskautistus)",
    L"Automaattinen lähetys Enterillä",
    L"Äänipalaute (sävelmät)",
    L"Kelluva merkki näkyy",
    L"Käynnistä Windowsin mukana",
    L"Pikanäppäinopas ja ohje...",
    L"Sulje Emebala Chat",
    L"Tietoja Emebala Chatista…",
    L"Emebala Chat — pikanäppäimet ja opas",
    L"Emebala Chatin pikanäppäimet ja käyttöopas:\n\n"
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
    L"Tietoja Emebala Chatista",
    L"Aktiivinen",
    L"Kääntää...",
    L"Keskeytetty",
    L"Emebala Chat",
    L"Valitun tekstin kopiointi epäonnistui. Tarkista kohdesovellus ja yritä uudelleen.",
    L"Käännettävää tekstiä ei ole valittu.",
    L"Tunnista automaattisesti",
    L"Emebala Chat on jo käynnissä taustalla.\nTarkista ilmoitusalue.",
    L"COM-käynnistys epäonnistui.\nKelluva merkki ja puheentuotto eivät ole käytettävissä,\nmutta käännös, pikanäppäimet, palkki ja äänet toimivat edelleen.",
    L"Liitä kopiointi ja liittäminen luonnollisesti aidolla kielelläsi — käännös korvaa kirjoituksesi reaaliajassa missä tahansa Windows-sovelluksessa.",
    L"⚡ Vedä kääntääksesi — valitse teksti missä tahansa sovelluksessa, kelluva kuvake kääntää heti.",
    L"🔊 Neuro TTS — puhuu kaikki 37 kieltä asennettujen Windows-äänipakettien avulla.",
    L"🔒 100 % laitteella ja yksityinen — aktiivinen vain pikanäppäintä pidettäessä; leikepöytää ei kosketa.",
    L"Vuonna 2000 eaa. mesopotamialaiset kirjurit kutsuivat kielen maailmat sillaksi kääntäviä «Eme-bala».",
    L"Verkkosivusto",
    L"Yhteys",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Toimistoaika 10:00–19:00 KST",
    L"Johtava arkkitehti: Yongtai Kim",
    L"✓ Kopioitu!",
    L"📋 Kopioi",
    L"🔊 Puhe",
    L"Käyttöliittymän kieli",
    L"Auto (järjestelmän kieli)",
    L"Palauta järjestelmän oletukset",
    L"Oletukset palautettu",
    L"Näppäimistökirjoitus",
    L"Veto-työkaluvihje",
    L"Emebala Chat",
    L"Tälle kielelle ei ole asennettu Windows-ääntä. Napsauta 🔊 uudelleen avataksesi Puhe-asetukset.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Tietosuojailmoitus",
    L"Nämä ovat Emebala Chatin tietosuojaperiaatteet.\n"
    L"\n"
    L"• Emebala Chatilla ei ole omia palvelimia.\n"
    L"• Paikallista mallia käytettäessä käännettävä teksti ei poistu laitteesta.\n"
    L"• Jos valitset Google-kääntäjän tai tila vaihtuu automaattisesti pilveen, valittu tai kirjoitettu teksti lähetetään suoraan Googlelle käännöstä varten – ei Emebalan kautta.\n"
    L"• Vianmäärityslokit ovat oletuksena POIS PAALTA; ota ne käyttöön asetuksissa (valinta).\n"
    L"• Jos et halua lähettää tekstiä pilveen (Google), avaa tehtäväpalkin kuvakkeen valikko, valitse “Käännösmoottori” ja sitten “Sisäänrakennettu paikallinen moottori”. Ilman paikallista mallia ja kun pilvivarajärjestelmä on pois päältä, käännös ei toimi — mitään ei lähetetä.\n"
    L"\n"
    L"Tarkat tiedot ovat README-tiedostossa, jonka voit lukea milloin tahansa.\n",
    L"Asetustiedosto: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Teksti oli liian pitkä: vain alku ja loppu käännettiin.",
    L"Yläpuolella on uutta kääntämätöntä tekstiä. Aseta kohdistin rivin loppuun ja paina Enter kääntääksesi.",
    L"Korjataan paikallisia moottorikomponentteja…",
    L"Paikallinen käännös ei ole käytettävissä",
    L"Paikallisen käännösmoottorin tiedostoja ei löydy, joten käännös on keskeytetty. Asenna Emebala Chat uudelleen palauttaaksesi paikallisen moottorin, tai vaihda pilvikäännökseen (Google) valitsemalla “Google Kääntäjä” ilmoitusalueen valikosta, “Käännösmoottori”.",
    L"OpenAI-yhteensopiva (mukautettu palvelin)…",
        L"OpenAI-yhteensopivan moottorin asetukset",
        L"OpenAI-yhteensopivan moottorin asetukset…",
        L"Perus-URL",
        L"API-avain",
        L"Malli",
        L"Hae malliluettelo",
        L"Malliluetteloa ei voitu hakea. Voit kirjoittaa mallin nimen suoraan.",
        L"Suojaamaton yhteys (HTTP)",
        L"Perus-URL käyttää HTTP:tä (salaamaton). API-avaimesi ja tekstisi lähetetään salaamattomana. Jatketaanko?",
        L"OpenAI-yhteensopivat asetukset tallennettu.",
        L"Tallennettu avain: ",
        L"Perus-URL on virheellinen. Esimerkki: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Käyttäjän määrittämä malli (.gguf)",
    L"Rekisteröi toinen .gguf-malli…",
        L"Huomautus käännöksen laadusta",
        L"Valittu malli ei ole Hy-MT2. Nykyinen versio käyttää vain Hy-MT2:lle tarkoitettua promptia, joten tämän mallin käännöksen laatua ei taata. Jatketaanko?",
        L"Malli rekisteröity",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Valittu malli on rekisteröity paikalliseen moottoriin.\n"
    L"\n"
    L"Tätä mallia käytetään kääntämiseen, kun valitset \"Käännösmoottorin valinta > Käyttäjän valinta (.gguf)\".\n"
    L"\n"
    L"Milloin voimaan: viimeistään noin 1 minuutin kuluttua rekisteröinnistä (moottorin ollessa sulkeutunut toimettomuuden vuoksi)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Tämä malli on jo sisäänrakennettu Emebala Chatiin. Rekisteröintiä ei tarvita. Valitse sisäänrakennettu paikallinen käännösmoottori suoraan.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ei rekisteröity)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Hallitse malleja…",
    L"Käyttäjämallien hallinta",
    L"Ei rekisteröityjä käyttäjämalleja.",
    L"Nimeä uudelleen…",
    L"Poista…",
    L"Sulje",
    L"Poista mallin rekisteröinti",
    L"Valitun mallin rekisteröinti poistetaan.\n"
    L"\n"
    L"Mallitiedosto (.gguf) säilyy levyllä eikä poisteta. Jos tätä mallia käytettiin, käännösmoottorin valinta palaa automaattiseen (Auto).\n"
    L"\n"
    L"Jatka?",
    L"Nimeä malli uudelleen",
    L"Syötä uusi nimi (enintään 64 merkkiä, ei välilyöntejä).",
    L"Tätä nimeä ei voi käyttää. Syötä nimi, joka ei ole tyhjä, eroaa olemassa olevista eikä sisällä välilyöntejä tai polkuerottimia, enintään 64 merkkiä.",
    L"Muutokset tallennettu.",

    L"registry.jsonia ei voitu serialisoida (tiedostonimi hylättiin). Mitään ei muutettu.",
    L"%LOCALAPPDATA% ei ole käytettävissä; jaettua mallikansiota ei löydy.",
    L"registry.jsonia ei voitu kirjoittaa.",
    L"registry.jsonia ei voitu kirjoittaa loppuun.",
    L"registry.json on vioittunut tai sen skeemaa ei tueta. Sitä EI muutettu. Korjaa tai poista se ja yritä uudelleen.",
    L"Käännös epäonnistui. Yritä uudelleen.",
    L"Paikallinen käännösmoottori on tilapäisesti EI käytettävissä (se saattaa olla käynnistymässä). Odota hetki ja yritä uudelleen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Vahvista",
    L"Peruuta",
};

// 21. Norwegian (no / nb)
const LocalizedStrings kStringsNorwegian = {
    L"Status: aktiv (F9: pause)",
    L"Status: pause (F9: gjenoppta)",
    L"Oversettelsesmotor",
    L"Google Oversetter (gratis / uten installasjon)",
    L"Innebygd lokal motor (Hy-MT2-1.8B offline)",
    L"Kildespråk (inndata)",
    L"Målspråk (utdata)",
    L"Bytt kilde ⇄ mål (dobbeltklikk)",
    L"Automatisk sending med Enter",
    L"Lydtilbakemelding (toner)",
    L"Svevende merke synlig",
    L"Start med Windows",
    L"Snarveisveiledning og hjelp...",
    L"Avslutt Emebala Chat",
    L"Om Emebala Chat…",
    L"Emebala Chat — snarveier og veiledning",
    L"Snarveier og veiledning for Emebala Chat:\n\n"
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
    L"Om Emebala Chat",
    L"Aktiv",
    L"Oversetter...",
    L"Pause",
    L"Emebala Chat",
    L"Kunne ikke kopiere den valgte teksten. Sjekk målappen og prøv igjen.",
    L"Ingen tekst er valgt for oversettelse.",
    L"Oppdag automatisk",
    L"Emebala Chat kjører allerede i bakgrunnen.\nSjekk varslingsfeltet.",
    L"COM-initialisering mislyktes.\nDet svevende merket og taleavspilling er utilgjengelige,\nmen oversettelse, snarveier, varleske og lyder fungerer fortsatt.",
    L"Slutt å kopiere og lime inn. Skriv naturally på morsmålet ditt — oversettelsen erstatter tastene dine i sanntid i enhver Windows-app.",
    L"⚡ Dra-for-å-oversette — merk tekst i en vilkårlig app, det svevende ikonet oversetter umiddelbart.",
    L"🔊 Nøyalt TTS — uttaler alle 37 språk via installerte Windows-talepakker.",
    L"🔒 100 % på enheten og privat — aktivt kun mens snarveien holdes; utklippstavla urørt.",
    L"I år 2000 f.Kr. kalde mesopotamiske skrivere «Eme-bala» — de som gjør språket til ei bru mellom verdener.",
    L"Nettside",
    L"Kontakt",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Åpningstider 10:00–19:00 KST",
    L"Ledende arkitekt: Yongtai Kim",
    L"✓ Kopiert!",
    L"📋 Kopier",
    L"🔊 Tale",
    L"Grensesnittspråk",
    L"Auto (systemspråk)",
    L"Tilbakestill til systemstandarder",
    L"Standarder gjenopprettet",
    L"Tastaturskriving",
    L"Dra-verktøytips",
    L"Emebala Chat",
    L"Ingen Windows-stemme er installert for dette språket. Klikk på 🔊 igjen for å åpne Tale-innstillingene.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Personvernerklæring",
    L"Dette er personvernprinsippene til Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat driver ingen egne servere.\n"
    L"• Med lokal modell forlater den oversatte teksten ikke enheten.\n"
    L"• Hvis du velger Google Oversett, eller det bytter automatisk til sky, sendes markert eller skrevet tekst direkte til Google for oversettelse – ikke via Emebala.\n"
    L"• Diagnosticslogger er AV som standard; du slår dem på i innstillingene (opt-in).\n"
    L"• Hvis du ikke vil sende tekst til skyen (Google), åpne menyen fra systemstatusfeltets ikon, velg “Oversettelsesmotor” og deretter “Innebygd lokal motor”. Uten lokal modell og med sky-reserve deaktivert, kjører ikke oversettelsen — ingenting sendes.\n"
    L"\n"
    L"Detaljene finnes i README-filen, som kan leses på nytt når som helst.\n",
    L"Konfigurasjonsfil: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Teksten var for lang: bare starten og slutten ble oversatt.",
    L"Det er ny uoversatt tekst ovenfor. Plasser markøren på slutten av den linjen og trykk Enter for å oversette.",
    L"Reparerer de lokale motorkomponentene…",
    L"Lokal oversettelse er ikke tilgjengelig",
    L"Filene til den lokale oversettelsesmotoren mangler, så oversettelsen er satt på pause. Installer Emebala Chat på nytt for å gjenopprette den lokale motoren, eller bytt til skyoversettelse (Google) ved å velge “Google Oversetter” fra menyen i systemfeltet, “Oversettelsesmotor”.",
    L"OpenAI-kompatibel (tilpasset server)…",
        L"Innstillinger for OpenAI-kompatibel motor",
        L"Innstillinger for OpenAI-kompatibel motor…",
        L"Basis-URL",
        L"API-nøkkel",
        L"Modell",
        L"Hent modelliste",
        L"Modellisten kunne ikke hentes. Du kan skrive modellnavnet direkte.",
        L"Usikker tilkobling (HTTP)",
        L"Basis-URL-en bruker HTTP (ukryptert). API-nøkkelen og teksten din sendes i klartekst. Fortsette?",
        L"OpenAI-kompatible innstillinger lagret.",
        L"Lagret nøkkel: ",
        L"Basis-URL-en er ugyldig. Eksempel: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Brukerspesifisert modell (.gguf)",
    L"Registrer en annen .gguf-modell…",
        L"Varsel om oversettelseskvalitet",
        L"Den valgte modellen er ikke Hy-MT2. Denne versjonen bruker prompten kun for Hy-MT2, så oversettelseskvaliteten med denne modellen kan ikke garanteres. Fortsette?",
        L"Modell registrert",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Den valgte modellen er registrert hos den lokale motoren.\n"
    L"\n"
    L"Denne modellen brukes til oversettelse når du velger \"Velg oversettelsesmotor > Brukervalg (.gguf)\".\n"
    L"\n"
    L"Når den trer i kraft: innen cirka 1 minutt etter registreringen (etter at motoren avsluttes ved inaktivitet)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Denne modellen er allerede innebygd i Emebala Chat. Registrering er ikke nødvendig. Velg den innebygde lokale oversettelsesmotoren direkte.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ikke registrert)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Administrer modeller…",
    L"Administrasjon av brukermodeller",
    L"Ingen brukermodeller registrert.",
    L"Gi nytt navn…",
    L"Slett…",
    L"Lukk",
    L"Slett modellregistrering",
    L"Registreringen av den valgte modellen slettes.\n"
    L"\n"
    L"Modellfilen (.gguf) beholdes på disken og slettes ikke. Hvis denne modellen var i bruk, går valget av oversettelsesmotor tilbake til Automatisk (Auto).\n"
    L"\n"
    L"Fortsett?",
    L"Gi modell nytt navn",
    L"Skriv inn et nytt navn (maks. 64 tegn, uten mellomrom).",
    L"Det navnet kan ikke brukes. Skriv inn et navn som ikke er tomt, er forskjellig fra eksisterende, uten mellomrom eller sti-separatorer, opptil 64 tegn.",
    L"Endringer lagret.",

    L"registry.json kunne ikke serialiseres (et filnavn ble avvist). Ingenting ble endret.",
    L"%LOCALAPPDATA% er ikke tilgjengelig; den delte modellmappen ble ikke funnet.",
    L"registry.json kunne ikke skrives.",
    L"registry.json kunne ikke skrives fullstendig.",
    L"registry.json er skadet eller har et schema som ikke støttes. Den har IKKE blitt endret. Reparer eller fjern den, og prøv igjen.",
    L"Oversettelsen mislyktes. Prøv igjen.",
    L"Den lokale oversettelsesmotoren er midlertidig utilgjengelig (den kan være under oppstart). Vent et øyeblikk og prøv igjen.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Bekreft",
    L"Avbryt",
};

// 22. Greek (el)
const LocalizedStrings kStringsGreek = {
    L"Κατάσταση: ενεργό (F9: παύση)",
    L"Κατάσταση: σε παύση (F9: συνέχεια)",
    L"Μηχανή μετάφρασης",
    L"Google Μετάφραση (δωρεάν / χωρίς εγκατάσταση)",
    L"Ενσωματωμένη τοπική μηχανή (Hy-MT2-1.8B εκτός σύνδεσης)",
    L"Γλώσσα πηγής (είσοδος)",
    L"Γλώσταση προορισμού (έξοδος)",
    L"Εναλλαγή πηγής ⇄ προορισμού (διπλό κλικ)",
    L"Αυτόματη αποστολή με Enter",
    L"Ηχητική ανάδραση (ήχοι)",
    L"Πloating σήμα ορατό",
    L"Έναρξη με τα Windows",
    L"Οδηγός συντομεύσεων και βοήθεια...",
    L"Έξοδος από το Εμεμπάλα Τσατ",
    L"Σχετικά με το Εμεμπάλα Τσατ…",
    L"Εμεμπάλα Τσατ — συντομεύσεις και οδηγός",
    L"Συντομεύσεις και οδηγός χρήσης του Εμεμπάλα Τσατ:\n\n"
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
    L"Σχετικά με το Εμεμπάλα Τσατ",
    L"Ενεργό",
    L"Μετάφραση...",
    L"Σε παύση",
    L"Εμεμπάλα Τσατ",
    L"Δεν ήταν δυνατή η αντιγραφή του επιλεγμένου κειμένου. Ελέγξτε την εφαρμογή προορισμού και δοκιμάστε ξανά.",
    L"Δεν υπάρχει επιλεγμένο κείμενο για μετάφραση.",
    L"Αυτόματος εντοπισμός",
    L"Το Εμεμπάλα Τσατ εκτελείται ήδη στο παρασκήνιο.\nΕλέγξτε το πεδίο ειδοποιήσεων.",
    L"Η αρχικοποίηση COM απέτυχε.\nΤο πloating σήμα και η συνθετική ομιλία δεν θα είναι διαθέσιμα,\nαλλά η μετάφραση, οι συντομεύσεις, το εικονίδιο και οι ήχοι συνεχίζουν να λειτουργούν.",
    L"Σταματήστε το αντιγραφή-επικόλληση. Πληκτρολογήστε φυσικά στη μητρική σας γλώσσα — η μετάφραση αντικαθιστά την πληκτρολόγησή σας σε πραγματικό χρόνο σε οποιαδήποτε εφαρμογή Windows.",
    L"⚡ Σύρε-για-μετάφραση — επιλέξτε κείμενο σε οποιαδήποτε εφαρμογή, το σήμα μεταφράζει αμέσως.",
    L"🔊 Νευρωνικό TTS — προφέρει και τις 37 γλώσσες μέσω εγκατεστημένων πακέτων φωνής των Windows.",
    L"🔒 100% στη συσκευή και ιδιωτικό — ενεργό μόνο όσο κρατιέται η συντόμευση· πρόχειρο ανέπαφο.",
    L"Το 2000 π.Χ., οι Μεσοποτάμιοι γραφείς ονόμαζαν «Eme-bala» όσους έκαναν τη γλώσσα γέφυρα μεταξύ κόσμων.",
    L"Ιστοσελίδα",
    L"Επικοινωνία",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Ώρες γραφείου 10:00–19:00 KST",
    L"Κύριος αρχιτέκτονας: Yongtai Kim",
    L"✓ Αντιγράφηκε!",
    L"📋 Αντιγραφή",
    L"🔊 Ομιλία",
    L"Γλώσσα διεπαφής",
    L"Αυτόματο (γλώσσα συστήματος)",
    L"Επαναφορά στις προεπιλογές συστήματος",
    L"Οι προεπιλογές αποκαταστάθηκαν",
    L"Πληκτρολόγηση",
    L"Υπόδειξη σύρσιμο",
    L"Εμεμπάλα Τσατ",
    L"Δεν είναι εγκατεστημένη φωνή των Windows για αυτή τη γλώσσα. Κάντε ξανά κλικ στο 🔊 για να ανοίξετε τις ρυθμίσεις Ομιλίας.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Γνωστοποίηση απορρήτου",
    L"Αυτές είναι οι αρχές απορρήτου του Emebala Chat.\n"
    L"\n"
    L"• Το Emebala Chat δεν λειτουργεί δικούς του διακομιστές.\n"
    L"• Με τοπικό μοντέλο, το μεταφραζόμενο κείμενο δεν φεύγει από τη συσκευή σας.\n"
    L"• Αν επιλέξετε τη Μετάφραση Google ή γίνει αυτόματη εναλλαγή στο cloud, το επιλεγμένο ή πληκτρολογημένο κείμενο στέλνεται απευθείας στην Google για μετάφραση, όχι μέσω Emebala.\n"
    L"• Τα αρχεία διαγνωστικών είναι ΑΠΕΝΕΡΓΟΠΟΙΗΜΕΝΑ από προεπιλογή· ενεργοποιούνται στις ρυθμίσεις (ρητή συναίνεση).\n"
    L"• Εάν δεν θέλετε να στείλετε κείμενο στο cloud (Google), ανοίξτε το μενού του εικονιδίου στη γραμμή εργασιών, επιλέξτε «Μηχανή μετάφρασης» και στη συνέχεια «Ενσωματωμένη τοπική μηχανή». Χωρίς εγκατεστημένο τοπικό μοντέλο και με απενεργοποιημένη την εφεδρική λειτουργία cloud, η μετάφραση δεν εκτελείται — τίποτα δεν στέλνεται.\n"
    L"\n"
    L"Οι πλήρεις λεπτομέρειες βρίσκονται στο αρχείο README, που διαβάζεται ξανά ανά πάσα στιγμή.\n",
    L"Αρχείο ρυθμίσεων: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Το κείμενο είναι πολύ μακρύ: μεταφράστηκαν μόνο η αρχή και το τέλος.",
    L"Υπάρχει νέο μη μεταφρασμένο κείμενο παραπάνω. Τοποθετήστε τον δείκτη στο τέλος εκείνης της γραμμής και πατήστε Enter για μετάφραση.",
    L"Επισκευή των τοπικών συστατικών μηχανής…",
    L"Η τοπική μετάφραση δεν είναι διαθέσιμη",
    L"Τα αρχεία της τοπικής μηχανής μετάφρασης λείπουν, επομένως η μετάφραση έχει διακοπεί. Εγκαταστήστε ξανά το Emebala Chat για να επαναφέρετε την τοπική μηχανή, ή μεταβείτε σε μετάφραση cloud (Google) επιλέγοντας «Google Μετάφραση» από το μενού της περιοχής ειδοποιήσεων, «Μηχανή μετάφρασης».",
    L"Συμβατό με OpenAI (προσαρμοσμένος διακομιστής)…",
        L"Ρυθμίσεις συμβατής μηχανής OpenAI",
        L"Ρυθμίσεις συμβατής μηχανής OpenAI…",
        L"Base URL",
        L"Κλειδί API",
        L"Μοντέλο",
        L"Λήψη λίστας μοντέλων",
        L"Δεν ήταν δυνατή η λήψη της λίστας μοντέλων. Μπορείτε να πληκτρολογήσετε το όνομα του μοντέλου απευθείας.",
        L"Μη ασφαλής σύνδεση (HTTP)",
        L"Το Base URL χρησιμοποιεί HTTP (μη κρυπτογραφημένο). Το κλειδί API και το κείμενό σας θα σταλούν σε απλή μορφή. Συνέχεια;",
        L"Οι συμβατές ρυθμίσεις OpenAI αποθηκεύτηκαν.",
        L"Αποθηκευμένο κλειδί: ",
        L"Το Base URL δεν είναι έγκυρο. Παράδειγμα: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Μοντέλο καθορισμένο από τον χρήστη (.gguf)",
    L"Καταχώριση άλλου μοντέλου .gguf…",
        L"Ειδοποίηση ποιότητας μετάφρασης",
        L"Το επιλεγμένο μοντέλο δεν είναι Hy-MT2. Η τρέχουσα έκδοση χρησιμοποιεί το prompt μόνο για Hy-MT2, επομένως η ποιότητα μετάφρασης με αυτό το μοντέλο δεν είναι εγγυημένη. Συνέχεια;",
        L"Το μοντέλο καταχωρήθηκε",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Το επιλεγμένο μοντέλο καταχωρήθηκε στην τοπική μηχανή.\n"
    L"\n"
    L"Αυτό το μοντέλο χρησιμοποιείται για μετάφραση όταν επιλέγετε «Επιλογή μηχανής μετάφρασης > Επιλογή χρήστη (.gguf)».\n"
    L"\n"
    L"Πότε ισχύει: έως περίπου 1 λεπτό μετά την καταχώρηση (μετά τον τερματισμό της μηχανής λόγω αδράνειας)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Αυτό το μοντέλο είναι ήδη ενσωματωμένο στο Emebala Chat. Δεν απαιτείται εγγραφή. Μπορείτε να επιλέξετε απευθείας τον ενσωματωμένο τοπικό μηχανισμό μετάφρασης.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(μη καταχωρημένο)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Διαχείριση μοντέλων…",
    L"Διαχείριση μοντέλων χρήστη",
    L"Δεν υπάρχουν εγγεγραμμένα μοντέλα χρήστη.",
    L"Μετονομασία…",
    L"Διαγραφή…",
    L"Κλείσιμο",
    L"Διαγραφή καταχώρισης μοντέλου",
    L"Η καταχώριση του επιλεγμένου μοντέλου θα διαγραφεί.\n"
    L"\n"
    L"Το αρχείο μοντέλου (.gguf) διατηρείται στον δίσκο και δεν διαγράφεται. Αν αυτό το μοντέλο ήταν σε χρήση, η επιλογή μηχανής μετάφρασης επιστρέφει σε Αυτόματο (Auto).\n"
    L"\n"
    L"Συνέχεια;",
    L"Μετονομασία μοντέλου",
    L"Εισαγάγετε νέο όνομα (έως 64 χαρακτήρες, χωρίς κενά).",
    L"Αυτό το όνομα δεν μπορεί να χρησιμοποιηθεί. Εισαγάγετε ένα μη κενό όνομα, διαφορετικό από τα υπάρχοντα, χωρίς κενά ή διαχωριστικά διαδρομής, έως 64 χαρακτήρες.",
    L"Οι αλλαγές αποθηκεύτηκαν.",

    L"Δεν ήταν δυνατή η σειριοποίηση του registry.json (απορρίφθηκε ένα όνομα αρχείου). Δεν άλλαξε τίποτα.",
    L"Το %LOCALAPPDATA% δεν είναι διαθέσιμο· δεν είναι δυνατός ο εντοπισμός του κοινόχρηστου φακέλου μοντέλων.",
    L"Δεν ήταν δυνατή η εγγραφή του registry.json.",
    L"Το registry.json δεν εγγράφηκε πλήρως.",
    L"Το registry.json είναι κατεστραμμένο ή έχει μη υποστηριζόμενο σχήμα. ΔΕΝ τροποποιήθηκε. Επισκευάστε το ή αφαιρέστε το και δοκιμάστε ξανά.",
    L"Η μετάφραση απέτυχε. Δοκιμάστε ξανά.",
    L"Η τοπική μηχανή μετάφρασης είναι προσωρινά μη διαθέσιμη (πιθανώς ξεκινά). Περιμένετε λίγο και δοκιμάστε ξανά.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Εντάξει",
    L"Άκυρο",
};

// 23. Turkish (tr)
const LocalizedStrings kStringsTurkish = {
    L"Durum: etkin (F9: duraklat)",
    L"Durum: duraklatıldı (F9: devam et)",
    L"Çeviri motoru",
    L"Google Çeviri (ücretsiz / kurulum gerektirmez)",
    L"Yerleşik yerel motor (Hy-MT2-1.8B çevrimdışı)",
    L"Kaynak dil (giriş)",
    L"Hedef dil (çıkış)",
    L"Kaynak ⇄ hedef değiştir (çift tık)",
    L"Enter ile otomatik gönder",
    L"Sesli geri bildirim (tonlar)",
    L"Yüzen rozet görünür",
    L"Windows ile başlat",
    L"Kısayol rehberi ve yardım...",
    L"Emebala Chat'ten çık",
    L"Emebala Chat hakkında…",
    L"Emebala Chat — kısayollar ve rehber",
    L"Emebala Chat kısayolları ve kullanım rehberi:\n\n"
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
    L"Emebala Chat hakkında",
    L"Etkin",
    L"Çevriliyor...",
    L"Duraklatıldı",
    L"Emebala Chat",
    L"Seçili metin kopyalanamadı. Hedef uygulamayı denetleyip yeniden deneyin.",
    L"Çevrilecek metin seçilmedi.",
    L"Otomatik algıla",
    L"Emebala Chat arka planda zaten çalışıyor.\nSistem bildirim tepsisini denetleyin.",
    L"COM başlatma başarısız.\nYüzen rozet ve metinden sese kullanılamayacak,\nancak çeviri, kısayollar, tepsi ve sesler çalışmaya devam ediyor.",
    L"Kopyala-yapıştırı bırakın. Ana dilinizde doğal yazın — çeviri, herhangi bir Windows uygulamasında yazdıklarınızı gerçek zamanlı değiştirir.",
    L"⚡ Sürükle-çevir — herhangi bir uygulamada metni seçin, yüzen simge anında çevirir.",
    L"🔊 Nöral TTS — yüklü Windows ses paketleriyle 37 dilin tamamını seslendirir.",
    L"🔒 %100 cihazda ve özel — yalnızca kısayol basılıyken etkin; panoya dokunulmaz.",
    L"MÖ 2000'de Mezopotamyalı kâtipler, dili dünyalar arasında köprüye çevirenlere «Eme-bala» derdi.",
    L"Web sitesi",
    L"İletişim",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Çalışma saatleri 10:00–19:00 KST",
    L"Baş mimar: Yongtai Kim",
    L"✓ Kopyalandı!",
    L"📋 Kopyala",
    L"🔊 Ses",
    L"Arayüz dili",
    L"Otomatik (sistem dili)",
    L"Sistem varsayılanlarına sıfırla",
    L"Varsayılanlar geri yüklendi",
    L"Klavye yazımı",
    L"Sürükle ipucu",
    L"Emebala Chat",
    L"Bu dil için yüklü bir Windows sesi yok. Konuşma ayarlarını açmak için 🔊 simgesine yeniden tıklayın.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Gizlilik uyarısı",
    L"Emebala Chat gizlilik ilkeleri şöyledir:\n"
    L"\n"
    L"• Emebala Chat kendi sunucularını işletmez.\n"
    L"• Yerel model kullanıldığında çevrilen metin cihazınızdan çıkmaz.\n"
    L"• Google Çeviri seçerseniz veya otomatik buluta geçilirse, seçilen ya da yazılan metin çeviri için doğrudan Google’a gönderilir; Emebala üzerinden geçmez.\n"
    L"• Tanılama günlükleri varsayılan olarak KAPALIDIR; ayarlardan açmanız gerekir (seçmeli onay).\n"
    L"• Metni buluta (Google) göndermek istemiyorsanız sistem tepsisindeki simgenin menüsünü açın, “Çeviri motoru” bölümünden “Yerleşik yerel motor” seçeneğini seçin. Yerel model kurulu değilse ve bulut yedeği kapalıysa çeviri çalışmaz — hiçbir şey gönderilmez.\n"
    L"\n"
    L"Tam ayrıntılar README dosyasındadır; istediğiniz zaman tekrar okuyabilirsiniz.\n",
    L"Yapılandırma dosyası: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Metin çok uzun olduğu için yalnızca başı ve sonu çevrildi.",
    L"Yukarıda çevrilmemiş yeni metin var. Çevirmek için imleci o satırın sonuna getirin ve Enter'a basın.",
    L"Yerel motor bileşenleri onarılıyor…",
    L"Yerel çeviri kullanılamıyor",
    L"Yerel çeviri motoru dosyaları bulunamadığı için çeviri duraklatıldı. Yerel motoru geri yüklemek için Emebala Chat’i yeniden yükleyin veya bulut (Google) çevirisine geçmek için tepsi menüsünden “Çeviri motoru” altında “Google Çeviri” seçin.",
    L"OpenAI uyumlu (özel sunucu)…",
        L"OpenAI Uyumlu Motor Ayarları",
        L"OpenAI Uyumlu Motor Ayarları…",
        L"Temel URL",
        L"API Anahtarı",
        L"Model",
        L"Model listesini getir",
        L"Model listesi alınamadı. Model adını doğrudan yazabilirsiniz.",
        L"Güvenli olmayan bağlantı (HTTP)",
        L"Temel URL HTTP kullanıyor (şifrelenmemiş). API anahtarınız ve metniniz düz metin olarak gönderilecek. Devam edilsin mi?",
        L"OpenAI uyumlu ayarlar kaydedildi.",
        L"Kaydedilen anahtar: ",
        L"Temel URL geçersiz. Örnek: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Kullanıcı tarafından belirtilen model (.gguf)",
    L"Başka bir .gguf modeli kaydet…",
        L"Çeviri kalitesi bildirimi",
        L"Seçilen model Hy-MT2 değil. Mevcut sürüm yalnızca Hy-MT2'ye özel istem kullandığından, bu modelle çeviri kalitesi garanti edilmez. Devam edilsin mi?",
        L"Model kaydedildi",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Seçilen model yerel motora kaydedildi.\n"
    L"\n"
    L"Bu model, \"Çeviri motorunu seç > Kullanıcı seçimi (.gguf)\" seçtiğinizde çeviri için kullanılır.\n"
    L"\n"
    L"Ne zaman etkili olur: kayıttan sonra en çok yaklaşık 1 dakika (motorun boşta kalma sonrası kapanmasından sonra)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Bu model Emebala Chat'e zaten yerleşiktir. Kayıt gerekmez. Yerleşik yerel çeviri motorunu doğrudan seçebilirsiniz.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(kayıtlı değil)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Modelleri yönet…",
    L"Kullanıcı Modeli Yöneticisi",
    L"Kayıtlı kullanıcı modeli yok.",
    L"Yeniden adlandır…",
    L"Sil…",
    L"Kapat",
    L"Model kaydını sil",
    L"Seçilen modelin kaydı silinecek.\n"
    L"\n"
    L"Model dosyası (.gguf) diskte tutulur ve silinmez. Bu model kullanılıyorsa, çeviri motoru seçimi Otomatik (Auto) olarak döner.\n"
    L"\n"
    L"Devam edilsin mi?",
    L"Modeli yeniden adlandır",
    L"Yeni bir ad girin (en fazla 64 karakter, boşluksuz).",
    L"Bu ad kullanılamaz. Boş olmayan, mevcutlardan farklı, boşluk veya yol ayıracı içermeyen, en fazla 64 karakterlik bir ad girin.",
    L"Değişiklikler kaydedildi.",

    L"registry.json serileştirilemedi (bir dosya adı reddedildi). Hiçbir şey değiştirilmedi.",
    L"%LOCALAPPDATA% kullanılamıyor; paylaşılan modeller klasörü bulunamıyor.",
    L"registry.json yazılamadı.",
    L"registry.json tamamen yazılamadı.",
    L"registry.json hasarlı veya desteklenmeyen bir şemaya sahip. DeğiştirilMEdi. Düzeltin veya kaldırın ve yeniden deneyin.",
    L"Çeviri başarısız oldu. Lütfen tekrar deneyin.",
    L"Yerel çeviri motoru geçici olarak kullanılamıyor (başlatılıyor olabilir). Lütfen biraz bekleyip tekrar deneyin.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Tamam",
    L"İptal",
};

// 24. Ukrainian (uk)
const LocalizedStrings kStringsUkrainian = {
    L"Стан: активний (F9: пауза)",
    L"Стан: призупинено (F9: відновити)",
    L"Рушій перекладу",
    L"Google Перекладач (безкоштовно / без встановлення)",
    L"Вбудований локальний рушій (Hy-MT2-1.8B офлайн)",
    L"Мова джерела (вхід)",
    L"Цільова мова (вихід)",
    L"Помняти джерело ⇄ ціль (подвійний клік)",
    L"Автонадсилання Enterом",
    L"Звуковий відгук (тони)",
    L"Плаваюча відзнака видима",
    L"Запускати з Windows",
    L"Гарячі клавіші та довідка...",
    L"Вийти з Емебала Чат",
    L"Про Емебала Чат…",
    L"Емебала Чат — гарячі клавіші та посібник",
    L"Гарячі клавіші та посібник Емебала Чат:\n\n"
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
    L"Про Емебала Чат",
    L"Активний",
    L"Переклад...",
    L"Призупинено",
    L"Емебала Чат",
    L"Не вдалося скопіювати вибраний текст. Перевірте цільовий застосунок і спробуйте ще раз.",
    L"Не вибрано тексту для перекладу.",
    L"Визначати автоматично",
    L"Емебала Чат уже працює у фоні.\nПеревірте область сповіщень.",
    L"Помилка ініціалізації COM.\nПлаваюча відзнака й озвучення тексту будуть недоступні,\nале переклад, гарячі клавіші, трей і звуки продовжують працювати.",
    L"Геть копіювання та вставляння. Друкуйте природно рідною мовою — переклад замінює ваш набір у реальному часі в будь-якій програмі Windows.",
    L"⚡ Перетягни, щоб перекласти — виділіть текст у будь-якій програмі, плаваюча піктограма перекрить миттєво.",
    L"🔊 Нейронне TTS — озвучує всі 37 мов через установлені голосові пакети Windows.",
    L"🔒 100% на пристрої та приватно — активно лише доки утримується скорочення; буфер обміну не чіпается.",
    L"У 2000 р. до н. е. месопотамські писці називали «Eme-bala» тих, хто перетворює мову на міст між світами.",
    L"Вебсайт",
    L"Контакт",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Робочі години 10:00–19:00 KST",
    L"Головний архітектор: Yongtai Kim",
    L"✓ Скопійовано!",
    L"📋 Копіювати",
    L"🔊 Озвучення",
    L"Мова інтерфейсу",
    L"Авто (системна мова)",
    L"Скинути до системних типових",
    L"Типові значення відновлено",
    L"Набирання з клавіатури",
    L"Підказка перетягування",
    L"Емебала Чат",
    L"Для цієї мови не встановлено голос Windows. Натисніть 🔊 ще раз, щоб відкрити параметри мовлення.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Повідомлення про конфіденційність",
    L"Ось принципи обробки даних в Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat не використовує власних серверів.\n"
    L"• З локальною моделлю текст перекладу не покидає вашого пристрою.\n"
    L"• Якщо обрано Google Перекладач або відбувається автоматичний перехід у хмару, виділений або надрукований текст надсилається напряму до Google для перекладу, минаючи Emebala.\n"
    L"• Діагностичні журнали типово ВИМКНЕНО; увімкніть їх у налаштуваннях (за згодою).\n"
    L"• Якщо не хочете надсилати текст у хмару (Google), відкрийте меню піктограми в системному треї, оберіть «Рушій перекладу» і потім «Вбудований локальний рушій». Без встановленої локальної моделі та з вимкненим хмарним резервуванням переклад не виконується — нічого не надсилається.\n"
    L"\n"
    L"Повні відомості — у файлі README, який можна перечитати будь-коли.\n",
    L"Файл налаштувань: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Текст надто довгий: перекладено лише початок і кінець.",
    L"Вище є новий неперекладений текст. Поставте курсор у кінець цього рядка та натисніть Enter, щоб перекласти.",
    L"Відновлення компонентів локального рушія…",
    L"Локальний переклад недоступний",
    L"Файли локального рушія перекладу не знайдено, тому переклад призупинено. Переустановіть Emebala Chat, щоб відновити локальний рушій, або перейдіть на хмарний (Google) переклад, вибравши «Google Перекладач» у меню області сповіщень, «Рушій перекладу».",
    L"Сумісно з OpenAI (користувацький сервер)…",
        L"Налаштування сумісного з OpenAI рушія",
        L"Налаштування сумісного з OpenAI рушія…",
        L"Base URL",
        L"Ключ API",
        L"Модель",
        L"Отримати список моделей",
        L"Не вдалося отримати список моделей. Ви можете ввести назву моделі безпосередньо.",
        L"Незахищене з'єднання (HTTP)",
        L"Base URL використовує HTTP (без шифрування). Ваш ключ API і текст будуть надіслані у відкритому вигляді. Продовжити?",
        L"Сумісні з OpenAI налаштування збережено.",
        L"Збережений ключ: ",
        L"Неприпустимий Base URL. Приклад: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Модель, вказана користувачем (.gguf)",
    L"Зареєструвати іншу модель .gguf…",
        L"Повідомлення про якість перекладу",
        L"Вибрана модель не Hy-MT2. Поточна версія використовує промпт лише для Hy-MT2, тому якість перекладу цією моделлю не гарантується. Продовжити?",
        L"Модель зареєстровано",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Вибрану модель зареєстровано в локальному рушії.\n"
    L"\n"
    L"Ця модель використовується для перекладу, коли ви вибираєте «Вибір рушія перекладу > Вибір користувача (.gguf)».\n"
    L"\n"
    L"Коли набирає чинності: не більше приблизно 1 хвилини після реєстрації (після завершення роботи рушія через бездіяльність)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Ця модель уже вбудована в Emebala Chat. Реєстрація не потрібна. Ви можете напряму вибрати вбудований локальний рушій перекладу.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(не зареєстровано)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Керування моделями…",
    L"Керування моделями користувача",
    L"Немає зареєстрованих моделей користувача.",
    L"Перейменувати…",
    L"Видалити…",
    L"Закрити",
    L"Видалення реєстрації моделі",
    L"Реєстрацію вибраної моделі буде видалено.\n"
    L"\n"
    L"Файл моделі (.gguf) залишається на диску і не видаляється. Якщо цю модель було використано, вибір рушія перекладу повернеться до Авто (Auto).\n"
    L"\n"
    L"Продовжити?",
    L"Перейменування моделі",
    L"Введіть нову назву (до 64 символів, без пробілів).",
    L"Цю назву не можна використати. Введіть непорожню назву, що відрізняється від наявних, без пробілів і роздільників шляху, до 64 символів.",
    L"Зміни збережено.",

    L"Не вдалося серіалізувати registry.json (назву файлу відхилено). Нічого не змінено.",
    L"%LOCALAPPDATA% недоступний; не вдається знайти спільну папку моделей.",
    L"Не вдалося записати registry.json.",
    L"Не вдалося повністю записати registry.json.",
    L"registry.json пошкоджено або має непідтримувану схему. Він НЕ змінений. Відновіть його або видаліть і повторіть спробу.",
    L"Переклад не вдався. Спробуйте ще раз.",
    L"Локальний рушій перекладу тимчасово недоступний (можливо, запускається). Зачекайте трохи й повторіть спробу.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Підтвердити",
    L"Скасувати",
};

// 25. Thai (th)
const LocalizedStrings kStringsThai = {
    L"สถานะ: ใช้งานอยู่ (F9: หยุดชั่วคราว)",
    L"สถานะ: หยุดชั่วคราว (F9: ทำงานต่อ)",
    L"ระบบแปลภาษา",
    L"Google แปลภาษา (ฟรี / ไม่ต้องติดตั้ง)",
    L"เครื่องยนต์ในตัวแบบออฟไลน์ (Hy-MT2-1.8B ออฟไลน์)",
    L"ภาษาต้นทาง (อินพุต)",
    L"ภาษาปลายทาง (เอาต์พุต)",
    L"สลับต้นทาง ⇄ ปลายทาง (ดับเบิลคลิก)",
    L"ส่งอัตโนมัติเมื่อกด Enter",
    L"เสียงตอบกลับ (โทนเสียง)",
    L"แสดงป้ายลอย",
    L"เริ่มทำงานพร้อม Windows",
    L"สรุปคีย์ลัดและวิธีใช้...",
    L"ออกจาก เอเมบาลา แชท",
    L"เกี่ยวกับ เอเมบาลา แชท…",
    L"เอเมบาลา แชท — คีย์ลัดและคู่มือการใช้งาน",
    L"คีย์ลัดและคู่มือการใช้งาน เอเมบาลา แชท:\n\n"
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
    L"เกี่ยวกับ เอเมบาลา แชท",
    L"ใช้งานอยู่",
    L"กำลังแปล...",
    L"หยุดชั่วคราว",
    L"เอเมบาลา แชท",
    L"คัดลอกข้อความที่เลือกไม่ได้ โปรดตรวจสอบแอปปลายทางแล้วลองอีกครั้ง",
    L"ยังไม่ได้เลือกข้อความสำหรับแปล",
    L"ตรวจจับอัตโนมัติ",
    L"เอเมบาลา แชท ทำงานอยู่แล้วในพื้นหลัง\nโปรดตรวจสอบถาดการแจ้งเตือนของระบบ",
    L"ไม่สามารถเริ่มการทำงาน COM ได้\nป้ายลอยและการอ่านออกเสียงจะไม่สามารถใช้งานได้\nแต่การแปล คีย์ลัด ถาดระบบ และเสียงจะยังทำงานตามปกติ",
    L"เลิกลั่บการคัดลอกวาง พิมพ์ตามธรรมชาติในภาษาแม่ของคุณ แล้วคำแปลจะมาแทนที่สิ่งที่คุณพิมพ์แบบเรียลไทม์ในทุกแอปของ Windows",
    L"⚡ ลากเพื่อแปล — เลือกข้อความในแอปใดก็ได้ ไอคอนลอยจะแปลให้ทันที",
    L"🔊 TTS แบบประสาท — ออกเสียงได้ครบทั้ง 37 ภาษาเมื่อติดตั้งชุดเสียงของ Windows",
    L"🔒 ความเป็นส่วนตัว 100% บนอุปกรณ์ — ทำงานเฉพาะตอนกดคีย์ลัดเท่านั้น ไม่แตะคลิปบอร์ด",
    L"ในปี 2000 ปีก่อนคริสตกาล เสมียนเมโสโปเตเมียเรียกผู้ที่เปลี่ยนภาษาให้เป็นสะพานเชื่อมโลกว่า «Eme-bala»",
    L"เว็บไซต์",
    L"ติดต่อ",
    L"Reddit",
    L"Team Sunplaza · โซล ยองดึงโป (ห้อง 219, 65 ยองจุง-โร)",
    L"+82 2 575 0414 · เวลาทำการ 10:00–19:00 KST",
    L"สถาปนิกหลัก: Yongtai Kim",
    L"✓ คัดลอกแล้ว!",
    L"📋 คัดลอก",
    L"🔊 อ่านออกเสียง",
    L"ภาษาอินเทอร์เฟซ",
    L"อัตโนมัติ (ภาษาของระบบ)",
    L"คืนค่าเป็นค่าเริ่มต้นของระบบ",
    L"กู้คืนค่าเริ่มต้นแล้ว",
    L"การพิมพ์ด้วยคีย์บอร์ด",
    L"ทูลทิปแบบลาก",
    L"เอเมบาลา แชท",
    L"ยังไม่ได้ติดตั้งเสียงของ Windows สำหรับภาษานี้ คลิก 🔊 อีกครั้งเพื่อเปิดการตั้งค่าคำพูด",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"ประกาศด้านความเป็นส่วนตัว",
    L"หลักเกณฑ์ด้านความเป็นส่วนตัวของ Emebala Chat มีดังนี้\n"
    L"\n"
    L"• Emebala Chat ไม่มีเซิร์ฟเวอร์ของตนเอง\n"
    L"• เมื่อใช้โมเดลในเครื่อง ข้อความที่แปลจะไม่ออกจากอุปกรณ์ของคุณ\n"
    L"• หากคุณเลือก Google Translate หรือสลับไปใช้ระบบคลาวด์อัตโนมัติ ข้อความที่คุณเลือกหรือพิมพ์จะถูกส่งตรงไปยัง Google เพื่อแปล โดยไม่ผ่าน Emebala\n"
    L"• บันทึกการวินิจฉัยปิด (OFF) เป็นค่าเริ่มต้น ต้องเปิดในการตั้งค่า (เลือกเข้าร่วม)\n"
    L"• หากคุณไม่ต้องการส่งข้อความไปยังคลาวด์ (Google) ให้เปิดเมนูที่ไอคอนถาดระบบ เลือก “ระบบแปลภาษา” แล้วเลือก “เครื่องยนต์ในตัวแบบออฟไลน์” หากไม่ได้ติดตั้งโมเดลภายในเครื่องและปิดการสลับไปคลาวด์ไว้ การแปลจะไม่ทำงานโดยไม่มีการส่งข้อมูลใดๆ\n"
    L"\n"
    L"ดูรายละเอียดฉบับเต็มในไฟล์ README ซึ่งอ่านซ้ำได้ทุกเมื่อ\n",
    L"ไฟล์ config: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"ข้อความยาวเกินไป จึงแปลเฉพาะส่วนต้นและส่วนท้าย",
    L"มีข้อความใหม่ที่ยังไม่ได้แปลด้านบน วางเคอร์เซอร์ที่ท้ายบรรทัดนั้นแล้วกด Enter เพื่อแปล",
    L"กำลังซ่อมแซมส่วนประกอบเอนจิ้นในเครื่อง…",
    L"การแปลในเครื่องไม่พร้อมใช้งาน",
    L"ไม่พบไฟล์เอนจิ้นแปลในเครื่อง จึงหยุดการแปลชั่วคราว ติดตั้ง Emebala Chat อีกครั้งเพื่อกู้คืนเอนจิ้นในเครื่อง หรือหากต้องการสลับไปใช้การแปลบนคลาวด์ (Google) ให้เลือก “Google แปลภาษา” จากเมนูถาดระบบ ที่ “เอนจิ้นการแปล”",
    L"เข้ากันได้กับ OpenAI (เซิร์ฟเวอร์ที่ผู้ใช้กำหนด)…",
        L"การตั้งค่าเอนจินแบบ OpenAI",
        L"การตั้งค่าเอนจินแบบ OpenAI…",
        L"Base URL",
        L"คีย์ API",
        L"โมเดล",
        L"ดึงรายการโมเดล",
        L"ไม่สามารถดึงรายการโมเดลได้ คุณสามารถพิมพ์ชื่อโมเดลโดยตรง",
        L"การเชื่อมต่อที่ไม่ปลอดภัย (HTTP)",
        L"Base URL ใช้ HTTP (ไม่ได้เข้ารหัส) คีย์ API และข้อความของคุณจะถูกส่งแบบไม่เข้ารหัส ดำเนินการต่อ?",
        L"บันทึกการตั้งค่าแบบ OpenAI แล้ว",
        L"คีย์ที่บันทึก: ",
        L"Base URL ไม่ถูกต้อง ตัวอย่าง: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"โมเดลที่ผู้ใช้กำหนด (.gguf)",
    L"ลงทะเบียนโมเดล .gguf อื่น…",
        L"ประกาศเกี่ยวกับคุณภาพการแปล",
        L"โมเดลที่เลือกไม่ใช่ Hy-MT2 เวอร์ชันปัจจุบันใช้พรอมต์เฉพาะสำหรับ Hy-MT2 ดังนั้นคุณภาพการแปลด้วยโมเดลนี้จึงไม่ได้รับประกัน ดำเนินการต่อ?",
        L"ลงทะเบียนโมเดลแล้ว",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"ลงทะเบียนโมเดลที่เลือกกับเอนจินในเครื่องแล้ว\n"
    L"\n"
    L"โมเดลนี้จะถูกใช้แปลเมื่อคุณเลือก \"เลือกเอนจินแปล > ผู้ใช้เลือก (.gguf)\"\n"
    L"\n"
    L"เมื่อมีผล: ภายในไม่เกินประมาณ 1 นาทีหลังลงทะเบียน (หลังเอนจินหยุดทำงานจากการไม่ได้ใช้งาน)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"โมเดลนี้มีอยู่ในตัว Emebala Chat อยู่แล้ว ไม่จำเป็นต้องลงทะเบียน เลือกเครื่องยนต์แปลภาษาในตัวได้โดยตรง",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ยังไม่ได้ลงทะเบียน)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"จัดการโมเดล…",
    L"ตัวจัดการโมเดลผู้ใช้",
    L"ไม่มีโมเดลผู้ใช้ที่ลงทะเบียน",
    L"เปลี่ยนชื่อ…",
    L"ลบ…",
    L"ปิด",
    L"ลบการลงทะเบียนโมเดล",
    L"การลงทะเบียนของโมเดลที่เลือกจะถูกลบ\n"
    L"\n"
    L"ไฟล์โมเดล (.gguf) ยังคงอยู่บนดิสก์และจะไม่ถูกลบ หากใช้โมเดลนี้อยู่ การเลือกเอนจินแปลจะกลับไปที่อัตโนมัติ (Auto)\n"
    L"\n"
    L"ดำเนินการต่อ?",
    L"เปลี่ยนชื่อโมเดล",
    L"ป้อนชื่อใหม่ (สูงสุด 64 อักขระ ไม่มีช่องว่าง)",
    L"ใช้ชื่อนี้ไม่ได้ โปรดป้อนชื่อที่ไม่ว่าง แตกต่างจากที่มีอยู่ ไม่มีช่องว่างหรือตัวคั่นเส้นทาง ไม่เกิน 64 อักขระ",
    L"บันทึกการเปลี่ยนแปลงแล้ว",

    L"ไม่สามารถ serialize registry.json ได้ (ชื่อไฟล์ถูกปฏิเสธ) ไม่มีการเปลี่ยนแปลงใดๆ",
    L"%LOCALAPPDATA% ไม่พร้อมใช้งาน ไม่พบโฟลเดอร์โมเดลที่แชร์",
    L"ไม่สามารถเขียน registry.json ได้",
    L"เขียน registry.json ไม่เสร็จสมบูรณ์",
    L"registry.json เสียหายหรือมี schema ที่ไม่รองรับ ไม่ได้ถูกแก้ไข โปรดซ่อมแซมหรือลบแล้วลองใหม่",
    L"การแปลล้มเหลว โปรดลองใหม่",
    L"เอนจินแปลในเครื่องไม่พร้อมใช้งานชั่วคราว (อาจกำลังเริ่มต้น) โปรดรอครู่แล้วลองใหม่",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"ตกลง",
    L"ยกเลิก",
};

// 26. Indonesian (id)
const LocalizedStrings kStringsIndonesian = {
    L"Status: aktif (F9: jeda)",
    L"Status: dijeda (F9: lanjutkan)",
    L"Mesin penerjemah",
    L"Google Translate (gratis / tanpa instalasi)",
    L"Mesin lokal bawaan (Hy-MT2-1.8B offline)",
    L"Bahasa sumber (masukan)",
    L"Bahasa tujuan (keluaran)",
    L"Tukar sumber ⇄ tujuan (klik ganda)",
    L"Kirim otomatis saat Enter",
    L"Umpan balik suara (nada)",
    L"Lambang mengambang terlihat",
    L"Mulai bersama Windows",
    L"Lembar contekan pintasan dan bantuan...",
    L"Keluar dari Emebala Chat",
    L"Tentang Emebala Chat…",
    L"Emebala Chat — pintasan dan panduan",
    L"Pintasan dan panduan penggunaan Emebala Chat:\n\n"
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
    L"Tentang Emebala Chat",
    L"Aktif",
    L"Menerjemahkan...",
    L"Terdahenti",
    L"Emebala Chat",
    L"Tidak dapat menyalin teks yang dipilih. Periksa aplikasi tujuan lalu coba lagi.",
    L"Tidak ada teks yang dipilih untuk diterjemahkan.",
    L"Deteksi otomatis",
    L"Emebala Chat sudah berjalan di latar belakang.\nPeriksa baki notifikasi sistem.",
    L"Inisialisasi COM gagal.\nLambang mengambang dan teks-ke-suara tidak tersedia,\ntapi terjemahan, pintasan, baki, dan suara tetap berfungsi.",
    L"Lupakan salin-tempel. Ketik secara alami dalam bahasa ibu Anda — terjemahan menggantikan ketikan Anda secara langsung di aplikasi Windows mana pun.",
    L"⚡ Seret-untuk-menerjemahkan — pilih teks di aplikasi mana pun, ikon mengambang menerjemahkan seketika.",
    L"🔊 TTS neural — mengucapkan semua 37 bahasa melalui paket suara Windows yang terpasang.",
    L"🔒 100% di perangkat & privat — aktif hanya saat pintasan ditekan; papan klip tak tersentuh.",
    L"Pada 2000 SM, para juru tulis Mesopotamia menyebut «Eme-bala» — mereka yang menjelma bahasa menjadi jembatan antar-dunia.",
    L"Situs web",
    L"Kontak",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Jam kerja 10:00–19:00 KST",
    L"Arsitek utama: Yongtai Kim",
    L"✓ Disalin!",
    L"📋 Salin",
    L"🔊 Suara",
    L"Bahasa antarmuka",
    L"Otomatis (bahasa sistem)",
    L"Setel ulang ke setelan bawaan sistem",
    L"Setelan bawaan dipulihkan",
    L"Pengetikan keyboard",
    L"Tooltip seret",
    L"Emebala Chat",
    L"Tidak ada suara Windows yang terpasang untuk bahasa ini. Klik 🔊 lagi untuk membuka pengaturan Ucapan.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Pemberitahuan privasi",
    L"Berikut prinsip privasi Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat tidak mengoperasikan server sendiri.\n"
    L"• Dengan model lokal, teks terjemahan tidak meninggalkan perangkat Anda.\n"
    L"• Jika Anda memilih Google Translate atau beralih otomatis ke cloud, teks yang dipilih atau diketik dikirim langsung ke Google untuk diterjemahkan, bukan melalui Emebala.\n"
    L"• Log diagnostik secara default MATI; aktifkan di pengaturan (opt-in).\n"
    L"• Jika tidak ingin mengirim teks ke cloud (Google), buka menu ikon di bilah tugas, pilih “Mesin penerjemah” lalu pilih “Mesin lokal bawaan”. Tanpa model lokal terpasang dan dengan cadangan cloud dinonaktifkan, terjemahan tidak berjalan — tidak ada yang dikirim.\n"
    L"\n"
    L"Detail lengkap ada di berkas README, yang dapat dibaca ulang kapan saja.\n",
    L"Berkas konfigurasi: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Teks terlalu panjang: hanya bagian awal dan akhir yang diterjemahkan.",
    L"Ada teks baru yang belum diterjemahkan di atas. Letakkan kursor di akhir baris itu dan tekan Enter untuk menerjemahkan.",
    L"Memperbaiki komponen mesin lokal…",
    L"Terjemahan lokal tidak tersedia",
    L"File mesin terjemahan lokal tidak ditemukan, jadi terjemahan dijeda. Instal ulang Emebala Chat untuk memulihkan mesin lokal, atau untuk beralih ke terjemahan cloud (Google), pilih “Google Terjemahan” dari menu baki, “Mesin terjemahan”.",
    L"Kompatibel OpenAI (server khusus)…",
        L"Pengaturan mesin kompatibel OpenAI",
        L"Pengaturan mesin kompatibel OpenAI…",
        L"URL dasar",
        L"Kunci API",
        L"Model",
        L"Ambil daftar model",
        L"Daftar model tidak dapat diambil. Anda dapat mengetik nama model secara langsung.",
        L"Koneksi tidak aman (HTTP)",
        L"URL dasar menggunakan HTTP (tidak terenkripsi). Kunci API dan teks Anda akan dikirim tanpa enkripsi. Lanjutkan?",
        L"Pengaturan kompatibel OpenAI disimpan.",
        L"Kunci tersimpan: ",
        L"URL dasar tidak valid. Contoh: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Model yang ditentukan pengguna (.gguf)",
    L"Daftarkan model .gguf lainnya…",
        L"Pemberitahuan kualitas terjemahan",
        L"Model yang dipilih bukan Hy-MT2. Versi saat ini menggunakan prompt khusus Hy-MT2, sehingga kualitas terjemahan dengan model ini tidak dijamin. Lanjutkan?",
        L"Model terdaftar",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Model yang dipilih telah didaftarkan ke mesin lokal.\n"
    L"\n"
    L"Model ini digunakan untuk terjemahan saat Anda memilih \"Pemilihan mesin terjemahan > Pilihan pengguna (.gguf)\".\n"
    L"\n"
    L"Kapan berlaku: paling lama sekitar 1 menit setelah pendaftaran (setelah mesin berhenti karena tidak aktif)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Model ini sudah tertanam di Emebala Chat. Pendaftaran tidak diperlukan. Pilih langsung mesin penerjemahan lokal bawaan.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(belum terdaftar)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Kelola model…",
    L"Pengelola Model Pengguna",
    L"Tidak ada model pengguna yang terdaftar.",
    L"Ubah nama…",
    L"Hapus…",
    L"Tutup",
    L"Hapus registrasi model",
    L"Registrasi model yang dipilih akan dihapus.\n"
    L"\n"
    L"File model (.gguf) tetap disimpan di disk dan tidak dihapus. Jika model ini sedang digunakan, pemilihan mesin penerjemah akan kembali ke Otomatis (Auto).\n"
    L"\n"
    L"Lanjutkan?",
    L"Ubah nama model",
    L"Masukkan nama baru (maks. 64 karakter, tanpa spasi).",
    L"Nama itu tidak dapat digunakan. Masukkan nama yang tidak kosong, berbeda dari yang sudah ada, tanpa spasi atau pemisah jalur, maksimal 64 karakter.",
    L"Perubahan disimpan.",

    L"registry.json tidak dapat diserialisasi (nama file ditolak). Tidak ada yang berubah.",
    L"%LOCALAPPDATA% tidak tersedia; folder model bersama tidak dapat ditemukan.",
    L"registry.json tidak dapat ditulis.",
    L"registry.json tidak dapat ditulis sepenuhnya.",
    L"registry.json rusak atau memiliki skema yang tidak didukung. File TIDAK diubah. Perbaiki atau hapus, lalu coba lagi.",
    L"Terjemahan gagal. Silakan coba lagi.",
    L"Mesin terjemahan lokal sementara tidak tersedia (mungkin sedang mulai). Tunggu sebentar dan coba lagi.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Oke",
    L"Batal",
};

// 27. Malay (ms)
const LocalizedStrings kStringsMalay = {
    L"Status: aktif (F9: jeda)",
    L"Status: dijeda (F9: sambung)",
    L"Enjin penterjemah",
    L"Google Terjemah (percuma / tanpa pemasangan)",
    L"Enjin setempat terbina dalam (Hy-MT2-1.8B luar talian)",
    L"Bahasa sumber (input)",
    L"Bahasa sasaran (output)",
    L"Tukar sumber ⇄ sasaran (klik dua kali)",
    L"Hantar automatik dengan Enter",
    L"Maklum balas bunyi (nada)",
    L"Lambang terapung kelihatan",
    L"Mula dengan Windows",
    L"Contehan pintasan dan bantuan...",
    L"Keluar Emebala Chat",
    L"Perihal Emebala Chat…",
    L"Emebala Chat — pintasan dan panduan",
    L"Pintasan dan panduan penggunaan Emebala Chat:\n\n"
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
    L"Perihal Emebala Chat",
    L"Aktif",
    L"Menterjemah...",
    L"Dijeda",
    L"Emebala Chat",
    L"Tidak dapat menyalin teks terpilih. Periksa apl sasaran dan cuba lagi.",
    L"Tiada teks dipilih untuk diterjemah.",
    L"Kesan automatik",
    L"Emebala Chat sudah berjalan di latar belakang.\nPapar dulang pemberitahuan sistem.",
    L"Permulaan COM gagal.\nLambang terapung dan teks-ke-suara tidak tersedia,\ntetapi terjemahan, pintasan, dulang dan bunyi masih berfungsi.",
    L"Berhenti menyalin dan menampal. Taip secara semula jadi dalam bahasa ibunda anda — terjemahan menggantikan taipan anda secara masa nyata dalam mana-mana apl Windows.",
    L"⚡ Seret-untuk-terjemah — pilih teks dalam mana-mana apl, ikon terapung serta-merta menterjemah.",
    L"🔊 TTS neural — menyebut kesemua 37 bahasa melalui pakej suara Windows yang dipasang.",
    L"🔒 100% pada peranti & persendirian — aktif hanya semasa pintasan ditekan; papan keratan tidak diusik.",
    L"Pada 2000 SM, jurutulis Mesopotamia memanggil «Eme-bala» — mereka yang menjadikan bahasa jambatan antara dunia.",
    L"Laman web",
    L"Hubungi",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Waktu pejabat 10:00–19:00 KST",
    L"Arkitek utama: Yongtai Kim",
    L"✓ Disalin!",
    L"📋 Salin",
    L"🔊 Suara",
    L"Bahasa antara muka",
    L"Auto (bahasa sistem)",
    L"Set semula ke lalai sistem",
    L"Lalai dipulihkan",
    L"Taipan papan kekunci",
    L"Tooltip seret",
    L"Emebala Chat",
    L"Tiada suara Windows dipasang untuk bahasa ini. Klik 🔊 sekali lagi untuk membuka tetapan Ucapan.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Pemberitahuan privasi",
    L"Berikut adalah prinsip privasi Emebala Chat.\n"
    L"\n"
    L"• Emebala Chat tidak mengendalikan pelayan sendiri.\n"
    L"• Dengan model setempat, teks diterjemahkan tidak meninggalkan peranti anda.\n"
    L"• Jika anda memilih Google Terjemah atau bertukar ke awan secara automatik, teks yang dipilih atau ditaip dihantar terus kepada Google untuk diterjemahkan, bukan melalui Emebala.\n"
    L"• Log diagnostik dimatikan secara lalai; aktifkan dalam tetapan (pilihan).\n"
    L"• Jika anda tidak mahu menghantar teks ke awan (Google), buka menu ikon pada tray sistem, pilih “Enjin penterjemah” kemudian “Enjin setempat terbina dalam”. Tanpa model setempat dipasang dan dengan fallback awan dimatikan, terjemahan tidak berjalan — tiada apa dihantar.\n"
    L"\n"
    L"Butiran penuh terdapat dalam fail README, yang boleh dibaca semula pada bila-bila masa.\n",
    L"Fail konfigurasi: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Teks terlalu panjang: hanya bahagian awal dan akhir diterjemahkan.",
    L"Terdapat teks baru yang belum diterjemahkan di atas. Letakkan kursor di hujung baris itu dan tekan Enter untuk menterjemah.",
    L"Membaiki komponen enjin tempatan…",
    L"Terjemahan tempatan tidak tersedia",
    L"Fail enjin terjemahan tempatan tidak dijumpai, jadi terjemahan dijeda. Pasang semula Emebala Chat untuk memulihkan enjin tempatan, atau untuk bertukar ke terjemahan awan (Google), pilih “Google Terjemah” dari menu dulang, “Enjin terjemahan”.",
    L"Serasi OpenAI (pelayan tersuai)…",
        L"Tetapan enjin serasi OpenAI",
        L"Tetapan enjin serasi OpenAI…",
        L"URL asas",
        L"Kunci API",
        L"Model",
        L"Ambil senarai model",
        L"Senarai model tidak dapat diambil. Anda boleh menaip nama model secara langsung.",
        L"Sambungan tidak selamat (HTTP)",
        L"URL asas menggunakan HTTP (tidak disulitkan). Kunci API dan teks anda akan dihantar tanpa sulitan. Teruskan?",
        L"Tetapan serasi OpenAI disimpan.",
        L"Kunci disimpan: ",
        L"URL asas tidak sah. Contoh: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Model yang ditentukan pengguna (.gguf)",
    L"Daftarkan model .gguf lain…",
        L"Nota kualiti terjemahan",
        L"Model yang dipilih bukan Hy-MT2. Versi semasa menggunakan prompt khas Hy-MT2, jadi kualiti terjemahan dengan model ini tidak dijamin. Teruskan?",
        L"Model didaftarkan",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Model yang dipilih telah didaftarkan dengan enjin tempatan.\n"
    L"\n"
    L"Model ini digunakan untuk terjemahan apabila anda memilih \"Pemilihan enjin terjemahan > Pilihan pengguna (.gguf)\".\n"
    L"\n"
    L"Bila berkuat kuasa: paling lambat kira-kira 1 minit selepas pendaftaran (selepas enjin berhenti kerana tidak aktif)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Model ini sudah terbina dalam Emebala Chat. Pendaftaran tidak diperlukan. Pilih terus enjin terjemahan tempatan terbina dalam.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(belum didaftarkan)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Urus model…",
    L"Pengurus Model Pengguna",
    L"Tiada model pengguna berdaftar.",
    L"Tukar nama…",
    L"Padam…",
    L"Tutup",
    L"Padam pendaftaran model",
    L"Pendaftaran model yang dipilih akan dipadamkan.\n"
    L"\n"
    L"Fail model (.gguf) kekal pada cakera dan tidak dipadamkan. Jika model ini sedang digunakan, pemilihan enjin terjemahan akan kembali kepada Automatik (Auto).\n"
    L"\n"
    L"Teruskan?",
    L"Tukar nama model",
    L"Masukkan nama baharu (maks. 64 aksara, tanpa ruang).",
    L"Nama itu tidak boleh digunakan. Masukkan nama yang tidak kosong, berbeza daripada yang sedia ada, tanpa ruang atau pemisah laluan, sehingga 64 aksara.",
    L"Perubahan disimpan.",

    L"registry.json tidak dapat disirikan (nama fail ditolak). Tiada apa yang berubah.",
    L"%LOCALAPPDATA% tidak tersedia; folder model kongsi tidak dapat ditemui.",
    L"registry.json tidak dapat ditulis.",
    L"registry.json tidak dapat ditulis dengan lengkap.",
    L"registry.json rosak atau mempunyai skema tidak disokong. Ia TIDAK diubah suai. Baiki atau padamkannya, kemudian cuba lagi.",
    L"Terjemahan gagal. Sila cuba lagi.",
    L"Enjin terjemahan tempatan tidak tersedia buat sementara (mungkin sedang dimulakan). Tunggu seketika dan cuba lagi.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Setuju",
    L"Batal",
};

// 28. Filipino (fil)
// Note: "Website", "Google Translate" and "Reddit" are used in Philippine
// English-dominant UI practice; the surrounding copy is genuine Filipino.
const LocalizedStrings kStringsFilipino = {
    L"Katayuan: Aktibo (F9: I-pause)",
    L"Katayuan: Nakahinto (F9: Ipagpatuloy)",
    L"Makina ng pagsasalin",
    L"Google Translate (Libre / walang i-install)",
    L"Built-in na lokal na makina (Hy-MT2-1.8B offline)",
    L"Pinagmulang wika (input)",
    L"Target na wika (output)",
    L"Palitan ang pinagmulan ⇄ target (i-double click)",
    L"Awtomatikong ipadala sa Enter",
    L"Tugon ng tunog (mga tono)",
    L"Ipakita ang lumulutang na badge",
    L"Simulan kasama ang Windows",
    L"Cheat sheet ng mga shortcut at tulong...",
    L"Lumabas sa Emebala Chat",
    L"Tungkol sa Emebala Chat…",
    L"Emebala Chat — mga shortcut at gabay",
    L"Mga shortcut at gabay sa Emebala Chat:\n\n"
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
    L"Tungkol sa Emebala Chat",
    L"Aktibo",
    L"Nagsasalin...",
    L"Nakahinto",
    L"Emebala Chat",
    L"Hindi maikopya ang napiling teksto. Suriin ang target app at subukang muli.",
    L"Walang napiling teksto na isasalin.",
    L"Kusang pagtukoy",
    L"Gumagana na ang Emebala Chat sa background.\nTingnan ang notification tray.",
    L"Nabigong i-initialize ang COM.\nHindi magagamit ang lumulutang badge at text-to-speech,\nngunit patuloy pa ring gumagana ang pagsasalin, shortcuts, tray at mga tunog.",
    L"Tigilan na ang kopya-at-idikit. Mag-type nang natural sa iyong katutubong wika — pinapalitan ng salin ang iyong pagta-type sa real time sa kahit aning app ng Windows.",
    L"⚡ I-drag para isalin — pumili ng teksto sa kahit aning app, agad isinasalin ng lumulutang icon.",
    L"🔊 Neural TTS — binibigkas ang lahat ng 37 wika sa pamamagitan ng naka-install na Windows voice packs.",
    L"🔒 100% sa device at pribado — aktibo lamang habang hawak ang shortcut; hindi hinahawakan ang clipboard.",
    L"Noong 2000 BCE, tinatawag ng mga maysulat ng Mesopotamia na «Eme-bala» ang mga gumagawa ng wika na tulay sa mga mundo.",
    L"Website",
    L"Kontak",
    L"Reddit",
    L"Team Sunplaza · Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · Oras ng opisina 10:00–19:00 KST",
    L"Namunang arkitekto: Yongtai Kim",
    L"✓ Nakopya!",
    L"📋 Kopyahin",
    L"🔊 Boses",
    L"Wika ng interface",
    L"Awtomatiko (wika ng sistema)",
    L"I-reset sa default ng sistema",
    L"Naibalik ang mga default",
    L"Pagta-type ng keyboard",
    L"Tooltip ng pagdrag",
    L"Emebala Chat",
    L"Walang naka-install na Windows voice para sa wikang ito. I-click muli ang 🔊 upang buksan ang mga setting ng Pagsasalita.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"Paalala sa privacy",
    L"Narito ang mga prinsipyo sa privacy ng Emebala Chat.\n"
    L"\n"
    L"• Walang sariling server na pinapatakbo ang Emebala Chat.\n"
    L"• Sa local na model, hindi lumalabas sa iyong device ang isinalin.\n"
    L"• Kung pipiliin ang Google Translate o awtomatikong lilipat sa cloud, ang napili o ni-type na teksto ay ipapadala nang direkta sa Google para isalin, hindi sa pamamagitan ng Emebala.\n"
    L"• Ang diagnostic log ay OFF sa default; i-on sa settings (opt-in).\n"
    L"• Kung ayaw mong ipadala ang teksto sa cloud (Google), buksan ang menu ng icon sa system tray, piliin ang “Makina ng pagsasalin” at pagkatapos “Built-in na lokal na makina”. Kung walang naka-install na lokal na modelo at nakapatay ang cloud fallback, hindi tumatakbo ang pagsasalin — walang ipinapadala.\n"
    L"\n"
    L"Makikita ang buong detalye sa README file na maaaring babasahin anumang oras.\n",
    L"Config file: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"Masyadong mahaba ang teksto: simula at katapusan lang ang isinalin.",
    L"May bagong hindi pa isinaling teksto sa itaas. Ilagay ang cursor sa dulo ng linyang iyon at pindutin ang Enter upang isalin.",
    L"Kinukumpuni ang mga bahagi ng lokal na makina…",
    L"Hindi available ang lokal na pagsasalin",
    L"Nawawala ang mga file ng lokal na makina ng pagsasalin, kaya pansamantalang tumigil ang pagsasalin. I-install muli ang Emebala Chat para maibalik ang lokal na makina, o para lumipat sa cloud (Google) na pagsasalin, piliin ang “Google Translate” mula sa menu ng tray, “Makina ng pagsasalin”.",
    L"OpenAI-compatible (custom server)…",
        L"Mga setting ng OpenAI-compatible na engine",
        L"Mga setting ng OpenAI-compatible na engine…",
        L"Base URL",
        L"API Key",
        L"Modelo",
        L"Kunin ang listahan ng modelo",
        L"Hindi nakuha ang listahan ng modelo. Maaari kang mag-type ng pangalan ng modelo nang direkta.",
        L"Di-ligtas na koneksyon (HTTP)",
        L"Ginagamit ng Base URL ang HTTP (hindi naka-encrypt). Ang iyong API key at teksto ay ipapadala nang walang encryption. Magpatuloy?",
        L"Nai-save ang mga OpenAI-compatible na setting.",
        L"Nai-save na key: ",
        L"Hindi wasto ang Base URL. Halimbawa: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"Modelong tinukoy ng user (.gguf)",
    L"Magrehistro ng ibang .gguf model…",
        L"Paalala sa kalidad ng pagsasalin",
        L"Ang napiling modelo ay hindi Hy-MT2. Ang kasalukuyang bersyon ay gumagamit ng prompt na para lamang sa Hy-MT2, kaya hindi ginagarantiyahan ang kalidad ng pagsasalin ng modelong ito. Magpatuloy?",
        L"Nakarehistro ang modelo",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"Ang napiling modelo ay nakarehistro sa lokal na engine.\n"
    L"\n"
    L"Ang modelong ito ay ginagamit sa pagsasalin kapag pinili mo ang \"Pagpili ng engine ng pagsasalin > Pagpili ng gumagamit (.gguf)\".\n"
    L"\n"
    L"Kailan magiging epektibo: hindi hihigit sa mga 1 minuto pagkatapos ng rehistro (pagkatapos umalis ng engine dahil sa kawalan ng aktibidad)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"Ang modelong ito ay nakabuilt-in na sa Emebala Chat. Hindi na kailangan ng pagpaparehistro. Maaari mong direktang piliin ang built-in na lokal na translation engine.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(hindi pa rehistrado)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"Pamahalaan ang mga modelo…",
    L"Tagapamahala ng Mga Modelo ng Gumagamit",
    L"Walang nakarehistrong modelong pang-gumagamit.",
    L"Palitan ang pangalan…",
    L"Tanggalin…",
    L"Isara",
    L"Tanggalin ang rehistro ng modelo",
    L"Ang rehistro ng napiling modelo ay tatanggalin.\n"
    L"\n"
    L"Ang file ng modelo (.gguf) ay mananatili sa disk at hindi tatanggalin. Kung ang modelong ito ay ginagamit, ang pagpili ng makina ng pagsasalin ay babalik sa Awtomatiko (Auto).\n"
    L"\n"
    L"Magpatuloy?",
    L"Palitan ang pangalan ng modelo",
    L"Maglagay ng bagong pangalan (hanggang 64 na karakter, walang espasyo).",
    L"Hindi magamit ang pangalang iyan. Maglagay ng di-walang-laman na pangalan, naiiba sa mga umiiral, walang espasyo o pantahip ng landas, hanggang 64 na karakter.",
    L"Nai-save ang mga pagbabago.",

    L"Hindi mai-serialize ang registry.json (tinanggihan ang isang pangalan ng file). Walang nagbago.",
    L"Hindi available ang %LOCALAPPDATA%; hindi mahanap ang ibinahaging folder ng mga modelo.",
    L"Hindi maisulat ang registry.json.",
    L"Hindi lubos na nasulat ang registry.json.",
    L"Sira ang registry.json o may hindi suportadong schema. HINDI ito binago. Ayusin o tanggalin ito, at subukan muli.",
    L"Nabigo ang pagsasalin. Subukan muli.",
    L"Ang lokal na engine ng pagsasalin ay pansamantalang hindi available (maaaring nagsisimula). Maghintay sandali at subukan muli.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"Oo",
    L"Kanselahin",
};

// 29. Hindi (hi)
const LocalizedStrings kStringsHindi = {
    L"स्थिति: सक्रिय (F9: रोकें)",
    L"स्थिति: रुका हुआ (F9: जारी रखें)",
    L"अनुवाद इंजन",
    L"Google अनुवाद (मुफ़्त / इंस्टॉलेशन नहीं)",
    L"अंतर्निहित लोकल इंजन (Hy-MT2-1.8B ऑफ़लाइन)",
    L"स्रोत भाषा (इनपुट)",
    L"लक्ष्य भाषा (आउटपुट)",
    L"स्रोत ⇄ लक्ष्य अदला-बदली (डबल-क्लिक)",
    L"Enter पर स्वतः भेजें",
    L"ध्वनि प्रतिक्रिया (टोन)",
    L"फ़्लोटिंग बैज दिखाएँ",
    L"Windows के साथ प्रारंभ करें",
    L"शॉर्टकट चीट-शीट और सहायता...",
    L"एमेबाला चैट से बाहर निकलें",
    L"एमेबाला चैट के बारे में…",
    L"एमेबाला चैट — शॉर्टकट और उपयोग गाइड",
    L"एमेबाला चैट शॉर्टकट और उपयोग गाइड:\n\n"
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
    L"एमेबाला चैट के बारे में",
    L"सक्रिय",
    L"अनुवाद हो रहा...",
    L"रुका हुआ",
    L"एमेबाला चैट",
    L"चयनित पाठ कॉपी नहीं हो सका। लक्ष्य ऐप जाँचें और फिर कोशिश करें।",
    L"अनुवाद हेतु कोई पाठ चयनित नहीं है।",
    L"स्वतः पहचान",
    L"एमेबाला चैट पहले से पृष्ठभूमि में चल रहा है।\nसूचना ट्रे देखें।",
    L"COM आरंभ विफल।\nफ़्लोटिंग बैज और वाचन अनुपलब्ध रहेंगे,\nपर अनुवाद, शॉर्टकट, ट्रे और ध्वनियाँ काम करती रहेंगी।",
    L"कापी-पेस्ट की ज़रूरत नहीं। अपनी मातृभाषा में सहज टाइप करें — अनुवाद किसी भी Windows ऐप में आपकी टाइपिंग को रीयल-टाइम में बदल देता है।",
    L"⚡ खींचें-और-अनुवाद — किसी भी ऐप में पाठ चुनें, फ़्लोटिंग आइकन तुरंत अनुवाद करता है।",
    L"🔊 न्यूरल TTS — इंस्टॉल Windows वॉइस पैक से सभी 37 भाषाओं का उच्चारण।",
    L"🔒 100% डिवाइस पर और निजी — जब तक शॉर्टकट दबा है तभी सक्रिय; क्लिपबोर्ड को छुए बिना।",
    L"2000 ईसा पूर्व, मेसोपोटामिया के लेखकों ने भाषा को दुनियाओं के बीच पुल बनाने वालों को «Eme-bala» कहा करता था।",
    L"वेबसाइट",
    L"संपर्क",
    L"Reddit",
    L"Team Sunplaza · सियोल येोंगदोंगपो (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · कार्यालय समय 10:00–19:00 KST",
    L"मुख्य आर्किटेक्ट: Yongtai Kim",
    L"✓ कॉपी हो गया!",
    L"📋 कॉपी",
    L"🔊 वाचन",
    L"इंटरफ़ेस भाषा",
    L"स्वतः (सिस्टम भाषा)",
    L"सिस्टम पूर्वनिर्धारित पर रीसेट करें",
    L"पूर्वनिर्धारित बहाल",
    L"कीबोर्ड टाइपिंग",
    L"ड्रैग टूलटिप",
    L"एमेबाला चैट",
    L"इस भाषा के लिए कोई Windows वॉइस इंस्टॉल नहीं है. वॉइस सेटिंग खोलने के लिए 🔊 पर फिर से क्लिक करें.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"गोपनीयता सूचना",
    L"Emebala Chat की गोपनीयता नीति इस प्रकार है:\n"
    L"\n"
    L"• Emebala Chat का कोई अपना सर्वर नहीं है।\n"
    L"• स्थानीय मॉडल पर अनुवादित पाठ आपके डिवाइस से बाहर नहीं जाता।\n"
    L"• यदि आप Google Translate चुनते हैं या स्वतः क्लाउड पर स्विच होता है, तो चयनित/लिखा पाठ अनुवाद हेतु सीधे Google भेजा जाता है — Emebala से नहीं।\n"
    L"• डायग्नोस्टिक लॉग डिफ़ॉल्ट बंद (OFF) हैं; सेटिंग में चालू करें (opt-in)।\n"
    L"• यदि आप टेक्स्ट क्लाउड (Google) में नहीं भेजना चाहते, तो सिस्टम ट्रे आइकन मेनू खोलें, “अनुवाद इंजन” चुनें और “अंतर्निहित लोकल इंजन” चुनें। यदि स्थानीय मॉडल इंस्टॉल नहीं है और क्लाउड फ़ॉलबैक बंद है, तो अनुवाद नहीं चलेगा — कुछ भी नहीं भेजा जाएगा।\n"
    L"\n"
    L"पूरा विवरण README फ़ाइल में है, जिसे आप कभी भी दोबारा पढ़ सकते हैं।\n",
    L"कॉन्फ़िग फ़ाइल: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"पाठ बहुत लंबा था: केवल शुरुआत और अंत का अनुवाद किया गया।",
    L"ऊपर नया अनअनुवादित टेक्स्ट है। उसे अनुवाद करने के लिए कर्सर को उस पंक्ति के अंत में रखें और Enter दबाएं।",
    L"लोकल इंजन घटकों की मरम्मत हो रही है…",
    L"लोकल अनुवाद उपलब्ध नहीं है",
    L"लोकल अनुवाद इंजन की फ़ाइलें नहीं मिलीं, इसलिए अनुवाद रुका हुआ है। लोकल इंजन को पुनर्स्थापित करने के लिए Emebala Chat को पुनः इंस्टॉल करें, या क्लाउड (Google) अनुवाद पर जाने के लिए, ट्रे मेनू से “अनुवाद इंजन” में “Google अनुवाद” चुनें।",
    L"OpenAI संगत (उपयोगकर्ता-परिभाषित सर्वर)…",
        L"OpenAI-संगत इंजन सेटिंग्स",
        L"OpenAI-संगत इंजन सेटिंग्स…",
        L"बेस URL",
        L"API कुंजी",
        L"मॉडल",
        L"मॉडल सूची प्राप्त करें",
        L"मॉडल सूची प्राप्त नहीं हो सकी। आप मॉडल का नाम सीधे टाइप कर सकते हैं।",
        L"असुरक्षित कनेक्शन (HTTP)",
        L"बेस URL HTTP (अनएन्क्रिप्टेड) उपयोग करता है। आपकी API कुंजी और टेक्स्ट सादे टेक्स्ट में भेजे जाएंगे। जारी रखें?",
        L"OpenAI-संगत सेटिंग्स सहेजी गईं।",
        L"सहेजी गई कुंजी: ",
        L"बेस URL मान्य नहीं है। उदाहरण: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"उपयोगकर्ता-परिभाषित मॉडल (.gguf)",
    L"कोई अन्य .gguf मॉडल पंजीकृत करें…",
        L"अनुवाद गुणवत्ता सूचना",
        L"चयनित मॉडल Hy-MT2 नहीं है। वर्तमान संस्करण केवल Hy-MT2 के लिए प्रॉम्प्ट का उपयोग करता है, इसलिए इस मॉडल से अनुवाद की गुणवत्ता की गारंटी नहीं है। जारी रखें?",
        L"मॉडल पंजीकृत",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"चयनित मॉडल स्थानीय इंजन में पंजीकृत हो गया है।\n"
    L"\n"
    L"इस मॉडल का उपयोग तब अनुवाद के लिए होता है जब आप \"अनुवाद इंजन चयन > उपयोगकर्ता चयन (.gguf)\" चुनते हैं।\n"
    L"\n"
    L"कब लागू: पंजीकरण के बाद अधिकतम लगभग 1 मिनट (इंजन के निष्क्रिय समापन के बाद)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"यह मॉडल Emebala Chat में पहले से ही अंतर्निहित है। पंजीकरण की आवश्यकता नहीं है। अंतर्निहित लोकल अनुवाद इंजन को सीधे चुनें।",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(पंजीकृत नहीं)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"मॉडल प्रबंधित करें…",
    L"उपयोगकर्ता मॉडल प्रबंधक",
    L"कोई उपयोगकर्ता मॉडल पंजीकृत नहीं है।",
    L"नाम बदलें…",
    L"हटाएं…",
    L"बंद करें",
    L"मॉडल पंजीकरण हटाएं",
    L"चयनित मॉडल का पंजीकरण हटाया जाएगा।\n"
    L"\n"
    L"मॉडल फ़ाइल (.gguf) डिस्क पर बनी रहती है और नहीं हटती। यदि यह मॉडल प्रयोग में था, तो अनुवाद इंजन चयन स्वतः (Auto) पर लौट आएगा।\n"
    L"\n"
    L"जारी रखें?",
    L"मॉडल का नाम बदलें",
    L"नया नाम दर्ज करें (अधिकतम 64 वर्ण, बिना रिक्त स्थान)।",
    L"वह नाम प्रयोग नहीं किया जा सकता। कोई खाली नहीं, मौजूदा से अलग, बिना रिक्त स्थान या पथ विभाजक, 64 वर्णों के भीतर नाम दर्ज करें।",
    L"परिवर्तन सहेजे गए।",

    L"registry.json को सीरियलाइज़ नहीं किया जा सका (फ़ाइल नाम अस्वीकृत)। कुछ भी नहीं बदला गया।",
    L"%LOCALAPPDATA% अनुपलब्ध है; साझा मॉडल फ़ोल्डर नहीं मिल सका।",
    L"registry.json नहीं लिखा जा सका।",
    L"registry.json पूरी तरह नहीं लिखा जा सका।",
    L"registry.json क्षतिग्रस्त है या उसका स्कीमा unsupported है। इसमें कोई बदलाव नहीं किया गया। इसे ठीक करें या हटाएं, फिर पुनः प्रयास करें।",
    L"अनुवाद विफल रहा। कृपया पुनः प्रयास करें।",
    L"स्थानीय अनुवाद इंजन अस्थायी रूप से उपलब्ध नहीं है (शायद प्रारंभ हो रहा हो)। कृपया कुछ देर प्रतीक्षा करें और पुनः प्रयास करें।",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"ठीक है",
    L"रद्द करें",
};

// 30. Bengali (bn)
const LocalizedStrings kStringsBengali = {
    L"স্ট্যাটাস: সক্রিয় (F9: বিরতি)",
    L"স্ট্যাটাস: বিরতিপ্রাপ্ত (F9: পুনরায় চালু)",
    L"অনুবাদ ইঞ্জিন",
    L"Google Translate (বিনামূল্যে / ইনস্টল ছাড়াই)",
    L"অন্তর্নির্মিত লোকাল ইঞ্জিন (Hy-MT2-1.8B অফলাইন)",
    L"উৎস ভাষা (ইনপুট)",
    L"লক্ষ্য ভাষা (আউটপুট)",
    L"উৎস ⇄ লক্ষ্য অদলবদল (ডাবল-ক্লিক)",
    L"Enter-এ স্বয়ংক্রিয় প্রেরণ",
    L"শব্দ প্রতিক্রিয়া (টোন)",
    L"ফ্লোটিং ব্যাজ দৃশ্যমান",
    L"Windows-এর সাথে চালু",
    L"শর্টকাট চিট-শিট ও সহায়তা...",
    L"এমেবালা চ্যাট প্রস্থান",
    L"এমেবালা চ্যাট সম্পর্কে…",
    L"এমেবালা চ্যাট — শর্টকাট ও ব্যবহার গাইড",
    L"এমেবালা চ্যাট শর্টকাট ও ব্যবহার গাইড:\n\n"
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
    L"এমেবালা চ্যাট সম্পর্কে",
    L"সক্রিয়",
    L"অনুবাদ হচ্ছে...",
    L"বিরতি",
    L"এমেবালা চ্যাট",
    L"নির্বাচিত লেখা কপি করা যায়নি। লক্ষ্য অ্যাপ দেখে আবার চেষ্টা করুন।",
    L"অনুবাদের জন্য কোনো লেখা নির্বাচন করা হয়নি।",
    L"স্বয়ংক্রিয় শনাক্ত",
    L"এমেবালা চ্যাট ইতিমধ্যে পটভূমিতে চলছে।\nসিস্টেম নোটিফিকেশন ট্রে দেখুন।",
    L"COM আরম্ভ বিফল।\nফ্লোটিং ব্যাজ ও টেক্সট-টু-স্পিচ পাওয়া যাবে না,\nতবে অনুবাদ, শর্টকাট, ট্রে ও শব্দ কাজ করবে।",
    L"আর কপি-পেস্ট নয়। মাতৃভাষায় স্বাভাবিকভাবে লিখুন — যেকোনো Windows অ্যাপে আপনার টাইপিং রিয়েল-টাইমে অনুবাদে বদলে যায়।",
    L"⚡ ড্র্যাগ-করুন-অনুবাদ — যেকোনো অ্যাপে লেখা নির্বাচন করুন, ফ্লোটিং আইকন সঙ্গে সঙ্গে অনুবাদ করে।",
    L"🔊 নিউরাল TTS — ইনস্টল করা Windows ভয়েস প্যাক দিয়ে সব 37টি ভাষার উচ্চারণ।",
    L"🔒 ১০০% ডিভাইসে ও গোপন — শর্টকাট চেপে রাখা অবধিই সক্রিয়; ক্লিপবোর্ড অক্ষত।",
    L"২০০০ খ্রিস্টপূর্বাব্দে মেসোপটেমীয় লেখকরা ভাষাকে জগতের সেতু বানানো লোকদের «Eme-bala» বলত।",
    L"ওয়েবসাইট",
    L"যোগাযোগ",
    L"Reddit",
    L"Team Sunplaza · সিউল ইয়ংডেংপো (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · অফিস সময় 10:00–19:00 KST",
    L"প্রধান আর্কিটেক্ট: Yongtai Kim",
    L"✓ কপি হয়েছে!",
    L"📋 কপি",
    L"🔊 ভাষণ",
    L"ইন্টারফেস ভাষা",
    L"অটো (সিস্টেম ভাষা)",
    L"সিস্টেম ডিফল্টে রিসেট",
    L"ডিফল্ট পুনরুদ্ধার",
    L"কীবোর্ড টাইপিং",
    L"ড্র্যাগ টুলটিপ",
    L"এমেবালা চ্যাট",
    L"এই ভাষার জন্য কোনো Windows ভয়েস ইনস্টল করা নেই. ভয়েস সেটিংস খুলতে 🔊-এ আবার ক্লিক করুন.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"গোপনীয়তা বিজ্ঞপ্তি",
    L"Emebala Chat-এর গোপনীয়তা নীতি নিচে দেওয়া হলো:\n"
    L"\n"
    L"• Emebala Chat-এর নিজস্ব কোনো সার্ভার নেই।\n"
    L"• লোকাল মডেল ব্যবহার করলে অনুবাদিত টেক্সট আপনার ডিভাইস ছেড়ে যায় না।\n"
    L"• আপনি Google Translate নির্বাচন করলে বা স্বয়ংক্রিয়ভাবে ক্লাউডে গেলে, নির্বাচিত/লিখিত টেক্সট অনুবাদের জন্য সরাসরি Google-এ যায় — Emebala-র মাধ্যমে নয়।\n"
    L"• ডায়াগনস্টিক লগ ডিফল্ট বন্ধ (OFF); সেটিংসে চালু করতে হয় (opt-in)।\n"
    L"• আপনি যদি টেক্সট ক্লাউডে (Google) পাঠাতে না চান, সিস্টেম ট্রে আইকনের মেনু খুলুন, “অনুবাদ ইঞ্জিন” থেকে “অন্তর্নির্মিত লোকাল ইঞ্জিন” নির্বাচন করুন। লোকাল মডেল ইনস্টল না থাকলে এবং ক্লাউড ফলব্যাক বন্ধ থাকলে অনুবাদ চলবে না — কিছুই পাঠানো হবে না।\n"
    L"\n"
    L"পূর্ণ বিবরণ README ফাইলে আছে, যেটি যেকোনো সময় আবার পড়া যাবে।\n",
    L"কনফিগ ফাইল: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"পাঠ্যটি খুব দীর্ঘ ছিল: শুধু শুরু এবং শেষ অংশ অনুবাদ করা হয়েছে।",
    L"উপরে নতুন অনুবাদহীন টেক্সট আছে। এটি অনুবাদ করতে কার্সারটি সেই লাইনের শেষে রাখুন এবং Enter চাপুন।",
    L"লোকাল ইঞ্জিন উপাদান মেরামত হচ্ছে…",
    L"লোকাল অনুবাদ পাওয়া যাচ্ছে না",
    L"লোকাল অনুবাদ ইঞ্জিনের ফাইল পাওয়া যায়নি, তাই অনুবাদ স্থগিত হয়েছে। লোকাল ইঞ্জিন পুনরুদ্ধার করতে Emebala Chat পুনরায় ইনস্টল করুন, অথবা ক্লাউড (Google) অনুবাদে যেতে, ট্রে মেনু থেকে “অনুবাদ ইঞ্জিন”-এ “Google অনুবাদ” নির্বাচন করুন।",
    L"OpenAI সামঞ্জস্যপূর্ণ (ব্যবহারকারী-সংজ্ঞায়িত সার্ভার)…",
        L"OpenAI-সংযুক্ত ইঞ্জিন সেটিংস",
        L"OpenAI-সংযুক্ত ইঞ্জিন সেটিংস…",
        L"বেস URL",
        L"API কী",
        L"মডেল",
        L"মডেল তালিকা আনুন",
        L"মডেল তালিকা আনা যায়নি। আপনি সরাসরি মডেলের নাম লিখতে পারেন।",
        L"অনিরাপদ সংযোগ (HTTP)",
        L"বেস URL HTTP (এনক্রিপ্ট করা নয়) ব্যবহার করে। আপনার API কী এবং টেক্সট সাদা টেক্সট হিসেবে পাঠানো হবে। চালিয়ে যাবেন?",
        L"OpenAI-সংযুক্ত সেটিংস সংরক্ষণ করা হয়েছে।",
        L"সংরক্ষিত কী: ",
        L"বেস URL অবৈধ। উদাহরণ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"ব্যবহারকারী-সংজ্ঞায়িত মডেল (.gguf)",
    L"অন্য .gguf মডেল নিবন্ধন করুন…",
        L"অনুবাদের মান সম্পর্কে নোটিশ",
        L"নির্বাচিত মডেল Hy-MT2 নয়। বর্তমান সংস্করণ শুধু Hy-MT2-এর জন্য প্রম্পট ব্যবহার করে, তাই এই মডেলের অনুবাদের মান নিশ্চিত করা যায় না। চালিয়ে যাবেন?",
        L"মডেল নিবন্ধিত",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"নির্বাচিত মডেলটি লোকাল ইঞ্জিনে নিবন্ধিত হয়েছে।\n"
    L"\n"
    L"আপনি \"অনুবাদ ইঞ্জিন নির্বাচন > ব্যবহারকারীর পছন্দ (.gguf)\" বেছে নিলে এই মডেলটি অনুবাদে ব্যবহৃত হয়।\n"
    L"\n"
    L"কখন কার্যকর: নিবন্ধনের পর সর্বোচ্চ প্রায় ১ মিনিট (ইঞ্জিন নিষ্ক্রিয় সমাপ্তির পর)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"এই মডেলটি ইতিমধ্যে Emebala Chat-এ অন্তর্নির্মিত। নিবন্ধনের প্রয়োজন নেই। অন্তর্নির্মিত লোকাল অনুবাদ ইঞ্জিন সরাসরি নির্বাচন করুন।",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(নিবন্ধিত নয়)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"মডেল পরিচালনা…",
    L"ব্যবহারকারী মডেল পরিচালক",
    L"কোনো ব্যবহারকারী মডেল নথিভুক্ত নেই।",
    L"নাম পরিবর্তন…",
    L"মুছুন…",
    L"বন্ধ করুন",
    L"মডেল নিবন্ধন মুছুন",
    L"নির্বাচিত মডেলের নিবন্ধন মুছে ফেলা হবে।\n"
    L"\n"
    L"মডেল ফাইল (.gguf) ডিস্কে থাকবে এবং মুছে ফেলা হবে না। এই মডেলটি ব্যবহারে থাকলে, অনুবাদ ইঞ্জিন নির্বাচন স্বয়ংক্রিয় (Auto)-তে ফিরে যাবে।\n"
    L"\n"
    L"চালিয়ে যাবেন?",
    L"মডেলের নাম পরিবর্তন",
    L"নতুন নাম লিখুন (সর্বোচ্চ ৬৪ অক্ষর, ফাঁক ছাড়া)।",
    L"সেই নাম ব্যবহার করা যাবে না। খালি নয়, বিদ্যমানগুলো থেকে আলাদা, ফাঁক বা পথ বিভাজক ছাড়া, সর্বোচ্চ ৬৪ অক্ষরের নাম লিখুন।",
    L"পরিবর্তন সংরক্ষণ করা হয়েছে।",

    L"registry.json সিরিয়ালাইজ করা যায়নি (একটি ফাইলনাম প্রত্যাখ্যাত)। কিছুই পরিবর্তন করা হয়নি।",
    L"%LOCALAPPDATA% অনুপলব্ধ; ভাগ করা মডেল ফোল্ডার খুঁজে পাওয়া যায়নি।",
    L"registry.json লেখা যায়নি।",
    L"registry.json সম্পূর্ণভাবে লেখা যায়নি।",
    L"registry.json ক্ষতিগ্রস্ত বা এর স্কিমা সমর্থিত নয়। এটি পরিবর্তন করা হয়নি। এটি মেরামত করুন বা মুছুন, তারপর আবার চেষ্টা করুন।",
    L"অনুবাদ ব্যর্থ হয়েছে। আবার চেষ্টা করুন।",
    L"লোকাল অনুবাদ ইঞ্জিনটি সাময়িকভাবে অনুপলব্ধ (হয়তো শুরু হচ্ছে)। একটু অপেক্ষা করে আবার চেষ্টা করুন।",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"ঠিক আছে",
    L"বাতিল",
};

// 31. Arabic (ar) — RTL language; string CONTENT is logical-order UTF-16, the
// UI chrome direction is decided by the render layer (bidi_utils).
const LocalizedStrings kStringsArabic = {
    L"الحالة: نشِط (F9: إيقاف مؤقت)",
    L"الحالة: متوقف مؤقتًا (F9: استئناف)",
    L"محرك الترجمة",
    L"ترجمة Google (مجاني / بدون تثبيت)",
    L"محرك الترجمة المحلي المدمج (Hy-MT2-1.8B دون اتصال)",
    L"لغة المصدر (الإدخال)",
    L"لغة الهدف (الإخراج)",
    L"تبديل المصدر ⇄ الهدف (نقر مزدوج)",
    L"الإرسال التلقائي عند الضغط على Enter",
    L"ردود الفعل الصوتية (نغمات)",
    L"إظهار الشارة العائمة",
    L"التشغيل عند بدء Windows",
    L"مرجع اختصارات لوحة المفاتيح والمساعدة...",
    L"إنهاء إيميبالا شات",
    L"حول إيميبالا شات…",
    L"إيميبالا شات — اختصارات ودليل الاستخدام",
    L"اختصارات إيميبالا شات ودليل الاستخدام:\n\n"
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
    L"حول إيميبالا شات",
    L"نشِط",
    L"جارٍ الترجمة...",
    L"متوقف مؤقتًا",
    L"إيميبالا شات",
    L"تعذّر نسخ النص المحدد. تحقق من التطبيق الهدف وأعد المحاولة.",
    L"لم يتم تحديد أي نص لترجمته.",
    L"كشف تلقائي",
    L"إيميبالا شات يعمل بالفعل في الخلفية.\nتحقق من منطقة إشعارات النظام.",
    L"فشل تهيئة COM.\nلن تتوافر الشارة العائمة وقراءة النص بصوت مسموع،\nلكن الترجمة والاختصارات والإشعارات والأصوات ستواصل العمل.",
    L"ودِّع النسخ واللصق. اكتب بلغتك الأم بطبيعتك — تحل الترجمة محل كتابتك في الوقت الفعلي داخل أي تطبيق من تطبيقات Windows.",
    L"⚡ ترجم بالسحب — حدّد النص في أي تطبيق، فتُترجمه الأيقونة العائمة فورًا.",
    L"🔊 تحويل نص إلى كلام عصبي — ينطق بجميع اللغات الـ37 عبر حزم الأصوات المثبتة في Windows.",
    L"🔒 يعمل محليًا وبخصوصية 100% — يتفعّل أثناء ضغط الاختصار فحسب، دون المساس بالحافظة.",
    L"في عام 2000 قبل الميلاد، أطلق كتّاب بلاد الرافدين اسم «Eme-bala» على من يجعلون من اللغة جسرًا بين العوالم.",
    L"الموقع الإلكتروني",
    L"اتصل بنا",
    L"Reddit",
    L"فريق سان بلازا · سيول يونغدونغبو (الغرفة 219، 65 يونغجونغ-رو)",
    L"+82 2 575 0414 · ساعات العمل 10:00–19:00 KST",
    L"كبير المهندسين: Yongtai Kim",
    L"✓ تم النسخ!",
    L"📋 نسخ",
    L"🔊 استماع",
    L"لغة الواجهة",
    L"تلقائي (لغة النظام)",
    L"إعادة التعيين إلى إعدادات النظام الافتراضية",
    L"تمت استعادة الإعدادات الافتراضية",
    L"الكتابة بلوحة المفاتيح",
    L"تلميح السحب",
    L"إيميبالا شات",
    L"لا يوجد صوت Windows مثبت لهذه اللغة. انقر فوق 🔊 مرة أخرى لفتح إعدادات الكلام.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"إشعار الخصوصية",
    L"هذه مبادئ الخصوصية في Emebala Chat.\n"
    L"\n"
    L"• لا يشغّل Emebala Chat أي خوادم خاصة به.\n"
    L"• عند استخدام النموذج المحلي، لا يغادر النص المترجم جهازك.\n"
    L"• إذا اخترت ترجمة Google أو تم التبديل التلقائي إلى السحابة، يُرسَل النص المحدَّد أو المكتوب مباشرة إلى Google للترجمة، وليس عبر Emebala.\n"
    L"• سجلات التشخيص معطّلة افتراضيًا؛ تفعّلها من الإعدادات (بموافقتك).\n"
    L"• إذا لم ترغب في إرسال النص إلى السحابة (Google)، افتح قائمة أيقونة شريط المهام واختر “محرك الترجمة” ثم “محرك الترجمة المحلي المدمج”. إذا لم يكن النموذج المحلي مثبتًا وكان التحويل السحابي معطلًا، فلن تعمل الترجمة ولن يُرسل أي شيء.\n"
    L"\n"
    L"التفاصيل الكاملة في ملف README ويمكن قراءته في أي وقت.\n",
    L"ملف الإعداد: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"النص طويل جدًا: تمت ترجمة البداية والنهاية فقط.",
    L"يوجد نص جديد غير مترجم في الأعلى. ضع المؤشر في نهاية ذلك السطر واضغط Enter للترجمة.",
    L"جارٍ إصلاح مكونات المحرك المحلي…",
    L"الترجمة المحلية غير متاحة",
    L"ملفات محرك الترجمة المحلي مفقودة، لذا تم إيقاف الترجمة مؤقتًا. أعد تثبيت Emebala Chat لاستعادة المحرك المحلي، أو للتبديل إلى ترجمة السحابة (Google)، اختر “Google ترجمة” من قائمة الشريط ضمن “محرك الترجمة”.",
    L"متوافق مع OpenAI (خادم مخصص)…",
        L"إعدادات المحرك المتوافق مع OpenAI",
        L"إعدادات المحرك المتوافق مع OpenAI…",
        L"عنوان URL الأساسي",
        L"مفتاح API",
        L"النموذج",
        L"جلب قائمة النماذج",
        L"تعذر جلب قائمة النماذج. يمكنك كتابة اسم النموذج مباشرة.",
        L"اتصال غير آمن (HTTP)",
        L"يستخدم عنوان URL الأساسي HTTP (غير مشفر). سيتم إرسال مفتاح API والنص بشكل غير مشفر. هل تريد المتابعة؟",
        L"تم حفظ الإعدادات المتوافقة مع OpenAI.",
        L"المفتاح المحفوظ: ",
        L"عنوان URL الأساسي غير صالح. مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"النموذج المحدد من قبل المستخدم (.gguf)",
    L"تسجيل نموذج .gguf آخر…",
        L"إشعار بشأن جودة الترجمة",
        L"النموذج المحدد ليس Hy-MT2. تستخدم النسخة الحالية موجه (prompt) خاصًا بـ Hy-MT2 فقط، لذا لا تُضمن جودة الترجمة بهذا النموذج. هل تريد المتابعة؟",
        L"تم تسجيل النموذج",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"تم تسجيل النموذج المحدد في المحرك المحلي.\n"
    L"\n"
    L"يُستخدم هذا النموذج للترجمة عند اختيار \"اختيار محرك الترجمة > اختيار المستخدم (.gguf)\".\n"
    L"\n"
    L"متى يسري: خلال دقيقة واحدة تقريبًا كحد أقصى بعد التسجيل (بعد خروج المحرك بسبب الخمول)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"هذا النموذج مدمج بالفعل في Emebala Chat. لا حاجة إلى التسجيل. يمكنك اختيار محرك الترجمة المحلي المدمج مباشرةً.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(غير مسجل)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"إدارة النماذج…",
    L"مدير نماذج المستخدم",
    L"لا توجد نماذج مستخدم مسجّلة.",
    L"إعادة تسمية…",
    L"حذف…",
    L"إغلاق",
    L"حذف تسجيل النموذج",
    L"سيتم حذف تسجيل النموذج المحدد.\n"
    L"\n"
    L"ملف النموذج (.gguf) يبقى على القرص ولا يُحذف. إذا كان هذا النموذج قيد الاستخدام، فسيعود اختيار محرك الترجمة إلى تلقائي (Auto).\n"
    L"\n"
    L"هل تريد المتابعة؟",
    L"إعادة تسمية النموذج",
    L"أدخل اسمًا جديدًا (بحد أقصى 64 حرفًا، بدون مسافات).",
    L"لا يمكن استخدام هذا الاسم. أدخل اسمًا غير فارغ ومختلف عن الأسماء الموجودة، بدون مسافات أو فواصل مسار، وضمن 64 حرفًا.",
    L"تم حفظ التغييرات.",

    L"تعذر تسلسل registry.json (تم رفض اسم ملف). لم يتغير شيء.",
    L"%LOCALAPPDATA% غير متاح؛ لا يمكن تحديد موقع مجلد النماذج المشترك.",
    L"تعذر كتابة registry.json.",
    L"تعذر كتابة registry.json بالكامل.",
    L"registry.json تالف أو يحتوي على مخطط غير مدعوم. لم يتم تعديله. أصلحه أو احذفه ثم أعد المحاولة.",
    L"فشلت الترجمة. يرجى المحاولة مرة أخرى.",
    L"محرك الترجمة المحلي غير متاح مؤقتًا (قد يكون قيد التشغيل). يرجى الانتظار قليلاً والمحاولة مرة أخرى.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"موافق",
    L"إلغاء",
};

// 32. Persian (fa) — RTL
const LocalizedStrings kStringsPersian = {
    L"وضعیت: فعال (F9: توقف موقت)",
    L"وضعیت: متوقف (F9: ادامه)",
    L"موتور ترجمه",
    L"Google Translate (رایگان / بدون نصب)",
    L"موتور ترجمه محلی داخلی (Hy-MT2-1.8B آفلاین)",
    L"زبان مبدأ (ورودی)",
    L"زبان مقصد (خروجی)",
    L"تعویض مبدأ ⇄ مقصد (دوبار کلیک)",
    L"ارسال خودکار با Enter",
    L"بازخورد صوتی (تُن‌ها)",
    L"نمایش نشان شناور",
    L"شروع با ویندوز",
    L"برگه تقلبی کلیدهای میان‌بر و راهنما...",
    L"خروج از امبالا چت",
    L"دربارهٔ امبالا چت…",
    L"کلیدهای میان‌بر و راهنمای امبالا چت",
    L"کلیدهای میان‌بر و راهنمای استفاده از امبالا چت:\n\n"
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
    L"دربارهٔ امبالا چت",
    L"فعال",
    L"در حال ترجمه...",
    L"متوقف",
    L"امبالا چت",
    L"متن انتخابی کپی نشد. برنامهٔ مقصد را بررسی کنید و دوباره تلاش کنید.",
    L"متنی برای ترجمه انتخاب نشده است.",
    L"تشخیص خودکار",
    L"امبالا چت هم‌اکنون در پس‌زمینه در حال اجراست.\nسینی اعلان‌های سیستم را بررسی کنید.",
    L"مقداردهی COM ناموفق بود.\nنشان شناور و متن‌به‌کلام در دسترس نخواهند بود،\nاما ترجمه، میان‌برها، سینی و صداها همچنان کار می‌کنند.",
    L"کپی-پیست را فراموش کنید. به زبان مادری‌تان طبیعی تایپ کنید — ترجمه در هر برنامهٔ ویندوزی در زمان واقعی جای تایپ شما می‌نشیند.",
    L"⚡ بکشید تا ترجمه شود — متنی را در هر برنامه‌ای انتخاب کنید، نماد شناور فوراً ترجمه می‌کند.",
    L"🔊 TTS عصبی — تلفظ هر ۳۷ زبان از طریق بسته‌های صدای نصب‌شده Windows.",
    L"🔒 ۱۰۰٪ روی دستگاه و خصوصی — فقط هنگام فشردن میان‌بر فعال است؛ کلیپ‌بورد دست‌نخورده.",
    L"در سال ۲۰۰۰ پیش از میلاد، کاتبان بین‌النهرین «Eme-bala» می‌خواندند آنان را که زبان را پل میان جهان‌ها می‌کردند.",
    L"وب‌سایت",
    L"تماس",
    L"Reddit",
    L"تیم سانپلازا · سئول یونگ‌دونگ‌پو (اتاق 219، 65 یونگ‌جونگ-رو)",
    L"+82 2 575 0414 · ساعت کاری 10:00–19:00 KST",
    L"معمار ارشد: Yongtai Kim",
    L"✓ کپی شد!",
    L"📋 کپی",
    L"🔊 گفتار",
    L"زبان رابط کاربری",
    L"خودکار (زبان سیستم)",
    L"بازنشانی به پیش‌فرض‌های سیستم",
    L"پیش‌فرض‌ها بازگردانده شدند",
    L"تایپ با کیبورد",
    L"راهنمای ابزار کشیدن",
    L"امبالا چت",
    L"هیچ صدای Windows برای این زبان نصب نشده است. برای باز کردن تنظیمات گفتار، دوباره روی 🔊 کلیک کنید.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"اعلان حریم خصوصی",
    L"اصول حریم خصوصی در Emebala Chat:\n"
    L"\n"
    L"• Emebala Chat سرور اختصاصی ندارد.\n"
    L"• با مدل محلی، متن ترجمه‌شده از دستگاه شما خارج نمی‌شود.\n"
    L"• اگر Google Translate را انتخاب کنید یا تبدیل خودکار به ابری رخ دهد، متن انتخابی یا تایپ‌شده برای ترجمه مستقیماً به Google ارسال می‌شود؛ نه از طریق Emebala.\n"
    L"• گزارش‌های تشخیصی به‌طور پیش‌فرض خاموش‌اند؛ از تنظیمات روشن می‌شوند (با انتخاب شما).\n"
    L"• اگر نمی‌خواهید متن به ابر (Google) ارسال شود، منوی نماد نوار وظیفه را باز کنید، “موتور ترجمه” و سپس “موتور ترجمه محلی داخلی” را انتخاب کنید. بدون نصب مدل محلی و با غیرفعال بودن جایگزین ابری، ترجمه انجام نمی‌شود — چیزی ارسال نمی‌شود.\n"
    L"\n"
    L"جزئیات کامل در فایل README است و هر زمان قابل خواندن مجدد است.\n",
    L"فایل پیکربندی: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"متن بسیار بلند بود: فقط ابتدا و انتها ترجمه شد.",
    L"در بالا متن جدیدی بدون ترجمه وجود دارد. برای ترجمه، مکان‌نما را در انتهای آن خط قرار دهید و Enter را بزنید.",
    L"در حال تعمیر اجزای موتور محلی…",
    L"ترجمه محلی در دسترس نیست",
    L"فایل‌های موتور ترجمه محلی پیدا نشدند، بنابراین ترجمه متوقف شده است. برای بازیابی موتور محلی، Emebala Chat را دوباره نصب کنید، یا برای تغییر به ترجمه ابری (Google)، از منوی سینی، «موتور ترجمه»، «Google ترجمه» را انتخاب کنید.",
    L"سازگار با OpenAI (سرور سفارشی)…",
        L"تنظیمات موتور سازگار با OpenAI",
        L"تنظیمات موتور سازگار با OpenAI…",
        L"آدرس پایه",
        L"کلید API",
        L"مدل",
        L"دریافت فهرست مدل‌ها",
        L"دریافت فهرست مدل‌ها ناموفق بود. می‌توانید نام مدل را مستقیماً تایپ کنید.",
        L"اتصال ناامن (HTTP)",
        L"آدرس پایه از HTTP (رمزنگاری‌نشده) استفاده می‌کند. کلید API و متن شما به‌صورت رمزنگاری‌نشده ارسال خواهد شد. ادامه می‌دهید؟",
        L"تنظیمات سازگار با OpenAI ذخیره شد.",
        L"کلید ذخیره‌شده: ",
        L"آدرس پایه معتبر نیست. مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"مدل تعیین شده توسط کاربر (.gguf)",
    L"ثبت مدل .gguf دیگر…",
        L"اطلاعیه درباره کیفیت ترجمه",
        L"مدل انتخاب‌شده Hy-MT2 نیست. نسخه فعلی از پرامپت مخصوص Hy-MT2 استفاده می‌کند، بنابراین کیفیت ترجمه با این مدل تضمین نمی‌شود. ادامه می‌دهید؟",
        L"مدل ثبت شد",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"مدل انتخاب‌شده در موتور محلی ثبت شد.\n"
    L"\n"
    L"این مدل هنگامی برای ترجمه استفاده می‌شود که \"انتخاب موتور ترجمه > انتخاب کاربر (.gguf)\" را برگزینید.\n"
    L"\n"
    L"چه زمانی اعمال می‌شود: حداکثر حدود ۱ دقیقه پس از ثبت (پس از خروج موتور به دلیل عدم فعالیت)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"این مدل از قبل در Emebala Chat داخلی شده است. نیازی به ثبت‌نام نیست. می‌توانید موتور ترجمه محلی داخلی را مستقیماً انتخاب کنید.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ثبت نشده)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"مدیریت مدل‌ها…",
    L"مدیریت مدل‌های کاربر",
    L"هیچ مدل کاربری ثبت نشده است.",
    L"تغییر نام…",
    L"حذف…",
    L"بستن",
    L"حذف ثبت مدل",
    L"ثبت مدل انتخاب‌شده حذف خواهد شد.\n"
    L"\n"
    L"فایل مدل (.gguf) روی دیسک باقی می‌ماند و حذف نمی‌شود. اگر از این مدل استفاده می‌شد، انتخاب موتور ترجمه به حالت خودکار (Auto) بازمی‌گردد.\n"
    L"\n"
    L"ادامه می‌دهید؟",
    L"تغییر نام مدل",
    L"نام جدید را وارد کنید (حداکثر ۶۴ نویسه، بدون فاصله).",
    L"این نام قابل استفاده نیست. نامی غیرخالی، متفاوت از نام‌های موجود، بدون فاصله یا جداکنندهٔ مسیر و حداکثر ۶۴ نویسه وارد کنید.",
    L"تغییرات ذخیره شد.",

    L"registry.json قابل سریال‌سازی نیست (نام فایل رد شد). هیچ چیز تغییر نکرد.",
    L"%LOCALAPPDATA% در دسترس نیست؛ پوشه مشترک مدل‌ها پیدا نشد.",
    L"registry.json نوشته نشد.",
    L"registry.json به‌طور کامل نوشته نشد.",
    L"registry.json آسیب دیده یا دارای طرح پشتیبانی‌نشده است. تغییر داده نشده است. آن را تعمیر یا حذف کنید و دوباره تلاش کنید.",
    L"ترجمه ناموفق بود. دوباره تلاش کنید.",
    L"موتور ترجمه محلی به‌طور موقت در دسترس نیست (ممکن است در حال راه‌اندازی باشد). کمی صبر کنید و دوباره تلاش کنید.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"تأیید",
    L"انصراف",
};

// 33. Urdu (ur) — RTL
const LocalizedStrings kStringsUrdu = {
    L"حالت: فعال (F9: وقفہ)",
    L"حالت: وقفے میں (F9: جاری رکھیں)",
    L"ترجمہ انجن",
    L"Google ٹرانسلیٹ (مفت / بغیر تنصیب)",
    L"بلٹ اِن مقامی انجن (Hy-MT2-1.8B آف لائن)",
    L"ماخذ زبان (ان پٹ)",
    L"ہدف زبان (آؤٹ پٹ)",
    L"ماخذ ⇄ ہدف بدلیں (ڈبل کلک)",
    L"Enter پر خودکار ارسال",
    L"آواز کا ردعمل (ٹونز)",
    L"فلوٹنگ بیج دکھائیں",
    L"Windows کے ساتھ شروع",
    L"شارٹ کٹ چِٹ شیٹ اور مدد...",
    L"ایمیبالا چیٹ سے باہر نکلیں",
    L"ایمیبالا چیٹ کے بارے میں…",
    L"ایمیبالا چیٹ — شارٹ کٹس اور استعمال گائیڈ",
    L"ایمیبالا چیٹ شارٹ کٹس اور استعمال گائیڈ:\n\n"
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
    L"ایمیبالا چیٹ کے بارے میں",
    L"فعال",
    L"ترجمہ جاری...",
    L"وقفہ",
    L"ایمیبالا چیٹ",
    L"منتخب متن کاپی نہیں ہو سکا۔ ہدف ایپ چیک کریں اور دوبارہ کوشش کریں۔",
    L"ترجمے کے لیے کوئی متن منتخب نہیں ہے۔",
    L"خودکار شناخت",
    L"ایمیبالا چیٹ پہلے سے پس منظر میں چل رہا ہے۔\nسسٹم نوٹیفکیشن ٹرے کو دیکھیں۔",
    L"COM آغاز ناکام۔\nفلوٹنگ بیج اور متن از آواز دستیاب نہیں ہوں گے،\nلیکن ترجمہ، شارٹ کٹس، ٹرے اور آوازیں چلتی رہیں گی۔",
    L"کاپی پیسٹ کی ضرورت ختم۔ اپنی مادری زبان میں قدرتی ٹائپ کریں — ترجمہ کسی بھی Windows ایپ میں آپ کی ٹائپنگ کو وقتی طور پر بدل دیتا ہے۔",
    L"⚡ کھینچیں اور ترجمہ کریں — کسی بھی ایپ میں متن منتخب کریں، فلوٹنگ آئیکن فوراً ترجمہ کر دیتا ہے۔",
    L"🔊 نیورل TTS — انسٹال شد Windows وائس پیکس کے ذریعے تمام 37 زبانوں کا تلفظ۔",
    L"🔒 100% ڈیوائس پر اور نجی — جب تک شارٹ کٹ دبا رہے تب تک فعال؛ کلپ بورڈ کو ہاتھ نہیں لگاتا۔",
    L"2000 ق م، میسوپوٹیمیا کے کاتبوں نے انہیں «Eme-bala» کہا جنہوں نے زبان کو دنیاؤں کے درمیان پل بنایا۔",
    L"ویب سائٹ",
    L"رابطہ",
    L"Reddit",
    L"ٹیم سنپلازہ · سول یونگڈونگپو (کمرہ 219، 65 یونگجنگ-رو)",
    L"+82 2 575 0414 · آفس اوقات 10:00–19:00 KST",
    L"لیڈ آرکیٹیکٹ: Yongtai Kim",
    L"✓ کاپی ہو گیا!",
    L"📋 کاپی",
    L"🔊 آواز",
    L"انٹرفیس زبان",
    L"خودکار (سسٹم زبان)",
    L"سسٹم ڈیفالٹس پر ری سیٹ",
    L"ڈیفالٹس بحال ہو گئے",
    L"کی بورڈ ٹائپنگ",
    L"ڈریگ ٹول ٹپ",
    L"ایمیبالا چیٹ",
    L"اس زبان کے لیے کوئی Windows وائس انسٹال نہیں ہے۔ اسپیچ سیٹنگز کھولنے کے لیے 🔊 پر دوبارہ کلک کریں۔",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"رازداری کا اعلان",
    L"Emebala Chat کے رازداری کے اصول:\n"
    L"\n"
    L"• Emebala Chat کا اپنا کوئی سرور نہیں ہے۔\n"
    L"• لوکل ماڈل پر ترجمہ شدہ متن آپ کے ڈیوائس سے باہر نہیں جاتا۔\n"
    L"• اگر آپ Google Translate منتخب کریں یا خودکار طور پر کلاؤڈ پر سوئچ ہو، تو منتخب/ٹائپ شدہ متن ترجمے کے لیے براہ راست Google کو جاتا ہے — Emebala کے ذریعے نہیں۔\n"
    L"• ڈائگناسٹک لاگ ڈیفالٹ طور پر بند (OFF) ہیں؛ سیٹنگز میں آن کریں (opt-in)۔\n"
    L"• اگر آپ متن کلاؤڈ (Google) میں نہیں بھیجنا چاہتے تو سسٹم ٹری آئیکن کا مینو کھولیں، “ترجمہ انجن” میں سے “بلٹ اِن مقامی انجن” منتخب کریں۔ اگر مقامی ماڈل انسٹال نہیں ہے اور کلاؤڈ فال بیک بند ہے تو ترجمہ نہیں چلے گا — کچھ بھی نہیں بھیجا جائے گا۔\n"
    L"\n"
    L"مکمل تفصیل README فائل میں ہے، جسے آپ کسی بھی وقت دوبارہ پڑھ سکتے ہیں۔\n",
    L"کنفیگ فائل: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"متن بہت لمبا تھا: صرف آغاز اور انجام کا ترجمہ کیا گیا۔",
    L"اوپر نیا غیر ترجمہ شدہ متن ہے۔ اس کا ترجمہ کرنے کے لیے کرسر کو اس لائن کے آخر میں رکھیں اور Enter دبائیں۔",
    L"مقامی انجن اجزا کی مرمت جاری ہے…",
    L"مقامی ترجمہ دستیاب نہیں ہے",
    L"مقامی ترجمہ انجن کی فائلیں نہیں ملیں، اس لیے ترجمہ روک دیا گیا ہے۔ مقامی انجن کو بحال کرنے کے لیے Emebala Chat کو دوبارہ انسٹال کریں، یا کلاؤڈ (Google) ترجمے پر جانے کے لیے، ٹرے مینیو سے “ترجمہ انجن” میں “Google ترجمہ” منتخب کریں۔",
    L"OpenAI مطابقت (صارف کی وضاحت کردہ سرور)…",
        L"OpenAI مطابق انجن سیٹنگز",
        L"OpenAI مطابق انجن سیٹنگز…",
        L"بیس URL",
        L"API کلید",
        L"ماڈل",
        L"ماڈل فہرست حاصل کریں",
        L"ماڈل فہرست حاصل نہ ہو سکی۔ آپ ماڈل کا نام براہ راست لکھ سکتے ہیں۔",
        L"غیر محفوظ کنکشن (HTTP)",
        L"بیس URL HTTP (غیر خفیہ) استعمال کرتا ہے۔ آپ کی API کلید اور متن سادہ متن میں بھیجا جائے گا۔ جاری رکھیں؟",
        L"OpenAI مطابق سیٹنگز محفوظ کر دی گئیں۔",
        L"محفوظ کلید: ",
        L"بیس URL درست نہیں۔ مثال: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"صارف کی وضاحت کردہ ماڈل (.gguf)",
    L"کوئی اور .gguf ماڈل رجسٹر کریں…",
        L"ترجمے کے معیار کا نوٹس",
        L"منتخب ماڈل Hy-MT2 نہیں ہے۔ موجودہ ورژن صرف Hy-MT2 کے لیے پرامپٹ استعمال کرتا ہے، اس لیے اس ماڈل کے ترجمے کی کوالٹی ضمانت نہیں۔ جاری رکھیں؟",
        L"ماڈل رجسٹر ہو گیا",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"منتخب ماڈل لوکل انجن میں رجسٹر کر دیا گیا ہے۔\n"
    L"\n"
    L"جب آپ \"ترجمہ انجن کا انتخاب > صارف کا انتخاب (.gguf)\" چنتے ہیں تو اس ماڈل کا ترجمے میں استعمال ہوتا ہے۔\n"
    L"\n"
    L"کب لاگو: رجسٹریشن کے بعد زیادہ سے زیادہ تقریباً 1 منٹ (انجن کے غیر فعال ہونے کے بعد)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"یہ ماڈل پہلے سے ہی Emebala Chat میں شامل ہے۔ رجسٹریشن کی ضرورت نہیں ہے۔ بلٹ اِن لوکل ترجمہ انجن کو براہ راست منتخب کریں۔",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(رجسٹرڈ نہیں)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"ماڈلز منظم کریں…",
    L"صارف ماڈل منیجر",
    L"کوئی صارف ماڈل رجسٹرڈ نہیں۔",
    L"نام تبدیل کریں…",
    L"حذف کریں…",
    L"بند کریں",
    L"ماڈل رجسٹریشن حذف کریں",
    L"منتخب ماڈل کی رجسٹریشن حذف کر دی جائے گی۔\n"
    L"\n"
    L"ماڈل فائل (.gguf) ڈسک پر محفوظ رہتی ہے اور حذف نہیں ہوتی۔ اگر یہ ماڈل استعمال ہو رہا تھا، تو ترجمہ انجن کا انتخاب خودکار (Auto) پر واپس آ جائے گا۔\n"
    L"\n"
    L"جاری رکھیں؟",
    L"ماڈل کا نام تبدیل کریں",
    L"نیا نام درج کریں (زیادہ سے زیادہ 64 حروف، بغیر خالی جگہ کے)۔",
    L"وہ نام استعمال نہیں ہو سکتا۔ کوئی خالی نہیں، موجودہ ناموں سے مختلف، بغیر خالی جگہ یا راستہ جداکننے والے، 64 حروف کے اندر نام درج کریں۔",
    L"تبدیلیاں محفوظ کر دی گئیں۔",

    L"registry.json کو سیریلائز نہیں کیا جا سکا (فائل کا نام مسترد کر دیا گیا)۔ کچھ نہیں بدلا۔",
    L"%LOCALAPPDATA% دستیاب نہیں؛ مشترکہ ماڈل فولڈر نہیں مل سکا۔",
    L"registry.json نہیں لکھی جا سکی۔",
    L"registry.json مکمل نہیں لکھی جا سکی۔",
    L"registry.json خراب ہے یا اس کا اسکیما معاونت یافتہ نہیں۔ اسے تبدیل نہیں کیا گیا۔ اسے ٹھیک کریں یا حذف کریں اور دوبارہ کوشش کریں۔",
    L"ترجمہ ناکام رہا۔ دوبارہ کوشش کریں۔",
    L"لوکل ترجمہ انجن عارضی طور پر دستیاب نہیں (ہو سکتا ہے شروع ہو رہا ہو)۔ تھوڑی دیر انتظار کریں اور دوبارہ کوشش کریں۔",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"ٹھیک ہے",
    L"منسوخ",
};

// 34. Hebrew (he) — RTL
const LocalizedStrings kStringsHebrew = {
    L"סטטוס: פעיל (F9: השהיה)",
    L"סטטוס: מושהה (F9: המשך)",
    L"מנוע תרגום",
    L"Google Translate (חינם / ללא התקנה)",
    L"מנוע תרגום מקומי מובנה (Hy-MT2-1.8B לא מקוון)",
    L"שפת מקור (קלט)",
    L"שפת יעד (פלט)",
    L"החלפת מקור ⇄ יעד (לחיצה כפולה)",
    L"שליחה אוטומטית ב-Enter",
    L"משוב קולי (צלילים)",
    L"תג צף גלוי",
    L"הפעל עם Windows",
    L"מדריך קיצורי מקשים ועזרה...",
    L"צא מ-אמבאלה צ'אט",
    L"אודות אמבאלה צ'אט…",
    L"אמבאלה צ'אט — קיצורי מקשים ומדריך שימוש",
    L"קיצורי המקשים ומדריך השימוש של אמבאלה צ'אט:\n\n"
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
    L"אודות אמבאלה צ'אט",
    L"פעיל",
    L"מתרגם...",
    L"מושהה",
    L"אמבאלה צ'אט",
    L"לא ניתן להעתיק את הטקסט הנבחר. בדוק את אפליקציית היעד ונסה שוב.",
    L"לא נבחר טקסט לתרגום.",
    L"זיהוי אוטומטי",
    L"אמבאלה צ'אט כבר פועל ברקע.\nבדוק את מגש ההודעות.",
    L"אתחול COM נכשל.\nהתג הצף והקראת טקסט לא יהיו זמינים,\nאך תרגום, קיצורי מקשים, מגש וצלילים ימשיכו לפעול.",
    L"להיפרד מהעתקה והדבקה. הקלדו בטבעיות בשפת האם — התרגום מחליף את ההקלדה בזמן אמת בכל אפליקציית Windows.",
    L"⚡ גררו ותרגמו — סמנו טקסט בכל אפליקציה, הסמל הצף מתרגם מיד.",
    L"🔊 TTS עצבי — מדבר את כל 37 השפות באמצעות חבילות קול של Windows מותקנות.",
    L"🔒 100% מקומי ופרטי — פעיל רק בעוד קיצור המקשים מוחזק; לוח גזירים נשאר נקי.",
    L"בשנת 2000 לפנה\"ס כינו סופרי מסופוטמיה «Eme-bala» — אלה שהפכו שפה לגשר בין עולמות.",
    L"אתר אינטרנט",
    L"יצירת קשר",
    L"Reddit",
    L"Team Sunplaza · סאול יונגדונגפו (Room 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · שעות משרד 10:00–19:00 KST",
    L"ארכיטקט מוביל: Yongtai Kim",
    L"✓ הועתק!",
    L"📋 העתק",
    L"🔊 הקראה",
    L"שפת הממשק",
    L"אוטומטי (שפת המערכת)",
    L"איפוס לברירות מחדל של המערכת",
    L"ברירות המחדל שוחזרו",
    L"הקלדה במקלדת",
    L"רמז גרירה",
    L"אמבאלה צ'אט",
    L"אין קול Windows מותקן עבור שפה זו. לחץ שוב על 🔊 כדי לפתוח את הגדרות הדיבור.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"הודעת פרטיות",
    L"עקרונות הפרטיות של Emebala Chat:\n"
    L"\n"
    L"• ל־Emebala Chat אין שרתים משלה.\n"
    L"• במודל מקומי, הטקסט המתורגם אינו עוזב את המכשיר שלך.\n"
    L"• אם תבחר ב־Google Translate או שהמעבר לענן יתבצע אוטומטית, הטקסט הנבחר או המוקלד נשלח ישירות ל־Google לצורך תרגום, ולא דרך Emebala.\n"
    L"• יומני אבחון כבויים כברירת מחדל; הפעל אותם בהגדרות (הסכמה מפורשת).\n"
    L"• אם אינך רוצה לשלוח טקסט לענן (Google), פתח את תפריט סמל מגש המערכת, בחר “מנוע תרגום” ואז “מנוע תרגום מקומי מובנה”. ללא מודל מקומי מותקן ועם גיבוי ענן מושבת, התרגום לא יפעל — שום דבר לא נשלח.\n"
    L"\n"
    L"הפירוט המלא בקובץ README, הניתן לקריאה חוזרת בכל עת.\n",
    L"קובץ ההגדרות: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"הטקסט ארוך מדי: רק הפתיח והסיום תורגמו.",
    L"יש טקסט חדש שלא תורגם למעלה. מקם את הסמן בסוף השורה ההיא ולחץ Enter כדי לתרגם.",
    L"מתקן את רכיבי המנוע המקומי…",
    L"התרגום המקומי אינו זמין",
    L"קבצי מנוע התרגום המקומי חסרים, לכן התרגום הושהה. התקינו מחדש את Emebala Chat כדי לשחזר את המנוע המקומי, או כדי לעבור לתרגום בענן (Google), בחרו “Google תרגום” בתפריט השורה, “מנוע תרגום”.",
    L"תואם OpenAI (שרת מותאם אישית)…",
        L"הגדרות מנוע תואם OpenAI",
        L"הגדרות מנוע תואם OpenAI…",
        L"כתובת בסיס",
        L"מפתח API",
        L"מודל",
        L"משוך רשימת מודלים",
        L"לא ניתן למשוך את רשימת המודלים. אפשר להקליד שם מודל ישירות.",
        L"חיבור לא מאובטח (HTTP)",
        L"כתובת הבסיס משתמשת ב־HTTP (לא מוצפן). מפתח ה־API והטקסט שלך יישלחו כטקסט גלוי. להמשיך?",
        L"הגדרות תואמות OpenAI נשמרו.",
        L"מפתח שמור: ",
        L"כתובת הבסיס אינה חוקית. דוגמה: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"מודל שצוין על ידי המשתמש (.gguf)",
    L"רישום מודל .gguf אחר…",
        L"הודעה על איכות התרגום",
        L"המודל שנבחר אינו Hy-MT2. הגרסה הנוכחית משתמשת בפרומפט ייעודי ל־Hy-MT2 בלבד, ולכן איכות התרגום עם מודל זה אינה מובטחת. להמשיך?",
        L"המודל נרשם",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"המודל שנבחר נרשם במנוע המקומי.\n"
    L"\n"
    L"מודל זה משמש לתרגום כאשר בוחרים \"בחירת מנוע תרגום > בחירת משתמש (.gguf)\".\n"
    L"\n"
    L"מתי זה נכנס לתוקף: עד כדקה אחת לאחר ההרשמה (לאחר סיום פעולת המנוע עקב חוסר פעילות)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"מודל זה כבר מובנה בתוך Emebala Chat. אין צורך ברישום. ניתן לבחור ישירות את מנוע התרגום המקומי המובנה.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(לא רשום)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"ניהול מודלים…",
    L"מנהל מודלים של המשתמש",
    L"אין מודלים של משתמש רשומים.",
    L"שינוי שם…",
    L"מחיקה…",
    L"סגירה",
    L"מחיקת רישום המודל",
    L"רישום המודל שנבחר יימחק.\n"
    L"\n"
    L"קובץ המודל (.gguf) נשמר על הדיסק ואינו נמחק. אם המודל היה בשימוש, הבחירה במנוע התרגום תחזור ל־אוטומטי (Auto).\n"
    L"\n"
    L"להמשיך?",
    L"שינוי שם המודל",
    L"הזן שם חדש (עד 64 תווים, ללא רווחים).",
    L"אי אפשר להשתמש בשם הזה. הזן שם שאינו ריק, שונה מהקיימים, ללא רווחים או מפרידי נתיב, בעד 64 תווים.",
    L"השינויים נשמרו.",

    L"לא ניתן היה לסריאליזם את registry.json (שם קובץ נדחה). לא שונה דבר.",
    L"%LOCALAPPDATA% אינו זמין; לא ניתן לאתר את תיקיית המודלים המשותפת.",
    L"לא ניתן היה לכתוב את registry.json.",
    L"registry.json לא נכתב במלואו.",
    L"registry.json פגום או בעל סכימה לא נתמכת. הוא לא שונה. תקנו או הסירו אותו ונסו שוב.",
    L"התרגום נכשל. נסה שוב.",
    L"מנוע התרגום המקומי אינו זמין זמנית (ייתכן שהוא בתהליך הפעלה). המתן רגע ונסה שוב.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"אישור",
    L"ביטול",
};

// 35. Khmer (km)
const LocalizedStrings kStringsKhmer = {
    L"ស្ថានភាព៖ សកម្ម (F9៖ ផ្អាក)",
    L"ស្ថានភាព៖ បានផ្អាក (F9៖ បន្ត)",
    L"ម៉ាស៊ីនបកប្រែ",
    L"Google បកប្រែ (ឥតគិតថ្លៃ / គ្មានការដំឡើង)",
    L"ម៉ាស៊ីនក្នុងស្រាប់ (Hy-MT2-1.8B ក្រៅបណ្តាញ)",
    L"ភាសាប្រភព (បញ្ចូល)",
    L"ភាសាគោលដៅ (បញ្ចេញ)",
    L"ប្ដូរប្រភព ⇄ គោលដៅ (ចុចទ្វេដង)",
    L"ផ្ញើស្វ័យប្រវត្តិពេលចុច Enter",
    L"ការឆ្លើយតបជាសំឡេង (តូន)",
    L"បង្ហាញផ្លាកអណ្ដែត",
    L"ដំណើរការជាមួយ Windows",
    L"តារាងគន្លឹះក្ដារចុច និងជំនួយ...",
    L"ចាកចេញពី អេមេបាឡា ឆាត",
    L"អំពី អេមេបាឡា ឆាត…",
    L"គន្លឹះ និងការណ៍នាំប្រើ អេមេបាឡា ឆាត",
    L"គន្លឹះ និងការណ៍នាំប្រើ អេមេបាឡា ឆាត:\n\n"
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
    L"អំពី អេមេបាឡា ឆាត",
    L"សកម្ម",
    L"កំពុងបកប្រែ...",
    L"បានផ្អាក",
    L"អេមេបាឡា ឆាត",
    L"មិនអាចចម្លងអត្ថបទដែលបានជ្រើសរើសទេ។ សូមពិនិត្យកម្មវិធីគោលដៅ ហើយព្យាយាមម្ដងទៀត។",
    L"គ្មានអត្ថបទត្រូវបានជ្រើសសម្រាប់បកប្រែទេ។",
    L"កំណត់ស្វ័យប្រវត្តិ",
    L"អេមេបាឡា ឆាត កំពុងដំណើរការក្នុងផ្ទៃខាងក្រោយរួចជាស្រេច។\nសូមពិនិត្យតំបន់ជូនដំណឹងរបស់ប្រព័ន្ធ។",
    L"ការដំណើរការ COM បានបរាជ័យ។\nផ្លាកអណ្ដែត និងការអានអត្ថបទជាសំឡេងនឹងមិនអាចប្រើបានទេ\nប៉ុន្តែការបកប្រែ គន្លឹះ តំបន់ជូនដំណឹង និងសំឡេង នៅតែដំណើរការ។",
    L"ឈប់ចម្លងបិទភ្ជាប់។ សរសេរដោយធម្មជាតិជាភាសាកំណើតរបស់អ្នក — ការបកប្រែនឹងជំនួសអក្សរដែលអ្នកបោះ ជានិច្ចកាលនៅក្នុងកម្មវិធី Windows ណាក៏បាន។",
    L"⚡ អូសដើម្បីបកប្រែ — ជ្រើសរើសអត្ថបទក្នុងកម្មវិធីណាក៏បាន សញ្ញាអណ្ដែតបកប្រែភ្លាម។",
    L"🔊 TTS បណ្ដាញប្រសាទ — បញ្ចេញសំឡេងគ្រប់ភាសាទាំង 37 តាមរយៈកញ្ចប់សំឡេង Windows ដែលបានដំឡើង។",
    L"🔒 100% នៅលើឧបករណ៍ និងឯកជន — សកម្មតែពេលគន្លឹះកំពុងចុច; ក្ដារតម្កេញមិនត្រូវប៉ះទេ។",
    L"នៅឆ្នាំ 2000 មុនគ្រិស្តសករាជ កាតិកាម៉េសូប៉ូតាមីយាបានហៅ «Eme-bala» ចំពោះមនុស្សដែលធ្វើឱ្យភាសាក្លាយជាស្ពានភ្ជាប់ពិភព។",
    L"គេហទំព័រ",
    L"ទំនាក់ទំនង",
    L"Reddit",
    L"ក្រុម Team Sunplaza · សេអ៊ូល Yeongdeungpo (បន្ទប់ 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · ម៉ោងធ្វើការ 10:00–19:00 KST",
    L"ស្ថាបតិករចម្បង៖ Yongtai Kim",
    L"✓ បានចម្លង!",
    L"📋 ចម្លង",
    L"🔊 សំឡេង",
    L"ភាសាចំណុចប្រទាក់",
    L"ស្វ័យប្រវត្តិ (ភាសាប្រព័ន្ធ)",
    L"កំណត់ឡើងវិញទៅលំនាំដើមប្រព័ន្ធ",
    L"លំនាំដើមត្រូវបានត្រឡប់មកវិញ",
    L"ការវាយអក្សរលើក្ដារចុច",
    L"បន្ទាត់ណែនាំពេលអូស",
    L"អេមេបាឡា ឆាត",
    L"មិនមានសំឡេង Windows ត្រូវបានដំឡើងសម្រាប់ភាសានេះទេ។ ចុច 🔊 ម្តងទៀតដើម្បីបើកការកំណត់ការនិយាយ។",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"ការជូនដំណឹងអំពីឯកជនភាព",
    L"គោលការណ៍ឯកជនភាពរបស់ Emebala Chat មានដូចខាងក្រោម៖\n"
    L"\n"
    L"• Emebala Chat មិនដំណើរការសាឺវឺរបស់ខ្លួនទេ។\n"
    L"• ពេលប្រើម៉ូដែលក្នុងឧបករណ៍ អត្ថបទបកប្រែមិនចេញពីឧបករណ៍របស់អ្នកទេ។\n"
    L"• បើអ្នកជ្រើស Google Translate ឬប្ដូរទៅពពកដោយស្វ័យប្រវត្តិ អត្ថបទដែលជ្រើស ឬបោះពុម្ពត្រូវផ្ញើទៅ Google ដោយផ្ទាល់ដើម្បីបកប្រែ ដោយមិនឆ្លងកាត់ Emebala ទេ។\n"
    L"• កំណត់ហេតុធ្វើរោគវិនិច្ឆ័យត្រូវបានបិទដោយលំនាំដើម ហើយត្រូវបើកក្នុងការកំណត់ (ជ្រើសរើសចូលរួម)។\n"
    L"• ប្រសិនបើអ្នកមិនចង់ផ្ញើអត្ថបទទៅ cloud (Google) សូមបើកម៉ឺនុយរូបតំណាងនៅថតការងារ ហើយជ្រើសរើស “ម៉ាស៊ីនបកប្រែ” រួច “ម៉ាស៊ីនក្នុងស្រាប់”។ ប្រសិនបើគ្មានគំរូក្នុងម៉ាស៊ីនត្រូវបានដំឡើង ហើយការបម្រុងទុក cloud ត្រូវបានបិទ ការបកប្រែនឹងមិនដំណើរការទេ — គ្មានអ្វីត្រូវបានផ្ញើឡើយ។\n"
    L"\n"
    L"ព័ត៌មានលម្អិតស្ថិតក្នុងឯកសារ README ដែលអាចអានឡើងវិញពេលណាក៏បាន។\n",
    L"ឯកសារកំណត់រចនាសម្ព័ន្ធ: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"អត្ថបទវែងពេក៖ បានបកប្រែតែផ្នែកដើម និងបញ្ចប់ប៉ុណ្ណោះ។",
    L"មានអត្ថបទថ្មីមិនទាន់បកប្រែខាងលើ។ ដើម្បីបកប្រែ សូមដាក់កូរ៉េស័រនៅចុងបន្ទាត់នោះ ហើយចុច Enter។",
    L"កំពុងជួសជុលសមាសភាគម៉ាស៊ីនក្នុងម៉ាស៊ីន…",
    L"ការបកប្រែក្នុងម៉ាស៊ីនមិនអាចប្រើបានទេ",
    L"រកមិនឃើញឯកសារម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនទេ ដូច្នេះការបកប្រែត្រូវបានផ្អាកជាបណ្តោះអាសន្ន។ ដើម្បីស្តារម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនឡើងវិញ សូមដំឡើង Emebala Chat ម្តងទៀត ឬដើម្បីប្តូរទៅការបកប្រែក្នុងពពក (Google) សូមជ្រើសរើស “Google បកប្រែ” ពីម៉ឺនុយ tray នៅ “ម៉ាស៊ីនបកប្រែ”។",
    L"ស្រប OpenAI (ម៉ាស៊ីនមេដែលអ្នកប្រើប្រាស់កំណត់)…",
        L"ការកំណត់ម៉ាស៊ីនឆបែល OpenAI",
        L"ការកំណត់ម៉ាស៊ីនឆបែល OpenAI…",
        L"Base URL",
        L"កូនសោ API",
        L"ម៉ូដែល",
        L"ទាញយកបញ្ជីម៉ូដែល",
        L"មិនអាចទាញយកបញ្ជីម៉ូដែលបានទេ។ អ្នកអាចវាយឈ្មោះម៉ូដែលដោយផ្ទាល់។",
        L"ការតភ្ជាប់មិនសុវត្ថិភាព (HTTP)",
        L"Base URL ប្រើ HTTP (មិនបានអ៊ិនគ្រីប)។ កូនសោ API និងអត្ថបទរបស់អ្នកនឹងត្រូវផ្ញើជាអក្សរធម្មតា។ បន្ត?",
        L"បានរក្សាទុកការកំណត់ឆបែល OpenAI។",
        L"កូនសោដែលបានរក្សាទុក៖ ",
        L"Base URL មិនត្រឹមត្រូវ។ ឧទាហរណ៍៖ https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"ម៉ូដែលដែលអ្នកប្រើប្រាស់កំណត់ (.gguf)",
    L"ចុះឈ្មោះម៉ូដែល .gguf ផ្សេងទៀត…",
        L"សេចក្ដីជូនដំណឹងអំពីគុណភាពបកប្រែ",
        L"ម៉ូដែលដែលបានជ្រើសរើសមិនមែនជា Hy-MT2 ទេ។ កំណែបច្ចុប្បន្នប្រើប្រាស់ prompt សម្រាប់តែ Hy-MT2 ដូច្នេះគុណភាពបកប្រែជាមួយម៉ូដែលនេះមិនត្រូវបានធានាទេ។ បន្ត?",
        L"បានចុះឈ្មោះម៉ូដែល",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"បានចុះឈ្មោះម៉ូដែលដែលបានជ្រើសរើសជាមួយម៉ាស៊ីនក្នុងសៀ។\n"
    L"\n"
    L"ម៉ូដែលនេះត្រូវបានប្រើសម្រាប់បកប្រែនៅពេលអ្នកជ្រើសរើស \"ការជ្រើសរើសម៉ាស៊ីនបកប្រែ > ជម្រើសរបស់អ្នកប្រើ (.gguf)\"។\n"
    L"\n"
    L"ពេលវេលាមישប្រើៈ យ៉ាងយូរប្រហែល 1 នាទីក្រោយពេលចុះឈ្មោះ (បន្ទាប់ពីម៉ាស៊ីនបញ្ចប់ដោយសារអសកម្ម)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"ម៉ូដែលនេះមានស្រាប់ក្នុង Emebala Chat រួចហើយ។ មិនចាំបាច់ចុះឈ្មោះទេ។ អ្នកអាចជ្រើសរើសម៉ាស៊ីនបកប្រែក្នុងស្រុកដែលមានស្រាប់ដោយផ្ទាល់។",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(មិនទាន់ចុះឈ្មោះ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"គ្រប់គ្រងម៉ូដែល…",
    L"កម្មវិធីគ្រប់គ្រងម៉ូដែលអ្នកប្រើ",
    L"មិនមានម៉ូដែលអ្នកប្រើដែលបានចុះឈ្មោះទេ។",
    L"ប្ដូរឈ្មោះ…",
    L"លុប…",
    L"បិទ",
    L"លុបការចុះឈ្មោះម៉ូដែល",
    L"ការចុះឈ្មោះនៃម៉ូដែលដែលបានជ្រើសរើស នឹងត្រូវលុប។\n"
    L"\n"
    L"ឯកសារម៉ូដែល (.gguf) នៅតែស្ថិតនៅលើថាស ហើយមិនត្រូវបានលុបទេ។ ប្រសិនបើម៉ូដែលនេះកំពុងប្រើប្រាស់ ការជ្រើសរើសម៉ាស៊ីនបកប្រែ នឹងត្រលប់ទៅស្វ័យប្រវត្តិ (Auto)។\n"
    L"\n"
    L"បន្ត?",
    L"ប្ដូរឈ្មោះម៉ូដែល",
    L"បញ្ចូលឈ្មោះថ្មី (អតិបរមា ៦៤ តួអក្សរ គ្មានដកឃ្លា)។",
    L"មិនអាចប្រើឈ្មោះនោះបានទេ។ សូមបញ្ចូលឈ្មោះមិនទទេ ខុសពីឈ្មោះដែលមានស្រាប់ គ្មានដកឃ្លា ឬសញ្ញាបំបែកផ្លូវ ក្នុងចំណោម ៦៤ តួអក្សរ។",
    L"បានរក្សាទុកការផ្លាស់ប្ដូរ។",

    L"មិនអាច serialize registry.json បានទេ (ឈ្មោះឯកសារត្រូវបានបដិសេធ)។ មិនមានអ្វីត្រូវបានផ្លាស់ប្ដូរទេ។",
    L"%LOCALAPPDATA% មិនអាចប្រើប្រាស់បានទេ; រកថតម៉ូដែលរួមមិនឃើញ។",
    L"មិនអាចសរសេរ registry.json បានទេ។",
    L"registry.json មិនត្រូវបានសរសេរពេញលេញទេ។",
    L"registry.json ខូចឬមានស្គីមាមិនត្រូវបានគាំទ្រ។ វាមិនត្រូវបានកែប្រាងទេ។ ជួសជុលឬលុបវាចេញ រួចព្យាយាមម្ដងទៀត។",
    L"ការបកប្រែបរាជ័យ។ សូមព្យាយាមម្ដងទៀត។",
    L"ម៉ាស៊ីនបកប្រែក្នុងម៉ាស៊ីនមិនអាចប្រើប្រាស់បណ្តោះអាសន្ន (អាចកំពុងចាប់ផ្តើម)។ សូមរង់ចាំបន្តិចហើយព្យាយាមម្ដងទៀត។",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"យល់ព្រម",
    L"បោះបង់",
};

// 36. Lao (lo)
const LocalizedStrings kStringsLao = {
    L"ສະຖານະ: ເປີດໃຊ້ (F9: ຢຸດຊົ່ວຄາວ)",
    L"ສະຖານະ: ຢຸດຊົ່ວຄາວ (F9: ສືບຕໍ່)",
    L"ເຄື່ອງຈັກແປ",
    L"Google ແປ (ຟຣີ / ບໍ່ຕ້ອງຕິດຕັ້ງ)",
    L"ເຄື່ອງຈັກພາຍໃນທ້ອງຖິ່ນ (Hy-MT2-1.8B ອັອບໄລນ໌)",
    L"ພາສາຕົ້ນສະບັບ (ຂໍ້ມູນເຂົ້າ)",
    L"ພາສາເປົ້າໝາຍ (ຂໍ້ມູນອອກ)",
    L"ສັບປ່ຽນຕົ້ນ ⇄ ເປົ້າໝາຍ (ກລິກສອງຄັ້ງ)",
    L"ສົ່ງອັດຕະໂນມັດເມື່ອກົດ Enter",
    L"ສຽງຕອບກັບ (ໂທນ)",
    L"ສະແດງແຖບປ້າຍລອຍ",
    L"ເປີດຄູ່ກັບ Windows",
    L"ຄີດທາງລັດ ແລະ ຄູ່ມືຊ່ວຍເຫຼືອ...",
    L"ອອກຈາກ ເອເມບາລາ ແຊັດ",
    L"ກ່ຽວກັບ ເອເມບາລາ ແຊັດ…",
    L"ຄີດທາງລັດ ແລະ ຄູ່ມືການໃຊ້ ເອເມບາລາ ແຊັດ",
    L"ຄີດທາງລັດ ແລະ ຄູ່ມືການໃຊ້ ເອເມບາລາ ແຊັດ:\n\n"
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
    L"ກ່ຽວກັບ ເອເມບາລາ ແຊັດ",
    L"ເປີດໃຊ້",
    L"ກຳລັງແປ...",
    L"ຢຸດຊົ່ວຄາວ",
    L"ເອເມບາລາ ແຊັດ",
    L"ບໍ່ສາມາດລອກເອົາຂໍ້ຄວາມທີ່ເລືອກໄດ້. ກະລຸນາກວດສອບແອັບເປົ້າໝາຍ ແລ້ວລອງໃໝ່.",
    L"ຍັງບໍ່ໄດ້ເລືອກຂໍ້ຄວາມສຳລັບແປ.",
    L"ກວດຈັບອັດຕະໂນມັດ",
    L"ເອເມບາລາ ແຊັດ ກຳລັງປະຕິບັດຢູ່ພື້ນຫຼັງແລ້ວ.\nກະລຸນາກວດເບິ່ງຖາດແຈ້ງເຕືອນຂອງລະບົບ.",
    L"ການເລີ່ມຕົ້ນ COM ລົ້ມເຫຼວ.\nແຖບປ້າຍລອຍ ແລະ ການອ່ານຂໍ້ຄວາມຈະໃຊ້ບໍ່ໄດ້,\nແຕ່ການແປ, ຄີດທາງລັດ, ຖາດ ແລະ ສຽງຍັງເຮັດວຽກຕາມປົກກະຕິ.",
    L"ພໍໄດ້ກັບການລອກແຜ່. ພິມຢ່າງທຳມະຊາດເປັນພາສາແມ່ຂອງທ່ານ — ຄຳແປຈະແທນການພິມຂອງທ່ານທັນທີໃນທຸກແອັບ Windows.",
    L"⚡ ລາກເພື່ອແປ — ເລືອກຂໍ້ຄວາມໃນແອັບໃດກໍໄດ້ ຮູບສັນຍາລັກລອຍຈະແປທັນທີ.",
    L"🔊 TTS ແບບປະສາດ — ອອກສຽງຄົບທັງ 37 ພາສາຜ່ານຊຸດສຽງ Windows ທີ່ຕິດຕັ້ງ.",
    L"🔒 100% ໃນອຸປະກອນ ແລະ ເປັນສ່ວນຕົວ — ເຮັດວຽກຕອນກົດຄີດທາງລັດເທົ່ານັ້ນ; ບໍ່ແຕະກະດານຂັບ.",
    L"ໃນປີ 2000 ກ່ອນຄຣິດສັກກະລາດ, ຜູ້ຂຽນແຫ່ງເມໂຊໂປເຕເມຍເອີ້ນຜູ້ທີ່ປ່ຽນພາສາເປັນຂົວເຊື່ອມໂລກວ່າ «Eme-bala».",
    L"ເວັບໄຊ",
    L"ຕິດຕໍ່",
    L"Reddit",
    L"ທີມ Team Sunplaza · ຊຽວລ Yeongdeungpo (ຫ້ອງ 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · ເວລາເຮັດວຽກ 10:00–19:00 KST",
    L"ສະຖາປະນິກຫຼັກ: Yongtai Kim",
    L"✓ ລອກເອົາແລ້ວ!",
    L"📋 ລອກເອົາ",
    L"🔊 ອ່ານດັງ",
    L"ພາສາຜູ້ໃຊ້ງານ",
    L"ອັດຕະໂນມັດ (ພາສາລະບົບ)",
    L"ຣີເຊັດຄືນຄ່າເລີ່ມຕົ້ນຂອງລະບົບ",
    L"ກູ້ຄືນຄ່າເລີ່ມຕົ້ນແລ້ວ",
    L"ການພິມດ້ວຍແປ້ນພິມ",
    L"ຄຳແນະນຳເມື່ອລາກ",
    L"ເອເມບາລາ ແຊັດ",
    L"ບໍ່ມີສຽງ Windows ຕິດຕັ້ງສຳລັບພາສານີ້. ຄລິກ 🔊 ອີກເທື່ອໜຶ່ງເພື່ອເປີດການຕັ້ງຄ່າການເວົ້າ.",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"ການແຈ້ງເຕືອນດ້ານຄວາມເປັນສ່ວນຕົວ",
    L"ຫຼັກການດ້ານຄວາມເປັນສ່ວນຕົວຂອງ Emebala Chat ມີດັ່ງນີ້:\n"
    L"\n"
    L"• Emebala Chat ບໍ່ມີເຊີເວີຂອງຕົນເອງ.\n"
    L"• ເມື່ອໃຊ້ແບບຈຳລອງພາຍໃນເຄື່ອງ, ຂໍ້ຄວາມແປຈະບໍ່ອອກຈາກອຸປະກອນຂອງທ່ານ.\n"
    L"• ຖ້າທ່ານເລືອກ Google Translate ຫຼືສະຫຼັບໄປຄລາວອັດຕະໂນມັດ, ຂໍ້ຄວາມທີ່ເລືອກ ຫຼືພິມ ຈະຖືກສົ່ງໄປ Google ໂດຍກົງ ເພື່ອແປ, ບໍ່ຜ່ານ Emebala.\n"
    L"• ໄຟລ໌ບັນທຶກການວິນິດໄສຖືກປິດໄວ້ເປັນຄ່າເລີ່ມຕົ້ນ; ເປີດໄດ້ໃນການຕັ້ງຄ່າ (ເລືອກເຂົ້າຮ່ວມ).\n"
    L"• ຖ້າທ່ານບໍ່ຕ້ອງສົ່ງຂໍ້ຄວາມໄປຍັງຄລາວ (Google), ໃຫ້ເປີດເມນູໄອຄອນຢູ່ແຖບຮາບພຽງລະບົບ ແລ້ວເລືອກ “ເຄື່ອງຈັກແປ” ຈາກນັ້ນ “ເຄື່ອງຈັກພາຍໃນທ້ອງຖິ່ນ”. ຖ້າບໍ່ມີແບບຈຳລອງໃນເຄື່ອງ ແລະປິດການສະຫຼັບໄປຄລາວໄວ້, ການແປຈະບໍ່ເຮັດວຽກ ໂດຍບໍ່ມີການສົ່ງຂໍ້ມູນ.\n"
    L"\n"
    L"ລາຍລະອຽດເຕັມຢູ່ໃນໄຟລ໌ README ທີ່ສາມາດອ່ານຄືນໄດ້ທຸກເວລາ.\n",
    L"ໄຟລ໌ config: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"ຂໍ້ຄວາມຍາວເກີນໄປ: ແປສະເພາະສ່ວນຕົ້ນແລະສ່ວນທ້າຍເທົ່ານັ້ນ.",
    L"ມີຂໍ້ຄວາມໃໝ່ທີ່ຍັງບໍ່ໄດ້ແປຢູ່ຂ້າງເທິງ. ເພື່ອແປ, ກະລຸນາວາງເຄີເຊີທີ່ທ້າຍບັນທັດນັ້ນ ແລ້ວກົດ Enter.",
    L"ກຳລັງສ້ອມແປງອົງປະກອບເຄື່ອງຈັກທ້ອງຖິ່ນ…",
    L"ການແປພາສາທ້ອງຖິ່ນບໍ່ສາມາດໃຊ້ໄດ້",
    L"ບໍ່ພົບໄຟລ໌ເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນ, ຈຶ່ງຢຸດການແປພາສາຊົ່ວຄາວ. ເພື່ອຟື້ນຟູເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນ, ກະລຸນາຕິດຕັ້ງ Emebala Chat ອີກຄັ້ງ, ຫຼື ເພື່ອປ່ຽນໄປໃຊ້ການແປພາສາຄລາວ (Google), ກະລຸນາເລືອກ “Google ແປພາສາ” ຈາກເມນູ tray, “ເຄື່ອງຈັກແປພາສາ”.",
    L"ຖັດກັນກັບ OpenAI (ເຄື່ອງແມ່ຂ່າຍທີ່ຜູ້ໃຊ້ກຳນົດ)…",
        L"ການຕັ້ງຄ່າເຄື່ອງຈັກທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI",
        L"ການຕັ້ງຄ່າເຄື່ອງຈັກທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI…",
        L"Base URL",
        L"ລະຫັດ API",
        L"ໂມເດລ",
        L"ດຶງລາຍການໂມເດລ",
        L"ບໍ່ສາມາດດຶງລາຍການໂມເດລໄດ້. ທ່ານສາມາດພິມຊື່ໂມເດລໂດຍກົງໄດ້.",
        L"ການເຊື່ອມຕໍ່ທີ່ບໍ່ປອດໄພ (HTTP)",
        L"Base URL ໃຊ້ HTTP (ບໍ່ໄດ້ເຂົ້າລະຫັດ). ລະຫັດ API ແລະ ຂໍ້ຄວາມຂອງທ່ານຈະຖືກສົ່ງແບບບໍ່ເຂົ້າລະຫັດ. ສືບຕໍ່?",
        L"ບັນທຶກການຕັ້ງຄ່າທີ່ເຂົ້າກັນໄດ້ກັບ OpenAI ແລ້ວ.",
        L"ລະຫັດທີ່ບັນທຶກ: ",
        L"Base URL ບໍ່ຖືກຕ້ອງ. ຕົວຢ່າງ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"ໂມເດວທີ່ຜູ້ໃຊ້ກຳນົດ (.gguf)",
    L"ລົງທະບຽນໂມເດວ .gguf ອື່ນ…",
        L"ແຈ້ງການກ່ຽວກັບຄຸນນະພາບການແປ",
        L"ໂມເດລທີ່ເລືອກບໍ່ແມ່ນ Hy-MT2. ເວີຊັນປັດຈຸບັນໃຊ້ prompt ສຳລັບສະເພາະ Hy-MT2 ເທົ່ານັ້ນ, ດັ່ງນັ້ນຄຸນນະພາບການແປດ້ວຍໂມເດລນີ້ຈຶ່ງບໍ່ໄດ້ຮັບການຮັບປະກັນ. ສືບຕໍ່?",
        L"ລົງທະບຽນໂມເດລແລ້ວ",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"ໂມເດລທີ່ເລືອກໄດ້ຖືກລົງທະບຽນກັບເຄື່ອງຈັກທ້ອງຖິ່ນແລ້ວ.\n"
    L"\n"
    L"ໂມເດລນີ້ຖືກໃຊ້ສຳລັບການແປເມື່ອທ່ານເລືອກ \"ການເລືອກເຄື່ອງຈັກແປ > ທາງເລືອກຂອງຜູ້ໃຊ້ (.gguf)\".\n"
    L"\n"
    L"ເມື່ອໃຊ້ໄດ້: ພາຍໃນ 1 ນາທີຫຼັງຈາກລົງທະບຽນ (ຫຼັງຈາກເຄື່ອງຈັກຢຸດເພາະບໍ່ໄດ້ໃຊ້ງານ)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"ໂມເດວນີ້ແມ່ນມີຢູ່ໃນ Emebala Chat ແລ້ວ. ບໍ່ຈຳເປັນຕ້ອງລົງທະບຽນ. ທ່ານສາມາດເລືອກເຄື່ອງຈັກແປພາສາທ້ອງຖິ່ນທີ່ມີຢູ່ໂດຍກົງໄດ້.",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(ຍັງບໍ່ໄດ້ລົງທະບຽນ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"ຈັດການໂມເດລ…",
    L"ຜູ້ຈັດການໂມເດລຜູ້ໃຊ້",
    L"ບໍ່ມີໂມເດລຜູ້ໃຊ້ທີ່ລົງທະບຽນ.",
    L"ປ່ຽນຊື່…",
    L"ລົບ…",
    L"ປິດ",
    L"ລົບການລົງທະບຽນໂມເດລ",
    L"ການລົງທະບຽນຂອງໂມເດລທີ່ເລືອກ ຈະຖືກລົບ.\n"
    L"\n"
    L"ໄຟລ໌ໂມເດລ (.gguf) ຍັງຄົງຢູ່ໃນດິສກ໌ ແລະ ຈະບໍ່ຖືກລົບ. ຖ້າໂມເດລນີ້ຖືກນໍາໃຊ້ຢູ່, ການເລືອກເຄື່ອງຈັກຮຽກຈະກັບໄປທີ່ອັດຕະໂນມັດ (Auto).\n"
    L"\n"
    L"ສືບຕໍ່?",
    L"ປ່ຽນຊື່ໂມເດລ",
    L"ໃສ່ຊື່ໃໝ່ (ສູງສຸດ 64 ຕົວອັກສອນ, ບໍ່ມີຊ່ອງຫວ່າງ).",
    L"ບໍ່ສາມາດໃຊ້ຊື່ນັ້ນໄດ້. ກະລຸນາໃສ່ຊື່ທີ່ບໍ່ວ່າງ, ຕ່າງຈາກທີ່ມີຢູ່, ບໍ່ມີຊ່ອງຫວ່າງ ຫຼື ຕົວແຍກເສັ້ນທາງ, ບໍ່ເກີນ 64 ຕົວອັກສອນ.",
    L"ບັນທຶກການປ່ຽນແປງແລ້ວ.",

    L"ບໍ່ສາມາດ serialize registry.json ໄດ້ (ຊື່ໄຟລ໌ຖືກປະຕິເສດ). ບໍ່ມີຫຍັງປ່ຽນແປງ.",
    L"%LOCALAPPDATA% ບໍ່ສາມາດໃຊ້ໄດ້; ບໍ່ພົບໂຟນເດີໂມເດລທີ່ແບ່ງປັນ.",
    L"ບໍ່ສາມາດຂຽນ registry.json ໄດ້.",
    L"registry.json ບໍ່ໄດ້ຖືກຂຽນໃຫ້ສົມບູນ.",
    L"registry.json ເສຍຫາຍ ຫຼື ມີ schema ທີ່ບໍ່ຮອງຮັບ. ມັນບໍ່ໄດ້ຖືກດັດແປງ. ສ້ອມແປງ ຫຼື ລຶບມັນອອກ ແລ້ວລອງໃໝ່.",
    L"ການແປລົ້ມເຫຼວ. ກະລຸນາລອງໃໝ່.",
    L"ເຄື່ອງຈັກແປທ້ອງຖິ່ນບໍ່ສາມາດໃຊ້ໄດ້ຊົ່ວຄາວ (ອາດກຳລັງເລີ່ມຕົ້ນ). ກະລຸນາลໍຖ້າຊົວຄູ່ແລ້ວລອງໃໝ່.",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"ຕົກລົງ",
    L"ຍົກເລີກ",
};

// 37. Burmese (my)
const LocalizedStrings kStringsBurmese = {
    L"အခြေအနေ: လုပ်ဆောင်နေ (F9: ခေတ္တရပ်)",
    L"အခြေအနေ: ခေတ္တရပ်ထား (F9: ပြန်စ)",
    L"ဘာသာပြန် အင်ဂျင်",
    L"Google Translate (အခမဲ့ / မတပ်ဆင်ရ)",
    L"အတွင်းသွင်းပြင်ပအင်ဂျင် (Hy-MT2-1.8B အင်တာနက်မဲ့)",
    L"မူလဘာသာစကား (ထည့်သွင်းမှု)",
    L"ပစ်မှတ်ဘာသာစကား (ထုတ်လွှင့်မှု)",
    L"မူလ ⇄ ပစ်မှတ် လှလှယ် (နှစ်ချက်နှိပ်)",
    L"Enter နှိပ်လျှင် အလိုအလျောက်ပို့ရန်",
    L"အသံ တုံ့ပြန်ချက် (တုန်များ)",
    L"ပေါ်လောတံဆိပ် ပြရန်",
    L"Windows နှင့်အတူ စတင်မည်",
    L"ရှော့တ်ကတ် လမ်းညွှန်နှင့် အကူအညီ...",
    L"အီမီဘာလာ ချက် မှ ထွက်မည်",
    L"အီမီဘာလာ ချက် အကြောင်း…",
    L"အီမီဘာလာ ချက် ရှော့တ်ကတ်နှင့် အသုံးပြုနည်း လမ်းညွှန်",
    L"အီမီဘာလာ ချက် ရှော့တ်ကတ်နှင့် အသုံးပြုနည်း လမ်းညွှန်:\n\n"
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
    L"အီမီဘာလာ ချက် အကြောင်း",
    L"လုပ်ဆောင်နေ",
    L"ပြန်ဆိုနေ...",
    L"ခေတ္တရပ်",
    L"အီမီဘာလာ ချက်",
    L"ရွေးချယ်ထားသော စာသားကို ကူးယူ၍မရပါ။ ပစ်မှတ်အက်ပ်ကို စစ်ဆေးပြီး ပြန်ကြိုးစားပါ။",
    L"ပြန်ဆိုရန် စာသား မရွေးချယ်ရသေးပါ။",
    L"အလိုအလျောက် ဖော်ထုတ်",
    L"အီမီဘာလာ ချက် သည် နောက်ခံတွင် စတင်လည်ပတ်နေပြီ ဖြစ်သည်။\nစနစ်အသိပေးချက် တရေးကို ကြည့်ပါ။",
    L"COM စတင်ခြင်း မအောင်မြင်ပါ။\nပေါ်လောတံဆိပ်နှင့် စာသားအသံဖတ်ခြင်း မရတော့သော်လည်း၊\nဘာသာပြန်ခြင်း၊ ရှော့တ်ကတ်များ၊ တရေးနှင့် အသံများ ဆက်လက်လည်ပတ်နေမည်။",
    L"ကူးတိပ် ပွဲတိပ်များကို မလုပ်တော့ပါနဲ့။ သင့်မိခင်ဘာသာဖြင့် သဘာဝကျ ရိုက်နှိပ်ပါ — Windows အက်ပ်များအတွင်း သင့်ရိုက်နှိပ်မှုများကို ဘာသာပြန်က ချက်ချင်း အစားထိုးပေးမည်။",
    L"⚡ ဆွဲပြန်ဆို — မည်သည့်အက်ပ်တွင်မဆို စာသားရွေးချယ်လိုက်လျှင် ပေါ်လောသင်္ကေတက ချက်ချင်း ပြန်ဆိုပေးသည်။",
    L"🔊 Neural TTS — တပ်ဆင်ထားသော Windows အသံပက်ကေ့ဂျ်များဖြင့် ဘာသာစကား 37 မျိုးလုံးကို အသံထွက်ဖတ်သည်။",
    L"🔒 100% စက်ပေါ်တွင်သာ၊ လုံခြုံစွာ — ရှော့တ်ကတ် ဖိထားချိန်၌သာ လုပ်ဆောင်; ကလစ်ပွိုဒ်ကို မထိတွေ့ပါ။",
    L"ခရစ်တော် မပေါ်မီ နှစ်ပေါင်း ၂၀၀၀ က မက်ဆိုပိုတေးမီးယား စာရေးဆရာများက ဘာသာစကားကို ကမ္ဘာ့တံတားအဖြစ် ပြောင်းလဲသူများကို «Eme-bala» ဟု ခေါ်ကြသည်။",
    L"ဝက်ဘ်ဆိုက်",
    L"ဆက်သွယ်ရန်",
    L"Reddit",
    L"Team Sunplaza · ဆီအူးလ် Yeongdeungpo (အခန်း 219, 65 Yeongjung-ro)",
    L"+82 2 575 0414 · ရုံးချိန် 10:00–19:00 KST",
    L"ဦးဆောင်ဗိသုကာ: Yongtai Kim",
    L"✓ ကူးယူပြီး!",
    L"📋 ကူးယူရန်",
    L"🔊 အသံဖတ်",
    L"အင်တာဖေ့စ် ဘာသာစကား",
    L"အလိုအလျောက် (စနစ်ဘာသာစကား)",
    L"စနစ်ပုံသေများသို့ ပြန်လည်သတ်မှတ်ရန်",
    L"ပုံသေများ ပြန်လည်ရရှိပါသည်",
    L"ကီးဘုတ် ရိုက်နှိပ်ခြင်း",
    L"ဆွဲယူမှု ToolTip",
    L"အီမီဘာလာ ချက်",
    L"ဤဘာသာစကားအတွက် Windows အသံ တပ်ဆင်ထားခြင်း မရှိပါ။ စပီခ် ဆက်တင်များ ဖွင့်ရန် 🔊 ကို ထပ်မံနှိပ်ပါ။",
    // REQ-206/208 (session 260911_0002 T2): privacy notice popup +
    // config-path line (trailing-initializer append, design §2.4).
    L"ကိုယ်ရေးအချက်အလက် လုံခြုံရေးကြေညာချက်",
    L"Emebala Chat ၏ ကိုယ်ရေးလုံခြုံမှု မူဝါဒများမှာ-\n"
    L"\n"
    L"• Emebala Chat သည် ကိုယ်ပိုင်ဆာဗာ မလည်ပတ်ပါ။\n"
    L"• ဒေသတွင်းမော်ဒယ်သုံးပါက ဘာသာပြန်စာသားသည် သင့်စက်မှ မထွက်ပါ။\n"
    L"• Google Translate ကို ရွေးလိုက်ပါက သို့မဟုတ် တစ်ဆက်တည်း cloud သို့ ပြောင်းလိုက်ပါက ရွေးထား/ရိုက်ထားသော စာသားသည် ဘာသာပြန်ရန် Google သို့ တိုက်ရိုက်ပို့ပြီး Emebala မဖြတ်ပါ။\n"
    L"• ရောဂါရှာဖွေ log များသည် မူလပိတ်ထားသည်။ ဆက်တင်တွင် ပွင့်အပ် (opt-in)။\n"
    L"• အကယ်၍ သင်သည် စာသားကို cloud (Google) သို့ မပို့လိုပါက စနစ်တရေး သင်္ကေတမီနူးကို ဖွင့်၍ “ဘာသာပြန် အင်ဂျင်” တွင် “အတွင်းသွင်းပြင်ပအင်ဂျင်” ကို ရွေးချယ်ပါ။ ဒေသတွင်းမော်ဒယ် မတပ်ဆင်ထားပါကနှင့် cloud ပြောင်းလဲခြင်း ပိတ်ထားပါက ဘာသာပြန်သည် စာသားမပို့ဘဲ အလုပ်မလုပ်ပါ။\n"
    L"\n"
    L"အသေးစိတ်ကို README ဖိုင်တွင် ဖတ်နိုင်ပြီး အချိန်မရွေး ပြန်ဖတ်နိုင်သည်။\n",
    L"config ဖိုင်: %LOCALAPPDATA%\\Emebalachat\\config.json",
    // SEC-M1 (session 260911_0002): cloud translation truncation notice
    // (trailing-initializer append; head/tail window applied above the
    // kMaxCloudQueryUnits cap - verify report 235100 §5.)
    L"စာသား အလွန်ရှည်သဖြင့် အစနှင့် အဆုံးကိုသာ ဘာသာပြန်ဆိုပါသည်။",
    L"အပေါ်တွင် ဘာသာပြန်မထားသော စာသားအသစ်ရှိသည်။ ဘာသာပြန်ရန် ကာဆာကို ထိုစာကြောင်းအဆုံးသို့ ထားပြီး Enter နှိပ်ပါ။",
    L"ပရိုဂရမ်အင်ဂျင်အစိတ်အပိုင်းများကို ပြုပြင်နေသည်…",
    L"ဒေသန္တရ ဘာသာပြန်ချက် မရနိုင်ပါ",
    L"ဒေသန္တရ ဘာသာပြန်အင်ဂျင် ဖိုင်များ မတွေ့ပါသဖြင့် ဘာသာပြန်မှုကို ယာယီရပ်နားထားပါသည်။ ဒေသန္တရ အင်ဂျင်ကို ပြန်လည်ရရှိရန် Emebala Chat ကို ပြန်လည်ထည့်သွင်းပါ၊ သို့မဟုတ် ကလောင်(Google) ဘာသာပြန်သို့ ပြောင်းလဲရန်၊ tray မီနူး၏ “ဘာသာပြန်အင်ဂျင်” မှ “Google ဘာသာပြန်” ကို ရွေးပါ။",
    L"OpenAI နှင့်သဟဇာတ (အသုံးပြုသူသတ်မှတ်ချက်စက်များ)…",
        L"OpenAI နှင့် ကိုက်ညီသော အင်ဂျင် ဆက်တင်များ",
        L"OpenAI နှင့် ကိုက်ညီသော အင်ဂျင် ဆက်တင်များ…",
        L"ပင်မ URL",
        L"API ကီး",
        L"မော်ဒယ်",
        L"မော်ဒယ် စာရင်း ရယူပါ",
        L"မော်ဒယ် စာရင်းကို ရယူ၍ မရပါ။ မော်ဒယ်အမည်ကို တိုက်ရိုက် ရိုက်ထည့်နိုင်ပါသည်။",
        L"လုံခြုံမှုမရှိသော ချိတ်ဆက်မှု (HTTP)",
        L"ပင်မ URL သည် HTTP (စာဝှက်မထားပါ) ကို အသုံးပြုပါသည်။ သင်၏ API ကီးနှင့် စာသားကို စာသားအဖြစ် ပေးပို့ပါမည်။ ဆက်လက်ပါသလား?",
        L"OpenAI နှင့် ကိုက်ညီသော ဆက်တင်များကို သိမ်းဆည်းပြီး။",
        L"သိမ်းဆည်းထားသော ကီး: ",
        L"ပင်မ URL မမှန်ကန်ပါ။ ဥပမာ: https://api.openai.com",
    // REQ-045 P4-5 (item 3a-2): third-party .gguf user-model registration.
    L"အသုံးပြုသူသတ်မှတ်ထားသော မော်ဒယ် (.gguf)",
    L"အခြား .gguf မော်ဒယ်ကို မှတ်ပုံတင်ပါ…",
        L"ဘာသာပြန် အရည်အသွေး သတိပေးချက်",
        L"ရွေးချယ်ထားသော မော်ဒယ်သည် Hy-MT2 မဟုတ်ပါ။ လက်ရှိ ဗားရှင်းသည် Hy-MT2 အတွက်သာ prompt ကို အသုံးပြုပါသည်၊ ထို့ကြောင့် ဒီမော်ဒယ်ဖြင့် ဘာသာပြန် အရည်အသွေးကို အာမခံ၍ မရပါ။ ဆက်လက်ပါသလား?",
        L"မော်ဒယ်စာရင်း သွင်းပြီး",
    // REQ-046 P4-2 (Rev2 section B-5, C2): same meaning as the Korean table -
    // no Local-LLM pick instruction; apply bound stated (about 1 minute max).
        L"ရွေးချယ်ထားသော မော်ဒယ်ကို ဒေသတွင်း အင်ဂျင်သို့ စာရင်းသွင်းပြီးပါပြီ။\n"
    L"\n"
    L"သင် \"ဘာသာပြန် အင်ဂျင် ရွေးချယ်မှု > အသုံးပြုသူ ရွေးချယ်မှု (.gguf)\" ကို ရွေးချယ်သောအခါ ဒီမော်ဒယ်ကို ဘာသာပြန်ရန် အသုံးပြုပါတယ်။\n"
    L"\n"
    L"ဘယ်အချိန် သက်ရောက်သလဲ: စာရင်းသွင်းပြီးနောက် အများဆုံး ခန့် 1 မိနစ် (အင်ဂျင် အလုပ်မလုပ်ဘဲ အဆုံးသတ်ပြီးနောက်)",
    // REQ-047 D2 (design section B.3): built-in model notice, appended tail
    // positional (same trailing-initializer discipline as SEC-M1).
    L"ဒီမော်ဒယ်က Emebala Chat ထဲမှာ အသင့်ပါပြီးသားဖြစ်ပါတယ်။ မှတ်ပုံတင်စရာမလိုပါဘူး။ ပါရှိပြီးသား ပြည်တွင်းဘာသာပြန်အင်ဂျင်ကို တိုက်ရိုက်ရွေးချယ်နိုင်ပါတယ်။",
// REQ-047 U1 (designer 164500 §5.3): "(미등록)" empty-slot marker.
    L"(မှတ်ပုံတင်မထားပါ)",
    // REQ-048 R2-D: registered user-.gguf model manager (English placeholder
    // pending per-locale translation; same trailing-initializer discipline).
    L"မော်ဒယ်များကို စီမံခန့်ခွဲပါ…",
    L"အသုံးပြုသူ မော်ဒယ် စီမံခန့်ခွဲခန်း",
    L"စာရင်းသွင်းထားသော အသုံးပြုသူ မော်ဒယ် မရှိပါ။",
    L"နာမည်ပြောင်းပါ…",
    L"ဖျက်ပါ…",
    L"ပိတ်ပါ",
    L"မော်ဒယ် စာရင်းသွင်းမှုကို ဖျက်ပါ",
    L"ရွေးချယ်ထားသော မော်ဒယ်၏ စာရင်းသွင်းမှုကို ဖျက်လိုက်ပါမည်။\n"
    L"\n"
    L"မော်ဒယ် ဖိုင် (.gguf) ကို ဒစ်စ်ပေါ်တွင် ထားရှိပြီး မဖျက်ပါ။ ဒီ မော်ဒယ်ကို အသုံးပြုနေပါက ဘာသာပြန် အင်ဂျင်ရွေးချယ်မှုကို အလိုအလျောက် (Auto) သို့ ပြန်သွားပါမည်။\n"
    L"\n"
    L"ဆက်လက်ပါသလား?",
    L"မော်ဒယ် နာမည်ပြောင်းပါ",
    L"နာမည် အသစ်ထည့်ပါ (အများဆုံး 64 အက္ခရာ၊ ဘေးလ်မပါ)။",
    L"အဲဒီနာမည် အသုံးမပြုနိုင်ပါ။ ဗလာမဟုတ်ပြီး ၆၄ အက္ခရာ အတွင်းနာမည် တစ်ခု ထည့်ပါ။",
    L"ပြောင်းလဲမှုများ သိမ်းဆည်းပြီး။",

    L"registry.json ကို serialize မလုပ်နိုင်ပါ (ဖိုင်နစ်မည်တစ်ခုကို ပယ်ချခံရ). ပြောင်းလဲမှုမရှိပါ။",
    L"%LOCALAPPDATA% မရနိုင်ပါ။ မော်ဒယ် ဖိုင် မတွေ့ပါ။",
    L"registry.json ကို မရေးနိုင်ပါ။",
    L"registry.json ကို အပြည့်အဝ မရေးနိုင်ပါ။",
    L"registry.json ပျက်စီးနေပါသည် သို့မဟုတ် မပံ့ပိုးသော schema ရှိပါသည်။ ၎င်းကို မပြောင်းလဲပါ။ ပြုပြင်ပါ သို့မဟုတ် ဖျက်ပြီး ထပ်စဉ်းစားပါ။",
    L"ဘာသာပြန် မအောင်မြင်ပါ။ ထပ်စဉ်းစားပါ။",
    L"ဒေသတွင်း ဘာသာပြန် အင်ဂျင်ကို ယာယီ အသုံးမပြုနိုင်ပါ (စတင်နေပါရန် ဖြစ်နိုင်ပါသည်)။ ခဏစောင့်ပြီး ထပ်စဉ်းစားပါ။",
    // REQ-050: dialog OK/Cancel push-button labels.
    L"အိုကေ",
    L"ပယ်ဖျက်ပါ",
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

        case StringId::EnumCount:
        default: return L""; // empty by design - the completeness test skips it
    }
    // unreachable
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
