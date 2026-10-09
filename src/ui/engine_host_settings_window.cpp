#include "engine_host_settings_window.hpp"

#include "../diag_logger.hpp" // REQ-047 D3 §C.4-style control-creation verification logging
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <string>

namespace emebalachat {

namespace {

// REQ-060: control IDs. Own band next to the manager (4100s) / HF-add (4300s)
// dialog-local ids; these are template-local ids, unrelated to the tray menu
// ID bands.
enum : WORD {
    IDC_EHS_TITLE = 4400,
    IDC_EHS_MODE_COMBO,
    IDC_EHS_DESC,
};

struct EngineHostDialogState {
    const EngineHostConfig* current = nullptr; // in: persisted block (pre-select)
    EngineHostConfig* out = nullptr;           // out: receives the edited block on OK
    bool saved = false;
};

// REQ-060: combo index -> warmup mapping. Index 0 is the recommended "load on
// use" leg (warmup stays false, the engine default); index 1 is the preload
// leg. Keeping the indices in one place makes both the init pre-select and the
// OK read-back impossible to desynchronize.
int WarmupToComboIndex(bool warmup) { return warmup ? 1 : 0; }
bool ComboIndexToWarmup(int index) { return index == 1; }

// REQ-060: the description line under the combo follows the selection.
void SyncModeDescription(HWND dlg) {
    HWND combo = ::GetDlgItem(dlg, IDC_EHS_MODE_COMBO);
    HWND desc = ::GetDlgItem(dlg, IDC_EHS_DESC);
    if (!combo || !desc) return;
    const int sel = static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    const StringId id = ComboIndexToWarmup(sel)
                            ? StringId::LoadModeWarmupDesc
                            : StringId::LoadModeOnUseDesc;
    ::SetWindowTextW(desc, I18n::Get(id).c_str());
}

INT_PTR CALLBACK EngineHostSettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EngineHostDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<EngineHostDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // REQ-047 D3 §C.4 (control-creation verification): the template itself
        // is proven-good by the unit byte-walk, but a missing combo would make
        // the dialog a dead form — surface a distinct log line per control.
        if (!::GetDlgItem(dlg, IDC_EHS_MODE_COMBO)) {
            DIAG_F("UI/EngineHostSettings/001: IDC_EHS_MODE_COMBO control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_EHS_DESC)) {
            DIAG_F("UI/EngineHostSettings/002: IDC_EHS_DESC control missing after dialog init\n");
        }
        // REQ-060: fixed option pair, localized captions. CBS_DROPDOWNLIST
        // means the user picks exactly one of the two legs — no free text.
        if (HWND combo = ::GetDlgItem(dlg, IDC_EHS_MODE_COMBO)) {
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::LoadModeOnUse).c_str()));
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::LoadModeWarmup).c_str()));
            ::SendMessageW(combo, CB_SETCURSEL,
                           static_cast<WPARAM>(
                               WarmupToComboIndex(st->current->warmup)), 0);
        }
        SyncModeDescription(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        // REQ-060: the description static mirrors the selection the moment it
        // changes (CBN_SELCHANGE on the CBS_DROPDOWNLIST combo).
        if (LOWORD(wp) == IDC_EHS_MODE_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
            SyncModeDescription(dlg);
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDOK: {
            // REQ-060: `out` = `current` with ONLY warmup replaced — every
            // other engine_host field (enabled / spawn / idle_exit_ms /
            // worker_idle_ms) is carried through untouched. Persistence is
            // the caller's job.
            int sel = 0;
            if (HWND combo = ::GetDlgItem(dlg, IDC_EHS_MODE_COMBO)) {
                sel = static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
            }
            if (sel < 0) sel = 0; // no selection (defensive): keep the default leg
            *st->out = *st->current;
            st->out->warmup = ComboIndexToWarmup(sel);
            st->saved = true;
            ::EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

} // namespace

// REQ-060: production item-set emission, split out of
// ShowEngineHostSettingsDialog so the unit suite byte-walks the exact bytes the
// dialog hands to DialogBoxIndirectParamW. itemCount must match the AddItem
// calls below: title label + mode combo + description static + OK + Cancel
// = 5.
void BuildEngineHostSettingsTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    // REQ-060: CBS_DROPDOWNLIST (0x0003) — a fixed two-option list the user
    // picks from; unlike the OpenAI model combo there is no free-text leg.
    const DWORD COMBO = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                        CBS_DROPDOWNLIST;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD COMBO_CLS  = 0x0085;  // "COMBOBOX"

    tb.AddItem(LBL, 8, 6, 224, 9, IDC_EHS_TITLE, STATIC_CLS,
               I18n::Get(StringId::EngineLoadSettingsTitle));
    tb.AddItem(COMBO, 8, 20, 224, 64, IDC_EHS_MODE_COMBO, COMBO_CLS, L"");
    // REQ-060: description static — empty in the template, filled per
    // selection at WM_INITDIALOG / CBN_SELCHANGE (SyncModeDescription). Two
    // text lines tall so the longer locale captions never clip.
    tb.AddItem(LBL, 8, 40, 224, 22, IDC_EHS_DESC, STATIC_CLS, L"");
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 104, 72, 60, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 172, 72, 60, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

bool ShowEngineHostSettingsDialog(HWND parent, const EngineHostConfig& current,
                                  EngineHostConfig& out) {
    EngineHostDialogState st;
    st.current = &current;
    st.out = &out;

    const std::wstring title = I18n::Get(StringId::EngineLoadSettingsTitle);
    TemplateBuilder tb;
    tb.Begin(title, 240, 90, /*itemCount=*/5);
    BuildEngineHostSettingsTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, EngineHostSettingsProc,
        reinterpret_cast<LPARAM>(&st));
    return st.saved && rc == IDOK;
}

} // namespace emebalachat
