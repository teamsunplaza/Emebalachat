#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "layered_renderer.hpp"
#include "../update_checker.hpp" // REQ-UC: ReleaseInfo + check/download outcomes

namespace emebalachat {

// REQ-005 (architect plan §2.2): branded About popup opened from the tray
// context menu ("About Emebala Chat…"). Self-contained Direct2D layered popup
// (own WndProc, single-threaded D2D factory + DC render target, DIB mem-DC,
// UpdateLayeredWindow blit).
//
// REQ-UC (260930_0004, update-checker): the card gained the spec's brand
// chrome (80 DIP logo tile, 16 pt name, version line, twin-gold-hairline seal,
// copyright footer) AND a manual update checker. The CEO-flagged regression
// (qa-screenshots/01-03): the rework REPLACED the intro content with the
// update zone. The merged layout restores the full intro (tagline, the three
// dynamic-height feature blocks, etymology, link pills, contact lines —
// byte-faithful to the pre-rework rendering, including the 260922_0001 A2
// no-clip-on-long-languages behavior) and demotes the update flow: Idle shows
// a QUIET secondary footer affordance next to the reset button; while a flow
// state is active (checking / up-to-date / available / downloading / ready /
// snoozed / cancelled / errors) the MIDDLE REGION swaps to the update UI and
// swaps back to the full intro on return to Idle.
//
// Threading: Create/Show/Dismiss/Destroy are GUI-thread operations. Show()
// marshals via PostMessageW when called from another thread (REQ-R10). The
// update check/download run on detached worker threads that capture ONLY an
// HWND value + a shared UpdateJob (never `this`), so Destroy() is safe: it
// flips job->cancel and returns; the worker finishes its in-flight call,
// fails its PostMessageW against the dead HWND harmlessly, and releases the
// job with the shared_ptr. All results cross back through the mutex-guarded
// job slot via two pure (0,0) wake-up messages (SEC-ADJ contract).
class AboutWindow {
public:
    AboutWindow();
    ~AboutWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // Shows the window centered on the work area of the monitor containing
    // (x, y) (typically the cursor position when the tray item was clicked).
    void Show(int x, int y);
    void Dismiss();

    bool IsVisible() const { return visible_.load(std::memory_order_relaxed); }
    HWND GetHwnd() const { return hwnd_; }

    // Number of link buttons on the intro (Website / Contact / Reddit).
    static constexpr int kNumLinks = 3;

    // Marshaled message IDs — WM_APP+0x300 block: distinct from the tooltip's
    // 0x200 block and the drag icon's 0x100 block (see tooltip.hpp comments).
    // SEC-ADJ (release readiness 260911_0002, Blocker-5 class): every payload
    // travels losslessly in the message parameters or as a pure (0,0) wake-up;
    // no pointer ever crosses the seam.
    static constexpr UINT kShowMessage   = WM_APP + 0x301;
    static constexpr UINT kDismissMessage = WM_APP + 0x302;
    static constexpr UINT kLocaleRefreshMessage = WM_APP + 0x303;
    // REQ-UC: worker->GUI completion wake-ups for the update flow. Pure (0,0)
    // payloads — the outcomes travel through the mutex-guarded UpdateJob slot.
    static constexpr UINT kUpdateCheckDoneMessage = WM_APP + 0x304;
    static constexpr UINT kUpdateDownloadDoneMessage = WM_APP + 0x305;

    // Coordinate packing for kShowMessage (SEC-ADJ): x travels in WPARAM, y
    // in LPARAM, as sign-extended int values — the exact DragIconWindow
    // contract (MAKELPARAM's 16-bit truncation rejected for multi-monitor
    // negative coordinates).
    static constexpr WPARAM PackShowX(int x) {
        return static_cast<WPARAM>(static_cast<INT_PTR>(x));
    }
    static constexpr LPARAM PackShowY(int y) {
        return static_cast<LPARAM>(static_cast<INT_PTR>(y));
    }
    static constexpr int ShowXFromWParam(WPARAM w) {
        return static_cast<int>(static_cast<INT_PTR>(w));
    }
    static constexpr int ShowYFromLParam(LPARAM l) {
        return static_cast<int>(static_cast<INT_PTR>(l));
    }

