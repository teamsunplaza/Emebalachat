#pragma once

#include <atomic>
#include <cstdint> // 261111_0001 U1 (v0.10.7): uint64_t show/hide generation stamp
#include <functional>
#include <mutex>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "layered_renderer.hpp"
#include "theme.hpp"

namespace emebalachat {

// 261111_0001 U1 (v0.10.7, R1-D): pure staleness decision for the drag-icon
// show slot. A show message applies only when its stamp still matches the
// pending slot: 0 means there is no pending show (a Hide() emptied it, or
// nothing was ever requested), and a mismatch means a newer show superseded
// this one (or a forged stamp). DragIconWindow::ShouldApplyShowAt delegates
// here so the decision has ONE source of truth shared with
// DragShowArbiter::TryApply below.
constexpr bool ShouldApplyDragShowAt(uint64_t stamped_gen, uint64_t pending_gen) {
    return stamped_gen != 0 && stamped_gen == pending_gen;
}

// 261111_0001 U1 (v0.10.7, R1-D): the SINGLE arbiter for the drag-icon
// show/hide publication protocol. Every producer (RequestShowAt) and the Hide
// invalidation run under ONE mutex, so {token, coords, slot} publish as one
// atomic unit. This closes three races the previous split-atomic design had
// (four independent atomics: show_gen / pending_show_gen / pending_show_x /
// pending_show_y):
//   (1) a preempted producer could publish an OLDER generation last, so the
//       newest show was dropped and the stale one rendered (the pending slot,
//       not the monotonic counter, was the de-facto arbiter);
//   (2) a Hide() invalidation (slot := 0) could be overwritten by a show whose
//       token predated the hide, resurrecting the icon at a dead selection site;
//   (3) x and y were two separate atomics, so a preempted producer tore the
//       coordinates (x from one show, y from another).
// Pure (no Win32), so the interleaving rules are unit-testable headlessly; the
// live DragIconWindow holds one instance as a file-static (one icon window per
// process, hook/GUI threads share it). Copies the tooltip.cpp:1248
// mutex-guarded by-value publication pattern (single slot, latest-wins).
struct DragShowArbiter {
    // Begin a show under the lock: assign the next monotonic token and publish
    // {token, coords} into the slot as ONE unit. Returns the token to carry in
    // the kShowAtMessage WPARAM. The caller posts OUTSIDE the lock - the slot is
    // the arbiter, so a reordered post still resolves latest-wins.
    uint64_t BeginShow(int x, int y) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_x_ = x;
        pending_y_ = y;
        pending_gen_ = ++show_gen_;
        return pending_gen_;
    }
    // Invalidate (Hide) under the lock: empty the slot and bump the token so
    // every show stamped before this is dropped. A BeginShow that ran before the
    // invalidate but posts after is dropped; a BeginShow after it gets a fresh,
    // higher token and applies.
    //
    // 261111_0001 (v0.10.7) fix-list round 2, G-10: returns the HIDE token -
    // the generation this hide owns. The marshaled kHideMessage carries it and
    // the GUI-side handler re-invalidates only while it is still current
    // (InvalidateIfCurrent below), so a Hide() requested BEFORE a newer
    // BeginShow can no longer empty that newer show's slot when its message is
    // finally drained.
    uint64_t Invalidate() {
        std::lock_guard<std::mutex> lk(mu_);
        pending_gen_ = 0;
        return ++show_gen_;
    }
    // GUI-side completion of a marshaled hide. Returns true when `hide_token`
    // is still the current generation - i.e. no BeginShow/Invalidate has
    // advanced it since this hide was stamped - and re-invalidates in that
    // case; false means a NEWER request owns the state and must be left
    // untouched. Same mutex as BeginShow/TryApply, so the check and the
    // invalidate cannot be split by a concurrent producer.
    bool InvalidateIfCurrent(uint64_t hide_token) {
        std::lock_guard<std::mutex> lk(mu_);
        if (hide_token == 0 || show_gen_ != hide_token) {
            return false;
        }
        pending_gen_ = 0;
        ++show_gen_;
        return true;
    }
    // Decide whether a stamped show still applies and, if so, copy its coords
    // out ATOMICALLY with the decision (no torn read). Runs under the lock.
    bool TryApply(uint64_t stamped, int& out_x, int& out_y) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ShouldApplyDragShowAt(stamped, pending_gen_)) {
            return false;
        }
        out_x = pending_x_;
        out_y = pending_y_;
        return true;
    }
