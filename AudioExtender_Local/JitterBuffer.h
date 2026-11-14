#pragma once
// Jitter buffer + "47/49" clock-drift control.
//
// SHARED FILE: an identical copy lives in AudioExtender_Remote/ and
// AudioExtender_Local/. Edit shared/JitterBuffer.h and run tools/sync_shared.sh.
//
// Microphone (local board): the ESP-NOW receive callback (Wi-Fi task) calls pushBlock()
// with each audio packet (12 ms) as it arrives - bursty, with radio jitter; the USB
// audio ISR calls frame() once per 1 ms USB frame, timed by the PC's USB clock.
// Speaker (remote): the same, the other way round: packets from the local board (timed
// by the PC's clock) in, and the USB host's 1 ms playback packets to the adapter out
// (timed by the remote's own clock), so the adapter receives the PC's true rate.
//
// The buffer holds about kTargetSamples (68 ms; 60 ms before the 12 ms packets) of audio. The remote's audio
// adapter has its own crystal, so it produces slightly more or fewer than
// 48,000 samples per PC second. frame() normally hands 48 samples to USB;
// when the (smoothed) fill level sits above the target it hands 49, below the
// target 47. No sample is altered, dropped or repeated in normal operation:
// the PC simply receives the adapter's true rate, as with any asynchronous
// USB microphone.
//
// Single producer / single consumer, lock-free. Plain C++ (no Arduino), so
// tests/jitter_buffer_test.cpp can simulate it on a PC.
//
// JitterBufferT<kCh>: kCh interleaved channels per frame (1.4.2: the remote's speaker
// buffer is stereo). All counts and positions are in frames (one sample per channel);
// pcm/out hold n * kCh values. JitterBuffer = the mono one.
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

template <uint8_t kCh>
class JitterBufferT {
    static_assert(kCh == 1 || kCh == 2, "mono or stereo");
public:
    static constexpr uint8_t kChannels = kCh;
    static constexpr uint32_t kCapacity = 8192;            // frames (170 ms), power of two
    static constexpr uint32_t kTargetSamples = 3264;       // 68 ms: same stall margin as 60 ms had with 4 ms packets
    static constexpr int32_t kHysteresis = 96;             // +-2 ms around the target
    static constexpr uint32_t kMinFramesBetweenAdjust = 16; // max correction ~1300 ppm
    static constexpr int32_t kResyncAbove = 2400;          // > target + 50 ms: drop to target
    static constexpr uint32_t kMaxGapSamples = 4800;       // fill up to 100 ms of lost packets
    static constexpr uint16_t kNominal = 48;               // samples per 1 ms at 48 kHz
    static constexpr uint16_t kMaxFrame = kNominal + 1;

    struct Stats {
        uint32_t blocks = 0;       // audio blocks received
        uint32_t lostBlocks = 0;   // sequence gaps (filled with silence)
        uint32_t lateBlocks = 0;   // duplicates / out of order (dropped)
        uint32_t overflows = 0;    // blocks dropped because the buffer was full
        uint32_t underruns = 0;    // buffer ran dry while streaming
        uint32_t resyncs = 0;      // buffer far too full: skipped back to target
        uint32_t frames = 0;       // USB frames served
        uint32_t plus = 0;         // 49-sample frames
        uint32_t minus = 0;        // 47-sample frames
    };

    // ---- writer side (one task) ----

    // Starts a new stream: the next block's sequence number is taken as is.
    void streamStart() { haveSeq_ = false; }

    void pushBlock(uint32_t seq, const int16_t* pcm, uint16_t n) {
        stats_.blocks++;
        if (haveSeq_) {
            const int32_t diff = int32_t(seq - nextSeq_);
            if (diff < 0) {  // duplicate or late: already played (or skipped)
                stats_.lateBlocks++;
                return;
            }
            if (diff > 0) {
                stats_.lostBlocks += uint32_t(diff);
                // Keep the timing: fill short gaps with silence. Longer gaps are
                // an outage; the reader has underrun and will re-prefill anyway.
                if (uint32_t(diff) * n <= kMaxGapSamples) pushSilence(uint32_t(diff) * n);
            }
        }
        haveSeq_ = true;
        nextSeq_ = seq + 1;
        if (!push(pcm, n)) stats_.overflows++;
    }

    // ---- reader side (USB ISR) ----

    // Ask the reader to empty the buffer and prefill again (e.g. when the PC
    // starts recording). Safe to call from any context.
    void requestFlush() { flushRequested_.store(true, std::memory_order_release); }

