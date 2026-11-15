// Host test for EventLog.h: a simulated NOR flash (writes only clear bits, erase sets
// 0xFF), restarts, wrap-around, and power cuts in the middle of writes and erases.
#include "EventLog.h"
#include "ReportHtml.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct SimFlash : evlog::Flash {
    std::vector<uint8_t> mem;
    long budget = -1;       // bytes that may still be written/erased before the "power cut"
    unsigned erases = 0;
    explicit SimFlash(uint32_t size) : mem(size, 0xFF) {}
    uint32_t size() override { return uint32_t(mem.size()); }
    bool read(uint32_t o, void* b, uint32_t n) override {
        if (o + n > mem.size()) return false;
        memcpy(b, &mem[o], n);
        return true;
    }
    bool write(uint32_t o, const void* b, uint32_t n) override {
        if (o + n > mem.size()) return false;
        const uint8_t* s = static_cast<const uint8_t*>(b);
        for (uint32_t i = 0; i < n; ++i) {
            if (budget == 0) return false;  // power cut: the rest is never written
            if (budget > 0) budget--;
            mem[o + i] &= s[i];             // NOR: can only clear bits
        }
        return true;
    }
    bool erase(uint32_t o, uint32_t n) override {
        assert(o % evlog::kSectorSize == 0 && n % evlog::kSectorSize == 0);
        erases++;
        for (uint32_t i = 0; i < n; ++i) {
            if (budget == 0) {                // power cut mid-erase: the rest is left as it was,
                for (uint32_t k = i; k < n; k += 7) mem[o + k] ^= 0x5A;  // scrambled a bit
                return false;
            }
            if (budget > 0) budget--;
            mem[o + i] = 0xFF;
        }
        return true;
    }
};

