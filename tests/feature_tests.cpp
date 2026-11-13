// Host tests for the 1.1 features: BOOT-button menu, announcement mixer,
// WAV parsing and the built-in ADPCM voice clips.
//   g++ -std=c++17 -I../AudioExtender_Local feature_tests.cpp && ./a.out
#include "HoldButton.h"
#include "UsbWatch.h"
#include "ConfigFile.h"
#include "VoiceMixer.h"
#include "WavFormat.h"
#include "Adpcm.h"
#include "voice_clips.h"
#include "HighPass.h"
#include "NoiseSvg.h"
#include "ReportHtml.h"
#include "SampleRing.h"
#include "../AudioExtender_Remote/ModeDecision.h"
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); failures++; } } while (0)

static void buttonTests() {
    using A = HoldButton::Action;
    HoldButton b;
    uint32_t t = 1000;
    auto hold = [&](uint32_t ms) {
        A last = A::None;
        for (uint32_t i = 0; i < ms; i += 5) { auto a = b.update(t += 5, true); if (a != A::None) last = a; }
        for (int i = 0; i < 20; ++i) { auto a = b.update(t += 5, false); if (a != A::None) last = a; }
        return last;
    };
    CHECK(hold(500) == A::Click, "a click");
    CHECK(hold(3500) == A::None, "a 3 s hold does nothing");
    CHECK(hold(1900) == A::Click && hold(2100) == A::None, "click means released within 2 s");
    CHECK(hold(10500) == A::Hold10, "10 s hold");
    for (int i = 0; i < 50; ++i) b.update(t += 2, (i % 5) == 0);
    CHECK(b.heldMs(t) == 0, "contact bounce ignored");
    b.update(t += 5, true); b.update(t += 40, true);
    CHECK(b.heldMs(t + 3000) >= 3000, "held time reported while pressed");
}

