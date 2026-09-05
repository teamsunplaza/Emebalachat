#include "i18n.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace emebalachat {

namespace {

UiLocale s_current_locale = UiLocale::English;

const wchar_t kRunRegistryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValueName[] = L"Emebalachat";

struct LocalizedStrings {
    const wchar_t* menu_status_active;
    const wchar_t* menu_status_paused;
    const wchar_t* menu_engine;
    const wchar_t* menu_engine_google;
    const wchar_t* menu_engine_local;
    const wchar_t* menu_source_lang;
    const wchar_t* menu_target_lang;
    const wchar_t* menu_swap_langs;
    const wchar_t* menu_auto_send;
    const wchar_t* menu_sound_feedback;
    const wchar_t* menu_show_badge;
    const wchar_t* menu_start_with_windows;
    const wchar_t* menu_cheatsheet;
    const wchar_t* menu_exit;

    const wchar_t* cheatsheet_title;
    const wchar_t* cheatsheet_body;

    const wchar_t* badge_active;
    const wchar_t* badge_translating;
    const wchar_t* badge_paused;

    const wchar_t* tooltip_title;
    const wchar_t* auto_detect;
};

// 1. Korean (ko)
const LocalizedStrings kStringsKorean = {
    L"상태: 활성 (F9: 일시 정지)",
    L"상태: 일시 정지 (F9: 활성화)",
    L"번역 엔진 선택",
    L"Google 번역 (무료 / 무설치 / 실시간)",
    L"로컬 LLM (Hy-MT2-1.8B 오프라인 모델)",
    L"출발 언어 (입력 언어)",
    L"도착 언어 (번역 대상)",
    L"출발어 ⇄ 도착어 맞교환 (더블클릭)",
    L"엔터 시 자동 전송 (Auto-Send)",
    L"알림음 효과 (Tones)",
    L"화면 플로팅 뱃지 표시",
    L"Windows 시작 시 자동 실행",
    L"단축키 안내 및 사용법 (도움말)...",
    L"Emebalachat 종료",

    L"Emebalachat 단축키 및 사용 안내",
    L"Emebalachat 단축키 및 간편 사용법:\n\n"
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

    L"활성",
    L"번역 중...",
    L"일시 정지",

    L"Emebalachat",
    L"자동 감지"
};

// 2. Japanese (ja)
const LocalizedStrings kStringsJapanese = {
    L"状態: 有効 (F9: 一時停止)",
    L"状態: 一時停止 (F9: 再開)",
    L"翻訳エンジンの選択",
    L"Google 翻訳 (無料 / インストール不要)",
    L"ローカル LLM (Hy-MT2-1.8B オフライン)",
    L"元の言語 (入力)",
    L"翻訳先言語 (ターゲット)",
    L"言語を入れ替える (ダブルクリック)",
    L"Enterで自動送信 (Auto-Send)",
    L"効果音 (Sound)",
    L"フローティングバッジを表示",
    L"Windows 起動時に自動実行",
    L"ショートカット案内とヘルプ...",
    L"Emebalachat を終了",

    L"Emebalachat ショートカットと使用案内",
    L"Emebalachat ショートカットと使用案内:\n\n"
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

    L"有効",
    L"翻訳中...",
    L"一時停止",

    L"Emebalachat",
    L"自動検出"
};

// 3. Chinese Simplified (zh-CN)
const LocalizedStrings kStringsChineseSimp = {
    L"状态: 运行中 (F9: 暂停)",
    L"状态: 已暂停 (F9: 启用)",
    L"选择翻译引擎",
    L"Google 翻译 (免费 / 免安装 / 极速)",
    L"本地 LLM (Hy-MT2-1.8B 离线模型)",
    L"源语言 (输入语言)",
    L"目标语言 (翻译目标)",
    L"源语言 ⇄ 目标语言 互换 (双击)",
    L"回车自动发送 (Auto-Send)",
    L"声音提示反馈 (Tones)",
    L"显示桌面悬浮徽章",
    L"开机自动启动 (Start with Windows)",
    L"快捷键与使用说明 (帮助)...",
    L"退出 Emebalachat",

    L"Emebalachat 快捷键与使用说明",
    L"Emebalachat 快捷键与使用说明:\n\n"
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

    L"运行中",
    L"翻译中...",
    L"已暂停",

    L"Emebalachat",
    L"自动检测"
};

// 4. Chinese Traditional (zh-TW)
const LocalizedStrings kStringsChineseTrad = {
    L"狀態: 運行中 (F9: 暫停)",
    L"狀態: 已暫停 (F9: 啟用)",
    L"選擇翻譯引擎",
    L"Google 翻譯 (免費 / 免安裝 / 線上)",
    L"本地 LLM (Hy-MT2-1.8B 離線模型)",
    L"來源語言 (輸入語言)",
    L"目標語言 (翻譯目標)",
    L"來源語言 ⇄ 目標語言 對調 (雙擊)",
    L"Enter 自動發送 (Auto-Send)",
    L"音效回饋 (Sound)",
    L"顯示桌面懸浮徽章",
    L"開機時自動啟動",
    L"快捷鍵與使用說明 (說明)...",
    L"結束 Emebalachat",

    L"Emebalachat 快捷鍵與使用說明",
    L"Emebalachat 快捷鍵與使用說明:\n\n"
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

    L"運行中",
    L"翻譯中...",
    L"已暫停",

    L"Emebalachat",
    L"自動檢測"
};

// 5. Vietnamese (vi)
const LocalizedStrings kStringsVietnamese = {
    L"Trạng thái: Đang bật (F9: Tạm dừng)",
    L"Trạng thái: Tạm dừng (F9: Bật lại)",
    L"Chọn công cụ dịch",
    L"Google Dịch (Miễn phí / Trực tuyến)",
    L"Mô hình cục bộ LLM (Hy-MT2-1.8B)",
    L"Ngôn ngữ nguồn (Nhập)",
    L"Ngôn ngữ đích (Dịch sang)",
    L"Hoán đổi nguồn ⇄ đích (Nhấp đúp)",
    L"Tự động gửi khi nhấn Enter (Auto-Send)",
    L"Âm thanh thông báo (Sound)",
    L"Hiển thị huy hiệu nổi",
    L"Khởi động cùng Windows",
    L"Hướng dẫn phím tắt (Trợ giúp)...",
    L"Thoát Emebalachat",

    L"Hướng dẫn sử dụng Emebalachat",
    L"Phím tắt & Hướng dẫn sử dụng Emebalachat:\n\n"
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

    L"Đang bật",
    L"Đang dịch...",
    L"Tạm dừng",

    L"Emebalachat",
    L"Tự động phát hiện"
};

// 6. Spanish (es)
const LocalizedStrings kStringsSpanish = {
    L"Estado: Activo (F9: Pausar)",
    L"Estado: Pausado (F9: Activar)",
    L"Motor de traducción",
    L"Google Translate (Gratuito / En línea)",
    L"LLM Local (Hy-MT2-1.8B Offline)",
    L"Idioma de origen (Entrada)",
    L"Idioma de destino (Traducción)",
    L"Intercambiar idiomas (Doble clic)",
    L"Enviar automáticamente con Enter",
    L"Sonidos de notificación",
    L"Mostrar insignia flotante",
    L"Iniciar con Windows",
    L"Guía de atajos de teclado...",
    L"Salir de Emebalachat",

    L"Guía de atajos de Emebalachat",
    L"Guía de uso y atajos de Emebalachat:\n\n"
    L"  • F9 : Activar / Pausar\n"
    L"  • Ctrl + F9 : Cambiar idioma de destino\n"
    L"  • Ctrl + Shift + Enter : Alternar envío automático\n"
    L"  • Shift + Enter : Traducir y enviar de inmediato\n\n"
    L"Acciones de ratón en la insignia:\n"
    L"  • Clic izquierdo : Activar / Pausar\n"
    L"  • Doble clic : Intercambiar origen ⇄ destino\n"
    L"  • Clic derecho : Abrir menú de opciones",

    L"Activo",
    L"Traduciendo...",
    L"Pausado",

    L"Emebalachat",
    L"Detectar automáticamente"
};

// 7. English (en) - Default Fallback
const LocalizedStrings kStringsEnglish = {
    L"Status: Active (F9: Pause)",
    L"Status: Paused (F9: Resume)",
    L"Translation Engine",
    L"Google Translate (Free / Zero-Install)",
    L"Local LLM (Hy-MT2-1.8B GGUF Offline)",
    L"Source Language (Input)",
    L"Target Language (Output)",
    L"Swap Source ⇄ Target (Double-click)",
    L"Auto-Send on Enter",
    L"Sound Feedback (Tones)",
    L"Floating Badge Visible",
    L"Start with Windows",
    L"Hotkey Cheat Sheet & Help...",
    L"Exit Emebalachat",

    L"Emebalachat Hotkeys & Usage Guide",
    L"Emebalachat Hotkeys & Usage Guide:\n\n"
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

    L"Active",
    L"Translating...",
    L"Paused",

    L"Emebalachat",
    L"Auto Detect"
};

const LocalizedStrings& GetStrings(UiLocale loc) {
    switch (loc) {
        case UiLocale::Korean:
            return kStringsKorean;
        case UiLocale::Japanese:
            return kStringsJapanese;
        case UiLocale::ChineseSimplified:
            return kStringsChineseSimp;
        case UiLocale::ChineseTraditional:
            return kStringsChineseTrad;
        case UiLocale::Vietnamese:
            return kStringsVietnamese;
        case UiLocale::Spanish:
            return kStringsSpanish;
        case UiLocale::English:
        default:
            return kStringsEnglish;
    }
}

} // namespace

