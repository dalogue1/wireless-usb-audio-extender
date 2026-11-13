#pragma once
// USB audio (UAC1) host driver, version 1.3.
#include <stddef.h>
#include <stdint.h>

// USB Audio Class 1.0: pure parser for a configuration descriptor.
// Kept free of Arduino/IDF so the host tests can feed it real descriptors.
// Finds the microphone streaming interface (isochronous IN, PCM Type I) or, for
// Dir::Playback, the speaker/headphone one (isochronous OUT), and the Feature
// Unit on its path (volume / mute / AGC).
namespace uac {

static constexpr uint32_t kRate = 48000;     // the project's fixed USB rate
static constexpr uint8_t kMaxUnits = 24;

enum class Dir : uint8_t { Capture, Playback };

// One streaming alt setting (capture or playback, see Dir).
struct Capture {
    bool found = false;          // usable UAC1 streaming alt setting in the asked direction
    bool uac2 = false;           // an Audio interface with protocol 0x20 (UAC2) was seen
    bool rate48k = false;        // the chosen alt offers 48 kHz
    uint8_t controlInterface = 0xFF;
    uint8_t streamInterface = 0;
    uint8_t alt = 0;
    uint8_t endpoint = 0;        // bEndpointAddress (capture: IN, bit 7 set; playback: OUT)
    uint8_t syncType = 0;        // isochronous synchronisation: 0 none, 1 async, 2 adaptive, 3 sync
    uint16_t maxPacket = 0;
    uint8_t interval = 1;
    uint8_t channels = 0;
    uint8_t subframe = 0;        // bytes per sample: 2, 3 or 4
    uint8_t bits = 0;
    bool rateControl = false;    // endpoint accepts SET_CUR sampling frequency
    uint8_t terminalLink = 0;
    // Feature Unit on the capture path (0 = none).
    uint8_t featureUnit = 0;
    uint8_t volumeChannel = 0xFF; // 0 master, 1.. logical channel; 0xFF none (the first with volume)
    uint8_t volumeMask = 0;       // every channel (bit n = channel n, 0..7) with a volume control
    uint8_t muteChannel = 0xFF;
    uint8_t agcChannel = 0xFF;
};

namespace detail {
struct Unit { uint8_t id, kind, source; uint16_t type; uint8_t ctlSize; const uint8_t* ctl; uint8_t ctlCount; };

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le24(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (uint32_t(p[2]) << 16)); }

// Bitmap for one channel of a Feature Unit (bControlSize bytes, little endian).
inline uint32_t fuControls(const Unit& u, uint8_t ch) {
    if (ch >= u.ctlCount || !u.ctlSize) return 0;
    uint32_t v = 0;
    for (uint8_t b = 0; b < u.ctlSize && b < 4; ++b) v |= uint32_t(u.ctl[ch * u.ctlSize + b]) << (8 * b);
    return v;
}
} // namespace detail

// Bits in a Feature Unit bmaControls entry (UAC1 table 4-7).
static constexpr uint32_t kMute = 0x01, kVolume = 0x02, kAgc = 0x40;

inline bool offers(const uint8_t* fmt, uint32_t rate) {
    // fmt -> FORMAT_TYPE descriptor; byte 7 = bSamFreqType, frequencies from byte 8.
    const uint8_t n = fmt[7];
    if (n == 0) return fmt[0] >= 14 && detail::le24(fmt + 8) <= rate && rate <= detail::le24(fmt + 11);
    for (uint8_t i = 0; i < n && 8 + 3 * (i + 1) <= fmt[0]; ++i)
        if (detail::le24(fmt + 8 + 3 * i) == rate) return true;
    return false;
}

using Playback = Capture;

