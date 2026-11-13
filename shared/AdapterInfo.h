#pragma once
// Wireless USB Audio Extender - the latest audio adapter's details ("adapter record").
//
// SHARED FILE: an identical copy lives in AudioExtender_Remote/ and
// AudioExtender_Local/. Edit shared/AdapterInfo.h and run tools/sync_shared.sh.
//
// The remote builds the record when an adapter is plugged in (its descriptors plus
// what the remote made of them) and sends it to the local board on request
// (LinkProtocol.h: kInfoRequest / kAdapterInfo). The local board keeps the latest one
// in flash and, when the EXTENDER drive opens, writes it out as adapter.txt with
// render(). Plain C++: tests/feature_tests.cpp checks it on the host.
//
// Record: RecordHeader, then the configuration descriptor (configLen bytes).
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace adinfo {

constexpr uint8_t kRecordFormat = 1;

struct __attribute__((packed)) RecordHeader {
    uint8_t format;           // kRecordFormat
    uint8_t state;            // wlink::AdapterState when last seen: 1 streaming, 2 not usable
    uint8_t speed;            // 0 low, 1 full, 2 high
    uint8_t usedInterface;    // the remote's choice (0xFF: none usable)
    uint8_t usedAlt;
    uint8_t usedEndpoint;
    uint8_t hasVolume;        // level control found (range below valid)
    int16_t volMin, volMax, volRes, volCur;  // 1/256 dB
    uint8_t channels, bits;   // the stream in use
    uint16_t configTotal;     // wTotalLength as reported by the adapter
    uint16_t configLen;       // configuration descriptor bytes that follow
    uint32_t remoteUptimeS;   // remote uptime when the adapter was seen
    char firmware[16];        // remote firmware
    char manufacturer[48], product[48], serial[48];
    char problem[64];         // why it is not usable ("" when streaming)
    uint8_t device[18];       // device descriptor
};

// Whole record size, or 0 if buf/len is not a valid record.
inline size_t recordSize(const uint8_t* buf, size_t len) {
    if (!buf || len < sizeof(RecordHeader)) return 0;
    RecordHeader h;
    memcpy(&h, buf, sizeof(h));
    if (h.format != kRecordFormat) return 0;
    const size_t n = sizeof(RecordHeader) + h.configLen;
    return n <= len ? n : 0;
}

// ---------------------------------------------------------------- text output
struct Out {
    void (*write)(void* ctx, const char* text);
    void* ctx;
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[200];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        write(ctx, b);
    }
};

namespace detail {
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le24(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (uint32_t(p[2]) << 16)); }

inline const char* terminalName(uint16_t t) {
    switch (t) {
        case 0x0101: return "USB streaming";
        case 0x01FF: return "USB vendor specific";
        case 0x0200: return "input (undefined)";
        case 0x0201: return "microphone";
        case 0x0202: return "desktop microphone";
        case 0x0203: return "personal microphone";
        case 0x0204: return "omni-directional microphone";
        case 0x0205: return "microphone array";
        case 0x0300: return "output (undefined)";
        case 0x0301: return "speaker";
        case 0x0302: return "headphones";
        case 0x0400: return "bi-directional (undefined)";
        case 0x0401: return "handset";
        case 0x0402: return "headset";
        case 0x0403: return "speakerphone";
        case 0x0600: return "external (undefined)";
        case 0x0601: return "analog connector";
        case 0x0602: return "digital audio interface";
        case 0x0603: return "line connector";
        case 0x0605: return "S/PDIF";
        default: return nullptr;
    }
}

inline void terminalText(uint16_t t, char* out, size_t cap) {
    const char* n = terminalName(t);
    if (n) snprintf(out, cap, "%s", n);
    else snprintf(out, cap, "type 0x%04x", t);
}

inline const char* formatTagName(uint16_t f) {
    switch (f) {
        case 1: return "PCM";
        case 2: return "PCM8";
        case 3: return "IEEE float";
        case 4: return "A-law";
        case 5: return "mu-law";
        default: return "other format";
    }
}

inline const char* speedText(uint8_t s) {
    return s == 0 ? "low speed (1.5 Mbit/s)" : s == 1 ? "full speed (12 Mbit/s)" : "high speed (480 Mbit/s)";
}