void I18n::Initialize(std::string_view config_ui_lang) {
    if (config_ui_lang == "auto" || config_ui_lang.empty()) {
        s_current_locale = DetectSystemLocale();
    } else {
        s_current_locale = StringToLocale(config_ui_lang);
    }
}

void I18n::SetLocale(UiLocale locale) {
    s_current_locale = locale;
}

UiLocale I18n::GetCurrentLocale() {
    return s_current_locale;
}

std::string_view I18n::GetLocaleCode() {
    switch (s_current_locale) {
        case UiLocale::Korean: return "ko";
        case UiLocale::Japanese: return "ja";
        case UiLocale::ChineseSimplified: return "zh-CN";
        case UiLocale::ChineseTraditional: return "zh-TW";
        case UiLocale::Vietnamese: return "vi";
        case UiLocale::Spanish: return "es";
        case UiLocale::French: return "fr";
        case UiLocale::German: return "de";
        case UiLocale::Russian: return "ru";
        case UiLocale::English:
        default:
            return "en";
    }
}

std::wstring I18n::Get(StringId id) {
    const auto& s = GetStrings(s_current_locale);
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

        case StringId::CheatSheetTitle: return s.cheatsheet_title;
        case StringId::CheatSheetBody: return s.cheatsheet_body;

        case StringId::BadgeActive: return s.badge_active;
        case StringId::BadgeTranslating: return s.badge_translating;
        case StringId::BadgePaused: return s.badge_paused;

        case StringId::TooltipTitle: return s.tooltip_title;
        case StringId::AutoDetect: return s.auto_detect;
        default: return L"";
    }
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
            if (s_current_locale == UiLocale::Korean) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else if (s_current_locale == UiLocale::Japanese) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else if (s_current_locale == UiLocale::ChineseSimplified || s_current_locale == UiLocale::ChineseTraditional) {
                return ToUtf16(lang.name_native) + L" (" + ToUtf16(lang.name_en) + L")";
            } else {
                return ToUtf16(lang.name_en) + L" (" + ToUtf16(lang.name_native) + L")";
            }
        }
    }

    return ToUtf16(lang_code);
}

