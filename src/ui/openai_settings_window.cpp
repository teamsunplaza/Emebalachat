#include "openai_settings_window.hpp"

#include "../diag_logger.hpp" // REQ-047 D3 §C.4: control-creation verification logging
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
        // REQ-047 D3 (architect §C.4 "API입력칸 미노출" verification): the
        // template itself is proven-good (10 controls incl. OK/Cancel), but
        // if any edit/combo control failed to materialize the dialog would
        // look like the
        // reported "API input field missing" symptom. Surface a distinct log
        // line per missing control so a future regression separates "window
        // failed to activate" (D3 root cause) from "control failed to create".
        if (!::GetDlgItem(dlg, IDC_BASE_URL)) {
            DIAG_F("UI/OpenAiSettings/001: IDC_BASE_URL control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_API_KEY)) {
            DIAG_F("UI/OpenAiSettings/002: IDC_API_KEY control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_MODEL_COMBO)) {
            DIAG_F("UI/OpenAiSettings/003: IDC_MODEL_COMBO control missing after dialog init\n");
        }
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
                // REQ-050: digest BEFORE protect — ProtectOpenAiApiKey scrubs
                // the caller's cleartext buffer in place, so hashing after it
                // persisted SHA-256(zero buffer) and the probed key never
                // matched the saved digest.
                OpenAiSha256Hex(keyUtf8, probe.api_key_sha256);
                ProtectOpenAiApiKey(keyUtf8, probe.api_key_dpapi);
            }
            SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
            // REQ-050: the pre-save fetch hit the same http consent gate as
            // IDOK, but the probe never set http_consent_given, so an http://
            // base URL was always rejected before any network I/O. Mirror the
            // IDOK flow: ask the SAME consent question when the probed base
            // URL is http and no consent is on record; abort the fetch on NO.
            const OpenAiUrlSecurity probe_sec = ClassifyOpenAiBaseUrl(probe.base_url);
            if (probe_sec == OpenAiUrlSecurity::Http && !st->cfg->http_consent_given) {
                const int rc = ::MessageBoxW(
                    dlg, I18n::Get(StringId::OpenAiHttpWarningBody).c_str(),
                    I18n::Get(StringId::OpenAiHttpWarningTitle).c_str(),
                    MB_YESNO | MB_ICONWARNING);
                if (rc != IDYES) return TRUE; // user declined: abort the fetch
                probe.http_consent_given = true;
            } else {
                probe.http_consent_given = st->cfg->http_consent_given;
            }
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
                // REQ-050: digest BEFORE protect — ProtectOpenAiApiKey scrubs
                // the caller's cleartext buffer in place (SecureZeroMemory),
                // so hashing after it persisted SHA-256(zero buffer);
                // WithUnprotectedKey re-hashes the real decrypted key, the
                // comparison never matched, and every saved key was refused
                // (live: OPENAI/WithUnprotectedKey/002). On protect failure
                // the already-computed digest is discarded with `out`.
                OpenAiSha256Hex(keyUtf8, out.api_key_sha256);
                if (!ProtectOpenAiApiKey(keyUtf8, out.api_key_dpapi)) {
                    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                    ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                                  I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                                  MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
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

} // namespace

// ---- In-memory dialog template builder ----
// REQ-046 P4-3 (Tech Gate 필수-5): class declaration moved to
// openai_settings_window.hpp (public emebalachat symbol) so the unit suite can
// pin the template style bits without entering the modal loop. The method
// implementations stay here. Serializes a DLGTEMPLATE + DLGITEMTEMPLATEs into
// a byte buffer with correct 4-byte alignment (per the Win32 dialog-template
// contract).
void TemplateBuilder::Begin(std::wstring_view title, short w, short h, WORD itemCount) {
    // Pad to a 4-byte boundary before the (DWORD-aligned) DLGTEMPLATE.
    while (buf_.size() % 4 != 0) buf_.push_back(0);
    dtpl_ = buf_.size();
    // REQ-046 P4-3 (Rev2 §C, Tech Gate 권고-6): WS_VISIBLE makes the dialog
    // actually show (without it the dialog came up as a minimized/invisible
    // shell — the reported "최소화된 창" symptom); DS_CENTER centers it on the
    // owner screen; DS_SETFOREGROUND forces foreground so it does not open
    // behind other windows.
    EmitDword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME |
              WS_VISIBLE | DS_CENTER | DS_SETFOREGROUND);
    EmitDword(0);                       // dwExtendedStyle
    // REQ-048 P2: cdit must follow dwExtendedStyle — the Win32 DLGTEMPLATE
    // contract orders the header style, dwExtendedStyle, cdit, x, y, cx, cy.
    // The pre-P2 layout emitted x,y,cx,cy first, so the loader read cx as the
    // item count and cdit as cy (an 8-DLU caption strip with zero parsed
    // items — the byte-level root cause of the device symptom "title + X
    // only, no input fields").
    EmitWord(itemCount);                // cdit
    EmitWord(0); EmitWord(0);           // x, y
    EmitWord(w); EmitWord(h);           // cx, cy
    EmitWord(0);                        // menu (none)
    EmitWord(0);                        // windowClass (none)
    EmitStr(title);
    EmitWord(9);                        // pointsize
    EmitStr(L"Segoe UI");
}

