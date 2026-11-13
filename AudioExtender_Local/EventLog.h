#pragma once
// Wireless USB Audio Extender local - the event log: critical events kept in flash, so they
// survive restarts AND power cuts (an unplugged board, a USB port that lost power).
// Written out as events.txt when the EXTENDER drive opens.
//
// Storage: a small raw flash partition ("evlog", no file system) used as a ring of
// 4 KB sectors. Every event is one 16-byte record with a checksum, written at once:
// a power cut loses at most the record being written and cannot damage the rest.
// A 64 KB area holds about 4000 events. Only erasing wears flash: one sector erase
// per 255 events.
//
// Record: code, a8, start (which start of the board), t (seconds since that start),
// a32, a16, crc16. The words for each code are in render(), not in flash. Events
// without a code of their own are a TEXT record plus 13-byte DATA records.
// The first record of every sector is a SECTOR record carrying the sector's sequence
// number; the sector with the highest one is the newest.
//
// Plain C++ with a Flash interface: tests/event_log_test.cpp simulates the flash
// (NOR rules: writes only clear bits, erase sets a sector to 0xFF) and power cuts.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace evlog {

// ---------------------------------------------------------------- event codes
enum Code : uint8_t {
    kSector = 1,        // a32 = sector sequence number
    kStart = 2,         // a8 = reset reason (esp_reset_reason_t), a32 = version (major<<16 | minor<<8 | patch)
    kText = 3,          // a8 = text length, a16 = source (0 local, 1 remote); DATA records follow
    kUsbSetUp = 10,     // a16 = how many times since the last record of this kind (1 = once)
    kUsbReleased = 11,
    kUsbSuspended = 12,
    kUsbResumed = 13,
    kUsbStall = 14,         // stream open but the PC stopped collecting audio
    kUsbStallCleared = 15,  // a32 = stall length, ms
    kUsbReattach = 16,      // a8 = 1 not set up by the PC / 2 audio stalled, a32 = ms waited
    kUsbStallRestart = 17,  // stalled again within 2 min of a re-attach: restarting
    kLinkUp = 20,           // a8 = mic, a32 = ms down (0: first link since start)
    kLinkLost = 21,         // a8 = mic
    kRadioRestart = 22,     // local radio restarted while the link was down; a8 = 1 failed
    kRemoteRestarted = 23,  // the remote's uptime went back; a32 = its uptime before, s
    kAdapterOk = 30,        // a32 = vid << 16 | pid, a8 = channels, a16 = bits
    kAdapterNone = 31,      // adapter removed
    kNoSignal = 33,
    kSignalBack = 34,
    kTempHigh = 40,         // a8 = 0 local / 1 remote, a16 = 0.1 C (int16)
    kTempOk = 41,
    kDriveOpened = 50,
    kFactoryReset = 51,
    kModeChange = 52,       // a8 = new wlink::AudioMode, a16 = wlink::ModeReason
    // Health snapshot (three records with the same time), hourly and just before critical
    // events. Counts are since the previous snapshot.
    kHealthRadio = 60,  // a8 = RSSI the local hears (int8), a16 = RSSI the remote hears (int8) | flags << 8,
                        // a32 = remote radio queue << 16 | remote sends failed (queue full + no ack)
    kHealthTemp = 61,   // a8 = audio blocks late, a16 = remote temp 0.1 C (int16),
                        // a32 = local temp 0.1 C (int16) << 16 | audio blocks lost
    kHealthMem = 62,    // a8 = buffer underruns, a16 = remote largest free block KB,
                        // a32 = remote free KB << 16 | local free KB
    kData = 0xFE,           // 13 bytes of the preceding TEXT record
};

// ---------------------------------------------------------------- flash access
struct Flash {
    virtual ~Flash() {}
    virtual uint32_t size() = 0;  // bytes, a multiple of kSectorSize
    virtual bool read(uint32_t offset, void* buf, uint32_t len) = 0;
    virtual bool write(uint32_t offset, const void* buf, uint32_t len) = 0;
    virtual bool erase(uint32_t offset, uint32_t len) = 0;  // whole sectors
};

