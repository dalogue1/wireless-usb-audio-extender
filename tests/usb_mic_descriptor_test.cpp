// Host test for UacDescriptors.h, using the UGREEN 30757 (KTMicro "KT USB
// Audio", 12d1:0010) configuration descriptor, taken from an `lsusb -v` dump.
#include "UacDescriptors.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ugreen_descriptor.h"

int main() {
    static_assert(sizeof(kUgreen) == 0x12f, "wTotalLength from lsusb");
    const uac::Capture c = uac::parse(kUgreen, sizeof(kUgreen));
    assert(c.found && !c.uac2 && c.rate48k);
    assert(c.controlInterface == 0 && c.streamInterface == 1 && c.alt == 1);
    assert(c.endpoint == 0x81 && c.maxPacket == 96 && c.interval == 1);
    assert(c.channels == 1 && c.subframe == 2 && c.bits == 16 && c.rateControl);
    assert(c.terminalLink == 4 && c.featureUnit == 2);           // OT4 <- SU3 <- FU2
    assert(c.volumeChannel == 1 && c.muteChannel == 0 && c.agcChannel == 0xFF);

    // Truncated descriptors never read past the end and find nothing usable.
    for (size_t n = 0; n < 150; ++n) assert(!uac::parse(kUgreen, n).found);

    // UAC2-only device: interface protocol 0x20 is flagged, not used.
    std::vector<uint8_t> v2(kUgreen, kUgreen + sizeof(kUgreen));
    for (size_t at = 0; at + 9 <= v2.size(); at += v2[at])
        if (v2[at + 1] == 0x04 && v2[at + 5] == 1) v2[at + 7] = 0x20;
    const uac::Capture c2 = uac::parse(v2.data(), v2.size());
    assert(c2.uac2);

    // A 44.1 kHz-only capture alt is still found but marked as lacking 48 kHz.
    std::vector<uint8_t> r44(kUgreen, kUgreen + sizeof(kUgreen));
    for (size_t at = 0; at + 14 <= r44.size(); at += r44[at])
        if (r44[at] == 0x0e && r44[at + 1] == 0x24 && r44[at + 2] == 2) { r44[at + 7] = 1; break; }
    const uac::Capture c3 = uac::parse(r44.data(), r44.size());
    assert(c3.found && !c3.rate48k);

    // Volume mapping, 1/256 dB, snapped to the device resolution.
    assert(uac::volumeForPercent(0, 7680, 256, 0) == 0);
    assert(uac::volumeForPercent(0, 7680, 256, 100) == 7680);
    assert(uac::volumeForPercent(0, 7680, 256, 50) == 3840);
    assert(uac::volumeForPercent(-4096, 4096, 0, 50) == 0);
    assert(uac::percentForVolume(0, 7680, 3840) == 50);
    assert(uac::percentForVolume(0, 7680, -5) == 0 && uac::percentForVolume(0, 0, 0) == 100);

    // Packet unpack: first channel, left-aligned to 32 bits.
    const uint8_t mono16[] = {0x01, 0x80, 0xff, 0x7f};
    int32_t out[8];
    assert(uac::unpack(mono16, sizeof(mono16), 1, 2, out, 8) == 2);
    assert(out[0] == int32_t(0x80010000u) && out[1] == 0x7fff0000);
    const uint8_t st24[] = {0x00, 0x00, 0x80, 0x11, 0x22, 0x33, 0xff, 0xff, 0x7f, 0, 0, 0};
    assert(uac::unpack(st24, sizeof(st24), 2, 3, out, 8) == 2);
    assert(out[0] == int32_t(0x80000000u) && out[1] == 0x7fffff00);
    assert(uac::unpack(mono16, sizeof(mono16), 1, 2, out, 1) == 1);
    assert(c.volumeMask == 0x02 && c.syncType == 1);              // capture EP 0x81 attr 0x05: asynchronous

    // Playback (speaker mode): interface 2 alt 1 (2 ch, 16-bit, 48 kHz), adaptive OUT EP 0x01.
    const uac::Playback pb = uac::parse(kUgreen, sizeof(kUgreen), uac::Dir::Playback);
    assert(pb.found && pb.rate48k && pb.streamInterface == 2 && pb.alt == 1);
    assert(pb.endpoint == 0x01 && pb.maxPacket == 384 && pb.syncType == 2);
    assert(pb.channels == 2 && pb.subframe == 2 && pb.bits == 16 && pb.rateControl);
    assert(pb.terminalLink == 5 && pb.featureUnit == 7);           // IT5 -> MU6 -> FU7 -> OT8
    assert(pb.volumeChannel == 1 && pb.volumeMask == 0x06 && pb.muteChannel == 0);
    // A capture-only device has no playback, and the other way round.
    std::vector<uint8_t> capOnly(kUgreen, kUgreen + sizeof(kUgreen));
    for (size_t at = 0; at + 9 <= capOnly.size(); at += capOnly[at])
        if (capOnly[at + 1] == 0x05 && capOnly[at + 2] == 0x01) capOnly[at + 2] = 0x83;  // playback EPs turned IN
    assert(!uac::parse(capOnly.data(), capOnly.size(), uac::Dir::Playback).found);
    {   // Truncated: never reads past the end; found only once alt 1 is complete.
        size_t complete = 0;
        for (size_t n = 0; n <= sizeof(kUgreen); ++n)
            if (uac::parse(kUgreen, n, uac::Dir::Playback).found) { complete = n; break; }
        assert(complete > 200 && complete < sizeof(kUgreen));
    }
    // Packing a mono block for a stereo 16-bit and a mono 24-bit adapter.
    const int16_t mono[2] = {0x1234, -2};
    uint8_t pk[16];
    assert(uac::pack(mono, 2, 2, 2, pk, sizeof(pk)) == 8);
    assert(pk[0] == 0x34 && pk[1] == 0x12 && pk[2] == 0x34 && pk[3] == 0x12 && pk[4] == 0xFE && pk[7] == 0xFF);
    assert(uac::pack(mono, 2, 1, 3, pk, sizeof(pk)) == 6 && pk[0] == 0 && pk[1] == 0x34 && pk[2] == 0x12);
    assert(uac::pack(mono, 2, 2, 2, pk, 5) == 4);                  // never past the buffer
    // 1.4: stereo frames (left, right) for stereo, mono and 24-bit adapters.
    const int16_t lr[4] = {0x1234, -2, 100, 300};
    assert(uac::packStereo(lr, 2, 2, 2, pk, sizeof(pk)) == 8);
    assert(pk[0] == 0x34 && pk[1] == 0x12 && pk[2] == 0xFE && pk[3] == 0xFF && pk[4] == 100 && pk[6] == 0x2C && pk[7] == 1);
    assert(uac::packStereo(lr, 2, 1, 2, pk, sizeof(pk)) == 4);     // mono: (L + R) / 2
    assert(int16_t(pk[0] | pk[1] << 8) == (0x1234 - 2) / 2 && int16_t(pk[2] | pk[3] << 8) == 200);
    assert(uac::packStereo(lr, 2, 2, 3, pk, sizeof(pk)) == 12 && pk[0] == 0 && pk[1] == 0x34 && pk[4] == 0xFE);
    assert(uac::packStereo(lr, 2, 2, 2, pk, 7) == 4);              // never past the buffer
    puts("usb mic descriptor test ok");
    return 0;
}
