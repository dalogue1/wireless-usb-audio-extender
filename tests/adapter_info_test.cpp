// Host test for AdapterInfo.h: the adapter record and adapter.txt, using the UGREEN
// 30757 descriptors. With -v the rendered text is printed.
#include "AdapterInfo.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ugreen_descriptor.h"

static const uint8_t kUgreenDevice[18] = {0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40, 0xd1,
                                          0x12, 0x10, 0x00, 0x00, 0x01, 0x01, 0x02, 0x00, 0x01};

static std::vector<uint8_t> record(const uint8_t* cfg, size_t cfgLen, uint8_t state, const char* problem) {
    adinfo::RecordHeader h = {};
    h.format = adinfo::kRecordFormat;
    h.state = state;
    h.speed = 1;
    h.usedInterface = state == 1 ? 1 : 0xFF;
    h.usedAlt = state == 1 ? 1 : 0xFF;
    h.usedEndpoint = state == 1 ? 0x81 : 0xFF;
    h.hasVolume = state == 1;
    h.volMin = -12 * 256;
    h.volMax = 30 * 256;
    h.volRes = 128;
    h.volCur = 6 * 256;
    h.channels = 1;
    h.bits = 16;
    h.configTotal = uint16_t(cfgLen);
    h.configLen = uint16_t(cfgLen);
    h.remoteUptimeS = 3725;
    strcpy(h.firmware, "1.3.0-dev");
    strcpy(h.manufacturer, "KTMicro");
    strcpy(h.product, "KT USB Audio");
    strcpy(h.problem, problem);
    memcpy(h.device, kUgreenDevice, sizeof(h.device));
    std::vector<uint8_t> r(sizeof(h) + cfgLen);
    memcpy(r.data(), &h, sizeof(h));
    if (cfgLen) memcpy(r.data() + sizeof(h), cfg, cfgLen);
    return r;
}

static std::string render(const std::vector<uint8_t>& r, size_t len) {
    std::string text;
    adinfo::Out out{[](void* ctx, const char* t) { static_cast<std::string*>(ctx)->append(t); }, &text};
    adinfo::render(r.data(), len, "test", out);
    return text;
}

static bool has(const std::string& s, const char* what) {
    if (s.find(what) != std::string::npos) return true;
    printf("  missing: %s\n", what);
    return false;
}

int main(int argc, char** argv) {
    const std::vector<uint8_t> r = record(kUgreen, sizeof(kUgreen), 1, "");
    assert(adinfo::recordSize(r.data(), r.size()) == r.size());
    assert(r.size() <= 3000);  // wlink::kInfoMax
    const std::string t = render(r, r.size());
    if (argc > 1 && strcmp(argv[1], "-v") == 0) fputs(t.c_str(), stdout);
    assert(has(t, "Result: streaming, 1 channel, 16-bit, 48000 Hz"));
    assert(has(t, "used by the extender: interface 1, alternate setting 1, endpoint 0x81"));
    assert(has(t, "-12.0 to 30.0 dB in 0.50 dB steps, left at 6.0 dB"));
    assert(has(t, "1:02:05 after the remote started"));
    assert(has(t, "ID (vendor:product)  12d1:0010"));
    assert(has(t, "USB 1.10, full speed (12 Mbit/s), device release 1.00"));
    assert(has(t, "4 interfaces, bus powered, up to 100 mA"));
    assert(has(t, "input terminal 1: microphone, 1 channel"));
    assert(has(t, "feature unit 2: from 1"));
    assert(has(t, "all channels: mute"));
    assert(has(t, "channel 1: volume"));
    assert(has(t, "output terminal 8: headphones, from 7"));
    assert(has(t, "1.1 audio streaming   <- used by the extender"));
    assert(has(t, "1.0 audio streaming: idle"));
    assert(has(t, "1 channel, 16-bit in 2-byte samples, 44100, 48000 Hz"));
    assert(has(t, "endpoint 0x81 IN isochronous, asynchronous, up to 96 bytes every 1 ms (recording)"));
    assert(has(t, "sampling rate can be set"));
    assert(has(t, "endpoint 0x01 OUT isochronous, adaptive"));
    assert(has(t, "2 channels, 24-bit in 3-byte samples"));
    assert(has(t, "3.0 HID (buttons / keys)"));
    assert(has(t, "endpoint 0x82 IN interrupt, up to 16 bytes"));
    assert(has(t, "configuration (303 bytes):"));
    assert(has(t, "    09 02 2f 01 04 01 00 80 32 09 04 00 00 00 01 01\r\n"));
    assert(t.find("\n") == t.find("\r\n") + 1);  // CRLF line endings

    // Refused adapter: reason shown, nothing used.
    const std::vector<uint8_t> bad = record(kUgreen, 40, 2, "USB Audio Class 2 only (needs UAC1)");
    const std::string tb = render(bad, bad.size());
    assert(has(tb, "Result: NOT USABLE - USB Audio Class 2 only (needs UAC1)"));
    assert(has(tb, "used by the extender: nothing"));

    // No record (nothing received yet) and a damaged record: a note, never a crash.
    assert(has(render(r, 0), "No adapter details yet"));
    assert(has(render(r, 100), "No adapter details yet"));                 // shorter than its header
    std::vector<uint8_t> wrong = r;
    wrong[0] = 99;                                                         // unknown format
    assert(has(render(wrong, wrong.size()), "No adapter details yet"));
    // Every truncation of the configuration descriptor, and random bytes, render safely.
    for (size_t n = 0; n <= sizeof(kUgreen); ++n) {
        const std::vector<uint8_t> part = record(kUgreen, n, 1, "");
        render(part, part.size());
    }
    unsigned seed = 1;
    for (int k = 0; k < 2000; ++k) {
        uint8_t junk[300];
        for (auto& b : junk) b = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
        const std::vector<uint8_t> j = record(junk, sizeof(junk), uint8_t(1 + k % 2), "x");
        render(j, j.size());
    }
    // A wrong declared length (configLen longer than the data) is not a record.
    std::vector<uint8_t> shortRec = r;
    shortRec.resize(r.size() - 1);
    assert(adinfo::recordSize(shortRec.data(), shortRec.size()) == 0);
    printf("adapter info test ok (%zu-byte record, %zu bytes of text)\n", r.size(), t.size());
    return 0;
}