enum HealthFlag : uint8_t {
    kHfLinked = 0x01,      // linked when taken
    kHfRemote = 0x02,      // remote values valid (its last status)
    kHfRecording = 0x04,   // the PC was recording
    kHfEvent = 0x08,       // taken for the event that follows (else hourly)
};
constexpr int16_t kTempNone = -32768;

constexpr uint32_t kSectorSize = 4096;
constexpr uint32_t kRecSize = 16;
constexpr uint32_t kPerSector = kSectorSize / kRecSize;  // incl. the SECTOR record
constexpr size_t kMaxText = 159;  // a remote log line (160-byte packet, minus its NUL) fits whole

struct __attribute__((packed)) Rec {
    uint8_t code;
    uint8_t a8;
    uint16_t start;
    uint32_t t;
    uint32_t a32;
    uint16_t a16;
    uint16_t crc;
};
static_assert(sizeof(Rec) == kRecSize, "16-byte records");

inline uint16_t crc16(const uint8_t* d, size_t n) {  // CRC-16/CCITT-FALSE
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}
inline bool erased(const uint8_t* d) {
    for (uint32_t i = 0; i < kRecSize; ++i)
        if (d[i] != 0xFF) return false;
    return true;
}
inline bool valid(const uint8_t* d) {
    if (d[0] == 0xFF || d[0] == 0) return false;
    const uint16_t c = uint16_t(d[14] | (d[15] << 8));
    return c == crc16(d, 14);
}

// ---------------------------------------------------------------- text output
struct Out {
    void (*write)(void* ctx, const char* text);
    void* ctx;
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[320];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        write(ctx, b);
    }
};

// Why the remote chose its mode (wlink::ModeReason order).
inline const char* modeReasonText(uint8_t r) {
    switch (r) {
        case 1: return "config.txt setting";
        case 2: return "the adapter only records";
        case 3: return "the adapter only plays";
        case 4: return "microphone signal at start-up";
        case 5: return "no microphone signal at start-up";
        case 6: return "remote has built-in mic only";
        default: return "not decided yet";
    }
}

inline const char* resetText(uint8_t r) {
    switch (r) {
        case 1: return "power on (plugged in, or the USB port's power was cut)";
        case 2: return "reset pin";
        case 3: return "restart by the firmware";
        case 4: return "crash";
        case 5: case 6: case 7: return "watchdog (the program had stopped)";
        case 8: return "wake from deep sleep";
        case 9: return "power dip (brownout)";
        case 11: return "reset through USB by the PC";
        case 14: return "power glitch";
        default: return "other reason";
    }
}

// ---------------------------------------------------------------- the log
class Log {
public:
    // Scans the flash: finds the newest sector, where to write next, and this start's
    // number (the last one recorded + 1). False: no usable flash area.
    bool begin(Flash* f) {
        flash_ = f;
        sectors_ = f ? f->size() / kSectorSize : 0;
        if (sectors_ < 2) {
            flash_ = nullptr;
            return false;
        }
        uint32_t bestSeq = 0;
        int best = -1;
        for (uint32_t s = 0; s < sectors_; ++s) {
            uint32_t seq;
            if (sectorSeq(s, seq) && (best < 0 || int32_t(seq - bestSeq) > 0)) {
                best = int(s);
                bestSeq = seq;
            }
        }
        lastStart_ = 0;
        if (best < 0) {  // empty (or unreadable): the first add starts at sector 0
            head_ = sectors_ - 1;
            seq_ = 0;
            slot_ = kPerSector;  // forces a fresh sector on the first add
        } else {
            head_ = uint32_t(best);
            seq_ = bestSeq;
            // Write after the last slot that is not erased (a damaged record is skipped).
            slot_ = 1;
            uint8_t r[kRecSize];
            for (uint32_t i = 1; i < kPerSector; ++i) {
                if (!f->read(head_ * kSectorSize + i * kRecSize, r, kRecSize)) break;
                if (!erased(r)) slot_ = i + 1;
            }
            // This start's number: one after the newest record's start.
            forEachRecord([this](const Rec& r) {
                if (r.code != kSector && r.code != kData) lastStart_ = r.start;
            });
        }
        start_ = uint16_t(lastStart_ + 1);
        if (!start_) start_ = 1;
        return true;
    }
    bool ok() const { return flash_ != nullptr; }
    uint16_t start() const { return start_; }
    uint32_t written() const { return written_; }

