#pragma once
// Wireless USB Audio Extender - ESP-NOW link protocol shared by the remote and the local board.
//
// SHARED FILE: an identical copy lives in AudioExtender_Remote/ and
// AudioExtender_Local/ (Arduino cannot include files from outside a sketch
// folder). Edit shared/LinkProtocol.h and run tools/sync_shared.sh.
//
// Radio: ESP-NOW v2 (ESP-IDF 5.4+, Arduino core 3.2+), no access point, no IP.
// Pairing: the remote broadcasts BEACONs on the channel of its pair ID; the
// local with the same pair ID answers with HELLO and both switch to unicast
// (hardware ACK + retries). The local board sends PINGs (and the remote too in speaker
// mode, where it sends no audio); either side drops the link after kLinkTimeoutMs of
// silence and goes back to pairing. With `remote = <MAC>` in config.txt both ends are
// locked to each other (see the README, "Pairing by MAC address").
#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace wlink {

constexpr uint32_t kMagic = 0x314D5557;  // "WUM1" little-endian
constexpr uint8_t kVersion = 19; // 1.4.5: microphone audio ADPCM at far, mu-law at normal (v18: ADPCM at far only); v17 = 1.4.4: temperature only at start and drive opening (no 10-min reads); v16 = 1.4.3: temperature snapshot when the drive opens; v15 = 1.4.2: stereo speaker mode (IMA ADPCM); v14 = 1.4.0: AtomS3U (board kind in status); v13 = 1.3.6: MAC pairing, distance, speaker mode, noise graph; v12 = 1.3.3: mic_level; v11 = 1.3.0: no jumpers, adapter details; v10 = 1.2.13: 3 blocks per audio packet, memory in status; v9 = 1.2.9: temperature on/off; v8 = 1.2.7: high-pass filter; v7 = 1.2.5:
                                  // temperature + cpu_mhz; v6 = 1.2.4: tx_power; v5 = 1.2.3: mic 1..9;
                                  // v4 = 1.2.2: silent-mic status; v3 = 1.2.0: config.txt

constexpr uint32_t kSampleRate = 48000;  // fixed: 48 kHz; microphone: mono 16-bit PCM; speaker: stereo IMA ADPCM
constexpr uint16_t kBlockSamples = 192;  // 4 ms: the remote's processing block
constexpr uint16_t kPacketBlocks = 3;    // blocks per audio packet
constexpr uint16_t kPacketSamples = kBlockSamples * kPacketBlocks;  // 12 ms per packet, 83 packets/s

constexpr uint8_t kPairIds = 9;  // mic numbers 1..9

constexpr uint32_t kBeaconMs = 200;       // remote, while not linked
constexpr uint32_t kPingMs = 250;         // local, while linked
constexpr uint32_t kStatusMs = 1000;      // remote, while linked
constexpr uint32_t kLinkTimeoutMs = 2000; // no packet from the peer -> unlinked
// Self-recovery when a link that had worked stays down (see README "Recovery"):
constexpr uint32_t kRecoverAfterMs = 15000;      // local restarts its radio
constexpr uint32_t kRemoteStuckBeacons = 10;     // remote: beacons in a row its radio refuses (2 s) -> restart
constexpr uint32_t kRemoteRestartMs = 20000;     // remote restarts itself if the link stays down
// Remote: a stuck transmitter shows as sends that stop succeeding while the local board is
// still heard (interference or range cut both directions). Restart after kTxStuckMs of that,
// once the link has been up for kTxStuckArmMs (bounds any restart loop).
constexpr uint32_t kTxStuckMs = 500, kTxStuckFails = 5, kTxStuckArmMs = 10000;
constexpr uint32_t kRecoverRepeatMs = 300000;    // local: again every 5 min while still unlinked

constexpr size_t kMaxPacket = 1470;       // ESP_NOW_MAX_DATA_LEN_V2

