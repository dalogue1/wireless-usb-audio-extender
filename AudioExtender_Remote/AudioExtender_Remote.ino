// Wireless USB Audio Extender - REMOTE (at the microphone)
//
// One firmware for two boards (board::kind(), from the chip's eFuses):
//   Seeed XIAO ESP32-S3: a USB audio adapter on its USB-C port (below), microphone or speaker.
//   M5Stack AtomS3U: its built-in PDM microphone (BuiltInMic.h), microphone only; the USB
//   host is never started (its USB-A plug only takes power).
//
// Microphone mode: captures from a USB Audio Class 1 adapter (e.g. UGREEN 30757 +
// AOM5024 capsule) on the XIAO's USB-C port in host mode, and streams it over ESP-NOW
// to the paired local. 48 kHz mono 16-bit, 12 ms per packet.
// Speaker mode: the other way round - audio from the local board (the PC's "Extender
// Speaker") is played through the adapter's headphone/speaker output.
// config.txt `mode` (auto: decided once when an adapter is plugged in, see decideMode()).
//
// Board: Seeed XIAO ESP32-S3 with its antenna attached.
// Power: 5 V into a USB-C OTG adapter with a charging input (the XIAO does not
// power its USB-C port), adapter -> audio adapter -> microphone.
// Arduino IDE (esp32 core 3.3.x) Tools menu:
//   Board:              XIAO_ESP32S3
//   USB Mode:           Hardware CDC and JTAG   (the OTG port is the USB host)
//   USB CDC On Boot:    Disabled   (logs go to the UART on D6/D7 and to the local board)
//   Upload Mode:        UART0 / Hardware CDC
// Once running, the USB-C port belongs to the audio adapter: to upload again,
// hold BOOT while plugging the XIAO into the computer.
// Settings (mic number, channel, filters...) come from the local board's config.txt.

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Build for the XIAO ESP32-S3"
#endif
#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 1
#error "Remote: set Tools > USB Mode to 'Hardware CDC and JTAG' (the OTG port is used as USB host)"
#endif

#include <atomic>
#include "esp_log.h"
#include "BoardCommon.h"
#include "LinkProtocol.h"
#include "UsbMic.h"
#include "NoiseSignature.h"
#include "HighPass.h"
#include "AdapterInfo.h"
#include "JitterBuffer.h"
#include "ModeDecision.h"
#include "BuiltInMic.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <esp_task_wdt.h>

static const char* kFirmware = "1.4.1-dev";
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static board::StatusLed led;
static bool builtIn = false;  // AtomS3U: built-in microphone instead of a USB adapter (set in setup())
// The microphone in use is delivering audio.
static bool micLive() { return builtIn ? pdmmic::running() : usbmic::streaming(); }
static uint8_t pairId = 0;
static uint8_t channel = 1;

// Settings: received from the local board (from its config.txt) and saved.
static board::SavedSettings settings;
static HoldButton bootButton;
static std::atomic<uint8_t> adaptSpeed{wlink::kAdaptNormal};
static uint8_t txPowerDbm = wlink::kMaxTxPowerDbm;
static uint8_t cpuMhz = wlink::kCpuMhzDefault;
static bool temperatureOn = false;  // config.txt 'temperature', from the local board
static uint8_t micLevel = wlink::kLevelDefault;  // config.txt 'mic_level': 0..100 %, or default
static int16_t chipTemp = wlink::kTempUnknown;  // latest reading (once every 10 min while on)
static uint8_t distance = wlink::kDistanceNormal;  // config.txt 'distance': radio rate of the link
// Audio direction: config.txt 'mode' (saved), and what is in effect (decideMode()).
static uint8_t modeSetting = wlink::kModeAuto;
static std::atomic<uint8_t> audioMode{wlink::kModeMicrophone};
static std::atomic<uint8_t> modeReason{wlink::kReasonDeciding};
static bool speaker() { return audioMode.load() == wlink::kModeSpeaker; }
// Speaker mode: audio from the local board, played to the adapter (USB task pulls 1 ms frames).
static JitterBuffer speakerJb;
// Pairing by MAC (config.txt 'remote'): the one local board this remote links to.
static uint8_t lockMac[6] = {0};
static volatile bool locked = false;
static std::atomic<uint32_t> rejected{0};  // hellos from other local boards (locked out, or we are linked)
static uint8_t rejectedMac[6] = {0};       // the last of them (under linkMux)
static std::atomic<bool> noiseRequested{false};
// High-pass filter: set by loop() (saved setting / local board), applied by the audio task.
static std::atomic<bool> highpassOn{true};
static std::atomic<uint16_t> highpassHz{hpf::kDefaultHz};
static std::atomic<uint32_t> highpassVersion{1};
// Survives ESP.restart() (not a power cut): why the remote restarted itself.
// What the radio looked like when the link was lost, and during the search. Kept in
// RTC memory so it survives the remote's own restart and is reported after relinking.
struct DropInfo {
    uint32_t magic;          // kNoteNoLink when the remote restarted itself after a drop
    uint32_t sinceRxMs;      // last packet from the local board, before the loss was noticed
    uint32_t sinceSendOkMs;  // last acknowledged send
    uint32_t errStreak;      // esp_now_send() failures in a row
    int32_t lastErr;         // the last of those errors
    uint32_t ackStreak;      // sends in a row without an acknowledgement
    uint32_t searchRx;       // packets heard from the local board while searching
    uint32_t beaconErr;      // beacons that could not be sent while searching
    int32_t beaconErrCode;   // the first of those errors
    uint32_t why;            // why the remote restarted itself (kWhy...)
    // To tell why the radio refuses sends (ESP_ERR_ESPNOW_NO_MEM): out of memory (a leak:
    // free memory low) or a jammed transmit queue (memory fine, packets stuck in the radio).
    uint32_t heapFree, heapLargest, heapMin;  // internal RAM at the drop
    uint32_t radioQueued;    // packets handed to the radio, not yet reported sent
    uint32_t sinceReportMs;  // last send report (acknowledged or not) from the radio
};
constexpr uint32_t kWhyNoLink = 0, kWhyRadioStuck = 1, kWhyTxStuck = 2;
RTC_NOINIT_ATTR static DropInfo dropNote;
constexpr uint32_t kNoteNoLink = 0x4E4F4C4B;  // "NOLK"
static DropInfo drop;                          // the current/last drop
static std::atomic<uint32_t> lastSendOkMs{0}, errStreak{0}, ackStreak{0}, searchRx{0};
static std::atomic<int32_t> lastSendErr{0};
static std::atomic<bool> radioPaused{false};   // radio stopped (temperature read / radio restart)
// Packets handed to the radio (esp_now_send OK) and send reports back from it; the
// difference is what sits in the radio's transmit queue.
static std::atomic<uint32_t> submitted{0}, reports{0}, lastReportMs{0};
static uint32_t radioQueued() { return submitted.load() - reports.load(); }
static esp_err_t radioSend(const uint8_t* mac, const void* data, size_t len) {
    const esp_err_t err = esp_now_send(mac, (const uint8_t*)data, len);
    if (err == ESP_OK) submitted++;
    return err;
}
static void reportDrop(const char* when);
static wlink::ConfigPacket pendingConfig;
static volatile bool configPending = false;
static uint8_t savedFilter = wlink::kFilterOn;

// Noise filter (adaptive noise-signature subtraction). The engine always runs
// and keeps learning; the mode only decides whether its output is used and how
// strongly it subtracts, so switching is instant.
static nsig::Engine* noiseEngine = nullptr;
static std::atomic<uint8_t> filterMode{wlink::kFilterOn};
static std::atomic<bool> filterLearned{false};

static portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t localMac[6];
static uint8_t candidateMac[6];
static std::atomic<bool> linked{false};
static volatile bool candidatePending = false;
static volatile uint32_t lastRxMs = 0;
static volatile int8_t rssi = 0;
static std::atomic<bool> streamStart{true};
static std::atomic<uint32_t> sent{0}, sendFailed{0}, deliveryFailed{0};
static std::atomic<uint16_t> blockPeak{0};  // loudest sample since the LED last looked
// Silent microphone: a real microphone always produces some hiss. Streaming
// audio that stays below -75 dBFS for 20 s means no microphone signal reaches
// the adapter (unplugged, wrong jack, TRRS headset plug in a TRS mic jack...).
static constexpr uint16_t kSilentPeak = 6;           // -75 dBFS
static constexpr uint32_t kSilentMs = 20000;
static std::atomic<bool> noSignal{false};
static std::atomic<int8_t> noSignalChange{0};       // +1 began, -1 ended (for loop() to log)

// Auto mode: any sample that is not exactly zero since the capture stream began means a
// microphone is there (an adapter with jack sensing sends pure zeros with nothing plugged in).
static std::atomic<bool> heardNonZero{false};
static std::atomic<uint32_t> captureSinceMs{0};  // capture stream (re)started at (0: not streaming)

static void checkSignal(const int16_t* pcm) {
    static uint32_t quietSince = 0;
    uint16_t peak = 0;
    for (int i = 0; i < wlink::kBlockSamples; ++i) {
        const int v = pcm[i] < 0 ? -int(pcm[i]) : pcm[i];
        if (v > peak) peak = uint16_t(v > 32767 ? 32767 : v);
    }
    const uint32_t now = millis();
    if (peak && !heardNonZero.load()) heardNonZero = true;
    if (peak > kSilentPeak) {
        quietSince = 0;
        if (noSignal.exchange(false)) noSignalChange = -1;
        return;
    }
    if (!quietSince) quietSince = now ? now : 1;
    if (!noSignal.load() && now - quietSince >= kSilentMs) {
        noSignal = true;
        noSignalChange = 1;
    }
}

// The latest adapter's details (AdapterInfo.h), built when an adapter is plugged in and
// sent when the local board asks for them (it does when it sees a new adapterInfoId).
static uint8_t adapterRecord[wlink::kInfoMax];
static uint16_t adapterRecordLen = 0;
static uint32_t adapterInfoId = 0;              // 0: no adapter seen since start
static std::atomic<uint32_t> infoRequested{0};  // id the local board asked for (0: none)

static QueueHandle_t logQueue = nullptr;
struct LogText { char text[sizeof(wlink::LogPacket::text)]; };

// ---------------------------------------------------------------- logging
// Every line goes to the UART (D6 = TX, 115200) and, when linked, to the
// local board's USB serial port.
static void logPiece(const char* body);
// Lines longer than one log packet are split, the continuation starting with "...",
// so nothing is cut off.
static void logf(const char* fmt, ...) {
    char all[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(all, sizeof(all), fmt, ap);
    va_end(ap);
    constexpr size_t kPiece = sizeof(LogText::text) - 16;  // room for the time and "... "
    const char* p = all;
    size_t left = strlen(all);
    bool first = true;
    do {
        char piece[sizeof(LogText::text)];
        size_t n = left > kPiece ? kPiece : left;
        if (n < left) {  // break at a space where possible
            size_t sp = n;
            while (sp > kPiece / 2 && p[sp] != ' ') --sp;
            if (p[sp] == ' ') n = sp;
        }
        snprintf(piece, sizeof(piece), "%s%.*s", first ? "" : "... ", int(n), p);
        logPiece(piece);
        p += n;
        left -= n;
        while (left && *p == ' ') { ++p; --left; }
        first = false;
    } while (left);
}

static void logPiece(const char* body) {
    LogText t;
    snprintf(t.text, sizeof(t.text), "%7.1f %s", esp_timer_get_time() / 1e6, body);  // 64-bit: no 49.7-day wrap
    Serial0.println(t.text);
    if (logQueue) {
        if (xQueueSend(logQueue, &t, 0) != pdTRUE) {  // full: drop the oldest
            LogText old;
            xQueueReceive(logQueue, &old, 0);
            xQueueSend(logQueue, &t, 0);
        }
    }
}

static void logLineFromUsb(const String& s) { logf("%s", s.c_str()); }

// ESP-IDF's own log (only errors are compiled into the Arduino core, e.g. USB
// enumeration failures) normally goes to the UART only. Also forward it to the
// local, without ANSI colour codes. Called from whatever task logged.
static vprintf_like_t idfVprintf = nullptr;
static int forwardIdfLog(const char* fmt, va_list ap) {
    char raw[sizeof(LogText::text)];
    va_list copy;
    va_copy(copy, ap);
    vsnprintf(raw, sizeof(raw), fmt, copy);
    va_end(copy);
    LogText t = {};
    size_t o = 0;
    snprintf(t.text, sizeof(t.text), "%7.1f idf: ", esp_timer_get_time() / 1e6);
    o = strlen(t.text);
    for (const char* p = raw; *p && o + 1 < sizeof(t.text); ++p) {
        if (*p == '\033') { while (*p && *p != 'm') ++p; if (!*p) break; continue; }  // skip colour codes
        if (*p == '\r' || *p == '\n') continue;
        t.text[o++] = *p;
    }
    t.text[o] = 0;
    if (logQueue) xQueueSend(logQueue, &t, 0);
    return idfVprintf ? idfVprintf(fmt, ap) : 0;
}

// Every 10 s while no audio adapter streams: what the USB host can see.
static void reportUsbHost() {
    if (builtIn) return;
    static uint32_t last = 0;
    if (millis() - last < 10000) return;
    last = millis();
    const usbmic::Status s = usbmic::status();
    if (s.streaming) return;
    const int n = usbmic::devicesOnBus();
    if (n < 0) logf("USB host: not running");
    else if (n == 0) logf("USB host: no device detected on the USB-C port (check 5 V at the OTG adapter)");
    else logf("USB host: %d device(s) on the bus, none streaming%s%s", n, s.problem[0] ? ": " : "", s.problem);
}

// ---------------------------------------------------------------- radio
static void noteRejected(const uint8_t* mac) {
    rejected++;
    portENTER_CRITICAL(&linkMux);
    memcpy(rejectedMac, mac, 6);
    portEXIT_CRITICAL(&linkMux);
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t type = wlink::check(data, len, pairId);
    if (!type) return;
    // Locked (config.txt remote = <MAC> on the local board): nothing from any other local board.
    if (locked && memcmp(info->src_addr, lockMac, 6) != 0) {
        if (type == wlink::kHello) noteRejected(info->src_addr);
        return;
    }
    if (type == wlink::kAudio) {  // speaker mode: the PC's audio from the local board
        if (!linked || !speaker() || memcmp(info->src_addr, localMac, 6) != 0) return;
        const auto* a = reinterpret_cast<const wlink::AudioPacket*>(data);
        static int16_t pcm[wlink::kPacketSamples];  // the radio buffer may not be 2-byte aligned
        const uint16_t n = a->samples;
        memcpy(pcm, data + offsetof(wlink::AudioPacket, pcm), n * sizeof(int16_t));
        if (a->h.flags & wlink::kFlagStreamStart) speakerJb.streamStart();
        speakerJb.pushBlock(a->seq, pcm, n);
        lastRxMs = millis();
        if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;
        return;
    }
    if (type == wlink::kNoiseRequest) {
        if (!linked || memcmp(info->src_addr, localMac, 6) != 0) return;
        noiseRequested = true;
        lastRxMs = millis();
        return;
    }
    if (type == wlink::kConfig) {
        if (!linked || memcmp(info->src_addr, localMac, 6) != 0) return;
        portENTER_CRITICAL(&linkMux);
        memcpy(&pendingConfig, data, sizeof(pendingConfig));
        configPending = true;
        portEXIT_CRITICAL(&linkMux);
        lastRxMs = millis();
        return;
    }
    if (type == wlink::kInfoRequest) {
        if (!linked || memcmp(info->src_addr, localMac, 6) != 0) return;
        infoRequested = reinterpret_cast<const wlink::InfoRequestPacket*>(data)->id;
        lastRxMs = millis();
        return;
    }
    if (type != wlink::kHello && type != wlink::kPing) return;
    if (!linked) {
        searchRx++;
        portENTER_CRITICAL(&linkMux);
        memcpy(candidateMac, info->src_addr, 6);
        candidatePending = true;
        portEXIT_CRITICAL(&linkMux);
        return;
    }
    if (memcmp(info->src_addr, localMac, 6) != 0) {  // another local board with our mic number
        if (type == wlink::kHello) noteRejected(info->src_addr);
        return;
    }
    lastRxMs = millis();
    if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;
}

static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
    (void)info;
    reports++;
    lastReportMs = millis();
    if (status != ESP_NOW_SEND_SUCCESS) {
        deliveryFailed++;  // no ACK after the MAC retries
        ackStreak++;
    } else {
        ackStreak = 0;
        lastSendOkMs = millis();
    }
}

