#pragma once
// Wireless USB Audio Extender - BOOT button: a click, or a 10-second hold.
//
// SHARED FILE: copied into both sketch folders by tools/sync_shared.sh.
// Plain C++ (no Arduino): tests/feature_tests.cpp drives it on the host.
//
//   click (press and release within 2 s): local board -> restart as the EXTENDER drive
//   hold 10 s (LED goes dark) and release:  factory reset (both boards)
//   Holding between 2 and 10 s does nothing, so a hold on its way to a reset
//   never opens the drive.
#include <stdint.h>

class HoldButton {
public:
    static constexpr uint32_t kDebounceMs = 30;
    enum class Action : uint8_t { None, Click, Hold10 };

    // Call every few ms with the raw button state (true = pressed).
    Action update(uint32_t now, bool rawPressed) {
        if (rawPressed != raw_) { raw_ = rawPressed; rawSince_ = now; }
        if (now - rawSince_ < kDebounceMs || raw_ == pressed_) return Action::None;
        pressed_ = raw_;
        if (pressed_) { pressStart_ = now; return Action::None; }
        const uint32_t held = now - pressStart_;
        if (held >= 10000) return Action::Hold10;
        if (held < 2000) return Action::Click;
        return Action::None;
    }
    // How long the button has been held (0 when released).
    uint32_t heldMs(uint32_t now) const { return pressed_ ? now - pressStart_ : 0; }

private:
    bool raw_ = false, pressed_ = false;
    uint32_t rawSince_ = 0, pressStart_ = 0;
};