    // R6 Phase 6 (plan §5.4): refresh caption + localized body after a
    // UI-language switch. Thread-safe (marshals like Show/Dismiss).
    void RequestLocaleRefresh();

    // Phase 4 (REQ-020): wires the "Reset to system defaults" action. The
    // window stays a pure view: on click it invokes this callback (GUI
    // thread — the WndProc runs there) after an explicit Yes confirm.
    void SetResetCallback(std::function<void()> cb) { reset_callback_ = std::move(cb); }

    // R6 Phase 5 (plan §5.2): pure localized-content snapshot resolved from the
    // i18n tables for the CURRENT locale. Headless test seam; Render consumes
    // it for the restored intro + the reset label.
    struct LocalizedContent {
        std::wstring title;
        std::wstring tagline;
        std::wstring features[3];
        std::wstring etymology;
        std::wstring link_labels[kNumLinks];
        std::wstring contacts[3];
        std::wstring reset_label;
    };
    static LocalizedContent BuildLocalizedContent();

    // ------------------------------------------------------------------
    // 260922_0001 A2 (fix plan §4.3-A / §5 A2, CPO triage Rank 2): dynamic
    // feature-block heights for the About card. The old layout hard-coded
    // `top = 230.0f + i*42.0f` with a fixed 40 DIP clip rect, so localized
    // feature lines wrapping to 2–3 lines (de/ru/es/vi) had their third line
    // silently clipped away. The fix measures each feature's actual text
    // height (IDWriteTextLayout::GetMetrics in Render) and stacks the three
    // blocks with cumulative offsets. Same "pure plan function + Render
    // consumes it" shape as REQ-052's TooltipWindow::PlanHeaderTargetButton.
    //
    // The POD metrics struct below deliberately abstracts DWRITE_TEXT_METRICS
    // so run_tests.exe (links Emebalachat_core only — never instantiates
    // DWrite) can call the planner with synthetic heights (fix plan §5 A2
    // verification item: TestReq053AboutFeatureLayout). Field mirrors the one
    // DWrite metric the planner actually consumes; Render copies it verbatim
    // from DWRITE_TEXT_METRICS::height.
    // ------------------------------------------------------------------
    struct AboutFeatureMetrics {
        float height; // measured text height in DIP (DWRITE_TEXT_METRICS.height)
    };
    struct AboutFeaturePlan {
        float block_top[3];   // stacked block start Y (DIP, region coords)
        float block_h[3];     // per-block height (>= 40 DIP floor pre-squeeze)
        float marker_x[3];    // gold marker-dot rect LEFT X; LTR: 28.0f, RTL: card_w - 33.0f
        float next_free_y;    // first Y below the last block (bottom anchors derive from this)
    };

