#pragma once
// High-pass filter for the remote: a 2nd-order Butterworth biquad (12 dB/octave,
// -3 dB at the cut-off), in single-precision float (the ESP32-S3 has a float unit;
// about 5 multiply-adds per sample). Removes wind rumble, traffic, HVAC and mains
// hum below the cut-off before the noise filter. Plain C++: tests/feature_tests.cpp.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace hpf {

constexpr uint16_t kMinHz = 50, kMaxHz = 1000, kDefaultHz = 200;
inline bool hzValid(uint32_t hz) { return hz >= kMinHz && hz <= kMaxHz; }

class Biquad {
public:
    // Coefficients from the Audio EQ Cookbook (R. Bristow-Johnson), Q = 1/sqrt(2).
    void setHighPass(float cutoffHz, float sampleRate) {
        const float w0 = 2.0f * float(M_PI) * cutoffHz / sampleRate;
        const float c = cosf(w0), alpha = sinf(w0) / (2.0f * 0.70710678f);
        const float a0 = 1.0f + alpha;
        b0_ = (1.0f + c) / 2.0f / a0;
        b1_ = -(1.0f + c) / a0;
        b2_ = b0_;
        a1_ = -2.0f * c / a0;
        a2_ = (1.0f - alpha) / a0;
    }
    void reset() { z1_ = z2_ = 0.0f; }

    // In place, transposed direct form II; output rounded and clamped to int16.
    void process(int16_t* x, size_t n) {
        float z1 = z1_, z2 = z2_;
        for (size_t i = 0; i < n; ++i) {
            const float in = float(x[i]);
            const float y = b0_ * in + z1;
            z1 = b1_ * in - a1_ * y + z2;
            z2 = b2_ * in - a2_ * y;
            const float r = y >= 0.0f ? y + 0.5f : y - 0.5f;
            x[i] = r >= 32767.0f ? int16_t(32767) : r <= -32768.0f ? int16_t(-32768) : int16_t(r);
        }
        // After silence the state decays towards tiny (subnormal) values: snap them to 0.
        z1_ = fabsf(z1) < 1e-12f ? 0.0f : z1;
        z2_ = fabsf(z2) < 1e-12f ? 0.0f : z2;
    }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
};

}  // namespace hpf