void TemplateBuilder::AddItem(DWORD style, short x, short y, short cx, short cy,
                              WORD id, WORD clsAtom, std::wstring_view text) {
    // Each DLGITEMTEMPLATE must be 4-byte aligned relative to the start
    // of the DLGTEMPLATE.
    while ((buf_.size() - dtpl_) % 4 != 0) buf_.push_back(0);
    EmitDword(style);
    EmitDword(0);                       // dwExtendedStyle
    EmitWord(x); EmitWord(y); EmitWord(cx); EmitWord(cy);
    EmitWord(id);
    // REQ-048 P2: the fixed DLGITEMTEMPLATE part ends at id (18 bytes total:
    // style 4 + exstyle 4 + x/y/cx/cy 8 + id 2); the class array follows
    // immediately. A leading 0xFFFF WORD marks the next WORD as a system-
    // class ordinal (0x0080..0x0085). The pre-P2 fake pad WORD after id and
    // the missing prefix desynced the parser walk, so every control after
    // the first was misparsed.
    EmitWord(0xFFFF);
    EmitWord(clsAtom);
    EmitStr(text);
    EmitWord(0);                        // extraData count = 0
}

const DLGTEMPLATE* TemplateBuilder::Get() const {
    return reinterpret_cast<const DLGTEMPLATE*>(buf_.data() + dtpl_);
}

// REQ-048 P2: whole-buffer byte count. For a freshly constructed builder
// dtpl_ == 0, so size() is exactly the serialized template length the test
// byte-walk consumes to.
size_t TemplateBuilder::size() const { return buf_.size(); }

void TemplateBuilder::EmitWord(WORD w) {
    buf_.push_back(static_cast<BYTE>(w & 0xFF));
    buf_.push_back(static_cast<BYTE>((w >> 8) & 0xFF));
}

void TemplateBuilder::EmitDword(DWORD d) { EmitWord(LOWORD(d)); EmitWord(HIWORD(d)); }

void TemplateBuilder::EmitStr(std::wstring_view s) {
    for (wchar_t ch : s) EmitWord(static_cast<WORD>(ch));
    EmitWord(0);
}

// REQ-048 P2: production item-set emission, split out of
// ShowOpenAiSettingsDialog so the unit suite serializes the exact bytes the
// dialog hands to DialogBoxIndirectParamW. The control IDs (IDC_*) live in
// the anonymous namespace above; a namespace-scope definition in this same
// TU can reference them. itemCount must match the AddItem calls below:
// 8 app controls + IDOK + IDCANCEL = 10.
void BuildOpenAiTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    const DWORD COMBO = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | CBS_DROPDOWN;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD EDIT_CLS   = 0x0081;  // "EDIT"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD COMBO_CLS  = 0x0085;  // "COMBOBOX"

    // REQ-050: dialog widened 210->262 DLU and the label/edit columns
    // realigned (labels 44->56, edits 148->186) so the fetch button label
    // ("모델 목록 가져오기" / "Fetch model list") no longer clips in any
    // locale; OK/Cancel moved right and widened 50->64 to match.
    tb.AddItem(LBL, 8, 6, 56, 9, IDC_STATIC_BASE, STATIC_CLS,
               I18n::Get(StringId::OpenAiBaseUrlLabel));
    tb.AddItem(EDT, 68, 5, 186, 12, IDC_BASE_URL, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 22, 56, 9, IDC_STATIC_KEY, STATIC_CLS,
               I18n::Get(StringId::OpenAiApiKeyLabel));
    tb.AddItem(EDT | ES_PASSWORD, 68, 21, 186, 12, IDC_API_KEY, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 38, 56, 9, IDC_STATIC_MODEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiModelLabel));
    tb.AddItem(COMBO, 68, 37, 118, 64, IDC_MODEL_COMBO, COMBO_CLS, L"");
    tb.AddItem(BTN, 190, 37, 64, 12, IDC_FETCH_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiFetchModels));
    tb.AddItem(LBL, 8, 54, 240, 9, IDC_MASKED_LABEL, STATIC_CLS, L"");
    // REQ-050: OK/Cancel are real i18n strings now (StringId::DialogOk /
    // DialogCancel), not hardcoded English — the 37-locale tables carry the
    // conventional native button label for each locale.
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 110, 70, 64, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 182, 70, 64, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg) {
    OpenAiDialogState st{&cfg, false};

    const std::wstring title = I18n::Get(StringId::OpenAiSettingsTitle);
    TemplateBuilder tb;
    tb.Begin(title, 262, 130, /*itemCount=*/10);
    BuildOpenAiTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, OpenAiSettingsProc,
        reinterpret_cast<LPARAM>(&st));
    return st.saved && rc == IDOK;
}

} // namespace emebalachat