    // Pure layout planner (260922_0001 A2). No D2D/DWrite COM dependency —
    // callable headless. card_w is the unused-in-current-geometry DIP card
    // width (parameter per the fix-plan signature; kept for the RTL marker
    // math and any future width-driven rules).
    //   block_h[i]   = max(metrics[i].height + 8.0f, 40.0f)   // 40 DIP = old fixed height floor
    //   block_top[0] = 230.0f; block_top[i] = block_top[i-1] + block_h[i-1] + 2.0f
    //   next_free_y  = block_top[2] + block_h[2]
    // Worst-case squeeze fallback (fix plan §5 A2 item 5): when the three
    // natural-height blocks plus the FIXED bottom section can no longer fit
    // above the footer bottom (578 DIP in the 596 DIP region), every block is
    // uniformly shrunk — but never below the 40 DIP floor; the residual
    // overflow is accepted (CLIP still prevents bleed, same contract as
    // REQ-052). NOTE (260930_0004 merge): the planner works in REGION
    // coordinates; Render adds kIntroShift (29 DIP) to every emitted Y.
    static constexpr AboutFeaturePlan PlanAboutFeatureLayout(
        const AboutFeatureMetrics metrics[3], bool ui_rtl, float card_w) {
        AboutFeaturePlan plan{};
        for (int i = 0; i < 3; ++i) {
            const float natural = metrics[i].height + 8.0f;
            plan.block_h[i] = natural > 40.0f ? natural : 40.0f;
        }
        plan.block_top[0] = 230.0f;
        for (int i = 1; i < 3; ++i) {
            plan.block_top[i] = plan.block_top[i - 1] + plan.block_h[i - 1] + 2.0f;
        }
        plan.next_free_y = plan.block_top[2] + plan.block_h[2];
        // Squeeze pass (fix plan §5 A2 item 5, geometry reconciled — see the
        // A2 report's plan-deviation note): the bottom section keeps its old
        // gaps relative to next_free_y (etymology +8 … footer bottom +224), so
        // the design ceiling for the footer bottom is 588 DIP = region 596
        // minus the 8 DIP border margin (the plan's "578" was the ONE-LINE
        // footer bottom, not a structural limit — clamping there would forbid
        // any growth and nullify the fix). next_free_y may therefore reach
        // 588 - 224 = 364 before blocks must give. Under the old fixed
        // geometry next_free_y was 230 + 42 + 42 + 40 = 354, so the 1-line
        // case renders byte-identically to the old layout; growth beyond the
        // 364 ceiling squeezes the blocks uniformly (40 DIP floor, then the
        // retained D2D1_DRAW_TEXT_OPTIONS_CLIP contains the residual).
        // (card_w is intentionally not referenced here — see the parameter
        // note above.)
        (void)card_w;
        float overflow = plan.next_free_y - 364.0f;
        // 40 DIP per-block floor: the blocks can shed at most their excess
        // above the floor. The overflow is spread uniformly ACROSS THE
        // SHEDDABLE EXCESS (pro-rata share of each block's height above 40)
        // rather than a flat third each: a flat third would push the 1-line
        // blocks below the 40 DIP floor while a tall block still overflows,
        // violating the floor contract. Pro-rata keeps every block >= 40 and
        // lands next_free_y back on exactly 354 whenever overflow <= the
        // total sheddable excess.
        float sheddable = 0.0f;
        float excess[3] = {};
        for (int i = 0; i < 3; ++i) {
            excess[i] = (plan.block_h[i] > 40.0f) ? (plan.block_h[i] - 40.0f) : 0.0f;
            sheddable += excess[i];
        }
        if (overflow > 0.0f) {
            if (overflow > sheddable) overflow = sheddable; // clamp: never cut below the floor
            for (int i = 0; i < 3; ++i) {
                plan.block_h[i] -= (sheddable > 0.0f) ? (overflow * excess[i] / sheddable)
                                                      : 0.0f;
            }
            // Recompute cumulative tops/next_free_y from the squeezed heights.
            plan.block_top[0] = 230.0f;
            for (int i = 1; i < 3; ++i) {
                plan.block_top[i] = plan.block_top[i - 1] + plan.block_h[i - 1] + 2.0f;
            }
            plan.next_free_y = plan.block_top[2] + plan.block_h[2];
        }
        // Marker dot: LTR dot rect left stays at the historical 28.0f; the
        // RTL mirror keeps the dot 33 DIP in from the card's right edge
        // (old expression `w - 33.0f`, fix plan §5 A2 risk item 2 — kept
        // byte-identical).
        const float marker_left = ui_rtl ? (card_w - 33.0f) : 28.0f;
        for (int i = 0; i < 3; ++i) {
            plan.marker_x[i] = marker_left;
        }
        return plan;
    }

    // ------------------------------------------------------------------
    // REQ-UC (260930_0004): the update-zone state machine (design spec §9).
    // While any non-Idle state is active, Render swaps the MIDDLE REGION
    // (the intro content area) for the update UI; Idle renders the intro.
    // Exposed for the unit tests.
    // ------------------------------------------------------------------
    enum class UpdateZoneState {
        Idle,        // full intro + quiet footer update affordance
        Checking,    // spinner + label stack + quiet hint (card stays live)
        UpToDate,    // gold check + "vX.Y.Z"; auto-returns to Idle (~6 s / zone click)
        Available,   // consent dialog (scrim over the whole card)
        Downloading, // progress bar + meta + cancel (fill stays physical LTR)
        Ready,       // gold seal + [Later]/[Install now]
        Snoozed,     // Later was picked; quiet line + [Check for updates]
        Cancelled,   // user cancelled the download; quiet line + retry button
        ErrOffline,  // quiet lapis "!" line + [Check for updates] (retry)
        ErrRate,     // quiet lapis "!" line (GitHub rate limit) + retry
        ErrHash      // the ONLY red state: danger "!" + [Retry]
    };

