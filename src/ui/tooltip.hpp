#pragma once

#include <atomic>
#include <cstdint> // R6 Phase 2 (B1-H1): uint64_t generation ids
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <sapi.h>

#include "layered_renderer.hpp"

namespace emebalachat {

// Returns the Windows LCID for a given language code or name. Returns 0 if unknown.
DWORD GetLcidForLanguage(std::string_view target_lang_name_or_code);

class TooltipWindow {
public:
    using LanguageChangeCallback = std::function<void(std::string_view new_target_lang)>;

    // F8 (ADR-A1-4): source-language dropdown callback. Passes the picked
    // source as name_en ("Auto Detect" for the AUTO entry) - the same payload
    // form the target menu uses, so main.cpp routes both through the single
    // ApplyLanguageChange coordinator (persist -> tray check-marks -> tooltip).
    using SourceLanguageChangeCallback = std::function<void(std::string_view new_source_lang)>;

    TooltipWindow();
    ~TooltipWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // Displays the tooltip with translated text at screen coordinates (x, y).
    // MAIN GUI THREAD ONLY: it drives the single-threaded D2D render target
    // (audit §3.4 / REQ-R10). Use ShowTranslationThreadSafe() from any other
    // thread (keyboard/mouse hook threads, the REQ-R06 double-Ctrl+C worker).
    //
    // R6 Phase 2 (B1-H1): `generation` carries the request sequence id stamped
    // by BeginTranslationRequest() at trigger time. A payload whose generation
    // is older than the latest-requested generation is DROPPED (never rendered)
    // so a slow, superseded translate thread can no longer overwrite a newer
    // result (the last-writer-wins stale-tooltip race). kGenNone (0) means
    // "unmanaged show" (synchronous GUI-thread callers, notices, tests): always
    // renders, keeping every pre-existing call site behaviorally unchanged.
    void ShowTranslation(
        int x, int y,
        std::wstring_view source_text,
        std::string_view source_lang_code,
        std::string_view target_lang,
        std::wstring_view translated_text,
        uint64_t generation = kGenNone
    );

    void Dismiss();
    bool IsVisible() const { return visible_.load(std::memory_order_relaxed); }
    HWND GetHwnd() const { return hwnd_; }

    // ---- REQ-R08 (audit §3.2): transient message-bubble mode ----
    // Lightweight "state changed" notice (used for the Win+F9 toggle feedback):
    // compact D2D card with a header line and a body line; no copy/TTS/language
    // buttons. Auto-hides after kMessageAutohideMs unless dismissed sooner.
    // Same thread affinity as ShowTranslation (GUI thread only).
    // R6 Phase 2 (B1-H1): same generation guard as ShowTranslation - a failure
    // notice from a superseded translate request must not replace newer content.
    void ShowMessage(int x, int y, std::wstring_view header, std::wstring_view body,
                     uint64_t generation = kGenNone);

    // ---- REQ-R10 companions: thread-safe marshaling seams ----
    // Cross-thread ShowWindow()/D2D calls are unsafe from hook threads
    // (D2DERR_WRONG_THREAD, and ShowWindow can block on the GUI thread's pump
    // while it is mid-translation). These seams post the work to the tooltip's
    // own window (main GUI thread) via PostMessageW, which is non-blocking for
    // the caller. SEC-ADJ (release readiness 260911_0002, Blocker-5 class):
    // the transport is NO LONGER a heap pointer in LPARAM (CWE-822 shatter —
    // any same-session process could post an attacker-chosen address that the
    // WndProc dereferenced and freed). Payloads travel BY VALUE through the
    // mutex-guarded deques in tooltip.cpp; the posted WM_APP message is a pure
    // wake-up notification (wParam = lParam = 0, both ignored by WndProc).
    // On a failed post the entry stays queued and is consumed by the next
    // wake-up or by the DrainMarshalQueue() teardown sweep — nothing leaks and
    // nothing double-applies.
    // R6 Phase 2 (B1-H1): the generation travels INSIDE the payload; the drop
    // decision runs on the GUI thread in ShowTranslation/ShowMessage against
    // latest_request_gen_, so it is immune to both producer-thread races and
    // marshal-queue reordering (a stale payload that overtakes a fresh one is
    // still dropped on arrival).
    void ShowMessageThreadSafe(int x, int y, std::wstring_view header, std::wstring_view body,
                               uint64_t generation = kGenNone);
    void ShowTranslationThreadSafe(
        int x, int y,
        std::wstring_view source_text,
        std::string_view source_lang_code,
        std::string_view target_lang,
        std::wstring_view translated_text,
        uint64_t generation = kGenNone
    );
    void DismissThreadSafe();

