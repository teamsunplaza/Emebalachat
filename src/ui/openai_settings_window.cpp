#include "openai_settings_window.hpp"

#include "../config.hpp"      // REQ-051 D: AppConfig read-modify-write for the [삭제] persist
#include "../diag_logger.hpp" // REQ-047 D3 §C.4: control-creation verification logging
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <string>
#include <string_view>
#include <vector>

// REQ-050 (cue banners): EM_SETCUEBANNER (0x1501, ECM_FIRST + 1) ships in the
// Vista+ SDK headers; define it defensively so the cue-banner sends below
// survive an older/minimal windows.h. It only works under the UNICODE build
// (the project compiles /DUNICODE) — which is also why SendMessageW is used.
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

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
    IDC_DELETE_BTN,   // REQ-051 D: [설정 삭제] push button (bottom-left row)
};

struct OpenAiDialogState {
    OpenAiConfig* cfg;   // in/out; key DPAPI-protected on save
    bool saved = false;
    bool deleted = false; // REQ-051 D: [삭제] persisted the cleared block itself
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

// REQ-050 (auto-fetch): debounce timer for the model-list auto-fetch. The
// timer id lives in the Win32 timer namespace (no clash with the control IDs
// 100..108 above). Re-arming an existing id via SetTimer is a silent
// kill+restart, which is exactly the debounce semantics the EN_CHANGE
// handler wants.
constexpr UINT_PTR kFetchTimerId = 101;
constexpr UINT kFetchDebounceMs = 800;

// REQ-050 (auto-fetch): a key is "available" for the debounced fetch when
// the edit holds anything (a typed key or the masked placeholder of the
// stored pair) or the stored pair itself survived the WM_INITDIALOG
// integrity check (a mismatch clears both fields, so a non-empty blob here
// is a verified pair).
bool KeyAvailableForFetch(HWND dlg, const OpenAiConfig* cfg) {
    if (!GetCtrlText(dlg, IDC_API_KEY).empty()) return true;
    return !cfg->api_key_dpapi.empty();
}

// REQ-050 (auto-fetch): the model-list fetch shared by the manual "Fetch
// model list" button and the debounced auto-fetch. Owns the probe
// composition (digest BEFORE protect), the http consent gate, the
// synchronous ListModels call (10 s budget, GUI thread — same as the
// pre-factor button body), and the combo refill; the RESULT NOTICE stays
// with the caller (the button shows the failure box on an empty result, the
// auto-fetch path stays silent). Returns the fetched ids — empty when the
// fetch was declined or failed — so the caller can tell "ran, got nothing"
// apart from "declined, combo untouched".
// REQ-051 (Symptom C): the refill no longer clobbers the current model —
// it pins the combo's current text (saved model or user-typed) via
// PlanOpenAiComboSelection: exact match -> select it; absent -> append it
// and select the append; empty fetch -> the combo is left completely
// untouched so the injected current-model text stays visible.
std::vector<std::string> DoFetchModels(HWND dlg, OpenAiDialogState* st) {
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
        if (rc != IDYES) return {}; // user declined: abort the fetch
        probe.http_consent_given = true;
    } else {
        probe.http_consent_given = st->cfg->http_consent_given;
    }
    const std::vector<std::string> models =
        OpenAiCompatibleClient::ListModels(probe);
    if (HWND combo = ::GetDlgItem(dlg, IDC_MODEL_COMBO)) {
        if (models.empty()) {
            // REQ-051 (Symptom C): fetch failed / empty — do NOT reset the
            // combo (CB_RESETCONTENT would also wipe the edit text on this
            // CBS_DROPDOWN control, hiding the injected current-model text).
            // Leaving the list + text intact keeps the in-use model visible
            // and selectable; the manual-fetch button still surfaces the
            // failure notice, the silent auto path stays silent.
            return models;
        }
        // REQ-051 (Symptom C): capture the current-model text BEFORE the
        // reset — after CB_RESETCONTENT the edit text is gone.
        const std::string target = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
        ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        std::vector<std::pair<int, std::string>> items;
        items.reserve(models.size());
        for (const auto& m : models) {
            const int idx = static_cast<int>(::SendMessageW(
                combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(ToUtf16(m).c_str())));
            if (idx >= 0) items.emplace_back(idx, m);
        }
        // REQ-051 (Symptom C): select the saved/current model when the list
        // holds it; otherwise append the current text and select THAT (the
        // pre-REQ-051 code unconditionally selected index 0, clobbering the
        // saved model). Correctness of the selection beats everything else.
        const OpenAiComboSelection plan = PlanOpenAiComboSelection(items, target);
        if (plan.insert_target) {
            const int idx = static_cast<int>(::SendMessageW(
                combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(ToUtf16(target).c_str())));
            if (idx >= 0) ::SendMessageW(combo, CB_SETCURSEL, idx, 0);
        } else if (plan.select_index >= 0) {
            ::SendMessageW(combo, CB_SETCURSEL, plan.select_index, 0);
        }
    }
    return models;
}