    // REQ-UC: pure {v}/{s} placeholder replacement for the consent dialog
    // (every occurrence, positional-safe). Empty token/value are no-ops.
    static std::wstring ReplaceToken(std::wstring text, std::wstring_view token,
                                     std::wstring_view value);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowAt(int x, int y); // real show path, GUI thread only
    void Render();
    void UpdateLayered();
    void ReallocateBuffer(int width, int height); // physical px buffer
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    void RecreateAfterDeviceLost();
    void LoadLogoBitmap();
    // C1: the single persistent scratch brush (SetColor just before use).
    void EnsureScratchBrush();   // also (re)builds the cached progress gradient
    void ReleaseScratchBrush();  // releases scratch + gradient (target still alive)
    void ApplyLocaleFormatting();
    float MeasureTextWidth(IDWriteTextFormat* format, const std::wstring& text);
    void OpenLink(int index); // 0=website 1=contact 2=reddit (ShellExecuteW)

    // REQ-UC seams (GUI thread only).
    void StartUpdateCheck();
    void StartDownload();
    void HandleUpdateCheckDone();
    void HandleUpdateDownloadDone();
    void SetZoneState(UpdateZoneState state);
    void EnterInstall(); // LaunchInstaller + app exit

    // REQ-UC: the shared worker<->GUI job slot. The detached worker threads
    // capture a shared_ptr<UpdateJob> (+ an HWND value + by-value copies of
    // everything they need) — NEVER `this` — so Destroy() cannot race a
    // use-after-free; it just flips cancel.
    struct UpdateJob {
        std::atomic<bool> cancel{false};
        std::atomic<std::uint64_t> received{0};
        std::atomic<std::uint64_t> total{0};
        std::mutex m;
        bool check_done = false;
        UpdateCheckStatus check_status = UpdateCheckStatus::Offline;
        updatelogic::ReleaseInfo release;
        bool download_done = false;
        UpdateDownloadStatus download_status = UpdateDownloadStatus::Failed;
        std::wstring download_path;
    };

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); WndProc dispatch owner
    std::atomic<bool> visible_{false};

    int current_width_ = 440;  // DIP (fixed 440 card)
    int current_height_ = 625; // DIP (REQ-UC chrome + restored intro + footer)
    UINT dpi_ = 96;            // REQ-R15: DPI of the monitor showing the window

    LayeredD2DRenderer renderer_;

    // Direct2D & DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1SolidColorBrush* scratch_brush_ = nullptr;   // C1: persistent single brush
    ID2D1LinearGradientBrush* progress_brush_ = nullptr; // REQ-UC: cached gold fill
    ID2D1PathGeometry* spinner_arc_ = nullptr;        // REQ-UC: cached 270-degree arc
    ID2D1Bitmap* logo_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    // REQ-UC chrome formats (design spec §3): brand header typography.
    IDWriteTextFormat* title_format_ = nullptr;     // 16 SemiBold, centered (app name)
    IDWriteTextFormat* version_format_ = nullptr;   // 11, centered, subtext
    IDWriteTextFormat* tagline_format_ = nullptr;   // 12, wrapped, centered
    IDWriteTextFormat* body_format_ = nullptr;      // 12, wrapped, leading
    IDWriteTextFormat* etymology_format_ = nullptr; // 11 italic, centered
    IDWriteTextFormat* link_format_ = nullptr;      // 12 medium, centered
    IDWriteTextFormat* reset_footer_format_ = nullptr; // footer reset pill (wraps)
    IDWriteTextFormat* small_format_ = nullptr;     // 10.5, centered (contact)
    IDWriteTextFormat* header_format_ = nullptr;    // 12 centered (close ✕)
    // REQ-UC update-zone formats (design spec §3).
    IDWriteTextFormat* uz_head_format_ = nullptr;     // 12.5 SemiBold centered
    IDWriteTextFormat* uz_sub_format_ = nullptr;      // 10.5 normal centered
    IDWriteTextFormat* uz_label_format_ = nullptr;    // 10.5 normal leading (labels/meta)
    IDWriteTextFormat* uz_button_format_ = nullptr;   // 12.5 SemiBold (trim)
    IDWriteTextFormat* uz_textbtn_format_ = nullptr;  // 11 medium centered
    IDWriteTextFormat* uz_dlgtitle_format_ = nullptr; // 13 SemiBold centered
    IDWriteTextFormat* uz_value_format_ = nullptr;    // 12 SemiBold leading
    IDWriteTextFormat* uz_pct_format_ = nullptr;      // 13 SemiBold trailing
    IDWriteTextFormat* copyright_format_ = nullptr;   // 10 normal leading

