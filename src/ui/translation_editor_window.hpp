#pragma once

// V10F L2-B (261122_0002 P2-G, v0.10.10 phased fixes): the TRANSLATION
// EDITOR window. A single-instance MODELESS Win32 dialog the tooltip's
// [edit/retranslate] affordance (P2-C) opens for one history entry
// (src/translation_history.hpp): three panes (editable source, editable
// target, read-only back-translation), [EditRetranslate] re-runs the engine
// on the edited source, [ApplyToOriginalWindow] pastes the edited target
// back into the window the translation came from.
//
// Contract notes:
//   * Open() runs on the GUI thread (the tooltip callback thread). The
//     retranslate engine call and the async on_done delivery marshal their
//     result back through a posted message, so any worker thread may invoke
//     on_done.
//   * Single instance: calling Open() again while the window is alive
//     refreshes the panes with the new entry instead of opening a second
//     window.
//   * Panes use RichEdit50W (msftedit.dll loaded with LoadLibraryW); when
//     the library is missing every pane falls back to a multiline EDIT.
//   * on_done delivers the re-translated FORWARD text only (the RetranslateFn
//     signature carries one string); it fills the target pane. The
//     back-translation pane is cleared when a retranslate starts because its
//     previous content no longer describes the edited source; a caller that
//     wants a fresh back-translation re-opens the entry after pushing it to
//     the history ring again.

#include <functional>
#include <string>

#include <windows.h>

#include "../translation_history.hpp"

namespace emebalachat {
namespace translation_editor_window {

// The retranslation seam. The wiring unit (P2-E, main.cpp) supplies the
// implementation: it runs the engine on `source` toward `tgt_code` and
// eventually invokes on_done with the translated text (from ANY thread; the
// editor marshals to its GUI thread). May be empty (std::function) — the
// button then simply does nothing.
using RetranslateFn = std::function<void(
    const std::wstring& source, const std::wstring& tgt_code,
    std::function<void(const std::wstring&)> on_done)>;

// Opens (or refreshes) the single editor window for `entry`. `retranslate`
// is stored for the [EditRetranslate] button. GUI thread only.
void Open(const translation_history::Entry& entry, RetranslateFn retranslate);

// V10F-R1 F-09: destroys the editor window if it is live (GUI thread only).
// Registered in main.cpp's shutdown sweep next to about_window.Destroy(): the
// modeless dialog has no parent/owner, so without this sweep entry its hwnd
// could outlive the surfaces a late posted result delivery would touch.
void ShutdownDestroy();

// ---- pure layout planner (SECTION A, headless-tested) ----------------------
//
// The window is authored in DIP; PlanEditorLayout maps the DIP client size
// to the six control rects. All constants live here so the unit suite can
// pin the geometry without entering the window code. Degenerate (tiny)
// inputs are tolerated: heights/widths may collapse to empty rects but never
// to negative extents, and the pane stack always fills the full client width
// minus the margins.

inline constexpr int kEditorClientWidthDip = 560;
inline constexpr int kEditorClientHeightDip = 440;
inline constexpr int kEditorMarginDip = 10;
inline constexpr int kEditorGapDip = 8;
inline constexpr int kEditorButtonHeightDip = 26;
inline constexpr int kEditorCloseButtonWidthDip = 72;
inline constexpr int kEditorActionButtonWidthDip = 150;

struct EditorLayoutPlan {
    RECT source_pane;         // editable original text
    RECT target_pane;         // editable translated text
    RECT back_pane;           // read-only back-translation
    RECT retranslate_button;  // [EditRetranslate]
    RECT apply_button;        // [ApplyToOriginalWindow]
    RECT close_button;        // generic close (bottom-right)
};

constexpr int EditorNonNeg(int v) { return v < 0 ? 0 : v; }

constexpr EditorLayoutPlan PlanEditorLayout(int client_w, int client_h) {
    const int m = kEditorMarginDip;
    const int g = kEditorGapDip;
    const int bh = kEditorButtonHeightDip;

    // Button row sits on the bottom margin, right-aligned, close last.
    const int btn_y = client_h - m - bh;
    const int close_l = client_w - m - kEditorCloseButtonWidthDip;
    const int apply_l = close_l - g - kEditorActionButtonWidthDip;
    const int retr_l = apply_l - g - kEditorActionButtonWidthDip;

    // The three panes split whatever is left above the button row; the back
    // pane absorbs the integer-division remainder so the stack reaches the
    // content bottom exactly.
    const int content_bottom = btn_y - g;
    const int pane_h = EditorNonNeg(content_bottom - m - 2 * g) / 3;
    const int src_t = m;
    const int tgt_t = src_t + pane_h + g;
    const int back_t = tgt_t + pane_h + g;

    EditorLayoutPlan p{};
    p.source_pane = RECT{m, src_t, client_w - m, src_t + pane_h};
    p.target_pane = RECT{m, tgt_t, client_w - m, tgt_t + pane_h};
    p.back_pane = RECT{m, back_t, client_w - m, content_bottom};
    p.retranslate_button = RECT{retr_l, btn_y, retr_l + kEditorActionButtonWidthDip, btn_y + bh};
    p.apply_button = RECT{apply_l, btn_y, apply_l + kEditorActionButtonWidthDip, btn_y + bh};
    p.close_button = RECT{close_l, btn_y, close_l + kEditorCloseButtonWidthDip, btn_y + bh};
    return p;
}

} // namespace translation_editor_window
} // namespace emebalachat