// Remembers the block's peak for the LED level meter (works linked or not).
// The meter shows the microphone as captured, before the noise filter.
static void notePeak(const int16_t* pcm) {
    uint16_t peak = 0;
    for (int i = 0; i < wlink::kBlockSamples; ++i) {
        const int s = pcm[i];
        const int v = s < 0 ? -s : s;
        if (v > peak) peak = uint16_t(v > 32767 ? 32767 : v);
    }
    if (peak > blockPeak.load(std::memory_order_relaxed)) blockPeak.store(peak, std::memory_order_relaxed);
}

// pcm: one 4 ms block, already filtered. kPacketBlocks blocks make one packet (12 ms):
// fewer, larger packets use less airtime and less radio work than one per block.
// Copied into the packed packet by member access.
static int packetFill = 0;  // blocks already in the packet being built (audio task only)
static void sendAudio(wlink::AudioPacket& pkt, const int16_t* pcm, uint32_t& seq) {
    if (!linked) {
        streamStart = true;
        packetFill = 0;
        return;
    }
    if (radioPaused.load()) {  // radio briefly stopped: the partial packet is dropped
        packetFill = 0;
        return;
    }
    int16_t* dst = pkt.pcm + packetFill * wlink::kBlockSamples;
    for (int i = 0; i < wlink::kBlockSamples; ++i) dst[i] = pcm[i];
    if (++packetFill < wlink::kPacketBlocks) return;
    packetFill = 0;
    wlink::fill(pkt.h, wlink::kAudio, pairId, streamStart.exchange(false) ? wlink::kFlagStreamStart : 0);
    pkt.seq = seq++;
    pkt.samples = wlink::kPacketSamples;
    // The ESP-NOW queue can be momentarily full (e.g. log lines going out
    // right after linking): wait a moment and retry rather than lose 12 ms.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const esp_err_t err = radioSend(localMac, &pkt, wlink::audioPacketBytes(pkt.samples));
        if (err == ESP_OK) { sent++; errStreak = 0; return; }
        lastSendErr = err;
        if (err != ESP_ERR_ESPNOW_NO_MEM) break;
        vTaskDelay(1);
    }
    sendFailed++;
    errStreak++;
}

// ---------------------------------------------------------------- audio task
static void audioTask(void*) {
    static wlink::AudioPacket pkt;
    static int16_t block[wlink::kBlockSamples];
    uint32_t seq = 0;
    static int32_t raw[wlink::kBlockSamples];
    size_t have = 0;
    bool learning = false;  // engine initialised for the current adapter stream
    float floorDb = 0;
    uint8_t appliedAdapt = 0xFF;
    hpf::Biquad highpass;
    uint32_t highpassApplied = 0;
    esp_task_wdt_add(nullptr);  // a stopped audio task restarts the board after 5 s
    for (;;) {
        esp_task_wdt_reset();
        if (speaker()) {  // speaker mode: the USB task plays from speakerJb; nothing to capture
            captureSinceMs = 0;
            if (noSignal.exchange(false)) noSignalChange = -1;
            have = 0;
            learning = false;
            filterLearned = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!micLive()) {
            captureSinceMs = 0;
            if (noSignal.exchange(false)) noSignalChange = -1;  // nothing to judge without an adapter
            have = 0;
            streamStart = true;
            packetFill = 0;
            learning = false;  // a new adapter (or replug) is learned from scratch
            filterLearned = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!captureSinceMs.load()) {
            heardNonZero = false;
            const uint32_t t = millis();
            captureSinceMs = t ? t : 1;
        }
        have += builtIn ? pdmmic::read(raw + have, wlink::kBlockSamples - have, 20)
                        : usbmic::read(raw + have, wlink::kBlockSamples - have, 20);
        if (have < wlink::kBlockSamples) continue;
        have = 0;
        // UsbMic and pdmmic deliver left-aligned int32 (16-bit -> exact shift)
        for (int i = 0; i < wlink::kBlockSamples; ++i) block[i] = int16_t(raw[i] >> 16);
        notePeak(block);
        checkSignal(block);
        // High-pass before the noise filter, so its reduction is not spent on rumble.
        // The level meter and no-signal check above see the microphone as captured.
        if (const uint32_t v = highpassVersion.load(); v != highpassApplied) {
            highpassApplied = v;
            highpass.setHighPass(float(highpassHz.load()), float(wlink::kSampleRate));
            highpass.reset();
        }
        if (highpassOn.load()) highpass.process(block, wlink::kBlockSamples);
        if (noiseEngine) {
            const uint8_t mode = filterMode.load();
            const float wantFloor = mode == wlink::kFilterStrong ? -20.0f : -12.0f;
            const uint8_t adapt = adaptSpeed.load();
            nsig::Params params;
            params.gainFloorDb = wantFloor;
            params.subwindowSec = wlink::adaptSubwindowSec(adapt);       // 4 / 15 / 60 s
            params.learnSec = params.subwindowSec * 2 > 120 ? params.subwindowSec * 2 : 120;
            if (!learning) {
                nsig::init(noiseEngine, wlink::kSampleRate, params);
                learning = true;
                floorDb = wantFloor;
                appliedAdapt = adapt;
            } else if (wantFloor != floorDb || adapt != appliedAdapt) {
                nsig::setParams(noiseEngine, params);  // strength / speed only: keeps what it learned
                floorDb = wantFloor;
                appliedAdapt = adapt;
            }
            // Always processed (constant 10.7 ms delay); 'off' passes the input through.
            nsig::processInPlace(noiseEngine, block, wlink::kBlockSamples, mode != wlink::kFilterOff);
            filterLearned = nsig::signatureLearned(noiseEngine);
        }
        sendAudio(pkt, block, seq);
    }
}

