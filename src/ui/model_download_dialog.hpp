#pragma once

// ---------------------------------------------------------------------------
// REQ-MD (session 260930_0004, decisions.md D9): consent + progress dialog
// for the on-demand bundled-model download. Replaces the dead-end
// "engine unavailable" notice when ONLY the pinned model file is missing:
// consent (size + privacy + disk pre-flight) -> progress (percent + MB,
// cancel) -> pinned-hash verify -> atomic place -> registry entry.
// Quiet inline failure states (retry), fail-closed hash state (red), same
// contract as the update checker. The worker runs the whole pipeline on a
// detached-from-GUI thread and is JOINED on every exit path before EndDialog
// (the HF add-dialog house contract).
// ---------------------------------------------------------------------------

#include <windows.h>

namespace emebalachat {

// Runs the modal consent+progress flow. Returns true when the pinned model
// file is present in the shared store afterwards (downloaded OR already
// there) — the caller then skips the "unavailable" notice. GUI thread only.
bool OfferModelDownload(HWND owner);

} // namespace emebalachat