    // ---- R6 Phase 2 (B1-H1): translation-request generation guard ----
    // Sentinel 0 = "generation-unmanaged show" (renders unconditionally).
    static constexpr uint64_t kGenNone = 0;

    // Stamp a NEW translate request and return its monotonic generation id.
    // Call at TRIGGER time (before any thread spawn / clipboard work) on the
    // thread that observed the user action, then pass the returned id to the
    // Show*ThreadSafe seam when the result lands. Thread-safe (atomic); never
    // returns kGenNone.
    uint64_t BeginTranslationRequest();

    // Pure staleness decision (headless-testable planner per plan §7.2):
    // render only when the payload is the newest request (or unmanaged).
    // newest-wins: payload_gen >= latest_requested -> show; older -> drop.
    static constexpr bool ShouldRenderForGeneration(uint64_t latest_requested,
                                                    uint64_t payload_gen) {
        if (payload_gen == kGenNone || latest_requested == kGenNone) {
            return true; // unmanaged show always renders
        }
        return payload_gen >= latest_requested;
    }

    // Test/inspection seams (GUI-thread-written values; the marshal tests pump
    // messages on the owning thread before reading).
    uint64_t LatestRequestGenerationForTest() const {
        return latest_request_gen_.load(std::memory_order_relaxed);
    }
    uint64_t DroppedStaleShowsForTest() const { return dropped_stale_shows_; }

    bool IsMessageMode() const { return is_message_mode_; }
    const std::wstring& GetMessageHeader() const { return message_header_; }

    // Marshaled message IDs. WM_APP+0x200 block: distinct from the drag icon's
    // WM_APP+0x100 block (src/ui/drag_icon.hpp) and the badge's WM_USER block.
    static constexpr UINT kShowTranslationMessage = WM_APP + 0x201;
    static constexpr UINT kShowMessageMessage     = WM_APP + 0x202;
    static constexpr UINT kDismissMessage         = WM_APP + 0x203;
    // REQ-002 (plan §2.1): wheel-forwarded scroll request. WPARAM unused,
    // LPARAM = raw WM_MOUSEWHEEL delta as signed int (WHEEL_DELTA multiples,
    // possibly fractional-notch values from precision touchpads). Posted from
    // the LL mouse hook callback (hook thread) — consumed on the GUI thread
    // (REQ-R10 marshaling discipline; the tooltip is WS_EX_NOACTIVATE so the
    // OS routes real WM_MOUSEWHEEL to the focused window, never to it).
    static constexpr UINT kScrollMessage          = WM_APP + 0x204;
    // R6 Phase 1 (B3): marshaled target-language view refresh posted by
    // RefreshTargetLanguageFromConfig() when called off the GUI thread.
    static constexpr UINT kRefreshTargetLangMessage = WM_APP + 0x205;

    // ---- REQ-002 scrollable tooltip (architect plan §2.1) ----
    // GUI-thread entry for kScrollMessage (and direct WM_MOUSEWHEEL). Converts
    // the raw wheel delta to a DIP offset step, clamps against the
    // content/viewport extents, and re-renders only when the offset actually
    // moved. Marshals via PostMessageW off-thread (REQ-R10). No-op in message
    // mode (fixed-height REQ-R08 card) and when the body fits (not scrollable_).
    void ScrollByDipWheel(int wheel_delta);

    // ---- pure, headless-testable scroll math (all DIP, plan REQ-002/REQ-R15) ----
    // Max tooltip window height after this batch (was 480; body viewport is
    // kMaxWindowHeightDip - header - footer reserves, see ShowTranslation).
    static constexpr int kMaxWindowHeightDip = 520;
    // Body viewport top / footer reserve matching the Render layout constants
    // (body text rect ends at current_height_ - 44, see tooltip.cpp Render).
    static constexpr float kBodyTopDip = 48.0f;
    static constexpr float kFooterReserveDip = 44.0f;

    // Viewport height available for body text at a given window height (DIP).
    static constexpr float BodyViewportHeightDip(int window_height_dip) {
        const float vh = static_cast<float>(window_height_dip) - kBodyTopDip - kFooterReserveDip;
        return vh > 0.0f ? vh : 0.0f;
    }

