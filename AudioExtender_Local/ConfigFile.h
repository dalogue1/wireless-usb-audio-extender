#pragma once
// config.txt on the local board's EXTENDER drive: reading and writing.
// Plain C++ (tests/feature_tests.cpp checks it on the host).
//
//   mic = 1              1..9                            (both boards)
//   channel = 11         Wi-Fi channel 1..11             (both boards)
//   feedback = voice     voice or off                    (local)
//   noise_filter = on    on, strong or off               (remote)
//   noise_adapt = normal normal, slow or fast            (remote)
//   tx_power = max       max, or 2..20 (dBm)             (both boards)
//   cpu_mhz = 240        240 or 160                      (both boards)
//   highpass = on        on, off                          (remote)
//   highpass_hz = 200    50..1000 (Hz)                    (remote)
//   (temperature = ...   up to 1.4.3; accepted and ignored since 1.4.4: both chips are read
//                        at start and when the drive opens, never while audio streams)
//   mic_level = 100      0..100 (% of the adapter's level range; AtomS3U: of 0..+30 dB gain),
//                        or adapter (leave the adapter's own level); "default" = 100 (remote)
//   distance = normal    near, normal or far: radio rate of the link (both boards)
//   mode = auto          auto, microphone or speaker (both boards)
//   remote = any         any, or the remote's MAC address: lock the pair (both boards)
//
// '#' or ';' starts a comment; keys and values are case-insensitive; "=" or ":".
// A line that cannot be understood keeps the previous value and is reported.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace cfg {

constexpr uint8_t kLevelDefault = 255;  // = wlink::kLevelDefault
inline void levelText(uint8_t v, char* out, size_t cap) {
    if (v > 100) snprintf(out, cap, "adapter");
    else snprintf(out, cap, "%u", unsigned(v));
}

struct Settings {
    uint8_t mic = 1;        // 1..9
    uint8_t channel = 11;   // 1..11
    uint8_t feedback = 0;   // 0 voice, 1 off               (announcer::Mode order)
    uint8_t filter = 0;     // 0 on, 1 strong, 2 off        (wlink::FilterMode order)
    uint8_t adapt = 0;      // 0 normal, 1 slow, 2 fast     (wlink::AdaptSpeed order)
    uint8_t txPower = 20;   // dBm, 2..20 (20 = max)
    uint8_t cpuMhz = 240;   // 240 or 160
    uint8_t highpass = 1;   // 1 on, 0 off
    uint16_t highpassHz = 200;  // 50..1000
    uint8_t micLevel = 100;  // 0..100 %, or kLevelDefault (leave the adapter's own level)
    uint8_t distance = 0;   // 0 normal, 1 near, 2 far            (wlink::Distance order)
    uint8_t mode = 0;       // 0 auto, 1 microphone, 2 speaker    (wlink::ModeSetting order)
    bool lock = false;      // remote = <MAC>: link only to that remote, and lock it to this local board
    uint8_t remoteMac[6] = {0, 0, 0, 0, 0, 0};
    bool operator==(const Settings& o) const {
        return mic == o.mic && channel == o.channel && feedback == o.feedback && filter == o.filter && adapt == o.adapt &&
               txPower == o.txPower && cpuMhz == o.cpuMhz && highpass == o.highpass && highpassHz == o.highpassHz &&
               micLevel == o.micLevel && distance == o.distance && mode == o.mode &&
               lock == o.lock && memcmp(remoteMac, o.remoteMac, 6) == 0;
    }
};

struct Result {
    Settings settings;
    int errors = 0;
    char firstError[96] = "";
};

namespace detail {
inline void lower(char* s) { for (; *s; ++s) *s = char(tolower((unsigned char)*s)); }
inline char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
inline int pick(const char* v, const char* const* names, int n) {
    for (int i = 0; i < n; ++i) if (strcmp(v, names[i]) == 0) return i;
    return -1;
}
}  // namespace detail

constexpr uint8_t kFeedbackVoice = 0, kFeedbackOff = 1;
inline const char* feedbackName(uint8_t v) { static const char* const n[] = {"voice", "off"}; return n[v < 2 ? v : 0]; }
inline const char* filterName(uint8_t v) { static const char* const n[] = {"on", "strong", "off"}; return n[v < 3 ? v : 0]; }
// "max" or the number; a static buffer, fine for logging and file writing.
inline const char* txPowerText(uint8_t dbm) {
    static char b[8];
    if (dbm >= 20) return "max";
    snprintf(b, sizeof(b), "%u", dbm);
    return b;
}
inline const char* adaptName(uint8_t v) { static const char* const n[] = {"normal", "slow", "fast"}; return n[v < 3 ? v : 0]; }
inline const char* distanceName(uint8_t v) { static const char* const n[] = {"normal", "near", "far"}; return n[v < 3 ? v : 0]; }
inline const char* modeName(uint8_t v) { static const char* const n[] = {"auto", "microphone", "speaker"}; return n[v < 3 ? v : 0]; }