// Parses a full configuration descriptor (wTotalLength bytes).
inline Capture parse(const uint8_t* d, size_t len, Dir dir = Dir::Capture) {
    const bool wantIn = dir == Dir::Capture;
    using detail::Unit;
    Capture best;
    Unit units[kMaxUnits];
    uint8_t unitCount = 0;
    uint8_t ifNum = 0xFF, ifAlt = 0, ifClass = 0, ifSub = 0;
    // State of the alt setting being read.
    Capture cur;
    const uint8_t* fmt = nullptr;
    bool pcm = false, haveEp = false;
    auto score = [](const Capture& c) {
        // Prefer 48 kHz, then mono (playback: up to 2 channels), then 16-bit; a usable packet size is required.
        return (c.rate48k ? 100 : 0) + (c.channels <= 2 ? 20 : 0) + (c.channels == 1 ? 10 : 0) + (c.subframe == 2 ? 1 : 0);
    };
    auto finish = [&]() {
        if (ifClass == 1 && ifSub == 2 && ifAlt && fmt && pcm && haveEp && bool(cur.endpoint & 0x80) == wantIn) {
            cur.found = cur.channels >= 1 && cur.subframe >= 2 && cur.subframe <= 4 &&
                        cur.maxPacket >= cur.channels * cur.subframe;
            if (cur.found && (!best.found || score(cur) > score(best))) {
                const bool uac2 = best.uac2;
                const uint8_t control = best.controlInterface;
                best = cur;
                best.uac2 = uac2;
                best.controlInterface = control;
            }
        }
        cur = Capture();
        fmt = nullptr; pcm = false; haveEp = false;
    };
    for (size_t at = 0; at + 2 <= len;) {
        const uint8_t* p = d + at;
        const uint8_t bl = p[0], type = p[1];
        if (bl < 2 || at + bl > len) break;
        if (type == 0x04 && bl >= 9) {          // INTERFACE
            finish();
            ifNum = p[2]; ifAlt = p[3]; ifClass = p[5]; ifSub = p[6];
            if (ifClass == 1 && p[7] == 0x20) best.uac2 = true;
            if (ifClass == 1 && ifSub == 1 && p[7] == 0 && best.controlInterface == 0xFF) best.controlInterface = ifNum;
            cur.streamInterface = ifNum; cur.alt = ifAlt;
        } else if (type == 0x24 && ifClass == 1 && ifSub == 1 && bl >= 3) { // AudioControl CS_INTERFACE
            const uint8_t sub = p[2];
            if (unitCount < kMaxUnits) {
                Unit u{0, sub, 0, 0, 0, nullptr, 0};
                if (sub == 2 && bl >= 12) { u.id = p[3]; u.type = detail::le16(p + 4); }               // INPUT_TERMINAL
                else if (sub == 3 && bl >= 9) { u.id = p[3]; u.type = detail::le16(p + 4); u.source = p[7]; } // OUTPUT_TERMINAL
                else if ((sub == 4 || sub == 5) && bl >= 6) { u.id = p[3]; u.source = p[5]; }         // MIXER / SELECTOR: first input
                else if (sub == 6 && bl >= 7) {                                                         // FEATURE_UNIT
                    u.id = p[3]; u.source = p[4]; u.ctlSize = p[5]; u.ctl = p + 6;
                    u.ctlCount = u.ctlSize ? uint8_t((bl - 7) / u.ctlSize) : 0;
                }
                else if (sub == 7 || sub == 8) { if (bl >= 7) { u.id = p[3]; u.source = p[6]; } }        // PROCESSING / EXTENSION
                if (u.id) units[unitCount++] = u;
            }
        } else if (type == 0x24 && ifClass == 1 && ifSub == 2 && ifAlt && bl >= 3) { // AudioStreaming CS_INTERFACE
            if (p[2] == 1 && bl >= 7) { cur.terminalLink = p[3]; pcm = detail::le16(p + 5) == 0x0001; } // AS_GENERAL
            else if (p[2] == 2 && bl >= 8 && p[3] == 1) {                                                  // FORMAT_TYPE I
                fmt = p; cur.channels = p[4]; cur.subframe = p[5]; cur.bits = p[6];
                cur.rate48k = offers(p, kRate);
            }
        } else if (type == 0x05 && bl >= 7 && ifClass == 1 && ifSub == 2 && ifAlt) { // ENDPOINT
            // The first isochronous data endpoint (a feedback endpoint, if any, follows it).
            if ((p[3] & 0x03) == 0x01 && (p[3] & 0x30) == 0 && !haveEp) {
                haveEp = true;
                cur.endpoint = p[2];
                cur.syncType = uint8_t((p[3] >> 2) & 0x03);
                cur.maxPacket = detail::le16(p + 4) & 0x07FF;
                cur.interval = p[6];
            }
        } else if (type == 0x25 && bl >= 4 && haveEp && p[2] == 1) { // CS_ENDPOINT EP_GENERAL
            cur.rateControl = (p[3] & 0x01) != 0;
        }
        at += bl;
    }
    finish();
    if (!best.found) return best;
    auto findUnit = [&](uint8_t id) -> const Unit* {
        for (uint8_t i = 0; i < unitCount; ++i) if (units[i].id == id) return &units[i];
        return nullptr;
    };
    auto useFeatureUnit = [&](const Unit* u) {
        best.featureUnit = u->id;
        const uint8_t chans = u->ctlCount;
        for (uint8_t ch = 0; ch < chans; ++ch) {
            const uint32_t c = detail::fuControls(*u, ch);
            if (c & kVolume) {
                if (best.volumeChannel == 0xFF) best.volumeChannel = ch;
                if (ch < 8) best.volumeMask |= uint8_t(1u << ch);
            }
            if ((c & kMute) && best.muteChannel == 0xFF) best.muteChannel = ch;
            if ((c & kAgc) && best.agcChannel == 0xFF) best.agcChannel = ch;
        }
    };
    if (wantIn) {
        // Walk the capture path from the streaming Output Terminal back to a Feature Unit.
        uint8_t id = best.terminalLink;
        for (uint8_t hop = 0; hop < 10 && id; ++hop) {
            const Unit* u = findUnit(id);
            if (!u) break;
            if (u->kind == 6) { useFeatureUnit(u); break; }
            if (u->kind == 2) break; // reached the Input Terminal
            id = u->source;
        }
    } else {
        // Playback: the Feature Unit whose path leads back to the streaming Input Terminal.
        for (uint8_t i = 0; i < unitCount && !best.featureUnit; ++i) {
            if (units[i].kind != 6) continue;
            uint8_t id = units[i].source;
            for (uint8_t hop = 0; hop < 10 && id; ++hop) {
                const Unit* u = findUnit(id);
                if (!u) break;
                if (u->kind == 2) {
                    if (u->id == best.terminalLink) useFeatureUnit(&units[i]);
                    break;
                }
                id = u->source;
            }
        }
    }
    return best;
}

