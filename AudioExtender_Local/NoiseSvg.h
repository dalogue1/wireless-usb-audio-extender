#pragma once
// noise.svg: the remote noise filter's learned background noise as one picture, written
// to the EXTENDER drive (and embedded in report.html). Plain SVG text, no image library.
//
// Style after a classic noise-floor graph: dark navy background, faint horizontal grid at
// 0 / -30 / -60 / -90 / -120 dB, x ticks every 6 kHz, one thin line, wide and short. Ours
// adds unit labels, a vertical line at the high-pass cut-off, light shading over 1-10 kHz
// (the bird band) and a header with the settings that produced it, so a picture shared on
// its own still says what it shows.
//
// Plain C++ (tests/feature_tests.cpp renders it on the host).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace noisesvg {

constexpr int kBins = 257;          // NoiseSignature.h: 0 .. 24 kHz in ~93.75 Hz steps
constexpr int16_t kNone = -32768;   // no value for a bin

struct Out {
    void (*write)(void* ctx, const char* text);
    void* ctx;
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        write(ctx, b);
    }
};

struct Info {
    const int16_t* deciDb = nullptr;  // kBins values, 0.1 dB re full scale (kNone: missing)
    const char* title = "";           // e.g. "Extender Mic 1 - background noise learned by the noise filter"
    const char* line1 = "";           // e.g. "learned for 1d 14:02:11 | adapter: UGREEN ..."
    const char* line2 = "";           // e.g. "noise_filter on (12 dB), noise_adapt normal, highpass on 200 Hz, mic_level default (adapter at 0.0 dB)"
    bool highpassOn = false;
    uint16_t highpassHz = 0;
};

// Text inside SVG/HTML: & < > " escaped.
inline void escaped(Out& out, const char* s) {
    char b[96];
    size_t o = 0;
    auto flush = [&]() { b[o] = 0; if (o) out.write(out.ctx, b); o = 0; };
    for (; *s; ++s) {
        const char* r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : nullptr;
        if (o + 7 >= sizeof(b)) flush();
        if (r) { strcpy(b + o, r); o += strlen(r); }
        else b[o++] = *s;
    }
    flush();
}

// The whole SVG document (also valid inline in HTML).
inline void render(Out& out, const Info& in) {
    // Layout: 960 x 284; plot 880 x 180 (about 4.6 : 1) under a 3-line header.
    constexpr int W = 960, H = 284, L = 52, T = 62, PW = 880, PH = 180;
    constexpr double kTopDb = 0, kBottomDb = -120, kMaxHz = 24000;
    auto xOf = [&](double hz) { return L + PW * hz / kMaxHz; };
    auto yOf = [&](double db) {
        if (db > kTopDb) db = kTopDb;
        if (db < kBottomDb) db = kBottomDb;
        return T + PH * (kTopDb - db) / (kTopDb - kBottomDb);
    };
    out.printf("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
               "font-family=\"Helvetica, Arial, sans-serif\">\n", W, H, W, H);
    out.printf("<rect width=\"%d\" height=\"%d\" fill=\"#0f1b33\"/>\n", W, H);
    // Header
    out.printf("<text x=\"%d\" y=\"20\" font-size=\"15\" fill=\"#e8eef8\">", L);
    escaped(out, in.title);
    out.printf("</text>\n<text x=\"%d\" y=\"38\" font-size=\"11.5\" fill=\"#a9b8d0\">", L);
    escaped(out, in.line1);
    out.printf("</text>\n<text x=\"%d\" y=\"53\" font-size=\"11.5\" fill=\"#a9b8d0\">", L);
    escaped(out, in.line2);
    out.printf("</text>\n");
    // Bird band 1-10 kHz
    out.printf("<rect x=\"%.1f\" y=\"%d\" width=\"%.1f\" height=\"%d\" fill=\"#ffffff\" fill-opacity=\"0.06\"/>\n",
               xOf(1000), T, xOf(10000) - xOf(1000), PH);
    out.printf("<text x=\"%.1f\" y=\"%d\" font-size=\"10\" fill=\"#7f8ea8\">birds 1-10 kHz</text>\n", xOf(1000) + 4, T + 12);
    // Grid and y labels
    for (int db = 0; db >= -120; db -= 30) {
        const double y = yOf(db);
        out.printf("<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" stroke=\"#ffffff\" stroke-opacity=\"0.12\"/>\n", L, y, L + PW, y);
        out.printf("<text x=\"%d\" y=\"%.1f\" font-size=\"10.5\" fill=\"#7f8ea8\" text-anchor=\"end\">%d</text>\n", L - 6, y + 3.5, db);
    }
    out.printf("<text x=\"%d\" y=\"%d\" font-size=\"10.5\" fill=\"#7f8ea8\" text-anchor=\"end\">dB</text>\n", L - 6, T - 6);
    // x ticks
    for (int khz = 0; khz <= 24; khz += 6) {
        const double x = xOf(khz * 1000.0);
        out.printf("<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#ffffff\" stroke-opacity=\"0.25\"/>\n", x, T + PH, x, T + PH + 4);
        out.printf("<text x=\"%.1f\" y=\"%d\" font-size=\"10.5\" fill=\"#7f8ea8\" text-anchor=\"middle\">%d</text>\n", x, T + PH + 16, khz);
    }
    out.printf("<text x=\"%d\" y=\"%d\" font-size=\"10.5\" fill=\"#7f8ea8\" text-anchor=\"end\">kHz</text>\n", L + PW, T + PH + 28);
    // High-pass cut-off
    if (in.highpassOn && in.highpassHz) {
        const double x = xOf(in.highpassHz);
        out.printf("<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#f2b84b\" stroke-opacity=\"0.8\" "
                   "stroke-dasharray=\"4 3\"/>\n", x, T, x, T + PH);
        out.printf("<text x=\"%.1f\" y=\"%d\" font-size=\"10\" fill=\"#f2b84b\">high-pass %u Hz</text>\n", x + 4, T + PH - 6,
                   unsigned(in.highpassHz));
    }
    // The signature: one polyline (a gap where a bin has no value).
    bool open = false;
    int points = 0;
    for (int k = 0; in.deciDb && k < kBins; ++k) {
        const int16_t v = in.deciDb[k];
        if (v == kNone) {
            if (open) out.printf("\"/>\n");
            open = false;
            continue;
        }
        if (!open) {
            out.printf("<polyline fill=\"none\" stroke=\"#5cc8ff\" stroke-width=\"1.4\" stroke-linejoin=\"round\" points=\"");
            open = true;
        }
        out.printf("%.1f,%.1f ", xOf(k * kMaxHz / (kBins - 1)), yOf(v / 10.0));
        points++;
    }
    if (open) out.printf("\"/>\n");
    if (!points)
        out.printf("<text x=\"%d\" y=\"%d\" font-size=\"13\" fill=\"#e8eef8\" text-anchor=\"middle\">no data</text>\n",
                   L + PW / 2, T + PH / 2);
    out.printf("</svg>\n");
}

}  // namespace noisesvg