static void configTests() {
    cfg::Settings base;
    base.mic = 2; base.channel = 6;
    cfg::Result r = cfg::parse("# comment\r\nMIC = 7\r\nchannel: 1   # note\nfeedback=Off\nnoise_filter = strong\nnoise_adapt = slow\ntx_power = 8\n", base);
    CHECK(r.errors == 0 && r.settings.mic == 7 && r.settings.channel == 1 && r.settings.feedback == 1 &&
          r.settings.filter == 1 && r.settings.adapt == 1 && r.settings.txPower == 8, "all keys, mixed case, ':' and comments");
    r = cfg::parse("feedback = off\n", base);
    CHECK(r.errors == 0 && r.settings.mic == 2 && r.settings.channel == 6 && r.settings.feedback == cfg::kFeedbackOff, "missing keys keep the current values");
    r = cfg::parse("mic_level = 40\n", base);
    CHECK(r.errors == 0 && r.settings.micLevel == 40, "mic_level 40");
    r = cfg::parse("Mic_Level: Default\n", r.settings);
    CHECK(r.errors == 0 && r.settings.micLevel == cfg::kLevelDefault, "mic_level default");
    CHECK(cfg::Settings().micLevel == cfg::kLevelDefault, "mic_level defaults to the adapter's own");
    r = cfg::parse("mic_level = 101\nmic_level = -1\nmic_level = loud\n", base);
    CHECK(r.errors == 3 && r.settings.micLevel == base.micLevel, "mic_level out of range keeps the value");
    {
        cfg::Settings z;
        z.micLevel = 0;
        char t2[2048];
        const int len = cfg::format(z, t2, sizeof(t2));
        CHECK(len > 0 && len < 2600, "config.txt fits the local board's 2600-byte buffer");
        CHECK(cfg::parse(t2, cfg::Settings()).settings.micLevel == 0, "mic_level 0 round-trips");
        CHECK(strstr(t2, "mic_level = 0 ") != nullptr, "mic_level written");
    }
    r = cfg::parse("feedback = beeps\n", base);
    CHECK(r.errors == 1 && r.settings.feedback == cfg::kFeedbackVoice, "beeps is no longer a setting (a voice pack instead)");
    CHECK(strcmp(cfg::feedbackName(cfg::kFeedbackOff), "off") == 0 && strcmp(cfg::feedbackName(7), "voice") == 0, "feedback names");
    r = cfg::parse("mic = 10\nchannel = 12\nnoise_filter = maybe\nhello\nmic = 1\n", base);
    CHECK(r.errors == 4 && r.settings.mic == 1 && r.settings.channel == 6 && r.settings.filter == 0, "bad lines reported, others applied");
    CHECK(strstr(r.firstError, "line 1") != nullptr, "first bad line named");
    r = cfg::parse("tx_power = MAX\n", base);
    CHECK(r.errors == 0 && r.settings.txPower == 20, "tx_power max");
    r = cfg::parse("tx_power = 1\n", base);
    CHECK(r.errors == 1 && r.settings.txPower == 20, "tx_power out of range refused");
    r = cfg::parse("cpu_mhz = 160\n", base);
    CHECK(r.errors == 0 && r.settings.cpuMhz == 160, "cpu_mhz 160");
    r = cfg::parse("cpu_mhz = 80\ncpu_mhz = 200\n", base);
    CHECK(r.errors == 2 && r.settings.cpuMhz == 240, "cpu_mhz other than 240/160 refused, default 240");
    r = cfg::parse("highpass = OFF\nhighpass_hz = 350\n", base);
    CHECK(r.errors == 0 && r.settings.highpass == 0 && r.settings.highpassHz == 350, "highpass off, 350 Hz");
    r = cfg::parse("highpass = maybe\nhighpass_hz = 20\nhighpass_hz = 5000\n", base);
    CHECK(r.errors == 3 && r.settings.highpass == 1 && r.settings.highpassHz == 200, "bad highpass values refused, defaults on / 200 Hz");
    r = cfg::parse("temperature = off\n", base);
    CHECK(r.errors == 0 && r.settings.temperature == 0, "temperature off");
    r = cfg::parse("temperature = 1\n", base);
    CHECK(r.errors == 1 && r.settings.temperature == 1, "temperature only on/off, default on");
    r = cfg::parse("", base);
    CHECK(r.errors == 0 && r.settings == base, "empty file changes nothing");
    // 1.3.6: distance, mode, remote (pairing by MAC)
    r = cfg::parse("distance = far\nmode = speaker\nremote = 44:bd:8d:76:ca:44\n", base);
    CHECK(r.errors == 0 && r.settings.distance == 2 && r.settings.mode == 2 && r.settings.lock &&
          r.settings.remoteMac[0] == 0x44 && r.settings.remoteMac[5] == 0x44, "distance, mode, remote MAC");
    CHECK(strcmp(cfg::remoteText(r.settings), "44:BD:8D:76:CA:44") == 0, "remote MAC written in upper case");
    r = cfg::parse("remote: 28-84-85-B3-9C-08\nmode = mic\ndistance = NEAR\n", base);
    CHECK(r.errors == 0 && r.settings.lock && r.settings.remoteMac[0] == 0x28 && r.settings.mode == 1 && r.settings.distance == 1,
          "remote with ':' separator and '-' in the MAC; mode = mic; NEAR");
    r = cfg::parse("remote = any\n", r.settings);
    CHECK(r.errors == 0 && !r.settings.lock && r.settings.remoteMac[0] == 0, "remote = any unlocks");
    r = cfg::parse("remote = 44:BD:8D:76:CA\nremote = 00:00:00:00:00:00\nremote = ff:ff:ff:ff:ff:ff\nremote = 44:BD:8D:76:CA:4G\n"
                   "distance = 6\nmode = both\n", base);
    CHECK(r.errors == 6 && !r.settings.lock && r.settings.distance == 0 && r.settings.mode == 0, "bad MACs, distance and mode refused");
    CHECK(cfg::Settings().distance == 0 && cfg::Settings().mode == 0 && !cfg::Settings().lock, "defaults: normal, auto, any");
    char text[2600];
    cfg::Settings d;
    d.mic = 2; d.channel = 9; d.feedback = cfg::kFeedbackOff; d.filter = 1; d.adapt = 2; d.txPower = 11; d.cpuMhz = 160; d.highpass = 0; d.highpassHz = 1000; d.temperature = 0;
    d.distance = 2; d.mode = 1; d.lock = true;
    const uint8_t mac[6] = {0x44, 0xBD, 0x8D, 0x76, 0xCA, 0x44};
    memcpy(d.remoteMac, mac, 6);
    cfg::format(d, text, sizeof(text));
    r = cfg::parse(text, cfg::Settings());
    CHECK(r.errors == 0 && r.settings == d, "the written file reads back the same");
    cfg::format(cfg::Settings(), text, sizeof(text));
    r = cfg::parse(text, base);
    CHECK(r.settings == cfg::Settings(), "default file gives the defaults");
}

