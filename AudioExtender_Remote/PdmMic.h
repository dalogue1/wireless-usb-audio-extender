#pragma once
// Wireless USB Audio Extender - REMOTE: a PDM microphone read by the ESP32-S3's I2S peripheral.
//
// 1.4.9: one capture module for every PDM microphone the remote can use (MicDetect.h
// finds which one at start-up):
//   AtomS3U built-in microphone (SPM1423)        clock G39, data G38
//   External PDM Microphone (IM73D122 board in    clock GPIO19 (USB D-), data GPIO20 (USB D+)
//     the OTG adapter's USB-A socket)
//   XIAO ESP32-S3 Sense built-in microphone       clock GPIO42, data GPIO41
//   a PDM microphone wired to the XIAO's D3/D2    clock D3 (GPIO4), data D2 (GPIO3)
// (Before 1.4.9 this was BuiltInMic.h, for the AtomS3U only.)
//
// The S3's hardware PDM-to-PCM filter delivers 48 kHz 16-bit samples, so no software
// decimation. Both PDM slots are read: the microphone answers on one of them (its SELECT
// pin; the External PDM Microphone's jumper JP1), and the first half second picks the slot
// whose signal varies most. Variance, not energy: an idle slot held at one level by the
// data line's pull-down gives a large constant value, which would look "loud" by energy.
// A digital gain (config.txt mic_level, 0..100 % = 0..+30 dB, default 100 = +30 dB; x24 =
// +27.6 dB was right on this mic in the esp32-birdnet-mic project) brings the capsule up to
// a usable level.
// read() hands out left-aligned int32 samples, like usbmic::read(), so the audio path is the
// same for every microphone.
// 1.4.8: the AtomS3U's mic can pick up a tone from the chip itself at exactly 48 kHz / 7 =
// 6857.14 Hz (strong at cpu_mhz 160, and at times at 240; other mics at the same spot don't
// have it). BoardTone removes it with a narrow notch (AtomS3U only).
#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <driver/i2s_pdm.h>
#include <driver/gpio.h>
#include "BoardCommon.h"
#include "LinkProtocol.h"
#include "HighPass.h"

namespace pdmmic {

constexpr float kMaxGainDb = 30.0f;
constexpr float kDefaultGainDb = 30.0f;  // mic_level = adapter (kLevelDefault): nothing to leave alone, so the default

inline i2s_chan_handle_t& chan() { static i2s_chan_handle_t h = nullptr; return h; }
inline std::atomic<bool>& runningFlag() { static std::atomic<bool> r{false}; return r; }
inline std::atomic<int32_t>& gainQ12() { static std::atomic<int32_t> g{4096}; return g; }  // x4096
inline std::atomic<int16_t>& gainDb256() { static std::atomic<int16_t> g{0}; return g; }

// mic_level: 0..100 % of 0..+30 dB, or wlink::kLevelDefault (= 100 %).
inline void setLevel(uint8_t level) {
    const float db = level == wlink::kLevelDefault ? kDefaultGainDb : kMaxGainDb * (level > 100 ? 100 : level) / 100.0f;
    gainQ12() = int32_t(lroundf(4096.0f * powf(10.0f, db / 20.0f)));
    gainDb256() = int16_t(lroundf(db * 256.0f));
}

inline bool running() { return runningFlag().load(); }

// Where the microphone is wired.
struct Pins {
    int clk = -1, data = -1;
    bool weakClock = false;  // clock at the weakest drive (a cable: softer edges, less crosstalk into DATA)
};

// Opens I2S0 in PDM receive mode on the pins: the clock (3.072 MHz) starts at once. The data
// line gets a pull-down, so a slot (or a socket) without a microphone reads as a steady low,
// not a floating input. Used by start() and by the start-up check (MicDetect.h).
inline bool openChannel(const Pins& p) {
    if (chan()) return false;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);  // the S3 has PDM RX on I2S0 only
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;  // 5 ms per DMA buffer
    if (i2s_new_channel(&cc, nullptr, &chan()) != ESP_OK) {
        chan() = nullptr;
        return false;
    }
    i2s_pdm_rx_config_t pc = {};
    pc.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(wlink::kSampleRate);  // PDM clock 3.072 MHz (SPM1423 1.0-3.25, IM73D122 to 3.3)
    pc.slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    pc.gpio_cfg.clk = gpio_num_t(p.clk);
    for (int i = 1; i < SOC_I2S_PDM_MAX_RX_LINES; ++i) pc.gpio_cfg.dins[i] = I2S_GPIO_UNUSED;
    pc.gpio_cfg.din = gpio_num_t(p.data);
    if (i2s_channel_init_pdm_rx_mode(chan(), &pc) != ESP_OK || i2s_channel_enable(chan()) != ESP_OK) {
        i2s_del_channel(chan());
        chan() = nullptr;
        return false;
    }
    gpio_pulldown_en(gpio_num_t(p.data));
    gpio_pullup_dis(gpio_num_t(p.data));
    if (p.weakClock) gpio_set_drive_capability(gpio_num_t(p.clk), GPIO_DRIVE_CAP_0);
    return true;
}