private:
    mutable std::mutex mu_;
    uint64_t show_gen_ = 0;
    uint64_t pending_gen_ = 0;
    int pending_x_ = 0;
    int pending_y_ = 0;
};

class DragIconWindow {
public:
    using ClickCallback = std::function<void(int click_x, int click_y)>;

    DragIconWindow();
    ~DragIconWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // ShowAt/Hide are safe to call from ANY thread: when invoked off the GUI
    // thread that created the window (mouse-hook thread, delayed-click worker),
    // they marshal themselves via PostMessageW to WndProc (REQ-R10, audit §3.4).
    // Same-thread callers run inline.
    void ShowAt(int x, int y);
    void Hide();
    void Invalidate() { if (IsVisible()) { Render(); UpdateLayered(); } }
    void RefreshTheme() { Invalidate(); }
    bool IsVisible() const { return visible_.load(std::memory_order_relaxed); }

    HWND GetHwnd() const { return hwnd_; }
    void SetClickCallback(ClickCallback cb) { click_cb_ = std::move(cb); }

    // REQ-R15 (audit §5 latent item 3): logical pill size in DIPs. The window,
    // the DIB buffer, and UpdateLayeredWindow are sized in PHYSICAL pixels of
    // the monitor the icon is shown on: phys_size_ = ScaleDipsToPixels(kSize,
    // dpi_) with dpi_ from ui::MonitorDpiAtPoint at ShowAt time, and the D2D
    // render target DPI is set to dpi_ so all DIP-authored geometry below
    // (rounded pill, logo rect) rasterizes crisply at 1:1 physical scale.
    static constexpr int kSize = 32;

    // ---- REQ-R10 (audit §3.4): D2D thread marshal for ShowAt ----
    //
    // The D2D factory and DC render target are created SINGLE_THREADED on the
    // main GUI thread (Create() runs in wWinMain). Calling ShowAt() from the
    // mouse-hook thread makes BeginDraw/EndDraw return D2DERR_WRONG_THREAD and
    // the icon renders invisible. RequestShowAt() is the thread-safe seam: it
    // posts kShowAtMessage to the icon's own window, so WndProc - dispatched by
    // the main GUI thread's message pump - performs the actual ShowAt(). Safe
    // from ANY thread including LowLevelMouseProc (PostMessageW never blocks on
    // the target queue; it only enqueues and returns).
    //
    // ---- 261111_0001 U1 (v0.10.7, audit D4/ISSUE-009): show/hide generation seam ----
    //
    // A kShowAtMessage posted BEFORE a Hide() used to land AFTER it (an inline
    // GUI-thread Hide() bypasses the posted queue) and strand a clickable icon
    // at a dead selection site. RequestShowAt() now stamps a monotonically
    // increasing generation: the stamp travels in WPARAM and the x/y pair is
    // held in a single-slot pending request (filled by RequestShowAt just
    // before the post). The kShowAtMessage handler drops any message whose
    // stamp no longer matches the pending slot - a show posted before a Hide()
    // (which empties the slot and bumps the generation) or one superseded by a
    // newer show (rapid releases coalesce, latest wins). The stamp also closes
    // the old coords-in-message forgery surface: a foreign process posting
    // kShowAtMessage can no longer choose WHERE the icon appears (coordinates
    // come from this process's own slot, and a guessed stamp never matches).
    //
    // The Pack* helpers keep the historical full-int coordinate packing as a
    // standalone, test-pinned contract (TestUIMarshaling in tests/run_tests.cpp):
    // MAKELPARAM(x, y) was deliberately NOT used - it truncates each coordinate
    // to 16 bits, which corrupts large negative virtual-screen offsets on
    // multi-monitor rigs (primary spans can push coordinates past INT16).
    // WPARAM/LPARAM are pointer-sized (64-bit on x64, 32-bit on x86, int is
    // 32-bit on both), so the int round-trip is lossless.
    static constexpr UINT kShowAtMessage = WM_APP + 0x101;
    // Hide marshaled the same way (ESC/outside-click dismiss paths run on the
    // keyboard/mouse hook threads). Hide() bumps the generation and empties the
    // pending slot at REQUEST time, so a later-processed stale show is dropped.
    // G-10 (fix-list round 2): WPARAM carries the HIDE TOKEN - the generation
    // that hide owns - and the handler re-invalidates only while that token is
    // still current, so a BeginShow that landed while this message sat in the
    // queue keeps its pending slot (the old unconditional re-invalidation
    // emptied it and dropped the newer show).
    static constexpr UINT kHideMessage = WM_APP + 0x102;