UiLocale I18n::DetectSystemLocale() {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::wstring loc(localeName);
        if (loc.rfind(L"ko", 0) == 0) return UiLocale::Korean;
        if (loc.rfind(L"ja", 0) == 0) return UiLocale::Japanese;
        if (loc == L"zh-CN" || loc == L"zh-Hans" || loc == L"zh-SG") return UiLocale::ChineseSimplified;
        if (loc == L"zh-TW" || loc == L"zh-Hant" || loc == L"zh-HK" || loc == L"zh-MO") return UiLocale::ChineseTraditional;
        if (loc.rfind(L"vi", 0) == 0) return UiLocale::Vietnamese;
        if (loc.rfind(L"es", 0) == 0) return UiLocale::Spanish;
        if (loc.rfind(L"fr", 0) == 0) return UiLocale::French;
        if (loc.rfind(L"de", 0) == 0) return UiLocale::German;
        if (loc.rfind(L"ru", 0) == 0) return UiLocale::Russian;
    }

    LANGID langId = ::GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(langId);
    WORD sub = SUBLANGID(langId);

    switch (primary) {
        case LANG_KOREAN: return UiLocale::Korean;
        case LANG_JAPANESE: return UiLocale::Japanese;
        case LANG_CHINESE:
            if (sub == SUBLANG_CHINESE_SIMPLIFIED || sub == SUBLANG_CHINESE_SINGAPORE) {
                return UiLocale::ChineseSimplified;
            } else {
                return UiLocale::ChineseTraditional;
            }
        case LANG_VIETNAMESE: return UiLocale::Vietnamese;
        case LANG_SPANISH: return UiLocale::Spanish;
        case LANG_FRENCH: return UiLocale::French;
        case LANG_GERMAN: return UiLocale::German;
        case LANG_RUSSIAN: return UiLocale::Russian;
        default: return UiLocale::English;
    }
}

