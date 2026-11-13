#include "Announcer.h"
#include <FFat.h>
#include <esp_partition.h>
#include <atomic>
#include "Adpcm.h"
#include "VoiceMixer.h"
#include "WavFormat.h"
#include "voice_clips.h"

namespace announcer {
namespace {

constexpr int kEvents = int(Event::Count);
static_assert(sizeof(kBuiltinClips) / sizeof(kBuiltinClips[0]) == kEvents, "voice_clips.h does not match Event");

constexpr uint32_t kOutRate = 48000;
constexpr uint32_t kBuiltinRate = 16000;
constexpr float kPlaybackGain = 0.25f;  // -12 dB relative to a full-scale clip

struct FileClip {
    bool ok = false;
    uint16_t channels = 1;
    uint32_t rate = 0;
    uint32_t dataOffset = 0;
    uint32_t firstFrame = 0, endFrame = 0;  // trimmed range
    float gain = 1;                         // normalise to -1 dBFS, then playback gain
    const char* problem = "";               // why it is not used ("" when ok)
};

VoiceMixer* s_mixer = nullptr;
bool (*s_recording)() = nullptr;
void (*s_log)(const char*, ...) = nullptr;
void (*s_event)(const char*, ...) = nullptr;
// What the built-in clips say (replacement files may say it differently).
const char* const kPhrases[] = {
    "Microphone one", "Microphone two", "Microphone three", "Microphone four", "Microphone five",
    "Microphone six", "Microphone seven", "Microphone eight", "Microphone nine",
    "Microphone connected", "Microphone link lost",
    "Adapter connected", "Adapter removed", "Adapter not supported",
    "Noise filter on", "Noise filter strong", "Noise filter off",
    "No microphone signal"};
static_assert(sizeof(kPhrases) / sizeof(kPhrases[0]) == int(Event::Count), "one phrase per announcement");
QueueHandle_t s_queue = nullptr;
std::atomic<uint8_t> s_mode{uint8_t(Mode::Voice)};
FileClip s_files[kEvents];
bool s_fatOk = false;

const char* clipName(int i) { return kBuiltinClips[i].name; }

String clipPath(int i) { return String("/voice/") + clipName(i) + ".wav"; }

// ---------------------------------------------------------------- voice drive contents
void writeWavHeader(File& f, uint32_t rate, uint32_t frames) {
    uint8_t h[44];
    auto w32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
    auto w16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
    memcpy(h, "RIFF", 4); w32(4, 36 + frames * 2); memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1); w32(24, rate); w32(28, rate * 2); w16(32, 2); w16(34, 16);
    memcpy(h + 36, "data", 4); w32(40, frames * 2);
    f.write(h, sizeof(h));
}

// First use (or after a factory reset): put the built-in clips on the drive as
// plain WAV files, plus a short README, so there is something to replace.
// Each clip is written as voice/<name>.tmp and renamed into place when complete, so a
// power cut during the first start (right after a full flash) never leaves a broken or
// empty clip behind. An empty file under a built-in name is written again (an empty
// file is never someone's own recording).
// README.txt first: it is small, so the drive explains itself even if the clips that follow
// are cut short (a power cut during the first start).
void writeReadme() {
    if (FFat.exists("/README.txt")) {
        File o = FFat.open("/README.txt", FILE_READ);
        const size_t size = o ? o.size() : 0;
        if (o) o.close();
        if (size) return;  // keep it (an empty one is written again)
    }
    File r = FFat.open("/README.txt", FILE_WRITE);
    if (r) {
        r.print("Wireless USB Audio Extender - EXTENDER drive\r\n\r\n"
                "config.txt  the settings of both boards (mic number, channel, feedback,\r\n"
                "            noise filter). Edit and save; delete it to return to the defaults.\r\n"
                "status.txt  what was in effect when the drive was opened.\r\n"
                "adapter.txt the remote's latest audio adapter: its details and capabilities.\r\n"
                "events.txt  the event log: starts, USB and link drops, restarts (kept through power cuts).\r\n"
                "noise.svg   the noise filter's learned background noise (when the filter has learned it).\r\n"
                "report.html all of the above at a glance: open it in any browser.\r\n"
                "voice       the announcements.\r\n\r\n"
                "The local board speaks the files in 'voice'. Replace any of them with\r\n"
                "your own recording or another language: keep the file names, save as\r\n"
                "WAV, 16-bit PCM, mono or stereo, 8000-48000 Hz, about 1-2 s each.\r\n"
                "Silence at the ends is trimmed and the level is evened out automatically.\r\n"
                "Ready-made sets (other languages, beeps) are in the project's voice-packs folder.\r\n"
                "A missing or unusable file plays the built-in voice instead.\r\n\r\n"
                "When done, eject the drive (or press RESET): the local board restarts as the microphone\r\n"
                "and passes the remote's settings on over the radio.\r\n");
        r.close();
    }
}

void fillVoiceDrive() {
    writeReadme();
    FFat.mkdir("/voice");
    int written = 0;
    for (int i = 0; i < kEvents; ++i) {
        const BuiltinClip& c = kBuiltinClips[i];
        if (!c.data) continue;
        const String path = clipPath(i);
        const String tmp = String("/voice/") + clipName(i) + ".tmp";
        if (FFat.exists(tmp)) FFat.remove(tmp);  // left by a power cut
        if (FFat.exists(path)) {
            File old = FFat.open(path, FILE_READ);
            const size_t size = old ? old.size() : 0;
            if (old) old.close();
            if (size > 0) continue;
            FFat.remove(path);
            if (s_log) s_log("voice: %s.wav was empty (power cut while it was written?): writing it again", clipName(i));
        }
        File f = FFat.open(tmp, FILE_WRITE);
        if (!f) continue;
        writeWavHeader(f, kBuiltinRate, c.samples);
        adpcm::Decoder d;
        int16_t chunk[256];
        size_t bytes = 44;
        for (uint32_t n = 0; n < c.samples;) {
            uint32_t k = 0;
            for (; k < 256 && n < c.samples; ++k, ++n) chunk[k] = adpcm::sampleAt(d, c.data, n);
            bytes += f.write((const uint8_t*)chunk, k * 2);
        }
        f.close();
        if (bytes != 44 + size_t(c.samples) * 2 || !FFat.rename(tmp, path)) {  // drive full?
            FFat.remove(tmp);
            if (s_log) s_log("voice: could not write %s.wav (drive full?)", clipName(i));
            continue;
        }
        written++;
    }
    if (written && s_log) s_log("voice drive: wrote %d built-in clip(s)", written);
}

// Validates /voice/<name>.wav and finds its trimmed range and level.
void checkFile(int i) {
    FileClip& c = s_files[i];
    c = FileClip();
    File f = FFat.open(clipPath(i), FILE_READ);
    if (!f) {
        c.problem = "missing";
        if (s_log) s_log("voice: %s.wav missing, using the built-in clip", clipName(i));
        return;
    }
    if (f.size() == 0) {
        c.problem = "empty file";
        if (s_log) s_log("voice: %s.wav is empty (0 bytes), using the built-in clip", clipName(i));
        f.close();
        return;
    }
    uint8_t head[512];
    const int got = f.read(head, sizeof(head));
    const wav::Info info = wav::parse(head, got > 0 ? uint32_t(got) : 0, f.size());
    if (!info.ok) {
        c.problem = info.problem;
        if (s_log) s_log("voice: %s.wav not used (%s), using the built-in clip", clipName(i), info.problem);
        f.close();
        return;
    }
    // One pass for the peak and the first/last audible frame.
    f.seek(info.dataOffset);
    const uint32_t frames = info.frames();
    int32_t peak = 0;
    int16_t buf[256];
    uint32_t frame = 0;
    while (frame < frames) {
        const uint32_t want = min<uint32_t>(256 / info.channels, frames - frame);
        const int n = f.read((uint8_t*)buf, want * info.channels * 2) / (2 * info.channels);
        if (n <= 0) break;
        for (int k = 0; k < n * info.channels; ++k) peak = max<int32_t>(peak, abs(int32_t(buf[k])));
        frame += n;
    }
    if (peak < 30) {
        c.problem = "silent";
        if (s_log) s_log("voice: %s.wav is silent, using the built-in clip", clipName(i));
        f.close();
        return;
    }
    const int32_t thr = max<int32_t>(peak / 33, 60);
    uint32_t first = frames, last = 0;
    f.seek(info.dataOffset);
    frame = 0;
    while (frame < frames) {
        const uint32_t want = min<uint32_t>(256 / info.channels, frames - frame);
        const int n = f.read((uint8_t*)buf, want * info.channels * 2) / (2 * info.channels);
        if (n <= 0) break;
        for (int k = 0; k < n; ++k) {
            int32_t v = 0;
            for (int ch = 0; ch < info.channels; ++ch) v = max<int32_t>(v, abs(int32_t(buf[k * info.channels + ch])));
            if (v > thr) { if (first == frames) first = frame + k; last = frame + k; }
        }
        frame += n;
    }
    f.close();
    const uint32_t pad = info.rate * 3 / 100;  // keep 30 ms around the sound
    c.ok = true;
    c.channels = info.channels;
    c.rate = info.rate;
    c.dataOffset = info.dataOffset;
    c.firstFrame = first > pad ? first - pad : 0;
    c.endFrame = min(frames, last + pad + 1);
    c.gain = min(8.0f, 32767.0f * 0.891f / float(peak)) * kPlaybackGain;
}

// ---------------------------------------------------------------- playback
bool stillWanted(Mode m) { return s_recording && s_recording() && Mode(s_mode.load()) == m; }

// Resamples a source (any rate) to 48 kHz by linear interpolation and pushes
// it into the mixer. pull(): next source sample, or false at the end.
template <typename Pull>
void playStream(uint32_t srcRate, float gain, Mode m, Pull pull) {
    const uint32_t step = uint32_t((uint64_t(srcRate) << 16) / kOutRate);  // Q16 source samples per output sample
    int16_t a = 0, b = 0;
    if (!pull(a) || !pull(b)) return;
    uint32_t frac = 0;
    int16_t out[256];
    bool more = true;
    while (more) {
        int n = 0;
        while (n < 256 && more) {
            const int32_t v = a + (((int32_t(b) - a) * int32_t(frac >> 1)) >> 15);
            const float g = float(v) * gain;
            out[n++] = int16_t(g > 32767 ? 32767 : g < -32768 ? -32768 : g);
            frac += step;
            while (frac >= 0x10000 && more) {
                frac -= 0x10000;
                a = b;
                more = pull(b);
            }
        }
        for (int done = 0; done < n;) {
            if (!stillWanted(m)) { s_mixer->requestFlush(); return; }
            done += s_mixer->push(out + done, n - done);
            if (done < n) vTaskDelay(pdMS_TO_TICKS(3));
        }
    }
}

void playBuiltin(int i) {
    const BuiltinClip& c = kBuiltinClips[i];
    if (!c.data) return;
    adpcm::Decoder d;
    uint32_t n = 0;
    playStream(kBuiltinRate, kPlaybackGain, Mode::Voice, [&](int16_t& s) {
        if (n >= c.samples) return false;
        s = adpcm::sampleAt(d, c.data, n++);
        return true;
    });
}

void playFile(int i) {
    const FileClip& c = s_files[i];
    File f = FFat.open(clipPath(i), FILE_READ);
    if (!f) { playBuiltin(i); return; }
    f.seek(c.dataOffset + c.firstFrame * c.channels * 2);
    int16_t buf[256];
    int have = 0, pos = 0;
    uint32_t frame = c.firstFrame;
    playStream(c.rate, c.gain, Mode::Voice, [&](int16_t& s) {
        if (frame >= c.endFrame) return false;
        if (pos >= have) {
            const uint32_t want = min<uint32_t>(256 / c.channels, c.endFrame - frame);
            have = f.read((uint8_t*)buf, want * c.channels * 2) / (2 * c.channels);
            pos = 0;
            if (have <= 0) return false;
        }
        s = c.channels == 2 ? int16_t((int32_t(buf[pos * 2]) + buf[pos * 2 + 1]) / 2) : buf[pos];
        pos++;
        frame++;
        return true;
    });
    f.close();
}

void task(void*) {
    for (;;) {
        uint8_t e;
        if (xQueueReceive(s_queue, &e, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        const Mode m = Mode(s_mode.load());
        if (m == Mode::Off || !s_recording || !s_recording()) continue;  // nobody would hear it
        if (s_fatOk && s_files[e].ok) playFile(e);
        else playBuiltin(e);  // tools/make_voice.py gives every announcement a built-in clip
        vTaskDelay(pdMS_TO_TICKS(150));  // a short pause between announcements
    }
}

}  // namespace

void begin(VoiceMixer* mixer, bool (*recording)(), void (*log)(const char* fmt, ...)) {
    s_mixer = mixer;
    s_recording = recording;
    s_log = log;
    s_fatOk = FFat.begin(true);  // formats on first use
    if (!s_fatOk) {
        if (s_log) s_log("voice drive: not available (FAT partition missing?); using built-in clips");
    } else {
        fillVoiceDrive();  // first use, factory reset, or a newer firmware with more announcements
        int custom = 0;
        for (int i = 0; i < kEvents; ++i) {
            checkFile(i);
            if (s_files[i].ok) custom++;
        }
        if (s_log) s_log("voice drive: %d of %d clips usable, %u KB free", custom, kEvents,
                         unsigned((FFat.totalBytes() - FFat.usedBytes()) / 1024));
        if (custom < kEvents && s_log) {
            char list[200];
            unusableText(list, sizeof(list));
            s_log("voice drive: not usable (the built-in voice plays instead): %s", list);
        }
    }
    s_queue = xQueueCreate(8, sizeof(uint8_t));
    xTaskCreatePinnedToCore(task, "announce", 6144, nullptr, 3, nullptr, 1);
}

void setEventLog(void (*log)(const char* fmt, ...)) { s_event = log; }

void setMode(Mode m) {
    if (Mode(s_mode.exchange(uint8_t(m))) != m && s_mixer) s_mixer->requestFlush();
}
Mode mode() { return Mode(s_mode.load()); }

void say(Event e) {
    if (!s_queue || Mode(s_mode.load()) == Mode::Off) return;
    if (!s_recording || !s_recording()) return;  // only while the PC records
    const uint8_t v = uint8_t(e);
    if (xQueueSend(s_queue, &v, 0) == pdTRUE && s_event) {
        s_event("said: \"%s\"%s", kPhrases[v], s_files[v].ok ? "" : " (built-in voice)");
    }
}

const char* modeText(Mode m) { return m == Mode::Voice ? "voice" : "off"; }

void unusableText(char* out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < kEvents && o + 1 < cap; ++i) {
        if (s_files[i].ok) continue;
        const int n = snprintf(out + o, cap - o, "%s%s.wav (%s)", o ? ", " : "", clipName(i),
                               s_files[i].problem[0] ? s_files[i].problem : "not checked");
        if (n < 0) break;
        o += size_t(n) < cap - o ? size_t(n) : cap - o - 1;
    }
    if (!o) snprintf(out, cap, "none");
}

int usableFiles() {
    int n = 0;
    for (int i = 0; i < kEvents; ++i) n += s_files[i].ok ? 1 : 0;
    return n;
}

// Raw erase of the whole FAT partition: wear levelling and FAT both start afresh at the next
// mount (FFat.format() works through wear levelling, so it cannot repair damage there).
bool eraseDrivePartition() {
    FFat.end();
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
    return p && esp_partition_erase_range(p, 0, p->size) == ESP_OK;
}

void eraseVoiceFiles() { eraseDrivePartition(); }

namespace {
// Adds up the files below dir (each rounded up to a 4 KB cluster, a folder counts one).
// false: a folder could not be opened (damaged).
bool sumDir(File dir, uint64_t& sum, int depth) {
    if (!dir || !dir.isDirectory()) return false;
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (f.isDirectory()) {
            sum += 4096;
            if (depth > 0) {
                File sub = FFat.open(f.path());
                if (!sumDir(sub, sum, depth - 1)) { f.close(); return false; }
            }
        } else {
            sum += (uint64_t(f.size()) + 4095) / 4096 * 4096;
        }
        f.close();
    }
    return true;
}
}  // namespace

bool driveDamaged(char* why, size_t cap) {
    if (FFat.exists("/voice")) {
        File v = FFat.open("/voice");
        const bool ok = v && v.isDirectory();
        if (v) v.close();
        if (!ok) { snprintf(why, cap, "the voice folder could not be read"); return true; }
    }
    uint64_t sum = 0;
    File root = FFat.open("/");
    const bool walked = sumDir(root, sum, 3);
    if (root) root.close();
    if (!walked) { snprintf(why, cap, "a folder could not be read"); return true; }
    const uint64_t used = FFat.usedBytes();
    if (used > sum + 128 * 1024) {  // space taken by nothing on the drive: lost clusters
        snprintf(why, cap, "%u KB used by no file", unsigned((used - sum) / 1024));
        return true;
    }
    return false;
}

}  // namespace announcer