enum Type : uint8_t {
    kBeacon = 1,  // remote -> broadcast: "pair ID n is here"
    kHello = 2,   // local -> remote: "I am your local"
    kPing = 3,    // local -> remote: keep-alive
    kAudio = 4,   // remote -> local: one block of PCM
    kStatus = 5,  // remote -> local: adapter and link counters
    kLog = 6,     // remote -> local: one log line for the local board's serial port
    kConfig = 7,  // local -> remote: settings from config.txt (resent until acknowledged)
    kConfigAck = 8,  // remote -> local: what the remote now uses
    kInfoRequest = 9,   // local -> remote: send the adapter details with this id
    kAdapterInfo = 10,  // remote -> local: one piece of the adapter details
    kNoiseRequest = 11, // local -> remote: send the noise filter's learned signature (drive opening)
    kNoiseData = 12,    // remote -> local: the signature, in 0.1 dB per bin
    kStereoAudio = 13,  // local -> remote (speaker mode): 12 ms of the PC's stereo audio, IMA ADPCM
    kTempRequest = 14,  // local -> remote: read the chip temperature once now (drive opening);
                        // the remote's next status carries it with tempFresh = 1
    kMonoAdpcm = 15,    // remote -> local (microphone, distance = far): 12 ms as IMA ADPCM
    kMonoMulaw = 16,    // remote -> local (microphone, distance = normal): 12 ms as 8-bit mu-law
};
constexpr uint32_t kTempSnapshotKeepMs = 30000;  // remote: a snapshot is reported this long

// Which way the audio goes. The remote decides (config.txt `mode`, or auto-detection at
// start-up) and reports it; the local board presents the matching USB device to the PC.
enum AudioMode : uint8_t { kModeMicrophone = 0, kModeSpeaker = 1 };
// config.txt `mode` (the setting; auto = the remote decides at start-up).
enum ModeSetting : uint8_t { kModeAuto = 0, kModeSetMicrophone = 1, kModeSetSpeaker = 2 };
// Why the remote uses the mode it reports.
enum ModeReason : uint8_t {
    kReasonDeciding = 0,      // auto: no adapter yet, or still listening (mode not final)
    kReasonSetting = 1,       // config.txt says microphone / speaker
    kReasonCaptureOnly = 2,   // auto: the adapter only records
    kReasonPlaybackOnly = 3,  // auto: the adapter only plays
    kReasonSignal = 4,        // auto: microphone signal heard at start-up
    kReasonSilence = 5,       // auto: pure digital silence at start-up (nothing in the mic jack)
    kReasonBuiltInMic = 6,    // the remote is an AtomS3U: built-in microphone only
};

// Which board the remote runs on (StatusPacket.board), and the local board for its own log.
enum BoardKind : uint8_t { kBoardXiao = 0, kBoardAtomS3U = 1 };
inline const char* boardName(uint8_t b) { return b == kBoardAtomS3U ? "AtomS3U" : "XIAO ESP32-S3"; }

// config.txt `distance`: the radio rate of the paired link (the further, the slower).
enum Distance : uint8_t { kDistanceNormal = 0, kDistanceNear = 1, kDistanceFar = 2 };
inline const char* distanceName(uint8_t d) { return d == kDistanceNear ? "near" : d == kDistanceFar ? "far" : "normal"; }
inline const char* distanceRateText(uint8_t d) {
    return d == kDistanceNear ? "12 Mbit/s" : d == kDistanceFar ? "2 Mbit/s, ADPCM audio" : "6 Mbit/s, mu-law audio";
}

enum Flags : uint8_t {
    kFlagStreamStart = 0x01,  // first audio block of a new stream: local resyncs sequence
};

enum AdapterState : uint8_t {
    kAdapterNone = 0,       // nothing plugged in (local sends silence)
    kAdapterStreaming = 1,  // UAC1 mic streaming at 48 kHz
    kAdapterUnusable = 2,   // device plugged in but not usable (see problem text)
};

