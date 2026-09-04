#pragma once

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

// Non-blocking sound dispatch spawning a detached std::jthread executing Win32 Beep.
// Safe fallback if audio hardware is unavailable.
void PlaySoundAsync(SoundType type);

// Convenience sound functions
void PlayToggleOn();
void PlayToggleOff();
void PlayLangChange();
void PlayModeChange();

} // namespace emebalachat
