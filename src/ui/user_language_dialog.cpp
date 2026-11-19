#include "user_language_dialog.hpp"

#include "../diag_logger.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <string>
#include <utility>

namespace emebalachat {
namespace user_language_dialog {

// ---- SECTION A: the pure fold -------------------------------------------

std::wstring SubstituteNameTokenOnce(std::wstring_view template_text,
                                     std::wstring_view value) {
    // The repo-wide "{n}" convention, one token, one swap (i18n has no format
    // layer: CountPhrase in language_search_palette.cpp is the same three
    // lines). A SECOND token is deliberately left alone, so "replaced exactly
    // once" is a property of this helper and a locale drift cannot amplify the
    // typed name through the message.
    std::wstring text(template_text);
    const std::wstring token = L"{n}";
    const std::size_t at = text.find(token);
    if (at == std::wstring::npos) {
        return text;
    }
    text.replace(at, token.size(), value);
    return text;
}

UserLanguageCandidate DeriveUserLanguageCandidate(
    std::string_view native_name,
    std::string_view english_name,
    const std::vector<UserLanguage>& existing,
    LocaleLanguageSource locale_source) {
    UserLanguageCandidate candidate;
    // ONE pass through the PUBLIC build choke point. MakeUserLanguage applies
    // the storage cleaning (PR9-03 sanitizer strip + PR10-08 invisible set +
    // ASCII trim + byte cap) to both typed fields, resolves the D5 code
    // (alias matrix -> injected locale names -> ""), back-fills the English
    // name when the code resolved and the user left it empty (PR9-07), and
    // infers the script with its resolved-code fallback. The dialog renders
    // that result and re-implements NONE of it - in particular it never
    // restates the strip set (the sanitizer itself is an anonymous-namespace
    // helper in config.cpp, so the builder is the public way to reach it).
    const UserLanguage built = MakeUserLanguage(native_name, english_name, locale_source);
    candidate.native_name = built.native_name;
    candidate.english_name = built.english_name;
    candidate.code = built.code;
    candidate.script_mask = built.script_mask;
    candidate.script_token = UserLanguageScriptToken(built.script_mask);
    candidate.name_present = !candidate.native_name.empty();
    // The notice explains a TYPED name that resolved to no code; an untouched
    // form must not open with it (thin-air refusal).
    candidate.no_code = candidate.name_present && candidate.code.empty();
    // The same fold the mutator applies inside its lock (PR9-11), on the same
    // cleaned spellings, so the inline message never disagrees with the verb.
    candidate.verdict =
        ValidateUserLanguageName(built.native_name, built.english_name, existing);
    return candidate;
}

UserLanguageCandidate DeriveUserLanguageCandidate(
    std::string_view native_name,
    std::string_view english_name,
    const std::vector<UserLanguage>& existing) {
    return DeriveUserLanguageCandidate(native_name, english_name, existing,
                                       &EnumerateSystemLocaleLanguages);
}

UserLanguageRefusal BuildUserLanguageNameRefusal(UserLanguageNameVerdict verdict,
                                                 std::string_view cleaned_native_name) {
    UserLanguageRefusal refusal;
    if (verdict == UserLanguageNameVerdict::Accepted) {
        return refusal; // nothing to say
    }
    refusal.refuse = true;
    // The landed copy carries the name as "{n}" ("{n}" is already a language
    // name. Enter a different name.). The VALUE is the cleaned form, never the
    // raw typed bytes: a bidi override smuggled into the field must not be able
    // to reorder the line that tells the user it was refused (PR9-03's recorded
    // render-time obligation; the storage strip already removed it, the message
    // renders that same form).
    refusal.message = SubstituteNameTokenOnce(I18n::Get(StringId::UserLangNameReserved),
                                              ToUtf16(cleaned_native_name));
    return refusal;
}

UserLanguageAddDisposition FoldUserLanguageAddResult(UserLanguageWriteResult result) {
    UserLanguageAddDisposition plan;
    switch (result) {
    case UserLanguageWriteResult::Applied:
        // The ONLY closing path: the locked mutator stored the entry, so the
        // dialog may report kAdded.
        plan.outcome = UserLanguageDialogOutcome::kAdded;
        plan.close_dialog = true;
        plan.block = UserLanguageAddBlock::None;
        break;
    case UserLanguageWriteResult::RejectedEmptyName:
        plan.block = UserLanguageAddBlock::EmptyName;
        break;
    case UserLanguageWriteResult::RejectedReservedName:
        plan.block = UserLanguageAddBlock::NameTaken;
        plan.name_verdict = UserLanguageNameVerdict::Reserved;
        break;
    case UserLanguageWriteResult::RejectedDuplicateName:
        plan.block = UserLanguageAddBlock::NameTaken;
        plan.name_verdict = UserLanguageNameVerdict::Duplicate;
        break;
    case UserLanguageWriteResult::RejectedCapacity:
        plan.block = UserLanguageAddBlock::Capacity;
        break;
    case UserLanguageWriteResult::RefusedForwardCompatCarrier:
        plan.block = UserLanguageAddBlock::ForwardCompatStored;
        break;
    default:
        // RejectedNotFound / RejectedMissingProbeKey / RejectedInvalidStatus
        // belong to the probe verbs and can never come back from the add verb;
        // if one ever did, the dialog stays open with no message rather than
        // claiming a success it did not get.
        plan.block = UserLanguageAddBlock::EmptyName;
        break;
    }
    return plan;
}

// ---- SECTION B: the window ----------------------------------------------

namespace {

// The D5 stage-2 data source is a stateless function pointer, so the dialog
// caches the enumeration ONCE per dialog run instead of re-enumerating every
// installed locale on each keystroke (EnumSystemLocalesEx + GetLocaleInfoEx x
// ~90 locales per character would stutter an IME composition). The dialog is
// modal, so there is at most one instance on the GUI thread at a time; the
// closing bounce to the live enumeration (an empty cache) keeps a failed
// enumeration harmless - the ladder then simply has no stage 2.
std::vector<LocaleLanguageName> g_locale_languages;

std::vector<LocaleLanguageName> CachedLocaleLanguages() { return g_locale_languages; }

struct UserLanguageDialogState {
    AppConfig* config = nullptr;
    bool added = false;
};

std::wstring GetCtrlText(HWND dlg, int id) {
    HWND c = ::GetDlgItem(dlg, id);
    if (!c) return {};
    const int len = ::GetWindowTextLengthW(c);
    std::wstring s(static_cast<std::size_t>(len) + 1, L'\0');
    ::GetWindowTextW(c, s.data(), len + 1);
    s.resize(static_cast<std::size_t>(len));
    return s;
}

void SetCtrlText(HWND dlg, int id, std::wstring_view text) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        const std::wstring value(text);
        ::SetWindowTextW(c, value.c_str());
    }
}

// Recomputes the derived rows from the two EDITs. Runs on every EN_CHANGE, so
// the code/script rows and the no-code notice track the typing live (D5: the
// user never needs to know a code). The typed text is NEVER logged - it is user
// content (language_search_palette.cpp:296-303 precedent) - and every value
// written back is the CLEANED form, so a bidi control typed into the field
// cannot reorder a derived row.
void RefreshDerived(HWND dlg, UserLanguageDialogState* st) {
    const std::string native_utf8 = ToUtf8(GetCtrlText(dlg, kIdNativeEdit));
    const std::string english_utf8 = ToUtf8(GetCtrlText(dlg, kIdEnglishEdit));
    const UserLanguageCandidate candidate =
        DeriveUserLanguageCandidate(native_utf8, english_utf8, st->config->user_languages,
                                    &CachedLocaleLanguages);

    SetCtrlText(dlg, kIdCodeValue, ToUtf16(candidate.code));
    // The script row names the mask the entry would carry; a blank form shows
    // nothing rather than a "not detected" verdict on an empty field.
    if (!candidate.name_present) {
        SetCtrlText(dlg, kIdScriptValue, L"");
    } else if (candidate.script_token.empty()) {
        SetCtrlText(dlg, kIdScriptValue, I18n::Get(StringId::UserLangScriptNone));
    } else {
        SetCtrlText(dlg, kIdScriptValue, ToUtf16(candidate.script_token));
    }
    SetCtrlText(dlg, kIdNoCodeNotice,
                candidate.no_code ? I18n::Get(StringId::UserLangNoCodeNotice) : L"");
    // A keystroke retires the previous verdict: a stale "that name is taken"
    // line must never sit next to a name the user already changed.
    SetCtrlText(dlg, kIdRefusal, L"");
    if (HWND add = ::GetDlgItem(dlg, kIdAdd)) {
        ::EnableWindow(add, candidate.name_present ? TRUE : FALSE);
    }
}

INT_PTR CALLBACK UserLanguageDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<UserLanguageDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<UserLanguageDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // Control-creation verification (the openai/engine-host dialogs' REQ-047
        // D3 contract): the template is proven by the unit byte-walk, but a
        // control that failed to materialize must be separable from a logic
        // bug in a support log.
        if (!::GetDlgItem(dlg, kIdNativeEdit)) {
            DIAG_F("UI/UserLanguageDialog/001: native-name edit missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, kIdCodeValue) || !::GetDlgItem(dlg, kIdScriptValue)) {
            DIAG_F("UI/UserLanguageDialog/002: derived rows missing after dialog init\n");
        }
        g_locale_languages = EnumerateSystemLocaleLanguages();
        RefreshDerived(dlg, st);
        // PR9-12 carrier rule (config.hpp: "a GUI writer consults this to
        // warn/refuse BEFORE offering an edit affordance"): while a
        // newer-schema block is preserved verbatim, any in-memory edit would be
        // DISCARDED on save, so the dialog opens read-only instead of letting
        // the user type a language that would vanish.
        if (st->config->HasUserLanguagesForwardCompatCarrier()) {
            if (HWND add = ::GetDlgItem(dlg, kIdAdd)) {
                ::EnableWindow(add, FALSE);
            }
            if (HWND native_edit = ::GetDlgItem(dlg, kIdNativeEdit)) {
                ::EnableWindow(native_edit, FALSE);
            }
            if (HWND english_edit = ::GetDlgItem(dlg, kIdEnglishEdit)) {
                ::EnableWindow(english_edit, FALSE);
            }
            DIAG_F("UI/UserLanguageDialog/003: forward-compat carrier armed; add disabled\n");
        }
        ::SetFocus(::GetDlgItem(dlg, kIdNativeEdit));
        return FALSE; // focus set explicitly
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdNativeEdit:
        case kIdEnglishEdit:
            if (HIWORD(wp) == EN_CHANGE) {
                RefreshDerived(dlg, st);
                return TRUE;
            }
            return FALSE;
        case kIdAdd: {
            const std::string native_utf8 = ToUtf8(GetCtrlText(dlg, kIdNativeEdit));
            const std::string english_utf8 = ToUtf8(GetCtrlText(dlg, kIdEnglishEdit));
            // The entry is built through the public choke point (cleaned,
            // resolved, scripted) and handed to the LOCKED mutator, which
            // re-validates inside the lock: the mutator, not this dialog, is
            // the authority on whether the entry may exist.
            const UserLanguage entry =
                MakeUserLanguage(native_utf8, english_utf8, &CachedLocaleLanguages);
            const UserLanguageWriteResult result = st->config->AddUserLanguage(entry);
            const UserLanguageAddDisposition plan = FoldUserLanguageAddResult(result);
            if (plan.close_dialog) {
                st->added = true;
                ::EndDialog(dlg, IDOK);
                return TRUE;
            }
            switch (plan.block) {
            case UserLanguageAddBlock::NameTaken:
                DIAG_F("UI/UserLanguageDialog/004: add refused (name taken)\n");
                break;
            case UserLanguageAddBlock::Capacity:
                DIAG_F("UI/UserLanguageDialog/005: add refused (capacity)\n");
                break;
            case UserLanguageAddBlock::ForwardCompatStored:
                DIAG_F("UI/UserLanguageDialog/006: add refused (forward-compat block)\n");
                break;
            default:
                break;
            }
            if (plan.block == UserLanguageAddBlock::NameTaken) {
                // The "{n}" value is the CLEANED typed name the derivation
                // already produced (a hand-typed bidi mark cannot reach the
                // message).
                const UserLanguageCandidate candidate = DeriveUserLanguageCandidate(
                    native_utf8, english_utf8, st->config->user_languages,
                    &CachedLocaleLanguages);
                const UserLanguageRefusal refusal = BuildUserLanguageNameRefusal(
                    plan.name_verdict, candidate.native_name);
                SetCtrlText(dlg, kIdRefusal, refusal.message);
                if (HWND native_edit = ::GetDlgItem(dlg, kIdNativeEdit)) {
                    ::SetFocus(native_edit);
                }
            }
            return TRUE;
        }
        case IDCANCEL:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    }
    return FALSE;
}

} // namespace

void BuildUserLanguageTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    // ES_AUTOHSCROLL: a single-line EDIT without it refuses input past its
    // visible width (device-confirmed on the HF/OpenAI add dialogs). Language
    // names are routinely longer than 244 DLU.
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082; // "STATIC"
    const WORD EDIT_CLS = 0x0081;   // "EDIT"
    const WORD BTN_CLS = 0x0080;    // "BUTTON"

    // Labels sit ABOVE their fields (full-width rows): the English label
    // ("English name (optional)" / "영어 이름 (선택)") is far too wide for the
    // 56-DLU label column the OpenAI form uses. The derived code/script rows
    // share one line with their labels (label 116 DLU + value 124 DLU).
    tb.AddItem(LBL, 8, 6, 244, 9, kIdNativeLabel, STATIC_CLS,
               I18n::Get(StringId::UserLangNativeNameLabel));
    tb.AddItem(EDT, 8, 19, 244, 12, kIdNativeEdit, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 35, 244, 9, kIdEnglishLabel, STATIC_CLS,
               I18n::Get(StringId::UserLangEnglishNameLabel));
    tb.AddItem(EDT, 8, 48, 244, 12, kIdEnglishEdit, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 64, 116, 9, kIdCodeLabel, STATIC_CLS,
               I18n::Get(StringId::UserLangCodeLabel));
    // Derived rows start empty: the proc fills them live from the typed name
    // (code via the D5 ladder, script via the inferred mask).
    tb.AddItem(LBL, 128, 64, 124, 9, kIdCodeValue, STATIC_CLS, L"");
    tb.AddItem(LBL, 8, 78, 116, 9, kIdScriptLabel, STATIC_CLS,
               I18n::Get(StringId::UserLangScriptLabel));
    tb.AddItem(LBL, 128, 78, 124, 9, kIdScriptValue, STATIC_CLS, L"");
    // Two text lines tall each so the longer locales wrap instead of clipping
    // (the notice is a full sentence; the refusal carries the typed name).
    tb.AddItem(LBL, 8, 92, 244, 26, kIdNoCodeNotice, STATIC_CLS, L"");
    tb.AddItem(LBL, 8, 120, 244, 20, kIdRefusal, STATIC_CLS, L"");
    // Add is the default button; it starts DISABLED (empty name) and the
    // EN_CHANGE handler enables it. The dialog's own i18n labels (P9b-1 ids,
    // independently localizable per dialog) rather than DialogOk/DialogCancel.
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 108, 146, 64, 14, kIdAdd, BTN_CLS,
               I18n::Get(StringId::UserLangAdd));
    tb.AddItem(BTN, 180, 146, 64, 14, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::UserLangCancel));
}

UserLanguageDialogOutcome ShowUserLanguageDialogEx(HWND parent, AppConfig& config) {
    UserLanguageDialogState st;
    st.config = &config;
    g_locale_languages.clear();

    const std::wstring title = I18n::Get(StringId::UserLangAddTitle);
    TemplateBuilder tb;
    tb.Begin(title, kDialogWidthDlu, kDialogHeightDlu, kDialogItemCount);
    BuildUserLanguageTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, UserLanguageDialogProc,
        reinterpret_cast<LPARAM>(&st));
    g_locale_languages.clear(); // no cached locale table survives the dialog

    // kAdded ONLY on IDOK, and `added` is only ever set on the Applied path of
    // the locked mutator - a refused add keeps the dialog open, so this pair
    // cannot report a success that did not happen.
    return (st.added && rc == IDOK) ? UserLanguageDialogOutcome::kAdded
                                    : UserLanguageDialogOutcome::kCancelled;
}

} // namespace user_language_dialog
} // namespace emebalachat
