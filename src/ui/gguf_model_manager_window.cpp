#include "gguf_model_manager_window.hpp"

#include "../diag_logger.hpp" // REQ-047 D3 §C.4-style control/IO verification logging
#include "../engine_host_json_util.hpp" // REQ-050: IsBareFilename (HF filename gate)
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")  // REQ-050: HF download (the bootstrap/google_translate discipline)
#pragma comment(lib, "comctl32.lib") // REQ-050: progress bar (msctls_progress32)

namespace emebalachat {

namespace {

// Control IDs (manager dialog). Own band inside the 2100-range free space;
// these are template-local ids, unrelated to the tray menu ID bands.
enum : WORD {
    IDC_MGR_LIST = 4100,
    IDC_MGR_EMPTY,
    IDC_MGR_RENAME,
    IDC_MGR_DELETE,
    // REQ-050: the top-row add methods.
    IDC_MGR_ADD_FILE,
    IDC_MGR_ADD_HF,
    // IDCANCEL doubles as the Close button (Esc == Close, Win32 default).
};

// Control IDs (rename prompt dialog).
enum : WORD {
    IDC_RENAME_PROMPT = 4200,
    IDC_RENAME_EDIT,
};

// Control IDs (REQ-050 Hugging Face add dialog).
enum : WORD {
    IDC_HF_URL_LABEL = 4300,
    IDC_HF_URL_EDIT,
    IDC_HF_STATUS,
    // IDC_HF_PROGRESS is created at runtime (msctls_progress32 has no system
    // ordinal for the TemplateBuilder); the ID labels the HWND's HMENU.
    IDC_HF_PROGRESS = 4303,
};

// REQ-050: value-only worker->dialog messages (SEC-ADJ house contract: no
// heap pointer crosses PostMessageW; the payloads are small integers).
// kHfMsgProgress: wParam = percent (0..100, or (WORD)-1 when the server gave
// no Content-Length -> marquee), LPARAM = bytes received so far.
// kHfMsgDone:     wParam = 0 download complete / 1 failed / 2 cancelled.
constexpr UINT kHfMsgProgress = WM_APP + 0x10;
constexpr UINT kHfMsgDone = WM_APP + 0x11;

// REQ-050: 20 GB sanity cap for a user-provided model download (a resolve
// URL that claims more is almost certainly wrong or hostile; the bootstrap
// repair client uses the same bounded-body discipline, sized for manifests).
constexpr unsigned long long kHfMaxDownloadBytes = 20ull * 1024 * 1024 * 1024;

// REQ-051 U-2: registry files[0] -> display stem (the engine name must flip
// to the model stem the moment the manager switches the binding). Same
// case-sensitive ".gguf" rfind-strip policy as main.cpp's GgufFileStem.
std::string ManagerModelStem(const std::string& file) {
    std::string stem = file;
    const auto dot = stem.rfind(".gguf");
    if (dot != std::string::npos && dot + 5 == stem.size()) {
        stem.resize(dot);
    }
    return stem;
}

struct ManagerDialogState {
    AppConfig* config = nullptr;          // REQ-048 R2-D: user_model_id tracking
    TranslationManager* engine = nullptr; // REQ-048 R2-D: delete -> Auto re-point
    // REQ-050: the file-picker registration pipeline (main.cpp callback).
    std::function<void()> add_from_file;
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
// state hides the list, shows the static placeholder, and (through
// SyncManagerActionButtons) greys the two action buttons out.
//
// 260922_0001 A7: forward declaration — the rebuild re-syncs the action
// buttons, while the helper itself reads SelectedUserIndex below.
void SyncManagerActionButtons(HWND dlg, ManagerDialogState* st);

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
    if (!empty) {
        // LB_SETCURSEL is silent — it posts no LBN_SELCHANGE — so the sync
        // below is the only thing that re-enables the buttons after a rebuild.
        ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
    // 260922_0001 A7: selection is the single source of truth for the action
    // pair (empty list, Ctrl+click deselection, post-rebuild auto-select).
    SyncManagerActionButtons(dlg, st);
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

// 260922_0001 A7: the rename/delete pair is enabled only while a user-model
// row is selected, so the IDC_MGR_RENAME / IDC_MGR_DELETE handlers' silent
// `idx < 0` returns become defensive guards instead of dead-click paths.
// Called on every list rebuild and on LBN_SELCHANGE (Ctrl+click deselection
// yields -1 and greys both buttons out). Same "no callback -> disabled"
// precedent as IDC_MGR_ADD_FILE in WM_INITDIALOG.
void SyncManagerActionButtons(HWND dlg, ManagerDialogState* st) {
    const bool enabled = SelectedUserIndex(dlg, st) >= 0;
    if (HWND b = ::GetDlgItem(dlg, IDC_MGR_RENAME)) {
        ::EnableWindow(b, enabled ? TRUE : FALSE);
    }
    if (HWND b = ::GetDlgItem(dlg, IDC_MGR_DELETE)) {
        ::EnableWindow(b, enabled ? TRUE : FALSE);
    }
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

// REQ-050: shared registry loader for the manager dialog — used by
// WM_INITDIALOG AND the post-add reloads ([파일에서 추가…] / [Hugging
// Face에서 추가…] can both append/reuse an entry on disk, so the in-memory
// copy is refreshed from the file before the ListBox is rebuilt). Same
// loader discipline as RegisterUserGgufModel: a damaged/schema-rejected
// registry is a LOUD failure and is NEVER opened for editing. Returns false
// when the dialog must abort (the message was already shown).
bool LoadManagerRegistry(HWND dlg, ManagerDialogState* st) {
    auto res = engine_host_registry::LoadDefaultRegistry();
    if (res.status != engine_host_registry::LoadStatus::Ok &&
        res.status != engine_host_registry::LoadStatus::Missing) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrRegistryDamaged).c_str(),
                      I18n::Get(StringId::GgufManagerTitle).c_str(),
                      MB_OK | MB_ICONERROR);
        DIAG_F("UI/GgufManager/012: registry load status=%d; manager refused to open\n",
               static_cast<int>(res.status));
        return false;
    }
    if (res.status == engine_host_registry::LoadStatus::Missing) {
        res.registry.schema_version = engine_host_registry::kRegistrySchemaVersion;
    }
    st->registry = std::move(res.registry);
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
    // REQ-052: localized captions (the generic DialogOk/DialogCancel pair) —
    // the pre-fix hardcoded English literals stayed English in every
    // non-English UI.
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 110, 34, 48, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 164, 34, 48, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
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

// ==================== REQ-050: Hugging Face add dialog =====================

struct HfAddDialogState {
    AppConfig* config = nullptr;
    TranslationManager* engine = nullptr;
    // The download pumps on a worker thread; joined on EVERY dialog exit path
    // before EndDialog, so this state (the caller's stack) can never dangle.
    std::atomic<bool> cancel{false};
    std::thread worker;
    bool worker_running = false;
    bool registered = false; // out: a model was registered (or reused)
    // Written by the worker before its final kHfMsgDone post; read by the GUI
    // thread only after the join (join is the happens-before edge).
    std::wstring worker_url;
    std::filesystem::path worker_tmp;
    std::string worker_filename;
    // REQ-052: the in-flight connect/request handles, published by the worker
    // so the IDCANCEL path can abort a blocked WinHTTP call — closing an
    // HINTERNET from another thread is the documented abort for a pending
    // operation (the pre-fix design joined the worker while it could sit in
    // WinHttpReadData for the whole 60s receive timeout -> GUI freeze). A
    // slot is nulled under the mutex BEFORE its close and whoever clears it
    // owns the single close, so no handle is double-closed and the worker
    // never uses a handle after the cancel path closed it (every post-close
    // API call fails, so the loop breaks out; the cancel poll sits at the
    // loop top).
    std::mutex hf_handles_mutex;
    HINTERNET hf_connect = nullptr;
    HINTERNET hf_request = nullptr;
};

void HfJoinWorker(HfAddDialogState* st) {
    if (st->worker.joinable()) {
        st->worker.join();
    }
    st->worker_running = false;
}

// REQ-052: single-owner close for a handle published in the dialog state.
// The slot is returned to nullptr under the mutex BEFORE the close; when the
// slot no longer holds `h`, the cancel path already closed it and this side
// must not (exactly-once close across the worker's deleters and the GUI
// cancel path). A null st/slot (the session handle is worker-local) means
// this side always closes.
void HfClearSlot(HfAddDialogState* st, HINTERNET HfAddDialogState::*slot,
                 HINTERNET h) {
    if (!h) {
        return;
    }
    bool mine = true;
    if (st != nullptr && slot != nullptr) {
        mine = false;
        {
            std::lock_guard<std::mutex> lk(st->hf_handles_mutex);
            if (st->*slot == h) {
                st->*slot = nullptr;
                mine = true;
            }
        }
    }
    if (mine) {
        ::WinHttpCloseHandle(h);
    }
}

struct HfScopedHandleDeleter {
    HfAddDialogState* st = nullptr;              // REQ-052: published-slot owner
    HINTERNET HfAddDialogState::*slot = nullptr; // (null => worker-local handle)
    void operator()(HINTERNET h) const {
        HfClearSlot(st, slot, h);
    }
};
using HfScopedHInternet = std::unique_ptr<void, HfScopedHandleDeleter>;

// REQ-050: https-only, host-pinned download worker (the
// engine_host_bootstrap_client::HttpsDownloadToFile discipline, extended for
// multi-GB model files: Content-Length progress, a 20 GB sanity cap, cancel
// polling, generous per-call io timeouts). Runs entirely on the worker
// thread; reports through VALUE-ONLY kHfMsgProgress / kHfMsgDone posts (no
// pointer crosses PostMessageW — the SEC-ADJ house contract) and never
// touches GUI objects or config. The temp <target>.download is deleted on
// every failure AND cancel path; the GUI thread moves it to the final name
// after a verified-complete exit.
//
// Redirect note: huggingface.co resolve URLs answer 302 -> the HF CDN, and
// WinHTTP's DEFAULT redirect policy follows up to 5 hops while REFUSING
// https->http downgrades — exactly the safe shape for this flow, so no
// explicit option is set.
void HfDownloadWorker(HWND dlg, HfAddDialogState* st) {
    int code = 1; // 0 complete / 1 failed / 2 cancelled by the user
    unsigned long long got = 0;
    unsigned long long advertised = 0;

    std::error_code ec;
    std::filesystem::remove(st->worker_tmp, ec); // clear any stale temp

    { // stream scope: the ofstream must close before the completion post
        std::ofstream out(st->worker_tmp, std::ios::binary | std::ios::trunc);
        bool ok = static_cast<bool>(out);
        if (!ok) {
            DIAG_F("UI/HfAdd/001: temp file open failed (path_len=%zu)\n",
                   st->worker_tmp.wstring().size());
        } else {
            // The normalizer guarantees the canonical pinned prefix, so the
            // remainder is the request path (<repo>/resolve/<rev>/<file>).
            constexpr std::size_t kPrefixLen = 23; // "https://huggingface.co/"
            static_assert(kPrefixLen == std::wstring_view(L"https://huggingface.co/").size());
            const wchar_t* path = st->worker_url.c_str() + kPrefixLen;
            HfScopedHInternet session(::WinHttpOpen(
                L"EmebalaChat/1.0 (gguf-model-manager)",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (session) {
                // Multi-GB models need generous per-call budgets (the
                // bootstrap's 15s receive timeout suits small manifests
                // only): resolve/connect 10s, send 30s, receive 60s.
                ::WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, 60000);
            }
            // REQ-052: connect/request publish into the dialog state as soon as
            // they exist, so the IDCANCEL path can close them and abort a
            // blocked call (the single-owner HfClearSlot protocol clears the
            // slot before either side closes).
            // REQ-052 integration fix: unique_ptr<void, Deleter> needs the
            // pointer argument (nullptr — the handle arrives via reset());
            // the deleter-only form never compiled (C2664).
            HfScopedHInternet connect(nullptr, HfScopedHandleDeleter{
                st, &HfAddDialogState::hf_connect});
            if (session) {
                connect.reset(::WinHttpConnect(session.get(), L"huggingface.co",
                                               INTERNET_DEFAULT_HTTPS_PORT, 0));
                if (connect) {
                    std::lock_guard<std::mutex> lk(st->hf_handles_mutex);
                    st->hf_connect = connect.get();
                }
            }
            HfScopedHInternet request(nullptr, HfScopedHandleDeleter{
                st, &HfAddDialogState::hf_request});
            if (connect) {
                request.reset(::WinHttpOpenRequest(connect.get(), L"GET", path,
                                                   nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE));
                if (request) {
                    std::lock_guard<std::mutex> lk(st->hf_handles_mutex);
                    st->hf_request = request.get();
                }
            }
            ok = request != nullptr;
            if (ok) {
                ok = ::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS,
                                          0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
            }
            if (ok) {
                ok = ::WinHttpReceiveResponse(request.get(), nullptr) != FALSE;
            }
            if (ok) {
                DWORD status = 0;
                DWORD sz = sizeof(status);
                ::WinHttpQueryHeaders(request.get(),
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                if (status != 200) {
                    DIAG_F("UI/HfAdd/002: HF download HTTP %lu (path_len=%zu)\n",
                           status, st->worker_url.size());
                    ok = false;
                }
            }
            if (ok) {
                // Content-Length is optional (chunked/CDN): when unknown the
                // dialog keeps the marquee and shows the byte count instead.
                // REQ-052: query the header as TEXT and parse it to a 64-bit
                // value — WINHTTP_QUERY_FLAG_NUMBER fills a DWORD, so a >4GB
                // model (the 20GB cap allows them) parsed as 0 and broke the
                // progress math. (The status-code query above keeps its
                // FLAG_NUMBER path: an HTTP code always fits a DWORD.)
                wchar_t len_buf[32] = {};
                DWORD len_sz = sizeof(len_buf);
                if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH,
                        WINHTTP_HEADER_NAME_BY_INDEX, len_buf, &len_sz,
                        WINHTTP_NO_HEADER_INDEX)) {
                    // Strict decimal parse: a bogus length must not reach the
                    // progress math; unparseable is treated like "unknown"
                    // (marquee), matching the pre-fix FLAG_NUMBER behavior.
                    unsigned long long parsed = 0;
                    bool len_ok = true;
                    for (const wchar_t* p = len_buf; *p != L'\0'; ++p) {
                        if (*p < L'0' || *p > L'9') {
                            len_ok = false;
                            break;
                        }
                        parsed = parsed * 10ull +
                                 static_cast<unsigned long long>(*p - L'0');
                    }
                    if (len_ok) {
                        advertised = parsed;
                        if (advertised > kHfMaxDownloadBytes) {
                            DIAG_F("UI/HfAdd/003: advertised length %llu exceeds the %llu-byte cap\n",
                                   advertised, kHfMaxDownloadBytes);
                            ok = false;
                        }
                    } else {
                        DIAG_F("UI/HfAdd/020: unparseable Content-Length ignored (len=%lu)\n",
                               len_sz);
                    }
                }
            }
            if (ok) {
                auto last_report = std::chrono::steady_clock::now() -
                                   std::chrono::milliseconds(400);
                for (;;) {
                    if (st->cancel.load(std::memory_order_relaxed)) {
                        code = 2;
                        break;
                    }
                    DWORD avail = 0;
                    if (!::WinHttpQueryDataAvailable(request.get(), &avail)) {
                        ok = false;
                        break;
                    }
                    if (avail == 0) {
                        break; // end of stream
                    }
                    if (got + static_cast<unsigned long long>(avail) >
                        kHfMaxDownloadBytes) {
                        DIAG_F("UI/HfAdd/004: streamed body exceeded the %llu-byte cap\n",
                               kHfMaxDownloadBytes);
                        ok = false;
                        break;
                    }
                    char buf[64 * 1024];
                    const DWORD want = avail < sizeof(buf)
                        ? avail : static_cast<DWORD>(sizeof(buf));
                    DWORD read = 0;
                    if (!::WinHttpReadData(request.get(), buf, want, &read)) {
                        ok = false;
                        break;
                    }
                    if (read == 0) {
                        break;
                    }
                    out.write(buf, static_cast<std::streamsize>(read));
                    if (!out) {
                        ok = false;
                        break;
                    }
                    got += read;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_report >= std::chrono::milliseconds(200)) {
                        last_report = now;
                        const int pct = advertised
                            ? static_cast<int>(got * 100ull / advertised) : -1;
                        ::PostMessageW(dlg, kHfMsgProgress,
                            static_cast<WPARAM>(static_cast<WORD>(pct)),
                            static_cast<LPARAM>(got));
                    }
                }
                if (ok && got == 0) {
                    ok = false; // an empty body is a failure, not a model
                }
            }
        }
        if (ok && code != 2) {
            code = 0; // the full body landed in the temp file
        }
        if (!ok && code != 2) {
            code = 1;
        }
        out.close(); // flush + release the temp before the completion post
    }
    if (code != 0) {
        // Every failure AND cancel path deletes the temp file.
        std::error_code ec2;
        std::filesystem::remove(st->worker_tmp, ec2);
    }
    ::PostMessageW(dlg, kHfMsgDone, static_cast<WPARAM>(code), 0);
}

// REQ-052: the fresh-registration identity shared by the post-download path
// and the orphan-file path (HfPreFlight) — the file-picker pipeline's id
// policy: case-insensitive ".gguf" tail strip for the display stem, then
// user-<stem> with "_2"/"_3"… suffixes on a registry id collision (the
// filename itself is fixed).
struct HfFreshIdentity {
    std::string stem;
    std::string model_id;
};

HfFreshIdentity HfDeriveFreshIdentity(engine_host_registry::Registry& registry,
                                      const std::string& filename) {
    const std::string kGgufExt = ".gguf";
    std::string stem = filename;
    if (stem.size() > kGgufExt.size()) {
        const std::string tail = stem.substr(stem.size() - kGgufExt.size());
        bool gguf_tail = true;
        for (size_t ci = 0; ci < kGgufExt.size(); ++ci) {
            if (std::tolower(static_cast<unsigned char>(tail[ci])) !=
                std::tolower(static_cast<unsigned char>(kGgufExt[ci]))) {
                gguf_tail = false;
                break;
            }
        }
        if (gguf_tail) {
            stem.resize(stem.size() - kGgufExt.size());
        }
    }
    HfFreshIdentity ident;
    ident.stem = std::move(stem);
    ident.model_id = "user-" + ident.stem;
    for (int suffix = 2;
         registry.FindModel(ident.model_id) != nullptr && suffix <= 1000;
         ++suffix) {
        ident.model_id = "user-" + ident.stem + "_" + std::to_string(suffix);
    }
    return ident;
}

// REQ-050: pre-download guards, run on the GUI thread BEFORE the worker
// starts so a multi-GB download is never wasted:
//   * LOCALAPPDATA / models dir (loud, reuses the manager-error strings)
//   * registry freshness (damaged -> loud refusal)
//   * filename already registered -> bundled refusal or user-entry reuse
//     (switch config to the existing entry; the file-picker pipeline's §4
//     semantics, so both add methods behave identically)
//   * REQ-052: filename present on disk but unregistered -> REGISTER the
//     existing complete file (origin "user") instead of dead-ending the
//     re-download with a bare failure status
enum class HfPreFlightResult { Proceed, Handled, Refused };

HfPreFlightResult HfPreFlight(HWND dlg, HfAddDialogState* st,
                              const std::string& filename) {
    const std::wstring title = I18n::Get(StringId::HfAddTitle);
    const std::filesystem::path models_dir = engine_host_registry::DefaultModelsDir();
    if (models_dir.empty()) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrNoLocalappdata).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        return HfPreFlightResult::Refused;
    }
    std::error_code ec;
    std::filesystem::create_directories(models_dir, ec);
    if (ec) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrWrite).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        DIAG_F("UI/HfAdd/008: models dir create_directories failed (ec=%d)\n",
               ec.value());
        return HfPreFlightResult::Refused;
    }
    auto res = engine_host_registry::LoadDefaultRegistry();
    if (res.status != engine_host_registry::LoadStatus::Ok &&
        res.status != engine_host_registry::LoadStatus::Missing) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrRegistryDamaged).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        DIAG_F("UI/HfAdd/009: registry load status=%d; refusing to download\n",
               static_cast<int>(res.status));
        return HfPreFlightResult::Refused;
    }
    if (res.status == engine_host_registry::LoadStatus::Missing) {
        res.registry.schema_version = engine_host_registry::kRegistrySchemaVersion;
    }
    for (const auto& m : res.registry.models) {
        if (m.files.empty() || m.files[0] != filename) {
            continue;
        }
        if (m.origin == "bundled") {
            // REQ-047 D2 guard, HF edition: the URL names the built-in model.
            ::MessageBoxW(dlg, I18n::Get(StringId::UserGgufBundledDuplicateBody).c_str(),
                          title.c_str(), MB_OK | MB_ICONINFORMATION);
            DIAG_F("UI/HfAdd/010: URL filename matches the bundled model; refused (id=%s)\n",
                   m.id.c_str());
            return HfPreFlightResult::Refused;
        }
        // Already registered -> switch the engine binding to the existing
        // entry and report the reuse (no download, no duplicate entry).
        st->config->SetUserModelId(m.id);
        st->config->SetEngineTypeName("user_gguf");
        st->engine->SetEngineType(EngineType::LocalLlama);
        // REQ-051 U-2: the engine name flips to the reused model's stem.
        st->engine->SetUserModelDisplayStem(ManagerModelStem(m.files[0]));
        st->config->SaveToFile();
        ::MessageBoxW(dlg, I18n::Get(StringId::UserGgufRegisteredBody).c_str(),
                      title.c_str(), MB_OK | MB_ICONINFORMATION);
        DIAG_F("UI/HfAdd/011: URL model already registered; reusing id=%s\n",
               m.id.c_str());
        st->registered = true;
        return HfPreFlightResult::Handled;
    }
    // REQ-052: orphan-file collision — a complete file with this name sits in
    // the models dir but is not registered. The pre-fix code refused with a
    // bare HfFailed ("다운로드 실패") even though nothing failed, dead-ending
    // re-downloads of a model the user already has. Register the EXISTING
    // file through the same origin:"user" path a completed download takes
    // (same id policy via HfDeriveFreshIdentity, same loud registry write,
    // same engine switch) and report an honest done status; the download
    // never starts.
    if (std::filesystem::exists(models_dir / ToUtf16(filename), ec)) {
        const HfFreshIdentity ident = HfDeriveFreshIdentity(res.registry, filename);
        engine_host_registry::ModelEntry entry;
        entry.id = ident.model_id;
        entry.family = "ggml-translate";
        entry.files = { filename };
        entry.origin = "user";
        res.registry.models.push_back(std::move(entry));
        if (!WriteRegistryLoud(dlg, res.registry)) {
            return HfPreFlightResult::Refused; // the guard already surfaced why
        }
        st->config->SetUserModelId(ident.model_id);
        st->config->SetEngineTypeName("user_gguf");
        st->engine->SetEngineType(EngineType::LocalLlama);
        // REQ-051 U-2: the engine name flips to the freshly derived stem.
        st->engine->SetUserModelDisplayStem(ident.stem);
        st->config->SaveToFile();
        if (HWND s = ::GetDlgItem(dlg, IDC_HF_STATUS)) {
            ::SetWindowTextW(s, I18n::Get(StringId::HfDone).c_str());
        }
        DIAG_F("UI/HfAdd/021: orphan file registered id=%s file=%s (origin=user)\n",
               ident.model_id.c_str(), filename.c_str());
        st->registered = true;
        return HfPreFlightResult::Handled;
    }
    return HfPreFlightResult::Proceed;
}