    static constexpr WPARAM PackShowAtX(int x) {
        return static_cast<WPARAM>(static_cast<INT_PTR>(x));
    }
    static constexpr LPARAM PackShowAtY(int y) {
        return static_cast<LPARAM>(static_cast<INT_PTR>(y));
    }
    static constexpr int ShowAtXFromWParam(WPARAM w) {
        return static_cast<int>(static_cast<INT_PTR>(w));
    }
    static constexpr int ShowAtYFromLParam(LPARAM l) {
        return static_cast<int>(static_cast<INT_PTR>(l));
    }

    // 261111_0001 U1 (v0.10.7, audit D4 / R1-D): pure staleness decision
    // (headless-testable, same pattern as TooltipWindow::ShouldRenderForGeneration).
    // Delegates to the free ShouldApplyDragShowAt so the decision has ONE source
    // of truth shared with DragShowArbiter::TryApply.
    static constexpr bool ShouldApplyShowAt(uint64_t stamped_gen, uint64_t pending_gen) {
        return ShouldApplyDragShowAt(stamped_gen, pending_gen);
    }

    // Returns false when hwnd is null (icon Create() failed or was Destroy()d)
    // or when PostMessage failed; callers may treat false as "no icon shown".
    // Thread-safe (REQ-R10): stamps the show/hide generation and fills the
    // single-slot pending coordinates consumed by the kShowAtMessage handler.
    static bool RequestShowAt(HWND hwnd, int x, int y);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // 261111_0001 (v0.10.7) fix-list round 2, G-10: Hide() is split into
    // (a) request-time invalidation + marshal, and (b) these two GUI-side
    // steps. HideWindow() is the pure GUI teardown (hidden flag + SW_HIDE);
    // CompleteHide() runs from the kHideMessage handler and re-invalidates the
    // arbiter ONLY while the hide's own token is still current, so a newer
    // BeginShow that landed while the hide was queued keeps its pending slot.
    // Round 3 (F-2): when the token is NOT current the newer show already
    // owns the slot, so CompleteHide also skips HideWindow() - a stale hide
    // must not tear down a newer show that already drained.
    void HideWindow();
    void CompleteHide(uint64_t hide_token);
    void Render();
    void UpdateLayered();
    void StartFadeoutTimer();
    void StopFadeoutTimer();
    void LoadLogoBitmap();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): recreate the DC render target
    // (+ device-dependent logo bitmap) after EndDraw returns
    // D2DERR_RECREATE_TARGET, so a driver reset cannot leave the icon a
    // permanently invisible pill.
    void RecreateAfterDeviceLost();
    // REQ-R15: (re)allocate the DIB + bind the DC render target at the given
    // physical edge size / DPI. No-op while geometry is unchanged.
    void EnsureBuffer(UINT dpi, int phys_edge);
    void RebindRenderTarget();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); owns all D2D/GUI calls
    std::atomic<bool> visible_{false}; // read cross-thread via IsVisible()
    bool is_hovered_ = false;          // WndProc (GUI thread) only
    BYTE alpha_ = 235;

    // REF-3.6: GDI DIB + memory DC + DC render-target lifetime moved to the
    // shared RAII owner. dc_render_target_ below is a NON-OWNING alias into
    // renderer_.target(), kept in sync at the three points where the target
    // changes (Create / RecreateAfterDeviceLost / Destroy).
    LayeredD2DRenderer renderer_;

    // REQ-R15: physical buffer geometry for the monitor the icon last showed on.
    UINT dpi_ = 96;
    int phys_size_ = kSize; // physical px edge length of window/DIB

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1Bitmap* logo_bitmap_ = nullptr;

    ClickCallback click_cb_;

    static constexpr UINT_PTR kTimerFadeout = 3001;
    static constexpr DWORD kFadeoutTimeoutMs = 2500;
};

} // namespace emebalachat
