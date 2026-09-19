#include "openai_settings_window.hpp"

#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace emebalachat {

namespace {

// Control IDs.
enum : WORD {
    IDC_STATIC_BASE = 100,
    IDC_BASE_URL,
    IDC_STATIC_KEY,
    IDC_API_KEY,
    IDC_STATIC_MODEL,
    IDC_MODEL_COMBO,
    IDC_FETCH_BTN,
    IDC_MASKED_LABEL,
};

struct OpenAiDialogState {
    OpenAiConfig* cfg;   // in/out; key DPAPI-protected on save
    bool saved = false;
};

std::wstring GetCtrlText(HWND dlg, int id) {
    HWND c = ::GetDlgItem(dlg, id);
    if (!c) return {};
    int len = ::GetWindowTextLengthW(c);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(c, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

void SetCtrlText(HWND dlg, int id, std::wstring_view text) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        ::SetWindowTextW(c, std::wstring(text).c_str());
    }
}

bool IsMaskedPlaceholder(std::string_view keyUtf8) {
    return keyUtf8.size() > 3 && keyUtf8.compare(keyUtf8.size() - 3, 3, "***") == 0;
}

INT_PTR CALLBACK OpenAiSettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<OpenAiDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<OpenAiDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        SetCtrlText(dlg, IDC_BASE_URL, ToUtf16(st->cfg->base_url));
        SetCtrlText(dlg, IDC_MODEL_COMBO, ToUtf16(st->cfg->model));
        if (!st->cfg->api_key_dpapi.empty()) {
            std::string key;
            if (UnprotectOpenAiApiKey(st->cfg->api_key_dpapi, key)) {
                SetCtrlText(dlg, IDC_API_KEY, ToUtf16(MaskOpenAiApiKey(key)));
                SecureZeroMemory(key.data(), key.size());
            }
        }
        ::SetDlgItemTextW(dlg, IDC_MASKED_LABEL,
                          I18n::Get(StringId::OpenAiKeyMasked).c_str());
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FETCH_BTN: {
            OpenAiConfig probe;
            probe.base_url = ToUtf8(GetCtrlText(dlg, IDC_BASE_URL));
            std::string keyUtf8 = ToUtf8(GetCtrlText(dlg, IDC_API_KEY));
            if (IsMaskedPlaceholder(keyUtf8)) {
                probe.api_key_dpapi = st->cfg->api_key_dpapi;
                probe.api_key_sha256 = st->cfg->api_key_sha256;
            } else if (!keyUtf8.empty()) {
                ProtectOpenAiApiKey(keyUtf8, probe.api_key_dpapi);
                OpenAiSha256Hex(keyUtf8, probe.api_key_sha256);
            }
            SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
            const std::vector<std::string> models =
                OpenAiCompatibleClient::ListModels(probe);
            if (HWND combo = ::GetDlgItem(dlg, IDC_MODEL_COMBO)) {
                ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
                for (const auto& m : models) {
                    ::SendMessageW(combo, CB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(ToUtf16(m).c_str()));
                }
                if (!models.empty()) ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
            }
            if (models.empty()) {
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDOK: {
            OpenAiConfig out;
            out.base_url = ToUtf8(GetCtrlText(dlg, IDC_BASE_URL));
            out.model = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
            std::string keyUtf8 = ToUtf8(GetCtrlText(dlg, IDC_API_KEY));

            const OpenAiUrlSecurity sec = ClassifyOpenAiBaseUrl(out.base_url);
            if (sec == OpenAiUrlSecurity::Invalid) {
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiInvalidBaseUrl).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONWARNING);
                SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                return TRUE;
            }
            if (sec == OpenAiUrlSecurity::Http && !st->cfg->http_consent_given) {
                const int rc = ::MessageBoxW(
                    dlg, I18n::Get(StringId::OpenAiHttpWarningBody).c_str(),
                    I18n::Get(StringId::OpenAiHttpWarningTitle).c_str(),
                    MB_YESNO | MB_ICONWARNING);
                if (rc != IDYES) {
                    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                    return TRUE;
                }
                out.http_consent_given = true;
            } else {
                out.http_consent_given = st->cfg->http_consent_given;
            }

            if (IsMaskedPlaceholder(keyUtf8)) {
                out.api_key_dpapi = st->cfg->api_key_dpapi;
                out.api_key_sha256 = st->cfg->api_key_sha256;
            } else if (!keyUtf8.empty()) {
                if (!ProtectOpenAiApiKey(keyUtf8, out.api_key_dpapi)) {
                    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                    ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                                  I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                                  MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                OpenAiSha256Hex(keyUtf8, out.api_key_sha256);
            }
            SecureZeroMemory(keyUtf8.data(), keyUtf8.size());

            *st->cfg = std::move(out);
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

// ---- In-memory dialog template builder ----
// Serializes a DLGTEMPLATE + DLGITEMTEMPLATEs into a byte buffer with correct
// 4-byte alignment (per the Win32 dialog-template contract).
class TemplateBuilder {
public:
    void Begin(std::wstring_view title, short w, short h, WORD itemCount) {
        // Pad to a 4-byte boundary before the (DWORD-aligned) DLGTEMPLATE.
        while (buf_.size() % 4 != 0) buf_.push_back(0);
        dtpl_ = buf_.size();
        EmitDword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME);
        EmitDword(0);                       // dwExtendedStyle
        EmitWord(0); EmitWord(0);           // x, y
        EmitWord(w); EmitWord(h);           // cx, cy
        EmitWord(itemCount);                // cdit
        EmitWord(0);                        // menu (none)
        EmitWord(0);                        // windowClass (none)
        EmitStr(title);
        EmitWord(9);                        // pointsize
        EmitStr(L"Segoe UI");
    }
    void AddItem(DWORD style, short x, short y, short cx, short cy,
                 WORD id, WORD clsAtom, std::wstring_view text) {
        // Each DLGITEMTEMPLATE must be 4-byte aligned relative to the start
        // of the DLGTEMPLATE.
        while ((buf_.size() - dtpl_) % 4 != 0) buf_.push_back(0);
        EmitDword(style);
        EmitDword(0);                       // dwExtendedStyle
        EmitWord(x); EmitWord(y); EmitWord(cx); EmitWord(cy);
        EmitWord(id); EmitWord(0);          // id (+ WORD padding -> DWORD align)
        EmitWord(clsAtom);
        EmitStr(text);
        EmitWord(0);                        // extraData count = 0
    }
    const DLGTEMPLATE* Get() const {
        return reinterpret_cast<const DLGTEMPLATE*>(buf_.data() + dtpl_);
    }
private:
    void EmitWord(WORD w) {
        buf_.push_back(static_cast<BYTE>(w & 0xFF));
        buf_.push_back(static_cast<BYTE>((w >> 8) & 0xFF));
    }
    void EmitDword(DWORD d) { EmitWord(LOWORD(d)); EmitWord(HIWORD(d)); }
    void EmitStr(std::wstring_view s) {
        for (wchar_t ch : s) EmitWord(static_cast<WORD>(ch));
        EmitWord(0);
    }
    std::vector<BYTE> buf_;
    size_t dtpl_ = 0;
};

} // namespace

bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg) {
    OpenAiDialogState st{&cfg, false};

    const std::wstring title = I18n::Get(StringId::OpenAiSettingsTitle);
    TemplateBuilder tb;
    tb.Begin(title, 210, 130, /*itemCount=*/8);

    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    const DWORD COMBO = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | CBS_DROPDOWN;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD EDIT_CLS   = 0x0081;  // "EDIT"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD COMBO_CLS  = 0x0085;  // "COMBOBOX"

    tb.AddItem(LBL, 8, 6, 44, 9, IDC_STATIC_BASE, STATIC_CLS,
               I18n::Get(StringId::OpenAiBaseUrlLabel));
    tb.AddItem(EDT, 54, 5, 148, 12, IDC_BASE_URL, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 22, 44, 9, IDC_STATIC_KEY, STATIC_CLS,
               I18n::Get(StringId::OpenAiApiKeyLabel));
    tb.AddItem(EDT | ES_PASSWORD, 54, 21, 148, 12, IDC_API_KEY, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 38, 44, 9, IDC_STATIC_MODEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiModelLabel));
    tb.AddItem(COMBO, 54, 37, 100, 64, IDC_MODEL_COMBO, COMBO_CLS, L"");
    tb.AddItem(BTN, 158, 37, 44, 12, IDC_FETCH_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiFetchModels));
    tb.AddItem(LBL, 8, 54, 120, 9, IDC_MASKED_LABEL, STATIC_CLS, L"");
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 96, 70, 50, 13, IDOK, BTN_CLS, L"OK");
    tb.AddItem(BTN, 152, 70, 50, 13, IDCANCEL, BTN_CLS, L"Cancel");

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, OpenAiSettingsProc,
        reinterpret_cast<LPARAM>(&st));
    return st.saved && rc == IDOK;
}

} // namespace emebalachat