static std::string render(evlog::Log& log, uint32_t now = 100) {
    std::string t;
    evlog::Out out{[](void* c, const char* s) { static_cast<std::string*>(c)->append(s); }, &t};
    log.render(out, now);
    return t;
}
static int count(const std::string& s, const char* what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) n++;
    return n;
}
static size_t records(evlog::Log& log) {
    size_t n = 0;
    log.forEachRecord([&](const evlog::Rec& r) { if (r.code != evlog::kSector && r.code != evlog::kData) n++; });
    return n;
}

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    SimFlash f(64 * 1024);
    {
        evlog::Log log;
        assert(log.begin(&f) && log.start() == 1);
        assert(count(render(log), "No events recorded yet") == 1);
        log.add(evlog::kStart, 0, 1, 0, 0x010302);
        log.add(evlog::kUsbSetUp, 1);
        log.add(evlog::kLinkUp, 3, 1, 0, 0);
        log.add(evlog::kUsbSuspended, 7200, 0, 3);
        {   // health snapshot just before the link loss (all three records)
            const uint8_t fl = evlog::kHfLinked | evlog::kHfRemote | evlog::kHfRecording | evlog::kHfEvent;
            log.add(evlog::kHealthRadio, 90061, uint8_t(int8_t(-71)), uint16_t(uint8_t(int8_t(-69))) | uint16_t(fl) << 8,
                    34u << 16 | 120u);
            log.add(evlog::kHealthTemp, 90061, 1, uint16_t(int16_t(682)), uint32_t(uint16_t(int16_t(410))) << 16 | 7u);
            log.add(evlog::kHealthMem, 90061, 2, 135, 176u << 16 | 103u);
        }
        log.add(evlog::kLinkLost, 90061, 1);
        log.add(evlog::kLinkUp, 90064, 1, 0, 2900);
        log.addText(90100, "restarted itself: its sends had stopped getting through (stuck transmitter)", 1);
        log.add(evlog::kTempHigh, 90200, 1, uint16_t(int16_t(781)));
        log.add(evlog::kAdapterOk, 90300, 1, 16, 0x12d10010);
        const std::string t = render(log, 90400);
        if (verbose) { fputs(t.c_str(), stdout); fflush(stdout); }
        assert(count(t, "LOCAL BOARD STARTED, firmware 1.3.2: power on") == 1);
        assert(count(t, "USB: bus suspended (PC asleep, or its port stopped) (3 times)") == 1);
        assert(count(t, "1d 01:01:01  LINK LOST to remote 1") == 1);
        assert(count(t, "link to remote 1 back after 2.9 s") == 1);
        assert(count(t, "remote: restarted itself: its sends had stopped getting through (stuck transmitter)\r\n") == 1);
        assert(count(t, "WARNING: remote chip temperature 78.1 C") == 1);
        assert(count(t, "remote adapter 12d1:0010 streaming, 1 ch 16-bit") == 1);
        assert(count(t, "Written during start 1, 1d 01:06:40 after it began.") == 1);
        assert(count(t, "1d 01:01:01  health: linked, RSSI -71 dBm (remote hears -69), remote queue 34, sends failed 120"
                        " | temp remote 68.2 C, local 41.0 C | audio lost 7, late 1, underruns 2"
                        " | mem remote 176 KB (largest 135), local 103 KB | PC using the audio\r\n") == 1);
        assert(t.find("health:") < t.find("LINK LOST"));
        assert(count(t, "began.\r\n\r\nstart 1\r\n") == 1);  // one blank line, then the first start's heading
        assert(count(t, "start 1\r\n") == 1);                     // the start number once, as a heading
        // An hourly snapshot while unlinked, and a snapshot cut short (only its first record).
        log.add(evlog::kHealthRadio, 93600, uint8_t(int8_t(-90)), 0, 0);
        log.add(evlog::kHealthTemp, 93600, 0, uint16_t(evlog::kTempNone), uint32_t(uint16_t(int16_t(395))) << 16);
        log.add(evlog::kHealthMem, 93600, 0, 0, 101u);
        log.add(evlog::kHealthRadio, 97200, uint8_t(int8_t(-60)), uint16_t(evlog::kHfLinked) << 8, 0);
        log.add(evlog::kLinkLost, 97201, 1);
        const std::string t2 = render(log, 97300);
        if (verbose) { fputs(t2.c_str(), stdout); fflush(stdout); }
        assert(count(t2, "1d 02:00:00  health (hourly): NOT linked, RSSI -90 dBm | temp local 39.5 C | audio lost 0, late 0,"
                         " underruns 0 | mem local 101 KB\r\n") == 1);
        assert(count(t2, "1d 03:00:00  health (hourly): linked, RSSI -60 dBm\r\n") == 1);
        // Capped counts read "255+" / "65535+"; a 159-character remote line is kept whole.
        const uint8_t fl2 = evlog::kHfLinked | evlog::kHfRemote | evlog::kHfEvent;
        log.add(evlog::kHealthRadio, 97300, uint8_t(int8_t(-60)), uint16_t(uint8_t(int8_t(-61))) | uint16_t(fl2) << 8, 65535u);
        log.add(evlog::kHealthTemp, 97300, 255, uint16_t(int16_t(700)), uint32_t(uint16_t(int16_t(500))) << 16 | 65535u);
        log.add(evlog::kHealthMem, 97300, 255, 135, 176u << 16 | 103u);
        std::string longLine = "link-loss details (this drop): last packet from the local board 2010 ms before, last acknowledged send 788 ms before";
        while (longLine.size() < evlog::kMaxText) longLine += "x";  // 255: a remote line joined from its pieces
        log.addText(97301, longLine.c_str(), 1);
        log.add(evlog::kModeChange, 97302, 1, 5);
        const std::string t3 = render(log, 97400);
        if (verbose) { fputs(t3.c_str(), stdout); fflush(stdout); }
        assert(count(t3, "sends failed 65535+") == 1 && count(t3, "audio lost 65535+, late 255+, underruns 255+") == 1);
        assert(count(t3, ("remote: " + longLine + "\r\n").c_str()) == 1);
        assert(count(t3, "MODE: speaker (no microphone signal at start-up)") == 1);
        // 1.4.3: kHealthMem2 - underruns and late past 255 (hourly snapshot), and their caps.
        {
            const uint8_t fl3 = evlog::kHfLinked | evlog::kHfRemote | evlog::kHfRecording;
            log.add(evlog::kHealthRadio, 100800, uint8_t(int8_t(-61)), uint16_t(uint8_t(int8_t(-56))) | uint16_t(fl3) << 8, 1u << 16 | 0u);
            log.add(evlog::kHealthTemp, 100800, 255, uint16_t(evlog::kTempNone), uint32_t(uint16_t(evlog::kTempNone)) << 16 | 2049u);
            log.add(evlog::kHealthMem2, 100800, 115, 1000, 300u << 20 | 157u << 10 | 64u);
            log.add(evlog::kHealthRadio, 100900, uint8_t(int8_t(-61)), uint16_t(uint8_t(int8_t(-56))) | uint16_t(fl2) << 8, 0u);
            log.add(evlog::kHealthTemp, 100900, 255, uint16_t(evlog::kTempNone), uint32_t(uint16_t(evlog::kTempNone)) << 16);
            log.add(evlog::kHealthMem2, 100900, 255, 65535, 4095u << 20 | 1023u << 10 | 1023u);
            const std::string t5 = render(log, 101000);
            if (verbose) { fputs(t5.c_str(), stdout); fflush(stdout); }
            assert(count(t5, "1d 04:00:00  health (hourly): linked, RSSI -61 dBm (remote hears -56), remote queue 1, sends failed 0"
                             " | audio lost 2049, late 300, underruns 1000"
                             " | mem remote 157 KB (largest 115), local 64 KB | PC using the audio\r\n") == 1);
            assert(count(t5, "late 4095+, underruns 65535+ | mem remote 1023 KB (largest 255), local 1023 KB") == 1);
            // the 1.4.2 record (kHealthMem) still reads as before
            assert(count(t5, "audio lost 7, late 1, underruns 2 | mem remote 176 KB (largest 135), local 103 KB") == 1);
        }
        // Shortened to fit a full drive: the oldest lines go, with a note.
        const uint32_t lines = log.lineCount();
        std::string t4;
        evlog::Out o4{[](void* c, const char* x) { *static_cast<std::string*>(c) += x; }, &t4};
        log.render(o4, 101000, lines - 3);  // the last three: the mode change and two 1.4.3 health lines
        assert(count(t4, "the oldest") == 1 && count(t4, "\nstart 1\r\n") == 1 && count(t4, "MODE: speaker") == 1);
        // lines(): health lines are flagged, hourly ones too.
        int healthN = 0, hourlyN = 0;
        log.lines([&](const evlog::Log::LineInfo& i, const char*) { healthN += i.health; hourlyN += i.hourly; });
        assert(healthN == 6 && hourlyN == 3);
        // report.html from the same log: summary counts, hourly rows, last events.
        std::string h;
        report::Out ro{[](void* c, const char* x) { *static_cast<std::string*>(c) += x; }, &h};
        report::begin(ro, "Extender Mic 1");
        report::logSections(ro, log, 97400);
        report::end(ro);
        if (verbose) { fputs(h.c_str(), stdout); fflush(stdout); }
        assert(count(h, "<td>Link drops</td><td class=\"n bad\">2</td><td>0:03:19 ago</td>") == 1);
        assert(count(h, "<td>Mode changes (microphone / speaker)</td><td class=\"n bad\">1</td>") == 1);
        assert(count(h, "<td>start 1, 1d 02:00:00</td><td class=\"bad\">NOT linked</td>") == 1);  // hourly rows only
        assert(count(h, "<td>start 1, 1d 01:01:01</td><td>linked") == 0);                              // (not event ones)
        assert(count(h, "LINK LOST to remote 1") == 2 && count(h, "</html>") == 1);
        assert(count(h, "<td class=\"n\">2049</td><td class=\"n\">300</td><td class=\"n\">1000</td><td class=\"n\">157 / 64</td>") == 1);
    }
    {   // Restart: next start number, old events kept, new ones after them.
        evlog::Log log;
        assert(log.begin(&f) && log.start() == 2);
        log.add(evlog::kStart, 0, 6, 0, 0x010302);
        const std::string t = render(log);
        assert(count(t, "LOCAL BOARD STARTED") == 2 && count(t, "watchdog") == 1);
        assert(t.rfind("\nstart 1\r\n") < t.find("\nstart 2\r\n"));
        assert(count(t, "\nstart 2\r\n    0:00:00  LOCAL BOARD STARTED") == 1);  // heading, then indented lines
    }
    {   // Many starts and events: wraps round the 16 sectors, keeps ~15 sectors of the newest.
        uint16_t lastStart = 0;
        for (int boot = 0; boot < 300; ++boot) {
            evlog::Log log;
            assert(log.begin(&f));
            assert(log.start() == uint16_t(lastStart ? lastStart + 1 : 3));
            lastStart = log.start();
            log.add(evlog::kStart, 0, 1, 0, 0x010302);
            for (int i = 0; i < 40; ++i) log.add(evlog::kUsbSetUp, uint32_t(i));
            if (boot % 10 == 0) log.addText(50, "a note long enough to need several data records to hold all of it");
        }
        evlog::Log log;
        assert(log.begin(&f));
        const size_t n = records(log);
        assert(n >= 15 * 250 && n <= 16 * 255);  // 15-16 sectors' worth
        uint32_t prevStart = 0, prevT = 0;
        bool ordered = true;
        log.forEachRecord([&](const evlog::Rec& r) {
            if (r.code == evlog::kSector || r.code == evlog::kData) return;
            if (r.start < prevStart || (r.start == prevStart && r.t < prevT)) ordered = false;
            prevStart = r.start;
            prevT = r.t;
        });
        assert(ordered);  // oldest first across the wrap
        const std::string t = render(log);
        assert(count(t, "a note long enough to need several data records to hold all of it\r\n") >= 1);
        assert(count(t, "[...]") <= 1);  // only a note cut by the wrap may be incomplete
        printf("event log: %zu events kept in 64 KB, %u sector erases for %d starts\n", n, f.erases, 300);
    }
    {   // Power cut at every byte of a record write, and in the middle of sector erases.
        for (long cut = 0; cut <= 64; ++cut) {
            SimFlash g(16 * 1024);
            {
                evlog::Log log;
                assert(log.begin(&g));
                for (int i = 0; i < 300; ++i) log.add(evlog::kLinkLost, uint32_t(i), 1);  // crosses sectors
                g.budget = cut;
                for (int i = 0; i < 300; ++i) log.add(evlog::kLinkUp, uint32_t(1000 + i), 1, 0, 5);
                g.budget = -1;
            }
            evlog::Log log;
            assert(log.begin(&g));
            const size_t before = records(log);
            log.add(evlog::kStart, 0, 1);
            for (int i = 0; i < 600; ++i) log.add(evlog::kUsbResumed, uint32_t(i));  // keeps working after the cut
            assert(records(log) > 0 && before > 0);
            const std::string t = render(log);
            assert(count(t, "LOCAL BOARD STARTED") == 1 && count(t, "USB: bus resumed") > 100);
            // Every record read back is one that was written (checksums hold, nothing invented).
            log.forEachRecord([&](const evlog::Rec& r) {
                assert(r.code == evlog::kSector || r.code == evlog::kLinkLost || r.code == evlog::kLinkUp ||
                       r.code == evlog::kStart || r.code == evlog::kUsbResumed);
            });
        }
    }
    {   // No flash area (update image without the new partition table): harmless.
        evlog::Log log;
        assert(!log.begin(nullptr));
        assert(!log.add(evlog::kStart, 0));
        assert(count(render(log), "flash the full image once") == 1);
        SimFlash tiny(4096);
        assert(!log.begin(&tiny));
    }
    {   // Garbage flash (never erased, random contents): treated as empty, then works.
        SimFlash g(16 * 1024);
        unsigned seed = 7;
        for (auto& b : g.mem) b = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
        evlog::Log log;
        assert(log.begin(&g));
        log.add(evlog::kStart, 0, 1);
        assert(records(log) == 1);
    }
    printf("event log test ok\n");
    return 0;
}
