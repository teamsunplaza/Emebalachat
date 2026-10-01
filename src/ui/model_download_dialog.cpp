#include "model_download_dialog.hpp"

#include "../i18n.hpp"
#include "../model_downloader.hpp"
#include "../unicode_utils.hpp"
#include "../update_checker.hpp"
#include "dwrite_helpers.hpp" // IsPointInRect not needed; included for house parity
#include "openai_settings_window.hpp" // TemplateBuilder (proven DLGTEMPLATE contract)

#include <atomic>
#include <string>
#include <thread>

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace emebalachat {

namespace {

constexpr UINT kMdMsgProgress = WM_APP + 0x20; // wParam = percent, lParam = received bytes
constexpr UINT kMdMsgDone = WM_APP + 0x21;     // wParam = FetchOutcome
constexpr UINT kMdMsgPhase = WM_APP + 0x22;    // wParam = StringId (phase label)

constexpr WORD IDC_MD_BODY = 1501;
constexpr WORD IDC_MD_PRIVACY = 1502;
constexpr WORD IDC_MD_PROGRESS = 1503;
constexpr WORD IDC_MD_STATUS = 1504;
constexpr WORD IDC_MD_DOWNLOAD = 1505;
constexpr WORD IDC_MD_LATER = 1506;
constexpr WORD IDC_MD_RETRY = 1507;
constexpr WORD IDC_MD_CLOSE = 1508;

enum class MdPhase { Consent, Progress, Result };

struct MdDialogState {
    std::atomic<bool> cancel{false};
    std::thread worker;
    bool worker_running = false;
    MdPhase phase = MdPhase::Consent;
    modeldownloader::FetchOutcome outcome = modeldownloader::FetchOutcome::Failed;
    bool hash_failed = false; // drives the red inline state
};

void MdApplyVisibility(HWND dlg, MdPhase phase, bool no_space, bool success) {
    const int SW = SW_SHOW;
    const int HD = SW_HIDE;
    // Body + status are always visible; the rest toggles by phase/state.
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_BODY), SW);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_STATUS), phase == MdPhase::Progress ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_PRIVACY),
                 phase == MdPhase::Consent && !no_space ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_PROGRESS),
                 phase == MdPhase::Progress ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_DOWNLOAD),
                 phase == MdPhase::Consent && !no_space ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_LATER),
                 phase == MdPhase::Consent || (phase == MdPhase::Result && !success) ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_RETRY),
                 phase == MdPhase::Result && !success ? SW : HD);
    ::ShowWindow(::GetDlgItem(dlg, IDC_MD_CLOSE),
                 phase == MdPhase::Result && success ? SW : HD);
}

void MdSetBody(HWND dlg, const std::wstring& text) {
    ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_BODY), text.c_str());
}

std::wstring MdReplace(std::wstring text, std::wstring_view token, std::wstring_view value) {
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::wstring::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
    return text;
}

