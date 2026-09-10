#include "sound.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
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
}

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
    const DWORD freq = tone.freq;
    const DWORD duration = tone.duration;

    try {
        std::jthread([freq, duration]() {
            ::Beep(freq, duration);
        }).detach();
    } catch (...) {
        // Suppress thread creation errors gracefully if system resources are exhausted
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