constexpr uint8_t kMaxTxPowerDbm = 20, kMinTxPowerDbm = 2;  // ESP32-S3 range; default = maximum
constexpr uint8_t kCpuMhzDefault = 240;  // processor speed: 240 (default) or 160 MHz
inline bool cpuMhzValid(uint8_t mhz) { return mhz == 240 || mhz == 160; }
constexpr int16_t kTempUnknown = -32768;  // chip temperature not available
constexpr int16_t kTempHotDeciC = 750, kTempCoolDeciC = 700;  // warn at 75 C, re-arm below 70 C
constexpr uint8_t kDefaultChannel = 11;   // factory default for every mic number
constexpr uint8_t kMinChannel = 1, kMaxChannel = 11;  // 2.4 GHz channels allowed everywhere

enum AdaptSpeed : uint8_t { kAdaptNormal = 0, kAdaptSlow = 1, kAdaptFast = 2 };  // 15 s / 60 s / 4 s subwindows
inline uint8_t adaptSubwindowSec(uint8_t a) { return a == kAdaptSlow ? 60 : a == kAdaptFast ? 4 : 15; }

enum FilterMode : uint8_t {
    kFilterOn = 0,      // 12 dB maximum reduction (factory default)
    kFilterStrong = 1,  // 20 dB
    kFilterOff = 2,     // unfiltered (the filter keeps learning, so switching is instant)
};

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t pairId;
    uint8_t flags;
};

// Beacon (remote -> broadcast while unlinked). lockedTo: the only local board this remote
// links to (all zeros: any), so other local boards on the same mic number skip it.
struct __attribute__((packed)) BeaconPacket {
    Header h;
    uint8_t lockedTo[6];
};

// Audio, both directions: remote -> local (microphone) or local -> remote (speaker).
struct __attribute__((packed)) AudioPacket {
    Header h;
    uint32_t seq;      // packet counter, +1 per packet
    uint16_t samples;  // <= kPacketSamples (always kPacketSamples from the remote)
    int16_t pcm[kPacketSamples];
};

// Speaker mode (local -> remote): 12 ms of the PC's left and right channels, each as IMA
// ADPCM (4 bits per sample, low nibble first; Adpcm.h). predictor/index are the encoder's
// state at the start of this packet, so every packet decodes on its own (a lost packet
// costs 12 ms of silence, nothing more). 590 bytes: half of a mono 16-bit packet.
constexpr uint16_t kAdpcmChannelBytes = kPacketSamples / 2;
struct __attribute__((packed)) StereoAudioPacket {
    Header h;
    uint32_t seq;          // packet counter, +1 per packet
    uint16_t frames;       // always kPacketSamples
    int16_t predictor[2];  // left, right: decoder state at the first sample
    uint8_t index[2];      // step index 0..88
    uint8_t data[2 * kAdpcmChannelBytes];  // left channel, then right
};

// Microphone at distance = far (1.4.5): 12 ms of mono audio as IMA ADPCM (4 bits per sample),
// a quarter of the PCM packet, so the slow 2 Mbit/s rate takes about as much air time as
// `normal`. predictor/index: the encoder's state at the first sample (each packet decodes
// on its own; a lost packet costs 12 ms, nothing more).
struct __attribute__((packed)) MonoAdpcmPacket {
    Header h;
    uint32_t seq;          // packet counter, +1 per packet (same sequence as kAudio)
    uint16_t frames;       // always kPacketSamples
    int16_t predictor;
    uint8_t index;         // step index 0..88
    uint8_t data[kAdpcmChannelBytes];
};

// Microphone at distance = normal (1.4.5): 12 ms of mono audio as 8-bit mu-law (G.711), half
// the PCM packet. Its noise stays about 38 dB below the sound at any level (31 dB below the
// background in a real outdoor recording): no practical loss, about 10 % air time instead of 14 %.
struct __attribute__((packed)) MonoMulawPacket {
    Header h;
    uint32_t seq;          // packet counter, +1 per packet (same sequence as kAudio)
    uint16_t frames;       // always kPacketSamples
    uint8_t data[kPacketSamples];
};