// Gain in dB of the high-pass filter for a sine of frequency f (48 kHz, after settling).
static double hpGainDb(float cutoff, double f) {
    hpf::Biquad h;
    h.setHighPass(cutoff, 48000.0f);
    const int n = 48000 * 2;
    std::vector<int16_t> x(n);
    for (int i = 0; i < n; ++i) x[i] = int16_t(lround(16000.0 * sin(2 * M_PI * f * i / 48000.0)));
    for (int i = 0; i < n; i += 192) h.process(x.data() + i, std::min(192, n - i));
    double in = 0, out = 0;
    for (int i = n / 2; i < n; ++i) {
        const double s = 16000.0 * sin(2 * M_PI * f * i / 48000.0);
        in += s * s;
        out += double(x[i]) * x[i];
    }
    return 10 * log10(out / in);
}

static void highpassTests() {
    // 2nd-order Butterworth: |H|^2 = f^4 / (f^4 + fc^4)
    struct { float fc; double f; } cases[] = {{200, 200}, {200, 100}, {200, 50}, {200, 2000}, {200, 8000},
                                             {50, 50}, {50, 25}, {1000, 1000}, {1000, 500}, {1000, 4000}};
    for (auto c : cases) {
        const double want = 10 * log10(pow(c.f, 4) / (pow(c.f, 4) + pow(c.fc, 4)));
        const double got = hpGainDb(c.fc, c.f);
        if (fabs(got - want) > 0.15) { printf("  high-pass %g Hz at %g Hz: %.2f dB, want %.2f\n", c.fc, c.f, got, want); failures++; }
    }
    // DC offset removed, full-scale input clamps instead of wrapping, silence stays silent.
    hpf::Biquad h;
    h.setHighPass(200, 48000);
    std::vector<int16_t> x(48000, 5000);
    h.process(x.data(), x.size());
    CHECK(std::abs(x.back()) <= 1, "DC offset removed");
    std::vector<int16_t> sq(48000);
    for (size_t i = 0; i < sq.size(); ++i) sq[i] = (i / 120) % 2 ? 32767 : -32768;  // 200 Hz full-scale square
    h.reset();
    h.process(sq.data(), sq.size());
    bool wrapped = false;
    for (size_t i = 1; i < sq.size(); ++i)
        if (std::abs(int32_t(sq[i]) - sq[i - 1]) > 60000 && (i % 120) != 0) wrapped = true;
    CHECK(!wrapped, "full-scale input clamps, no wrap-around");
    std::vector<int16_t> z(48000, 0);
    h.process(z.data(), z.size());
    h.process(z.data(), z.size());
    CHECK(z.back() == 0, "silence decays to exact silence");
    CHECK(!hpf::hzValid(49) && hpf::hzValid(50) && hpf::hzValid(1000) && !hpf::hzValid(1001), "cut-off range 50..1000 Hz");
}

