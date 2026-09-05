#include "i18n.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic> // R6 Phase 6 (plan §5.4): race-free runtime locale switching
#include <cctype>
#include <unordered_map>

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
    L"Emebala Chat 종료",
    L"Emebala Chat 소개…",

    L"Emebala Chat 단축키 및 사용 안내",
    L"Emebala Chat 단축키 및 간편 사용법:\n\n"
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
    L"Emebala Chat 소개",

    L"활성",
    L"번역 중...",
    L"일시 정지",

    L"Emebala Chat",
    L"선택한 텍스트를 복사하지 못했습니다. 대상 앱을 확인하고 다시 시도하세요.",
    L"번역할 텍스트가 선택되어 있지 않습니다.",
    L"자동 감지",

    L"Emebala Chat이 백그라운드에서 이미 실행 중입니다.\n시스템 알림 트레이를 확인하세요.",
    L"COM 초기화에 실패했습니다.\n플로팅 배지와 음성 읽기(TTS)는 사용할 수 없지만,\n번역, 단축키, 트레이, 알림음은 계속 동작합니다.",
    L"복사·붙여넣기는 이제 그만. 모국어로 자연스럽게 입력하면 어떤 Windows 앱에서든 실시간으로 번역문이 타이핑을 대체합니다.",
    L"⚡ 드래그 번역 — 어떤 앱에서든 텍스트를 선택하면 플로팅 아이콘이 즉시 번역합니다.",
    L"🔊 뉴럴 TTS — KO/EN/JA/DE 발음 지원.",
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
    L"자동 (시스템 언어)"
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
    L"Emebala Chat を終了",
    L"Emebala Chat について…",

    L"Emebala Chat ショートカットと使用案内",
    L"Emebala Chat ショートカットと使用案内:\n\n"
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
    L"Emebala Chat について",

    L"有効",
    L"翻訳中...",
    L"一時停止",

    L"Emebala Chat",
    L"選択したテキストをコピーできませんでした。対象アプリを確認して再試行してください。",
    L"翻訳するテキストが選択されていません。",
    L"自動検出",

    L"Emebala Chat はすでにバックグラウンドで実行中です。\nシステムトレイを確認してください。",
    L"COM の初期化に失敗しました。\nフローティングバッジと音声読み上げは利用できませんが、\n翻訳・ショートカット・トレイ・効果音は引き続き動作します。",
    L"コピー＆ペーストはもう不要。母語で自然に入力すると、あらゆる Windows アプリの中で打鍵がリアルタイムに翻訳へ置きわります。",
    L"⚡ ドラッグ翻訳 — 任意のアプリでテキストを選択すると、フローティングアイコンが即座に翻訳。",
    L"🔊 ニューラルTTS — KO/EN/JA/DE の発音。",
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
    L"自動 (システム言語)"
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
    L"退出 Emebala Chat",
    L"关于 Emebala Chat…",

    L"Emebala Chat 快捷键与使用说明",
    L"Emebala Chat 快捷键与使用说明:\n\n"
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
    L"关于 Emebala Chat",

    L"运行中",
    L"翻译中...",
    L"已暂停",

    L"Emebala Chat",
    L"无法复制所选文本。请检查目标应用后重试。",
    L"未选择要翻译的文本。",
    L"自动检测",

    L"Emebala Chat 已在后台运行。\n请查看系统通知托盘。",
    L"COM 初始化失败。\n悬浮徽章和语音朗读将不可用，\n但翻译、快捷键、托盘和提示音仍可正常使用。",
    L"告别复制粘贴。用母语自然输入，译文会在任何 Windows 应用中实时替换你的键入。",
    L"⚡ 拖拽翻译 — 在任意应用中选中文本，悬浮图标即刻翻译。",
    L"🔊 神经 TTS — 支持 KO/EN/JA/DE 发音。",
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
    L"自动（系统语言）"
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
    L"結束 Emebala Chat",
    L"關於 Emebala Chat…",

    L"Emebala Chat 快捷鍵與使用說明",
    L"Emebala Chat 快捷鍵與使用說明:\n\n"
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
    L"關於 Emebala Chat",

    L"運行中",
    L"翻譯中...",
    L"已暫停",

    L"Emebala Chat",
    L"無法複製所選文字。請檢查目標應用程式後重試。",
    L"尚未選取要翻譯的文字。",
    L"自動檢測",

    L"Emebala Chat 已在背景執行。\n請檢視系統通知列。",
    L"COM 初始化失敗。\n懸浮徽章與語音朗讀將不可用，\n但翻譯、快捷鍵、系統匣與提示音仍可正常使用。",
    L"告別複製貼上。用母語自然輸入，譯文會在任何 Windows 應用程式中即時取代你的鍵入。",
    L"⚡ 拖曳翻譯 — 在任何應用程式中選取文字，懸浮圖示立即翻譯。",
    L"🔊 神經 TTS — 支援 KO/EN/JA/DE 發音。",
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
    L"自動（系統語言）"
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
    L"🔊 TTS thần kinh — phát âm KO/EN/JA/DE.",
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
    L"Tự động (ngôn ngữ hệ thống)"
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
    L"🔊 TTS neuronal — pronunciación KO/EN/JA/DE.",
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
    L"Automático (idioma del sistema)"
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
    L"\U0001F50A Neural TTS \u2014 KO/EN/JA/DE pronunciation.",
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
    L"Auto (system language)"
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
    switch (GetCurrentLocale()) {
        case UiLocale::Korean: return "ko";
        case UiLocale::Japanese: return "ja";
        case UiLocale::ChineseSimplified: return "zh-CN";
        case UiLocale::ChineseTraditional: return "zh-TW";
        case UiLocale::Vietnamese: return "vi";
        case UiLocale::Spanish: return "es";
        case UiLocale::English:
        case UiLocale::Auto: // never resolved (Initialize maps Auto -> Detect); en fallback
        default:
            return "en";
    }
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
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::wstring loc(localeName);
        if (loc.rfind(L"ko", 0) == 0) return UiLocale::Korean;
        if (loc.rfind(L"ja", 0) == 0) return UiLocale::Japanese;
        if (loc == L"zh-CN" || loc == L"zh-Hans" || loc == L"zh-SG") return UiLocale::ChineseSimplified;
        if (loc == L"zh-TW" || loc == L"zh-Hant" || loc == L"zh-HK" || loc == L"zh-MO") return UiLocale::ChineseTraditional;
        if (loc.rfind(L"vi", 0) == 0) return UiLocale::Vietnamese;
        if (loc.rfind(L"es", 0) == 0) return UiLocale::Spanish;
        // R6 Phase 6: fr/de/ru OS languages intentionally fall through to the
        // LANGID switch below and then to English - no translation tables
        // exist for them (half-wired locales removed per plan §5.5).
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
    if (IEqualsAscii(str, "ko")) return UiLocale::Korean;
    if (IEqualsAscii(str, "ja")) return UiLocale::Japanese;
    if (IEqualsAscii(str, "zh-CN") || IEqualsAscii(str, "zh_cn") || IEqualsAscii(str, "zh")) return UiLocale::ChineseSimplified;
    if (IEqualsAscii(str, "zh-TW") || IEqualsAscii(str, "zh_tw")) return UiLocale::ChineseTraditional;
    if (IEqualsAscii(str, "vi")) return UiLocale::Vietnamese;
    if (IEqualsAscii(str, "es")) return UiLocale::Spanish;
    if (IEqualsAscii(str, "auto")) return UiLocale::Auto;
    // R6 Phase 6: "fr"/"de"/"ru" (and any unknown value) resolve to English.
    // The old half-wired mapping is gone; PlanUiLocaleChange REFUSES those
    // codes at the selector so they can never be (re-)persisted.
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
        case UiLocale::Auto: return "auto";
        case UiLocale::English:
        default:
            return "en";
    }
}

