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
    const wchar_t* menu_about;

    const wchar_t* cheatsheet_title;
    const wchar_t* cheatsheet_body;
    const wchar_t* about_title;

    const wchar_t* badge_active;
    const wchar_t* badge_translating;
    const wchar_t* badge_paused;

    const wchar_t* tooltip_title;
    // REQ-R1 (session 260905_0001): drag-icon click failure notices.
    const wchar_t* tooltip_copy_failed;
    const wchar_t* tooltip_no_selection;
    const wchar_t* auto_detect;

    // ---- R6 Phase 5/6 (plan §5.2-§5.4): About body, migrated literals,
    // UI-language selector. Field order below MUST match the Get() switch and
    // every locale table (aggregate initialization). ----
    const wchar_t* app_already_running;
    const wchar_t* app_com_failed;
    const wchar_t* about_tagline;
    const wchar_t* about_feature0;
    const wchar_t* about_feature1;
    const wchar_t* about_feature2;
    const wchar_t* about_etymology;
    const wchar_t* about_link_website;
    const wchar_t* about_link_contact;
    const wchar_t* about_link_reddit;
    const wchar_t* about_contact_org;
    const wchar_t* about_contact_phone;
    const wchar_t* about_contact_lead;
    const wchar_t* tooltip_copied;
    const wchar_t* tooltip_button_copy;
    const wchar_t* tooltip_button_tts;
    const wchar_t* menu_ui_language;
    const wchar_t* menu_ui_language_auto;

    // Phase 4 (REQ-020, plan §1.4/§2.5): About-window "Reset to system
    // defaults" button label + its transient post-click confirmation label.
    // Appended at the end so every aggregate table below only gains trailing
    // initializers (field order MUST keep matching table order).
    const wchar_t* about_reset_button;
    const wchar_t* about_reset_done;

    // REQ-025 (Phase A §2.1.A3-25): tray language-picker group headers.
    // Appended at the end (same trailing-initializer rule as above).
    const wchar_t* menu_typing_group;
    const wchar_t* menu_tooltip_group;

    // REQ-B-001 (session 260909_0001 Batch-1): per-locale brand display
    // name. Appended at the end (same trailing-initializer rule as above).
    const wchar_t* app_name;
    // REQ-C-004 (session 260909_0001 Batch-1): no-voice TTS notice body
    // (Phase C, design §1.2.2). Same trailing-initializer rule.
    const wchar_t* tooltip_no_tts_voice;
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
    L"에메발라 챗 종료",
    L"에메발라 챗 소개…",

    L"에메발라 챗 단축키 및 사용 안내",
    L"에메발라 챗 단축키 및 간편 사용법:\n\n"
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
    L"에메발라 챗 소개",

    L"활성",
    L"번역 중...",
    L"일시 정지",

    L"에메발라 챗",
    L"선택한 텍스트를 복사하지 못했습니다. 대상 앱을 확인하고 다시 시도하세요.",
    L"번역할 텍스트가 선택되어 있지 않습니다.",
    L"자동 감지",

    L"에메발라 챗이 백그라운드에서 이미 실행 중입니다.\n시스템 알림 트레이를 확인하세요.",
    L"COM 초기화에 실패했습니다.\n플로팅 배지와 음성 읽기(TTS)는 사용할 수 없지만,\n번역, 단축키, 트레이, 알림음은 계속 동작합니다.",
    L"복사·붙여넣기는 이제 그만. 모국어로 자연스럽게 입력하면 어떤 Windows 앱에서든 실시간으로 번역문이 타이핑을 대체합니다.",
    L"⚡ 드래그 번역 — 어떤 앱에서든 텍스트를 선택하면 플로팅 아이콘이 즉시 번역합니다.",
    L"🔊 뉴럴 TTS — Windows 음성팩 연동 시 37개 전 언어 발음 지원.",
    L"🔒 100% 온디바이스·프라이빗 — 단축키를 누르는 동안만 작동하며 클립보드는 건드리지 않습니다.",
    L"기원전 2000년, 메소포타미아 서기들은 언어로 세계를 잇는 자들을 '에메-발라(Eme-bala)'라 불렀습니다.",
    L"웹사이트",
    L"문의",
    L"Reddit",
    L"Team Sunplaza · 서울 영등포 (영중로 65, 219호)",
    L"+82 2 575 0414 · 업무시간 10:00–19:00 KST",
    L"총괄 아키텍트: Yongtai Kim",
    L"✓ 복사됨!",
    L"📋 복사",
    L"🔊 음성",
    L"인터페이스 언어",
    L"자동 (시스템 언어)",
    L"시스템 기본값으로 리셋",
    L"기본값으로 복원됨",
    L"키보드 타이핑",
    L"번역 툴팁",
    L"에메발라 챗",
    L"이 언어에 설치된 Windows 음성이 없습니다. 🔊를 다시 클릭하면 음성 설정이 열립니다."
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
    L"この言語用の Windows 音声がインストールされていません。🔊 をもう一度クリックすると音声設定が開きます。"
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
    L"此语言未安装 Windows 语音。再次点击 🔊 可打开语音设置。"
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
    L"此語言未安裝 Windows 語音。再次點擊 🔊 可開啟語音設定。"
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
    L"Chưa cài đặt giọng nói Windows cho ngôn ngữ này. Nhấp lại 🔊 để mở Cài đặt Nhận dạng và giọng nói."
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
    L"No hay una voz de Windows instalada para este idioma. Haz clic de nuevo en 🔊 para abrir la configuración de Voz."
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
    L"No Windows voice installed for this language. Click 🔊 again to open Speech settings."
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
    L"LLM local (Hy-MT2-1.8B hors ligne)",
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
    L"Aucune voix Windows installée pour cette langue. Cliquez à nouveau sur 🔊 pour ouvrir les paramètres de la Voix."
};