    bool add(uint8_t code, uint32_t tSec, uint8_t a8 = 0, uint16_t a16 = 0, uint32_t a32 = 0) {
        Rec r;
        r.code = code;
        r.a8 = a8;
        r.start = start_;
        r.t = tSec;
        r.a32 = a32;
        r.a16 = a16;
        return put(r);
    }

    // An event without a code of its own: the text itself (up to kMaxText characters).
    bool addText(uint32_t tSec, const char* text, uint16_t source = 0) {
        if (!flash_) return false;
        size_t n = strlen(text);
        if (n > kMaxText) n = kMaxText;
        if (!add(kText, tSec, uint8_t(n), source, 0)) return false;
        for (size_t o = 0; o < n; o += 13) {
            uint8_t d[kRecSize];
            memset(d, 0, sizeof(d));
            d[0] = kData;
            memcpy(d + 1, text + o, n - o < 13 ? n - o : 13);
            Rec r;
            memcpy(&r, d, sizeof(r));
            if (!put(r)) return false;
        }
        return true;
    }

    // Every valid record, oldest first.
    template <class F>
    void forEachRecord(F fn) {
        if (!flash_) return;
        // Sectors in sequence order: the oldest is the one after the head going round.
        for (uint32_t k = 1; k <= sectors_; ++k) {
            const uint32_t s = (head_ + k) % sectors_;
            uint32_t seq;
            if (!sectorSeq(s, seq)) continue;
            if (s != head_ && int32_t(seq - seq_) > 0) continue;  // stale (should not happen)
            uint8_t buf[kRecSize];
            for (uint32_t i = 1; i < kPerSector; ++i) {
                if (!flash_->read(s * kSectorSize + i * kRecSize, buf, kRecSize)) break;
                if (!valid(buf)) continue;
                Rec r;
                memcpy(&r, buf, sizeof(r));
                fn(r);
            }
        }
    }

    // One rendered line of the log (events.txt and report.html share it).
    struct LineInfo {
        uint16_t start;   // the start it belongs to
        uint32_t t;       // seconds since that start
        uint8_t code;     // event code (kText for notes, kHealthRadio for a health line)
        bool health;      // a health snapshot line
        bool hourly;      // ...taken on the hour (else just before an event)
        const Rec* snap;  // health: its three records (radio, temp, mem); mask tells which arrived
        uint8_t mask;
    };