INT_PTR CALLBACK OpenAiSettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<OpenAiDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<OpenAiDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // REQ-047 D3 (architect §C.4 "API입력칸 미노출" verification): the
        // template itself is proven-good (11 controls incl. OK/Cancel and the
        // REQ-051 delete button), but
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
        // REQ-050 (cue banners): gray placeholder hints on both single-line
        // edits while they are empty (EM_SETCUEBANNER — see the define above;
        // works on Vista+ under the UNICODE build).
        if (HWND base_edit = ::GetDlgItem(dlg, IDC_BASE_URL)) {
            ::SendMessageW(base_edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::OpenAiBaseUrlHint).c_str()));
        }
        if (HWND key_edit = ::GetDlgItem(dlg, IDC_API_KEY)) {
            ::SendMessageW(key_edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::OpenAiApiKeyHint).c_str()));
        }
        SetCtrlText(dlg, IDC_BASE_URL, ToUtf16(st->cfg->base_url));
        SetCtrlText(dlg, IDC_MODEL_COMBO, ToUtf16(st->cfg->model));
        // REQ-050 (corrupt-pair recovery): upgraders from the pre-REQ-050
        // build carry api_key_sha256 = SHA-256 of a ZEROED buffer (the old
        // dialog hashed after ProtectOpenAiApiKey scrubbed the cleartext), so
        // the masked-placeholder path would keep reusing a pair
        // WithUnprotectedKey refuses forever (live: OPENAI/WithUnprotectedKey/
        // 002). Verify the persisted pair headlessly at dialog open; on
        // mismatch — or an unprotect failure, which is equally unusable —
        // clear BOTH fields in this in-memory copy so the user is forced to
        // re-enter the key once and the digest-before-protect save path
        // stores a valid pair. Shape-only: no key material is logged.
        if (!st->cfg->api_key_dpapi.empty() && !OpenAiKeyPairIntegrityOk(*st->cfg)) {
            st->cfg->api_key_dpapi.clear();
            st->cfg->api_key_sha256.clear();
            DIAG_F("UI/OpenAiSettings/004: persisted API-key pair failed the integrity "
                   "check; fields cleared for re-entry\n");
        }
        if (!st->cfg->api_key_dpapi.empty()) {
            std::string key;
            if (UnprotectOpenAiApiKey(st->cfg->api_key_dpapi, key)) {
                SetCtrlText(dlg, IDC_API_KEY, ToUtf16(MaskOpenAiApiKey(key)));
                SecureZeroMemory(key.data(), key.size());
            }
        }
        ::SetDlgItemTextW(dlg, IDC_MASKED_LABEL,
                          I18n::Get(StringId::OpenAiKeyMasked).c_str());
        // REQ-050 (auto-fetch): reopening a saved dialog repopulates the
        // model combo — both values already present (https base URL + a
        // usable key) arms the same debounce timer the EN_CHANGE handler
        // uses, so the list arrives without a button click.
        if (ClassifyOpenAiBaseUrl(ToUtf8(GetCtrlText(dlg, IDC_BASE_URL))) ==
                OpenAiUrlSecurity::Https &&
            KeyAvailableForFetch(dlg, st->cfg)) {
            ::SetTimer(dlg, kFetchTimerId, kFetchDebounceMs, nullptr);
        }
        return TRUE;
    }
    case WM_TIMER:
        // REQ-050 (auto-fetch): debounce timer expired. Kill it BEFORE the
        // synchronous fetch — ListModels blocks the GUI thread so no timer
        // can fire during it, and the kill also cancels any timer an
        // in-flight keystroke batch re-armed. The auto path stays SILENT:
        // DoFetchModels refills the combo but never shows the failure box.
        if (wp == kFetchTimerId) {
            ::KillTimer(dlg, kFetchTimerId);
            DoFetchModels(dlg, st);
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND:
        // REQ-050 (auto-fetch): debounce model-list fetches while the user
        // types. https-only — an http:// base must NEVER auto-fetch (the
        // consent question may appear only on explicit button/OK actions,
        // never as a popup mid-typing), so the timer is killed whenever the
        // base URL drops out of the https shape or no key is available.
        if (HIWORD(wp) == EN_CHANGE &&
            (LOWORD(wp) == IDC_BASE_URL || LOWORD(wp) == IDC_API_KEY)) {
            if (ClassifyOpenAiBaseUrl(ToUtf8(GetCtrlText(dlg, IDC_BASE_URL))) ==
                    OpenAiUrlSecurity::Https &&
                KeyAvailableForFetch(dlg, st->cfg)) {
                ::SetTimer(dlg, kFetchTimerId, kFetchDebounceMs, nullptr);
            } else {
                ::KillTimer(dlg, kFetchTimerId);
            }
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_FETCH_BTN: {
            // REQ-050 (auto-fetch): the fetch itself (probe + consent gate +
            // ListModels + combo refill) lives in DoFetchModels, shared with
            // the debounced auto-fetch; the button keeps the failure notice
            // the silent auto path must not show.
            const std::vector<std::string> models = DoFetchModels(dlg, st);
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
        case IDC_DELETE_BTN: {
            // REQ-051 (Symptom D, decisions.md 260921 17:08 — user-frozen
            // scope): clear EXACTLY the saved OpenAI settings (base_url,
            // model, the api_key_dpapi+api_key_sha256 pair TOGETHER so a
            // half-cleared pair can never reach the refuse-forever
            // WithUnprotectedKey state, http_consent_given) and persist,
            // leaving the engine selection untouched. The persist is a
            // read-modify-write of the canonical config.json through a
            // SECOND AppConfig instance: the dialog only ever receives the
            // openai block, so file-level persistence is the only save path
            // that cannot touch engine_type. On success the in/out copy is
            // cleared too and the dialog ends with a NON-IDOK code (kDeleted
            // outcome) so pre-REQ-051 callers — main.cpp
            // kMsgOpenOpenAiSettings via the bool wrapper — see plain cancel
            // semantics (refresh_tray only, NO engine switch, no persisted
            // engine_type="openai"). main.cpp's in-memory AppConfig::openai
            // is reconciled by the caller adopting ShowOpenAiSettingsDialogEx
            // (see the REQ-051 handoff); until then the file is correct and
            // any later in-memory save is the documented integration gap.
            // Fail-closed: a load/save failure keeps the dialog open with
            // every field intact — nothing is ever half-cleared.
            AppConfig persisted;
            if (!persisted.LoadFromFile()) {
                DIAG_F("UI/OpenAiSettings/005: delete requested but config load failed; "
                       "settings left intact\n");
                return TRUE;
            }
            ClearOpenAiSettings(persisted.openai);
            if (!persisted.SaveToFile()) {
                DIAG_F("UI/OpenAiSettings/006: delete requested but config save failed; "
                       "settings left intact\n");
                return TRUE;
            }
            DIAG_F("UI/OpenAiSettings/007: saved OpenAI settings deleted "
                   "(base_url/model/key pair/consent cleared; engine selection untouched)\n");
            *st->cfg = OpenAiConfig{};
            st->deleted = true;
            ::EndDialog(dlg, IDC_DELETE_BTN);
            return TRUE;
        }
        }
        return FALSE;
    }
    return FALSE;
}

} // namespace