static void mixerTests() {
    std::unique_ptr<VoiceMixer> vm(new VoiceMixer());
    int16_t frame[48];
    for (auto& s : frame) s = 10000;
    vm->mix(frame, 48);
    CHECK(frame[0] == 10000, "no voice: microphone untouched");
    std::vector<int16_t> voice(4000, 1000);
    CHECK(vm->push(voice.data(), voice.size()) == voice.size(), "push 83 ms");
    CHECK(vm->push(voice.data(), 200) == 96, "push stops at the 4096-sample capacity");
    int32_t minMic = 32767;
    for (int f = 0; f < 70; ++f) {
        for (auto& s : frame) s = 10000;
        vm->mix(frame, 48);
        if (f > 30) minMic = std::min<int32_t>(minMic, frame[47] - 1000);
    }
    CHECK(std::abs(minMic - 10000 / 4) < 50, "microphone ducked to -12 dB under the voice");
    // after the voice: 200 ms hang, then back up
    for (int f = 0; f < 260; ++f) { for (auto& s : frame) s = 10000; vm->mix(frame, 48); }
    CHECK(frame[47] == 10000 && !vm->playing(), "microphone back to full level afterwards");
    // flush
    vm->push(voice.data(), 1000);
    vm->requestFlush();
    for (auto& s : frame) s = 0;
    vm->mix(frame, 48);
    CHECK(frame[0] == 0, "flush drops queued voice");
    // clipping
    vm->push(voice.data(), 48);
    for (auto& s : frame) s = 32767;
    std::vector<int16_t> loud(48, 32767);
    VoiceMixer* v2 = new VoiceMixer();
    v2->push(loud.data(), 48);
    v2->mix(frame, 48);
    CHECK(frame[0] == 32767, "mix saturates instead of wrapping");
    delete v2;
}

static std::vector<uint8_t> makeWav(uint16_t fmt, uint16_t ch, uint32_t rate, uint16_t bits, uint32_t frames, bool listChunk) {
    std::vector<uint8_t> v;
    auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); };
    auto u16 = [&](uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    const uint32_t data = frames * ch * bits / 8;
    v.insert(v.end(), {'R', 'I', 'F', 'F'}); u32(0); v.insert(v.end(), {'W', 'A', 'V', 'E'});
    v.insert(v.end(), {'f', 'm', 't', ' '}); u32(16); u16(fmt); u16(ch); u32(rate); u32(rate * ch * bits / 8); u16(ch * bits / 8); u16(bits);
    if (listChunk) { v.insert(v.end(), {'L', 'I', 'S', 'T'}); u32(5); v.insert(v.end(), {'a', 'b', 'c', 'd', 'e', 0}); }
    v.insert(v.end(), {'d', 'a', 't', 'a'}); u32(data);
    v.resize(v.size() + data, 0);
    return v;
}

static void wavTests() {
    auto ok = makeWav(1, 1, 44100, 16, 44100, false);
    wav::Info i = wav::parse(ok.data(), 512, ok.size());
    CHECK(i.ok && i.rate == 44100 && i.channels == 1 && i.dataOffset == 44 && i.frames() == 44100, "plain mono WAV");
    auto st = makeWav(1, 2, 48000, 16, 48000, true);
    i = wav::parse(st.data(), 512, st.size());
    CHECK(i.ok && i.channels == 2 && i.dataOffset == 44 + 14, "stereo with a LIST chunk (odd size padded)");
    auto f32 = makeWav(3, 1, 48000, 32, 4800, false);
    CHECK(!wav::parse(f32.data(), 512, f32.size()).ok, "float WAV refused");
    auto b8 = makeWav(1, 1, 16000, 8, 16000, false);
    CHECK(!wav::parse(b8.data(), 512, b8.size()).ok, "8-bit refused");
    auto hi = makeWav(1, 1, 96000, 16, 96000, false);
    CHECK(!wav::parse(hi.data(), 512, hi.size()).ok, "96 kHz refused");
    const char junk[] = "this is not a wav file at all.......";
    CHECK(!wav::parse((const uint8_t*)junk, sizeof(junk), sizeof(junk)).ok, "garbage refused");
    auto cut = makeWav(1, 1, 16000, 16, 16000, false);
    i = wav::parse(cut.data(), 512, 1044);  // truncated file
    CHECK(i.ok && i.dataBytes == 1000, "truncated data clipped to the file size");
}

static void clipTests() {
    int present = 0;
    for (const BuiltinClip& c : kBuiltinClips) {
        if (!c.data) continue;
        present++;
        adpcm::Decoder d;
        int32_t peak = 0;
        for (uint32_t n = 0; n < c.samples; ++n) peak = std::max<int32_t>(peak, std::abs(int32_t(adpcm::sampleAt(d, c.data, n))));
        const double db = 20 * std::log10(peak / 32768.0);
        if (std::fabs(db + 1.0) > 1.5) { printf("  clip %s peak %.1f dBFS\n", c.name, db); failures++; }
        CHECK(c.samples > 4000 && c.samples < 3 * 16000, "clip length 0.25..3 s");
    }
    printf("built-in clips: %d of %zu present\n", present, sizeof(kBuiltinClips) / sizeof(kBuiltinClips[0]));
    CHECK(present == 18, "all 18 built-in clips present");
}