    // Fills one USB frame. Returns the number of frames written to out
    // (47, 48 or 49). Outputs silence while prefilling.
    uint16_t frame(int16_t* out) {
        stats_.frames++;
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        uint32_t head = head_.load(std::memory_order_acquire);
        if (flushRequested_.exchange(false, std::memory_order_acq_rel)) {
            tail_.store(head, std::memory_order_release);
            running_ = false;
            return silence(out, kNominal);
        }
        uint32_t level = head - tail;
        if (!running_) {
            if (level < kTargetSamples) return silence(out, kNominal);
            running_ = true;
            smoothedQ8_ = int32_t(level) << 8;
            sinceAdjust_ = 0;
        }
        // Smoothed level (time constant ~256 frames) hides the 4 ms block sawtooth.
        smoothedQ8_ += ((int32_t(level) << 8) - smoothedQ8_) >> 8;
        const int32_t error = (smoothedQ8_ >> 8) - int32_t(kTargetSamples);

        if (error > kResyncAbove) {  // e.g. after a long stall the radio caught up
            const uint32_t drop = level - kTargetSamples;
            tail_.store(tail + drop, std::memory_order_release);
            smoothedQ8_ = int32_t(kTargetSamples) << 8;
            stats_.resyncs++;
            return frame(out);
        }

        uint16_t n = kNominal;
        if (++sinceAdjust_ >= kMinFramesBetweenAdjust) {
            if (error > kHysteresis) {
                n = kNominal + 1;
                stats_.plus++;
                sinceAdjust_ = 0;
            } else if (error < -kHysteresis) {
                n = kNominal - 1;
                stats_.minus++;
                sinceAdjust_ = 0;
            }
        }
        if (level < n) {  // ran dry: output what is left, then prefill again
            stats_.underruns++;
            running_ = false;
            copyOut(out, tail, level);
            tail_.store(tail + level, std::memory_order_release);
            memset(out + level * kCh, 0, (n - level) * kCh * sizeof(int16_t));
            return n;
        }
        copyOut(out, tail, n);
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // ---- either side (approximate when read across tasks) ----
    uint32_t level() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    bool running() const { return running_; }
    Stats stats() const { return stats_; }
    // Remote clock vs PC clock in ppm, from the corrections made so far
    // (positive: the remote's adapter runs fast).
    static float ppm(const Stats& from, const Stats& to) {
        const uint32_t frames = to.frames - from.frames;
        if (!frames) return 0;
        const int32_t extra = int32_t(to.plus - from.plus) - int32_t(to.minus - from.minus);
        return float(extra) * 1e6f / (float(frames) * kNominal);
    }

private:
    static constexpr uint32_t kMask = kCapacity - 1;
    static_assert((kCapacity & kMask) == 0, "capacity must be a power of two");

    bool push(const int16_t* pcm, uint32_t n) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail + n > kCapacity) return false;
        for (uint32_t i = 0; i < n; ++i)
            for (uint8_t c = 0; c < kCh; ++c) buf_[((head + i) & kMask) * kCh + c] = pcm ? pcm[i * kCh + c] : 0;
        head_.store(head + n, std::memory_order_release);
        return true;
    }
    void pushSilence(uint32_t n) {
        while (n) {
            const uint32_t chunk = n > 256 ? 256 : n;
            if (!push(nullptr, chunk)) { stats_.overflows++; return; }
            n -= chunk;
        }
    }
    void copyOut(int16_t* out, uint32_t tail, uint32_t n) const {
        for (uint32_t i = 0; i < n; ++i)
            for (uint8_t c = 0; c < kCh; ++c) out[i * kCh + c] = buf_[((tail + i) & kMask) * kCh + c];
    }
    uint16_t silence(int16_t* out, uint16_t n) {
        memset(out, 0, n * kCh * sizeof(int16_t));
        return n;
    }

    int16_t buf_[kCapacity * kCh] = {};
    std::atomic<uint32_t> head_{0}, tail_{0};
    std::atomic<bool> flushRequested_{false};
    // writer-owned
    bool haveSeq_ = false;
    uint32_t nextSeq_ = 0;
    // reader-owned
    bool running_ = false;
    int32_t smoothedQ8_ = 0;
    uint32_t sinceAdjust_ = 0;
    // counters: each field has a single writer; readers may see slightly stale values
    Stats stats_;
};

using JitterBuffer = JitterBufferT<1>;
using StereoJitterBuffer = JitterBufferT<2>;
