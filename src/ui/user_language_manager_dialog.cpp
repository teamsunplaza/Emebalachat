// 261116_0003 P9b-3 U4b (language-menu redesign, plan §5 P9): the user-language
// MANAGER dialog. See user_language_manager_dialog.hpp for the contract; this
// TU holds the pure folds (SECTION A, headless-tested) and the only
// window-creating code (SECTION B, linked but never entered by the suite).

#include "user_language_manager_dialog.hpp"

#include "../diag_logger.hpp"
#include "../unicode_utils.hpp"
#include "settings_hub_window.hpp"   // V10F UX-4: PlanUserLanguageRemoval (shared guard)
#include "user_language_dialog.hpp"  // V10F UX-4: the [추가…] entry point main.cpp uses

#include <string>
#include <utility>
#include <vector>

namespace emebalachat {
namespace user_language_manager_dialog {

// ---- SECTION A: the pure folds -------------------------------------------

UserLanguageManagerRow FoldUserLanguageManagerRow(const UserLanguage& entry,
                                                  LanguageEngine engine,
                                                  std::string_view model,
                                                  bool in_flight,
                                                  bool writes_allowed) {
    UserLanguageManagerRow row;
    // The STORED name is the only identifier a row surfaces: it is the locked
    // verbs' key, the menu dispatch payload, and a string the storage path has
    // already sanitized (PR9-03 strip set + byte cap at write time). The native
    // spelling never reaches the list.
    row.stored_name = UserLanguageStoredName(entry);
    row.label = ToUtf16(row.stored_name);
    row.engine_judges = UserLanguageProbeEngineJudgedByEvidence(engine);
    row.in_flight = in_flight && row.engine_judges;

    const UserLanguageProbe* probe = nullptr;
    if (row.engine_judges) {
        // Same key construction the runner writes with (U3 StartProbe): the
        // engine id from UserLanguageEngineId + the caller's model half. A
        // different model here reads a DIFFERENT slot on purpose (D6 caches per
        // engine + model).
        probe = FindUserLanguageProbe(entry, UserLanguageEngineId(engine), model);
        row.override_active =
            probe != nullptr &&
            (probe->override_status == UserLanguageProbeStatus::Supported ||
             probe->override_status == UserLanguageProbeStatus::Unsupported);
    }

    // The badge cell. D6's five display states, all pinned:
    //   in flight        -> 확인 중 (runtime-only; never read from the slot)
    //   Supported        -> ✓ 지원
    //   Unsupported      -> ✗ 미지원 의심
    //   UserOverridden   -> ✓/✗ by the override direction + 직접 설정함
    //   Unknown / no probe engine -> nothing (nothing has been judged yet)
    if (row.engine_judges) {
        if (row.in_flight) {
            row.badge_shown = true;
            row.badge_id = StringId::UserLangProbeChecking;
            row.badge_text = I18n::Get(row.badge_id);
        } else if (probe != nullptr) {
            switch (DisplayUserLanguageProbeStatus(*probe)) {
            case UserLanguageProbeStatus::Supported:
                row.badge_shown = true;
                row.badge_id = StringId::UserLangProbeSupported;
                row.badge_text =
                    std::wstring(kBadgeSupportedPrefix) + I18n::Get(row.badge_id);
                break;
            case UserLanguageProbeStatus::Unsupported:
                row.badge_shown = true;
                row.badge_id = StringId::UserLangProbeUnsupported;
                row.badge_text =
                    std::wstring(kBadgeUnsupportedPrefix) + I18n::Get(row.badge_id);
                break;
            case UserLanguageProbeStatus::Checking:
                // A persisted slot can still READ Checking (only the load path
                // maps it to Unknown, config.cpp ParseProbeStatusToken); render
                // it exactly like the runtime state rather than inventing a
                // sixth look.
                row.badge_shown = true;
                row.badge_id = StringId::UserLangProbeChecking;
                row.badge_text = I18n::Get(row.badge_id);
                break;
            case UserLanguageProbeStatus::UserOverridden: {
                // DisplayUserLanguageProbeStatus returns UserOverridden only
                // when override_status is a real answer (config.cpp:2539-2548),
                // so the direction is always one of the two override buttons.
                const bool supported =
                    probe->override_status == UserLanguageProbeStatus::Supported;
                row.override_active = true;
                row.badge_shown = true;
                row.badge_id = supported ? StringId::UserLangProbeSupported
                                         : StringId::UserLangProbeUnsupported;
                row.badge_text = std::wstring(supported ? kBadgeSupportedPrefix
                                                       : kBadgeUnsupportedPrefix) +
                                 I18n::Get(row.badge_id) + kBadgeMarkerSeparator +
                                 I18n::Get(StringId::UserLangOverrideActive);
                break;
            }
            case UserLanguageProbeStatus::Unknown:
            default:
                // Never probed: no badge (the only state with no copy).
                break;
            }
        }
    }

    // The evidence line, VERBATIM: the P9a evidence grammar
    // ("nonempty=1 differs=1 script=1 expected=latin detected=latin") is cached
    // per (engine, model) and compared across runs, so this dialog wraps it in
    // the localized label and never re-words or truncates it.
    if (probe != nullptr && !probe->evidence.empty()) {
        row.evidence = ToUtf16(probe->evidence);
        row.evidence_line = I18n::Get(StringId::UserLangProbeEvidenceLabel) +
                            kEvidenceSeparator + row.evidence;
    }

    row.list_text = row.badge_text.empty()
                        ? row.label
                        : row.label + kListBadgeSeparator + row.badge_text;

    // Per-action gates. `writes_allowed` is the PR9-12 carrier gate: while a
    // newer-schema block is preserved every verb refuses, so the dialog must not
    // offer an edit that would be discarded on save (U2's read-only opening).
    // Retest is a write path too (it records through the locked verb), so it
    // carries the same gate.
    row.retest_enabled = row.engine_judges && writes_allowed && !row.in_flight;
    row.override_supported_enabled = row.engine_judges && writes_allowed;
    row.override_unsupported_enabled = row.override_supported_enabled;
    row.clear_override_enabled =
        row.engine_judges && writes_allowed && row.override_active;
    return row;
}

std::vector<UserLanguageManagerRow> FoldUserLanguageManagerRows(
    const std::vector<UserLanguage>& entries,
    LanguageEngine engine,
    std::string_view model,
    const UserLanguageProbeService* service,
    bool writes_allowed) {
    std::vector<UserLanguageManagerRow> rows;
    rows.reserve(entries.size());
    const std::string engine_id = UserLanguageEngineId(engine);
    for (const UserLanguage& entry : entries) {
        // IsInFlight is the runtime half of the fold (Checking is never read
        // from the persisted slot). A null service is the headless shape: every
        // row is simply not in flight.
        std::string stored_name = UserLanguageStoredName(entry);
        const bool in_flight =
            service != nullptr && service->IsInFlight(stored_name, engine_id, model);
        rows.push_back(
            FoldUserLanguageManagerRow(entry, engine, model, in_flight, writes_allowed));
    }
    return rows;
}

std::string ResolveUserLanguageManagerModel(LanguageEngine engine,
                                            const AppConfig& config,
                                            std::wstring_view model) {
    std::string resolved = ToUtf8(model);
    if (!resolved.empty()) {
        return resolved;
    }
    // The runner builds its key from the config (UserLanguageProbeModelFor), so
    // a caller that passed an empty model half would otherwise address a
    // different slot than the one the verdict lands in - every badge would read
    // "not checked". Deriving it here keeps the bare call correct.
    return UserLanguageProbeModelFor(engine, config);
}

UserLanguageManagerActionResult FoldUserLanguageManagerVerbResult(
    UserLanguageWriteResult result) {
    UserLanguageManagerActionResult action;
    if (result == UserLanguageWriteResult::Applied) {
        // The ONLY changing path: the locked verb stored the entry's override /
        // cleared it under mutex_, so the caller has something to persist.
        action.changed = true;
        return action;
    }
    // RefusedForwardCompatCarrier / RejectedNotFound / RejectedMissingProbeKey /
    // RejectedInvalidStatus / RejectedCapacity / ... : the config is untouched,
    // so the dialog stays open and reports no change (never a success it did
    // not get - U2's FoldUserLanguageAddResult discipline).
    action.refused = true;
    return action;
}

UserLanguageManagerActionResult FoldUserLanguageManagerRetest(bool launched) {
    UserLanguageManagerActionResult action;
    // A launched retest changes nothing NOW: U3 records the verdict later, on
    // the GUI thread, and its recorded callback persists. Treating the launch as
    // kChanged would make the caller write a file for a verdict it does not have
    // yet.
    action.refused = !launched;
    return action;
}

UserLanguageManagerOutcome FoldUserLanguageManagerOutcome(bool any_applied) {
    return any_applied ? UserLanguageManagerOutcome::kChanged
                       : UserLanguageManagerOutcome::kClosed;
}

// ---- SECTION B: the window ----------------------------------------------

namespace {

// V10F UX-4 (261122_0002 P3-C3): the CRUD pair. The template stays BYTE-
// IDENTICAL (the p9b3 suite walks it row by row), so both buttons are created
// at runtime in WM_INITDIALOG and placed through MapDialogRect (the gguf
// manager's duplicate-warning / progress-bar precedent). The ids sit past the
// header's control run (4700-4707) and the WM_TIMER id (4710), so no control
// and no timer can ever alias.
enum : WORD {
    kIdAdd = 4711,    // "파일에서 추가…" copy (GgufManagerAddFile) -> U2's dialog
    kIdDelete = 4712, // "삭제…" copy (GgufManagerDelete) -> RemoveUserLanguage
};
// DLU rects for the runtime row, left of the template's [닫기] button.
inline constexpr RECT kAddButtonRectDlu{8, 278, 98, 292};
inline constexpr RECT kDeleteButtonRectDlu{104, 278, 194, 292};

struct ManagerDialogState {
    AppConfig* config = nullptr;
    LanguageEngine engine = LanguageEngine::Auto;
    std::string engine_id; // UserLanguageEngineId(engine); "" = no probe row (D7)
    std::string model;     // the UTF-8 model half of the probe-cache key
    UserLanguageProbeService* service = nullptr;
    std::vector<UserLanguageManagerRow> rows;
    std::string selected_name; // survives a refresh (rows are rebuilt)
    bool any_applied = false;  // any locked verb APPLIED (-> kChanged)
    bool writes_allowed = true;
    UINT_PTR timer = 0;
};

void SetCtrlText(HWND dlg, int id, std::wstring_view text) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        const std::wstring value(text);
        ::SetWindowTextW(c, value.c_str());
    }
}

void SetCtrlEnabled(HWND dlg, int id, bool enabled) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        ::EnableWindow(c, enabled ? TRUE : FALSE);
    }
}

