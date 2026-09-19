#pragma once

// REQ-045 P4-3 (design §3b, item 3b): the OpenAI Compatible engine settings
// dialog. A plain Win32 modal dialog (no D2D — it is a functional form, not a
// branded card) opened from the tray engine submenu. Captures:
//   * Base URL   (https forced; http requires an explicit consent confirmation
//                that persists http_consent_given)
//   * API Key    (ES_PASSWORD edit; stored DPAPI-protected, never cleartext;
//                masked read-back "abcdef***" after save)
//   * Model      (editable combo — "Fetch model list" calls ListModels on the
//                GUI thread with a bounded 10 s budget and fills the combo;
//                when the fetch fails or returns empty the user types a model
//                id directly, per the user requirement)
// Save validates, DPAPI-protects the key, writes the openai{} block through
// the AppConfig the caller passes, and persists via SaveToFile.

#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../openai_compatible_client.hpp"

namespace emebalachat {

// REQ-046 P4-3 (Tech Gate 필수-5): the in-memory DLGTEMPLATE builder is a
// public symbol so the unit suite can instantiate it directly and pin the
// template style bits (WS_VISIBLE/DS_CENTER/DS_SETFOREGROUND) WITHOUT entering
// the modal DialogBoxIndirectParamW loop (which would block the headless test
// runner forever). Declaration lives here; implementation stays in the .cpp.
// The buffer layout follows the Win32 dialog-template contract (4-byte
// alignment of DLGTEMPLATE + each DLGITEMTEMPLATE).
class TemplateBuilder {
public:
    void Begin(std::wstring_view title, short w, short h, WORD itemCount);
    void AddItem(DWORD style, short x, short y, short cx, short cy,
                 WORD id, WORD clsAtom, std::wstring_view text);
    const DLGTEMPLATE* Get() const;
private:
    void EmitWord(WORD w);
    void EmitDword(DWORD d);
    void EmitStr(std::wstring_view s);
    std::vector<BYTE> buf_;
    size_t dtpl_ = 0;
};

// Shows the modal settings dialog. `parent` is the owner HWND (may be null).
// `cfg` is the current persisted openai block (read to pre-fill); on OK it is
// replaced with the edited values AND the key is DPAPI-protected in-place.
// Returns true when the user saved (caller persists via AppConfig::SaveToFile).
bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg);

} // namespace emebalachat
