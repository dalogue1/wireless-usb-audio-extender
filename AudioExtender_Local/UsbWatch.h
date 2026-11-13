#pragma once
// Wireless USB Audio Extender local - when to recover the USB connection to the PC by itself.
// Plain C++ (no Arduino): tests/feature_tests.cpp drives it with simulated time.
//
//  * The PC has not set the device up for 30 s (after a start, unplug or bus reset):
//    soft re-attach (disconnect + connect), so the PC enumerates it from scratch;
//    again after 1, 2, 4 ... 10 min for as long as it stays that way.
//  * The PC has the stream open, is awake, but has stopped collecting audio: noted
//    after 1 s, re-attach after 5 s; stalling again within 2 min of that restarts the board.
//  * A suspended bus (PC asleep) is left alone: re-attaching could wake a sleeping PC.
#include <stdint.h>

class UsbWatch {
public:
    static constexpr uint32_t kFirstWaitMs = 30000;
    static constexpr uint32_t kMaxWaitMs = 600000;
    static constexpr uint32_t kStallNoteMs = 1000;
    static constexpr uint32_t kStallFixMs = 5000;
    static constexpr uint32_t kStallRepeatMs = 120000;

    struct Input {
        bool configured;   // the PC has set the device up
        bool suspended;    // bus suspended
        bool recording;    // stream open (alternate setting 1)
        uint32_t frames;   // USB audio frames served so far
    };
    enum class Action : uint8_t {
        None,
        StallNoted,        // stream open but no audio collected for 1 s (log it)
        StallCleared,      // audio is collected again (log it)
        ReattachUnset,     // re-attach: not set up by the PC (waitedMs says how long)
        ReattachStall,     // re-attach: no audio collected for 5 s
        Restart,           // stalled again within 2 min of a stall re-attach
    };

    // Call often (every loop). After a re-attach action the caller re-attaches and
    // calls reattached() with the time it finished.
    Action update(uint32_t now, const Input& in) {
        if (in.configured) {
            counting_ = false;
            wait_ = kFirstWaitMs;
        } else {
            if (!counting_) {  // first update without the PC: start counting
                counting_ = true;
                unsetSince_ = now;
            }
            if (now - unsetSince_ >= wait_) {
                waitedMs = wait_;
                wait_ = wait_ * 2 > kMaxWaitMs ? kMaxWaitMs : wait_ * 2;
                return Action::ReattachUnset;
            }
        }
        if (in.frames != frames_ || !in.recording || !in.configured || in.suspended) {
            const bool was = stalled_ && in.frames != frames_;
            if (was) stalledMs = now - movedMs_;
            stalled_ = false;
            frames_ = in.frames;
            movedMs_ = now;
            return was ? Action::StallCleared : Action::None;
        }
        if (!stalled_ && now - movedMs_ >= kStallNoteMs) {
            stalled_ = true;
            return Action::StallNoted;
        }
        if (stalled_ && now - movedMs_ >= kStallFixMs) {
            if (haveFix_ && now - lastFixMs_ < kStallRepeatMs) return Action::Restart;
            lastFixMs_ = now;
            haveFix_ = true;
            stalled_ = false;
            return Action::ReattachStall;
        }
        return Action::None;
    }

    // The re-attach finished at time now (the PC will set the device up again).
    void reattached(uint32_t now, uint32_t frames) {
        counting_ = true;
        unsetSince_ = now;
        movedMs_ = now;
        frames_ = frames;
        stalled_ = false;
    }

    uint32_t waitedMs = 0;   // ReattachUnset: how long it was not set up
    uint32_t stalledMs = 0;  // StallCleared: how long the stall lasted

private:
    bool counting_ = false;    // not set up by the PC: counting since unsetSince_
    uint32_t unsetSince_ = 0;
    uint32_t wait_ = kFirstWaitMs;
    uint32_t frames_ = 0, movedMs_ = 0, lastFixMs_ = 0;
    bool stalled_ = false, haveFix_ = false;
};