// G.711 mu-law (the classic 8-bit telephone code: sign, 3-bit segment, 4-bit step).
inline uint8_t mulawEncode(int16_t pcm) {
    constexpr int kBias = 0x84, kClip = 32635;
    int s = pcm;
    const uint8_t sign = s < 0 ? 0x80 : 0;
    if (s < 0) s = -s;
    if (s > kClip) s = kClip;
    s += kBias;
    int exp = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) --exp;
    const int mant = (s >> (exp + 3)) & 0x0F;
    return uint8_t(~(sign | (exp << 4) | mant));
}
inline int16_t mulawDecode(uint8_t u) {
    u = uint8_t(~u);
    const int exp = (u >> 4) & 7, mant = u & 0x0F;
    const int mag = (((mant << 3) + 0x84) << exp) - 0x84;
    return int16_t((u & 0x80) ? -mag : mag);
}

struct __attribute__((packed)) PingPacket {
    Header h;
    uint32_t uptimeMs;
};

struct __attribute__((packed)) StatusPacket {
    Header h;
    uint8_t adapter;  // AdapterState
    int8_t rssi;      // RSSI of the local board's packets as heard by the remote
    uint16_t vid, pid;
    uint8_t channels, bits;
    uint32_t usbPackets, usbBadPackets, usbOverflows;
    uint32_t sent, sendFailed, uptimeMs;  // sendFailed = queueFull + noAck
    uint32_t queueFull;  // audio blocks refused by the ESP-NOW queue (even after retries)
    uint32_t noAck;      // packets the local board never acknowledged (after MAC retries)
    uint8_t filterMode;     // FilterMode in effect
    uint8_t filterLearned;  // 1 once the noise signature is learned
    uint8_t adapt;          // AdaptSpeed in effect
    char firmware[16];      // remote firmware version
    uint8_t noSignal;       // 1: the adapter streams, but only silence for 20 s (mic not connected?)
    uint8_t txPowerDbm;     // radio transmit power in effect
    uint8_t cpuMhz;         // processor speed in effect
    int16_t tempDeciC;      // chip (die) temperature in 0.1 C, or kTempUnknown: read at start, or on kTempRequest
    uint8_t highpassOn;     // high-pass filter in effect
    uint16_t highpassHz;    // its cut-off
    uint8_t tempFresh;      // 1: tempDeciC was read on kTempRequest (drive opening); 0: at the remote's start
    uint8_t micLevel;       // mic_level setting: 0..100 %, or kLevelDefault
    uint8_t hasLevel;       // the adapter has a level control (the values below are valid)
    int16_t levelMin, levelMax, levelNow;  // its range and current level, 1/256 dB
    uint32_t heapFree;      // internal RAM: free now, lowest since start, largest free block (bytes)
    uint32_t heapMin;
    uint32_t heapLargest;
    uint16_t radioQueued;   // packets handed to the radio and not yet reported sent
    uint32_t adapterInfoId; // id of the latest adapter's details (0: no adapter seen since start)
    uint8_t distance;       // Distance in effect
    uint8_t audioMode;      // AudioMode in effect
    uint8_t modeSetting;    // ModeSetting received from the local board
    uint8_t modeReason;     // ModeReason (kReasonDeciding: audioMode not final yet)
    uint8_t lockedTo[6];    // the local board this remote is locked to (all zeros: any)
    uint32_t rejected;      // hellos from other local boards refused (locked) or ignored (linked)
    uint8_t rejectedMac[6]; // the last of them
    uint8_t underrunsKnown; // speaker: the counts below are valid
    uint32_t spkUnderruns;  // speaker: playback buffer ran dry
    uint32_t spkLost;       // speaker: audio packets lost on the way
    char name[40];
    char problem[48];
    uint8_t board;          // BoardKind the remote runs on
};