// ---------------------------------------------------------------- status
static uint8_t adapterState(const usbmic::Status& s) {
    if (builtIn) return pdmmic::running() ? wlink::kAdapterStreaming : wlink::kAdapterUnusable;
    if (s.streaming) return wlink::kAdapterStreaming;
    if (s.connected && s.problem[0]) return wlink::kAdapterUnusable;  // plugged in, refused
    return wlink::kAdapterNone;
}

static void sendStatus() {
    const usbmic::Status s = usbmic::status();
    wlink::StatusPacket p = {};
    wlink::fill(p.h, wlink::kStatus, pairId);
    p.adapter = adapterState(s);
    p.rssi = rssi;
    p.vid = s.vid;
    p.pid = s.pid;
    p.channels = s.channels;
    p.bits = s.bits;
    p.usbPackets = s.packets;
    p.usbBadPackets = s.badPackets;
    p.usbOverflows = s.overflows;
    p.sent = sent;
    p.sendFailed = sendFailed + deliveryFailed;
    p.queueFull = sendFailed;
    p.noAck = deliveryFailed;
    p.uptimeMs = millis();
    strlcpy(p.name, s.name, sizeof(p.name));
    strlcpy(p.problem, s.problem, sizeof(p.problem));
    p.board = uint8_t(board::kind());
    if (builtIn) {
        p.vid = p.pid = 0;
        p.channels = 1;
        p.bits = 16;
        strlcpy(p.name, "built-in microphone (SPM1423)", sizeof(p.name));
        strlcpy(p.problem, pdmmic::running() ? "" : "built-in microphone did not start", sizeof(p.problem));
    }
    p.filterMode = filterMode.load();
    p.filterLearned = filterLearned.load() ? 1 : 0;
    p.adapt = adaptSpeed.load();
    strlcpy(p.firmware, kFirmware, sizeof(p.firmware));
    p.noSignal = noSignal.load() ? 1 : 0;
    p.txPowerDbm = txPowerDbm;
    p.cpuMhz = uint8_t(getCpuFrequencyMhz());
    p.tempDeciC = temperatureOn ? chipTemp : wlink::kTempUnknown;
    p.highpassOn = highpassOn.load();
    p.highpassHz = highpassHz.load();
    p.temperatureOn = temperatureOn;
    p.micLevel = micLevel;
    if (builtIn) {  // digital gain, 0..+30 dB
        p.hasLevel = 1;
        p.levelMin = 0;
        p.levelMax = int16_t(pdmmic::kMaxGainDb * 256);
        p.levelNow = pdmmic::gainDb256().load();
    } else {
        const usbmic::Status us = usbmic::status();
        p.hasLevel = us.hasVolume ? 1 : 0;
        p.levelMin = us.volMin;
        p.levelMax = us.volMax;
        p.levelNow = us.volCur;
    }
    const board::MemInfo m = board::memInfo();
    p.heapFree = m.free;
    p.heapMin = m.min;
    p.heapLargest = m.largest;
    const uint32_t q = radioQueued();
    p.radioQueued = uint16_t(q > 65535 ? 65535 : q);
    p.adapterInfoId = adapterInfoId;
    p.distance = distance;
    p.audioMode = audioMode.load();
    p.modeSetting = modeSetting;
    p.modeReason = modeReason.load();
    portENTER_CRITICAL(&linkMux);
    if (locked) memcpy(p.lockedTo, lockMac, 6);
    memcpy(p.rejectedMac, rejectedMac, 6);
    portEXIT_CRITICAL(&linkMux);
    p.rejected = rejected.load();
    if (speaker()) {
        const JitterBuffer::Stats js = speakerJb.stats();
        p.underrunsKnown = 1;
        p.spkUnderruns = js.underruns;
        p.spkLost = js.lostBlocks;
    }
    radioSend(localMac, &p, sizeof(p));
}

// Builds a new adapter record when a different adapter (or the same one again) has been
// opened and its outcome is known (streaming, or refused). Unplugging keeps the last one.
static void updateAdapterRecord() {
    if (builtIn) return;  // no USB adapter: status.txt describes the built-in microphone
    static uint32_t lastConnects = 0;
    static uint8_t lastState = 0;
    static char lastProblem[sizeof(usbmic::Status::problem)] = "";
    static uint16_t counter = 0;
    static const uint32_t bootTag = (esp_random() | 0x10000) & 0xFFFF0000;  // differs per start
    const usbmic::Status s = usbmic::status();
    const uint8_t state = adapterState(s);
    if (state == wlink::kAdapterNone) return;
    if (s.connects == lastConnects && state == lastState && strcmp(s.problem, lastProblem) == 0) return;
    static usbmic::Descriptors d;  // ~2.3 KB: not on the stack
    if (!usbmic::descriptors(d)) return;
    lastConnects = s.connects;
    lastState = state;
    strlcpy(lastProblem, s.problem, sizeof(lastProblem));
    adinfo::RecordHeader h = {};
    h.format = adinfo::kRecordFormat;
    h.state = state;
    h.speed = d.speed;
    h.usedInterface = d.usedInterface;
    h.usedAlt = d.usedAlt;
    h.usedEndpoint = d.usedEndpoint;
    h.hasVolume = s.hasVolume;
    h.volMin = s.volMin;
    h.volMax = s.volMax;
    h.volRes = s.volRes;
    h.volCur = s.volCur;
    h.channels = s.channels;
    h.bits = s.bits;
    h.configTotal = d.configTotal;
    const size_t room = sizeof(adapterRecord) - sizeof(h);
    h.configLen = uint16_t(d.configLen < room ? d.configLen : room);
    h.remoteUptimeS = uint32_t(esp_timer_get_time() / 1000000);
    strlcpy(h.firmware, kFirmware, sizeof(h.firmware));
    strlcpy(h.manufacturer, d.manufacturer, sizeof(h.manufacturer));
    strlcpy(h.product, d.product, sizeof(h.product));
    strlcpy(h.serial, d.serial, sizeof(h.serial));
    strlcpy(h.problem, s.problem, sizeof(h.problem));
    memcpy(h.device, d.device, sizeof(h.device));
    memcpy(adapterRecord, &h, sizeof(h));
    memcpy(adapterRecord + sizeof(h), d.config, h.configLen);
    adapterRecordLen = uint16_t(sizeof(h) + h.configLen);
    if (++counter == 0) counter = 1;
    adapterInfoId = bootTag | counter;
}

// The local board asked for the adapter details: send the whole record (a few packets).
static void sendAdapterRecord() {
    const uint32_t want = infoRequested.load();
    if (!want || !linked || radioPaused.load()) return;
    infoRequested = 0;
    if (want != adapterInfoId || !adapterRecordLen) return;  // outdated request: the next status has the new id
    static wlink::AdapterInfoPacket p;
    wlink::fill(p.h, wlink::kAdapterInfo, pairId);
    p.id = adapterInfoId;
    p.total = adapterRecordLen;
    for (uint16_t off = 0; off < adapterRecordLen; off += wlink::kInfoPiece) {
        p.offset = off;
        p.len = adapterRecordLen - off < wlink::kInfoPiece ? adapterRecordLen - off : wlink::kInfoPiece;
        memcpy(p.data, adapterRecord + off, p.len);
        radioSend(localMac, &p, offsetof(wlink::AdapterInfoPacket, data) + p.len);
    }
}