    // Every event as one line of text (without the "start  time" prefix), oldest first.
    template <class F>
    void lines(F fn) {
        if (!flash_) return;
        char text[kMaxText + 1];
        size_t textLen = 0, textWant = 0;
        Rec textRec = {};
        Rec health[3] = {};
        uint8_t healthMask = 0;
        uint16_t snapStart = 0;
        uint32_t snapT = 0;
        auto flushHealth = [&]() {
            if (!healthMask) return;
            const Rec& any = (healthMask & 1) ? health[0] : (healthMask & 2) ? health[1] : health[2];
            Line L;
            healthLine(L, health, healthMask);
            const uint8_t flags = (healthMask & 1) ? uint8_t(health[0].a16 >> 8) : 0;
            LineInfo i{any.start, any.t, kHealthRadio, true, !(flags & kHfEvent), health, healthMask};
            fn(i, L.b);
            healthMask = 0;
        };
        auto flushText = [&]() {
            if (!textWant) return;
            text[textLen] = 0;
            Line L;
            L("%s%s%s", textRec.a16 == 1 ? "remote: " : "", text, textLen < textWant ? " [...]" : "");
            LineInfo i{textRec.start, textRec.t, kText, false, false, nullptr, 0};
            fn(i, L.b);
            textWant = textLen = 0;
        };
        forEachRecord([&](const Rec& r) {
            if (r.code == kData) {
                if (textWant) {
                    const uint8_t* d = reinterpret_cast<const uint8_t*>(&r) + 1;
                    for (int i = 0; i < 13 && textLen < textWant; ++i) text[textLen++] = char(d[i]);
                    if (textLen >= textWant) flushText();
                }
                return;
            }
            flushText();  // a TEXT whose pieces were cut short
            if (r.code == kSector) return;
            if (r.code >= kHealthRadio && r.code <= kHealthMem) {
                const int i = r.code - kHealthRadio;
                if ((healthMask & (1 << i)) || (healthMask && (r.start != snapStart || r.t != snapT)))
                    flushHealth();  // a different snapshot
                if (!healthMask) {
                    snapStart = r.start;
                    snapT = r.t;
                }
                health[i] = r;
                healthMask |= uint8_t(1 << i);
                if (healthMask == 7) flushHealth();
                return;
            }
            flushHealth();
            if (r.code == kText) {
                textRec = r;
                textWant = r.a8;
                textLen = 0;
                if (!textWant) {
                    LineInfo i{r.start, r.t, kText, false, false, nullptr, 0};
                    fn(i, "(empty note)");
                }
                return;
            }
            Line L;
            describe(L, r);
            LineInfo i{r.start, r.t, r.code, false, false, nullptr, 0};
            fn(i, L.b);
        });
        flushText();
        flushHealth();
    }

    // events.txt: every event, oldest first. skipLines: leave out that many of the oldest
    // lines (the drive is nearly full); a note says so.
    void render(Out& out, uint32_t nowSec, uint32_t skipLines = 0) {
        out.printf("Wireless USB Audio Extender local - event log (oldest first)\r\n");
        out.printf("Events are grouped by start of the local board; times are since that start (it has no clock).\r\n");
        out.printf("Written during start %u, %s after it began.\r\n\r\n", start_, dur(nowSec).s);
        if (!flash_) {
            out.printf("No event log on this board: flash the full image once to create its flash area.\r\n");
            return;
        }
        if (skipLines)
            out.printf("(The drive was nearly full: the oldest %lu lines are left out. The board still has them.)\r\n\r\n",
                       (unsigned long)skipLines);
        bool any = false;
        uint32_t n = 0;
        int32_t heading = -1;  // the start number of the group being written
        lines([&](const LineInfo& i, const char* text) {
            if (n++ < skipLines) return;
            if (int32_t(i.start) != heading || (i.code == kStart && any)) {  // a new group: blank line + heading
                if (any) out.printf("\r\n");
                out.printf("start %u\r\n", i.start);
                heading = int32_t(i.start);
            }
            any = true;
            out.printf("%11s  %s\r\n", dur(i.t).s, text);  // lines up to "9d 23:59:59"
        });
        if (!any) out.printf("No events recorded yet.\r\n");
    }
    // Lines render() would write (for shortening events.txt to fit the drive).
    uint32_t lineCount() {
        uint32_t n = 0;
        lines([&](const LineInfo&, const char*) { n++; });
        return n;
    }