// REQ-051 (Symptom C): pick-or-insert plan for the model-combo refill. Pure
// (the combo is stateless w.r.t. this helper), so the unit suite drives the
// exact algorithm the refill applies. Model ids are case-sensitive, so the
// match is exact and case-sensitive; the first occurrence wins. An empty
// target never selects or inserts — the combo is left untouched rather than
// fabricating a selection the user never made.
OpenAiComboSelection PlanOpenAiComboSelection(
    const std::vector<std::pair<int, std::string>>& items,
    const std::string& target) {
    if (target.empty()) return {-1, false};
    for (const auto& [index, text] : items) {
        if (text == target) return {index, false};
    }
    return {items.empty() ? 0 : items.back().first + 1, true};
}

// REQ-051 (Symptom D): clears exactly the five persisted OpenAI settings
// fields. The key pair (DPAPI blob + SHA-256 digest) is wiped TOGETHER: a
// half-cleared pair is what made WithUnprotectedKey refuse a valid key
// forever (REQ-050 b6f98da contract). engine selection is untouched —
// OpenAiConfig carries no engine field.
void ClearOpenAiSettings(OpenAiConfig& cfg) {
    cfg.base_url.clear();
    cfg.model.clear();
    cfg.api_key_dpapi.clear();
    cfg.api_key_sha256.clear();
    cfg.http_consent_given = false;
}