static void forwardLogs() {
    if (!linked || !logQueue) return;
    for (int i = 0; i < 1; ++i) {  // one per loop (~500/s max): audio has priority on air
        LogText t;
        if (xQueueReceive(logQueue, &t, 0) != pdTRUE) return;
        wlink::LogPacket p;
        wlink::fill(p.h, wlink::kLog, pairId);
        strlcpy(p.text, t.text, sizeof(p.text));
        radioSend(localMac, &p, sizeof(wlink::Header) + strlen(p.text) + 1);
    }
}

// While audio comes in, the LED is a level meter (instant rise, ~0.3 s fall),
// linked or not. Otherwise it shows the link / adapter state.
// Speaker mode: the USB task asks for each 1 ms frame of the adapter's playback stream.
static uint16_t playbackFill(int16_t* mono, uint16_t maxSamples) {
    if (maxSamples < JitterBuffer::kMaxFrame) return 0;
    const uint16_t n = speakerJb.frame(mono);
    uint16_t peak = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const int v = mono[i] < 0 ? -int(mono[i]) : mono[i];
        if (v > peak) peak = uint16_t(v > 32767 ? 32767 : v);
    }
    if (peak > blockPeak.load(std::memory_order_relaxed)) blockPeak.store(peak, std::memory_order_relaxed);
    return n;
}

static void updateLed() {
    led.feedPeak(blockPeak.exchange(0));
    if (bootButton.heldMs(millis()) >= 10000) { led.raw(false); return; }  // dark: release = factory reset

    const usbmic::Status s = usbmic::status();
    const uint8_t a = adapterState(s);
    const bool audio = a == wlink::kAdapterStreaming;
    if (audio && noSignal.load() && !speaker()) led.mode(board::LedMode::Problem);  // streaming, but only silence
    else if (audio) led.mode(linked ? board::LedMode::LevelGlow : board::LedMode::LevelFlash);
    else if (!linked) led.mode(board::LedMode::Searching);
    else if (a == wlink::kAdapterUnusable) led.mode(board::LedMode::Problem);
    else led.mode(board::LedMode::LinkedIdle);
    led.update();
}

// ---------------------------------------------------------------- settings
static const char* adaptText(uint8_t a) {
    return a == wlink::kAdaptSlow ? "slow (60 s)" : a == wlink::kAdaptFast ? "fast (4 s)" : "normal (15 s)";
}

static const char* filterText(uint8_t m) {
    return m == wlink::kFilterStrong ? "on, strong (20 dB)" : m == wlink::kFilterOff ? "off" : "on (12 dB)";
}

static void applyFilterSetting(bool announce) {
    const uint8_t mode = savedFilter;
    if (announce || mode != filterMode.load()) logf("noise filter: %s (setting from the local board)", filterText(mode));
    filterMode = mode;
}

static void restartNow(const char* why, int blinks) {
    logf("%s, restarting", why);
    const uint32_t t0 = millis();
    while (millis() - t0 < 400) { forwardLogs(); delay(5); }  // let the log line go out
    for (int i = 0; i < blinks; ++i) { led.raw(true); delay(150); led.raw(false); delay(150); }
    ESP.restart();
}

// BOOT held 10 s: forget the settings received from the local board (Mic 1, channel 11,
// filter on, normal adapt speed). The recovery if the remote ends up on a mic
// number or channel no local board uses.
static void runButton() {
    if (bootButton.update(millis(), digitalRead(board::buttonPin()) == LOW) == HoldButton::Action::Hold10) {
        settings.factoryReset();
        restartNow("factory reset (held BOOT 10 s)", 3);
    }
}

// Settings from the local board (config.txt). Filter and adapt speed apply at once;
// a new mic number or channel is saved and applied by restarting.
static void handleConfig() {
    if (!configPending) return;
    wlink::ConfigPacket c;
    portENTER_CRITICAL(&linkMux);
    memcpy(&c, &pendingConfig, sizeof(c));
    configPending = false;
    portEXIT_CRITICAL(&linkMux);

    const uint8_t filter = c.filterMode <= wlink::kFilterOff ? c.filterMode : wlink::kFilterOn;
    const uint8_t adapt = c.adapt <= wlink::kAdaptFast ? c.adapt : wlink::kAdaptNormal;
    if (filter != savedFilter || adapt != adaptSpeed.load()) {
        savedFilter = filter;
        settings.put("opt", filter);
        settings.put("adapt", adapt);
        if (adapt != adaptSpeed.load()) logf("noise filter adapt speed: %s (from the local board)", adaptText(adapt));
        adaptSpeed = adapt;
        applyFilterSetting(false);
    }
    const uint8_t txp = c.txPowerDbm >= wlink::kMinTxPowerDbm && c.txPowerDbm <= wlink::kMaxTxPowerDbm ? c.txPowerDbm
                                                                                                    : wlink::kMaxTxPowerDbm;
    if (txp != txPowerDbm) {
        txPowerDbm = txp;
        settings.put("txp", txp);
        board::setTxPower(txp);
        logf("radio transmit power: %u dBm%s (from the local board)", txp, txp == wlink::kMaxTxPowerDbm ? " (max)" : "");
    }
    const bool hpOn = c.highpassOn != 0;
    const uint16_t hpHz = hpf::hzValid(c.highpassHz) ? c.highpassHz : hpf::kDefaultHz;
    if (hpOn != highpassOn.load() || hpHz != highpassHz.load()) {
        highpassOn = hpOn;
        highpassHz = hpHz;
        highpassVersion++;
        settings.put("hp", hpOn ? 1 : 0);
        settings.put16("hpz", hpHz);
        logf("high-pass filter: %s, %u Hz (from the local board)", hpOn ? "on" : "off", unsigned(hpHz));
    }
    const uint8_t lvl = wlink::levelValid(c.micLevel) ? c.micLevel : wlink::kLevelDefault;
    if (lvl != micLevel) {
        micLevel = lvl;
        settings.put("lvl", lvl);
        if (builtIn) {
            pdmmic::setLevel(lvl);
            logf("mic level: %s, gain %.1f dB (built-in microphone, from the local board)",
                 lvl == wlink::kLevelDefault ? "default" : (String(lvl) + " %").c_str(), pdmmic::gainDb256().load() / 256.0f);
        } else {
            usbmic::setVolume(lvl);  // applied now if an adapter streams, else when one is plugged in
            if (lvl == wlink::kLevelDefault) logf("mic level: default (the adapter's own; it keeps the last level until unplugged)");
            else logf("mic level: %u %% of the adapter's range (from the local board)", unsigned(lvl));
        }
    }
    if ((c.temperatureOn != 0) != temperatureOn) {
        temperatureOn = c.temperatureOn != 0;
        settings.put("tmp", temperatureOn ? 1 : 0);
        logf("temperature reading: %s (from the local board)", temperatureOn ? "on, every 10 min with the radio paused" : "off");
    }
    const uint8_t dist = c.distance <= wlink::kDistanceFar ? c.distance : wlink::kDistanceNormal;
    if (dist != distance) {
        distance = dist;
        settings.put("dist", dist);
        if (linked) board::setPeerRate(localMac, dist);
        logf("distance: %s, radio rate %s (from the local board)", wlink::distanceName(dist), wlink::distanceRateText(dist));
    }
    // Pairing by MAC: lock to this local board, or (remote = any) unlock.
    if (c.lock && (!locked || memcmp(lockMac, localMac, 6) != 0)) {
        portENTER_CRITICAL(&linkMux);
        memcpy(lockMac, localMac, 6);
        portEXIT_CRITICAL(&linkMux);
        locked = true;
        settings.putBytes("lock", lockMac, 6);
        logf("locked to local %s: other local boards are ignored (BOOT 10 s unlocks)", board::macText(lockMac).c_str());
    } else if (!c.lock && locked) {
        locked = false;
        settings.remove("lock");
        logf("unlocked: links to any local board on mic %u again (config.txt remote = any)", board::micNumber(pairId));
    }
    bool modeRestart = false;
    const uint8_t mset = c.modeSetting <= wlink::kModeSetSpeaker ? c.modeSetting : wlink::kModeAuto;
    if (mset != modeSetting) {
        static const char* const kSet[] = {"auto", "microphone", "speaker"};
        settings.put("mode", mset);
        if (builtIn) {
            logf("mode setting: %s; stays microphone (remote has built-in mic only)", kSet[mset]);
        } else {
            logf("mode: %s -> %s (from the local board)", kSet[modeSetting], kSet[mset]);
            modeRestart = true;
        }
        modeSetting = mset;
    }
    const uint8_t cpu = wlink::cpuMhzValid(c.cpuMhz) ? c.cpuMhz : wlink::kCpuMhzDefault;
    if (cpu != cpuMhz) {
        cpuMhz = cpu;
        settings.put("cpu", cpu);
        board::setCpuMhz(cpu);
        logf("processor speed: %u MHz (from the local board)", unsigned(getCpuFrequencyMhz()));
    }
    bool restart = false;
    if (c.newPairId < wlink::kPairIds && c.newPairId != pairId) {
        settings.put("pair", c.newPairId);
        logf("mic number: %u -> %u (from the local board)", board::micNumber(pairId), board::micNumber(c.newPairId));
        restart = true;
    }
    if (c.newChannel >= wlink::kMinChannel && c.newChannel <= wlink::kMaxChannel && c.newChannel != channel) {
        settings.put("chan", c.newChannel);
        logf("radio channel: %u -> %u (from the local board)", channel, c.newChannel);
        restart = true;
    }
    wlink::ConfigAckPacket a = {};
    wlink::fill(a.h, wlink::kConfigAck, pairId);
    a.seq = c.seq;
    a.pairId = settings.get("pair", 0);  // what it will use after a restart
    a.channel = settings.get("chan", wlink::kDefaultChannel);
    a.filterMode = filterMode.load();
    a.adapt = adaptSpeed.load();
    a.restarting = restart ? 1 : 0;
    a.txPowerDbm = txPowerDbm;
    a.cpuMhz = uint8_t(getCpuFrequencyMhz());
    a.highpassOn = highpassOn.load();
    a.highpassHz = highpassHz.load();
    a.temperatureOn = temperatureOn;
    a.micLevel = micLevel;
    a.distance = distance;
    a.modeSetting = modeSetting;
    a.locked = locked ? 1 : 0;
    for (int i = 0; i < (restart ? 3 : 1); ++i) {  // a restart is final: say it three times
        radioSend(localMac, &a, sizeof(a));
        delay(restart ? 20 : 0);
    }
    if (restart) restartNow("new mic number / channel from the local board", 0);
    if (modeRestart) restartNow("new mode from the local board", 0);
}