    // Clamping scroll-offset rule: nothing to scroll (content fits the
    // viewport) pins the offset at 0; otherwise clamp into [0, content-viewport].
    static constexpr float ClampScrollOffset(float offset, float content_h, float viewport_h) {
        if (content_h <= viewport_h) return 0.0f;
        if (offset < 0.0f) return 0.0f;
        const float max_offset = content_h - viewport_h;
        return offset > max_offset ? max_offset : offset;
    }

    // Wheel delta -> offset step in DIP. One full notch (WHEEL_DELTA=120)
    // scrolls 3 body lines (Windows SPI_GETWHEELSCROLLLINES default); negative
    // for wheel-down (offset grows downward). Fractional deltas from precision
    // touchpads scale proportionally.
    static constexpr float WheelDeltaToOffsetStepDip(int wheel_delta, float line_height_dip) {
        return -static_cast<float>(wheel_delta) * (3.0f * line_height_dip / static_cast<float>(WHEEL_DELTA));
    }

    // Scrollbar thumb extent: viewport/content proportional, min 24 DIP,
    // never taller than the track.
    static constexpr float ScrollbarThumbHeightDip(float track_h, float viewport_h, float content_h) {
        if (content_h <= 0.0f || viewport_h <= 0.0f) return track_h;
        if (viewport_h >= content_h) return track_h;
        float h = track_h * viewport_h / content_h;
        if (h < 24.0f) h = 24.0f;
        return h > track_h ? track_h : h;
    }

    // Scrollbar thumb top: linear map of offset in [0, content-viewport] to
    // [track_top, track_top + track_h - thumb_h]. Degenerate ranges pin to the
    // track top (no jitter at the boundaries, plan §2.1 edge case 2).
    static constexpr float ScrollbarThumbTopDip(float track_top, float track_h, float thumb_h,
                                                float offset, float content_h, float viewport_h) {
        const float range = track_h - thumb_h;
        if (range <= 0.0f) return track_top;
        const float scroll_range = content_h - viewport_h;
        if (scroll_range <= 0.0f) return track_top;
        float frac = offset / scroll_range;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return track_top + frac * range;
    }

    // Test/inspection seams for the REQ-002 scroll state (values are written
    // only on the GUI thread; tests pump messages on the owning thread).
    bool IsScrollableForTest() const { return scrollable_; }
    float ScrollOffsetForTest() const { return scroll_offset_dip_; }
    float ContentHeightForTest() const { return content_height_dip_; }

    // SEC-ADJ (Blocker-5 class fix): these payloads NO LONGER travel as heap
    // pointers in LPARAM. They are value types enqueued by the posting seams
    // into the mutex-guarded deques in tooltip.cpp (one deque per payload
    // type, each with its own mutex so a wake-up on one message id never
    // blocks on another), and popped one-at-a-time by the WndProc handlers
    // (per-item lock: entries enqueued while an earlier one is being applied
    // are still consumed by the same wake-up loop). The struct definitions
    // are unchanged so every producer/test call site keeps compiling.
    // R6 Phase 2 (B1-H1): both carry the originating request generation
    // (kGenNone default = unmanaged, always renders - existing producers and
    // test payloads that never set it keep their exact behavior).
    struct TranslationPayload {
        int x;
        int y;
        std::wstring source_text;
        std::string source_lang_code;
        std::string target_lang;
        std::wstring translated_text;
        uint64_t generation = kGenNone;
    };
    struct MessagePayload {
        int x;
        int y;
        std::wstring header;
        std::wstring body;
        uint64_t generation = kGenNone;
    };
    // R6 B3: marshaled RefreshTargetLanguageFromConfig payload (value-queue
    // transport, same SEC-ADJ contract as the seams above).
    struct TargetLangPayload {
        std::string target_lang;
    };
    // Test/inspection seam: injects a caller-provided payload VALUE into the
    // same value queue the hook/worker threads use for `msg`, then posts the
    // identical parameter-less wake-up notification. The WndProc drains and
    // applies it on the next pump. `payload` must point at a live Translation/
    // Message/TargetLangPayload matching `msg` (the seam copies it by value;
    // ownership never transfers). Returns false on null hwnd, unsupported msg,
    // or PostMessage failure.
    static bool PostPayloadForTest(HWND hwnd, UINT msg, const void* payload);

