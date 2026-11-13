#pragma once
// Mixes spoken announcements into the USB microphone stream.
//
// Writer: the announcer task pushes 48 kHz samples (already at playback level).
// Reader: the USB audio ISR calls mix() on each 1 ms frame after the jitter
// buffer filled it. While an announcement plays, the microphone is turned down
// ("ducked") to -12 dB, with ~16 ms ramps so nothing clicks, and comes back
// 200 ms after the announcement ends.
//
// Single producer / single consumer, lock-free, integer only (runs in an ISR).
// Plain C++: tests/voice_mixer_test.cpp checks it on the host.
#include <atomic>
#include <stdint.h>

class VoiceMixer {
public:
    static constexpr uint32_t kCapacity = 4096;  // samples (85 ms), power of two
    static constexpr int32_t kDuckQ15 = 8192;     // mic level under an announcement (-12 dB)
    static constexpr int32_t kRampPerSample = 32; // 32767 -> 8192 in ~770 samples (16 ms)
    static constexpr uint16_t kHangFrames = 200;  // keep ducked 200 ms after the voice ends

    // ---- writer (announcer task) ----
    uint32_t space() const { return kCapacity - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire)); }
    // Pushes up to n samples; returns how many fitted.
    uint32_t push(const int16_t* s, uint32_t n) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t room = kCapacity - (head - tail_.load(std::memory_order_acquire));
        if (n > room) n = room;
        for (uint32_t i = 0; i < n; ++i) buf_[(head + i) & kMask] = s[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }
    // Drop whatever is queued (recording stopped, mode changed). Any context.
    void requestFlush() { flush_.store(true, std::memory_order_release); }
    bool playing() const { return active_.load(std::memory_order_relaxed); }

    // ---- reader (USB ISR) ----
    void mix(int16_t* pcm, uint16_t n) {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (flush_.exchange(false, std::memory_order_acq_rel)) {
            tail = head;
            tail_.store(tail, std::memory_order_release);
        }
        const uint32_t avail = head - tail;
        const uint32_t take = avail < n ? avail : n;
        if (take) hang_ = kHangFrames;
        else if (hang_) hang_--;
        const int32_t target = (take || hang_) ? kDuckQ15 : 32767;
        active_.store(take != 0 || hang_ != 0, std::memory_order_relaxed);
        if (target == 32767 && duck_ == 32767 && !take) return;  // nothing to do
        for (uint16_t i = 0; i < n; ++i) {
            if (duck_ < target) duck_ = duck_ + kRampPerSample > target ? target : duck_ + kRampPerSample;
            else if (duck_ > target) duck_ = duck_ - kRampPerSample < target ? target : duck_ - kRampPerSample;
            int32_t v = (int32_t(pcm[i]) * duck_) >> 15;
            if (i < take) v += buf_[(tail + i) & kMask];
            pcm[i] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        tail_.store(tail + take, std::memory_order_release);
    }

private:
    static constexpr uint32_t kMask = kCapacity - 1;
    int16_t buf_[kCapacity] = {};
    std::atomic<uint32_t> head_{0}, tail_{0};
    std::atomic<bool> flush_{false}, active_{false};
    int32_t duck_ = 32767;  // reader-owned
    uint16_t hang_ = 0;     // reader-owned
};