std::string I18n::GetDefaultTargetLanguage(UiLocale locale) {
    switch (locale) {
        case UiLocale::Korean:
            return "English";
        case UiLocale::Japanese:
            return "English";
        case UiLocale::ChineseSimplified:
        case UiLocale::ChineseTraditional:
            return "English";
        case UiLocale::Vietnamese:
            return "English";
        case UiLocale::Spanish:
            return "English";
        case UiLocale::English:
        default:
            return "Korean";
    }
}

UiLocale I18n::StringToLocale(std::string_view str) {
    if (str == "ko") return UiLocale::Korean;
    if (str == "ja") return UiLocale::Japanese;
    if (str == "zh-CN" || str == "zh_cn" || str == "zh") return UiLocale::ChineseSimplified;
    if (str == "zh-TW" || str == "zh_tw") return UiLocale::ChineseTraditional;
    if (str == "vi") return UiLocale::Vietnamese;
    if (str == "es") return UiLocale::Spanish;
    if (str == "fr") return UiLocale::French;
    if (str == "de") return UiLocale::German;
    if (str == "ru") return UiLocale::Russian;
    return UiLocale::English;
}

std::string I18n::LocaleToString(UiLocale locale) {
    switch (locale) {
        case UiLocale::Korean: return "ko";
        case UiLocale::Japanese: return "ja";
        case UiLocale::ChineseSimplified: return "zh-CN";
        case UiLocale::ChineseTraditional: return "zh-TW";
        case UiLocale::Vietnamese: return "vi";
        case UiLocale::Spanish: return "es";
        case UiLocale::French: return "fr";
        case UiLocale::German: return "de";
        case UiLocale::Russian: return "ru";
        case UiLocale::English:
        default:
            return "en";
    }
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