    // R6 Phase 3 (audit items 6+8) / SEC-ADJ: drains the three value queues
    // (Translation/Message/TargetLang), discarding still-queued payloads.
    // Destroy() calls it on the GUI thread right before DestroyWindow, and it
    // replicates the old PeekMessage purge semantics exactly: messages are
    // REMOVED without running the apply path (discard, not apply). With the
    // by-value deques there is no heap to free; clearing the deque is what
    // releases the string memory the old `delete` did. Returns the number of
    // payloads removed (test seam for the drain invariant).
    int DrainMarshalQueue();

    void SetLanguageChangeCallback(LanguageChangeCallback cb) { lang_change_cb_ = std::move(cb); }

    // F8 (ADR-A1-4): register the source-language dropdown pick handler.
    void SetSourceLanguageChangeCallback(SourceLanguageChangeCallback cb) { src_lang_change_cb_ = std::move(cb); }

    // ---- R6 Phase 1 (B3): single-source-of-truth language sync (plan §2.4) ----
    // View-refresh seam called by the main.cpp ApplyLanguageChange coordinator
    // whenever ANY surface (tray menu, swap, cycle, config load) mutates the
    // persisted target language. Best-effort by design: a no-op while hidden
    // or in REQ-R08 message mode (the compact notice has no language button);
    // otherwise it re-labels the target button and re-renders WITHOUT touching
    // the translated body (re-translation on external change is out of scope -
    // the plan calls for view sync, not content churn). Thread-safe: posts to
    // the GUI-thread WndProc through kRefreshTargetLangMessage like the other
    // REQ-R10 seams, so hook/worker-thread coordinators can call it directly.
    void RefreshTargetLanguageFromConfig(std::string_view new_target);

    // Audio & Clipboard actions
    void SpeakCurrentText();
    void StopTTS();
    void CopyToClipboard();

    // Voice selection for multi-language TTS
    //
    // P4 Batch-3 (REQ-C-002, design §1.2.2 / decision D8) return contract:
    // true  = a language-matching voice was selected;
    // false = no voice for the language (unresolvable LCID or zero installed
    // matches); voice state unchanged - the forced default-voice fallback is
    // REMOVED (that was the silent wrong-language reading defect) and the
    // caller decides the UX (SpeakCurrentText shows ShowNoVoiceNotice and
    // speaks nothing).
    bool SelectVoiceForLanguage(std::string_view target_lang_name_or_code);
    const std::string& GetCurrentVoiceName() const { return current_voice_name_; }
    const std::string& GetCurrentVoiceLanguage() const { return current_voice_lang_; }

    // ---- P4 Batch-3 (REQ-C-001, design §1.2.1 / decision D6): voice score
    // lattice. Pure + constexpr, compile-time-proven below (same pattern as
    // the scroll math section). cat_idx is the SelectVoiceForLanguage
    // kCategories enumeration order: 0 = SPCAT_VOICES (legacy SAPI desktop
    // voices), 1 = Speech_OneCore (modern high-quality Windows voices).
    // Historical defect: SAPI base 80 outranked OneCore base 40, so a legacy
    // mechanical voice won over an installed OneCore voice for the same
    // language. Fixed lattice: OneCore 100 / SAPI 50 base, the exact-LCID
    // bonus (+20, match == 2 per MatchTokenLanguage) preserved, so a SAPI
    // voice can still win only when OneCore has no language match at all.
    static constexpr int kVoiceCategorySapi = 0;
    static constexpr int kVoiceCategoryOneCore = 1;
    static constexpr int kVoiceBaseScoreSapi = 50;
    static constexpr int kVoiceBaseScoreOneCore = 100;
    static constexpr int kVoiceExactMatchBonus = 20;

    static constexpr int VoiceBaseScore(int cat_idx) {
        return cat_idx == kVoiceCategoryOneCore ? kVoiceBaseScoreOneCore : kVoiceBaseScoreSapi;
    }

    // match: 2 = exact LCID, 1 = primary-language match (MatchTokenLanguage).
    static constexpr int VoiceTotalScore(int cat_idx, int match) {
        return VoiceBaseScore(cat_idx) + (match == 2 ? kVoiceExactMatchBonus : 0);
    }
    // NOTE: the compile-time lattice proofs (static_assert 120/100/70/50) live
    // at namespace scope right after the class - an in-class static_assert
    // that CALLS a member of the same class is evaluated while the class is
    // still incomplete and is rejected by MSVC (C2131). Same reason the
    // scroll-math asserts sit in completed-type scope (run_tests.cpp:3142).