// "44:BD:8D:76:CA:44" (also with '-' between the bytes, any case) -> 6 bytes. All zeros and
// broadcast (FF:FF:...) are refused: neither is a board.
inline bool parseMac(const char* s, uint8_t out[6]) {
    unsigned v[6];
    char sep[5], tail;
    const int n = sscanf(s, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x%c", &v[0], &sep[0], &v[1], &sep[1], &v[2], &sep[2], &v[3],
                         &sep[3], &v[4], &sep[4], &v[5], &tail);
    if (n != 11) return false;
    for (int i = 0; i < 5; ++i)
        if (sep[i] != ':' && sep[i] != '-') return false;
    if (strlen(s) != 17) return false;
    bool zero = true, ones = true;
    for (int i = 0; i < 6; ++i) {
        out[i] = uint8_t(v[i]);
        zero = zero && v[i] == 0;
        ones = ones && v[i] == 0xFF;
    }
    return !zero && !ones;
}
// "any" or the MAC, upper case; a static buffer, fine for logging and file writing.
inline const char* remoteText(const Settings& s) {
    static char b[18];
    if (!s.lock) return "any";
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", s.remoteMac[0], s.remoteMac[1], s.remoteMac[2],
             s.remoteMac[3], s.remoteMac[4], s.remoteMac[5]);
    return b;
}

// Parses text over a copy of base (values not mentioned keep base's).
inline Result parse(const char* text, const Settings& base) {
    using namespace detail;
    Result r;
    r.settings = base;
    static const char* const feedback[] = {"voice", "off"};
    static const char* const filter[] = {"on", "strong", "off"};
    static const char* const adapt[] = {"normal", "slow", "fast"};
    static const char* const distance[] = {"normal", "near", "far"};
    static const char* const mode[] = {"auto", "microphone", "speaker"};
    int lineNo = 0;
    while (text && *text) {
        char line[160];
        size_t n = strcspn(text, "\r\n");
        const size_t copy = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, text, copy);
        line[copy] = 0;
        text += n;
        if (*text == '\r') ++text;  // one line ending: \r\n, \n or \r
        if (*text == '\n') ++text;
        lineNo++;
        char* hash = strpbrk(line, "#;");
        if (hash) *hash = 0;
        char* l = trim(line);
        if (!*l) continue;
        char* sep = strchr(l, '=');  // "remote: 44:BD:..." has more ':' than one
        if (!sep) sep = strchr(l, ':');
        bool ok = false;
        if (sep) {
            *sep = 0;
            char* key = trim(l);
            char* val = trim(sep + 1);
            lower(key);
            lower(val);
            char* end = nullptr;
            const long num = strtol(val, &end, 10);
            const bool isNum = *val && end && *end == 0;
            int idx;
            if (strcmp(key, "mic") == 0) {
                if ((ok = isNum && num >= 1 && num <= 9)) r.settings.mic = uint8_t(num);
            } else if (strcmp(key, "channel") == 0) {
                if ((ok = isNum && num >= 1 && num <= 11)) r.settings.channel = uint8_t(num);
            } else if (strcmp(key, "feedback") == 0) {
                if ((ok = (idx = pick(val, feedback, 2)) >= 0)) r.settings.feedback = uint8_t(idx);
            } else if (strcmp(key, "noise_filter") == 0) {
                if ((ok = (idx = pick(val, filter, 3)) >= 0)) r.settings.filter = uint8_t(idx);
            } else if (strcmp(key, "tx_power") == 0) {
                if (strcmp(val, "max") == 0 || strcmp(val, "maximum") == 0) { r.settings.txPower = 20; ok = true; }
                else if ((ok = isNum && num >= 2 && num <= 20)) r.settings.txPower = uint8_t(num);
            } else if (strcmp(key, "highpass") == 0) {
                if ((ok = strcmp(val, "on") == 0 || strcmp(val, "off") == 0)) r.settings.highpass = strcmp(val, "on") == 0;
            } else if (strcmp(key, "temperature") == 0) {
                ok = true;  // 1.4.4: setting removed; an old config.txt line is ignored
            } else if (strcmp(key, "highpass_hz") == 0) {
                if ((ok = isNum && num >= 50 && num <= 1000)) r.settings.highpassHz = uint16_t(num);
            } else if (strcmp(key, "cpu_mhz") == 0) {
                if ((ok = isNum && (num == 240 || num == 160))) r.settings.cpuMhz = uint8_t(num);
            } else if (strcmp(key, "mic_level") == 0) {
                if (strcmp(val, "default") == 0) { r.settings.micLevel = 100; ok = true; }  // 1.4.1: default = 100 %
                else if (strcmp(val, "adapter") == 0) { r.settings.micLevel = kLevelDefault; ok = true; }
                else if ((ok = isNum && num >= 0 && num <= 100)) r.settings.micLevel = uint8_t(num);
            } else if (strcmp(key, "noise_adapt") == 0) {
                if ((ok = (idx = pick(val, adapt, 3)) >= 0)) r.settings.adapt = uint8_t(idx);
            } else if (strcmp(key, "distance") == 0) {
                if ((ok = (idx = pick(val, distance, 3)) >= 0)) r.settings.distance = uint8_t(idx);
            } else if (strcmp(key, "mode") == 0) {
                if ((ok = (idx = pick(val, mode, 3)) >= 0)) r.settings.mode = uint8_t(idx);
                else if ((ok = strcmp(val, "mic") == 0)) r.settings.mode = 1;
            } else if (strcmp(key, "remote") == 0) {
                uint8_t mac[6];
                if (strcmp(val, "any") == 0) {
                    r.settings.lock = false;
                    memset(r.settings.remoteMac, 0, 6);
                    ok = true;
                } else if ((ok = parseMac(val, mac))) {
                    r.settings.lock = true;
                    memcpy(r.settings.remoteMac, mac, 6);
                }
            }
        }
        if (!ok) {
            if (!r.errors) snprintf(r.firstError, sizeof(r.firstError), "line %d not understood", lineNo);
            r.errors++;
        }
    }
    return r;
}