// 9. German (de)
const LocalizedStrings kStringsGerman = {
    L"Status: Aktiv (F9: Pause)",
    L"Status: Pausiert (F9: Fortsetzen)",
    L"Übersetzungsengine",
    L"Google Übersetzen (kostenlos / ohne Installation)",
    L"Lokales LLM (Hy-MT2-1.8B offline)",
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
    L"Für diese Sprache ist keine Windows-Stimme installiert. Klicken Sie erneut auf 🔊, um die Spracheinstellungen zu öffnen."
};

// 10. Russian (ru)
const LocalizedStrings kStringsRussian = {
    L"Состояние: активно (F9: пауза)",
    L"Состояние: пауза (F9: возобновить)",
    L"Движок перевода",
    L"Google Переводчик (бесплатно / без установки)",
    L"Локальная LLM (Hy-MT2-1.8B офлайн)",
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
    L"Для этого языка не установлен голос Windows. Нажмите 🔊 ещё раз, чтобы открыть параметры распознавания речи."
};

// 11. Portuguese (pt)
const LocalizedStrings kStringsPortuguese = {
    L"Estado: ativo (F9: pausar)",
    L"Estado: pausado (F9: retomar)",
    L"Mecanismo de tradução",
    L"Google Tradutor (grátis / sem instalação)",
    L"LLM local (Hy-MT2-1.8B offline)",
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
    L"Não há voz do Windows instalada para este idioma. Clique novamente em 🔊 para abrir as configurações de Fala."
};

// 12. Italian (it)
const LocalizedStrings kStringsItalian = {
    L"Stato: attivo (F9: pausa)",
    L"Stato: in pausa (F9: riprendi)",
    L"Motore di traduzione",
    L"Google Traduttore (gratis / senza installazione)",
    L"LLM locale (Hy-MT2-1.8B offline)",
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
    L"Nessuna voce Windows installata per questa lingua. Fai di nuovo clic su 🔊 per aprire le impostazioni di Riconoscimento vocale."
};

// 13. Dutch (nl)
const LocalizedStrings kStringsDutch = {
    L"Status: actief (F9: pauze)",
    L"Status: gepauzeerd (F9: hervatten)",
    L"Vertaalengine",
    L"Google Vertalen (gratis / zonder installatie)",
    L"Lokale LLM (Hy-MT2-1.8B offline)",
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
    L"Er is geen Windows-stem geïnstalleerd voor deze taal. Klik nogmaals op 🔊 om de Spraak-instellingen te openen."
};

// 14. Polish (pl)
const LocalizedStrings kStringsPolish = {
    L"Status: aktywny (F9: pauza)",
    L"Status: wstrzymany (F9: wznow)",
    L"Silnik tłumaczenia",
    L"Tłumacz Google (darmowy / bez instalacji)",
    L"Lokalny LLM (Hy-MT2-1.8B offline)",
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
    L"Dla tego języka nie zainstalowano głosu Windows. Kliknij ponownie 🔊, aby otworzyć ustawienia Mowy."
};

// 15. Czech (cs)
const LocalizedStrings kStringsCzech = {
    L"Stav: aktivní (F9: pauza)",
    L"Stav: pozastaveno (F9: pokračovat)",
    L"Překladový engine",
    L"Google Překladač (zdarma / bez instalace)",
    L"Lokální LLM (Hy-MT2-1.8B offline)",
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
    L"Pro tento jazyk není nainstalován žádný hlas Windows. Klikněte znovu na 🔊 pro otevření nastavení Rozpoznávání řeči."
};