    // Retrieves current translation content
    const std::wstring& GetSourceText() const { return source_text_; }
    const std::string& GetSourceLangCode() const { return source_lang_code_; }
    const std::string& GetTargetLang() const { return target_lang_; }
    // Body text (translation, or the notice body in REQ-R08 message mode). Read
    // by the marshaling unit tests to assert a posted payload reached the model.
    const std::wstring& GetTranslatedText() const { return translated_text_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void UpdateLayered();
    void ReallocateBuffer(int width, int height); // physical px buffer
    // REQ-R15 (audit §5 latent item 3): current_width_/current_height_ are
    // DIP layout extents; the window/DIB are physical px of the target
    // monitor. PhysW/PhysH convert with the DPI captured at show time.
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery. Called
    // when EndDraw returns D2DERR_RECREATE_TARGET; recreates the single-
    // threaded DC render target + the device-dependent logo bitmap on the new
    // target. Without it a driver reset leaves the surface permanently blank.
    void RecreateAfterDeviceLost();
    void LoadLogoBitmap();
    // C1 (session 260910_0007): the single persistent scratch brush the whole
    // Render path draws through (SetColor just before every Fill/Draw/DrawText
    // call). Created at Create() and device-lost recovery - NEVER at render
    // entry - and released in RecreateAfterDeviceLost (before the old target
    // is dropped) and Destroy. Null when target creation failed; every draw
    // site null-guards exactly like the old per-brush `if (brush)` checks.
    void EnsureScratchBrush();
    void ReleaseScratchBrush();
    // C2 (session 260910_0007): measured-layout cache seam. Recomputes
    // src_tag_width_ / copy_btn_w_ / tts_btn_w_ ONLY when the label strings
    // feeding them change (content set, UI-locale string switch - detected by
    // the cached-key comparison; DirectWrite metrics are DPI-independent DIPs
    // so a DPI crossing needs no recompute). Render consumes the cached
    // floats; the hit-test rects derive from the same values (lockstep by
    // construction).
    void EnsureMeasuredLayouts();
    void InitSapi();
    void CleanupSapi();
    // P4 Batch-3 (REQ-C-002, design §1.2.2 / decision D7): transient "no
    // Windows voice installed for this language" notice. Reuses the REQ-R08
    // message-mode state machine through ShowMessage (compact card, header +
    // body, 1.8s autohide via kMessageAutohideMs, generation guard) - no new
    // window type, no new message ids, no new timers. While it is showing,
    // no_voice_notice_active_ routes a card click to the Windows speech
    // settings page (ms-settings:speech deep link, decision D7).
    void ShowNoVoiceNotice();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); WndProc dispatch owner
    // Hook/worker threads read IsVisible() concurrently with GUI-thread writes
    // (same rule that made badge/tray state atomics mandatory in I4).
    std::atomic<bool> visible_{false};

    std::wstring source_text_;
    std::string source_lang_code_;
    std::string target_lang_;
    std::wstring translated_text_;

    // ---- R6 Phase 2 (B1-H1): generation-guard state ----
    // latest_request_gen_: bumped by BeginTranslationRequest() on ANY thread
    // (trigger sites run on the GUI thread, the REQ-R06 double-Ctrl+C worker,
    // and the detached drag threads). Read ONLY inside the GUI-thread
    // ShowTranslation/ShowMessage bodies to drop superseded deliveries.
    // dropped_stale_shows_: GUI-thread-only diagnostic counter (test seam).
    std::atomic<uint64_t> latest_request_gen_{kGenNone};
    uint64_t dropped_stale_shows_ = 0;

    int current_width_ = 360;  // DIP
    int current_height_ = 160; // DIP
    UINT dpi_ = 96;            // REQ-R15: DPI of the monitor showing the tooltip

    // REF-3.6: GDI DIB + memory DC + DC render-target lifetime moved to the
    // shared RAII owner. dc_render_target_ below is a NON-OWNING alias into
    // renderer_.target(), kept in sync at the three points where the target
    // changes (Create / RecreateAfterDeviceLost / Destroy).
    LayeredD2DRenderer renderer_;

