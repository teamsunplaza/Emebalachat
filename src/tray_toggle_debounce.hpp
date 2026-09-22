#pragma once

// -----------------------------------------------------------------------------
// 260922_0001 A1 (UI audit fix): pure tray single-click debounce state machine.
//
// Why header-only: tests/run_tests.cpp links Emebalachat_core ONLY and never
// includes main.cpp (the GUI entry TU is deliberately excluded from the test
// binary). The decision logic therefore lives in an inline, OS-free function
// that the unit suite can call directly, while kControllerWndProc consumes it
// for the real WM_LBUTTONUP / WM_LBUTTONDBLCLK / WM_TIMER wiring.
//
// Contract (see badge.cpp:768 kTimerSingleClick for the in-app precedent):
//   * Every WM_LBUTTONUP (re)arms the debounce timer -> N rapid single clicks
//     collapse to ONE toggle, evaluated at the LAST click.
//   * WM_LBUTTONDBLCLK cancels the pending arm -> a double-click never toggles
//     the engine (it only flips the badge, handled by the caller).
//   * With no pending arm, a stray timer expiry is a no-op.
// -----------------------------------------------------------------------------

namespace emebalachat {

// Observed tray-icon event fed into the state machine.
enum class TrayToggleEvent {
    kLeftButtonUp,     // WM_LBUTTONUP on the tray icon
    kLeftButtonDblClk, // WM_LBUTTONDBLCLK on the tray icon
    kTimerExpired,     // the debounce timer fired (WM_TIMER, kTimerTrayToggle)
};

// Action the caller must take for the observed event.
enum class TrayToggleAction {
    kArmTimer,    // (re)start the debounce timer; do NOT toggle yet
    kCancelTimer, // cancel the pending arm (double-click path)
    kToggleNow,   // timer expired with a pending arm -> toggle the engine now
    kNone,        // no-op (stray expiry, nothing pending)
};

struct TrayToggleDecision {
    TrayToggleAction action;
    bool timer_pending; // armed state to carry into the next event
};

// Pure transition function. `timer_pending` is the debounce timer's armed
// state BEFORE this event; the returned `timer_pending` is the state AFTER.
inline TrayToggleDecision TrayToggleDebounceStateMachine(TrayToggleEvent event,
                                                         bool timer_pending) {
    switch (event) {
        case TrayToggleEvent::kLeftButtonUp:
            return { TrayToggleAction::kArmTimer, true };
        case TrayToggleEvent::kLeftButtonDblClk:
            return { TrayToggleAction::kCancelTimer, false };
        case TrayToggleEvent::kTimerExpired:
            if (timer_pending) {
                return { TrayToggleAction::kToggleNow, false };
            }
            return { TrayToggleAction::kNone, false };
    }
    return { TrayToggleAction::kNone, timer_pending };
}

} // namespace emebalachat
