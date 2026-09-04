#include "sound.hpp"

#include <atomic>
#include <thread>
#include <windows.h>

namespace emebalachat {

namespace {
static std::atomic<bool> g_sound_enabled{true};
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

    DWORD freq = 1000;
    DWORD duration = 100;

    switch (type) {
        case SoundType::Enable:
            freq = 1000;
            duration = 100;
            break;
        case SoundType::Disable:
            freq = 400;
            duration = 120;
            break;
        case SoundType::CycleLang:
            freq = 800;
            duration = 80;
            break;
        case SoundType::ToggleAutoSend:
            freq = 1200;
            duration = 80;
            break;
    }

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