const std::vector<UiLocaleEntry>& GetSupportedUiLocales() {
    // Endonyms (each language written in its own script) - the universal
    // convention for language pickers, so these are NOT routed through the
    // locale tables. Selector display order (plan §5.4): KO, JA, zh-CN,
    // zh-TW, VI, ES, EN.
    static const std::vector<UiLocaleEntry> kEntries = {
        { UiLocale::Korean, L"한국어" },
        { UiLocale::Japanese, L"日本語" },
        { UiLocale::ChineseSimplified, L"简体中文" },
        { UiLocale::ChineseTraditional, L"繁體中文" },
        { UiLocale::Vietnamese, L"Tiếng Việt" },
        { UiLocale::Spanish, L"Español" },
        { UiLocale::English, L"English" },
    };
    return kEntries;
}

UiLocaleChangePlan PlanUiLocaleChange(std::string_view current_persisted,
                                      std::string_view requested) {
    UiLocaleChangePlan plan;
    const std::string req(requested);

    // Reject at the selector boundary: "auto" or an exact populated-locale
    // code (any ASCII case - normalized to the canonical table spelling).
    std::string canonical;
    if (IEqualsAscii(req, "auto")) {
        canonical = "auto";
    } else {
        for (const auto& entry : GetSupportedUiLocales()) {
            const std::string code = I18n::LocaleToString(entry.locale);
            if (IEqualsAscii(req, code)) {
                canonical = code;
                break;
            }
        }
    }
    if (canonical.empty()) {
        plan.valid = false; // unknown/removed locale: apply NOTHING (half-wired guard)
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