    struct Dur { char s[24]; };
    static Dur dur(uint32_t sec) {
        Dur d;
        const uint32_t days = sec / 86400;
        if (days) snprintf(d.s, sizeof(d.s), "%lud %02lu:%02lu:%02lu", (unsigned long)days,
                           (unsigned long)(sec / 3600 % 24), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
        else snprintf(d.s, sizeof(d.s), "%lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60),
                      (unsigned long)(sec % 60));
        return d;
    }
    // "255+" when a one-byte count was capped, "65535+" for two bytes.
    struct Count { char s[12]; };
    static Count count8(uint8_t v) { Count c; snprintf(c.s, sizeof(c.s), v == 255 ? "255+" : "%u", unsigned(v)); return c; }
    static Count count16(uint32_t v) { Count c; snprintf(c.s, sizeof(c.s), v == 65535 ? "65535+" : "%lu", (unsigned long)v); return c; }

private:
    struct Line {  // a growing line of text
        char b[288] = {0};
        size_t o = 0;
        void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
            va_list ap;
            va_start(ap, fmt);
            const int n = vsnprintf(b + o, sizeof(b) - o, fmt, ap);
            va_end(ap);
            if (n > 0) o = o + size_t(n) < sizeof(b) ? o + size_t(n) : sizeof(b) - 1;
        }
    };
    static void tempText(int16_t t, char* b, size_t n) {
        if (t == kTempNone) snprintf(b, n, "?");
        else snprintf(b, n, "%.1f C", t / 10.0);
    }
    // One line from a snapshot's records (mask: which of the three arrived).
    static void healthLine(Line& add, const Rec* h, uint8_t mask) {
        const uint8_t flags = (mask & 1) ? uint8_t(h[0].a16 >> 8) : 0;
        const bool remote = flags & kHfRemote;
        add("health%s: %s", (flags & kHfEvent) ? "" : " (hourly)", (flags & kHfLinked) ? "linked" : "NOT linked");
        if (mask & 1) {
            add(", RSSI %d dBm", int(int8_t(h[0].a8)));
            if (remote) add(" (remote hears %d), remote queue %s, sends failed %s", int(int8_t(h[0].a16 & 0xFF)),
                            count16(h[0].a32 >> 16).s, count16(h[0].a32 & 0xFFFF).s);
        }
        if (mask & 2) {
            char rt[16], lt[16];
            tempText(int16_t(h[1].a16), rt, sizeof(rt));
            tempText(int16_t(h[1].a32 >> 16), lt, sizeof(lt));
            if (remote) add(" | temp remote %s, local %s", rt, lt);
            else add(" | temp local %s", lt);
            add(" | audio lost %s, late %s", count16(h[1].a32 & 0xFFFF).s, count8(h[1].a8).s);
        }
        if (mask & 4) {
            add(", underruns %s", count8(h[2].a8).s);
            if (remote) add(" | mem remote %lu KB (largest %u), local %lu KB", (unsigned long)(h[2].a32 >> 16),
                            unsigned(h[2].a16), (unsigned long)(h[2].a32 & 0xFFFF));
            else add(" | mem local %lu KB", (unsigned long)(h[2].a32 & 0xFFFF));
        }
        if (flags & kHfRecording) add(" | PC using the audio");
    }
    static void describe(Line& out, const Rec& r) {
        const unsigned n = r.a16 > 1 ? r.a16 : 1;
        switch (r.code) {
            case kStart:
                out("LOCAL BOARD STARTED%s, firmware %lu.%lu.%lu: %s", r.a16 == 1 ? " as the EXTENDER drive" : "",
                           (unsigned long)(r.a32 >> 16), (unsigned long)(r.a32 >> 8 & 0xFF), (unsigned long)(r.a32 & 0xFF),
                           r.a16 == 1 ? "opened with BOOT" : resetText(r.a8));
                break;
            case kUsbSetUp: out("USB: the PC set the device up%s", times(n).s); break;
            case kUsbReleased: out("USB: the PC released the device (unplug, reset or re-attach)%s", times(n).s); break;
            case kUsbSuspended: out("USB: bus suspended (PC asleep, or its port stopped)%s", times(n).s); break;
            case kUsbResumed: out("USB: bus resumed%s", times(n).s); break;
            case kUsbStall: out("USB: stream open but the PC stopped collecting audio"); break;
            case kUsbStallCleared: out("USB: the PC collects audio again after %.1f s", r.a32 / 1000.0); break;
            case kUsbReattach:
                if (r.a8 == 1) out("USB: not set up by the PC for %lu s: re-attached", (unsigned long)(r.a32 / 1000));
                else out("USB: no audio collected for 5 s: re-attached");
                break;
            case kUsbStallRestart: out("USB: audio stalled again within 2 min: board restarting"); break;
            case kLinkUp:
                if (r.a32) out("link to remote %u back after %.1f s", r.a8, r.a32 / 1000.0);
                else out("linked to remote %u", r.a8);
                break;
            case kLinkLost: out("LINK LOST to remote %u", r.a8); break;
            case kRadioRestart: out("link still down: local radio restarted%s", r.a8 ? " (FAILED)" : ""); break;
            case kRemoteRestarted: out("REMOTE RESTARTED (it had run %s)", dur(r.a32).s); break;
            case kAdapterOk:
                out("remote adapter %04lx:%04lx streaming, %u ch %u-bit", (unsigned long)(r.a32 >> 16),
                           (unsigned long)(r.a32 & 0xFFFF), r.a8, r.a16);
                break;
            case kAdapterNone: out("remote adapter removed"); break;
            case kNoSignal: out("NO MICROPHONE SIGNAL (the adapter sends only silence)"); break;
            case kSignalBack: out("microphone signal back"); break;
            case kTempHigh:
                out("WARNING: %s chip temperature %.1f C", r.a8 ? "remote" : "local", int16_t(r.a16) / 10.0);
                break;
            case kTempOk:
                out("%s chip temperature back to %.1f C", r.a8 ? "remote" : "local", int16_t(r.a16) / 10.0);
                break;
            case kDriveOpened: out("EXTENDER drive opened (board restarts as the drive)"); break;
            case kFactoryReset: out("FACTORY RESET"); break;
            case kModeChange:
                out("MODE: %s (%s): the local board restarts as the USB %s", r.a8 ? "speaker" : "microphone",
                    modeReasonText(uint8_t(r.a16)), r.a8 ? "speaker" : "microphone");
                break;
            default: out("event %u (%u, %u, %lu)", r.code, r.a8, r.a16, (unsigned long)r.a32); break;
        }
    }
    struct Times { char s[24]; };
    static Times times(unsigned n) {
        Times t;
        if (n > 1) snprintf(t.s, sizeof(t.s), " (%u times)", n);
        else t.s[0] = 0;
        return t;
    }