// USB recovery decisions (UsbWatch.h), 10 ms steps of simulated time.
static void usbWatchTests() {
    using A = UsbWatch::Action;
    struct Sim {
        UsbWatch w;
        uint32_t t = 0, frames = 0;
        UsbWatch::Input in{true, false, false, 0};
        bool flowing = false;   // the PC collects audio (frames advance)
        int notes = 0, clears = 0, unset = 0, stall = 0, restarts = 0;
        uint32_t lastUnsetWait = 0;
        void run(uint32_t ms) {
            for (uint32_t end = t + ms; t < end; t += 10) {
                if (flowing && in.recording && in.configured && !in.suspended) frames += 10;
                in.frames = frames;
                switch (w.update(t, in)) {
                    case A::StallNoted: notes++; break;
                    case A::StallCleared: clears++; break;
                    case A::ReattachUnset: unset++; lastUnsetWait = w.waitedMs; w.reattached(t, frames); break;
                    case A::ReattachStall: stall++; w.reattached(t, frames); break;
                    case A::Restart: restarts++; break;
                    default: break;
                }
            }
        }
    };
    {   // Normal: set up, recording, audio flowing for an hour; closing the stream is no stall.
        Sim s;
        s.in.recording = true;
        s.flowing = true;
        s.run(3600000);
        s.in.recording = false;
        s.run(60000);
        CHECK(s.notes + s.unset + s.stall + s.restarts == 0, "usb: normal use needs no recovery");
    }
    {   // PC asleep for 8 h with the stream open: left alone.
        Sim s;
        s.in.recording = true;
        s.flowing = true;
        s.run(10000);
        s.in.suspended = true;
        s.run(8 * 3600000u);
        s.in.suspended = false;
        s.run(10000);
        CHECK(s.notes + s.unset + s.stall + s.restarts == 0, "usb: a sleeping PC is left alone");
    }
    {   // Never set up: re-attach at 30 s, then 60, 120, 240, 480, 600, 600 ... s.
        Sim s;
        s.in.configured = false;
        s.run(29990);
        CHECK(s.unset == 0, "usb: no re-attach before 30 s");
        s.run(20);
        CHECK(s.unset == 1 && s.lastUnsetWait == 30000, "usb: re-attach at 30 s");
        s.run(60000 + 120000 + 240000 + 480000 - 20);
        CHECK(s.unset == 4 && s.lastUnsetWait == 240000, "usb: back-off doubles");
        s.run(20 + 600000);
        CHECK(s.unset == 6 && s.lastUnsetWait == 600000, "usb: capped at 10 min");
        s.run(600000);
        CHECK(s.unset == 7 && s.lastUnsetWait == 600000, "usb: then every 10 min");
        s.in.configured = true;  // the PC finally sets it up: back to 30 s for next time
        s.run(1000);
        s.in.configured = false;
        s.run(30010);
        CHECK(s.unset == 8 && s.lastUnsetWait == 30000, "usb: wait starts again at 30 s");
        CHECK(s.restarts == 0, "usb: an absent PC never restarts the board");
    }
    {   // A short unplug / re-enumeration (2 s) does nothing.
        Sim s;
        s.run(60000);
        s.in.configured = false;
        s.run(2000);
        s.in.configured = true;
        s.run(60000);
        CHECK(s.unset == 0, "usb: a quick re-enumeration needs no re-attach");
    }
    {   // Stream stalls: note at 1 s, re-attach at 5 s; audio back = no restart.
        Sim s;
        s.in.recording = true;
        s.flowing = true;
        s.run(60000);
        s.flowing = false;
        s.run(990);
        CHECK(s.notes == 0, "usb: a stall under 1 s is not noted");
        s.run(20);
        CHECK(s.notes == 1, "usb: stall noted at 1 s");
        s.run(4000);
        CHECK(s.stall == 1 && s.restarts == 0, "usb: re-attach at 5 s");
        s.flowing = true;
        s.run(300000);
        CHECK(s.stall == 1 && s.restarts == 0, "usb: flowing again, nothing more");
        s.flowing = false;  // a new stall long after: another re-attach, not a restart
        s.run(6000);
        CHECK(s.stall == 2 && s.restarts == 0, "usb: a stall 5 min later only re-attaches");
        s.flowing = true;
        s.run(30000);
        s.flowing = false;  // and again within 2 min: restart
        s.run(6000);
        CHECK(s.restarts >= 1, "usb: a second stall within 2 min restarts the board");
    }
    {   // Stall cleared by itself (the PC resumes collecting after 2 s).
        Sim s;
        s.in.recording = true;
        s.flowing = true;
        s.run(10000);
        s.flowing = false;
        s.run(2000);
        s.flowing = true;
        s.run(1000);
        CHECK(s.notes == 1 && s.clears == 1 && s.stall == 0, "usb: a 2 s stall clears without re-attach");
        CHECK(s.w.stalledMs >= 2000 && s.w.stalledMs <= 2100, "usb: stall length reported");
    }
    {   // millis() wrap-around (49.7 days) changes nothing.
        Sim s;
        s.t = 0xFFFFFFFFu - 100000;
        s.in.recording = true;
        s.flowing = true;
        s.run(300000);
        CHECK(s.notes + s.unset + s.stall + s.restarts == 0, "usb: millis wrap is harmless");
    }
}