// The file as written for a new drive (and after a reset): every setting with its choices.
inline int format(const Settings& s, char* out, size_t cap) {
    char level[12];
    levelText(s.micLevel, level, sizeof(level));
    return snprintf(out, cap,
        "# Wireless USB Audio Extender settings\r\n"
        "# Edit, save, then eject the EXTENDER drive: the local board restarts and\r\n"
        "# passes the remote's settings on over the radio. Delete this file to\r\n"
        "# return to the defaults.\r\n"
        "\r\n"
        "mic = %u               # 1-9: the PC sees \"Extender Mic N\" (both boards follow)\r\n"
        "channel = %u          # Wi-Fi channel 1-11 (both boards follow); pick a quiet one.\r\n"
        "                       # More than one pair: give each pair its own channel (1, 6, 11)\r\n"
        "feedback = %s       # voice or off (announcements in the recording; for beeps, copy\r\n"
        "                       # the beeps voice pack into the voice folder)\r\n"
        "noise_filter = %s       # on (12 dB), strong (20 dB) or off (at the remote)\r\n"
        "noise_adapt = %s   # normal, slow or fast: how quickly the filter follows changes\r\n"
        "tx_power = %s          # max, or 2-20 (dBm): radio transmit power of both boards\r\n"
        "cpu_mhz = %u           # 240 or 160: processor speed of both boards (160 runs a little cooler)\r\n"
        "highpass = %s           # on, off: remove rumble, wind and hum below highpass_hz (remote)\r\n"
        "highpass_hz = %u        # 50-1000: high-pass cut-off in Hz (200 keeps low owl and dove calls)\r\n"
        "mic_level = %s         # 0-100: the adapter's microphone level, %% of its range (default 100);\r\n"
        "                       # adapter = leave the adapter's own level. AtomS3U: the built-in mic's\r\n"
        "                       # gain, 0-100 = 0 to +30 dB. status.txt shows it in dB. In speaker\r\n"
        "                       # mode it sets the adapter's speaker level\r\n"
        "distance = %s       # near, normal or far: far reaches furthest but copes worst with busy\r\n"
        "                       # Wi-Fi; near is the most robust where Wi-Fi is busy (both boards)\r\n"
        "mode = %s             # auto, microphone or speaker: which way the audio goes. auto decides\r\n"
        "                       # when the remote starts: a mic signal = microphone, silence = speaker\r\n"
        "                       # (with a microphone always connected, microphone is the safe choice)\r\n"
        "remote = %s            # any, or the remote's MAC (status.txt): link only to that remote, and\r\n"
        "                       # lock it to this local board. Back to any unlocks it again.\r\n",
        s.mic, s.channel, feedbackName(s.feedback), filterName(s.filter), adaptName(s.adapt), txPowerText(s.txPower),
        unsigned(s.cpuMhz), s.highpass ? "on" : "off", unsigned(s.highpassHz), level,
        distanceName(s.distance), modeName(s.mode), remoteText(s));
}

}  // namespace cfg