// REQ-050: post-download registration, ALWAYS on the GUI thread (called from
// the kHfMsgDone handler after the worker joined). Re-runs the reuse/bundle
// checks (the download took minutes — the registry may have changed), moves
// the temp to the final bare filename (refusing any collision), appends the
// origin:"user" entry and persists through WriteRegistryLoud, then switches
// config.user_model_id + engine_type + the runtime engine exactly like the
// file-picker pipeline (RegisterUserGgufModel §5).
bool HfRegisterDownloaded(HWND dlg, HfAddDialogState* st) {
    const std::wstring title = I18n::Get(StringId::HfAddTitle);
    auto discard_tmp = [&]() {
        std::error_code ec;
        std::filesystem::remove(st->worker_tmp, ec);
    };
    const std::filesystem::path models_dir = engine_host_registry::DefaultModelsDir();
    if (models_dir.empty()) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrNoLocalappdata).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        discard_tmp();
        return false;
    }
    const std::filesystem::path final_path = models_dir / ToUtf16(st->worker_filename);
    auto res = engine_host_registry::LoadDefaultRegistry();
    if (res.status != engine_host_registry::LoadStatus::Ok &&
        res.status != engine_host_registry::LoadStatus::Missing) {
        ::MessageBoxW(dlg, I18n::Get(StringId::GgufManagerErrRegistryDamaged).c_str(),
                      title.c_str(), MB_OK | MB_ICONERROR);
        DIAG_F("UI/HfAdd/013: registry load status=%d after download; registration refused\n",
               static_cast<int>(res.status));
        discard_tmp();
        return false;
    }
    if (res.status == engine_host_registry::LoadStatus::Missing) {
        res.registry.schema_version = engine_host_registry::kRegistrySchemaVersion;
    }
    for (const auto& m : res.registry.models) {
        if (m.files.empty() || m.files[0] != st->worker_filename) {
            continue;
        }
        discard_tmp(); // the bytes are already on disk under this entry
        if (m.origin == "bundled") {
            ::MessageBoxW(dlg, I18n::Get(StringId::UserGgufBundledDuplicateBody).c_str(),
                          title.c_str(), MB_OK | MB_ICONINFORMATION);
            DIAG_F("UI/HfAdd/014: downloaded file matches a bundled entry; refused (id=%s)\n",
                   m.id.c_str());
            return false;
        }
        st->config->SetUserModelId(m.id);
        st->config->SetEngineTypeName("user_gguf");
        st->engine->SetEngineType(EngineType::LocalLlama);
        // REQ-051 U-2: the engine name flips to the reused model's stem.
        st->engine->SetUserModelDisplayStem(ManagerModelStem(m.files[0]));
        st->config->SaveToFile();
        ::MessageBoxW(dlg, I18n::Get(StringId::UserGgufRegisteredBody).c_str(),
                      title.c_str(), MB_OK | MB_ICONINFORMATION);
        DIAG_F("UI/HfAdd/015: downloaded model already registered; reusing id=%s\n",
               m.id.c_str());
        st->registered = true;
        return true;
    }
    std::error_code ec;
    if (std::filesystem::exists(final_path, ec)) {
        discard_tmp();
        if (HWND s = ::GetDlgItem(dlg, IDC_HF_STATUS)) {
            ::SetWindowTextW(s, I18n::Get(StringId::HfFailed).c_str());
        }
        DIAG_F("UI/HfAdd/016: target file appeared during the download; refusing (file=%s)\n",
               st->worker_filename.c_str());
        return false;
    }
    // Move temp -> final. std::filesystem::rename refuses to overwrite an
    // existing destination on Windows, so a mid-download collision can never
    // clobber a file.
    std::error_code ec_move;
    std::filesystem::rename(st->worker_tmp, final_path, ec_move);
    if (ec_move) {
        ::MessageBoxW(dlg, I18n::Get(StringId::HfFailed).c_str(), title.c_str(),
                      MB_OK | MB_ICONWARNING);
        DIAG_F("UI/HfAdd/017: temp->final move failed (ec=%d)\n", ec_move.value());
        discard_tmp();
        return false;
    }
    // Fresh registration — the file-picker pipeline's id policy via the
    // shared helper: user-<stem>, "_2"/"_3"… suffixes on id collision (the
    // filename itself is fixed).
    const HfFreshIdentity ident = HfDeriveFreshIdentity(res.registry, st->worker_filename);
    engine_host_registry::ModelEntry entry;
    entry.id = ident.model_id;
    entry.family = "ggml-translate";
    entry.files = { st->worker_filename };
    entry.origin = "user";
    res.registry.models.push_back(std::move(entry));
    if (!WriteRegistryLoud(dlg, res.registry)) {
        // The file landed but the registry did not; it stays on disk exactly
        // like the file-picker pipeline's copy-then-write-failure case.
        return false;
    }
    st->config->SetUserModelId(ident.model_id);
    st->config->SetEngineTypeName("user_gguf");
    st->engine->SetEngineType(EngineType::LocalLlama);
    // REQ-051 U-2: the engine name flips to the freshly derived stem.
    st->engine->SetUserModelDisplayStem(ident.stem);
    st->config->SaveToFile();
    DIAG_F("UI/HfAdd/018: registered HF model id=%s file=%s (origin=user)\n",
           ident.model_id.c_str(), st->worker_filename.c_str());
    // REQ-052: the fresh path must flip `registered` too — ShowHfAddDialog
    // returns it and the caller gates the manager-list reload on it, so a
    // successful fresh add used to leave the list stale (both reuse paths
    // already set it).
    st->registered = true;
    return true;
}