// Settings the local board wants the remote to use. newPairId/newChannel differ from
// the current link only during a handover: the remote saves them, acknowledges,
// and both boards restart on the new mic number / channel.
constexpr uint8_t kLevelDefault = 255;  // mic_level = default: leave the adapter's level alone
inline bool levelValid(uint8_t v) { return v <= 100 || v == kLevelDefault; }

struct __attribute__((packed)) ConfigPacket {
    Header h;
    uint16_t seq;
    uint8_t newPairId, newChannel;
    uint8_t filterMode, adapt;
    uint8_t txPowerDbm;   // radio transmit power, 2..20 dBm (20 = maximum)
    uint8_t cpuMhz;       // processor speed, 240 or 160
    uint8_t highpassOn;   // high-pass filter on/off
    uint16_t highpassHz;  // its cut-off, 50..1000 Hz
    uint8_t reserved1;      // was temperatureOn (up to 1.4.3); 0
    uint8_t micLevel;       // the adapter's level: 0..100 % of its range, or kLevelDefault
    uint8_t distance;       // Distance: radio rate of the paired link
    uint8_t modeSetting;    // ModeSetting
    uint8_t lock;           // 1: lock the remote to this local board; 0: unlock (remote = any)
};

struct __attribute__((packed)) ConfigAckPacket {
    Header h;
    uint16_t seq;
    uint8_t pairId, channel;      // saved (will apply after a restart if they changed)
    uint8_t filterMode, adapt;    // in effect now
    uint8_t restarting;           // 1: mic number / channel changed, restarting now
    uint8_t txPowerDbm;           // in effect now
    uint8_t cpuMhz;               // in effect now
    uint8_t highpassOn;           // in effect now
    uint16_t highpassHz;          // in effect now
    uint8_t reserved1;            // was temperatureOn (up to 1.4.3); 0
    uint8_t micLevel;             // in effect now
    uint8_t distance;             // in effect now
    uint8_t modeSetting;          // saved (a change restarts the remote)
    uint8_t locked;               // 1: locked to this local board now
};

// The latest audio adapter's details (descriptors and what the remote made of them),
// sent in pieces when the local board asks for an id it does not have yet. The local
// board keeps the latest copy in flash and writes it out as adapter.txt (AdapterInfo.h).
constexpr uint16_t kInfoPiece = 1000;   // bytes per kAdapterInfo packet
constexpr uint16_t kInfoMax = 3000;     // whole record, at most
struct __attribute__((packed)) InfoRequestPacket {
    Header h;
    uint32_t id;
};
struct __attribute__((packed)) AdapterInfoPacket {
    Header h;
    uint32_t id;
    uint16_t total;   // bytes in the whole record
    uint16_t offset;  // where this piece goes
    uint16_t len;     // bytes in this piece
    uint8_t data[kInfoPiece];
};

// The noise filter's learned background noise (NoiseSignature.h: 257 bins of ~94 Hz),
// sent once when the local board asks (it opens the EXTENDER drive and draws noise.svg).
constexpr uint16_t kNoiseBins = 257;
constexpr int16_t kNoiseDbNone = -32768;
struct __attribute__((packed)) NoisePacket {
    Header h;
    uint8_t valid;        // 1: learned signature below; 0: none (filter off, learning, no adapter)
    uint8_t filterMode;   // FilterMode when taken
    uint32_t learnedS;    // seconds the signature has been learning (since the adapter stream began)
    int16_t deciDb[kNoiseBins];  // per bin, 0.1 dB re full scale
};

struct __attribute__((packed)) LogPacket {
    Header h;
    char text[160];  // NUL-terminated
};

