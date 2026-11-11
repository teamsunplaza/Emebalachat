#pragma once

// ---------------------------------------------------------------------------
// REQ-060 (session 261009_0001, engine-idle-eviction-warmup): the "번역 모델
// 로드 방식" (translation-model load mode) settings dialog. A plain Win32 modal
// dialog (no D2D — functional form, same contract as openai_settings_window)
// opened from the tray engine submenu (the tray row + dispatch are the wiring
// agent's job — see the session ui-report). Captures ONE choice:
//   * Load when used (recommended) — engine_host.warmup = false (default):
//     the local engine loads the model on the first translation; memory stays
//     free until then at the cost of a slower first translation.
//   * Preload — engine_host.warmup = true: the model is loaded eagerly so the
//     first translation starts immediately; the model stays resident.
// Layout: title label + CBS_DROPDOWNLIST fixed combo (2 options) + one
// description STATIC under the combo whose text follows the CBN_SELCHANGE
// selection + [확인]/[취소]. The dialog only edits the in-memory block: on OK
// `out` receives `current` with `warmup` replaced; the CALLER persists
// config.json (SaveToFile) and owns any runtime re-application — this dialog
// never touches the file itself (unlike the REQ-051 OpenAI delete path).
// ---------------------------------------------------------------------------

#include <windows.h>

// 261111_0001 U7: engine headers moved to the shared engine repo (EMEBALA_ENGINE_DIR);
// they resolve through Emebala_engine_host's PUBLIC include dir (engine src/).
#include "engine_host_client.hpp" // EngineHostConfig (warmup field, REQ-060)
#include "openai_settings_window.hpp" // TemplateBuilder (proven DLGTEMPLATE contract)

namespace emebalachat {

// REQ-060: shows the modal load-mode dialog. `parent` is the owner HWND (may
// be null). `current` is the persisted engine_host block (read to pre-select);
// on OK `out` is replaced with a copy of `current` whose `warmup` field
// reflects the combo selection and the function returns true. On Cancel `out`
// is untouched and false is returned. The caller persists via
// AppConfig::SaveToFile (this dialog performs no file I/O).
bool ShowEngineHostSettingsDialog(HWND parent, const EngineHostConfig& current,
                                  EngineHostConfig& out);

// REQ-060: the production item-set emission, split out of
// ShowEngineHostSettingsDialog so the unit suite serializes the exact bytes the
// dialog hands to DialogBoxIndirectParamW (BuildOpenAiTemplate /
// BuildGgufManagerTemplate precedent). Begin() must already have been called
// with the matching item count — 5 (title label + mode combo + description
// static + OK + Cancel).
void BuildEngineHostSettingsTemplate(TemplateBuilder& tb);

} // namespace emebalachat