// Feature Unit controls (UAC1 bmaControls bits) as a list.
inline void controlsText(uint32_t bits, char* out, size_t cap) {
    static const char* const names[] = {"mute", "volume", "bass", "mid", "treble", "equalizer", "AGC",
                                        "delay", "bass boost", "loudness"};
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < 10; ++i) {
        if (!(bits & (1u << i))) continue;
        const int n = snprintf(out + o, cap - o, "%s%s", o ? ", " : "", names[i]);
        if (n < 0 || size_t(n) >= cap - o) break;
        o += size_t(n);
    }
    if (!o) snprintf(out, cap, "none");
}

inline void ids(const uint8_t* d, int count, char* out, size_t cap) {  // "1, 2, 5"
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < count; ++i) {
        const int n = snprintf(out + o, cap - o, "%s%u", i ? ", " : "", d[i]);
        if (n < 0 || size_t(n) >= cap - o) break;
        o += size_t(n);
    }
}

inline void hexDump(Out& out, const uint8_t* d, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        char line[64];
        size_t o = 0;
        for (size_t k = i; k < len && k < i + 16; ++k) o += size_t(snprintf(line + o, sizeof(line) - o, "%02x ", d[k]));
        if (o) line[o - 1] = 0;
        out.printf("    %s\r\n", line);
    }
}

// Audio Control interface: class-specific units and terminals (UAC1 layout).
inline void controlDescriptor(Out& out, const uint8_t* d) {
    const uint8_t len = d[0], sub = d[2];
    char t[48], list[96];
    switch (sub) {
        case 1:
            if (len >= 8) out.printf("      audio class %x.%02x\r\n", d[4], d[3]);
            break;
        case 2:  // input terminal
            if (len >= 12) {
                terminalText(le16(d + 4), t, sizeof(t));
                out.printf("      input terminal %u: %s, %u channel%s\r\n", d[3], t, d[7], d[7] == 1 ? "" : "s");
            }
            break;
        case 3:  // output terminal
            if (len >= 9) {
                terminalText(le16(d + 4), t, sizeof(t));
                out.printf("      output terminal %u: %s, from %u\r\n", d[3], t, d[7]);
            }
            break;
        case 4:  // mixer unit
        case 5:  // selector unit
            if (len >= 5 && 5 + d[4] <= len) {
                ids(d + 5, d[4], list, sizeof(list));
                out.printf("      %s unit %u: from %s\r\n", sub == 4 ? "mixer" : "selector", d[3], list);
            }
            break;
        case 6:  // feature unit
            if (len >= 7 && d[5]) {
                const uint8_t n = d[5];
                const int count = (len - 7) / n;
                out.printf("      feature unit %u: from %u\r\n", d[3], d[4]);
                for (int ch = 0; ch < count; ++ch) {
                    uint32_t bits = 0;
                    for (int b = 0; b < n && b < 4; ++b) bits |= uint32_t(d[6 + ch * n + b]) << (8 * b);
                    if (!bits) continue;
                    controlsText(bits, list, sizeof(list));
                    if (ch == 0) out.printf("        all channels: %s\r\n", list);
                    else out.printf("        channel %d: %s\r\n", ch, list);
                }
            }
            break;
        case 7:  // processing unit
        case 8:  // extension unit
            if (len >= 7 && 7 + d[6] <= len) {
                ids(d + 7, d[6], list, sizeof(list));
                out.printf("      %s unit %u (code 0x%04x): from %s\r\n", sub == 7 ? "processing" : "extension", d[3],
                           le16(d + 4), list);
            }
            break;
        default:
            break;
    }
}

inline void interfaceName(uint8_t cls, uint8_t sub, uint8_t proto, char* out, size_t cap) {
    if (cls == 1 && sub == 1) snprintf(out, cap, "audio control%s", proto == 0x20 ? " (USB Audio Class 2)" : "");
    else if (cls == 1 && sub == 2) snprintf(out, cap, "audio streaming%s", proto == 0x20 ? " (USB Audio Class 2)" : "");
    else if (cls == 1 && sub == 3) snprintf(out, cap, "MIDI");
    else if (cls == 1) snprintf(out, cap, "audio (subclass %u)", sub);
    else if (cls == 3) snprintf(out, cap, "HID (buttons / keys)");
    else if (cls == 0xFE) snprintf(out, cap, "application specific (e.g. firmware update)");
    else if (cls == 0xFF) snprintf(out, cap, "vendor specific");
    else snprintf(out, cap, "class 0x%02x", cls);
}
}  // namespace detail