static std::string g_out;
static void sinkStr(void*, const char* t) { g_out += t; }

static void svgReportTests() {
    int16_t bins[noisesvg::kBins];
    for (int k = 0; k < noisesvg::kBins; ++k) bins[k] = int16_t(-900 - k);  // -90 .. -115.6 dB
    bins[0] = noisesvg::kNone;
    noisesvg::Info in;
    in.deciDb = bins;
    in.title = "Extender Mic 1 - background noise <test> & more";
    in.line1 = "learned over 1:00:00";
    in.line2 = "noise_filter on (12 dB) | highpass on, 200 Hz";
    in.highpassOn = true;
    in.highpassHz = 200;
    g_out.clear();
    noisesvg::Out o{sinkStr, nullptr};
    noisesvg::render(o, in);
    CHECK(g_out.rfind("<svg", 0) == 0 && g_out.find("</svg>") != std::string::npos, "svg: a whole document");
    CHECK(g_out.find("&lt;test&gt; &amp; more") != std::string::npos && g_out.find("<test>") == std::string::npos, "svg: text escaped");
    CHECK(g_out.find("high-pass 200 Hz") != std::string::npos && g_out.find("birds 1-10 kHz") != std::string::npos, "svg: markings");
    size_t pts = 0;
    for (size_t at = g_out.find("points=\""); at != std::string::npos && at < g_out.size(); ++at) {
        if (g_out[at] == ',') pts++;
        if (g_out[at] == '/' ) break;
    }
    CHECK(pts == noisesvg::kBins - 1, "svg: one point per bin (the missing one left out)");
    CHECK(g_out.size() > 4000 && g_out.size() < 20000, "svg: a few KB");
    // A tiny HTML report with an empty log.
    g_out.clear();
    report::Out r{sinkStr, nullptr};
    report::begin(r, "USB Audio Extender - Extender Mic 1");
    noisesvg::render(r, in);
    evlog::Log log;  // no flash: says so
    report::logSections(r, log, 100);
    report::end(r);
    CHECK(g_out.find("<!DOCTYPE html>") == 0 && g_out.find("</html>") != std::string::npos, "report: a whole page");
    CHECK(g_out.find("width:100%") != std::string::npos, "report: the style is complete");
    CHECK(g_out.find("flash the full image") != std::string::npos, "report: no event log explained");
    CHECK(g_out.find("http") == g_out.find("http://www.w3.org"), "report: no external links (only the SVG namespace)");
    // Sample ring (speaker mode): wraps, drops what does not fit.
    static SampleRing<16> ring;
    int16_t a[20], b[20];
    for (int i = 0; i < 20; ++i) a[i] = int16_t(i);
    CHECK(ring.push(a, 10) == 10 && ring.available() == 10, "ring: push");
    CHECK(ring.pop(b, 6) == 6 && b[5] == 5, "ring: pop");
    CHECK(ring.push(a, 20) == 12 && ring.dropped() == 8, "ring: full, the rest dropped");
    CHECK(ring.pop(b, 20) == 16 && b[0] == 6 && b[4] == 0 && b[15] == 11, "ring: wraps in order");
    ring.push(a, 3);
    ring.clear();
    CHECK(ring.available() == 0, "ring: clear");
}