// 16. Hungarian (hu)
const LocalizedStrings kStringsHungarian = {
    L"Állapot: aktív (F9: szünet)",
    L"Állapot: szünetel (F9: folytatás)",
    L"Fordítómotor",
    L"Google Fordító (ingyenes / telepítés nélkül)",
    L"Helyi LLM (Hy-MT2-1.8B offline)",
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
    L"Ehhez a nyelvhez nincs telepítve Windows-hang. Kattintson újra a 🔊 gombra a Beszéd beállításainak megnyitásához."
};

// 17. Romanian (ro)
const LocalizedStrings kStringsRomanian = {
    L"Stare: activ (F9: pauză)",
    L"Stare: întrerupt (F9: reia)",
    L"Motor de traducere",
    L"Google Translate (gratuit / fără instalare)",
    L"LLM local (Hy-MT2-1.8B offline)",
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
    L"Nu există o voce Windows instalată pentru această limbă. Faceți clic din nou pe 🔊 pentru a deschide setările Vocii."
};

// 18. Swedish (sv)
const LocalizedStrings kStringsSwedish = {
    L"Status: aktiv (F9: paus)",
    L"Status: pausad (F9: återuppta)",
    L"Översättningsmotor",
    L"Google Översätt (gratis / utan installation)",
    L"Lokal LLM (Hy-MT2-1.8B offline)",
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
    L"Ingen Windows-röst är installerad för det här språket. Klicka på 🔊 igen för att öppna Tal-inställningarna."
};

// 19. Danish (da)
const LocalizedStrings kStringsDanish = {
    L"Status: aktiv (F9: pause)",
    L"Status: pause (F9: genoptag)",
    L"Oversættelsesmotor",
    L"Google Oversæt (gratis / uden installation)",
    L"Lokal LLM (Hy-MT2-1.8B offline)",
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
    L"Der er ikke installeret en Windows-tale til dette sprog. Klik på 🔊 igen for at åbne Tale-indstillingerne."
};

// 20. Finnish (fi)
const LocalizedStrings kStringsFinnish = {
    L"Tila: aktiivinen (F9: tauko)",
    L"Tila: keskeytetty (F9: jatka)",
    L"Käännösmoottori",
    L"Google Kääntäjä (ilmainen / ei asennusta)",
    L"Paikallinen LLM (Hy-MT2-1.8B offline)",
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
    L"Tälle kielelle ei ole asennettu Windows-ääntä. Napsauta 🔊 uudelleen avataksesi Puhe-asetukset."
};

// 21. Norwegian (no / nb)
const LocalizedStrings kStringsNorwegian = {
    L"Status: aktiv (F9: pause)",
    L"Status: pause (F9: gjenoppta)",
    L"Oversettelsesmotor",
    L"Google Oversetter (gratis / uten installasjon)",
    L"Lokal LLM (Hy-MT2-1.8B offline)",
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
    L"Ingen Windows-stemme er installert for dette språket. Klikk på 🔊 igjen for å åpne Tale-innstillingene."
};

// 22. Greek (el)
const LocalizedStrings kStringsGreek = {
    L"Κατάσταση: ενεργό (F9: παύση)",
    L"Κατάσταση: σε παύση (F9: συνέχεια)",
    L"Μηχανή μετάφρασης",
    L"Google Μετάφραση (δωρεάν / χωρίς εγκατάσταση)",
    L"Τοπικό LLM (Hy-MT2-1.8B εκτός σύνδεσης)",
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
    L"Δεν είναι εγκατεστημένη φωνή των Windows για αυτή τη γλώσσα. Κάντε ξανά κλικ στο 🔊 για να ανοίξετε τις ρυθμίσεις Ομιλίας."
};

// 23. Turkish (tr)
const LocalizedStrings kStringsTurkish = {
    L"Durum: etkin (F9: duraklat)",
    L"Durum: duraklatıldı (F9: devam et)",
    L"Çeviri motoru",
    L"Google Çeviri (ücretsiz / kurulum gerektirmez)",
    L"Yerel LLM (Hy-MT2-1.8B çevrimdışı)",
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
    L"Bu dil için yüklü bir Windows sesi yok. Konuşma ayarlarını açmak için 🔊 simgesine yeniden tıklayın."
};