    bool sectorSeq(uint32_t s, uint32_t& seq) {
        uint8_t r[kRecSize];
        if (!flash_->read(s * kSectorSize, r, kRecSize) || !valid(r) || r[0] != kSector) return false;
        Rec h;
        memcpy(&h, r, sizeof(h));
        seq = h.a32;
        return true;
    }

    bool put(Rec r) {
        if (!flash_) return false;
        if (slot_ >= kPerSector) {  // next sector: erase it, then mark it with the next sequence number
            const uint32_t next = (head_ + 1) % sectors_;
            if (!flash_->erase(next * kSectorSize, kSectorSize)) return false;
            Rec h = {};
            h.code = kSector;
            h.a32 = seq_ + 1;
            h.start = start_;
            seal(h);
            if (!flash_->write(next * kSectorSize, &h, kRecSize)) return false;
            head_ = next;
            seq_ = h.a32;
            slot_ = 1;
        }
        seal(r);
        const bool ok = flash_->write(head_ * kSectorSize + slot_ * kRecSize, &r, kRecSize);
        slot_++;  // a failed write still uses the slot (its bits may be partly cleared)
        if (ok) written_++;
        return ok;
    }
    static void seal(Rec& r) {
        uint8_t* d = reinterpret_cast<uint8_t*>(&r);
        r.crc = crc16(d, 14);
    }

    Flash* flash_ = nullptr;
    uint32_t sectors_ = 0, head_ = 0, seq_ = 0, slot_ = 0, written_ = 0;
    uint16_t start_ = 1, lastStart_ = 0;
};

}  // namespace evlog