INT_PTR CALLBACK ModelDownloadProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<MdDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            st = reinterpret_cast<MdDialogState*>(lParam);
            ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            const std::wstring size_text =
                updatelogic::FormatSizeMb(modeldownloader::kModelExpectedBytes);
            MdSetBody(dlg, MdReplace(I18n::Get(StringId::ModelDlConsentBody),
                                     L"{s}", size_text));
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_PRIVACY),
                             I18n::Get(StringId::ModelDlPrivacy).c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_DOWNLOAD),
                             I18n::Get(StringId::ModelDlDownload).c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_LATER),
                             I18n::Get(StringId::UpdateDlgLater).c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_RETRY),
                             I18n::Get(StringId::UpdateErrRetry).c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_CLOSE),
                             I18n::Get(StringId::GgufManagerClose).c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_STATUS), L"");
            // Disk pre-flight at open: no space -> the consent shows the
            // disk line with only [Later].
            const auto dir = engine_host_registry::DefaultModelsDir();
            bool no_space = false;
            if (!dir.empty()) {
                no_space = modeldownloader::FreeBytesOnVolume(dir) <
                           modeldownloader::kModelExpectedBytes + modeldownloader::kDiskReserveBytes;
            }
            if (no_space) {
                MdSetBody(dlg, MdReplace(I18n::Get(StringId::ModelDlDiskSpace),
                                         L"{s}",
                                         updatelogic::FormatSizeMb(
                                             modeldownloader::kModelExpectedBytes +
                                             modeldownloader::kDiskReserveBytes)));
            }
            MdApplyVisibility(dlg, MdPhase::Consent, no_space, false);
            ::SetFocus(::GetDlgItem(dlg, no_space ? IDC_MD_LATER : IDC_MD_DOWNLOAD));
            return FALSE;
        }

        case kMdMsgProgress: {
            const int percent = static_cast<int>(wParam);
            const std::uint64_t received = static_cast<std::uint64_t>(lParam);
            ::SendMessageW(::GetDlgItem(dlg, IDC_MD_PROGRESS), PBM_SETPOS, percent, 0);
            wchar_t buf[96] = {};
            const std::wstring meta = updatelogic::FormatProgressMb(
                received, modeldownloader::kModelExpectedBytes);
            std::swprintf(buf, 96, L"%d%%   %ls", percent, meta.c_str());
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_STATUS), buf);
            return TRUE;
        }

        case kMdMsgPhase: {
            // Hash/move phase label (percent frozen at 100 during the hash).
            ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_BODY),
                             I18n::Get(static_cast<StringId>(wParam)).c_str());
            return TRUE;
        }

        case kMdMsgDone: {
            st->outcome = static_cast<modeldownloader::FetchOutcome>(wParam);
            const bool success = st->outcome == modeldownloader::FetchOutcome::Ok;
            st->hash_failed = st->outcome == modeldownloader::FetchOutcome::HashMismatch;
            st->phase = MdPhase::Result;
            if (success) {
                MdSetBody(dlg, I18n::Get(StringId::ModelDlDone));
                ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_STATUS), L"");
            } else if (st->hash_failed) {
                MdSetBody(dlg, I18n::Get(StringId::UpdateErrHash)); // the only red state
                ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_STATUS), L"");
            } else if (st->outcome == modeldownloader::FetchOutcome::NoDiskSpace) {
                MdSetBody(dlg, MdReplace(I18n::Get(StringId::ModelDlDiskSpace), L"{s}",
                                         updatelogic::FormatSizeMb(
                                             modeldownloader::kModelExpectedBytes +
                                             modeldownloader::kDiskReserveBytes)));
            } else if (st->outcome == modeldownloader::FetchOutcome::Cancelled) {
                ::EndDialog(dlg, IDCANCEL); // Later-equivalent: quiet close
                return TRUE;
            } else {
                MdSetBody(dlg, I18n::Get(StringId::ModelDlFailed));
                ::SetWindowTextW(::GetDlgItem(dlg, IDC_MD_STATUS), L"");
            }
            MdApplyVisibility(dlg, MdPhase::Result, false, success);
            ::InvalidateRect(dlg, nullptr, TRUE);
            return TRUE;
        }

        case WM_COMMAND: {
            const WORD id = LOWORD(wParam);
            if (id == IDC_MD_LATER) {
                st->cancel.store(true);
                ::EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            if (id == IDC_MD_CLOSE) {
                ::EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDC_MD_RETRY) {
                // Re-run the flow: back to progress with the same worker shape.
                ::PostMessageW(dlg, WM_COMMAND,
                               MAKEWPARAM(IDC_MD_DOWNLOAD, BN_CLICKED), 0);
                return TRUE;
            }
            if (id == IDC_MD_DOWNLOAD) {
                // D-01/N1 (delta re-review): the prior fetch has already
                // exited (its kMdMsgDone posted — that is what put us in the
                // Result phase), so its thread object is joinable. Assigning
                // a new std::thread over it would call std::terminate. Join
                // BEFORE re-create — the one owner discipline for this
                // dialog (spawn here, join here on Retry and in
                // OfferModelDownload on every exit path; never detach).
                if (st->worker.joinable()) {
                    st->worker.join();
                }
                st->phase = MdPhase::Progress;
                st->hash_failed = false;
                MdSetBody(dlg, I18n::Get(StringId::UpdateDlProgress));
                MdApplyVisibility(dlg, MdPhase::Progress, false, false);
                ::SendMessageW(::GetDlgItem(dlg, IDC_MD_PROGRESS), PBM_SETRANGE, 0,
                               MAKELPARAM(0, 100));
                st->cancel.store(false);
                HWND dlg_copy = dlg;
                st->worker = std::thread([st, dlg_copy]() {
                    const modeldownloader::FetchOutcome out =
                        modeldownloader::FetchAndInstallModel(
                            [dlg_copy](std::uint64_t received, std::uint64_t total) {
                                const int percent = total > 0
                                    ? static_cast<int>(received * 100 / total)
                                    : 0;
                                ::PostMessageW(dlg_copy, kMdMsgProgress, percent,
                                               static_cast<LPARAM>(received));
                            },
                            st->cancel,
                            [dlg_copy](modeldownloader::FetchPhase phase) {
                                const StringId id =
                                    phase == modeldownloader::FetchPhase::Verifying
                                        ? StringId::ModelDlVerifying
                                        : StringId::ModelDlPlacing;
                                ::PostMessageW(dlg_copy, kMdMsgPhase,
                                               static_cast<WPARAM>(id), 0);
                            });
                    ::PostMessageW(dlg_copy, kMdMsgDone,
                                   static_cast<WPARAM>(out), 0);
                });
                st->worker_running = true;
                return TRUE;
            }
            return FALSE;
        }

        case WM_CTLCOLORSTATIC: {
            const HDC hdc = reinterpret_cast<HDC>(wParam);
            const HWND ctl = reinterpret_cast<HWND>(lParam);
            if (ctl == ::GetDlgItem(dlg, IDC_MD_BODY) && st && st->hash_failed) {
                ::SetTextColor(hdc, RGB(0xF2, 0xC1, 0xBD)); // danger-ink (the only red)
                ::SetBkMode(hdc, TRANSPARENT);
            }
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_BTNFACE));
        }

        case WM_CLOSE: {
            st->cancel.store(true);
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

} // namespace

