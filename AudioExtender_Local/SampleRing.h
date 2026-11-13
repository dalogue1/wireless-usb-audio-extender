#pragma once
// Single producer / single consumer ring of 16-bit samples, lock-free.
// Speaker mode on the local board: the USB audio ISR writes the PC's samples, the
// sender task reads them in 12 ms radio packets. Plain C++ (host-tested).
#include <atomic>
#include <stdint.h>

template <uint32_t kCapacity>
class SampleRing {
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");
public:
    // Writer. Returns how many fitted (the rest is dropped: the reader is too slow).
    uint32_t push(const int16_t* s, uint32_t n) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t room = kCapacity - (head - tail_.load(std::memory_order_acquire));
        if (n > room) { dropped_ += n - room; n = room; }
        for (uint32_t i = 0; i < n; ++i) buf_[(head + i) & (kCapacity - 1)] = s[i];
        head_.store(head + n, std::memory_order_release);
        return n;
    }
    // Reader.
    uint32_t available() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed); }
    uint32_t pop(int16_t* out, uint32_t n) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t have = head_.load(std::memory_order_acquire) - tail;
        if (n > have) n = have;
        for (uint32_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & (kCapacity - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }
    void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }  // reader side
    uint32_t dropped() const { return dropped_; }

private:
    int16_t buf_[kCapacity] = {};
    std::atomic<uint32_t> head_{0}, tail_{0};
    uint32_t dropped_ = 0;  // writer-owned
};