// Re-enable the form after a failed download so the user can fix the URL
// (or cancel); the status line already carries the reason.
void HfResetForRetry(HWND dlg, HfAddDialogState* st) {
    (void)st;
    if (HWND e = ::GetDlgItem(dlg, IDC_HF_URL_EDIT)) {
        ::EnableWindow(e, TRUE);
    }
    if (HWND b = ::GetDlgItem(dlg, IDOK)) {
        ::EnableWindow(b, TRUE);
    }
}

INT_PTR CALLBACK HfAddProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<HfAddDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<HfAddDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (!::GetDlgItem(dlg, IDC_HF_URL_EDIT)) {
            DIAG_F("UI/HfAdd/005: IDC_HF_URL_EDIT control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_HF_STATUS)) {
            DIAG_F("UI/HfAdd/006: IDC_HF_STATUS control missing after dialog init\n");
        }
        // REQ-050 (2-2): the progress bar is created at runtime — the
        // TemplateBuilder only emits system-class ordinals and
        // "msctls_progress32" has none. Its rect is DIALOG UNITS and MUST be
        // mapped through MapDialogRect: CreateWindowExW takes pixels, and the
        // pre-fix raw-DLU numbers landed the bar on top of the URL edit.
        static bool s_comctl_initialized = false;
        if (!s_comctl_initialized) {
            INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
            ::InitCommonControlsEx(&icc);
            s_comctl_initialized = true;
        }
        RECT prc = kHfProgressRectDlu;
        ::MapDialogRect(dlg, &prc);
        HWND prog = ::CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            prc.left, prc.top, prc.right - prc.left, prc.bottom - prc.top,
            dlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HF_PROGRESS)),
            ::GetModuleHandleW(nullptr), nullptr);
        if (!prog) {
            DIAG_F("UI/HfAdd/007: progress bar creation failed (GLE %lu)\n",
                   ::GetLastError());
        } else {
            ::SendMessageW(prog, PBM_SETMARQUEE, TRUE, 50);
        }
        return TRUE;
    }
    case kHfMsgProgress: {
        // REQ-052: decode through the helper — the raw WORD->int cast turned
        // the (WORD)-1 marquee sentinel into 65535 ("(65535%)").
        const int percent = HfDecodeProgressPercent(wp);
        const auto bytes = static_cast<unsigned long long>(lp);
        if (HWND prog = ::GetDlgItem(dlg, IDC_HF_PROGRESS)) {
            if (percent < 0) {
                ::SendMessageW(prog, PBM_SETMARQUEE, TRUE, 50);
            } else {
                ::SendMessageW(prog, PBM_SETMARQUEE, FALSE, 0);
                ::SendMessageW(prog, PBM_SETRANGE32, 0, 100);
                ::SendMessageW(prog, PBM_SETPOS, static_cast<WPARAM>(percent), 0);
            }
        }
        std::wstring status = I18n::Get(StringId::HfDownloading);
        if (percent >= 0) {
            status += L" (" + std::to_wstring(percent) + L"%)";
        } else if (bytes > 0) {
            status += L" (" + std::to_wstring(bytes / (1024 * 1024)) + L" MB)";
        }
        if (HWND s = ::GetDlgItem(dlg, IDC_HF_STATUS)) {
            ::SetWindowTextW(s, status.c_str());
        }
        return TRUE;
    }
    case kHfMsgDone: {
        const int code = static_cast<int>(wp);
        HfJoinWorker(st); // join first: the worker is finished, this is cheap
        if (HWND prog = ::GetDlgItem(dlg, IDC_HF_PROGRESS)) {
            ::SendMessageW(prog, PBM_SETMARQUEE, FALSE, 0);
        }
        if (code == 0) {
            // Registration runs HERE, on the GUI thread — never on the
            // worker (it touches config, MessageBox and the registry).
            if (HfRegisterDownloaded(dlg, st)) {
                ::MessageBoxW(dlg, I18n::Get(StringId::HfDone).c_str(),
                              I18n::Get(StringId::HfAddTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                ::EndDialog(dlg, IDOK);
            } else {
                HfResetForRetry(dlg, st);
            }
        } else if (code == 2) {
            ::EndDialog(dlg, IDCANCEL); // user cancel: temp already deleted
        } else {
            if (HWND s = ::GetDlgItem(dlg, IDC_HF_STATUS)) {
                ::SetWindowTextW(s, I18n::Get(StringId::HfFailed).c_str());
            }
            HfResetForRetry(dlg, st);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: { // [다운로드] — validate + normalize + guard, then spawn the worker
            if (st->worker_running) {
                return TRUE; // a download is already in flight
            }
            const std::string url = ToUtf8(GetCtrlText(dlg, IDC_HF_URL_EDIT));
            // REQ-050 (2-1): the user pastes one of three HF shapes (resolve /
            // blob / model-page-with-show_file_info); only the canonical
            // resolve form may reach WinHTTP, so the download always uses the
            // NORMALIZED url, never the raw edit text.
            std::string resolve_url;
            std::string filename;
            const bool normalized = NormalizeHfUrl(url, &resolve_url);
            if (!normalized || !HfResolveFilename(resolve_url, &filename)) {
                if (HWND s = ::GetDlgItem(dlg, IDC_HF_STATUS)) {
                    ::SetWindowTextW(s, I18n::Get(StringId::HfInvalidUrl).c_str());
                }
                DIAG_F("UI/HfAdd/019: URL rejected (len=%zu normalized=%d)\n",
                       url.size(), normalized ? 1 : 0);
                return TRUE;
            }
            switch (HfPreFlight(dlg, st, filename)) {
            case HfPreFlightResult::Handled:
                ::EndDialog(dlg, IDOK); // reuse path registered + noticed
                return TRUE;
            case HfPreFlightResult::Refused:
                return TRUE; // the guard already surfaced why
            case HfPreFlightResult::Proceed:
                break;
            }
            st->cancel.store(false);
            st->worker_url = ToUtf16(resolve_url);
            st->worker_filename = std::move(filename);
            st->worker_tmp = engine_host_registry::DefaultModelsDir() /
                             (ToUtf16(st->worker_filename) + L".download");
            st->worker = std::thread(HfDownloadWorker, dlg, st);
            st->worker_running = true;
            // Freeze the form for the download duration.
            if (HWND e = ::GetDlgItem(dlg, IDC_HF_URL_EDIT)) {
                ::EnableWindow(e, FALSE);
            }
            if (HWND b = ::GetDlgItem(dlg, IDOK)) {
                ::EnableWindow(b, FALSE);
            }
            // Kick the marquee + status immediately.
            ::SendMessageW(dlg, kHfMsgProgress,
                           static_cast<WPARAM>(static_cast<WORD>(-1)), 0);
            return TRUE;
        }
        case IDCANCEL:
            if (st->worker_running) {
                // Ask the worker to stop, ABORT the in-flight I/O, and only
                // then wait for it. REQ-052: the pre-fix code joined directly,
                // so a worker blocked in WinHttpReadData sat until the 60s
                // receive timeout — a ~60s GUI freeze. WinHTTP documents
                // WinHttpCloseHandle from another thread as the abort for a
                // pending operation; the slot-clear protocol (HfClearSlot)
                // makes each close exactly-once, and the worker breaks out of
                // its loop on the very next failed call (the cancel poll sits
                // at the loop top, so a handle is never used after this
                // close). If the download actually finished in the meantime,
                // the completed temp is discarded — the user explicitly asked
                // to cancel.
                st->cancel.store(true);
                HINTERNET abort_request = nullptr;
                HINTERNET abort_connect = nullptr;
                {
                    std::lock_guard<std::mutex> lk(st->hf_handles_mutex);
                    abort_request = st->hf_request;
                    st->hf_request = nullptr;
                    abort_connect = st->hf_connect;
                    st->hf_connect = nullptr;
                }
                if (abort_request) {
                    ::WinHttpCloseHandle(abort_request);
                }
                if (abort_connect) {
                    ::WinHttpCloseHandle(abort_connect);
                }
                HfJoinWorker(st);
                // REQ-052: when the cancel click wins the race against
                // kHfMsgDone, the completed temp would otherwise linger as
                // <name>.download (the worker only deletes it on a
                // failure/cancel exit, and the done handler that consumes it
                // never runs).
                std::error_code cancel_ec;
                std::filesystem::remove(st->worker_tmp, cancel_ec);
            }
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
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
        if (!::GetDlgItem(dlg, IDC_MGR_ADD_FILE)) {
            DIAG_F("UI/GgufManager/015: IDC_MGR_ADD_FILE control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_MGR_ADD_HF)) {
            DIAG_F("UI/GgufManager/016: IDC_MGR_ADD_HF control missing after dialog init\n");
        }
        if (HWND b = ::GetDlgItem(dlg, IDC_MGR_ADD_FILE)) {
            // REQ-050: without the main.cpp pipeline callback the button has
            // nothing to run — disable rather than dead-click.
            ::EnableWindow(b, st->add_from_file ? TRUE : FALSE);
        }
        // REQ-048 R2-D: same loader discipline as RegisterUserGgufModel — a
        // damaged/schema-rejected registry is NEVER opened for editing; the
        // manager aborts loudly instead of offering a destructive no-op.
        if (!LoadManagerRegistry(dlg, st)) {
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        RebuildManagerList(dlg, st);
        // 260922_0001 A7: explicit sync even when RebuildManagerList returned
        // early on a missing IDC_MGR_LIST — SelectedUserIndex then reports -1,
        // so the pair stays disabled rather than dead-clickable.
        SyncManagerActionButtons(dlg, st);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MGR_ADD_FILE: {
            // REQ-050: [파일에서 추가…] runs the app's existing file-picker
            // registration pipeline (main.cpp's RegisterUserGgufModel, passed
            // in as this callback). Nested modal loops on the GUI thread are
            // safe; afterwards the on-disk registry may have a new/reused
            // entry, so reload + rebuild (the pipeline shows its own notices
            // and switches config itself when it registers).
            if (!st->add_from_file) return TRUE;
            st->add_from_file();
            st->registry_changed = true;
            if (!LoadManagerRegistry(dlg, st)) {
                ::EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            RebuildManagerList(dlg, st);
            return TRUE;
        }
        case IDC_MGR_ADD_HF: {
            // REQ-050: [Hugging Face에서 추가…] — paste-a-model-URL dialog
            // (resolve / blob / page-with-show_file_info auto-converted; see
            // NormalizeHfUrl); on a registration it returns true and the list
            // is rebuilt.
            if (ShowHfAddDialog(dlg, *st->config, *st->engine)) {
                st->registry_changed = true;
                if (!LoadManagerRegistry(dlg, st)) {
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                }
                RebuildManagerList(dlg, st);
            }
            return TRUE;
        }
        case IDC_MGR_LIST:
            // 260922_0001 A7: LBN_SELCHANGE (mouse click, arrow keys,
            // Ctrl+click deselection) drives the action-button pair. Other
            // list notifications (LBN_DBLCLK) have no handler — return FALSE.
            if (HIWORD(wp) == LBN_SELCHANGE) {
                SyncManagerActionButtons(dlg, st);
                return TRUE;
            }
            return FALSE;
        case IDC_MGR_RENAME: {
            const int idx = SelectedUserIndex(dlg, st);
            // 260922_0001 A7: defensive guard — the button is disabled unless
            // a row is selected, so this path is unreachable by click.
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
            // 260922_0001 A7: defensive guard — the button is disabled unless
            // a row is selected, so this path is unreachable by click.
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
                // REQ-051 U-2: the user-model route is gone — the display
                // stem must not linger on the Auto (pinned Hy-MT2) leg.
                st->engine->SetUserModelDisplayStem({});
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

// ---- REQ-050: Hugging Face resolve-URL gate (pure, headless-testable) -----

bool IsHfResolveUrl(std::string_view url) {
    // Scheme + host pinned, case-sensitive: a lookalike host or casing can
    // never reach WinHTTP (fail-closed).
    constexpr std::string_view kPrefix = "https://huggingface.co/";
    if (url.rfind(kPrefix, 0) != 0) {
        return false;
    }
    // Query/fragment tricks are rejected outright.
    if (url.find_first_of("?#") != std::string_view::npos) {
        return false;
    }
    const std::string_view rest = url.substr(kPrefix.size());
    const size_t resolve_pos = rest.find("/resolve/");
    if (resolve_pos == std::string_view::npos || resolve_pos == 0) {
        return false; // missing marker, or an empty repo segment
    }
    const std::string_view after =
        rest.substr(resolve_pos + std::string_view("/resolve/").size());
    const size_t slash = after.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return false; // a file path with a non-empty revision is required
    }
    const std::string_view path = after.substr(slash + 1);
    if (path.empty() || path.back() == '/') {
        return false; // empty file path / empty last segment
    }
    // Every remaining segment must be non-empty and free of dot-segments.
    size_t seg_start = 0;
    for (;;) {
        const size_t seg_end = path.find('/', seg_start);
        const std::string_view seg = path.substr(
            seg_start, seg_end == std::string_view::npos
                           ? std::string_view::npos
                           : seg_end - seg_start);
        if (seg.empty() || seg == "." || seg == "..") {
            return false;
        }
        if (seg_end == std::string_view::npos) {
            break;
        }
        seg_start = seg_end + 1;
    }
    return true;
}

namespace {

// REQ-050 (2-1): strict percent-decoder for the show_file_info query value.
// Only %HH triplets are decoded; every other byte passes through verbatim.
// Returns false on a malformed triplet so a partially decoded value can never
// reach the normalized URL (fail-closed, same house policy as the URL gate).
bool HfPercentDecode(std::string_view in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false; // truncated triplet
        }
        const int hi = hexval(in[i + 1]);
        const int lo = hexval(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false; // non-hex digits
        }
        out->push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

// REQ-050 (2-1): the segment rules shared by the auto-conversion branches.
// Mirrors IsHfResolveUrl's per-segment checks (non-empty, no "." / "..") and
// additionally bans '\' and control chars, which the canonical gate leaves to
// HfResolveFilename — the normalizer must never emit them in the first place.
// Bytes >= 0x80 pass through (percent-encoded UTF-8 filenames stay legal).
bool HfValidRepoRelPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return false;
    }
    size_t seg_start = 0;
    for (;;) {
        const size_t seg_end = path.find('/', seg_start);
        const std::string_view seg = path.substr(
            seg_start, seg_end == std::string_view::npos
                           ? std::string_view::npos
                           : seg_end - seg_start);
        if (seg.empty() || seg == "." || seg == "..") {
            return false;
        }
        for (const unsigned char c : seg) {
            if (c < 0x20 || c == 0x7F || c == '\\') {
                return false;
            }
        }
        if (seg_end == std::string_view::npos) {
            break;
        }
        seg_start = seg_end + 1;
    }
    return true;
}

} // namespace

bool NormalizeHfUrl(std::string_view url, std::string* out_resolve_url) {
    if (out_resolve_url) {
        out_resolve_url->clear();
    }
    if (!out_resolve_url || url.empty()) {
        return false; // null out-parameter is a safe no-op false
    }
    // (1) The canonical resolve URL passes through verbatim — IsHfResolveUrl
    //     stays the single authority on that shape (queries/fragments still
    //     rejected there).
    if (IsHfResolveUrl(url)) {
        *out_resolve_url = std::string(url);
        return true;
    }
    constexpr std::string_view kPrefix = "https://huggingface.co/";
    if (url.rfind(kPrefix, 0) != 0) {
        return false; // pinned host only (also rejects http, lookalikes)
    }
    if (url.find('#') != std::string_view::npos) {
        return false; // fragments are never accepted
    }
    const std::string_view rest = url.substr(kPrefix.size());
    const size_t q_pos = rest.find('?');
    if (q_pos == std::string_view::npos) {
        // (2) Blob link: <repo>/blob/<rev>/<file...> -> <repo>/resolve/<rev>/<file...>
        const std::string_view path = rest;
        const size_t blob_pos = path.find("/blob/");
        if (blob_pos == std::string_view::npos) {
            return false; // page URL without query / /tree/ path / bad resolve
        }
        const std::string_view repo = path.substr(0, blob_pos);
        const std::string_view tail = path.substr(blob_pos + std::string_view("/blob/").size());
        const size_t slash = tail.find('/');
        if (slash == std::string_view::npos || slash == 0) {
            return false; // a revision and a file path are both required
        }
        const std::string_view rev = tail.substr(0, slash);
        const std::string_view file = tail.substr(slash + 1);
        if (!HfValidRepoRelPath(repo) || !HfValidRepoRelPath(rev) ||
            !HfValidRepoRelPath(file)) {
            return false; // empty/dot segments, traversal, '\' or controls
        }
        if (repo.find("/resolve/") != std::string_view::npos ||
            repo.find("/tree/") != std::string_view::npos ||
            repo.find("/blob/") != std::string_view::npos) {
            return false; // reserved markers inside the repo path
        }
        std::string candidate = std::string(kPrefix) + std::string(repo) +
                                "/resolve/" + std::string(rev) + "/" +
                                std::string(file);
        if (!IsHfResolveUrl(candidate)) {
            return false; // belt and braces: canonical re-validation
        }
        *out_resolve_url = std::move(candidate);
        return true;
    }
    // (3) Model page URL: <repo>?show_file_info=<file> (percent-decoded) ->
    //     <repo>/resolve/main/<file>. The file is undeterminable from any
    //     other page shape, so those fail closed here (bare page, /tree/…).
    const std::string_view path = rest.substr(0, q_pos);
    const std::string_view query = rest.substr(q_pos + 1);
    if (!HfValidRepoRelPath(path)) {
        return false;
    }
    if (path.find("/resolve/") != std::string_view::npos ||
        path.find("/blob/") != std::string_view::npos ||
        path.find("/tree/") != std::string_view::npos) {
        return false; // reserved markers inside the repo path
    }
    if (query.empty()) {
        return false;
    }
    // Exactly one show_file_info parameter; ANY other parameter is rejected
    // outright (fail-closed; multiple show_file_info params are ambiguous).
    std::string_view raw_value;
    int seen = 0;
    size_t seg_start = 0;
    for (;;) {
        const size_t seg_end = query.find('&', seg_start);
        const std::string_view pair = query.substr(
            seg_start, seg_end == std::string_view::npos
                           ? std::string_view::npos
                           : seg_end - seg_start);
        constexpr std::string_view kKey = "show_file_info=";
        if (pair.rfind(kKey, 0) != 0) {
            return false; // unknown parameter (incl. a bare key without '=')
        }
        raw_value = pair.substr(kKey.size());
        if (++seen > 1) {
            return false; // multiple show_file_info params
        }
        if (seg_end == std::string_view::npos) {
            break;
        }
        seg_start = seg_end + 1;
    }
    // Raw value hygiene: '+', raw space and control bytes are ambiguous or
    // invalid inside a URL; a real HF page percent-encodes them instead.
    for (const unsigned char c : raw_value) {
        if (c <= 0x20 || c == 0x7F || c == '+') {
            return false;
        }
    }
    std::string file;
    if (!HfPercentDecode(raw_value, &file) || !HfValidRepoRelPath(file)) {
        return false; // malformed triplet or traversal after decoding
    }
    const std::string candidate = std::string(kPrefix) + std::string(path) +
                                  "/resolve/main/" + std::move(file);
    if (!IsHfResolveUrl(candidate)) {
        return false; // belt and braces: canonical re-validation
    }
    *out_resolve_url = candidate;
    return true;
}

// REQ-050: filename length ceiling. NTFS allows 255, but the models dir
// prefix + this name must stay comfortably inside the classic MAX_PATH for
// every consumer (worker, registry writer, installer), so the derived name
// is capped well below the filesystem limit.
inline constexpr std::size_t kHfMaxFilenameLen = 200;

bool HfResolveFilename(std::string_view url, std::string* out_filename) {
    if (out_filename) {
        out_filename->clear();
    }
    if (!out_filename || !IsHfResolveUrl(url)) {
        return false;
    }
    const size_t last_slash = url.find_last_of('/');
    const std::string_view last = url.substr(last_slash + 1);
    if (last.empty() || last == "." || last == ".." ||
        last.size() > kHfMaxFilenameLen) {
        return false;
    }
    // IsBareFilename covers separators / drive colon / control chars; Windows
    // filenames additionally forbid these.
    if (!engine_host_json::IsBareFilename(last)) {
        return false;
    }
    if (last.find_first_of("<>\"|?*") != std::string_view::npos) {
        return false;
    }
    *out_filename = std::string(last);
    return true;
}

// REQ-048 R2-D + REQ-050: production manager item-set emission, split out of
// ShowGgufModelManagerDialog so the unit suite serializes the exact bytes the
// dialog hands to DialogBoxIndirectParamW (BuildOpenAiTemplate precedent).
// itemCount must match the AddItem calls below. REQ-050 added the top-row
// add-method buttons ([파일에서 추가…] / [Hugging Face에서 추가…]) and shifted
// the list + action row down by 16 DLU (dialog 210x96 -> 242x112, widened so
// the longer add-button labels do not clip):
//   add-file + add-HF + list + empty placeholder + Rename + Delete +
//   Close(IDCANCEL) = 7.
void BuildGgufManagerTemplate(TemplateBuilder& tb) {
    const DWORD LBX = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                      WS_VSCROLL | LBS_NOTIFY;
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD LIST_CLS   = 0x0083;  // "LISTBOX"

    // REQ-050: top row — the two add methods.
    tb.AddItem(BTN, 8, 5, 110, 13, IDC_MGR_ADD_FILE, BTN_CLS,
               I18n::Get(StringId::GgufManagerAddFile));
    tb.AddItem(BTN, 124, 5, 110, 13, IDC_MGR_ADD_HF, BTN_CLS,
               I18n::Get(StringId::GgufManagerAddHf));
    // REQ-050: shifted 6 -> 22 DLU (below the add row).
    tb.AddItem(LBX, 8, 22, 226, 58, IDC_MGR_LIST, LIST_CLS, L"");
    tb.AddItem(LBL, 8, 22, 226, 58, IDC_MGR_EMPTY, STATIC_CLS,
               I18n::Get(StringId::GgufManagerEmpty));
    tb.AddItem(BTN, 8, 86, 60, 13, IDC_MGR_RENAME, BTN_CLS,
               I18n::Get(StringId::GgufManagerRename));
    tb.AddItem(BTN, 80, 86, 60, 13, IDC_MGR_DELETE, BTN_CLS,
               I18n::Get(StringId::GgufManagerDelete));
    // Close doubles as IDCANCEL so Esc dismisses the dialog (Win32 default).
    tb.AddItem(BTN, 172, 86, 62, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::GgufManagerClose));
}

bool ShowGgufModelManagerDialog(HWND parent, AppConfig& config,
                                TranslationManager& engine,
                                const std::function<void()>& add_from_file) {
    ManagerDialogState st;
    st.config = &config;
    st.engine = &engine;
    st.add_from_file = add_from_file;

    const std::wstring title = I18n::Get(StringId::GgufManagerTitle);
    TemplateBuilder tb;
    tb.Begin(title, 242, 112, /*itemCount=*/7);
    BuildGgufManagerTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, GgufManagerProc,
        reinterpret_cast<LPARAM>(&st));
    (void)rc; // IDCANCEL vs IDOK carries no extra meaning; registry_changed does
    return st.registry_changed;
}

// REQ-050: the Hugging Face add-dialog item set (split out for the unit
// suite, the BuildGgufManagerTemplate precedent). itemCount must match the
// AddItem calls below: URL label + URL edit + status static + Download(IDOK)
// + Cancel(IDCANCEL) = 5. The progress bar is NOT a template item (see the
// header note); the dialog proc creates msctls_progress32 at runtime.
//
// REQ-050 (user item 2-2): 256x88 -> 262x92. Every control now sits fully
// inside the client rect with >= 6 DLU margins on ALL sides (the old template
// only kept 8 on the right of a 240-wide edit inside 256 — and the runtime
// progress bar, created from raw DLU-as-pixel numbers, landed across the URL
// edit; see kHfProgressRectDlu). y-order: label, edit, progress (runtime,
// y 35..47), status, buttons.
void BuildHfAddTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD EDIT_CLS   = 0x0081;  // "EDIT"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"

    tb.AddItem(LBL, 8, 6, 140, 9, IDC_HF_URL_LABEL, STATIC_CLS,
               I18n::Get(StringId::HfUrlLabel));
    // REQ-050 (single-line edit limits): a single-line EDIT without
    // ES_AUTOHSCROLL rejects any input beyond its visible width (device-
    // confirmed: a 110-char model URL was truncated at ~76 chars on paste),
    // so the URL edit must scroll horizontally.
    tb.AddItem(EDT | ES_AUTOHSCROLL, 8, 17, 246, 12, IDC_HF_URL_EDIT, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 52, 246, 9, IDC_HF_STATUS, STATIC_CLS, L"");
    // [다운로드] doubles as IDOK (Enter in the edit starts the download);
    // the label reuses DialogOk — the action-button half of the OK/Cancel
    // pair — per the REQ-050 string budget (cancel reuses DialogCancel).
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 118, 68, 64, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 190, 68, 64, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

bool ShowHfAddDialog(HWND parent, AppConfig& config, TranslationManager& engine) {
    HfAddDialogState st;
    st.config = &config;
    st.engine = &engine;

    const std::wstring title = I18n::Get(StringId::HfAddTitle);
    TemplateBuilder tb;
    tb.Begin(title, 262, 92, /*itemCount=*/5);
    BuildHfAddTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, HfAddProc,
        reinterpret_cast<LPARAM>(&st));
    (void)rc; // registered carries the meaning
    // The worker is joined on every exit path (cancel + done handlers)
    // BEFORE EndDialog, so no thread can outlive this stack state.
    return st.registered;
}

} // namespace emebalachat