bool OfferModelDownload(HWND owner) {
    MdDialogState st;
    const std::wstring title = I18n::Get(StringId::ModelDlCaption);
    TemplateBuilder tb;
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;
    const WORD BTN_CLS = 0x0080;
    tb.Begin(title, 240, 92, /*itemCount=*/8);
    tb.AddItem(LBL, 7, 7, 226, 38, IDC_MD_BODY, STATIC_CLS, L"");
    tb.AddItem(LBL, 7, 48, 226, 16, IDC_MD_PRIVACY, STATIC_CLS, L"");
    tb.AddItem(WS_CHILD | PBS_SMOOTH, 7, 26, 226, 12, IDC_MD_PROGRESS,
               0x0083 /*msctls_progress32*/, L"");
    tb.AddItem(LBL, 7, 42, 226, 12, IDC_MD_STATUS, STATIC_CLS, L"");
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 178, 68, 55, 14, IDC_MD_DOWNLOAD, BTN_CLS, L"");
    tb.AddItem(BTN, 7, 68, 55, 14, IDC_MD_LATER, BTN_CLS, L"");
    tb.AddItem(BTN, 178, 68, 55, 14, IDC_MD_RETRY, BTN_CLS, L"");
    tb.AddItem(BTN, 178, 68, 55, 14, IDC_MD_CLOSE, BTN_CLS, L"");
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
    ::InitCommonControlsEx(&icc);
    ::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), tb.Get(), owner,
                              ModelDownloadProc, reinterpret_cast<LPARAM>(&st));
    if (st.worker_running && st.worker.joinable()) {
        st.worker.join(); // HF house contract: join on EVERY exit path
    }
    return modeldownloader::ModelFilePresent();
}

} // namespace emebalachat
