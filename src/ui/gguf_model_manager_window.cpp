#include "gguf_model_manager_window.hpp"

#include "../diag_logger.hpp" // REQ-047 D3 §C.4-style control/IO verification logging
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace emebalachat {

namespace {

// Control IDs (manager dialog). Own band inside the 2100-range free space;
// these are template-local ids, unrelated to the tray menu ID bands.
enum : WORD {
    IDC_MGR_LIST = 4100,
    IDC_MGR_EMPTY,
    IDC_MGR_RENAME,
    IDC_MGR_DELETE,
    // IDCANCEL doubles as the Close button (Esc == Close, Win32 default).
};

// Control IDs (rename prompt dialog).
enum : WORD {
    IDC_RENAME_PROMPT = 4200,
    IDC_RENAME_EDIT,
};

struct ManagerDialogState {
    AppConfig* config = nullptr;          // REQ-048 R2-D: user_model_id tracking
    TranslationManager* engine = nullptr; // REQ-048 R2-D: delete -> Auto re-point
    engine_host_registry::Registry registry;
    std::vector<std::string> user_ids; // ListBox row i -> registry user-model id
    bool registry_changed = false;
};

struct RenameDialogState {
    std::wstring initial; // pre-fill (current id)
    std::wstring result;  // out: edit content on IDOK
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

// REQ-048 R2-D: the list shows exactly the origin != "bundled" entries
// (UserModelsOf), so row i of the ListBox maps to user_ids[i]. The empty
// state hides the list, shows the static placeholder, and disables the two
// action buttons.
void RebuildManagerList(HWND dlg, ManagerDialogState* st) {
    HWND list = ::GetDlgItem(dlg, IDC_MGR_LIST);
    if (!list) return;
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    st->user_ids.clear();
    const auto users = UserModelsOf(st->registry);
    st->user_ids.reserve(users.size());
    for (const auto* m : users) {
        std::wstring label = ToUtf16(m->id);
        if (!m->files.empty()) {
            label += L" — " + ToUtf16(m->files[0]); // "id — filename"
        }
        const int row = static_cast<int>(
            ::SendMessageW(list, LB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(label.c_str())));
        st->user_ids.push_back(m->id);
        ::SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(row),
                       static_cast<LPARAM>(st->user_ids.size() - 1));
    }
    const bool empty = st->user_ids.empty();
    ::ShowWindow(list, empty ? SW_HIDE : SW_SHOW);
    if (HWND e = ::GetDlgItem(dlg, IDC_MGR_EMPTY)) {
        ::ShowWindow(e, empty ? SW_SHOW : SW_HIDE);
    }
    if (HWND b = ::GetDlgItem(dlg, IDC_MGR_RENAME)) {
        ::EnableWindow(b, !empty);
    }
    if (HWND b = ::GetDlgItem(dlg, IDC_MGR_DELETE)) {
        ::EnableWindow(b, !empty);
    }
    if (!empty) {
        ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
}

// Returns the user_ids index of the selected row, or -1.
int SelectedUserIndex(HWND dlg, const ManagerDialogState* st) {
    HWND list = ::GetDlgItem(dlg, IDC_MGR_LIST);
    if (!list) return -1;
    const int sel = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (sel < 0) return -1;
    const LPARAM data = ::SendMessageW(list, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
    if (data < 0 || static_cast<size_t>(data) >= st->user_ids.size()) {
        return -1;
    }
    return static_cast<int>(data);
}

// REQ-048 R2-D: registry persistence with the RegisterUserGgufModel loud-
// failure discipline — SerializeRegistry's bare-filename refusal and every IO
// error surface a MessageBox + DIAG line and leave disk untouched. Returns
// true only when the document landed complete.
bool WriteRegistryLoud(HWND owner, const engine_host_registry::Registry& registry) {
    const std::wstring title = I18n::Get(StringId::GgufManagerTitle);
    const std::string serialized = engine_host_registry::SerializeRegistry(registry);
    if (serialized.empty()) {
        ::MessageBoxW(owner, I18n::Get(StringId::GgufManagerErrSerialize).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        DIAG_F("UI/GgufManager/001: SerializeRegistry refused (non-bare filename)\n");
        return false;
    }
    const std::filesystem::path models_dir = engine_host_registry::DefaultModelsDir();
    if (models_dir.empty()) {
        ::MessageBoxW(owner, I18n::Get(StringId::GgufManagerErrNoLocalappdata).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        DIAG_F("UI/GgufManager/002: LOCALAPPDATA missing; registry not written\n");
        return false;
    }
    const std::filesystem::path registry_path = models_dir / L"registry.json";
    {
        std::ofstream out(registry_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            ::MessageBoxW(owner, I18n::Get(StringId::GgufManagerErrWrite).c_str(),
                          title.c_str(), MB_OK | MB_ICONERROR);
            DIAG_F("UI/GgufManager/003: registry.json open-for-write failed\n");
            return false;
        }
        out << serialized;
        out.close();
        if (!out) {
            ::MessageBoxW(owner, I18n::Get(StringId::GgufManagerErrWritePartial).c_str(),
                          title.c_str(), MB_OK | MB_ICONERROR);
            DIAG_F("UI/GgufManager/004: registry.json write failed mid-stream\n");
            return false;
        }
    }
    return true;
}

INT_PTR CALLBACK RenameModelProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* st = reinterpret_cast<RenameDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (HWND edit = ::GetDlgItem(dlg, IDC_RENAME_EDIT)) {
            ::SetWindowTextW(edit, st->initial.c_str());
            ::SendMessageW(edit, EM_SETSEL, 0, -1); // pre-select: typing replaces
        } else {
            DIAG_F("UI/GgufManager/005: IDC_RENAME_EDIT control missing after dialog init\n");
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            auto* st = reinterpret_cast<RenameDialogState*>(
                ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
            st->result = GetCtrlText(dlg, IDC_RENAME_EDIT);
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

// REQ-048 R2-D: rename prompt template — 1 prompt label + 1 edit + OK/Cancel.
// Anonymous namespace (only the manager proc uses it); BuildGgufManagerTemplate
// below stays namespace-scope for the unit-suite byte-walk, mirroring the
// BuildOpenAiTemplate split precedent.
void BuildGgufRenameTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD EDIT_CLS   = 0x0081;  // "EDIT"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"

    tb.AddItem(LBL, 8, 5, 204, 9, IDC_RENAME_PROMPT, STATIC_CLS,
               I18n::Get(StringId::GgufManagerRenameBody));
    tb.AddItem(EDT, 8, 16, 204, 12, IDC_RENAME_EDIT, EDIT_CLS, L"");
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 110, 34, 48, 13, IDOK, BTN_CLS, L"OK");
    tb.AddItem(BTN, 164, 34, 48, 13, IDCANCEL, BTN_CLS, L"Cancel");
}

// Runs the rename prompt loop for `old_id` until the user cancels, enters a
// valid new id, or the write fails. Returns true when a rename was applied
// and persisted (caller sets registry_changed + refreshes the list);
// `applied_new_id` receives the persisted id so the caller can re-select it.
bool RunRenameFlow(HWND dlg, ManagerDialogState* st, const std::string& old_id,
                   std::string* applied_new_id) {
    const std::wstring title = I18n::Get(StringId::GgufManagerTitle);
    std::wstring last_input = ToUtf16(old_id);
    for (;;) {
        RenameDialogState rs;
        rs.initial = last_input;
        const std::wstring dlg_title = I18n::Get(StringId::GgufManagerRenameTitle);
        TemplateBuilder tb;
        tb.Begin(dlg_title, 220, 54, /*itemCount=*/4);
        BuildGgufRenameTemplate(tb);
        const INT_PTR rc = ::DialogBoxIndirectParamW(
            ::GetModuleHandleW(nullptr), tb.Get(), dlg, RenameModelProc,
            reinterpret_cast<LPARAM>(&rs));
        if (rc != IDOK) {
            return false; // Cancel (or dialog failure): nothing applied
        }
        last_input = rs.result;
        const std::string new_id = ToUtf8(rs.result);
        if (new_id == old_id) {
            return false; // no-op rename
        }
        const GgufModelIdError verr =
            ValidateUserModelId(new_id, st->registry, old_id);
        if (verr != GgufModelIdError::Ok) {
            // Re-prompt: the edit re-opens pre-filled with the rejected input.
            ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerRenameInvalid).c_str(),
                          title.c_str(), MB_OK | MB_ICONWARNING);
            DIAG_F("UI/GgufManager/006: rename rejected (old=%s len=%zu err=%d)\n",
                   old_id.c_str(), new_id.size(), static_cast<int>(verr));
            continue;
        }
        // Mutate a COPY so an in-memory rename can never diverge from a
        // failed/partial disk write (state == disk stays the invariant).
        engine_host_registry::Registry candidate = st->registry;
        if (!RenameUserModelEntry(candidate, old_id, new_id)) {
            DIAG_F("UI/GgufManager/007: rename lost the source entry (id=%s)\n",
                   old_id.c_str());
            return false;
        }
        if (!WriteRegistryLoud(dlg, candidate)) {
            return false;
        }
        st->registry = std::move(candidate);
        // REQ-048 R2-D: keep config's engine binding intact across renames.
        if (st->config->user_model_id == old_id) {
            st->config->SetUserModelId(new_id);
            st->config->SaveToFile();
        }
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerDone).c_str(),
                      title.c_str(), MB_OK | MB_ICONINFORMATION);
        DIAG_F("UI/GgufManager/010: renamed model %s -> %s\n",
               old_id.c_str(), new_id.c_str());
        *applied_new_id = new_id;
        return true;
    }
}

INT_PTR CALLBACK GgufManagerProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ManagerDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<ManagerDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (!::GetDlgItem(dlg, IDC_MGR_LIST)) {
            DIAG_F("UI/GgufManager/011: IDC_MGR_LIST control missing after dialog init\n");
        }
        // REQ-048 R2-D: same loader discipline as RegisterUserGgufModel — a
        // damaged/schema-rejected registry is NEVER opened for editing; the
        // manager aborts loudly instead of offering a destructive no-op.
        auto res = engine_host_registry::LoadDefaultRegistry();
        if (res.status != engine_host_registry::LoadStatus::Ok &&
            res.status != engine_host_registry::LoadStatus::Missing) {
            ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrRegistryDamaged).c_str(),
                          I18n::Get(StringId::GgufManagerTitle).c_str(),
                          MB_OK | MB_ICONERROR);
            DIAG_F("UI/GgufManager/012: registry load status=%d; manager refused to open\n",
                   static_cast<int>(res.status));
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        if (res.status == engine_host_registry::LoadStatus::Missing) {
            res.registry.schema_version = engine_host_registry::kRegistrySchemaVersion;
        }
        st->registry = std::move(res.registry);
        RebuildManagerList(dlg, st);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MGR_RENAME: {
            const int idx = SelectedUserIndex(dlg, st);
            if (idx < 0) return TRUE;
            const std::string old_id = st->user_ids[static_cast<size_t>(idx)];
            std::string applied_new_id;
            if (RunRenameFlow(dlg, st, old_id, &applied_new_id)) {
                st->registry_changed = true;
                RebuildManagerList(dlg, st);
                // Re-select the renamed row so the next action is obvious.
                if (!applied_new_id.empty()) {
                    if (HWND list = ::GetDlgItem(dlg, IDC_MGR_LIST)) {
                        const int count =
                            static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
                        for (int row = 0; row < count; ++row) {
                            const LPARAM data = ::SendMessageW(
                                list, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
                            if (data >= 0 &&
                                static_cast<size_t>(data) < st->user_ids.size() &&
                                st->user_ids[static_cast<size_t>(data)] == applied_new_id) {
                                ::SendMessageW(list, LB_SETCURSEL,
                                               static_cast<WPARAM>(row), 0);
                                break;
                            }
                        }
                    }
                }
            }
            return TRUE;
        }
        case IDC_MGR_DELETE: {
            const int idx = SelectedUserIndex(dlg, st);
            if (idx < 0) return TRUE;
            const std::string old_id = st->user_ids[static_cast<size_t>(idx)];
            // REQ-048 R2-D: explicit confirmation. The body states the .gguf
            // FILE STAYS ON DISK — multi-GB files are never auto-deleted.
            const int rc = ::MessageBoxW(
                dlg, I18n::Get(StringId::GgufManagerDeleteConfirmBody).c_str(),
                I18n::Get(StringId::GgufManagerDeleteConfirmTitle).c_str(),
                MB_YESNO | MB_ICONWARNING);
            if (rc != IDYES) {
                return TRUE;
            }
            engine_host_registry::Registry candidate = st->registry;
            if (!RemoveUserModelEntry(candidate, old_id)) {
                DIAG_F("UI/GgufManager/013: delete refused (missing or bundled id=%s)\n",
                       old_id.c_str());
                return TRUE;
            }
            if (!WriteRegistryLoud(dlg, candidate)) {
                return TRUE;
            }
            st->registry = std::move(candidate);
            // REQ-048 R2-D: when the deleted model was the active binding,
            // reset user_model_id and fall engine_type back to "auto" — with
            // the runtime re-point, mirroring the registration pipeline's
            // config-only-would-stale precedent (RegisterUserGgufModel §5).
            if (st->config->user_model_id == old_id) {
                st->config->SetUserModelId("");
                st->config->SetEngineTypeName("auto");
                st->engine->SetEngineType(EngineType::Auto);
                st->config->SaveToFile();
            }
            st->registry_changed = true;
            RebuildManagerList(dlg, st);
            ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerDone).c_str(),
                          I18n::Get(StringId::GgufManagerTitle).c_str(),
                          MB_OK | MB_ICONINFORMATION);
            DIAG_F("UI/GgufManager/014: deleted model registration id=%s (file kept on disk)\n",
                   old_id.c_str());
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

// ---- Pure helpers (declared in the header, linked into the unit suite) ----

GgufModelIdError ValidateUserModelId(const std::string& new_id,
                                     const engine_host_registry::Registry& registry,
                                     const std::string& current_id) {
    if (new_id.empty()) {
        return GgufModelIdError::Empty;
    }
    if (new_id.size() > kMaxUserModelIdLen) {
        return GgufModelIdError::TooLong;
    }
    for (const unsigned char c : new_id) {
        // REQ-048 R2-D: no whitespace (incl. space/tab), no control chars, and
        // no path separators / drive colon (a registry id must stay a plain
        // label, never a path fragment). Non-ASCII filename stems stay legal
        // (the registration pipeline can produce them).
        if (c <= 0x20 || c == 0x7F) {
            return GgufModelIdError::BadChars;
        }
        if (c == '/' || c == '\\' || c == ':') {
            return GgufModelIdError::BadChars;
        }
    }
    for (const auto& m : registry.models) {
        if (m.id != current_id && m.id == new_id) {
            return GgufModelIdError::Duplicate;
        }
    }
    return GgufModelIdError::Ok;
}

std::vector<const engine_host_registry::ModelEntry*>
UserModelsOf(const engine_host_registry::Registry& registry) {
    std::vector<const engine_host_registry::ModelEntry*> out;
    for (const auto& m : registry.models) {
        if (m.origin != "bundled") {
            out.push_back(&m);
        }
    }
    return out;
}

bool RenameUserModelEntry(engine_host_registry::Registry& registry,
                          const std::string& old_id, const std::string& new_id) {
    for (auto& m : registry.models) {
        if (m.id != old_id) {
            continue;
        }
        // Bundled entries are unconditionally preserved (double guard: the
        // manager list never shows them either).
        if (m.origin == "bundled") {
            return false;
        }
        if (registry.FindModel(new_id) != nullptr) {
            return false;
        }
        m.id = new_id;
        return true;
    }
    return false;
}

bool RemoveUserModelEntry(engine_host_registry::Registry& registry,
                          const std::string& id) {
    for (auto it = registry.models.begin(); it != registry.models.end(); ++it) {
        if (it->id == id && it->origin != "bundled") {
            registry.models.erase(it);
            return true;
        }
    }
    return false;
}

// REQ-048 R2-D: production manager item-set emission, split out of
// ShowGgufModelManagerDialog so the unit suite serializes the exact bytes the
// dialog hands to DialogBoxIndirectParamW (BuildOpenAiTemplate precedent).
// itemCount must match the AddItem calls below: list + empty placeholder +
// Rename + Delete + Close(IDCANCEL) = 5.
void BuildGgufManagerTemplate(TemplateBuilder& tb) {
    const DWORD LBX = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                      WS_VSCROLL | LBS_NOTIFY;
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD LIST_CLS   = 0x0083;  // "LISTBOX"

    tb.AddItem(LBX, 8, 6, 194, 58, IDC_MGR_LIST, LIST_CLS, L"");
    tb.AddItem(LBL, 8, 6, 194, 58, IDC_MGR_EMPTY, STATIC_CLS,
               I18n::Get(StringId::GgufManagerEmpty));
    tb.AddItem(BTN, 8, 70, 60, 13, IDC_MGR_RENAME, BTN_CLS,
               I18n::Get(StringId::GgufManagerRename));
    tb.AddItem(BTN, 74, 70, 60, 13, IDC_MGR_DELETE, BTN_CLS,
               I18n::Get(StringId::GgufManagerDelete));
    // Close doubles as IDCANCEL so Esc dismisses the dialog (Win32 default).
    tb.AddItem(BTN, 148, 70, 54, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::GgufManagerClose));
}

bool ShowGgufModelManagerDialog(HWND parent, AppConfig& config,
                                TranslationManager& engine) {
    ManagerDialogState st;
    st.config = &config;
    st.engine = &engine;

    const std::wstring title = I18n::Get(StringId::GgufManagerTitle);
    TemplateBuilder tb;
    tb.Begin(title, 210, 96, /*itemCount=*/5);
    BuildGgufManagerTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, GgufManagerProc,
        reinterpret_cast<LPARAM>(&st));
    (void)rc; // IDCANCEL vs IDOK carries no extra meaning; registry_changed does
    return st.registry_changed;
}

} // namespace emebalachat
