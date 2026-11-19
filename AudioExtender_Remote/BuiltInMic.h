#pragma once
// Wireless USB Audio Extender - REMOTE: the AtomS3U's built-in PDM microphone (SPM1423).
//
// The ESP32-S3's I2S peripheral reads the PDM bit stream (clock G39, data G38) and its
// hardware PDM-to-PCM filter delivers 48 kHz 16-bit samples, so no software decimation.
// Both PDM slots are read: the microphone answers on one of them (its SELECT pin), and the
// first half second picks the slot with the signal. A digital gain (config.txt mic_level,
// 0..100 % = 0..+30 dB, default 100 = +30 dB; x24 = +27.6 dB was right on this mic in the
// esp32-birdnet-mic project) brings the capsule up to a usable level.
// read() hands out left-aligned int32 samples, like usbmic::read(), so the audio path is the
// same for both boards.
// 1.4.8: the mic can pick up a tone from the chip itself at exactly 48 kHz / 7 = 6857.14 Hz
// (strong at cpu_mhz 160, and at times at 240; other mics at the same spot don't have it).
// BoardTone removes it with a narrow notch.
#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <driver/i2s_pdm.h>
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

inline bool start(uint8_t level) {
    setLevel(level);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;  // 5 ms per DMA buffer
    if (i2s_new_channel(&cc, nullptr, &chan()) != ESP_OK) return false;
    i2s_pdm_rx_config_t pc = {};
    pc.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(wlink::kSampleRate);  // PDM clock 3.072 MHz (SPM1423: 1.0-3.25)
    pc.slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    pc.gpio_cfg.clk = gpio_num_t(board::kAtomMicClk);
    for (int i = 1; i < SOC_I2S_PDM_MAX_RX_LINES; ++i) pc.gpio_cfg.dins[i] = I2S_GPIO_UNUSED;
    pc.gpio_cfg.din = gpio_num_t(board::kAtomMicData);
    if (i2s_channel_init_pdm_rx_mode(chan(), &pc) != ESP_OK || i2s_channel_enable(chan()) != ESP_OK) {
        i2s_del_channel(chan());
        chan() = nullptr;
        return false;
    }
    runningFlag() = true;
    return true;
}

// Which slot carries the microphone: 0 / 1, decided after kDecideFrames; -1 = not yet.
struct SlotPick {
    double energy[2] = {0, 0};
    uint32_t frames = 0;
    int8_t slot = -1;
};
inline SlotPick& pick() { static SlotPick p; return p; }
constexpr uint32_t kDecideFrames = wlink::kSampleRate / 2;
inline int slotInUse() { const SlotPick& p = pick(); return p.slot >= 0 ? p.slot : (p.energy[1] > p.energy[0] ? 1 : 0); }
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
            p.energy[0] += double(buf[2 * i]) * buf[2 * i];
            p.energy[1] += double(buf[2 * i + 1]) * buf[2 * i + 1];
        }
        p.frames += frames;
        if (p.frames >= kDecideFrames) p.slot = p.energy[1] > p.energy[0] ? 1 : 0;
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

// Notch for the chip's own tone (applied by the audio task, built-in mic only).
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
