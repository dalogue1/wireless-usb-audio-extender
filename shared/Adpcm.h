#pragma once
// IMA ADPCM (4 bits per sample).
//
// SHARED FILE: an identical copy lives in AudioExtender_Remote/ and
// AudioExtender_Local/. Edit shared/Adpcm.h and run tools/sync_shared.sh.
//
// Two uses:
// * The built-in voice clips (local board): tools/make_voice.py writes the encoder output
//   into voice_clips.h, low nibble first, predictor and step index starting at 0 per clip.
// * Speaker mode's stereo radio link (1.4.2): the local board encodes the PC's left and
//   right channels (Encoder), the remote decodes them (Decoder). Each packet carries the
//   encoder state it started from, so a lost packet never upsets the next one.
// Plain C++ (host-tested).
#include <stdint.h>

namespace adpcm {

static const int16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Decoder {
    int32_t predictor = 0;
    int index = 0;

    int16_t next(uint8_t nibble) {
        const int step = kStepTable[index];
        int diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
        predictor += (nibble & 8) ? -diff : diff;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += kIndexTable[nibble & 15];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        return int16_t(predictor);
    }
};

// The standard IMA encoder. It runs the decoder on its own output, so both stay in step.
struct Encoder {
    Decoder d;

    uint8_t next(int16_t sample) {
        const int step = kStepTable[d.index];
        int diff = int(sample) - int(d.predictor);
        uint8_t nibble = 0;
        if (diff < 0) { nibble = 8; diff = -diff; }
        if (diff >= step) { nibble |= 4; diff -= step; }
        if (diff >= (step >> 1)) { nibble |= 2; diff -= step >> 1; }
        if (diff >= (step >> 2)) nibble |= 1;
        d.next(nibble);
        return nibble;
    }
    // n samples (even) -> n/2 bytes, low nibble first.
    void encode(const int16_t* in, uint32_t n, uint32_t stride, uint8_t* out) {
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            const uint8_t lo = next(in[i * stride]);
            const uint8_t hi = next(in[(i + 1) * stride]);
            out[i >> 1] = uint8_t(lo | (hi << 4));
        }
    }
};

// n samples (even) from n/2 bytes, written every `stride` values (2: one channel of
// interleaved stereo).
inline void decode(Decoder& d, const uint8_t* in, uint32_t n, int16_t* out, uint32_t stride) {
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        const uint8_t b = in[i >> 1];
        out[i * stride] = d.next(b & 15);
        out[(i + 1) * stride] = d.next(b >> 4);
    }
}

// Sample i of a clip, decoding sequentially (call with i = 0, 1, 2, ...).
inline int16_t sampleAt(Decoder& d, const uint8_t* data, uint32_t i) {
    const uint8_t byte = data[i >> 1];
    return d.next((i & 1) ? (byte >> 4) : (byte & 15));
}

}  // namespace adpcm
