#pragma once
// config.txt mode = auto: microphone or speaker, decided on the remote once per adapter
// plug-in (and at start-up), never mid-run.
//   adapter only records -> microphone; only plays -> speaker;
//   both (most): listen kDecideMs on its microphone side: any sample that is not pure
//   digital silence = microphone; pure silence (an adapter with jack sensing and nothing
//   in the mic jack) = speaker. An adapter that hisses with nothing plugged in counts as
//   a microphone (fails safe). The audio flows as a microphone while it listens.
// Plain C++ (tests/feature_tests.cpp drives it through plug-ins and timings).
#include <stdint.h>
#include "LinkProtocol.h"

namespace modesel {

constexpr uint32_t kDecideMs = 10000;

struct Input {
    bool connected = false;      // an adapter is plugged in (opened)
    bool canCapture = false;     // it has a usable 48 kHz microphone side
    bool canPlayback = false;    // ...and/or speaker side
    bool streaming = false;      // its stream is open in the direction in use
    uint32_t plugIns = 0;        // adapters plugged in since start (re-opens not counted)
    uint32_t captureSinceMs = 0; // when the capture stream began (0: not capturing)
    bool heardNonZero = false;   // any non-zero sample since then
    uint32_t nowMs = 0;
};

struct State {
    uint8_t mode = wlink::kModeMicrophone;   // wlink::AudioMode in effect
    uint8_t reason = wlink::kReasonDeciding; // wlink::ModeReason
    uint32_t lastPlugIns = 0;
};

enum class Action : uint8_t {
    None,
    Decided,          // mode/reason just became final (log it); switch direction if mode changed
    ListenAgain,      // a new plug-in while in speaker mode: open the microphone side to listen
};

// One step (call often). setting: wlink::ModeSetting. Returns what to do; state holds the result.
inline Action step(State& st, const Input& in, uint8_t setting) {
    if (setting == wlink::kModeSetMicrophone || setting == wlink::kModeSetSpeaker) {
        const uint8_t m = setting == wlink::kModeSetSpeaker ? wlink::kModeSpeaker : wlink::kModeMicrophone;
        if (st.mode == m && st.reason == wlink::kReasonSetting) return Action::None;
        st.mode = m;
        st.reason = wlink::kReasonSetting;
        return Action::Decided;
    }
    Action act = Action::None;
    if (in.plugIns != st.lastPlugIns) {  // a new adapter (or the same one again): decide again
        st.lastPlugIns = in.plugIns;
        if (st.reason != wlink::kReasonDeciding) {
            st.reason = wlink::kReasonDeciding;
            if (st.mode == wlink::kModeSpeaker) {
                st.mode = wlink::kModeMicrophone;
                act = Action::ListenAgain;
            }
        }
    }
    if (st.reason != wlink::kReasonDeciding || !in.connected) return act;
    auto decide = [&](uint8_t mode, uint8_t reason) {
        st.mode = mode;
        st.reason = reason;
        return Action::Decided;
    };
    if (in.canPlayback && !in.canCapture) return decide(wlink::kModeSpeaker, wlink::kReasonPlaybackOnly);
    if (in.canCapture && !in.canPlayback) return decide(wlink::kModeMicrophone, wlink::kReasonCaptureOnly);
    if (!in.canCapture) return act;  // neither side usable: stays undecided (reported as not usable)
    if (!in.streaming || !in.captureSinceMs || in.nowMs - in.captureSinceMs < kDecideMs) return act;
    return in.heardNonZero ? decide(wlink::kModeMicrophone, wlink::kReasonSignal)
                           : decide(wlink::kModeSpeaker, wlink::kReasonSilence);
}

}  // namespace modesel
