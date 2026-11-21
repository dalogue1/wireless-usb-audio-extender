// Checks LinkProtocol.h packet sizes and the receive filter.
#include "LinkProtocol.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

int main() {
    using namespace wlink;
    printf("audio %zu B (%zu on air), status %zu B, log %zu B\n", sizeof(AudioPacket),
           audioPacketBytes(kPacketSamples), sizeof(StatusPacket), sizeof(LogPacket));
    assert(audioPacketBytes(kPacketSamples) == sizeof(AudioPacket));
    assert(audioPacketBytes(kPacketSamples) > 250);  // needs ESP-NOW v2, by design
    assert(sizeof(AudioPacket) <= kMaxPacket);
    assert(kBlockSamples * 1000 / kSampleRate == 4);
    assert(kPacketSamples * 1000 / kSampleRate == 12);

    AudioPacket a = {};
    fill(a.h, kAudio, 1);
    a.samples = kPacketSamples;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&a);
    assert(check(p, sizeof(a), 1) == kAudio);
    assert(check(p, sizeof(a), 0) == 0);                    // other pair ignored
    assert(check(p, sizeof(a) - 2, 1) == 0);                // truncated
    a.samples = kPacketSamples + 1;
    assert(check(p, sizeof(a), 1) == 0);                    // bad sample count
    a.samples = 10;
    assert(check(p, audioPacketBytes(10), 1) == kAudio);    // short block ok

    BeaconPacket bp = {};
    fill(bp.h, kBeacon, 2);
    assert(check(reinterpret_cast<uint8_t*>(&bp), sizeof(bp), 2) == kBeacon);
    assert(check(reinterpret_cast<uint8_t*>(&bp), sizeof(Header), 2) == 0);  // a 1.3.5 beacon (no lock field)
    assert(macIsZero(bp.lockedTo));
    bp.lockedTo[5] = 1;
    assert(!macIsZero(bp.lockedTo));
    bp.h.magic ^= 1;
    assert(check(reinterpret_cast<uint8_t*>(&bp), sizeof(bp), 2) == 0);
    Header h;
    fill(h, kPing, 0);
    assert(check(reinterpret_cast<uint8_t*>(&h), sizeof(h), 0) == 0);  // ping too short
    PingPacket ping;
    fill(ping.h, kPing, 0);
    assert(check(reinterpret_cast<uint8_t*>(&ping), sizeof(ping), 0) == kPing);
    assert(kDefaultChannel == 11 && adaptSubwindowSec(kAdaptSlow) == 60 && adaptSubwindowSec(kAdaptFast) == 4);
    // Adapter details: a 2300-byte record in three pieces; the receive check refuses bad pieces.
    static_assert(sizeof(AdapterInfoPacket) <= kMaxPacket, "fits ESP-NOW v2");
    static_assert((kInfoMax + kInfoPiece - 1) / kInfoPiece <= 8, "pieces fit the local board's 8-bit mask");
    AdapterInfoPacket ip = {};
    fill(ip.h, kAdapterInfo, 4);
    ip.id = 0x12340001;
    ip.total = 2300;
    ip.offset = 2000;
    ip.len = 300;
    const uint8_t* ib = reinterpret_cast<const uint8_t*>(&ip);
    const int onAir = int(offsetof(AdapterInfoPacket, data) + ip.len);
    assert(check(ib, onAir, 4) == kAdapterInfo);
    assert(check(ib, onAir - 1, 4) == 0);                     // truncated
    ip.len = 301;
    assert(check(ib, onAir + 1, 4) == 0);                     // past the end of the record
    ip.len = 300; ip.total = kInfoMax + 1;
    assert(check(ib, onAir, 4) == 0);                         // record too big
    InfoRequestPacket rq;
    fill(rq.h, kInfoRequest, 4);
    rq.id = 7;
    assert(check(reinterpret_cast<uint8_t*>(&rq), sizeof(rq), 4) == kInfoRequest);
    assert(check(reinterpret_cast<uint8_t*>(&rq), sizeof(rq) - 1, 4) == 0);
    // 1.3.6: the noise graph request and data; the version moved on (both boards must match).
    static_assert(kVersion == 22, "protocol v22");  // 1.4.9: micSource / micSlot in status
    // 1.4.6: the remote's radio backlog of audio stays shorter than the local's 150 ms buffer,
    // and a weak signal (reports still coming) no longer counts as a stuck transmitter at once.
    static_assert(kMaxAudioQueued * 12 < 150, "audio backlog under the 150 ms buffer");
    static_assert(kTxStuckSilentMs >= 2000 && kTxStuckNoAckMs >= kTxStuckSilentMs, "stuck checks not hair-trigger");  // 1.4.5: ADPCM at far, mu-law at normal
    Header nr;
    fill(nr, kNoiseRequest, 3);
    assert(check(reinterpret_cast<uint8_t*>(&nr), sizeof(nr), 3) == kNoiseRequest);
    NoisePacket np = {};
    fill(np.h, kNoiseData, 3);
    assert(check(reinterpret_cast<uint8_t*>(&np), sizeof(np), 3) == kNoiseData);
    assert(check(reinterpret_cast<uint8_t*>(&np), sizeof(np) - 1, 3) == 0);
    Header old = nr;
    old.version = 14;
    assert(check(reinterpret_cast<uint8_t*>(&old), sizeof(old), 3) == 0);  // 1.4.1 packets are ignored
    // 1.4.2: stereo speaker audio, IMA ADPCM.
    static StereoAudioPacket sa = {};
    fill(sa.h, kStereoAudio, 2);
    sa.frames = kPacketSamples;
    const uint8_t* sb = reinterpret_cast<const uint8_t*>(&sa);
    assert(sizeof(sa) == 8 + 4 + 2 + 4 + 2 + 576 && sizeof(sa) * 2 < audioPacketBytes(kPacketSamples) + 64);
    assert(check(sb, sizeof(sa), 2) == kStereoAudio);
    assert(check(sb, sizeof(sa) - 1, 2) == 0);                     // short
    sa.frames = kPacketSamples - 1;
    assert(check(sb, sizeof(sa), 2) == 0);                         // wrong frame count
    sa.frames = kPacketSamples; sa.index[1] = 89;
    assert(check(sb, sizeof(sa), 2) == 0);                         // step index out of range
    printf("stereo ADPCM packet %zu B (mono PCM %zu B)\n", sizeof(sa), audioPacketBytes(kPacketSamples));
    // 1.4.5: mono ADPCM microphone packet (distance = far).
    static MonoAdpcmPacket ma = {};
    fill(ma.h, kMonoAdpcm, 2);
    ma.frames = kPacketSamples;
    const uint8_t* mb = reinterpret_cast<const uint8_t*>(&ma);
    assert(sizeof(ma) == 8 + 4 + 2 + 2 + 1 + 288 && sizeof(ma) * 3 < audioPacketBytes(kPacketSamples));
    assert(check(mb, sizeof(ma), 2) == kMonoAdpcm);
    assert(check(mb, sizeof(ma) - 1, 2) == 0);
    ma.index = 89;
    assert(check(mb, sizeof(ma), 2) == 0);
    assert(strstr(distanceRateText(kDistanceFar), "ADPCM") && strstr(distanceRateText(kDistanceNormal), "mu-law"));
    assert(strcmp(distanceRateText(kDistanceNear), "12 Mbit/s, uncompressed audio") == 0);
    // 1.4.7: audio_compression. auto follows distance; the others force the format at any distance.
    assert(compressionInEffect(kDistanceNear, kCompAuto) == kCompNone);
    assert(compressionInEffect(kDistanceNormal, kCompAuto) == kCompLow);
    assert(compressionInEffect(kDistanceFar, kCompAuto) == kCompHigh);
    assert(compressionInEffect(kDistanceFar, kCompNone) == kCompNone);
    assert(compressionInEffect(kDistanceNear, kCompHigh) == kCompHigh);
    assert(compressionValid(9) == kCompAuto);
    assert(strcmp(distanceRateText(kDistanceFar, kCompLow), "2 Mbit/s, mu-law audio, audio_compression low") == 0);
    assert(strcmp(distanceRateText(kDistanceNormal, kCompAuto), "6 Mbit/s, mu-law audio") == 0);
    static MonoMulawPacket mu = {};
    fill(mu.h, kMonoMulaw, 2);
    mu.frames = kPacketSamples;
    const uint8_t* ub = reinterpret_cast<const uint8_t*>(&mu);
    assert(sizeof(mu) == 8 + 4 + 2 + 576 && sizeof(mu) * 100 < audioPacketBytes(kPacketSamples) * 55);  // about half
    assert(check(ub, sizeof(mu), 2) == kMonoMulaw);
    assert(check(ub, sizeof(mu) - 1, 2) == 0);
    // G.711 reference points: 0 -> 0xFF, -1 -> 0x7F, full scale -> 0x80 / 0x00
    assert(mulawEncode(0) == 0xFF && mulawEncode(-1) == 0x7F && mulawEncode(32767) == 0x80 && mulawEncode(-32768) == 0x00);
    assert(mulawDecode(0xFF) == 0 && mulawDecode(0x80) == 32124 && mulawDecode(0x00) == -32124);
    for (int v = -32768; v <= 32767; v += 7) {  // round trip within one step of the segment
        const int d = mulawDecode(mulawEncode(int16_t(v))), err = d > v ? d - v : v - d;
        const int mag = v < 0 ? -v : v;
        assert(err <= (mag < 32 ? 4 : mag / 16 + 4) || mag > 32635);
    }
    printf("mu-law packet %zu B\n", sizeof(mu));
    printf("mono ADPCM packet %zu B\n", sizeof(ma));
    static_assert(sizeof(StatusPacket) <= kMaxPacket, "status fits");
    assert(strcmp(boardName(kBoardAtomS3U), "AtomS3U") == 0 && strcmp(boardName(kBoardXiao), "XIAO ESP32-S3") == 0);
    // 1.4.9: microphone sources. Only the USB adapter is not PDM; the names appear in status.txt.
    assert(!pdmSource(kSrcUsbAdapter) && pdmSource(kSrcAtomMic) && pdmSource(kSrcExternalPdm) && pdmSource(kSrcSensePdm) &&
           pdmSource(kSrcPinPdm) && !pdmSource(kSrcPinPdm + 1));
    assert(strcmp(micSourceName(kSrcExternalPdm), "External PDM Microphone") == 0);
    assert(strcmp(micSourceName(99), "USB audio adapter") == 0);
    {
        StatusPacket st = {};
        assert(st.micSource == kSrcUsbAdapter);  // a zeroed status means a USB adapter, as before 1.4.9
        assert(offsetof(StatusPacket, micSlot) == sizeof(StatusPacket) - 1);
    }
    printf("noise %zu B, beacon %zu B, config %zu B, status %zu B\n", sizeof(NoisePacket), sizeof(BeaconPacket),
           sizeof(ConfigPacket), sizeof(StatusPacket));
    assert(sizeof(NoisePacket) <= kMaxPacket && kNoiseBins == 257);
    assert(kDistanceNormal == 0 && kModeAuto == 0);  // zero = the default, also in config.txt's order
    printf("link protocol test ok\n");
    return 0;
}