// V10F UX-4: one runtime push button in a DLU rect (MapDialogRect first -
// CreateWindowExW takes pixels) that adopts the dialog font.
HWND EnsureRuntimeButton(HWND dlg, WORD id, const RECT& dlu_rect,
                         std::wstring_view caption) {
    if (HWND existing = ::GetDlgItem(dlg, id)) {
        return existing;
    }
    RECT rc = dlu_rect;
    ::MapDialogRect(dlg, &rc);
    HWND button = ::CreateWindowExW(
        0, L"BUTTON", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.left,
        rc.top, rc.right - rc.left, rc.bottom - rc.top, dlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (button == nullptr) {
        return nullptr;
    }
    if (HFONT font = reinterpret_cast<HFONT>(
            ::SendMessageW(dlg, WM_GETFONT, 0, 0))) {
        ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    const std::wstring text(caption);
    ::SetWindowTextW(button, text.c_str());
    return button;
}

// V10F UX-4: the [추가…] / [삭제] pair. The captions are the landed gguf-manager
// add/delete labels (no new StringId): adding reuses U2's dialog, deleting reuses
// AppConfig::RemoveUserLanguage.
void CreateCrudButtons(HWND dlg) {
    HWND add = EnsureRuntimeButton(dlg, kIdAdd, kAddButtonRectDlu,
                                   I18n::Get(StringId::GgufManagerAddFile));
    HWND del = EnsureRuntimeButton(dlg, kIdDelete, kDeleteButtonRectDlu,
                                   I18n::Get(StringId::GgufManagerDelete));
    if (add == nullptr || del == nullptr) {
        DIAG_F("UI/UserLanguageManager/012: CRUD button creation failed "
               "(add=%p delete=%p)\n",
               static_cast<void*>(add), static_cast<void*>(del));
    }
}

// The selected row (index through LB_SETITEMDATA, so a sort/refresh can never
// desynchronise the list from the vector - the gguf manager's SelectedUserIndex
// idiom). nullptr when nothing is selected.
const UserLanguageManagerRow* SelectedRow(HWND dlg, const ManagerDialogState* st) {
    HWND list = ::GetDlgItem(dlg, kIdList);
    if (!list) return nullptr;
    const int sel = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (sel < 0) return nullptr;
    const LPARAM data =
        ::SendMessageW(list, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
    if (data < 0 || static_cast<std::size_t>(data) >= st->rows.size()) {
        return nullptr;
    }
    return &st->rows[static_cast<std::size_t>(data)];
}

// Paints the detail area + the action gates from the current selection. Called
// after every rebuild and on LBN_SELCHANGE, so the buttons are never enabled for
// a row the user is not looking at.
void SyncSelection(HWND dlg, ManagerDialogState* st) {
    const UserLanguageManagerRow* row = SelectedRow(dlg, st);
    SetCtrlText(dlg, kIdBadge, row ? row->badge_text : std::wstring());
    SetCtrlText(dlg, kIdEvidence, row ? row->evidence_line : std::wstring());
    SetCtrlText(dlg, kIdEvidenceLabel,
                I18n::Get(StringId::UserLangProbeEvidenceLabel));

    // 다시 시험 needs a runner: the service is the only caller of the engine.
    SetCtrlEnabled(dlg, kIdRetest, row != nullptr && st->service != nullptr &&
                                       row->retest_enabled);
    SetCtrlEnabled(dlg, kIdOverrideSupported,
                   row != nullptr && row->override_supported_enabled);
    SetCtrlEnabled(dlg, kIdOverrideUnsupported,
                   row != nullptr && row->override_unsupported_enabled);
    // The marker checkbox carries the UserLangOverrideActive copy AND the clear
    // affordance: it reads checked exactly while an override is set, and it is
    // clickable only then (unchecking = ClearUserLanguageProbeOverrideFor).
    // CheckDlgButton keeps the visual state mirroring the CONFIG, never the
    // click, so a refused clear springs back instead of lying.
    if (HWND marker = ::GetDlgItem(dlg, kIdOverrideMarker)) {
        ::CheckDlgButton(dlg, kIdOverrideMarker,
                         (row != nullptr && row->override_active) ? BST_CHECKED
                                                                  : BST_UNCHECKED);
        ::EnableWindow(marker, (row != nullptr && row->clear_override_enabled) ? TRUE
                                                                             : FALSE);
    }
    // V10F UX-4: [추가…] follows the same carrier write gate as every verb, and
    // [삭제] additionally needs a resolvable selection (PlanUserLanguageRemoval
    // is the ONE guard, so the button can never delete a row the user is not
    // looking at).
    SetCtrlEnabled(dlg, kIdAdd, st->writes_allowed);
    bool delete_enabled = false;
    if (st->config != nullptr) {
        const settings_hub::UserLanguageRemovalPlan removal =
            settings_hub::PlanUserLanguageRemoval(
                st->config->user_languages,
                row ? row->stored_name : std::string(), st->writes_allowed);
        delete_enabled = removal.enabled;
    }
    SetCtrlEnabled(dlg, kIdDelete, delete_enabled);
    st->selected_name = row ? row->stored_name : std::string();
}

// Rebuilds the list from the LIVE config (GUI-thread read, config.hpp:999-1003),
// then re-selects the previously selected entry by STORED NAME - indices shift
// when an entry is removed elsewhere, names do not.
void RefreshRows(HWND dlg, ManagerDialogState* st) {
    if (st->config == nullptr) return;
    st->writes_allowed = !st->config->HasUserLanguagesForwardCompatCarrier();
    st->rows = FoldUserLanguageManagerRows(st->config->user_languages, st->engine,
                                           st->model, st->service,
                                           st->writes_allowed);

    HWND list = ::GetDlgItem(dlg, kIdList);
    if (!list) return;
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int reselect = -1;
    for (std::size_t i = 0; i < st->rows.size(); ++i) {
        const std::wstring& text = st->rows[i].list_text;
        const int item = static_cast<int>(
            ::SendMessageW(list, LB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(text.c_str())));
        ::SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(item),
                       static_cast<LPARAM>(i));
        if (!st->selected_name.empty() &&
            st->rows[i].stored_name == st->selected_name) {
            reselect = static_cast<int>(i);
        }
    }
    if (reselect < 0 && !st->rows.empty()) {
        reselect = 0; // a refresh never leaves the dialog without a selection
    }
    if (reselect >= 0) {
        // LB_SETCURSEL is silent (no LBN_SELCHANGE), so SyncSelection below is
        // the only thing that repaints the detail area after a rebuild.
        ::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(reselect), 0);
    }
    SyncSelection(dlg, st);
}

void StopInFlightPoll(HWND dlg, ManagerDialogState* st) {
    if (st->timer != 0) {
        ::KillTimer(dlg, st->timer);
        st->timer = 0;
    }
}

// Starts (or keeps) the in-flight poll. See the header's recorded-callback note:
// the runner's callback slot belongs to the caller (persist + its own surfaces),
// so this dialog observes the completion instead of registering there.
void StartInFlightPoll(HWND dlg, ManagerDialogState* st) {
    if (st->service == nullptr || st->timer != 0) return;
    st->timer = ::SetTimer(dlg, kInFlightTimerId, kInFlightPollMs, nullptr);
    if (st->timer == 0) {
        DIAG_F("UI/UserLanguageManager/001: in-flight timer unavailable\n");
    }
}

// One poll tick: drain (idempotent - the controller's posted wake-up may already
// have drained the same outcome) so the verdict + the owner's persist callback
// run, repaint whenever anything drained, and stop polling once nothing this
// dialog shows is in flight (V10F RISK-04).
void PollInFlight(HWND dlg, ManagerDialogState* st) {
    if (st->service == nullptr) {
        StopInFlightPoll(dlg, st);
        return;
    }
    const std::size_t drained = st->service->DrainOutcomes();
    bool any_in_flight = false;
    for (const UserLanguageManagerRow& row : st->rows) {
        if (st->service->IsInFlight(row.stored_name, st->engine_id, st->model)) {
            any_in_flight = true;
            break;
        }
    }
    if (drained > 0) {
        // Shape-only: counts and statuses, never a name or a model string.
        DIAG_F("UI/UserLanguageManager/002: probe completions drained=%zu\n", drained);
    }
    // V10F RISK-04 (261122_0002 P2-G): refresh the moment ANY outcome drained
    // (a finished probe's row must flip to its verdict while the other rows
    // are still checking — the old code left it showing 확인 중 until ALL of
    // them finished), and stop the poll timer as soon as nothing is in flight.
    if (!any_in_flight) {
        StopInFlightPoll(dlg, st);
    }
    if (drained > 0) {
        RefreshRows(dlg, st);
    }
}

// One override-direction application through U1's LOCKED verb. Returns nothing:
// the caller only needs the state flag, and the row refresh repaints from the
// config (a refusal therefore shows as "nothing happened", never as a lie).
void ApplyOverrideDirection(HWND dlg, ManagerDialogState* st,
                            std::string stored_name, bool supported) {
    // `stored_name` is passed BY VALUE: RefreshRows below replaces st->rows, so a
    // reference to a row would dangle the moment the verb returned.
    const UserLanguageWriteResult result =
        st->config->RecordUserLanguageProbeOverrideFor(
            stored_name, st->engine_id, st->model,
            supported ? UserLanguageProbeStatus::Supported
                      : UserLanguageProbeStatus::Unsupported,
            UserLanguageProbeTimestampNowUtc());
    const UserLanguageManagerActionResult action =
        FoldUserLanguageManagerVerbResult(result);
    if (action.changed) {
        st->any_applied = true;
    }
    // Shape-only: the write result only (no name, no model).
    DIAG_F("UI/UserLanguageManager/003: override direction=%d write=%u\n",
           supported ? 1 : 0, static_cast<unsigned>(result));
    RefreshRows(dlg, st);
}

INT_PTR CALLBACK UserLanguageManagerDialogProc(HWND dlg, UINT msg, WPARAM wp,
                                               LPARAM lp) {
    auto* st = reinterpret_cast<ManagerDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<ManagerDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // Control-creation verification (the openai/engine-host/U2 dialogs'
        // REQ-047 D3 contract): a control that failed to materialize must be
        // separable from a logic bug in a support log.
        if (!::GetDlgItem(dlg, kIdList)) {
            DIAG_F("UI/UserLanguageManager/004: list missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, kIdBadge) || !::GetDlgItem(dlg, kIdEvidence)) {
            DIAG_F("UI/UserLanguageManager/005: detail rows missing after dialog init\n");
        }
        // V10F UX-4: the runtime CRUD row (the template stays byte-identical).
        CreateCrudButtons(dlg);
        RefreshRows(dlg, st);
        if (!st->writes_allowed) {
            // PR9-12 carrier armed: every verb would refuse, so the actions are
            // already greyed by the fold; this line makes the CAUSE greppable.
            DIAG_F("UI/UserLanguageManager/006: forward-compat carrier armed; "
                   "actions disabled\n");
        }
        if (st->service != nullptr) {
            // A probe may already be running when the dialog opens (a first-use
            // trigger fired just before the tray row was picked); poll it so the
            // row shows 확인 중 and flips when the verdict lands.
            for (const UserLanguageManagerRow& row : st->rows) {
                if (st->service->IsInFlight(row.stored_name, st->engine_id,
                                            st->model)) {
                    StartInFlightPoll(dlg, st);
                    break;
                }
            }
        }
        return FALSE; // no control needs the initial focus beyond the default
    }
    case WM_COMMAND: {
        const WORD code = HIWORD(wp); // BN_CLICKED / LBN_SELCHANGE / ...
        switch (LOWORD(wp)) {
        case kIdList:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                SyncSelection(dlg, st);
                return TRUE;
            }
            return FALSE;
        case kIdRetest: {
            const UserLanguageManagerRow* row = SelectedRow(dlg, st);
            if (row == nullptr || st->service == nullptr || !row->retest_enabled) {
                return TRUE;
            }
            // Copy the key BY VALUE: RefreshRows replaces st->rows, which would
            // dangle `row`.
            const std::string stored_name = row->stored_name;
            const bool launched = st->service->RequestRetest(st->engine, stored_name);
            const UserLanguageManagerActionResult action =
                FoldUserLanguageManagerRetest(launched);
            DIAG_F("UI/UserLanguageManager/007: retest launched=%d refused=%d\n",
                   launched ? 1 : 0, action.refused ? 1 : 0);
            if (launched) {
                StartInFlightPoll(dlg, st);
                RefreshRows(dlg, st); // repaints the row as 확인 중
            }
            return TRUE;
        }
        case kIdOverrideSupported:
        case kIdOverrideUnsupported: {
            const UserLanguageManagerRow* row = SelectedRow(dlg, st);
            if (row == nullptr) return TRUE;
            const bool supported = LOWORD(wp) == kIdOverrideSupported;
            const bool allowed = supported ? row->override_supported_enabled
                                           : row->override_unsupported_enabled;
            if (!allowed) return TRUE;
            ApplyOverrideDirection(dlg, st, row->stored_name, supported);
            return TRUE;
        }
        case kIdOverrideMarker: {
            const UserLanguageManagerRow* row = SelectedRow(dlg, st);
            if (row == nullptr || !row->clear_override_enabled) {
                // Not a clearable state: re-sync the visual state from the config
                // (a BS_AUTOCHECKBOX already toggled itself).
                SyncSelection(dlg, st);
                return TRUE;
            }
            const std::string stored_name = row->stored_name;
            const UserLanguageWriteResult result =
                st->config->ClearUserLanguageProbeOverrideFor(
                    stored_name, st->engine_id, st->model);
            const UserLanguageManagerActionResult action =
                FoldUserLanguageManagerVerbResult(result);
            if (action.changed) {
                st->any_applied = true;
            }
            DIAG_F("UI/UserLanguageManager/008: clear override write=%u\n",
                   static_cast<unsigned>(result));
            RefreshRows(dlg, st);
            return TRUE;
        }
        case kIdAdd: {
            // V10F UX-4: [추가…] runs the SAME entry point the tray row uses
            // (U2's ShowUserLanguageDialogEx), on THIS dialog as the owner. U2
            // writes in memory through the locked mutator and performs no file
            // I/O, so the kChanged outcome is what makes the caller persist.
            if (code != BN_CLICKED || !st->writes_allowed || st->config == nullptr) {
                return TRUE;
            }
            const UserLanguageDialogOutcome outcome =
                user_language_dialog::ShowUserLanguageDialogEx(dlg, *st->config);
            DIAG_F("UI/UserLanguageManager/013: add dialog outcome=%d\n",
                   static_cast<int>(outcome));
            if (outcome == UserLanguageDialogOutcome::kAdded) {
                st->any_applied = true; // -> kChanged: the caller persists
                RefreshRows(dlg, st);   // the new row appears immediately
            }
            return TRUE;
        }
        case kIdDelete: {
            // V10F UX-4: [삭제] through U1's locked verb. The guard resolves the
            // SELECTED row to its index (the rows and the config vector share one
            // order), refuses while the carrier is armed, and never trusts a stale
            // selection after a refusal.
            if (code != BN_CLICKED || st->config == nullptr) {
                return TRUE;
            }
            const UserLanguageManagerRow* row = SelectedRow(dlg, st);
            if (row == nullptr) {
                SyncSelection(dlg, st);
                return TRUE;
            }
            // Copy the key BY VALUE: RefreshRows replaces st->rows, which would
            // dangle `row`.
            const std::string stored_name = row->stored_name;
            const settings_hub::UserLanguageRemovalPlan plan =
                settings_hub::PlanUserLanguageRemoval(st->config->user_languages,
                                                      stored_name,
                                                      st->writes_allowed);
            if (!plan.enabled) {
                DIAG_F("UI/UserLanguageManager/014: delete refused "
                       "(carrier=%d not_found=%d)\n",
                       plan.carrier_blocks ? 1 : 0, plan.not_found ? 1 : 0);
                SyncSelection(dlg, st);
                return TRUE;
            }
            const UserLanguageWriteResult result =
                st->config->RemoveUserLanguage(plan.index);
            const UserLanguageManagerActionResult action =
                FoldUserLanguageManagerVerbResult(result);
            if (action.changed) {
                st->any_applied = true;
            }
            DIAG_F("UI/UserLanguageManager/015: delete write=%u\n",
                   static_cast<unsigned>(result));
            // The removed name can never be re-selected: fall back to row 0.
            st->selected_name.clear();
            RefreshRows(dlg, st);
            return TRUE;
        }
        case IDCANCEL:
            StopInFlightPoll(dlg, st);
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
        return FALSE;
    }
    case WM_TIMER:
        if (st != nullptr && wp == kInFlightTimerId) {
            PollInFlight(dlg, st);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        if (st != nullptr) {
            StopInFlightPoll(dlg, st);
        }
        ::EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // namespace

void BuildUserLanguageManagerTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    // LBS_NOTIFY is what delivers LBN_SELCHANGE (the only way the detail area
    // learns the selection changed); WS_VSCROLL keeps a 64-entry registry usable.
    const DWORD LIST =
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    const DWORD CHK = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
    const WORD STATIC_CLS = 0x0082; // "STATIC"
    const WORD BTN_CLS = 0x0080;    // "BUTTON"
    const WORD LIST_CLS = 0x0083;   // "LISTBOX"

    // The list carries the label + the inline badge; the detail block below it
    // repeats the selected row's full badge and its evidence (the evidence string
    // is ~60 ASCII columns - too wide to sit inside a row next to the label).
    tb.AddItem(LIST, 8, 6, 404, 170, kIdList, LIST_CLS, L"");
    tb.AddItem(LBL, 8, 180, 404, 10, kIdBadge, STATIC_CLS, L"");
    tb.AddItem(LBL, 8, 194, 64, 9, kIdEvidenceLabel, STATIC_CLS,
               I18n::Get(StringId::UserLangProbeEvidenceLabel));
    // Two text lines tall so the evidence wraps instead of clipping.
    tb.AddItem(LBL, 76, 194, 336, 22, kIdEvidence, STATIC_CLS, L"");
    tb.AddItem(BTN, 8, 222, 92, 14, kIdRetest, BTN_CLS,
               I18n::Get(StringId::UserLangProbeRetest));
    tb.AddItem(BTN, 104, 222, 132, 14, kIdOverrideSupported, BTN_CLS,
               I18n::Get(StringId::UserLangOverrideSupported));
    tb.AddItem(BTN, 240, 222, 132, 14, kIdOverrideUnsupported, BTN_CLS,
               I18n::Get(StringId::UserLangOverrideUnsupported));
    // The marker doubles as the clear affordance (the header note): its caption
    // is the landed UserLangOverrideActive copy, so no new StringId is invented
    // for "clear override" (P9b-1 did not land one - reported as a copy gap).
    tb.AddItem(CHK, 8, 242, 232, 12, kIdOverrideMarker, BTN_CLS,
               I18n::Get(StringId::UserLangOverrideActive));
    tb.AddItem(LBL, 8, 260, 404, 10, kIdDisclaimer, STATIC_CLS,
               I18n::Get(StringId::UserLangSupportNotQuality));
    // The dialog's own close label: the landed generic "닫기" / "Close" copy
    // (GgufManagerClose) rather than a new StringId. IDCANCEL is the button id so
    // Esc closes the modal loop exactly like a click.
    tb.AddItem(BTN, 348, 278, 64, 14, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::GgufManagerClose));
}

UserLanguageManagerOutcome ShowUserLanguageManagerDialogEx(
    HWND parent,
    AppConfig& config,
    LanguageEngine engine,
    const std::wstring& model,
    UserLanguageProbeService* probe_service) {
    ManagerDialogState st;
    st.config = &config;
    st.engine = engine;
    st.engine_id = UserLanguageEngineId(engine);
    st.model = ResolveUserLanguageManagerModel(engine, config, model);
    st.service = probe_service;

    const std::wstring title = I18n::Get(StringId::UserLangsGroup);
    TemplateBuilder tb;
    tb.Begin(title, kDialogWidthDlu, kDialogHeightDlu, kDialogItemCount);
    BuildUserLanguageManagerTemplate(tb);

    ::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), tb.Get(), parent,
                              UserLanguageManagerDialogProc,
                              reinterpret_cast<LPARAM>(&st));
    // The dialog HWND (and its timer) is gone with the modal loop; the outcome
    // reports only what the locked verbs actually applied. The caller persists
    // on kChanged; a retest verdict persists through the runner's own callback,
    // which this dialog never touched.
    return FoldUserLanguageManagerOutcome(st.any_applied);
}

} // namespace user_language_manager_dialog
} // namespace emebalachat
