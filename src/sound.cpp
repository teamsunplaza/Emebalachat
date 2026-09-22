#include "sound.hpp"

#include <array>
#include <atomic>
#include <condition_variable> // 260922_0001 A8: worker wake-up on queue/stop
#include <cstddef>
#include <deque> // 260922_0001 A8: pending-tone queue
#include <mutex> // 260922_0001 A8: queue guard
#include <thread> // 260922_0001 A8: resident worker (was: detached jthread per beep)
#include <windows.h>

namespace emebalachat {

namespace {
static std::atomic<bool> g_sound_enabled{true};

// REF-3.7 (session 260910_0006): tone parameters table indexed by the
// SoundType enum value. The enum is contiguous 0..3 (sound.hpp - no explicit
// values, so the implicit sequence is language-guaranteed), and the table
// size is pinned to the highest enumerator: adding a SoundType without a
// tone row breaks the build here instead of silently falling back.
// Row order mirrors the enum order; values are byte-identical to the former
// switch arms (Enable 1000/100, Disable 400/120, CycleLang 800/80,
// ToggleAutoSend 1200/80).
// 260922_0001 A8: table, static_asserts, and the out-of-range fallback below
// are UNCHANGED on purpose — the CPO triage (Rank 8) restricts this fix to
// the threading model only; tone/frequency/duration must not move.
struct ToneSpec {
    DWORD freq;
    DWORD duration;
};

constexpr std::array<ToneSpec, 4> kToneTable = {{
    {1000, 100}, // Enable:         F9 Toggle ON
    {400, 120},  // Disable:        F9 Toggle OFF
    {800, 80},   // CycleLang:      Ctrl+F9 Target Lang Change
    {1200, 80},  // ToggleAutoSend: Ctrl+Shift+Enter Auto-Send Toggle
}};

static_assert(kToneTable.size() == static_cast<std::size_t>(SoundType::ToggleAutoSend) + 1,
              "REF-3.7: kToneTable must have exactly one row per SoundType value");
static_assert(static_cast<std::size_t>(SoundType::Enable) == 0,
              "REF-3.7: table indexing assumes Enable is the first enum value");

// ---------------------------------------------------------------------------
// 260922_0001 A8 (UI audit fix): single resident worker + serialized queue.
//
// Previous model: every PlaySoundAsync() spawned a detached std::jthread that
// ran the blocking ::Beep (max 120 ms) — rapid F9 spam minted unbounded OS
// threads. New model: ONE lazily-created worker owns the Beep calls; requests
// queue FIFO behind a mutex. On saturation the OLDEST waiting tone is dropped
// (drop-oldest, fix-plan §4.2-B) so the freshest user feedback lands with
// bounded latency (8 queued tones <= ~960 ms worst-case backlog).
// ShutdownSound() latches stop, wakes the worker, and joins it — bounded by
// one in-flight Beep (<= 120 ms).
// ---------------------------------------------------------------------------
std::mutex g_queue_mu;
std::condition_variable g_queue_cv;
std::deque<ToneSpec> g_queue;
std::atomic<bool> g_stop{false};
std::thread g_worker;
std::once_flag g_worker_once;

// Worker body: waits for work (or stop), pops the front, RELEASES the lock
// across the blocking ::Beep, then re-acquires and loops.
void SoundWorkerLoop() {
    for (;;) {
        ToneSpec tone{};
        {
            std::unique_lock<std::mutex> lock(g_queue_mu);
            g_queue_cv.wait(lock, [] {
                return g_stop.load(std::memory_order_relaxed) || !g_queue.empty();
            });
            // 260922_0001 A8: stop wins over backlog — pending tones are
            // discarded on shutdown (the app is leaving; stale beeps are noise).
            if (g_stop.load(std::memory_order_relaxed)) {
                return;
            }
            tone = g_queue.front();
            g_queue.pop_front();
        } // lock released BEFORE the blocking Beep
        ::Beep(tone.freq, tone.duration);
    }
}

// Creates the resident worker exactly once (first sound request). Mirrors the
// single-worker + condition_variable serialization of worker.cpp, adapted for
// fire-and-forget GUI-thread callers.
void EnsureSoundWorker() {
    std::call_once(g_worker_once, [] {
        // 260922_0001 A8: preserve the old best-effort semantics — if thread
        // creation fails (resource exhaustion), skip playback silently rather
        // than propagating into the hotkey path.
        try {
            g_worker = std::thread(SoundWorkerLoop);
        } catch (...) {
        }
    });
}
} // namespace

void SetSoundEnabled(bool enabled) {
    g_sound_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsSoundEnabled() {
    return g_sound_enabled.load(std::memory_order_relaxed);
}

void PlaySoundAsync(SoundType type) {
    if (!IsSoundEnabled()) {
        return;
    }

    // Bounds check preserves the old switch's no-default semantics: an
    // out-of-range (corrupt) enum value fell through with the initial
    // 1000 Hz / 100 ms, which is exactly kToneTable[Enable].
    const std::size_t idx = static_cast<std::size_t>(type);
    const ToneSpec& tone = idx < kToneTable.size() ? kToneTable[idx] : kToneTable[0];

    // 260922_0001 A8: enqueue onto the resident worker instead of spawning a
    // detached thread per beep. Policy lives in sound.hpp (pure inline) so
    // run_tests can exercise the boundary without Win32 playback.
    EnsureSoundWorker();
    {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        if (g_stop.load(std::memory_order_relaxed)) {
            return; // shutdown latched: drop the request (old model had none —
                    // this only fires between ShutdownSound() and process exit)
        }
        // drop-oldest (§4.2-B): a saturated queue sheds its stalest waiting
        // tone so the freshest feedback always gets a slot.
        if (SoundEnqueuePolicy(g_queue.size(), kMaxQueueDepth) ==
            SoundEnqueueDecision::kDropOldestEnqueue) {
            g_queue.pop_front();
        }
        g_queue.push_back(tone);
    }
    g_queue_cv.notify_one();
}

// 260922_0001 A8: deterministic teardown of the resident worker. main.cpp's
// clean-shutdown block calls this after every sound producer (hook/worker/
// tray callbacks) is stopped, so no PlaySoundAsync can enqueue after the join.
void ShutdownSound() {
    {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        g_stop.store(true, std::memory_order_relaxed);
    }
    g_queue_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join(); // bounded: at most one in-flight Beep (<= 120 ms)
    }
}

void PlayToggleOn() {
    PlaySoundAsync(SoundType::Enable);
}

void PlayToggleOff() {
    PlaySoundAsync(SoundType::Disable);
}

void PlayLangChange() {
    PlaySoundAsync(SoundType::CycleLang);
}

void PlayModeChange() {
    PlaySoundAsync(SoundType::ToggleAutoSend);
}

} // namespace emebalachat