static void modeTests() {
    using namespace modesel;
    using A = Action;
    // UGREEN-like (both sides), mic plugged in: microphone after 10 s of capture.
    State st;
    Input in;
    CHECK(step(st, in, wlink::kModeAuto) == A::None && st.reason == wlink::kReasonDeciding, "mode: nothing plugged in");
    in.connected = in.canCapture = in.canPlayback = in.streaming = true;
    in.plugIns = 1;
    in.captureSinceMs = 1000;
    in.nowMs = 5000;
    in.heardNonZero = true;
    CHECK(step(st, in, wlink::kModeAuto) == A::None && st.reason == wlink::kReasonDeciding, "mode: still listening at 4 s");
    in.nowMs = 11000;
    CHECK(step(st, in, wlink::kModeAuto) == A::Decided && st.mode == wlink::kModeMicrophone &&
          st.reason == wlink::kReasonSignal, "mode: signal -> microphone");
    in.heardNonZero = false;  // the mic goes quiet later: never changes mid-run
    in.nowMs = 999999;
    CHECK(step(st, in, wlink::kModeAuto) == A::None && st.mode == wlink::kModeMicrophone, "mode: never changes mid-run");
    // Unplugged and plugged in again with nothing in the mic jack: pure zeros -> speaker.
    in.plugIns = 2;
    in.captureSinceMs = 1000000;
    in.nowMs = 1005000;
    CHECK(step(st, in, wlink::kModeAuto) == A::None && st.reason == wlink::kReasonDeciding, "mode: a re-plug decides again");
    in.nowMs = 1010000;
    CHECK(step(st, in, wlink::kModeAuto) == A::Decided && st.mode == wlink::kModeSpeaker &&
          st.reason == wlink::kReasonSilence, "mode: pure silence -> speaker");
    // The direction switch re-opens the adapter (no new plug-in): stays speaker.
    in.streaming = true;
    in.captureSinceMs = 0;
    CHECK(step(st, in, wlink::kModeAuto) == A::None && st.mode == wlink::kModeSpeaker, "mode: re-open is not a plug-in");
    // Plugged in again while a speaker: listen on the microphone side first.
    in.plugIns = 3;
    CHECK(step(st, in, wlink::kModeAuto) == A::ListenAgain && st.mode == wlink::kModeMicrophone, "mode: listen again after a re-plug");
    // Capture-only and playback-only adapters decide at once.
    State a, b;
    Input c;
    c.connected = c.canCapture = true;
    c.plugIns = 1;
    CHECK(step(a, c, wlink::kModeAuto) == A::Decided && a.mode == wlink::kModeMicrophone && a.reason == wlink::kReasonCaptureOnly,
          "mode: capture-only adapter -> microphone");
    c.canCapture = false;
    c.canPlayback = true;
    CHECK(step(b, c, wlink::kModeAuto) == A::Decided && b.mode == wlink::kModeSpeaker && b.reason == wlink::kReasonPlaybackOnly,
          "mode: playback-only adapter -> speaker");
    State u;
    c.canPlayback = false;
    CHECK(step(u, c, wlink::kModeAuto) == A::None && u.reason == wlink::kReasonDeciding, "mode: unusable adapter stays undecided");
    // A setting is final, whatever the adapter.
    State s1;
    CHECK(step(s1, in, wlink::kModeSetSpeaker) == A::Decided && s1.mode == wlink::kModeSpeaker && s1.reason == wlink::kReasonSetting,
          "mode: speaker setting");
    CHECK(step(s1, in, wlink::kModeSetSpeaker) == A::None, "mode: setting decided once");
}

int main() {
    modeTests();
    svgReportTests();
    usbWatchTests();
    buttonTests();
    configTests();
    highpassTests();
    mixerTests();
    wavTests();
    clipTests();
    printf(failures ? "%d FAILED\n" : "feature tests ok\n", failures);
    return failures ? 1 : 0;
}