// Stops the clock and lets go of both pins (inputs with a pull-down: nothing driven).
inline void closeChannel(const Pins& p) {
    runningFlag() = false;
    if (chan()) {
        i2s_channel_disable(chan());
        i2s_del_channel(chan());
        chan() = nullptr;
    }
    for (int pin : {p.clk, p.data}) {
        if (pin < 0) continue;
        gpio_set_direction(gpio_num_t(pin), GPIO_MODE_INPUT);
        gpio_pullup_dis(gpio_num_t(pin));
        gpio_pulldown_en(gpio_num_t(pin));
    }
}

inline bool start(uint8_t level, const Pins& p) {
    setLevel(level);
    if (!openChannel(p)) return false;
    runningFlag() = true;
    return true;
}

// Which slot carries the microphone: 0 / 1, decided after kDecideFrames; -1 = not yet.
// Picked by variance (sum, sum of squares per slot).
struct SlotPick {
    double sum[2] = {0, 0}, sq[2] = {0, 0};
    uint32_t frames = 0;
    int8_t slot = -1;
    double variance(int s) const {
        if (!frames) return 0;
        const double m = sum[s] / frames;
        return sq[s] / frames - m * m;
    }
    int better() const { return variance(1) > variance(0) ? 1 : 0; }
};
inline SlotPick& pick() { static SlotPick p; return p; }
constexpr uint32_t kDecideFrames = wlink::kSampleRate / 2;
inline int slotInUse() { const SlotPick& p = pick(); return p.slot >= 0 ? p.slot : p.better(); }
inline bool slotDecided() { return pick().slot >= 0; }

// Up to maxSamples mono samples, left-aligned int32; waits up to timeoutMs.
inline size_t read(int32_t* out, size_t maxSamples, uint32_t timeoutMs) {
    if (!running() || !maxSamples) return 0;
    static int16_t buf[2 * 192];
    if (maxSamples > 192) maxSamples = 192;
    size_t bytes = 0;
    if (i2s_channel_read(chan(), buf, maxSamples * 2 * sizeof(int16_t), &bytes, timeoutMs) != ESP_OK && !bytes) return 0;
    const size_t frames = bytes / (2 * sizeof(int16_t));
    SlotPick& p = pick();
    if (p.slot < 0) {
        for (size_t i = 0; i < frames; ++i) {
            for (int s = 0; s < 2; ++s) {
                const double v = buf[2 * i + s];
                p.sum[s] += v;
                p.sq[s] += v * v;
            }
        }
        p.frames += frames;
        if (p.frames >= kDecideFrames) p.slot = int8_t(p.better());
    }
    const int s = slotInUse();
    const int32_t g = gainQ12().load();
    for (size_t i = 0; i < frames; ++i) {
        int32_t v = (int32_t(buf[2 * i + s]) * g) >> 12;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = v << 16;
    }
    return frames;
}

// Notch for the chip's own tone (applied by the audio task, AtomS3U built-in mic only).
// Q 25: -3 dB points about 270 Hz apart; the rest of the band is untouched.
constexpr float kBoardToneHz = float(wlink::kSampleRate) / 7.0f;
constexpr float kBoardToneQ = 25.0f;
class BoardTone {
public:
    BoardTone() { n_.setNotch(kBoardToneHz, float(wlink::kSampleRate), kBoardToneQ); }
    void process(int16_t* x, size_t n) { n_.process(x, n); }

private:
    hpf::Biquad n_;
};

}  // namespace pdmmic
