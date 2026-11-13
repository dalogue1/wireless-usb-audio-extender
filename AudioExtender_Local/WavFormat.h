#pragma once
// Minimal WAV (RIFF) header parsing for replacement voice files.
// Accepted: PCM (format 1, or WAVE_FORMAT_EXTENSIBLE with PCM), 16-bit,
// mono or stereo, 8 000..48 000 Hz. Plain C++ (tests/wav_format_test.cpp).
#include <stdint.h>
#include <string.h>

namespace wav {

struct Info {
    bool ok = false;
    const char* problem = "not checked";
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    uint32_t dataOffset = 0;  // byte offset of the samples in the file
    uint32_t dataBytes = 0;
    uint32_t frames() const { return channels && bits ? dataBytes / (channels * (bits / 8)) : 0; }
};

inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// head: the first bytes of the file (512 is plenty for normal files);
// fileSize: the whole file's size (clips the data chunk if the header lies).
inline Info parse(const uint8_t* head, uint32_t headLen, uint32_t fileSize) {
    Info info;
    if (headLen < 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
        info.problem = "not a WAV file";
        return info;
    }
    bool haveFmt = false;
    uint32_t off = 12;
    while (off + 8 <= headLen) {
        const uint32_t size = le32(head + off + 4);
        if (memcmp(head + off, "fmt ", 4) == 0) {
            if (size < 16 || off + 8 + 16 > headLen) { info.problem = "bad fmt chunk"; return info; }
            const uint8_t* f = head + off + 8;
            uint16_t format = le16(f);
            if (format == 0xFFFE && size >= 40 && off + 8 + 26 <= headLen) format = le16(f + 24);  // extensible
            info.channels = le16(f + 2);
            info.rate = le32(f + 4);
            info.bits = le16(f + 14);
            if (format != 1) { info.problem = "not PCM (export as 16-bit PCM WAV)"; return info; }
            haveFmt = true;
        } else if (memcmp(head + off, "data", 4) == 0) {
            if (!haveFmt) { info.problem = "data before fmt"; return info; }
            info.dataOffset = off + 8;
            info.dataBytes = size;
            if (info.dataOffset + info.dataBytes > fileSize) info.dataBytes = fileSize > info.dataOffset ? fileSize - info.dataOffset : 0;
            if (info.bits != 16) { info.problem = "not 16-bit"; return info; }
            if (info.channels < 1 || info.channels > 2) { info.problem = "not mono or stereo"; return info; }
            if (info.rate < 8000 || info.rate > 48000) { info.problem = "sample rate not 8-48 kHz"; return info; }
            if (info.frames() < 160) { info.problem = "too short"; return info; }
            info.ok = true;
            info.problem = "";
            return info;
        }
        off += 8 + size + (size & 1);  // chunks are padded to even sizes
    }
    info.problem = haveFmt ? "no data chunk in the first 512 bytes" : "no fmt chunk";
    return info;
}

}  // namespace wav