// 24. Ukrainian (uk)
const LocalizedStrings kStringsUkrainian = {
    L"Стан: активний (F9: пауза)",
    L"Стан: призупинено (F9: відновити)",
    L"Рушій перекладу",
    L"Google Перекладач (безкоштовно / без встановлення)",
    L"Локальна LLM (Hy-MT2-1.8B офлайн)",
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
    L"Для цієї мови не встановлено голос Windows. Натисніть 🔊 ще раз, щоб відкрити параметри мовлення."
};

// 25. Thai (th)
const LocalizedStrings kStringsThai = {
    L"สถานะ: ใช้งานอยู่ (F9: หยุดชั่วคราว)",
    L"สถานะ: หยุดชั่วคราว (F9: ทำงานต่อ)",
    L"ระบบแปลภาษา",
    L"Google แปลภาษา (ฟรี / ไม่ต้องติดตั้ง)",
    L"LLM ภายในเครื่อง (Hy-MT2-1.8B ออฟไลน์)",
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
    L"ยังไม่ได้ติดตั้งเสียงของ Windows สำหรับภาษานี้ คลิก 🔊 อีกครั้งเพื่อเปิดการตั้งค่าคำพูด"
};

// 26. Indonesian (id)
const LocalizedStrings kStringsIndonesian = {
    L"Status: aktif (F9: jeda)",
    L"Status: dijeda (F9: lanjutkan)",
    L"Mesin penerjemah",
    L"Google Translate (gratis / tanpa instalasi)",
    L"LLM lokal (Hy-MT2-1.8B luring)",
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
    L"Tidak ada suara Windows yang terpasang untuk bahasa ini. Klik 🔊 lagi untuk membuka pengaturan Ucapan."
};

// 27. Malay (ms)
const LocalizedStrings kStringsMalay = {
    L"Status: aktif (F9: jeda)",
    L"Status: dijeda (F9: sambung)",
    L"Enjin penterjemah",
    L"Google Terjemah (percuma / tanpa pemasangan)",
    L"LLM setempat (Hy-MT2-1.8B luar talian)",
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
    L"Tiada suara Windows dipasang untuk bahasa ini. Klik 🔊 sekali lagi untuk membuka tetapan Ucapan."
};

// 28. Filipino (fil)
// Note: "Website", "Google Translate" and "Reddit" are used in Philippine
// English-dominant UI practice; the surrounding copy is genuine Filipino.
const LocalizedStrings kStringsFilipino = {
    L"Katayuan: Aktibo (F9: I-pause)",
    L"Katayuan: Nakahinto (F9: Ipagpatuloy)",
    L"Makina ng pagsasalin",
    L"Google Translate (Libre / walang i-install)",
    L"Lokal na LLM (Hy-MT2-1.8B offline)",
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
    L"Walang naka-install na Windows voice para sa wikang ito. I-click muli ang 🔊 upang buksan ang mga setting ng Pagsasalita."
};

// 29. Hindi (hi)
const LocalizedStrings kStringsHindi = {
    L"स्थिति: सक्रिय (F9: रोकें)",
    L"स्थिति: रुका हुआ (F9: जारी रखें)",
    L"अनुवाद इंजन",
    L"Google अनुवाद (मुफ़्त / इंस्टॉलेशन नहीं)",
    L"स्थानीय LLM (Hy-MT2-1.8B ऑफ़लाइन)",
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
    L"इस भाषा के लिए कोई Windows वॉइस इंस्टॉल नहीं है. वॉइस सेटिंग खोलने के लिए 🔊 पर फिर से क्लिक करें."
};

// 30. Bengali (bn)
const LocalizedStrings kStringsBengali = {
    L"স্ট্যাটাস: সক্রিয় (F9: বিরতি)",
    L"স্ট্যাটাস: বিরতিপ্রাপ্ত (F9: পুনরায় চালু)",
    L"অনুবাদ ইঞ্জিন",
    L"Google Translate (বিনামূল্যে / ইনস্টল ছাড়াই)",
    L"লোকাল LLM (Hy-MT2-1.8B অফলাইন)",
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
    L"এই ভাষার জন্য কোনো Windows ভয়েস ইনস্টল করা নেই. ভয়েস সেটিংস খুলতে 🔊-এ আবার ক্লিক করুন."
};

// 31. Arabic (ar) — RTL language; string CONTENT is logical-order UTF-16, the
// UI chrome direction is decided by the render layer (bidi_utils).
const LocalizedStrings kStringsArabic = {
    L"الحالة: نشِط (F9: إيقاف مؤقت)",
    L"الحالة: متوقف مؤقتًا (F9: استئناف)",
    L"محرك الترجمة",
    L"ترجمة Google (مجاني / بدون تثبيت)",
    L"نموذج محلي LLM (Hy-MT2-1.8B دون اتصال)",
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
    L"لا يوجد صوت Windows مثبت لهذه اللغة. انقر فوق 🔊 مرة أخرى لفتح إعدادات الكلام."
};