// Writes the record as text (adapter.txt). seen: when / how the local board got it.
inline void render(const uint8_t* rec, size_t len, const char* seen, Out& out) {
    using namespace detail;
    const size_t total = recordSize(rec, len);
    out.printf("Wireless USB Audio Extender - the remote's latest audio adapter\r\n");
    out.printf("(%s)\r\n\r\n", seen);
    if (!total) {
        out.printf("No adapter details yet: plug an audio adapter into the remote while both boards are linked.\r\n");
        return;
    }
    RecordHeader h;
    memcpy(&h, rec, sizeof(h));
    h.firmware[sizeof(h.firmware) - 1] = h.manufacturer[sizeof(h.manufacturer) - 1] = 0;
    h.product[sizeof(h.product) - 1] = h.serial[sizeof(h.serial) - 1] = h.problem[sizeof(h.problem) - 1] = 0;
    const uint8_t* cfg = rec + sizeof(RecordHeader);
    const uint8_t* dev = h.device;

    // ---- result
    if (h.state == 1) out.printf("Result: streaming, %u channel%s, %u-bit, 48000 Hz\r\n", h.channels,
                                 h.channels == 1 ? "" : "s", h.bits);
    else out.printf("Result: NOT USABLE - %s\r\n", h.problem[0] ? h.problem : "reason unknown");
    if (h.usedInterface != 0xFF)
        out.printf("  used by the extender: interface %u, alternate setting %u, endpoint 0x%02x\r\n", h.usedInterface,
                   h.usedAlt, h.usedEndpoint);
    else
        out.printf("  used by the extender: nothing (no USB Audio Class 1 microphone at 48000 Hz found)\r\n");
    if (h.hasVolume)
        out.printf("  microphone level control: %.1f to %.1f dB in %.2f dB steps, left at %.1f dB\r\n", h.volMin / 256.0,
                   h.volMax / 256.0, h.volRes / 256.0, h.volCur / 256.0);
    else if (h.state == 1)
        out.printf("  microphone level control: none offered\r\n");
    {
        const uint32_t s = h.remoteUptimeS;
        out.printf("  seen by remote firmware %s, %lu:%02lu:%02lu after the remote started\r\n", h.firmware,
                   (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    }

    // ---- device
    out.printf("\r\nDevice\r\n");
    out.printf("  ID (vendor:product)  %04x:%04x\r\n", le16(dev + 8), le16(dev + 10));
    out.printf("  manufacturer         %s\r\n", h.manufacturer[0] ? h.manufacturer : "(none)");
    out.printf("  product              %s\r\n", h.product[0] ? h.product : "(none)");
    out.printf("  serial number        %s\r\n", h.serial[0] ? h.serial : "(none)");
    out.printf("  USB %x.%02x, %s, device release %x.%02x\r\n", dev[3], dev[2], speedText(h.speed), dev[13], dev[12]);
    if (cfg && h.configLen >= 9 && cfg[1] == 2) {
        const uint8_t attr = cfg[7];
        out.printf("  %u interface%s, %s, up to %u mA%s\r\n", cfg[4], cfg[4] == 1 ? "" : "s",
                   (attr & 0x40) ? "self powered" : "bus powered", cfg[8] * 2u, (attr & 0x20) ? ", remote wakeup" : "");
    }
    if (h.configLen < h.configTotal)
        out.printf("  (configuration descriptor is %u bytes; only the first %u were kept)\r\n", h.configTotal,
                   h.configLen);

    // ---- interfaces
    out.printf("\r\nInterfaces\r\n");
    uint8_t ifCls = 0, ifSub = 0, ifProto = 0, ifNum = 0, ifAlt = 0;
    bool inStreamAlt = false;
    for (size_t i = 0; i + 2 <= h.configLen;) {
        const uint8_t* d = cfg + i;
        const uint8_t dl = d[0];
        if (dl < 2 || i + dl > h.configLen) {
            out.printf("  (descriptor at byte %u is damaged; stopped here)\r\n", unsigned(i));
            break;
        }
        const uint8_t type = d[1];
        char name[64], text[96];
        if (type == 4 && dl >= 9) {  // interface
            ifNum = d[2];
            ifAlt = d[3];
            ifCls = d[5];
            ifSub = d[6];
            ifProto = d[7];
            inStreamAlt = ifCls == 1 && ifSub == 2;
            interfaceName(ifCls, ifSub, ifProto, name, sizeof(name));
            const bool used = ifNum == h.usedInterface && ifAlt == h.usedAlt;
            if (ifCls == 1 && ifSub == 2 && d[4] == 0)
                out.printf("  %u.%u %s: idle (no endpoints)\r\n", ifNum, ifAlt, name);
            else
                out.printf("  %u.%u %s%s\r\n", ifNum, ifAlt, name, used ? "   <- used by the extender" : "");
        } else if (type == 0x24 && ifCls == 1 && ifSub == 1 && ifProto != 0x20) {  // UAC1 audio control
            controlDescriptor(out, d);
        } else if (type == 0x24 && inStreamAlt && ifProto != 0x20) {  // UAC1 audio streaming
            if (d[2] == 1 && dl >= 7) {
                out.printf("      %s, linked to terminal %u\r\n", formatTagName(le16(d + 5)), d[3]);
            } else if (d[2] == 2 && dl >= 8 && d[3] == 1) {
                const uint8_t n = d[7];
                size_t o = 0;
                text[0] = 0;
                if (n == 0 && dl >= 14) {
                    snprintf(text, sizeof(text), "%lu to %lu Hz", (unsigned long)le24(d + 8), (unsigned long)le24(d + 11));
                } else {
                    for (uint8_t k = 0; k < n && 8u + 3u * (k + 1u) <= dl; ++k) {
                        const int w = snprintf(text + o, sizeof(text) - o, "%s%lu", k ? ", " : "", (unsigned long)le24(d + 8 + 3 * k));
                        if (w < 0 || size_t(w) >= sizeof(text) - o) break;
                        o += size_t(w);
                    }
                    if (o + 4 < sizeof(text)) strcat(text, " Hz");
                }
                out.printf("      %u channel%s, %u-bit in %u-byte samples, %s\r\n", d[4], d[4] == 1 ? "" : "s", d[6],
                           d[5], text);
            } else if (d[2] == 2 && dl >= 4) {
                out.printf("      format type %u (not PCM type I)\r\n", d[3]);
            }
        } else if (type == 5 && dl >= 7) {  // endpoint
            const uint8_t addr = d[2], attr = d[3];
            const uint16_t mps = le16(d + 4) & 0x7FF;
            static const char* const kTransfer[] = {"control", "isochronous", "bulk", "interrupt"};
            static const char* const kSync[] = {"no sync", "asynchronous", "adaptive", "synchronous"};
            const uint8_t tt = attr & 3;
            if (tt == 1) {
                // Isochronous: 2^(bInterval-1) frames (full speed) or microframes (high speed).
                const uint8_t bi = d[6] >= 1 && d[6] <= 16 ? d[6] : 1;
                const double ms = (h.speed == 2 ? 0.125 : 1.0) * double(1u << (bi - 1));
                out.printf("      endpoint 0x%02x %s %s, %s, up to %u bytes every %g ms%s\r\n", addr,
                           (addr & 0x80) ? "IN" : "OUT", kTransfer[tt], kSync[(attr >> 2) & 3], mps, ms,
                           inStreamAlt ? ((addr & 0x80) ? " (recording)" : " (playback)") : "");
            } else {
                out.printf("      endpoint 0x%02x %s %s, up to %u bytes\r\n", addr, (addr & 0x80) ? "IN" : "OUT",
                           kTransfer[tt], mps);
            }
        } else if (type == 0x25 && dl >= 4 && d[2] == 1 && inStreamAlt) {  // audio endpoint
            const uint8_t a = d[3];
            out.printf("        sampling rate %s%s\r\n", (a & 1) ? "can be set" : "fixed (cannot be set)",
                       (a & 2) ? ", pitch control" : "");
        }
        i += dl;
    }

    // ---- raw
    out.printf("\r\nRaw descriptors (hex), for bug reports\r\n");
    out.printf("  device:\r\n");
    hexDump(out, dev, sizeof(h.device));
    out.printf("  configuration (%u bytes):\r\n", h.configLen);
    hexDump(out, cfg, h.configLen);
}

}  // namespace adinfo
