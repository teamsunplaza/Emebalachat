// V10F L2-B (261122_0002 P2-G, v0.10.10 phased fixes): the translation
// editor window. SECTION A (the pure layout plan) lives header-inline in
// translation_editor_window.hpp so the unit suite drives it headlessly;
// this TU holds the only window-creating code (the modeless dialog, the
// RichEdit50W panes, and the two action buttons), which the suite links but
// never enters.

#include "translation_editor_window.hpp"

#include "../diag_logger.hpp"
#include "../i18n.hpp"
#include "../win32_input.hpp"
#include "dpi.hpp"
#include "openai_settings_window.hpp" // TemplateBuilder (REQ-046 P4-3)

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace emebalachat {
namespace translation_editor_window {

namespace {

// Control ids (unique per dialog; the manager dialog occupies the 47xx band,
// so this window takes 48xx).
enum : WORD {
    kIdSourcePane = 4800,
    kIdTargetPane,
    kIdBackPane,
    kIdRetranslate,
    kIdApply,
};

// Initial template size in DLU; WM_INITDIALOG resizes the window to the exact
// DIP plan immediately, so these only matter for the first paint frame.
inline constexpr short kTemplateWidthDlu = 320;
inline constexpr short kTemplateHeightDlu = 252;

// Custom message: an async on_done delivered the retranslated forward text;
// the payload waits in EditorState::pending_target (never posted as a
// pointer, so a window destroyed before the message drains cannot leak).
inline constexpr UINT kMsgResultReady = WM_APP + 0x120;

struct EditorState {
    translation_history::Entry entry;
    RetranslateFn retranslate;
    HWND hwnd = nullptr;
    HWND source_pane = nullptr;
    HWND target_pane = nullptr;
    HWND back_pane = nullptr;
    DWORD gui_thread_id = 0;
    HMODULE richedit_lib = nullptr; // msftedit.dll, process-lifetime on purpose
    bool richedit_ok = false;
    std::mutex result_m;
    std::optional<std::wstring> pending_target;
    // V10F-R2 (fix-list round 2, F-25): the apply single-flight flag. Set on
    // entry to OnApplyToOriginalWindow, cleared on every exit (the
    // ApplySingleFlight guard below); a re-request while the flag is held
    // early-returns. GUI thread only (the apply handler and its bounded pump
    // both run on it), held as an atomic to document the contract.
    std::atomic<bool> apply_in_progress{false};
};

EditorState& State() {
    static EditorState s;
    return s;
}

std::wstring GetPaneText(HWND pane) {
    if (pane == nullptr || !::IsWindow(pane)) return std::wstring();
    const int len = ::GetWindowTextLengthW(pane);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int copied = ::GetWindowTextW(pane, text.data(), len + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void SetPaneText(HWND pane, const std::wstring& text) {
    if (pane != nullptr && ::IsWindow(pane)) {
        ::SetWindowTextW(pane, text.c_str());
    }
}

void FillPanes(EditorState& st) {
    SetPaneText(st.source_pane, st.entry.original);
    SetPaneText(st.target_pane, st.entry.forward);
    SetPaneText(st.back_pane, st.entry.back);
}

RECT ScalePlanRect(const RECT& dip, UINT dpi) {
    RECT px;
    px.left = ui::ScaleDipsToPixels(dip.left, dpi);
    px.top = ui::ScaleDipsToPixels(dip.top, dpi);
    px.right = ui::ScaleDipsToPixels(dip.right, dpi);
    px.bottom = ui::ScaleDipsToPixels(dip.bottom, dpi);
    return px;
}

HWND CreatePane(HWND dlg, HINSTANCE inst, const wchar_t* class_name, const RECT& px,
                WORD id, bool read_only) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN |
                  WS_VSCROLL | ES_AUTOVSCROLL;
    if (read_only) style |= ES_READONLY;
    return ::CreateWindowExW(WS_EX_CLIENTEDGE, class_name, nullptr, style, px.left, px.top,
                             px.right - px.left, px.bottom - px.top, dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

HWND CreateButton(HWND dlg, HINSTANCE inst, const RECT& px, WORD id,
                  const std::wstring& label) {
    return ::CreateWindowExW(0, L"BUTTON", label.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             px.left, px.top, px.right - px.left, px.bottom - px.top, dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

void ApplyDefaultFont(HWND ctl) {
    if (ctl != nullptr) {
        ::SendMessageW(ctl, WM_SETFONT,
                       reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)),
                       TRUE);
    }
}

// [EditRetranslate]: read the edited source, clear the now-stale
// back-translation pane, and hand the retranslation to the caller's fn. The
// fn may deliver on_done from a worker thread; the delivery marshals through
// kMsgResultReady and fills the target pane on the GUI thread.
void OnRetranslate(EditorState& st) {
    const std::wstring source = GetPaneText(st.source_pane);
    ::SetWindowTextW(st.back_pane, L""); // stale: it describes the pre-edit source
    if (!st.retranslate) return;
    HWND self = st.hwnd;
    std::function<void(const std::wstring&)> on_done =
        [self](const std::wstring& result) {
            EditorState& s = State();
            {
                std::lock_guard<std::mutex> lk(s.result_m);
                s.pending_target = result;
            }
            if (self != nullptr && ::IsWindow(self)) {
                ::PostMessageW(self, kMsgResultReady, 0, 0);
            }
        };
    st.retranslate(source, st.entry.tgt_code, std::move(on_done));
}

// V10F-R2 (fix-list round 2, F-25): the apply single-flight guard. Acquiring
// the flag (in OnApplyToOriginalWindow) grays [ApplyToOriginalWindow] and
// [EditRetranslate] for the whole handler and re-enables them on EVERY exit
// arm, so a double-clicked Apply can never run a second PasteAndRestore and
// the bounded activation pump can never re-enter this dialog through its own
// command messages while the wait is in flight.
struct ApplySingleFlight {
    EditorState& st;
    ~ApplySingleFlight() {
        st.apply_in_progress.store(false, std::memory_order_release);
        if (st.hwnd != nullptr && ::IsWindow(st.hwnd)) {
            ::EnableWindow(::GetDlgItem(st.hwnd, kIdApply), TRUE);
            ::EnableWindow(::GetDlgItem(st.hwnd, kIdRetranslate), TRUE);
        }
    }
};

// [ApplyToOriginalWindow]: paste the edited target text into the window the
// translation originally came from. The paste uses the win32_input
// PasteAndRestore primitive EXACTLY (clipboard backup -> SetClipboardText +
// Ctrl+V -> restore), after activating the original window and proving the
// activation landed — PasteAndRestore's own H1 foreground guard stays the
// fail-closed backstop (any focus drift aborts with no clipboard write).
void OnApplyToOriginalWindow(EditorState& st) {
    // V10F-R2 (F-25): single-flight. The pre-fix handler had no reentrancy
    // guard and its activation pump dispatches queued messages, so a
    // double-clicked [Apply] queued a second BN_CLICKED that the pump
    // dispatched reentrantly — two PasteAndRestore calls, the translated text
    // pasted twice into the target.
    if (st.apply_in_progress.exchange(true, std::memory_order_acquire)) {
        DIAG_F("UI/TranslationEditor/008: apply re-requested while one is in "
               "flight; ignored (single-flight)\n");
        return;
    }
    ApplySingleFlight single_flight{st};
    if (st.hwnd != nullptr && ::IsWindow(st.hwnd)) {
        ::EnableWindow(::GetDlgItem(st.hwnd, kIdApply), FALSE);
        ::EnableWindow(::GetDlgItem(st.hwnd, kIdRetranslate), FALSE);
    }

    const std::wstring text = GetPaneText(st.target_pane);
    if (text.empty()) return;
    HWND target = st.entry.target_hwnd;
    if (target == nullptr || !::IsWindow(target)) {
        DIAG_F("UI/TranslationEditor/003: apply requested with no live target hwnd\n");
        return;
    }
    // V10F-R1 F-09 + V10F-R2 (fix-list round 2, F-27): HWND recycling guard.
    // The handle was captured at Push time; the OS recycles HWND values, so a
    // live-looking handle may now name a DIFFERENT process's window. The PID
    // recorded at Push time must still own the handle, otherwise the paste
    // would inject into a stranger's window. F-27 hardens the F-09 gate twice:
    //   * REFUSE when the recorded PID is 0 for a non-null target_hwnd — the
    //     ring records the owner centrally at Push for every entry this build
    //     writes (it is in-memory only), so 0 means the handle was already
    //     dead/recycled at capture time; the F-09 skip-on-0 left exactly those
    //     recycled handles guarded by the liveness probe alone;
    //   * RE-VERIFY the owner after the activation wait, immediately before
    //     the injection — the bounded pump below runs this thread's queue for
    //     up to 300 ms, and the target can close or be recycled inside that
    //     window.
    if (st.entry.target_pid == 0) {
        DIAG_F("UI/TranslationEditor/010: target pid unrecordable (dead/recycled "
               "hwnd at capture); paste aborted\n");
        return;
    }
    {
        DWORD live_pid = 0;
        ::GetWindowThreadProcessId(target, &live_pid);
        if (live_pid != st.entry.target_pid) {
            DIAG_F("UI/TranslationEditor/007: target hwnd recycled (pid %lu != %lu); paste aborted\n",
                   static_cast<unsigned long>(live_pid),
                   static_cast<unsigned long>(st.entry.target_pid));
            return;
        }
    }
    ::SetForegroundWindow(target); // async: the bounded wait below absorbs it
    // V10F-R1 F-09: bounded message-peek wait (<= 300 ms total) instead of the
    // old 30-iteration 10 ms Sleep busy-wait. The wait runs ON THE GUI THREAD, so it must
    // PUMP: a sleeping GUI thread cannot dispatch the very activation/FOCUS
    // messages that make the target foreground, and cannot service a
    // kMsgResultReady retranslate delivery that races this loop. PeekMessageW
    // with PM_REMOVE keeps both flowing; the deadline bounds the total stay.
    // V10F-R2 (fix-list round 2): F-24 re-posts an observed WM_QUIT exactly
    // once and ABORTS instead of `continue`-ing on it (the re-posted quit was
    // re-peeked forever, so one WM_QUIT spun this drain at 100% with the
    // deadline unreachable), and F-25 stops the pump from re-entering THIS
    // dialog mid-wait (the editor-owned-message filter below).
    bool foreground_ok = false;
    bool quit_seen = false;
    const ULONGLONG wait_deadline = ::GetTickCount64() + 300;
    for (;;) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.hwnd == nullptr && msg.message == WM_QUIT) {
                // V10F-R2 (F-24): re-post ONCE (the app's quit must survive)
                // and break out of the pump — the handler returns without
                // pasting and the re-posted quit reaches the main loop.
                ::PostQuitMessage(static_cast<int>(msg.wParam));
                quit_seen = true;
                break;
            }
            // V10F-R2 (F-25): the wait must not re-enter THIS dialog. Skip —
            // and re-queue behind the wait — every message addressed to the
            // editor or one of its children, EXCEPT kMsgResultReady (the async
            // retranslate delivery the pump contract above exists to
            // service). Thread messages (WM_QUIT) and the target window's
            // activation/FOCUS messages still flow normally.
            if (msg.hwnd != nullptr &&
                (msg.hwnd == st.hwnd || ::IsChild(st.hwnd, msg.hwnd)) &&
                msg.message != kMsgResultReady) {
                ::PostMessageW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
                break; // one re-queue per outer tick; the deadline bounds the stay
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (quit_seen) break;
        if (IsSameWindowForInjection(target, ::GetForegroundWindow())) {
            foreground_ok = true;
            break;
        }
        if (::GetTickCount64() >= wait_deadline) break;
        // V10F-R1 F-09 integrator note: the SDK 10.0.26100 header spells the
        // flag MWMO_INPUTAVAILABLE (0x0004; MWMO_INPUTSOK is documentation-only
        // and is not declared by the current winuser.h).
        // V10F-R2 (F-25): a re-queued editor message makes this return
        // immediately instead of waiting the 20 ms; the inner drain re-queues
        // at most one editor message per outer tick, so the stay stays
        // bounded by the same deadline (with no editor traffic the wait is
        // unchanged).
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (quit_seen) {
        DIAG_F("UI/TranslationEditor/009: WM_QUIT observed during the activation "
               "wait; apply aborted (quit re-posted for the main loop)\n");
        return;
    }
    if (!foreground_ok) {
        DIAG_F("UI/TranslationEditor/004: target never took foreground; paste aborted\n");
        return;
    }
    // V10F-R2 (F-27): the owner is re-verified IMMEDIATELY before the
    // injection — the activation wait above ran the GUI thread's queue for up
    // to 300 ms, and the target may have closed or been recycled inside that
    // window.
    {
        DWORD live_pid = 0;
        ::GetWindowThreadProcessId(target, &live_pid);
        if (live_pid != st.entry.target_pid) {
            DIAG_F("UI/TranslationEditor/011: target hwnd recycled during the "
                   "activation wait; paste aborted\n");
            return;
        }
    }
    ClipboardBackup backup{};
    const bool backed_up = BackupClipboard(backup);
    bool restored = false;
    // V10F-R1 F-09: scope-exit restorer fallback. PasteAndRestore's contract
    // (win32_input.hpp F7) leaves `restored` false when its own restore did not
    // confirm; the guard below then puts the user's ORIGINAL clipboard content
    // back on ANY exit arm (unconfirmed restore, H1 abort, exception), so the
    // backup collected here can never be silently discarded. Disarmed exactly
    // like the worker's RestorerGuard: only a CONFIRMED restore turns it off.
    struct RestorerFallback {
        const ClipboardBackup& backup;
        bool backed_up;
        const bool* confirmed;
        ~RestorerFallback() {
            if (ClipboardRestoreArmable(backed_up, backup) &&
                !(confirmed != nullptr && *confirmed)) {
                RestoreClipboard(backup);
            }
        }
    } restorer_fallback{backup, backed_up, &restored};
    const bool pasted = PasteAndRestore(text, backup, target, &restored);
    // Shape-only: result flags + length, never the content.
    DIAG_F("UI/TranslationEditor/005: apply pasted=%d backup=%d restored=%d len=%zu\n",
           pasted ? 1 : 0, backed_up ? 1 : 0, restored ? 1 : 0, text.size());
}

INT_PTR CALLBACK EditorDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgResultReady) {
        EditorState* st = reinterpret_cast<EditorState*>(
            ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
        if (st != nullptr) {
            std::optional<std::wstring> result;
            {
                std::lock_guard<std::mutex> lk(st->result_m);
                result.swap(st->pending_target);
            }
            if (result.has_value()) {
                SetPaneText(st->target_pane, *result);
            }
        }
        return TRUE;
    }

    EditorState* st = reinterpret_cast<EditorState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<EditorState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->hwnd = dlg; // assigned here: this proc runs inside CreateDialogIndirectParamW
        st->gui_thread_id = ::GetCurrentThreadId();

        // RichEdit50W needs msftedit.dll; fall back to a multiline EDIT when
        // the library is missing. The library stays loaded for the process
        // lifetime (system DLL; a FreeLibrary could strand the class atom of
        // a live pane).
        st->richedit_lib = ::LoadLibraryW(L"msftedit.dll");
        st->richedit_ok = st->richedit_lib != nullptr;
        const wchar_t* pane_class = st->richedit_ok ? L"RichEdit50W" : L"EDIT";
        if (!st->richedit_ok) {
            DIAG_F("UI/TranslationEditor/002: msftedit.dll unavailable; EDIT fallback\n");
        }

        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        const UINT dpi = ui::WindowDpi(dlg);
        const EditorLayoutPlan plan =
            PlanEditorLayout(kEditorClientWidthDip, kEditorClientHeightDip);

        // Size the window so the CLIENT matches the DIP plan exactly, then
        // center it on its monitor's work area (the template's DS_CENTER used
        // the template size, which the resize below would have drifted).
        RECT client_px = ScalePlanRect(
            RECT{0, 0, kEditorClientWidthDip, kEditorClientHeightDip}, dpi);
        RECT window_rc = client_px;
        const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(dlg, GWL_STYLE));
        const DWORD exstyle = static_cast<DWORD>(::GetWindowLongPtrW(dlg, GWL_EXSTYLE));
        ::AdjustWindowRectEx(&window_rc, style, FALSE, exstyle);
        const int win_w = window_rc.right - window_rc.left;
        const int win_h = window_rc.bottom - window_rc.top;
        int win_x = 0, win_y = 0;
        HMONITOR mon = ::MonitorFromWindow(dlg, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(mon, &mi)) {
            win_x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - win_w) / 2;
            win_y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - win_h) / 2;
        }
        ::SetWindowPos(dlg, nullptr, win_x, win_y, win_w, win_h,
                       SWP_NOZORDER | SWP_NOACTIVATE);

        st->source_pane = CreatePane(dlg, inst, pane_class, ScalePlanRect(plan.source_pane, dpi),
                                     kIdSourcePane, /*read_only=*/false);
        st->target_pane = CreatePane(dlg, inst, pane_class, ScalePlanRect(plan.target_pane, dpi),
                                     kIdTargetPane, /*read_only=*/false);
        st->back_pane = CreatePane(dlg, inst, pane_class, ScalePlanRect(plan.back_pane, dpi),
                                   kIdBackPane, /*read_only=*/true);

        const std::wstring retranslate_label = I18n::Get(StringId::EditRetranslate);
        const std::wstring apply_label = I18n::Get(StringId::ApplyToOriginalWindow);
        const std::wstring close_label = I18n::Get(StringId::GgufManagerClose);
        HWND buttons[6] = {
            CreateButton(dlg, inst, ScalePlanRect(plan.retranslate_button, dpi),
                         kIdRetranslate, retranslate_label),
            CreateButton(dlg, inst, ScalePlanRect(plan.apply_button, dpi),
                         kIdApply, apply_label),
            CreateButton(dlg, inst, ScalePlanRect(plan.close_button, dpi),
                         IDCANCEL, close_label),
            st->source_pane, st->target_pane, st->back_pane,
        };
        for (HWND ctl : buttons) {
            ApplyDefaultFont(ctl); // manual children don't inherit the dialog font
        }
        if (st->source_pane == nullptr || st->target_pane == nullptr ||
            st->back_pane == nullptr) {
            DIAG_F("UI/TranslationEditor/006: a text pane failed to create\n");
        }

        FillPanes(*st);
        return TRUE; // default focus is fine
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdRetranslate:
            if (st != nullptr) OnRetranslate(*st);
            return TRUE;
        case kIdApply:
            if (st != nullptr) OnApplyToOriginalWindow(*st);
            return TRUE;
        case IDCANCEL:
            ::DestroyWindow(dlg);
            return TRUE;
        default:
            return FALSE;
        }
    case WM_CLOSE:
        ::DestroyWindow(dlg);
        return TRUE;
    case WM_DESTROY:
        if (st != nullptr) {
            st->hwnd = nullptr;
            st->source_pane = nullptr;
            st->target_pane = nullptr;
            st->back_pane = nullptr;
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

void Open(const translation_history::Entry& entry, RetranslateFn retranslate) {
    EditorState& st = State();
    st.entry = entry;
    st.retranslate = std::move(retranslate);
    {
        std::lock_guard<std::mutex> lk(st.result_m);
        st.pending_target.reset(); // a stale async result must not fill a refreshed window
    }

    // Single instance: reopening refreshes the panes instead of stacking a
    // second editor over the first.
    if (st.hwnd != nullptr && ::IsWindow(st.hwnd)) {
        FillPanes(st);
        ::ShowWindow(st.hwnd, SW_SHOW);
        ::SetForegroundWindow(st.hwnd);
        return;
    }

    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    TemplateBuilder tb;
    tb.Begin(I18n::Get(StringId::TranslationEditorTitle), kTemplateWidthDlu,
             kTemplateHeightDlu, /*itemCount=*/0); // panes/buttons are created in WM_INITDIALOG
    HWND dlg = ::CreateDialogIndirectParamW(inst, tb.Get(), /*hWndParent=*/nullptr,
                                            EditorDialogProc,
                                            reinterpret_cast<LPARAM>(&st));
    if (dlg == nullptr) {
        DIAG_F("UI/TranslationEditor/001: CreateDialogIndirectParamW failed (GLE %lu)\n",
               ::GetLastError());
    }
    // st.hwnd is assigned inside EditorDialogProc's WM_INITDIALOG.
}

void ShutdownDestroy() {
    // V10F-R1 F-09: the modeless editor is created WITHOUT a parent/owner, so
    // nothing in the shutdown sweep knew it existed; a late kMsgResultReady
    // post could then land on a destroyed... or worse, a recycled... dialog.
    // Destroying it here (GUI thread, registered in main.cpp's shutdown sweep
    // next to about_window.Destroy()) retires the hwnd before the teardown
    // continues. WM_DESTROY clears the state's hwnd/pane pointers, so a queued
    // result delivery after this point is an IsWindow-guarded no-op.
    EditorState& st = State();
    if (st.hwnd != nullptr && ::IsWindow(st.hwnd)) {
        ::DestroyWindow(st.hwnd);
    }
}

} // namespace translation_editor_window
} // namespace emebalachat