static_assert(sizeof(AudioPacket) <= kMaxPacket, "audio packet too big for ESP-NOW v2");
static_assert(sizeof(StereoAudioPacket) <= kMaxPacket, "stereo audio packet too big");
static_assert(sizeof(StatusPacket) <= kMaxPacket, "status packet too big");
static_assert(sizeof(LogPacket) <= kMaxPacket, "log packet too big");
static_assert(sizeof(AdapterInfoPacket) <= kMaxPacket, "adapter info packet too big");
static_assert(sizeof(NoisePacket) <= kMaxPacket, "noise packet too big");

inline void fill(Header& h, Type type, uint8_t pairId, uint8_t flags = 0) {
    h.magic = kMagic;
    h.version = kVersion;
    h.type = type;
    h.pairId = pairId;
    h.flags = flags;
}

// Returns the packet type, or 0 if the packet is not ours (wrong magic,
// version or pair ID, or too short for its type).
inline uint8_t check(const uint8_t* data, int len, uint8_t pairId) {
    if (!data || len < (int)sizeof(Header)) return 0;
    const Header* h = reinterpret_cast<const Header*>(data);
    if (h->magic != kMagic || h->version != kVersion || h->pairId != pairId) return 0;
    switch (h->type) {
        case kBeacon:
            return len >= (int)sizeof(BeaconPacket) ? h->type : 0;
        case kHello:
        case kNoiseRequest:
            return h->type;
        case kNoiseData:
            return len >= (int)sizeof(NoisePacket) ? h->type : 0;
        case kPing:
            return len >= (int)sizeof(PingPacket) ? h->type : 0;
        case kAudio: {
            if (len < (int)offsetof(AudioPacket, pcm)) return 0;
            const AudioPacket* a = reinterpret_cast<const AudioPacket*>(data);
            if (a->samples > kPacketSamples) return 0;
            return len >= (int)(offsetof(AudioPacket, pcm) + a->samples * sizeof(int16_t)) ? h->type : 0;
        }
        case kMonoMulaw: {
            if (len < (int)sizeof(MonoMulawPacket)) return 0;
            return reinterpret_cast<const MonoMulawPacket*>(data)->frames == kPacketSamples ? h->type : 0;
        }
        case kMonoAdpcm: {
            if (len < (int)sizeof(MonoAdpcmPacket)) return 0;
            const MonoAdpcmPacket* a = reinterpret_cast<const MonoAdpcmPacket*>(data);
            return a->frames == kPacketSamples && a->index <= 88 ? h->type : 0;
        }
        case kStereoAudio: {
            if (len < (int)sizeof(StereoAudioPacket)) return 0;
            const StereoAudioPacket* a = reinterpret_cast<const StereoAudioPacket*>(data);
            return a->frames == kPacketSamples && a->index[0] <= 88 && a->index[1] <= 88 ? h->type : 0;
        }
        case kStatus:
            return len >= (int)sizeof(StatusPacket) ? h->type : 0;
        case kLog:
            return len > (int)sizeof(Header) ? h->type : 0;
        case kConfig:
            return len >= (int)sizeof(ConfigPacket) ? h->type : 0;
        case kConfigAck:
            return len >= (int)sizeof(ConfigAckPacket) ? h->type : 0;
        case kInfoRequest:
            return len >= (int)sizeof(InfoRequestPacket) ? h->type : 0;
        case kAdapterInfo: {
            if (len < (int)offsetof(AdapterInfoPacket, data)) return 0;
            const AdapterInfoPacket* a = reinterpret_cast<const AdapterInfoPacket*>(data);
            if (a->len > kInfoPiece || a->total > kInfoMax || a->offset + a->len > a->total) return 0;
            return len >= (int)(offsetof(AdapterInfoPacket, data) + a->len) ? h->type : 0;
        }
        default:
            return 0;
    }
}

inline bool macIsZero(const uint8_t m[6]) {
    for (int i = 0; i < 6; ++i)
        if (m[i]) return false;
    return true;
}

inline size_t audioPacketBytes(uint16_t samples) {
    return offsetof(AudioPacket, pcm) + samples * sizeof(int16_t);
}

}  // namespace wlink