// ---------------------------------------------------------------- mode
// config.txt mode: microphone / speaker, or auto (ModeDecision.h: decided once per adapter
// plug-in, never mid-run). The USB host opens the adapter's matching side.
static modesel::State modeState;
static void decideMode() {
    if (builtIn) {  // AtomS3U: always a microphone, whatever config.txt says
        if (modeReason.load() != wlink::kReasonBuiltInMic) {
            audioMode = wlink::kModeMicrophone;
            modeReason = wlink::kReasonBuiltInMic;
            logf("mode: microphone (remote has built-in mic only)");
        }
        return;
    }
    const usbmic::Status s = usbmic::status();
    modesel::Input in;
    in.connected = s.connected;
    in.canCapture = s.canCapture;
    in.canPlayback = s.canPlayback;
    in.streaming = s.streaming;
    in.plugIns = s.plugIns;
    in.captureSinceMs = captureSinceMs.load();
    in.heardNonZero = heardNonZero.load();
    in.nowMs = millis();
    const uint8_t was = audioMode.load();
    const modesel::Action a = modesel::step(modeState, in, modeSetting);
    audioMode = modeState.mode;
    modeReason = modeState.reason;
    if (a == modesel::Action::None) return;
    if (a == modesel::Action::ListenAgain) logf("mode: a new adapter - listening on its microphone side first (auto)");
    if (a == modesel::Action::Decided) {
        static const char* const kWhy[] = {"", "config.txt", "the adapter only records", "the adapter only plays",
                                           "microphone signal at start-up", "no microphone signal at start-up"};
        const uint8_t r = modeState.reason;
        logf("mode: %s (%s)%s", speaker() ? "speaker" : "microphone", kWhy[r < 6 ? r : 0],
             r == wlink::kReasonSilence ? " - plug the mic in and restart the remote, or set mode = microphone" : "");
    }
    if (audioMode.load() != was) {
        speakerJb.requestFlush();
        usbmic::setDirection(speaker() ? usbmic::Direction::Playback : usbmic::Direction::Capture);
    }
}

// Another local board said hello on our mic number: locked out, or we are already linked.
// Logged as a WARNING (at most every 10 min), so it reaches the local board's events.txt.
static void reportRejected() {
    static uint32_t seen = 0, lastLog = 0;
    const uint32_t n = rejected.load();
    if (n == seen || (lastLog && millis() - lastLog < 600000)) return;
    seen = n;
    lastLog = millis();
    uint8_t mac[6];
    portENTER_CRITICAL(&linkMux);
    memcpy(mac, rejectedMac, 6);
    portEXIT_CRITICAL(&linkMux);
    if (locked)
        logf("WARNING: local board %s tried to link on mic %u: ignored, this remote is locked to %s", board::macText(mac).c_str(),
             board::micNumber(pairId), board::macText(lockMac).c_str());
    else
        logf("WARNING: a second local board %s is on mic %u / channel %u (another extender nearby?) - "
             "set remote = <MAC> in config.txt to lock the pair", board::macText(mac).c_str(), board::micNumber(pairId), channel);
}

// The local board is opening its EXTENDER drive and wants the noise graph (noise.svg).
static void sendNoise() {
    if (!noiseRequested.exchange(false) || !linked || radioPaused.load()) return;
    static wlink::NoisePacket p;
    memset(&p, 0, sizeof(p));
    wlink::fill(p.h, wlink::kNoiseData, pairId);
    p.filterMode = filterMode.load();
    const bool valid = noiseEngine && !speaker() && micLive() && filterLearned.load() && noiseEngine->sigValid;
    p.valid = valid ? 1 : 0;
    if (valid) {
        const double ref = nsig::fullScaleRefLog2();
        for (int k = 0; k < wlink::kNoiseBins && k < nsig::kBins; ++k) {
            const double db = (noiseEngine->sig[k] / 256.0 - ref) * 3.0102999566;
            const long d = lround(db * 10.0);
            p.deciDb[k] = int16_t(d < -32000 ? -32000 : d > 32000 ? 32000 : d);
        }
        p.learnedS = uint32_t(uint64_t(noiseEngine->sigFrames) * nsig::kH / wlink::kSampleRate);
    } else {
        for (int k = 0; k < wlink::kNoiseBins; ++k) p.deciDb[k] = wlink::kNoiseDbNone;
    }
    radioSend(localMac, &p, sizeof(p));
}

