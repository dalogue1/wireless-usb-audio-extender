// Checks the local board's USB microphone descriptor with the remote's UAC1 parser
// (UacDescriptors.h): what the local board offers the PC must look like a plain
// 48 kHz mono 16-bit UAC1 microphone.
//   g++ -std=c++17 -Ishim -I<tinyusb>/src -I../AudioExtender_Local -I../AudioExtender_Remote usb_device_descriptor_test.cpp
#include "UsbMicDescriptor.h"
#include "UsbSpeakerDescriptor.h"
#include "UacDescriptors.h"
#include <cassert>
#include <cstdio>

int main() {
    // Build a whole configuration: config header, then a CDC-like dummy
    // interface pair (the local board has CDC first), then the mic at interfaces 2-3.
    uint8_t cfg[256] = {};
    const uint8_t ac = 2, ep = 0x83;
    size_t n = 9;
    n += buildMicDescriptor(cfg + n, ac, 4, ep);
    const uint8_t header[9] = {9, 2, uint8_t(n & 0xFF), uint8_t(n >> 8), 4, 1, 0, 0x80, 50};
    memcpy(cfg, header, 9);
    assert(n - 9 == kMicDescLen);

    // Every descriptor's length byte walks exactly to the end.
    size_t off = 9;
    int count = 0;
    while (off < n) { assert(cfg[off] >= 2); off += cfg[off]; count++; }
    assert(off == n);
    printf("descriptor: %u bytes, %d sub-descriptors\n", unsigned(kMicDescLen), count);

    const uac::Capture c = uac::parse(cfg, n);
    printf("parsed: found=%d uac2=%d rate48k=%d itf=%u alt=%u ep=0x%02x maxPacket=%u ch=%u bits=%u subframe=%u fu=%u\n",
           c.found, c.uac2, c.rate48k, c.streamInterface, c.alt, c.endpoint, c.maxPacket, c.channels, c.bits,
           c.subframe, c.featureUnit);
    assert(c.found && !c.uac2 && c.rate48k);
    assert(c.streamInterface == ac + 1 && c.alt == 1);
    assert(c.endpoint == ep && c.maxPacket == 98);
    assert(c.channels == 1 && c.bits == 16 && c.subframe == 2);
    assert(c.controlInterface == ac);
    // Endpoint attributes: isochronous, asynchronous (0x05)
    bool sawEp = false;
    for (off = 9; off < n; off += cfg[off])
        if (cfg[off + 1] == 0x05) { assert(cfg[off + 3] == 0x05); assert(cfg[off + 6] == 1); sawEp = true; }
    assert(sawEp);
    // IAD covers the two audio interfaces
    assert(cfg[9 + 1] == 0x0B && cfg[9 + 2] == ac && cfg[9 + 3] == 2 && cfg[9 + 4] == 1);
    printf("local USB mic descriptor OK\n");

    // Speaker mode: the same checks for the speaker function, with the playback parser.
    {
        uint8_t sp[256] = {};
        const uint8_t ep2 = 0x03;
        size_t m = 9;
        m += buildSpeakerDescriptor(sp + m, ac, 4, ep2);
        const uint8_t hdr[9] = {9, 2, uint8_t(m & 0xFF), uint8_t(m >> 8), 4, 1, 0, 0x80, 50};
        memcpy(sp, hdr, 9);
        assert(m - 9 == kSpkDescLen);
        size_t o = 9;
        while (o < m) { assert(sp[o] >= 2); o += sp[o]; }
        assert(o == m);
        assert(!uac::parse(sp, m, uac::Dir::Capture).found);  // not a microphone
        const uac::Playback p = uac::parse(sp, m, uac::Dir::Playback);
        assert(p.found && p.rate48k && p.streamInterface == ac + 1 && p.alt == 1);
        assert(p.endpoint == ep2 && p.maxPacket == 196 && p.channels == 2 && p.bits == 16 && p.subframe == 2);  // 1.4.2: stereo
        assert(p.syncType == 2);  // adaptive
        assert(p.terminalLink == kSpkTermInput);
        printf("local USB speaker descriptor OK (%u bytes)\n", unsigned(kSpkDescLen));
    }
    return 0;
}