// REQ-050 (corrupt-pair recovery): headless integrity check of a persisted
// key pair — unprotects the DPAPI blob, re-hashes the cleartext, and compares
// against the persisted digest. The cleartext is zeroed before return; it is
// never logged. Returns false on ANY failure or mismatch, including an
// empty/missing digest, so a pair this app cannot vouch for never reaches the
// masked-placeholder reuse path (IDC_FETCH_BTN / IDOK). Namespace scope (not
// the anonymous block above) so the unit suite exercises the exact helper the
// dialog proc calls.
bool OpenAiKeyPairIntegrityOk(const OpenAiConfig& cfg) {
    if (cfg.api_key_dpapi.empty()) return false;
    std::string key;
    std::string digest;
    const bool ok = UnprotectOpenAiApiKey(cfg.api_key_dpapi, key) &&
                    OpenAiSha256Hex(key, digest) &&
                    digest == cfg.api_key_sha256;
    SecureZeroMemory(key.data(), key.size());
    return ok;
}

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
// 9 app controls + IDOK + IDCANCEL = 11 (REQ-051 added the delete button).
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
    // REQ-050 (single-line edit limits): single-line EDITs without
    // ES_AUTOHSCROLL reject input beyond their visible width (device-confirmed
    // on the HF add dialog's URL edit, same latent hazard here) — base URLs
    // and API keys are routinely longer than the 186-DLU edit, so both scroll.
    tb.AddItem(EDT | ES_AUTOHSCROLL, 68, 5, 186, 12, IDC_BASE_URL, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 22, 56, 9, IDC_STATIC_KEY, STATIC_CLS,
               I18n::Get(StringId::OpenAiApiKeyLabel));
    tb.AddItem(EDT | ES_AUTOHSCROLL | ES_PASSWORD, 68, 21, 186, 12, IDC_API_KEY,
               EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 38, 56, 9, IDC_STATIC_MODEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiModelLabel));
    tb.AddItem(COMBO, 68, 37, 118, 64, IDC_MODEL_COMBO, COMBO_CLS, L"");
    tb.AddItem(BTN, 190, 37, 64, 12, IDC_FETCH_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiFetchModels));
    tb.AddItem(LBL, 8, 54, 240, 9, IDC_MASKED_LABEL, STATIC_CLS, L"");
    // REQ-051 (Symptom D): the [설정 삭제] button owns the bottom-LEFT corner
    // of the action row, far from OK/Cancel — a destructive action gets its
    // own corner so it cannot be hit by muscle memory aiming at OK. The
    // 262x130 DLU shell is unchanged. i18n caption (all 37 locales).
    tb.AddItem(BTN, 8, 70, 64, 13, IDC_DELETE_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiDeleteSettings));
    // REQ-050: OK/Cancel are real i18n strings now (StringId::DialogOk /
    // DialogCancel), not hardcoded English — the 37-locale tables carry the
    // conventional native button label for each locale.
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 110, 70, 64, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 182, 70, 64, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(HWND parent, OpenAiConfig& cfg) {
    OpenAiDialogState st{&cfg, false, false};

    const std::wstring title = I18n::Get(StringId::OpenAiSettingsTitle);
    TemplateBuilder tb;
    // REQ-051: 11 items — the IDC_DELETE_BTN button joined the 10-control
    // REQ-048 P2 template (shell size unchanged at 262x130 DLU).
    tb.Begin(title, 262, 130, /*itemCount=*/11);
    BuildOpenAiTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, OpenAiSettingsProc,
        reinterpret_cast<LPARAM>(&st));
    // REQ-051 (Symptom D): the [삭제] button persisted the cleared block
    // itself and ended with a NON-IDOK code; report it distinctly so the
    // caller never routes a deletion through the save/switch (engine-flip)
    // path. Deleted wins over saved (the button ends the dialog immediately,
    // so the two can never co-occur).
    if (st.deleted) return OpenAiSettingsOutcome::kDeleted;
    return (st.saved && rc == IDOK) ? OpenAiSettingsOutcome::kSaved
                                    : OpenAiSettingsOutcome::kCancelled;
}

bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg) {
    // REQ-051 (Symptom D): pre-REQ-051 callers keep their exact contract —
    // only the kSaved outcome reads as true (kDeleted arrives as false, i.e.
    // cancel semantics: engine untouched, no save/switch side effects).
    return ShowOpenAiSettingsDialogEx(parent, cfg) == OpenAiSettingsOutcome::kSaved;
}

} // namespace emebalachat