// ---------------------------------------------------------------- setup / loop
void setup() {
    Serial0.begin(115200);
    builtIn = board::atomS3U();
    led.begin();
    pinMode(board::buttonPin(), INPUT_PULLUP);
    settings.begin();
    pairId = settings.get("pair", 0) < wlink::kPairIds ? settings.get("pair", 0) : 0;
    channel = settings.get("chan", wlink::kDefaultChannel);
    if (channel < wlink::kMinChannel || channel > wlink::kMaxChannel) channel = wlink::kDefaultChannel;
    adaptSpeed = settings.get("adapt", wlink::kAdaptNormal) <= wlink::kAdaptFast ? settings.get("adapt", 0) : 0;
    savedFilter = settings.get("opt", wlink::kFilterOn) <= wlink::kFilterOff ? settings.get("opt", 0) : wlink::kFilterOn;
    highpassOn = settings.get("hp", 1) != 0;
    highpassHz = hpf::hzValid(settings.get16("hpz", hpf::kDefaultHz)) ? settings.get16("hpz", hpf::kDefaultHz) : hpf::kDefaultHz;
    temperatureOn = settings.get("tmp", 1) == 1;  // default on (one paused read every 10 min)
    micLevel = wlink::levelValid(settings.get("lvl", wlink::kLevelDefault)) ? settings.get("lvl", wlink::kLevelDefault)
                                                                            : wlink::kLevelDefault;
    distance = settings.get("dist", wlink::kDistanceNormal);
    if (distance > wlink::kDistanceFar) distance = wlink::kDistanceNormal;
    modeSetting = settings.get("mode", wlink::kModeAuto);
    if (modeSetting > wlink::kModeSetSpeaker) modeSetting = wlink::kModeAuto;
    locked = settings.getBytes("lock", lockMac, sizeof(lockMac)) == 6 && !wlink::macIsZero(lockMac);
    cpuMhz = settings.get("cpu", wlink::kCpuMhzDefault);
    if (!wlink::cpuMhzValid(cpuMhz)) cpuMhz = wlink::kCpuMhzDefault;
    board::setCpuMhz(cpuMhz);
    led.blinkMicNumber(board::micNumber(pairId));
    logQueue = xQueueCreate(24, sizeof(LogText));
    idfVprintf = esp_log_set_vprintf(forwardIdfLog);

    const bool radioOk = board::startRadio(channel, onRecv, onSent);
    txPowerDbm = settings.get("txp", wlink::kMaxTxPowerDbm);
    if (txPowerDbm < wlink::kMinTxPowerDbm || txPowerDbm > wlink::kMaxTxPowerDbm) txPowerDbm = wlink::kMaxTxPowerDbm;
    board::setTxPower(txPowerDbm);
    board::addPeer(kBroadcast, false);
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    static const char* const kSet[] = {"auto", "microphone", "speaker"};
    logf("Wireless USB Audio Extender remote %s | %s | MAC %s", kFirmware, board::kindName(), board::macText(mac).c_str());
    logf("settings: mic %u | channel %u | %u MHz | high-pass %s | distance %s (%s) | mode %s",
         board::micNumber(pairId), channel, unsigned(getCpuFrequencyMhz()),
         highpassOn.load() ? (String(highpassHz.load()) + " Hz").c_str() : "off", wlink::distanceName(distance),
         wlink::distanceRateText(distance), builtIn ? "microphone (built-in mic)" : kSet[modeSetting]);
    if (locked) logf("locked to local %s (config.txt remote = MAC; BOOT 10 s unlocks)", board::macText(lockMac).c_str());
    if (!radioOk) logf("ERROR: ESP-NOW v2 start failed (needs Arduino-ESP32 core 3.2 or newer)");
    if (dropNote.magic == kNoteNoLink) {
        drop = dropNote;
        if (drop.why == kWhyTxStuck)
            logf("restarted itself: its sends had stopped getting through while it still heard the local board "
                 "(stuck transmitter)");
        else if (drop.why == kWhyRadioStuck)
            logf("restarted itself: its radio had refused to send for %.1f s (stuck radio)",
                 wlink::kRemoteStuckBeacons * wlink::kBeaconMs / 1000.0f);
        else
            logf("restarted itself: the link had been down for %lu s", (unsigned long)(wlink::kRemoteRestartMs / 1000));
        reportDrop("before the restart");
    } else if (const char* why = board::unusualResetText()) {
        logf("%s", why);
    }
    dropNote.magic = 0;
    noiseEngine = (nsig::Engine*)heap_caps_malloc(sizeof(nsig::Engine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!noiseEngine) logf("ERROR: no RAM for the noise filter (%u bytes); audio passes unfiltered", (unsigned)sizeof(nsig::Engine));
    applyFilterSetting(true);
    static_assert(wlink::kLevelDefault == usbmic::kVolumeDeviceDefault, "same 'leave it' value");
    // Mode: a setting is final at once; auto opens the adapter's microphone side first and
    // decides when it is plugged in (decideMode()).
    if (builtIn) {
        audioMode = wlink::kModeMicrophone;  // decideMode() reports it
    } else if (modeSetting != wlink::kModeAuto) {  // final at once (decideMode() logs it)
        modeState.mode = modeSetting == wlink::kModeSetSpeaker ? wlink::kModeSpeaker : wlink::kModeMicrophone;
        audioMode = modeState.mode;
    }
    if (builtIn) {
        if (pdmmic::start(micLevel))
            logf("built-in microphone: 48 kHz, gain %.1f dB (mic_level %s)", pdmmic::gainDb256().load() / 256.0f,
                 micLevel == wlink::kLevelDefault ? "default" : (String(micLevel) + " %").c_str());
        else
            logf("ERROR: the built-in microphone did not start");
    } else {
        usbmic::setPlaybackSource(playbackFill);
        if (!usbmic::start(micLevel, speaker() ? usbmic::Direction::Playback : usbmic::Direction::Capture))
            logf("ERROR: USB host did not start");
    }
    // Core 1, above loop(): the audio path never waits for logging or status.
    xTaskCreatePinnedToCore(audioTask, "audio_tx", 4096, nullptr, 5, nullptr, 1);
    enableLoopWDT();  // a stopped loop() (no beacons, no status) restarts the board after 5 s
}

// Temperature read with the radio paused, so the radio and our code never use the sensor
// at the same moment; starting the radio again also re-runs its own calibration. ESP-NOW
// is shut down first and started again afterwards (ESP-IDF's recommended order), with the
// broadcast peer and, if linked, the local board restored. Costs about 0.1-0.3 s of audio.
static void stopRadio() {
    radioPaused = true;  // the audio task skips its sends meanwhile
    esp_now_deinit();
    esp_wifi_stop();
}

static void startRadioAgain() {
    reports = submitted.load();  // reports for packets queued before the stop never come
    esp_wifi_start();
    board::startRadio(channel, onRecv, onSent);
    board::setTxPower(txPowerDbm);
    board::addPeer(kBroadcast, false);
    if (linked) board::addPeer(localMac, true, distance);
    radioPaused = false;
}

static int16_t readTempRadioPaused() {
    stopRadio();
    const int16_t t = board::readChipTempOnce();
    startRadioAgain();
    return t;
}

static const char* errName(int32_t e) { return e ? esp_err_to_name(esp_err_t(e)) : "none"; }

// One line describing the radio around the drop (see DropInfo).
// What the radio and memory looked like at the moment of a drop (see DropInfo).
static void snapshotDrop(uint32_t now) {
    drop = DropInfo{};
    drop.sinceRxMs = now - lastRxMs;
    drop.sinceSendOkMs = lastSendOkMs ? now - lastSendOkMs : 0;
    drop.errStreak = errStreak;
    drop.lastErr = lastSendErr;
    drop.ackStreak = ackStreak;
    const board::MemInfo m = board::memInfo();
    drop.heapFree = m.free;
    drop.heapLargest = m.largest;
    drop.heapMin = m.min;
    drop.radioQueued = radioQueued();
    drop.sinceReportMs = lastReportMs ? now - lastReportMs : 0;
}

static void reportDrop(const char* when) {
    logf("link-loss details (%s): last packet from the local board %lu ms before, last acknowledged send %lu ms before",
         when, (unsigned long)drop.sinceRxMs, (unsigned long)drop.sinceSendOkMs);
    logf("link-loss details: audio sends failed in a row %lu (last error %s), unacknowledged in a row %lu",
         (unsigned long)drop.errStreak, errName(drop.lastErr), (unsigned long)drop.ackStreak);
    logf("link-loss details: memory %lu bytes free (largest block %lu, lowest since start %lu), packets queued in the "
         "radio %lu, last send report %lu ms before", (unsigned long)drop.heapFree, (unsigned long)drop.heapLargest,
         (unsigned long)drop.heapMin, (unsigned long)drop.radioQueued, (unsigned long)drop.sinceReportMs);
    logf("while searching: heard %lu packets from the local board, %lu beacons could not be sent (first error %s)",
         (unsigned long)drop.searchRx, (unsigned long)drop.beaconErr, errName(drop.beaconErrCode));
}

void loop() {
    const uint32_t now = millis();
    static uint32_t lastBeacon = 0, lastStatus = 0, unlinkedSince = 0, nextTempRead = 30000;
    static bool everLinked = false, searching = false;
    static uint32_t beaconFailRun = 0;  // beacons in a row the radio refused to send
    static uint32_t linkedSinceMs = 0;

    if (!builtIn) usbmic::pollLog(logLineFromUsb);
    static bool slotLogged = false;
    if (builtIn && !slotLogged && pdmmic::slotDecided()) {
        slotLogged = true;
        logf("built-in microphone: signal on PDM slot %s", pdmmic::slotInUse() ? "right" : "left");
    }
    reportUsbHost();
    runButton();
    handleConfig();
    if (const int8_t c = noSignalChange.exchange(0)) {
        if (c > 0) logf("adapter is sending silence: no microphone signal for 20 s - check the microphone is in the "
                        "adapter's MIC jack and the plug type matches (a 4-pole TRRS plug in a 3-pole TRS mic jack is silent)");
        else logf("microphone signal is back");
    }

    if (candidatePending && !linked) {
        uint8_t mac[6];
        portENTER_CRITICAL(&linkMux);
        memcpy(mac, candidateMac, 6);
        candidatePending = false;
        portEXIT_CRITICAL(&linkMux);
        if (board::addPeer(mac, true, distance)) {
            memcpy(localMac, mac, 6);
            lastRxMs = now;
            lastSendOkMs = now;
            linkedSinceMs = now;
            errStreak = 0;
            ackStreak = 0;
            streamStart = true;
            linked = true;
            everLinked = true;
            logf("linked to local %s", board::macText(mac).c_str());
            if (searching) {
                searching = false;
                drop.searchRx = searchRx;
                logf("link back after %.1f s", (now - unlinkedSince) / 1000.0f);
                reportDrop("this drop");
            }
            lastStatus = 0;  // report the adapter right away
        }
    }
    // Stuck transmitter: sends stopped succeeding while the local board is still heard.
    if (linked && now - linkedSinceMs > wlink::kTxStuckArmMs && !radioPaused.load() &&
        int32_t(now - lastSendOkMs.load()) > int32_t(wlink::kTxStuckMs) &&
        errStreak.load() + ackStreak.load() >= wlink::kTxStuckFails &&
        int32_t(now - lastRxMs) < int32_t(wlink::kTxStuckMs)) {
        snapshotDrop(now);
        dropNote = drop;
        dropNote.why = kWhyTxStuck;
        dropNote.magic = kNoteNoLink;
        restartNow("sends stopped getting through while the local board is still heard (stuck transmitter)", 0);
    }
    if (linked && int32_t(millis() - lastRxMs) > int32_t(wlink::kLinkTimeoutMs)) {
        linked = false;
        esp_now_del_peer(localMac);
        snapshotDrop(now);
        searchRx = 0;
        beaconFailRun = 0;
        logf("link lost (no packets from the local board for 2 s); searching");
        unlinkedSince = now;
        searching = true;
    }
    // A link that had worked and stays down. A stuck radio shows as beacons the radio
    // refuses to send (a healthy radio always takes a broadcast, local board or not):
    // after kRemoteStuckBeacons of those in a row (2 s), restart the remote at once.
    // Restarting only the radio did not clear it (1.2.11 logs). Otherwise restart after
    // 20 s anyway (clears a stuck USB host too). Not after a boot without any link
    // (local board off or out of range).
    if (!linked && everLinked) {
        const int32_t down = int32_t(now - unlinkedSince);
        drop.searchRx = searchRx;
        if (beaconFailRun >= wlink::kRemoteStuckBeacons) {
            dropNote = drop;
            dropNote.why = kWhyRadioStuck;
            dropNote.magic = kNoteNoLink;
            restartNow("radio refuses to send (stuck radio)", 0);
        }
        if (down > int32_t(wlink::kRemoteRestartMs)) {
            dropNote = drop;
            dropNote.why = kWhyNoLink;
            dropNote.magic = kNoteNoLink;
            restartNow("no link for 20 s", 0);
        }
    }
    if (!linked && now - lastBeacon >= wlink::kBeaconMs) {
        lastBeacon = now;
        wlink::BeaconPacket b = {};
        wlink::fill(b.h, wlink::kBeacon, pairId);
        if (locked) memcpy(b.lockedTo, lockMac, 6);
        const esp_err_t err = radioSend(kBroadcast, &b, sizeof(b));
        if (err == ESP_OK) {
            beaconFailRun = 0;
        } else if (everLinked) {
            if (!drop.beaconErr) drop.beaconErrCode = err;
            drop.beaconErr++;
            beaconFailRun++;
        }
    }
    if (linked && now - lastStatus >= wlink::kStatusMs) {
        lastStatus = now;
        updateAdapterRecord();
        sendStatus();
    }
    // Speaker mode sends no audio: keep the local board hearing us (its link timeout is 2 s).
    static uint32_t lastPing = 0;
    if (linked && speaker() && now - lastPing >= wlink::kPingMs && !radioPaused.load()) {
        lastPing = now;
        wlink::PingPacket pg;
        wlink::fill(pg.h, wlink::kPing, pairId);
        pg.uptimeMs = now;
        radioSend(localMac, &pg, sizeof(pg));
    }
    sendAdapterRecord();
    sendNoise();
    decideMode();
    reportRejected();
    // Temperature: one short read 30 s after start or after switching on (not while the
    // radio calibrates at start-up), then every 10 minutes.
    if (!temperatureOn) {
        chipTemp = wlink::kTempUnknown;
        nextTempRead = now + 30000;
    } else if (int32_t(now - nextTempRead) >= 0) {
        chipTemp = readTempRadioPaused();
        nextTempRead = now + board::kTempReadMs;
    }
    forwardLogs();
    updateLed();
    delay(2);
}