// 32. Persian (fa) — RTL
const LocalizedStrings kStringsPersian = {
    L"وضعیت: فعال (F9: توقف موقت)",
    L"وضعیت: متوقف (F9: ادامه)",
    L"موتور ترجمه",
    L"Google Translate (رایگان / بدون نصب)",
    L"LLM محلی (Hy-MT2-1.8B آفلاین)",
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
    L"هیچ صدای Windows برای این زبان نصب نشده است. برای باز کردن تنظیمات گفتار، دوباره روی 🔊 کلیک کنید."
};

// 33. Urdu (ur) — RTL
const LocalizedStrings kStringsUrdu = {
    L"حالت: فعال (F9: وقفہ)",
    L"حالت: وقفے میں (F9: جاری رکھیں)",
    L"ترجمہ انجن",
    L"Google ٹرانسلیٹ (مفت / بغیر تنصیب)",
    L"مقامی LLM (Hy-MT2-1.8B آف لائن)",
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
    L"اس زبان کے لیے کوئی Windows وائس انسٹال نہیں ہے۔ اسپیچ سیٹنگز کھولنے کے لیے 🔊 پر دوبارہ کلک کریں۔"
};

// 34. Hebrew (he) — RTL
const LocalizedStrings kStringsHebrew = {
    L"סטטוס: פעיל (F9: השהיה)",
    L"סטטוס: מושהה (F9: המשך)",
    L"מנוע תרגום",
    L"Google Translate (חינם / ללא התקנה)",
    L"LLM מקומי (Hy-MT2-1.8B לא מקוון)",
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
    L"אין קול Windows מותקן עבור שפה זו. לחץ שוב על 🔊 כדי לפתוח את הגדרות הדיבור."
};

// 35. Khmer (km)
const LocalizedStrings kStringsKhmer = {
    L"ស្ថានភាព៖ សកម្ម (F9៖ ផ្អាក)",
    L"ស្ថានភាព៖ បានផ្អាក (F9៖ បន្ត)",
    L"ម៉ាស៊ីនបកប្រែ",
    L"Google បកប្រែ (ឥតគិតថ្លៃ / គ្មានការដំឡើង)",
    L"LLM ក្នុងម៉ាស៊ីន (Hy-MT2-1.8B ក្រៅបណ្ដាញ)",
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
    L"មិនមានសំឡេង Windows ត្រូវបានដំឡើងសម្រាប់ភាសានេះទេ។ ចុច 🔊 ម្តងទៀតដើម្បីបើកការកំណត់ការនិយាយ។"
};

// 36. Lao (lo)
const LocalizedStrings kStringsLao = {
    L"ສະຖານະ: ເປີດໃຊ້ (F9: ຢຸດຊົ່ວຄາວ)",
    L"ສະຖານະ: ຢຸດຊົ່ວຄາວ (F9: ສືບຕໍ່)",
    L"ເຄື່ອງຈັກແປ",
    L"Google ແປ (ຟຣີ / ບໍ່ຕ້ອງຕິດຕັ້ງ)",
    L"LLM ໃນເຄື່ອງ (Hy-MT2-1.8B ບໍ່ອອນລາຍ)",
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
    L"ບໍ່ມີສຽງ Windows ຕິດຕັ້ງສຳລັບພາສານີ້. ຄລິກ 🔊 ອີກເທື່ອໜຶ່ງເພື່ອເປີດການຕັ້ງຄ່າການເວົ້າ."
};

// 37. Burmese (my)
const LocalizedStrings kStringsBurmese = {
    L"အခြေအနေ: လုပ်ဆောင်နေ (F9: ခေတ္တရပ်)",
    L"အခြေအနေ: ခေတ္တရပ်ထား (F9: ပြန်စ)",
    L"ဘာသာပြန် အင်ဂျင်",
    L"Google Translate (အခမဲ့ / မတပ်ဆင်ရ)",
    L"ဒေသတွင်း LLM (Hy-MT2-1.8B offline)",
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
    L"ဤဘာသာစကားအတွက် Windows အသံ တပ်ဆင်ထားခြင်း မရှိပါ။ စပီခ် ဆက်တင်များ ဖွင့်ရန် 🔊 ကို ထပ်မံနှိပ်ပါ။"
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

        case StringId::EnumCount:
        default: return L""; // empty by design - the completeness test skips it
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
