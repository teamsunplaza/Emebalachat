#pragma once

// 260922_0001 A8: std::size_t for the pure enqueue policy below.
#include <cstddef>

namespace emebalachat {

enum class SoundType {
    Enable,         // 1000 Hz, 100 ms (F9 Toggle ON)
    Disable,        // 400 Hz, 120 ms  (F9 Toggle OFF)
    CycleLang,      // 800 Hz, 80 ms   (Ctrl+F9 Target Lang Change)
    ToggleAutoSend  // 1200 Hz, 80 ms  (Ctrl+Shift+Enter Auto-Send Toggle)
};

// Sets whether audio feedback beeps are enabled globally.
void SetSoundEnabled(bool enabled);

// Returns current state of audio feedback setting.
bool IsSoundEnabled();

// ---------------------------------------------------------------------------
// 260922_0001 A8 (UI audit fix): sound playback is now serialized on ONE
// resident worker thread (was: a detached std::jthread per beep). Pending
// tones queue up in FIFO order; when the queue saturates, the OLDEST waiting
// request is dropped so the freshest user feedback lands with bounded latency
// (drop-oldest policy, fix-plan §4.2-B; CPO triage Rank 8 explicitly forbids
// any tone/frequency/duration change — the kToneTable contract in sound.cpp
// is byte-identical to the previous behavior).
// ---------------------------------------------------------------------------

// Saturation bound for the worker queue: at most this many tones may wait.
inline constexpr std::size_t kMaxQueueDepth = 8;

// Decision returned by SoundEnqueuePolicy.
enum class SoundEnqueueDecision {
    kEnqueue,           // queue has room: append the tone
    kDropOldestEnqueue, // queue full: discard the front (oldest), then append
};

// Pure, OS-free enqueue policy: given the pending queue size BEFORE the append
// and the saturation bound, decide whether the oldest item must be dropped.
// Header-only inline so tests/run_tests.cpp can exercise the boundary
// directly (same rationale as tray_toggle_debounce.hpp — the test binary never
// links main.cpp and must not depend on Win32 playback to test the policy).
inline SoundEnqueueDecision SoundEnqueuePolicy(std::size_t queue_size,
                                               std::size_t max_depth) {
    return (queue_size >= max_depth) ? SoundEnqueueDecision::kDropOldestEnqueue
                                     : SoundEnqueueDecision::kEnqueue;
}

// Non-blocking sound dispatch: enqueues the tone onto the resident worker
// thread (lazily created on first call) and returns immediately. The actual
// Win32 Beep runs on the worker, serialized in queue order. Safe fallback if
// audio hardware is unavailable; thread-creation failure is suppressed.
void PlaySoundAsync(SoundType type);

// Requests shutdown of the resident sound worker and joins it. Call once from
// the application exit path AFTER every beep producer is down (main.cpp clean
// shutdown). Pending queued tones are discarded; the join is bounded by one
// in-flight Beep (<= 120 ms per the kToneTable durations). Idempotent-safe
// when no worker was ever created.
void ShutdownSound();

// Convenience sound functions
void PlayToggleOn();
void PlayToggleOff();
void PlayLangChange();
void PlayModeChange();

} // namespace emebalachat
