#pragma once
// Filters for the remote, as biquads in single-precision float (the ESP32-S3 has a float
// unit; about 5 multiply-adds per sample per biquad). Plain C++: tests/feature_tests.cpp.
// - High-pass: 2nd-order Butterworth (12 dB/octave, -3 dB at the cut-off). Removes wind
//   rumble, traffic, HVAC and mains hum below the cut-off before the noise filter.
// - Low-pass (1.4.8): 8th-order Butterworth, four biquads (48 dB/octave, -3 dB at the
//   cut-off). Removes hiss above the cut-off (config.txt lowpass / lowpass_hz).
// - Notch (1.4.8): removes one narrow tone (the AtomS3U's built-in mic, PdmMic.h).
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace hpf {

constexpr uint16_t kMinHz = 50, kMaxHz = 1000, kDefaultHz = 200;
inline bool hzValid(uint32_t hz) { return hz >= kMinHz && hz <= kMaxHz; }

// Low-pass cut-off (config.txt lowpass_hz), Hz. Off by default.
constexpr uint16_t kLowMinHz = 2000, kLowMaxHz = 20000, kLowDefaultHz = 12000;
inline bool lowHzValid(uint32_t hz) { return hz >= kLowMinHz && hz <= kLowMaxHz; }

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
    // Low-pass section with quality q (Q = 1/sqrt(2): one 2nd-order Butterworth).
    void setLowPass(float cutoffHz, float sampleRate, float q = 0.70710678f) {
        const float w0 = 2.0f * float(M_PI) * cutoffHz / sampleRate;
        const float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0_ = (1.0f - c) / 2.0f / a0;
        b1_ = (1.0f - c) / a0;
        b2_ = b0_;
        a1_ = -2.0f * c / a0;
        a2_ = (1.0f - alpha) / a0;
    }
    // Notch: full cut at centreHz, -3 dB points about centreHz / q apart.
    void setNotch(float centreHz, float sampleRate, float q) {
        const float w0 = 2.0f * float(M_PI) * centreHz / sampleRate;
        const float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0_ = 1.0f / a0;
        b1_ = -2.0f * c / a0;
        b2_ = b0_;
        a1_ = b1_;
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

// 8th-order Butterworth low-pass: four biquads with the Butterworth section Qs.
class LowPass8 {
public:
    void set(float cutoffHz, float sampleRate) {
        static constexpr float kQ[4] = {0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f};
        for (int i = 0; i < 4; ++i) s_[i].setLowPass(cutoffHz, sampleRate, kQ[i]);
    }
    void reset() { for (Biquad& b : s_) b.reset(); }
    void process(int16_t* x, size_t n) { for (Biquad& b : s_) b.process(x, n); }

private:
    Biquad s_[4];
};

}  // namespace hpf
