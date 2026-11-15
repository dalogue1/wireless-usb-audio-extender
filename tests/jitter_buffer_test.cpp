// Host simulation of the local board's jitter buffer and 47/49 drift control.
//   g++ -std=c++17 -O2 -I../AudioExtender_Local jitter_buffer_test.cpp -o jb && ./jb
//
// The remote's adapter clock runs at (1 + ppm) relative to the PC's USB clock.
// Packets of wlink::kPacketSamples (576) samples cross a simulated radio with random delay, stalls and
// losses. The PC pulls one frame per millisecond. Samples carry a ramp so the
// test can prove none are dropped, repeated or reordered.
#include "JitterBuffer.h"
#include "LinkProtocol.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <vector>

struct Result {
    JitterBuffer::Stats st;
    float ppmMeasured;
    uint32_t minLevel, maxLevel;
    uint64_t discontinuities;
};

struct Pending { double arriveMs; uint32_t seq; std::vector<int16_t> pcm; };

static Result run(double ppm, double seconds, double jitterMs, double stallEverySec,
                  double stallMs, double lossRate, unsigned seed) {
    std::unique_ptr<JitterBuffer> holder(new JitterBuffer());  // 16 KB: keep off the stack
    JitterBuffer& jb = *holder;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0, 1);
    const double srcRate = 48.0 * (1.0 + ppm * 1e-6);  // samples per PC millisecond
    const double blockMs = JitterBuffer::kNominal ? double(wlink::kPacketSamples) / srcRate : 0;
    std::deque<Pending> air;
    double nextBlockMs = 0, stallUntil = -1, nextStall = stallEverySec > 0 ? stallEverySec * 1000 : 1e18;
    uint32_t seq = 0;
    int16_t ramp = 0;
    jb.streamStart();

    Result r{};
    r.minLevel = UINT32_MAX;
    int16_t expect = 0;
    bool haveExpect = false;
    JitterBuffer::Stats warm{};
    int16_t out[JitterBuffer::kMaxFrame];
    const int totalMs = int(seconds * 1000);
    for (int ms = 0; ms < totalMs; ++ms) {
        // remote produces blocks on its own clock
        while (nextBlockMs <= ms) {
            Pending p;
            p.seq = seq++;
            p.pcm.resize(wlink::kPacketSamples);
            for (auto& s : p.pcm) s = ramp++;
            double delay = 3 + uni(rng) * jitterMs;
            if (nextBlockMs >= nextStall) { stallUntil = nextBlockMs + stallMs; nextStall += stallEverySec * 1000; }
            if (nextBlockMs < stallUntil) delay += stallUntil - nextBlockMs;  // retries pile up, then burst
            p.arriveMs = nextBlockMs + delay;
            if (uni(rng) >= lossRate) air.push_back(std::move(p));
            nextBlockMs += blockMs;
        }
        // radio delivers in order (ESP-NOW unicast with MAC retries)
        while (!air.empty() && air.front().arriveMs <= ms) {
            jb.pushBlock(air.front().seq, air.front().pcm.data(), wlink::kPacketSamples);
            air.pop_front();
        }
        const uint16_t n = jb.frame(out);
        assert(n >= 47 && n <= 49);
        if (ms == 5000) warm = jb.stats();
        if (ms > 5000) {
            const uint32_t lv = jb.level();
            if (lv < r.minLevel) r.minLevel = lv;
            if (lv > r.maxLevel) r.maxLevel = lv;
        }
        if (jb.running()) {
            for (uint16_t i = 0; i < n; ++i) {
                if (haveExpect && out[i] != expect) r.discontinuities++;
                expect = int16_t(out[i] + 1);
                haveExpect = true;
            }
        } else {
            haveExpect = false;  // prefilling: silence
        }
    }
    r.st = jb.stats();
    r.ppmMeasured = JitterBuffer::ppm(warm, r.st);
    return r;
}

static void print(const char* name, double ppm, const Result& r) {
    printf("%-34s src %+7.1f ppm  measured %+7.1f ppm  level %4u..%4u  under %u  lost %u  resync %u  breaks %llu\n",
           name, ppm, r.ppmMeasured, r.minLevel, r.maxLevel, r.st.underruns, r.st.lostBlocks,
           r.st.resyncs, (unsigned long long)r.discontinuities);
}

int main() {
    int failures = 0;
    auto expectTrue = [&](bool ok, const char* what) {
        if (!ok) { printf("  FAIL: %s\n", what); failures++; }
    };

    // Clean link, typical crystal offsets: no underruns, no breaks, ppm tracked.
    for (double ppm : {-200.0, -50.0, 0.0, 30.0, 100.0, 500.0}) {
        Result r = run(ppm, 600, 6, 0, 0, 0, 1);
        print("clean, 10 min", ppm, r);
        expectTrue(r.st.underruns == 0, "no underruns on a clean link");
        expectTrue(r.discontinuities == 0, "every sample delivered once, in order");
        expectTrue(std::fabs(r.ppmMeasured - ppm) < 10 + std::fabs(ppm) * 0.05, "drift measured");
        expectTrue(r.minLevel > 5000 && r.maxLevel < 9500, "level stays near 150 ms");
    }

    // Busy radio: up to 25 ms jitter, a 40 ms stall every 7 s.
    {
        Result r = run(80, 600, 25, 7, 40, 0, 2);
        print("jitter 25 ms + 40 ms stalls", 80, r);
        expectTrue(r.st.underruns == 0, "40 ms stalls absorbed by the 150 ms buffer");
        expectTrue(r.discontinuities == 0, "no breaks with stalls");
    }

    // 1.4.5: hold-ups of 120 ms (busy channel, retries at `far`), then the burst: absorbed.
    {
        Result r = run(80, 600, 25, 5, 120, 0, 5);
        print("jitter 25 ms + 120 ms stalls", 80, r);
        expectTrue(r.st.underruns == 0, "120 ms stalls absorbed by the 150 ms buffer");
        expectTrue(r.discontinuities == 0, "no breaks with 120 ms stalls");
        expectTrue(r.maxLevel < JitterBuffer::kCapacity, "no overflow after the bursts");
    }

    // Lost packets (0.5 %): each loss is 4 ms of silence; timing must hold.
    {
        Result r = run(-60, 300, 6, 0, 0, 0.005, 3);
        print("0.5 % packet loss", -60, r);
        expectTrue(r.st.underruns == 0, "losses do not cause underruns");
        expectTrue(r.st.lostBlocks > 100, "losses counted");
        expectTrue(std::fabs(r.ppmMeasured + 60) < 15, "drift still tracked with losses");
    }

    // Outage longer than the buffer: underrun, then recovers cleanly.
    {
        Result r = run(40, 120, 6, 30, 300, 0, 4);
        print("300 ms outages every 30 s", 40, r);
        expectTrue(r.st.underruns >= 3, "outages underrun");
        expectTrue(r.maxLevel < JitterBuffer::kCapacity, "no overflow after the burst");
    }

    printf(failures ? "\n%d FAILED\n" : "\nall jitter buffer tests passed\n", failures);
    return failures ? 1 : 0;
}