    // Interactive rectangles, DIP window coords (recomputed every Render).
    D2D1_RECT_F close_btn_rect_ = {};
    D2D1_RECT_F link_rects_[kNumLinks] = {}; // restored intro link pills
    D2D1_RECT_F reset_rect_ = {};            // footer reset pill (REQ-020 confirm)
    D2D1_RECT_F update_check_rect_ = {};     // REQ-UC: idle footer / zone retry
    D2D1_RECT_F dlg_later_rect_ = {};        // REQ-UC: consent dialog [Later]
    D2D1_RECT_F dlg_download_rect_ = {};     // REQ-UC: consent dialog [Download]
    D2D1_RECT_F dl_cancel_rect_ = {};        // REQ-UC: downloading [Cancel]
    D2D1_RECT_F retry_rect_ = {};            // REQ-UC: err-hash [Retry]
    D2D1_RECT_F ready_later_rect_ = {};      // REQ-UC: ready [Later]
    D2D1_RECT_F ready_install_rect_ = {};    // REQ-UC: ready [Install now]
    int hovered_ctl_ = -1; // hover encoding below; -1 none
    static constexpr int kHoverLink0 = 0; // intro link pills 0..2
    static constexpr int kHoverLink2 = 2;
    static constexpr int kHoverClose = 3;
    static constexpr int kHoverReset = 4;
    static constexpr int kHoverUpdateCheck = 5;
    static constexpr int kHoverDlgLater = 6;
    static constexpr int kHoverDlgDownload = 7;
    static constexpr int kHoverDlCancel = 8;
    static constexpr int kHoverRetry = 9;
    static constexpr int kHoverReadyLater = 10;
    static constexpr int kHoverReadyInstall = 11;

    // REQ-020: reset feedback + the REQ-052 confirm gate.
    static constexpr UINT_PTR kResetFeedbackTimerId = 0xB1B1; // unique on this hwnd
    static constexpr UINT kResetFeedbackMs = 1600;
    ULONGLONG reset_feedback_until_ = 0;
    std::function<void()> reset_callback_;
    bool confirm_pending_ = false;

    // REQ-UC timers: 50 ms animation (spinner rotation + progress chase) and
    // the ~6 s up-to-date auto-return.
    static constexpr UINT_PTR kUpdateAnimTimerId = 0xB2;
    static constexpr UINT_PTR kUpToDateReturnTimerId = 0xB3;
    static constexpr UINT kUpToDateReturnMs = 6000;

    // REQ-UC update-zone state (GUI thread only).
    UpdateZoneState zone_state_ = UpdateZoneState::Idle;
    std::shared_ptr<UpdateJob> job_;
    updatelogic::ReleaseInfo pending_release_; // the offered release (Available)
    std::wstring ready_path_;                  // verified installer path (Ready)
    float displayed_pct_ = 0.0f;               // smoothed progress (chase target)
    ULONGLONG checking_started_ = 0;           // spinner rotation anchor (ms)
    int dlg_focus_ = 0;                        // consent focus: 0=Later 1=Download
};

} // namespace emebalachat