    // Direct2D & DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1SolidColorBrush* scratch_brush_ = nullptr;   // C1: persistent single brush
    ID2D1Bitmap* logo_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* header_format_ = nullptr;
    IDWriteTextFormat* body_format_ = nullptr;
    IDWriteTextFormat* button_format_ = nullptr;
    IDWriteTextFormat* small_format_ = nullptr;

    // Interactive Button Rectangles (local window coordinates)
    D2D1_RECT_F copy_btn_rect_ = {};
    D2D1_RECT_F tts_btn_rect_ = {};
    D2D1_RECT_F lang_btn_rect_ = {};
    D2D1_RECT_F close_btn_rect_ = {};
    // F8 (ADR-A1-4): source-language dropdown button rect (target-pattern
    // member so the WM_MOUSEMOVE/WM_LBUTTONUP hit tests can reach it).
    D2D1_RECT_F src_btn_rect_ = {};

    // ---- REQ-002 scroll state (DIP layout space, plan §1.3) ----
    float scroll_offset_dip_ = 0.0f;    // current scroll position (>= 0)
    float content_height_dip_ = 0.0f;   // measured body text height
    float line_height_dip_ = 18.0f;     // measured line spacing (wheel step unit)
    bool scrollable_ = false;           // content overflows the viewport
    bool dragging_thumb_ = false;       // L-button capture held on the thumb
    bool thumb_hover_ = false;          // brighter thumb while pointer is over it
    float drag_start_offset_dip_ = 0.0f;
    float drag_start_y_dip_ = 0.0f;
    D2D1_RECT_F body_viewport_rect_ = {};
    D2D1_RECT_F scrollbar_track_rect_ = {};
    D2D1_RECT_F scrollbar_thumb_rect_ = {};

    // ---- C2 measured-layout cache (DIP widths; keys are the exact label
    // strings the old per-Render measurement blocks consumed) ----
    float src_tag_width_ = 44.0f;      // floor == old fixed-size fallback
    std::wstring src_tag_key_;         // label that produced src_tag_width_
    float copy_btn_w_ = 88.0f;         // floors == old hardcoded boxes
    float tts_btn_w_ = 82.0f;
    std::wstring footer_labels_key_;   // concat of the 3 footer labels measured

    int hovered_btn_ = 0; // 0=none, 1=copy, 2=tts, 3=lang, 4=close, 5=src
    bool copied_feedback_ = false;
    bool is_message_mode_ = false;
    std::wstring message_header_;
    // P4 Batch-3 (REQ-C-002 / D7): true only while the CURRENT message-mode
    // card is the no-voice notice. Cleared by every show/dismiss path that
    // can replace or hide the card (ShowTranslation / ShowMessage / Dismiss)
    // and consumed by the WM_LBUTTONDOWN click router.
    bool no_voice_notice_active_ = false;

    // SAPI TTS Voice COM interface
    ISpVoice* voice_ = nullptr;
    ISpObjectToken* default_voice_token_ = nullptr;
    std::string current_voice_name_;
    std::string current_voice_lang_;

    LanguageChangeCallback lang_change_cb_;
    // F8 (ADR-A1-4): source-language dropdown handler (optional; hidden when
    // unset, exactly like the target callback).
    SourceLanguageChangeCallback src_lang_change_cb_;

    static constexpr UINT_PTR kTimerCopiedFeedback = 4001;
    static constexpr UINT_PTR kTimerMessageAutohide = 4002;
    static constexpr DWORD kMessageAutohideMs = 1800; // auto-hide delay for message-mode cards (F-17/R5/toggle notices)
};

// P4 Batch-3 (REQ-C-001 / D6): compile-time proof of the voice score lattice
// (§1.2.1). Class is complete here, so the constexpr member calls evaluate.
// The resulting order is OneCore exact 120 > OneCore partial 100 >
// SAPI exact 70 > SAPI partial 50 - OneCore always outranks legacy SAPI for
// the same language match level, and a SAPI voice can win only when OneCore
// has no language match at all.
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategoryOneCore, 2) == 120,
              "REQ-C-001: OneCore exact-match lattice point is 120");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategoryOneCore, 1) == 100,
              "REQ-C-001: OneCore partial-match lattice point is 100");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategorySapi, 2) == 70,
              "REQ-C-001: SAPI exact-match lattice point is 70");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategorySapi, 1) == 50,
              "REQ-C-001: SAPI partial-match lattice point is 50");

} // namespace emebalachat