// One block of mono samples -> one isochronous OUT packet: every channel gets the
// sample, left-aligned for 3- and 4-byte subframes. Returns the bytes written.
inline size_t pack(const int16_t* mono, size_t n, uint8_t channels, uint8_t subframe, uint8_t* out, size_t maxBytes) {
    const size_t frame = size_t(channels) * subframe;
    if (!frame) return 0;
    if (n * frame > maxBytes) n = maxBytes / frame;
    uint8_t* o = out;
    for (size_t i = 0; i < n; ++i) {
        const uint16_t v = uint16_t(mono[i]);
        for (uint8_t ch = 0; ch < channels; ++ch) {
            for (uint8_t b = 0; b + 2 < subframe; ++b) *o++ = 0;  // low bytes of 24/32-bit samples
            *o++ = uint8_t(v & 0xFF);
            *o++ = uint8_t(v >> 8);
        }
    }
    return n * frame;
}

// Byte layout of one Feature Unit volume value: int16, 1/256 dB.
inline int16_t volumeForPercent(int16_t lo, int16_t hi, int16_t res, uint8_t pct) {
    if (pct > 100) pct = 100;
    if (hi < lo) { const int16_t t = lo; lo = hi; hi = t; }
    int32_t v = lo + int32_t(hi - lo) * pct / 100;
    if (res > 0) v = lo + ((v - lo) + res / 2) / res * res;
    if (v > hi) v = hi;
    return int16_t(v);
}

inline uint8_t percentForVolume(int16_t lo, int16_t hi, int16_t v) {
    if (hi <= lo) return 100;
    if (v <= lo) return 0;
    if (v >= hi) return 100;
    return uint8_t((int32_t(v - lo) * 100 + (hi - lo) / 2) / (hi - lo));
}

// One isochronous packet -> left-aligned int32 samples of the first channel.
inline size_t unpack(const uint8_t* data, size_t bytes, uint8_t channels, uint8_t subframe, int32_t* out, size_t maxOut) {
    const size_t frame = size_t(channels) * subframe;
    if (!frame) return 0;
    size_t n = bytes / frame;
    if (n > maxOut) n = maxOut;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* s = data + i * frame;
        int32_t v;
        if (subframe == 2) v = int32_t(uint32_t(s[0]) << 16 | uint32_t(s[1]) << 24);
        else if (subframe == 3) v = int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24);
        else v = int32_t(uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24);
        out[i] = v;
    }
    return n;
}

} // namespace uac
