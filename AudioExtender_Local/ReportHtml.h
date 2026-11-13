#pragma once
// report.html on the EXTENDER drive: one self-contained page (no internet, no other
// files) with everything at a glance - status, the noise graph, a summary of the whole
// event log, the last 24 hourly health rows and the last 20 events. The full history
// stays in events.txt. A snapshot: no auto-refresh (in drive mode the board runs neither
// the radio nor the microphone).
//
// Plain C++ (tests/feature_tests.cpp renders one on the host).
#include "EventLog.h"
#include "NoiseSvg.h"

namespace report {

using Out = noisesvg::Out;
using noisesvg::escaped;

// Constant text of any length (printf() has a 256-byte buffer).
inline void put(Out& out, const char* text) { out.write(out.ctx, text); }

inline void begin(Out& out, const char* title) {
    put(out, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    escaped(out, title);
    put(out, "</title>\n<style>\n"
               "body{font-family:Helvetica,Arial,sans-serif;margin:24px auto;max-width:1000px;padding:0 16px;color:#1d2433;"
               "background:#f6f7f9;line-height:1.4}\n"
               "h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px;border-bottom:1px solid #d5d9e0;"
               "padding-bottom:4px}\n"
               ".sub{color:#5b6475;font-size:13px}\n"
               "pre{background:#fff;border:1px solid #d5d9e0;border-radius:6px;padding:12px;overflow-x:auto;font-size:12.5px;"
               "line-height:1.35}\n"
               "table{border-collapse:collapse;width:100%;background:#fff;font-size:12.5px}\n"
               "th,td{border:1px solid #d5d9e0;padding:4px 7px;text-align:left;vertical-align:top}\n"
               "th{background:#eceef2;font-weight:600}td.n{text-align:right;font-variant-numeric:tabular-nums}\n"
               ".bad{color:#b3261e;font-weight:600}.none{color:#5b6475;font-style:italic}\n"
               "svg{max-width:100%;height:auto;border-radius:6px}\n"
               "</style></head><body>\n<h1>");
    escaped(out, title);
    out.printf("</h1>\n");
}

inline void end(Out& out) { out.printf("</body></html>\n"); }

inline void heading(Out& out, const char* text) {
    out.printf("<h2>");
    escaped(out, text);
    out.printf("</h2>\n");
}

inline void note(Out& out, const char* text, const char* cls = "sub") {
    out.printf("<p class=\"%s\">", cls);
    escaped(out, text);
    out.printf("</p>\n");
}

namespace detail {
inline void when(char* b, size_t n, uint16_t start, uint32_t t) {
    snprintf(b, n, "start %u, %s", start, evlog::Log::dur(t).s);
}
// "3:12:05 ago" in this start, or "in start 11" for an earlier one.
inline void ago(char* b, size_t n, uint16_t start, uint32_t t, uint16_t nowStart, uint32_t nowS) {
    if (start == nowStart && nowS >= t) snprintf(b, n, "%s ago", evlog::Log::dur(nowS - t).s);
    else snprintf(b, n, "in start %u (%s after it began)", start, evlog::Log::dur(t).s);
}
}  // namespace detail

// The log sections: summary of the whole log, hourly health table, last events.
inline void logSections(Out& out, evlog::Log& log, uint32_t nowS) {
    using evlog::Log;
    if (!log.ok()) {
        heading(out, "Event log");
        note(out, "No event log on this board: flash the full image once to create its flash area.", "none");
        return;
    }
    struct Kind { const char* name; uint8_t code; uint32_t n; uint16_t start; uint32_t t; };
    Kind kinds[] = {
        {"Local board starts", evlog::kStart, 0, 0, 0},
        {"Link drops", evlog::kLinkLost, 0, 0, 0},
        {"Remote restarts", evlog::kRemoteRestarted, 0, 0, 0},
        {"Local radio restarts", evlog::kRadioRestart, 0, 0, 0},
        {"USB re-attaches", evlog::kUsbReattach, 0, 0, 0},
        {"USB audio stalls", evlog::kUsbStall, 0, 0, 0},
        {"USB released by the PC", evlog::kUsbReleased, 0, 0, 0},
        {"Mode changes (microphone / speaker)", evlog::kModeChange, 0, 0, 0},
        {"Warnings and errors (notes)", evlog::kText, 0, 0, 0},
    };
    constexpr int kKinds = sizeof(kinds) / sizeof(kinds[0]);
    // The last 24 hourly snapshots and the last 20 events (rings).
    struct Snap { evlog::Rec h[3]; uint8_t mask; };
    static Snap snaps[24];
    int snapN = 0, snapNext = 0;
    struct Ev { uint16_t start; uint32_t t; char text[200]; };
    static Ev evs[20];
    int evN = 0, evNext = 0;
    uint32_t total = 0;
    uint16_t firstStart = 0;
    uint32_t firstT = 0;
    log.lines([&](const Log::LineInfo& i, const char* text) {
        if (!total) { firstStart = i.start; firstT = i.t; }
        total++;
        if (i.health) {
            if (i.hourly) {
                Snap& s = snaps[snapNext];
                for (int k = 0; k < 3; ++k) s.h[k] = i.snap[k];
                s.mask = i.mask;
                snapNext = (snapNext + 1) % 24;
                if (snapN < 24) snapN++;
            }
            return;
        }
        for (Kind& k : kinds) {
            if (k.code != i.code) continue;
            if (k.code == evlog::kText && !strstr(text, "WARNING") && !strstr(text, "ERROR")) continue;
            k.n++;
            k.start = i.start;
            k.t = i.t;
        }
        Ev& e = evs[evNext];
        e.start = i.start;
        e.t = i.t;
        snprintf(e.text, sizeof(e.text), "%s", text);
        evNext = (evNext + 1) % 20;
        if (evN < 20) evN++;
    });
    heading(out, "Event log summary");
    char b[96], c[96];
    if (!total) {
        note(out, "No events recorded yet.", "none");
    } else {
        detail::when(b, sizeof(b), firstStart, firstT);
        out.printf("<p class=\"sub\">%lu lines in the log, the oldest from %s. Times are since each start of the local "
                   "board (it has no clock).</p>\n", (unsigned long)total, b);
    }
    out.printf("<table><tr><th>What</th><th>Count</th><th>Last</th></tr>\n");
    for (int k = 0; k < kKinds; ++k) {
        const bool alarm = kinds[k].n && kinds[k].code != evlog::kStart && kinds[k].code != evlog::kUsbReleased;
        if (kinds[k].n) detail::ago(b, sizeof(b), kinds[k].start, kinds[k].t, log.start(), nowS);
        else snprintf(b, sizeof(b), "-");
        out.printf("<tr><td>%s</td><td class=\"n%s\">%lu</td><td>%s</td></tr>\n", kinds[k].name, alarm ? " bad" : "",
                   (unsigned long)kinds[k].n, b);
    }
    out.printf("</table>\n");

    heading(out, "Hourly health (the last 24 hours on record)");
    if (!snapN) {
        note(out, "No hourly health rows yet (the first is written an hour after the local board starts).", "none");
    } else {
        put(out, "<table><tr><th>When</th><th>Link</th><th>RSSI dBm<br>local / remote</th><th>Sends failed</th>"
                 "<th>Temp C<br>remote / local</th><th>Audio lost</th><th>Late</th><th>Underruns</th>"
                 "<th>Free KB<br>remote / local</th><th>PC using</th></tr>\n");
        const int first = (snapNext - snapN + 24) % 24;
        for (int i = 0; i < snapN; ++i) {
            const Snap& s = snaps[(first + i) % 24];
            const evlog::Rec& r = (s.mask & 1) ? s.h[0] : (s.mask & 2) ? s.h[1] : s.h[2];
            const uint8_t flags = (s.mask & 1) ? uint8_t(s.h[0].a16 >> 8) : 0;
            const bool remote = flags & evlog::kHfRemote;
            detail::when(b, sizeof(b), r.start, r.t);
            out.printf("<tr><td>%s</td><td%s>%s</td>", b, (flags & evlog::kHfLinked) ? "" : " class=\"bad\"",
                       (flags & evlog::kHfLinked) ? "linked" : "NOT linked");
            if (s.mask & 1) {
                if (remote) out.printf("<td class=\"n\">%d / %d</td><td class=\"n\">%s</td>", int(int8_t(s.h[0].a8)),
                                       int(int8_t(s.h[0].a16 & 0xFF)), Log::count16(s.h[0].a32 & 0xFFFF).s);
                else out.printf("<td class=\"n\">%d / -</td><td class=\"n\">-</td>", int(int8_t(s.h[0].a8)));
            } else {
                out.printf("<td>-</td><td>-</td>");
            }
            if (s.mask & 2) {
                const int16_t rt = int16_t(s.h[1].a16), lt = int16_t(s.h[1].a32 >> 16);
                if (remote && rt != evlog::kTempNone) snprintf(b, sizeof(b), "%.1f", rt / 10.0);
                else snprintf(b, sizeof(b), "-");
                if (lt != evlog::kTempNone) snprintf(c, sizeof(c), "%.1f", lt / 10.0);
                else snprintf(c, sizeof(c), "-");
                out.printf("<td class=\"n\">%s / %s</td><td class=\"n\">%s</td><td class=\"n\">%s</td>", b, c,
                           Log::count16(s.h[1].a32 & 0xFFFF).s, Log::count8(s.h[1].a8).s);
            } else {
                out.printf("<td>-</td><td>-</td><td>-</td>");
            }
            if (s.mask & 4) {
                out.printf("<td class=\"n\">%s</td>", Log::count8(s.h[2].a8).s);
                if (remote) out.printf("<td class=\"n\">%lu / %lu</td>", (unsigned long)(s.h[2].a32 >> 16),
                                       (unsigned long)(s.h[2].a32 & 0xFFFF));
                else out.printf("<td class=\"n\">- / %lu</td>", (unsigned long)(s.h[2].a32 & 0xFFFF));
            } else {
                out.printf("<td>-</td><td>-</td>");
            }
            out.printf("<td>%s</td></tr>\n", (flags & evlog::kHfRecording) ? "yes" : "no");
        }
        put(out, "</table>\n<p class=\"sub\">Audio lost / late / underruns count since the row before. "
                 "255+ / 65535+: at least that many.</p>\n");
    }

    heading(out, "The last 20 events");
    if (!evN) {
        note(out, "None.", "none");
    } else {
        out.printf("<table><tr><th>When</th><th>Event</th></tr>\n");
        const int first = (evNext - evN + 20) % 20;
        for (int i = 0; i < evN; ++i) {
            const Ev& e = evs[(first + i) % 20];
            detail::when(b, sizeof(b), e.start, e.t);
            const bool bad = strstr(e.text, "LOST") || strstr(e.text, "RESTARTED") || strstr(e.text, "WARNING") ||
                             strstr(e.text, "ERROR") || strstr(e.text, "NO MICROPHONE");
            out.printf("<tr><td>%s</td><td%s>", b, bad ? " class=\"bad\"" : "");
            escaped(out, e.text);
            out.printf("</td></tr>\n");
        }
        out.printf("</table>\n");
    }
    note(out, "Full history (every event and health line): events.txt on this drive.");
}

}  // namespace report
