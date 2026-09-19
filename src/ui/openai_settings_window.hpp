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

#include <windows.h>

#include "../openai_compatible_client.hpp"

namespace emebalachat {

// Shows the modal settings dialog. `parent` is the owner HWND (may be null).
// `cfg` is the current persisted openai block (read to pre-fill); on OK it is
// replaced with the edited values AND the key is DPAPI-protected in-place.
// Returns true when the user saved (caller persists via AppConfig::SaveToFile).
bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg);

} // namespace emebalachat
