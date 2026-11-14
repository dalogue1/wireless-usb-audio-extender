// Wireless USB Audio Extender - LOCAL (plugs into the PC)
// One firmware for the Seeed XIAO ESP32-S3 and the M5Stack AtomS3U (board::kind()), and for
// either kind of remote (the remote reports its board in its status).
//
// Microphone mode: receives audio from its paired remote over ESP-NOW and presents
// it to the PC as a USB microphone ("Extender Mic 1".."9", 48 kHz mono 16-bit).
// Speaker mode: presents a stereo USB speaker ("Extender Speaker N") and sends what the PC
// plays to the remote (IMA ADPCM), which plays it through its adapter (config.txt mode).
// Both: a USB serial port with the logs of both boards.
//
// Board: Seeed XIAO ESP32-S3 with its antenna attached.
// Arduino IDE (esp32 core 3.3.x) Tools menu:
//   Board:              XIAO_ESP32S3
//   USB Mode:           USB-OTG (TinyUSB)
//   USB CDC On Boot:    Disabled      (this sketch starts USB itself)
//   Upload Mode:        UART0 / Hardware CDC
// Settings: config.txt on the EXTENDER drive (BOOT click opens it); the remote follows.

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Build for the XIAO ESP32-S3"
#endif
#if ARDUINO_USB_MODE
#error "Local: set Tools > USB Mode to 'USB-OTG (TinyUSB)'"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "Local: set Tools > USB CDC On Boot to 'Disabled' (the sketch names the USB mic before starting USB)"
#endif

#include <atomic>
#include "USB.h"
#include "USBCDC.h"
#include "BoardCommon.h"
#include "JitterBuffer.h"
#include "LinkProtocol.h"
#include "UsbMicDevice.h"
#include "UsbSpeakerDevice.h"
#include "Adpcm.h"
#include "Announcer.h"
#include "VoiceMixer.h"
#include "VoiceDrive.h"
#include "ConfigFile.h"
#include "AdapterInfo.h"
#include "UsbWatch.h"
#include "EventLog.h"
#include "NoiseSvg.h"
#include "ReportHtml.h"
#include <FFat.h>
#include "ff.h"
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>

static const char* kFirmware = "1.4.2-dev";

// Time since start from the 64-bit microsecond timer: unlike millis() it does not wrap
// after 49.7 days, so times in the logs, events.txt and status.txt stay right.
static inline uint64_t upMs() { return uint64_t(esp_timer_get_time() / 1000); }
static inline uint32_t upS() { return uint32_t(esp_timer_get_time() / 1000000); }

USBCDC LogPort;
static board::StatusLed led;
static JitterBuffer jitter;
static void healthSnapshot(bool forEvent);  // event log health records (below)
static bool pcUsing();                      // the PC has the audio stream open (below)

static uint8_t pairId = 0;
static uint8_t channel = 1;
// The USB device this start presents: microphone or speaker (config.txt mode; with auto,
// the remote's last decision, saved as "amode"). A change restarts the board.
static uint8_t audioMode = wlink::kModeMicrophone;
static bool speakerMode() { return audioMode == wlink::kModeSpeaker; }
static uint8_t myMac[6] = {0};

// Settings: config.txt on the EXTENDER drive.
static board::SavedSettings settings;
static HoldButton bootButton;
static cfg::Settings config;        // as read from config.txt
static char configProblem[96] = "";
static uint8_t wantPair = 0, wantChannel = wlink::kDefaultChannel;  // what config.txt asks for
static bool handoverPending = false;
static uint32_t handoverSince = 0;
// While a handover waits and the link is down, the local board also listens for the remote
// on the NEW mic number / channel (it may already be there: a BOOT reset, an interrupted
// handover). It moves only when it hears the remote there; it never moves without it.
static volatile bool probing = false;      // radio briefly on another channel (see probeKind)
static volatile bool heardOnNew = false;   // a beacon from the remote on the new setting
// A LOCKED local board (config.txt remote = <MAC>) whose link stays down also listens on
// the factory setting (mic 1 / channel 11) for ITS remote: a remote reset with BOOT 10 s
// goes back there. Heard there: move to it, then hand it back to the configured setting.
enum : uint8_t { kProbeNone = 0, kProbeHandover = 1, kProbeFactory = 2 };
static volatile uint8_t probeKind = kProbeNone;
static volatile bool heardOnFactory = false;
static uint16_t configSeq = 0;
static wlink::ConfigAckPacket lastAck;
static volatile bool ackPending = false;
static bool driveMode = false;  // booted as the voice-files USB drive
static bool driveOpening = false;  // BOOT clicked: waiting for the remote, then writing the drive's files
static VoiceMixer voiceMixer;

// Link state. The receive callback runs in the Wi-Fi task; loop() owns peers.
static portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t remoteMac[6];
static uint8_t candidateMac[6];
static volatile bool linked = false;
static volatile bool candidatePending = false;
static volatile bool helloAgain = false;
static volatile uint32_t lastRxMs = 0;
static volatile uint32_t lastAudioMs = 0;
static volatile int8_t rssi = 0;
static std::atomic<uint16_t> blockPeak{0};  // loudest received sample since the LED last looked

static wlink::StatusPacket remoteStatus;
static volatile bool haveStatus = false;
static std::atomic<uint32_t> statusCount{0};  // status packets received (the drive waits for a fresh one)
// Other boards on our mic number (always warned about: status.txt, log, events.txt).
static std::atomic<uint32_t> otherRemotes{0};  // beacons from a remote that is not ours
static uint8_t otherRemoteMac[6] = {0};         // the last of them (under linkMux)
// The remote's noise filter signature, asked for when the drive opens (noise.svg).
static wlink::NoisePacket noise;
static volatile bool haveNoise = false;
static QueueHandle_t remoteLogs = nullptr;
struct LogText { char text[sizeof(wlink::LogPacket::text)]; };

// The remote's latest adapter details (AdapterInfo.h). Pieces arrive in the Wi-Fi task
// (infoBuf, under linkMux); loop() takes the finished record, keeps it in flash and
// writes it out as adapter.txt when the drive opens.
static uint8_t infoBuf[wlink::kInfoMax];
static uint32_t infoBufId = 0;
static uint16_t infoBufTotal = 0;
static uint8_t infoBufPieces = 0;            // bit per kInfoPiece received
static volatile bool infoComplete = false;
static uint8_t adapterRec[wlink::kInfoMax];  // the latest record (loop() only)
static uint16_t adapterRecLen = 0;
static volatile uint32_t adapterRecId = 0;   // 0: from flash (or none) - not received since start
static uint32_t adapterRecS = 0;

// ---------------------------------------------------------------- logging
// Every line (local and remote) is also kept in a small history that is
// replayed when a serial monitor connects, so the first seconds after boot or
// upload are not lost while the monitor is still opening the port.
static constexpr int kHistoryLines = 64;
static char history[kHistoryLines][200];
static int historyNext = 0, historyCount = 0;
static volatile bool replayRequested = false;

// The last lines before a restart survive it (RTC memory keeps them through a crash,
// watchdog or firmware restart, not a power cut) and are printed after the next start.
struct LastLines {
    static constexpr uint32_t kMagic = 0x4C415354;  // "LAST"
    static constexpr int kLines = 16;
    uint32_t magic;
    uint16_t next, count;
    char line[kLines][128];
};
RTC_NOINIT_ATTR static LastLines lastLines;
static LastLines prevLines;      // copy taken at start, before anything is logged
static bool havePrevLines = false;

static void emit(const char* line) {  // loop()/setup() only
    strlcpy(lastLines.line[lastLines.next], line, sizeof(lastLines.line[0]));
    lastLines.next = (lastLines.next + 1) % LastLines::kLines;
    if (lastLines.count < LastLines::kLines) lastLines.count++;
    strlcpy(history[historyNext], line, sizeof(history[0]));
    historyNext = (historyNext + 1) % kHistoryLines;
    if (historyCount < kHistoryLines) historyCount++;
    LogPort.print(line);
    LogPort.print("\r\n");
}

static void onSerialEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id == ARDUINO_USB_CDC_CONNECTED_EVENT) replayRequested = true;  // a monitor opened the port
}

static char remoteFirmware[sizeof(wlink::StatusPacket::firmware)] = "";  // "" until the remote reports
static const char* remoteFirmwareText() { return remoteFirmware[0] ? remoteFirmware : "not heard yet"; }

static void replayHistory() {
    LogPort.printf("---- log since boot (last %d lines) | firmware: local %s, remote %s | up %lu s ----\r\n",
                   historyCount, kFirmware, remoteFirmwareText(), (unsigned long)upS());
    const int first = (historyNext - historyCount + kHistoryLines) % kHistoryLines;
    for (int i = 0; i < historyCount; ++i) {
        LogPort.print(history[(first + i) % kHistoryLines]);
        LogPort.print("\r\n");
    }
    LogPort.print("---- live ----\r\n");
}

static void logf(const char* fmt, ...) {
    char body[180], line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "%7.1f [local] %s", upMs() / 1000.0, body);
    emit(line);
}

// Noteworthy events (link, adapter, filter, handovers, announcements) go to the log.
// (Until 1.3.5 also to a RAM list in status.txt; events.txt is the record now.)
static void logEvent(const char* fmt, ...) {  // loop()/setup() only
    char body[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    logf("%s", body);
}

// ---------------------------------------------------------------- event log (flash)
// Critical events in their own flash partition ("evlog"), kept through restarts and
// power cuts; written out as events.txt when the drive opens. See EventLog.h.
struct PartitionFlash : evlog::Flash {
    const esp_partition_t* p = nullptr;
    uint32_t size() override { return p ? p->size : 0; }
    bool read(uint32_t o, void* b, uint32_t n) override { return esp_partition_read(p, o, b, n) == ESP_OK; }
    bool write(uint32_t o, const void* b, uint32_t n) override { return esp_partition_write(p, o, b, n) == ESP_OK; }
    bool erase(uint32_t o, uint32_t n) override { return esp_partition_erase_range(p, o, n) == ESP_OK; }
};
static PartitionFlash evFlash;
static evlog::Log eventLog;

static void ev(uint8_t code, uint8_t a8 = 0, uint16_t a16 = 0, uint32_t a32 = 0) {
    eventLog.add(code, upS(), a8, a16, a32);
}
static void evText(uint16_t source, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void evText(uint16_t source, const char* fmt, ...) {
    char b[evlog::kMaxText + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    eventLog.addText(upS(), b, source);
}
static uint32_t firmwareCode() {  // "1.3.2-dev" -> 0x010302
    unsigned a = 0, b = 0, c = 0;
    sscanf(kFirmware, "%u.%u.%u", &a, &b, &c);
    return (a & 0xFF) << 16 | (b & 0xFF) << 8 | (c & 0xFF);
}
static void startEventLog(bool asDrive) {
    evFlash.p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, esp_partition_subtype_t(0x40), "evlog");
    if (evFlash.p && eventLog.begin(&evFlash)) ev(evlog::kStart, uint8_t(esp_reset_reason()), asDrive ? 1 : 0, firmwareCode());
}
// Remote log lines worth keeping: restarts and their reasons (all four link-loss lines),
// errors, warnings, mode and lock changes.
// A critical remote line waiting for its "... " continuation pieces (they follow within ms).
static char remotePending[evlog::kMaxText + 1];
static uint32_t remotePendingAt = 0, remotePendingMs = 0;
static void flushRemotePending() {
    if (!remotePending[0]) return;
    eventLog.addText(remotePendingAt, remotePending, 1);
    remotePending[0] = 0;
}

static bool remoteLineIsCritical(const char* t) {
    static const char* const kWords[] = {"restart", "ERROR", "WARNING", "stuck", "crash", "watchdog", "brownout",
                                         "Audio Extender remote", "remote settings:", "link-loss", "while searching", "mode:",
                                         "locked to", "unlocked"};
    if (strncmp(t, "settings: ", 10) == 0) return true;  // the remote's start-up settings line
    for (const char* w : kWords)
        if (strstr(t, w)) return true;
    return false;
}

// ---------------------------------------------------------------- USB robustness
// Logs every change of the USB connection to the PC and recovers by itself where it
// safely can (re-attach, or a restart): see UsbWatch.h.
static void restartNow(const char* why, int blinks);
static std::atomic<uint32_t> usbEvents[4];  // ARDUINO_USB_STARTED / STOPPED / SUSPEND / RESUME
static uint32_t usbReattaches = 0, usbStalls = 0;
static char usbLastEvent[96] = "none yet";
static uint32_t usbLastEventS = 0;

static void onUsbEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id < 0 || id > ARDUINO_USB_RESUME_EVENT) return;
    // Set up again: the PC resets every interface to its idle setting, so a stream left
    // "open" from before (bus reset, re-attach, unplug) is stale.
    if (id == ARDUINO_USB_STARTED_EVENT || id == ARDUINO_USB_STOPPED_EVENT) usbmicdev::streamReset();
    usbEvents[id]++;
}

static void usbNote(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(usbLastEvent, sizeof(usbLastEvent), fmt, ap);
    va_end(ap);
    usbLastEventS = upS();
    logEvent("USB: %s", usbLastEvent);
}

static void usbReattach(const char* why) {
    usbReattaches++;
    usbNote("%s: re-attaching (soft unplug)", why);
    usbmicdev::reattach(300);
}

static UsbWatch usbWatcher;

static void usbWatch(uint32_t now) {
    static uint32_t seen[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        const uint32_t n = usbEvents[i].load();
        if (n == seen[i]) continue;
        const uint32_t more = n - seen[i];
        seen[i] = n;
        static const char* const kText[] = {"the PC set the device up", "the PC released the device (unplug, reset or re-attach)",
                                            "the bus is suspended (PC asleep, or its port stopped)", "the bus resumed"};
        if (more > 1) usbNote("%s (%lu times)", kText[i], (unsigned long)more);
        else usbNote("%s", kText[i]);
        static const uint8_t kCode[] = {evlog::kUsbSetUp, evlog::kUsbReleased, evlog::kUsbSuspended, evlog::kUsbResumed};
        ev(kCode[i], 0, uint16_t(more > 65535 ? 65535 : more));
    }
    const UsbWatch::Input in{usbmicdev::pcConfigured(), usbmicdev::pcSuspended(), pcUsing(),
                             speakerMode() ? usbspkdev::packetsReceived() : usbmicdev::framesServed()};
    char why[64];
    switch (usbWatcher.update(now, in)) {
        case UsbWatch::Action::StallNoted:
            usbStalls++;
            usbNote("stream open but the PC has stopped collecting audio");
            healthSnapshot(true);
            ev(evlog::kUsbStall);
            break;
        case UsbWatch::Action::StallCleared:
            usbNote("the PC is collecting audio again after %.1f s", usbWatcher.stalledMs / 1000.0f);
            ev(evlog::kUsbStallCleared, 0, 0, usbWatcher.stalledMs);
            break;
        case UsbWatch::Action::ReattachUnset:
            snprintf(why, sizeof(why), "the PC has not set the device up for %lu s", (unsigned long)(usbWatcher.waitedMs / 1000));
            ev(evlog::kUsbReattach, 1, 0, usbWatcher.waitedMs);
            usbReattach(why);
            usbWatcher.reattached(millis(), speakerMode() ? usbspkdev::packetsReceived() : usbmicdev::framesServed());
            break;
        case UsbWatch::Action::ReattachStall:
            ev(evlog::kUsbReattach, 2);
            usbReattach(speakerMode() ? "no audio from the PC for 5 s" : "no audio collected for 5 s");
            usbWatcher.reattached(millis(), speakerMode() ? usbspkdev::packetsReceived() : usbmicdev::framesServed());
            break;
        case UsbWatch::Action::Restart:
            ev(evlog::kUsbStallRestart);
            restartNow("USB: audio stalled again within 2 min of re-attaching", 0);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- radio
static std::atomic<uint32_t> heardWhileDown{0};  // remote beacons heard while unlinked (diagnostic)

// A remote we may link to: config.txt remote = <MAC> (only that one), and a remote locked
// to another local board is never ours.
static bool remoteAcceptable(const uint8_t* src, const uint8_t* data) {
    if (config.lock && memcmp(src, config.remoteMac, 6) != 0) return false;
    const auto* b = reinterpret_cast<const wlink::BeaconPacket*>(data);
    return wlink::macIsZero(b->lockedTo) || memcmp(b->lockedTo, myMac, 6) == 0;
}

static void noteOtherRemote(const uint8_t* src) {
    otherRemotes++;
    portENTER_CRITICAL(&linkMux);
    memcpy(otherRemoteMac, src, 6);
    portEXIT_CRITICAL(&linkMux);
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (!linked) {
        if (handoverPending && (probeKind == kProbeHandover || (wantChannel == channel && wantPair != pairId)) &&
            wlink::check(data, len, wantPair) == wlink::kBeacon && remoteAcceptable(info->src_addr, data))
            heardOnNew = true;
        if (config.lock && (probeKind == kProbeFactory || (channel == wlink::kDefaultChannel && pairId != 0)) &&
            wlink::check(data, len, 0) == wlink::kBeacon && memcmp(info->src_addr, config.remoteMac, 6) == 0)
            heardOnFactory = true;
        if (probing) return;  // on another channel: nothing else is for us
    }
    const uint8_t type = wlink::check(data, len, pairId);
    if (!type) return;
    if (type == wlink::kBeacon) {
        const bool ours = linked ? memcmp(info->src_addr, remoteMac, 6) == 0 : remoteAcceptable(info->src_addr, data);
        if (!ours) {
            noteOtherRemote(info->src_addr);  // a second remote on our mic number
            return;
        }
        if (!linked) {
            heardWhileDown++;
            portENTER_CRITICAL(&linkMux);
            if (candidatePending && memcmp(candidateMac, info->src_addr, 6) != 0 && !config.lock)
                memcpy(otherRemoteMac, candidateMac, 6), otherRemotes++;  // two remotes answering at once
            memcpy(candidateMac, info->src_addr, 6);
            candidatePending = true;
            portEXIT_CRITICAL(&linkMux);
        } else {
            helloAgain = true;  // our remote lost us: answer again
        }
        return;
    }
    if (!linked || memcmp(info->src_addr, remoteMac, 6) != 0) return;
    lastRxMs = millis();
    if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;
    switch (type) {
        case wlink::kAudio: {
            if (speakerMode()) break;  // (never sent to a speaker-mode local board)
            const auto* a = reinterpret_cast<const wlink::AudioPacket*>(data);
            // The radio buffer may not be 2-byte aligned: copy before reading samples.
            static int16_t pcm[wlink::kPacketSamples];
            const uint16_t n = a->samples;
            memcpy(pcm, data + offsetof(wlink::AudioPacket, pcm), n * sizeof(int16_t));
            if (a->h.flags & wlink::kFlagStreamStart) jitter.streamStart();
            jitter.pushBlock(a->seq, pcm, n);
            uint16_t peak = 0;
            for (uint16_t i = 0; i < n; ++i) {
                const int v = pcm[i] < 0 ? -int(pcm[i]) : pcm[i];
                if (v > peak) peak = uint16_t(v > 32767 ? 32767 : v);
            }
            if (peak > blockPeak.load(std::memory_order_relaxed)) blockPeak.store(peak, std::memory_order_relaxed);
            lastAudioMs = millis();
            break;
        }
        case wlink::kStatus:
            portENTER_CRITICAL(&linkMux);
            memcpy(&remoteStatus, data, sizeof(remoteStatus));
            haveStatus = true;
            portEXIT_CRITICAL(&linkMux);
            statusCount++;
            break;
        case wlink::kNoiseData:
            portENTER_CRITICAL(&linkMux);
            memcpy(&noise, data, sizeof(noise));
            haveNoise = true;
            portEXIT_CRITICAL(&linkMux);
            break;
        case wlink::kConfigAck:
            portENTER_CRITICAL(&linkMux);
            memcpy(&lastAck, data, sizeof(lastAck));
            ackPending = true;
            portEXIT_CRITICAL(&linkMux);
            break;
        case wlink::kAdapterInfo: {
            const auto* a = reinterpret_cast<const wlink::AdapterInfoPacket*>(data);
            if (a->id == adapterRecId) break;  // a late copy of a record we already have
            const uint8_t piece = uint8_t(a->offset / wlink::kInfoPiece);
            portENTER_CRITICAL(&linkMux);
            if (!infoComplete && a->offset % wlink::kInfoPiece == 0) {
                if (a->id != infoBufId || a->total != infoBufTotal) {  // a new record: start over
                    infoBufId = a->id;
                    infoBufTotal = a->total;
                    infoBufPieces = 0;
                }
                memcpy(infoBuf + a->offset, data + offsetof(wlink::AdapterInfoPacket, data), a->len);
                infoBufPieces |= uint8_t(1u << piece);
                const int pieces = (infoBufTotal + wlink::kInfoPiece - 1) / wlink::kInfoPiece;
                if (infoBufPieces == uint8_t((1u << pieces) - 1)) infoComplete = true;
            }
            portEXIT_CRITICAL(&linkMux);
            break;
        }
        case wlink::kLog: {
            LogText t = {};
            const int n = len - (int)sizeof(wlink::Header);
            memcpy(t.text, data + sizeof(wlink::Header), min(n, (int)sizeof(t.text) - 1));
            if (remoteLogs) xQueueSend(remoteLogs, &t, 0);
            break;
        }
        default:
            break;
    }
}

static void sendSmall(wlink::Type type) {
    if (type == wlink::kPing) {
        wlink::PingPacket p;
        wlink::fill(p.h, type, pairId);
        p.uptimeMs = millis();
        esp_now_send(remoteMac, (const uint8_t*)&p, sizeof(p));
    } else {
        wlink::Header h;
        wlink::fill(h, type, pairId);
        esp_now_send(remoteMac, (const uint8_t*)&h, sizeof(h));
    }
}

// Restarts ESP-NOW and the Wi-Fi radio without restarting the board, so the PC keeps
// its microphone (sending silence) while the radio recovers.
static void restartRadio() {
    probing = false;
    probeKind = kProbeNone;
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_start();
    const bool ok = board::startRadio(channel, onRecv, nullptr);
    board::setTxPower(config.txPower);
    healthSnapshot(true);
    ev(evlog::kRadioRestart, ok ? 0 : 1);
    logEvent("link still down after %lu s: radio restarted%s (remote beacons heard meanwhile: %lu); waiting for remote %u",
             (unsigned long)(wlink::kRecoverAfterMs / 1000), ok ? "" : " (FAILED - press RESET)",
             (unsigned long)heardWhileDown.load(), board::micNumber(pairId));
}

static uint32_t droppedAtMs = 0;      // when the current outage began (0: none)
static bool radioRestartedInDrop = false;
static uint32_t linkDrops = 0;        // since start (hourly line)

static void dropLink(const char* why) {
    healthSnapshot(true);  // the conditions just before the link went
    esp_now_del_peer(remoteMac);
    linked = false;
    droppedAtMs = millis();
    heardWhileDown = 0;
    radioRestartedInDrop = false;
    haveStatus = false;
    linkDrops++;
    logEvent("link lost (%s); waiting for remote %u", why, board::micNumber(pairId));
    ev(evlog::kLinkLost, board::micNumber(pairId));
    announcer::say(announcer::Event::LinkLost);
}

static bool pcRecording() { return usbmicdev::pcRecording(); }
static bool pcNever() { return false; }  // speaker mode: no announcements
// The PC has the audio stream open: recording from the mic, or playing to the speaker.
static bool pcUsing() { return speakerMode() ? usbspkdev::pcPlaying() : usbmicdev::pcRecording(); }

// ---------------------------------------------------------------- speaker mode
// The PC's stereo samples (USB ISR -> ring) go to the remote in 12 ms packets, like the
// remote's microphone packets the other way, each channel as IMA ADPCM (1.4.2: half the
// airtime of mono 16-bit). The encoders run continuously; each packet carries their state
// at its start, so the remote decodes every packet on its own. Its own task, so the drive
// files or a slow loop never hold the audio up.
static std::atomic<uint32_t> spkSent{0}, spkFailed{0};
static void speakerTask(void*) {
    static wlink::StereoAudioPacket pkt;
    static int16_t pcm[2 * wlink::kPacketSamples];  // left, right interleaved
    adpcm::Encoder enc[2];
    uint32_t seq = 0;
    bool start = true;
    for (;;) {
        if (usbspkdev::takeStreamStart() || !linked || probing || !usbspkdev::pcPlaying()) {
            if (!linked || !usbspkdev::pcPlaying()) usbspkdev::clear();
            start = true;
            enc[0] = adpcm::Encoder();
            enc[1] = adpcm::Encoder();
        }
        if (!linked || probing || usbspkdev::available() < wlink::kPacketSamples) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        usbspkdev::read(pcm, wlink::kPacketSamples);
        uint16_t peak = 0;
        for (uint16_t i = 0; i < 2 * wlink::kPacketSamples; ++i) {
            const int v = pcm[i] < 0 ? -int(pcm[i]) : pcm[i];
            if (v > peak) peak = uint16_t(v > 32767 ? 32767 : v);
        }
        if (peak > blockPeak.load(std::memory_order_relaxed)) blockPeak.store(peak, std::memory_order_relaxed);
        wlink::fill(pkt.h, wlink::kStereoAudio, pairId, start ? wlink::kFlagStreamStart : 0);
        start = false;
        pkt.seq = seq++;
        pkt.frames = wlink::kPacketSamples;
        for (int ch = 0; ch < 2; ++ch) {
            pkt.predictor[ch] = int16_t(enc[ch].d.predictor);
            pkt.index[ch] = uint8_t(enc[ch].d.index);
            enc[ch].encode(pcm + ch, wlink::kPacketSamples, 2, pkt.data + ch * wlink::kAdpcmChannelBytes);
        }
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < 4; ++attempt) {  // the radio queue can be full for a moment
            err = esp_now_send(remoteMac, (const uint8_t*)&pkt, sizeof(pkt));
            if (err != ESP_ERR_ESPNOW_NO_MEM) break;
            vTaskDelay(1);
        }
        if (err == ESP_OK) spkSent++;
        else spkFailed++;
    }
}

// ---------------------------------------------------------------- status
static const char* adapterText(const wlink::StatusPacket& s) {
    switch (s.adapter) {
        case wlink::kAdapterStreaming: return "streaming";
        case wlink::kAdapterUnusable: return "not usable";
        default: return "no adapter";
    }
}

static const char* filterText(uint8_t m) {
    return m == wlink::kFilterStrong ? "on, strong (20 dB)" : m == wlink::kFilterOff ? "off" : "on (12 dB)";
}

// Logs (and announces) what changed at the remote. The first status after
// linking is only logged, except a refused adapter, which is also announced.
static void reportRemoteChanges() {
    static uint8_t lastAdapter = 0xFF, lastFilter = 0xFF, lastLearned = 0xFF;
    static char lastName[sizeof(wlink::StatusPacket::name)] = "";
    if (!haveStatus) { lastAdapter = lastFilter = lastLearned = 0xFF; return; }
    wlink::StatusPacket s;
    portENTER_CRITICAL(&linkMux);
    memcpy(&s, &remoteStatus, sizeof(s));
    portEXIT_CRITICAL(&linkMux);
    s.name[sizeof(s.name) - 1] = 0;
    s.problem[sizeof(s.problem) - 1] = 0;
    s.firmware[sizeof(s.firmware) - 1] = 0;
    static uint32_t lastRemoteUpMs = 0;  // a smaller uptime than before: the remote restarted
    // (The remote's millisecond uptime wraps after 49.7 days: a wrap is not a restart.)
    // A different remote (another MAC, e.g. swapped for an AtomS3U) is not a restart either.
    static uint8_t lastStatusMac[6] = {};
    const bool otherRemote = !wlink::macIsZero(lastStatusMac) && memcmp(lastStatusMac, remoteMac, 6) != 0;
    memcpy(lastStatusMac, remoteMac, 6);
    if (otherRemote) {
        evText(0, "a different remote linked: %s, MAC %s", wlink::boardName(s.board), board::macText(remoteMac).c_str());
        lastRemoteUpMs = 0;
    }
    const bool wrapped = lastRemoteUpMs > 0xFFFFFFFFu - 120000u && s.uptimeMs < 120000u;
    if (lastRemoteUpMs && s.uptimeMs + 5000 < lastRemoteUpMs && !wrapped) {
        healthSnapshot(true);
        ev(evlog::kRemoteRestarted, 0, 0, lastRemoteUpMs / 1000);
    }
    lastRemoteUpMs = s.uptimeMs;
    if (strcmp(s.firmware, remoteFirmware) != 0) {  // first status, or the remote was updated
        strlcpy(remoteFirmware, s.firmware, sizeof(remoteFirmware));
        if (strcmp(remoteFirmware, kFirmware) == 0) logEvent("firmware: local %s, remote %s", kFirmware, remoteFirmware);
        else logEvent("firmware: local %s, remote %s - DIFFERENT versions: update both boards", kFirmware, remoteFirmware);
        evText(0, "remote firmware %s%s", remoteFirmware, strcmp(remoteFirmware, kFirmware) ? " - DIFFERENT from the local board" : "");
    }
    if (s.filterMode != lastFilter || s.filterLearned != lastLearned) {
        const bool first = lastFilter == 0xFF;
        if (s.filterMode != lastFilter)
            logEvent("remote noise filter: %s%s", filterText(s.filterMode),
                 s.filterMode != wlink::kFilterOff && !s.filterLearned ? ", learning the background noise" : "");
        else if (s.filterLearned && !lastLearned)
            logEvent("remote noise filter: background noise learned");
        if (!first && s.filterMode != lastFilter)
            announcer::say(s.filterMode == wlink::kFilterOff      ? announcer::Event::FilterOff
                           : s.filterMode == wlink::kFilterStrong ? announcer::Event::FilterStrong
                                                                  : announcer::Event::FilterOn);
        lastFilter = s.filterMode;
        lastLearned = s.filterLearned;
    }
    static uint32_t lastHighpass = 0xFFFFFFFF;  // on/off << 16 | cut-off
    const uint32_t hp = (uint32_t(s.highpassOn) << 16) | s.highpassHz;
    if (hp != lastHighpass) {
        lastHighpass = hp;
        if (s.highpassOn) logEvent("remote high-pass filter: on, %u Hz", unsigned(s.highpassHz));
        else logEvent("remote high-pass filter: off");
    }
    static uint8_t lastNoSignal = 0;
    if (s.noSignal != lastNoSignal) {
        lastNoSignal = s.noSignal;
        if (s.noSignal) {
            logEvent("remote: NO MICROPHONE SIGNAL (silence for 20 s) - check the mic plug and jack");
            ev(evlog::kNoSignal);
            announcer::say(announcer::Event::NoSignal);
        } else {
            logEvent("remote: microphone signal is back");
            ev(evlog::kSignalBack);
        }
    }
    if (s.adapter == lastAdapter && strcmp(s.name, lastName) == 0) return;
    const bool firstStatus = lastAdapter == 0xFF;
    if (s.adapter == wlink::kAdapterUnusable) announcer::say(announcer::Event::AdapterUnsupported);
    else if (!firstStatus && s.adapter == wlink::kAdapterStreaming) announcer::say(announcer::Event::AdapterConnected);
    else if (!firstStatus && s.adapter == wlink::kAdapterNone) announcer::say(announcer::Event::AdapterRemoved);
    lastAdapter = s.adapter;
    strlcpy(lastName, s.name, sizeof(lastName));
    if (s.board == wlink::kBoardAtomS3U) {  // no adapter: the AtomS3U's own microphone
        if (s.adapter == wlink::kAdapterStreaming) logEvent("remote: AtomS3U, %s, streaming", s.name);
        else logEvent("remote: AtomS3U, %s", s.problem[0] ? s.problem : "microphone not running");
        if (!firstStatus || s.adapter != wlink::kAdapterStreaming)
            evText(0, "remote AtomS3U: %s", s.adapter == wlink::kAdapterStreaming ? "built-in microphone streaming" : s.problem);
    } else if (s.adapter == wlink::kAdapterStreaming) {
        logEvent("remote adapter: %04x:%04x \"%s\" %u ch %u-bit, streaming", s.vid, s.pid, s.name, s.channels, s.bits);
        ev(evlog::kAdapterOk, s.channels, s.bits, uint32_t(s.vid) << 16 | s.pid);
    } else if (s.adapter == wlink::kAdapterUnusable) {
        logEvent("remote adapter: %04x:%04x \"%s\" NOT USABLE: %s", s.vid, s.pid, s.name, s.problem);
        evText(0, "remote adapter %04x:%04x NOT USABLE: %s", s.vid, s.pid, s.problem);
    } else {
        logEvent("remote adapter: %s", adapterText(s));
        if (!firstStatus) ev(evlog::kAdapterNone);
    }
}

// Mode (microphone / speaker): the remote decides (config.txt mode, or auto at start-up)
// and reports it. When it differs from the USB device this board presents, switch: save
// it and restart (the PC sees the new device). Only once the remote uses our mode setting.
static const char* modeReasonText(uint8_t r) { return evlog::modeReasonText(r); }
static void checkMode() {
    if (!linked || !haveStatus) return;
    wlink::StatusPacket s;
    portENTER_CRITICAL(&linkMux);
    memcpy(&s, &remoteStatus, sizeof(s));
    portEXIT_CRITICAL(&linkMux);
    static uint16_t lastSeen = 0xFFFF;
    const uint16_t seen = uint16_t(s.audioMode) << 8 | s.modeReason;
    if (seen != lastSeen) {
        lastSeen = seen;
        logEvent("remote mode: %s (%s)", s.audioMode == wlink::kModeSpeaker ? "speaker" : "microphone",
                 modeReasonText(s.modeReason));
    }
    if (s.modeReason == wlink::kReasonDeciding || s.modeSetting != config.mode || s.audioMode == audioMode) return;
    ev(evlog::kModeChange, s.audioMode, s.modeReason);
    settings.put("amode", s.audioMode);
    char why[96];
    snprintf(why, sizeof(why), "mode: %s (%s): the PC gets the USB %s", s.audioMode == wlink::kModeSpeaker ? "speaker" : "microphone",
             modeReasonText(s.modeReason), s.audioMode == wlink::kModeSpeaker ? "speaker" : "microphone");
    restartNow(why, 0);
}

// Another remote on our mic number (a neighbour's extender?): a WARNING in the log and
// events.txt, at most every 10 minutes; status.txt says it too.
static void reportPairing() {
    static uint32_t seen = 0, lastLog = 0;
    const uint32_t n = otherRemotes.load();
    if (n == seen || (lastLog && millis() - lastLog < 600000)) return;
    seen = n;
    lastLog = millis();
    uint8_t mac[6];
    portENTER_CRITICAL(&linkMux);
    memcpy(mac, otherRemoteMac, 6);
    portEXIT_CRITICAL(&linkMux);
    if (config.lock) {
        logEvent("another remote %s is on mic %u / channel %u: ignored (config.txt remote = %s)", board::macText(mac).c_str(),
                 board::micNumber(pairId), channel, cfg::remoteText(config));
    } else {
        logEvent("WARNING: a second remote %s is on mic %u / channel %u (another extender nearby?) - set remote = <your "
                 "remote's MAC> in config.txt so this board links only to its own", board::macText(mac).c_str(),
                 board::micNumber(pairId), channel);
        evText(0, "WARNING: a second remote %s is on mic %u / channel %u", board::macText(mac).c_str(), board::micNumber(pairId), channel);
    }
}

// Once an hour: versions, uptime and link drops, so any part of a long log shows
// which firmware produced it. Also the hourly health snapshot.
static void reportHourly() {
    static uint32_t next = 3600000;
    const uint32_t now = millis();
    if (int32_t(now - next) < 0) return;
    next += 3600000;
    healthSnapshot(false);
    const uint32_t s = upS();
    logf("hourly: firmware local %s, remote %s | up %lu:%02lu:%02lu | link drops since start %lu", kFirmware,
         remoteFirmwareText(), (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60),
         (unsigned long)linkDrops);
}

// Every 10 minutes: internal RAM of both boards (the radio driver's packet buffers come
// from it) and the remote's radio queue, so a slow leak shows as a steady fall.
static void reportMemory() {
    static uint32_t next = 30000;
    const uint32_t now = millis();
    if (int32_t(now - next) < 0) return;
    next = now + 600000;
    String remote = "not linked";
    if (linked && haveStatus) {
        wlink::StatusPacket s;
        portENTER_CRITICAL(&linkMux);
        memcpy(&s, &remoteStatus, sizeof(s));
        portEXIT_CRITICAL(&linkMux);
        remote = board::memText({s.heapFree, s.heapMin, s.heapLargest}) + ", radio queue " + String(s.radioQueued);
    }
    logf("memory: remote %s | local %s", remote.c_str(), board::memText(board::memInfo()).c_str());
}

static void reportStats() {
    static uint32_t lastMs = 0;
    static JitterBuffer::Stats prev;
    if (millis() - lastMs < 10000) return;
    lastMs = millis();
    const JitterBuffer::Stats now = jitter.stats();
    const float ppm = JitterBuffer::ppm(prev, now);
    // Remote's send counters (from its status packets), as changes since the last line.
    static uint32_t prevQueue = 0, prevNoAck = 0;
    uint32_t queueFull = prevQueue, noAck = prevNoAck;
    if (haveStatus) {
        portENTER_CRITICAL(&linkMux);
        queueFull = remoteStatus.queueFull;
        noAck = remoteStatus.noAck;
        portEXIT_CRITICAL(&linkMux);
    }
    const uint32_t dQueue = queueFull >= prevQueue ? queueFull - prevQueue : queueFull;  // remote rebooted
    const uint32_t dNoAck = noAck >= prevNoAck ? noAck - prevNoAck : noAck;
    prevQueue = queueFull;
    prevNoAck = noAck;
    char remote[112] = "not linked";
    if (linked) snprintf(remote, sizeof(remote), "linked (%s), RSSI %d dBm, tx queue-full %lu no-ack %lu",
                         haveStatus ? adapterText(remoteStatus) : "no status yet", rssi,
                         (unsigned long)dQueue, (unsigned long)dNoAck);
    if (speakerMode()) {
        // Speaker: what this board sent, and how the remote's playback buffer coped.
        static uint32_t pSent = 0, pFailed = 0, pDropped = 0, pUnder = 0, pLost = 0;
        const uint32_t sentNow = spkSent.load(), failedNow = spkFailed.load(), droppedNow = usbspkdev::dropped();
        uint32_t under = pUnder, lost = pLost;
        if (haveStatus && remoteStatus.underrunsKnown) { under = remoteStatus.spkUnderruns; lost = remoteStatus.spkLost; }
        logf("remote %s | PC playing: %s | sent %lu, failed %lu, PC samples dropped %lu | remote: lost %lu, underruns %lu",
             remote, usbspkdev::pcPlaying() ? "yes" : "no", (unsigned long)(sentNow - pSent), (unsigned long)(failedNow - pFailed),
             (unsigned long)(droppedNow - pDropped), (unsigned long)(lost >= pLost ? lost - pLost : lost),
             (unsigned long)(under >= pUnder ? under - pUnder : under));
        pSent = sentNow; pFailed = failedNow; pDropped = droppedNow; pUnder = under; pLost = lost;
        prev = now;
        return;
    }
    logf("remote %s | PC recording: %s | buffer %lu ms | drift %+.0f ppm | lost %lu late %lu | underruns %lu",
         remote, usbmicdev::pcRecording() ? "yes" : "no", (unsigned long)(jitter.level() / 48), ppm,
         (unsigned long)(now.lostBlocks - prev.lostBlocks), (unsigned long)(now.lateBlocks - prev.lateBlocks),
         (unsigned long)(now.underruns - prev.underruns));
    prev = now;
}

static void updateLed() {
    static uint32_t lastUnderruns = 0;
    const JitterBuffer::Stats s = jitter.stats();
    if (s.underruns != lastUnderruns) { lastUnderruns = s.underruns; led.flicker(); }
    led.feedPeak(blockPeak.exchange(0));
    const uint32_t held = bootButton.heldMs(millis());
    if (held >= 10000) { led.raw(false); return; }                       // dark: release = factory reset
    if (driveOpening) { led.mode(board::LedMode::Waiting); led.update(); return; }
    // Audio flowing: from the remote (microphone), or from the PC to the remote (speaker).
    if (speakerMode() && usbspkdev::pcPlaying() && linked) lastAudioMs = millis();
    const bool audio = linked && millis() - lastAudioMs < 300;
    // Audio arriving: level meter (glow = PC recording, 2 s flash = PC not recording).
    if (!linked) led.mode(board::LedMode::Searching);
    else if (haveStatus && (remoteStatus.adapter == wlink::kAdapterUnusable || (remoteStatus.noSignal && !speakerMode())))
        led.mode(board::LedMode::Problem);
    else if (audio) led.mode(pcUsing() ? board::LedMode::LevelGlow : board::LedMode::LevelFlash);
    else led.mode(board::LedMode::LinkedIdle);
    led.update();
}

// ---------------------------------------------------------------- settings
static void restartNow(const char* why, int blinks) {
    flushRemotePending();
    logf("%s, restarting", why);
    for (int i = 0; i < blinks; ++i) { led.raw(true); delay(150); led.raw(false); delay(150); }
    delay(300);
    ESP.restart();
}

// Mounts the drive at start-up, and repairs it if it is damaged (a power cut while it was
// being written, e.g. during the first start right after flashing): the whole partition is
// erased raw and formatted again; config.txt is kept if it could still be read; the voice
// clips and README.txt are written again by announcer::begin().
static bool mountDrive() {
    char why[64] = "";
    bool damaged = false;
    if (!FFat.begin(true)) {
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat")) return false;
        snprintf(why, sizeof(why), "it could not be mounted");
        damaged = true;
    } else {
        damaged = announcer::driveDamaged(why, sizeof(why));
    }
    if (!damaged) return true;
    String keep;
    if (FFat.exists("/config.txt")) {
        File f = FFat.open("/config.txt", FILE_READ);
        if (f && f.size() > 0 && f.size() < 8192) keep = f.readString();
        if (f) f.close();
    }
    logf("drive: DAMAGED (%s): erasing and formatting it again", why);
    esp_task_wdt_reset();
    const bool erased = announcer::eraseDrivePartition();
    const bool ok = erased && FFat.begin(true);
    if (ok && keep.length()) {
        File f = FFat.open("/config.txt", FILE_WRITE);
        if (f) { f.print(keep); f.close(); }
    }
    evText(0, "EXTENDER drive was damaged (%s): %s%s", why, ok ? "erased and formatted again" : "repair FAILED",
           ok && keep.length() ? ", config.txt kept" : "");
    return ok;
}

// Reads /config.txt (creating it with the defaults if missing). Values the
// file does not mention keep the current ones.
static void readConfig(uint8_t curPair, uint8_t curChannel) {
    cfg::Settings base;
    base.mic = curPair + 1;
    base.channel = curChannel;
    File f = FFat.open("/config.txt", FILE_READ);
    if (!f) {
        config = cfg::Settings();  // defaults: Mic 1, channel 11, voice, filter on, normal
        static char text[2600];  // config.txt with its comments is ~2 KB
        int n = cfg::format(config, text, sizeof(text));
        if (n < 0) n = 0;
        if (n >= int(sizeof(text))) n = sizeof(text) - 1;  // never write past the buffer
        File w = FFat.open("/config.txt", FILE_WRITE);
        if (w) { w.write((const uint8_t*)text, n); w.close(); }
        logEvent("config.txt: not found, created with the defaults");
        return;
    }
    static char text[4096];
    const int n = f.read((uint8_t*)text, sizeof(text) - 1);
    f.close();
    text[n > 0 ? n : 0] = 0;
    const cfg::Result r = cfg::parse(text, base);
    config = r.settings;
    if (r.errors) {
        snprintf(configProblem, sizeof(configProblem), "%d line(s) not understood (first: %s)", r.errors, r.firstError);
        evText(0, "config.txt: %s", configProblem);
    }
    char hp[16];
    if (config.highpass) snprintf(hp, sizeof(hp), "%u Hz", unsigned(config.highpassHz));
    else strcpy(hp, "off");
    logEvent("config.txt: mic %u, channel %u, feedback %s, noise filter %s, adapt %s, high-pass %s, tx power %s, cpu %u MHz, "
         "temperature %s%s%s",
         config.mic, config.channel, cfg::feedbackName(config.feedback), cfg::filterName(config.filter),
         cfg::adaptName(config.adapt), hp, cfg::txPowerText(config.txPower), unsigned(config.cpuMhz),
         config.temperature ? "on" : "off",
         r.errors ? " | " : "", configProblem);
    logf("config.txt: distance %s, mode %s, remote %s", cfg::distanceName(config.distance), cfg::modeName(config.mode),
         cfg::remoteText(config));
    char lv[12];
    cfg::levelText(config.micLevel, lv, sizeof(lv));
    logf("config.txt: mic level %s%s", lv, config.micLevel <= 100 ? " % of the adapter's range (AtomS3U: of its gain)" : " (the adapter's own)");
}

// ---------------------------------------------------------------- temperatures
// Both chips' temperatures: the latest, the maximum since the local board started,
// a log line 1 minute after start and then every 10 minutes, and an event when
// either reaches 75 C (again only after it has cooled below 70 C).
struct TempTrack {
    int16_t now = wlink::kTempUnknown, max = wlink::kTempUnknown;
    uint32_t maxS = 0;  // seconds since start
    bool hot = false;
    void add(int16_t t, uint32_t sec, const char* who) {
        now = t;
        if (t == wlink::kTempUnknown) return;
        if (max == wlink::kTempUnknown || t > max) { max = t; maxS = sec; }
        if (!hot && t >= wlink::kTempHotDeciC) {
            hot = true;
            logEvent("WARNING: %s chip temperature %s - shade or ventilate it; cpu_mhz = 160 or a lower tx_power runs cooler",
                     who, board::tempText(t).c_str());
            ev(evlog::kTempHigh, strcmp(who, "remote") == 0, uint16_t(t));
        } else if (hot && t < wlink::kTempCoolDeciC) {
            hot = false;
            logEvent("%s chip temperature back to %s", who, board::tempText(t).c_str());
            ev(evlog::kTempOk, strcmp(who, "remote") == 0, uint16_t(t));
        }
    }
};
static TempTrack localTemp, remoteTemp;

// Temperature read with the radio paused (see the remote's readTempRadioPaused): ESP-NOW
// and Wi-Fi stop, one read, then both start again and the remote is restored as a peer.
static int16_t readTempRadioPaused() {
    esp_now_deinit();
    esp_wifi_stop();
    const int16_t t = board::readChipTempOnce();
    esp_wifi_start();
    board::startRadio(channel, onRecv, nullptr);
    board::setTxPower(config.txPower);
    if (linked) board::addPeer(remoteMac, true, config.distance);
    return t;
}

static void reportTemperatures() {
    if (!config.temperature) return;
    static uint32_t nextRead = 60000;
    static int16_t lastRemote = wlink::kTempUnknown;
    const uint32_t now = millis();
    // The remote reads its own sensor every 10 min and sends the value with its status.
    int16_t remote = wlink::kTempUnknown;
    if (linked && haveStatus) {
        portENTER_CRITICAL(&linkMux);
        remote = remoteStatus.tempDeciC;
        portEXIT_CRITICAL(&linkMux);
    }
    if (remote != wlink::kTempUnknown && remote != lastRemote) remoteTemp.add(remote, upS(), "remote");
    lastRemote = remote;
    // Our own: one short read 1 minute after start, then every 10 minutes, with the log line.
    if (int32_t(now - nextRead) >= 0) {
        nextRead = now + board::kTempReadMs;
        localTemp.add(readTempRadioPaused(), upS(), "local");
        logf("temperature: remote %s, local %s (max since start: remote %s, local %s)",
             board::tempText(remote).c_str(), board::tempText(localTemp.now).c_str(),
             board::tempText(remoteTemp.max).c_str(), board::tempText(localTemp.max).c_str());
    }
}

static String maxText(const TempTrack& t) {
    if (t.max == wlink::kTempUnknown) return "unknown";
    const uint32_t s = t.maxS;
    char b[48];
    snprintf(b, sizeof(b), "%s at %lu:%02lu:%02lu", board::tempText(t.max).c_str(), (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    return b;
}

// Health snapshot into the event log (three records): signal, both temperatures, both
// boards' memory, the remote's radio queue and failed sends, audio lost / late /
// underruns since the previous snapshot. Hourly, and just before critical events (at
// most one a minute for those, so a flapping link does not fill the log).
static void healthSnapshot(bool forEvent) {
    if (!eventLog.ok()) return;
    const uint32_t nowS = upS();
    static uint32_t lastEventSnapS = 0;
    static bool anyEventSnap = false;
    if (forEvent) {
        if (anyEventSnap && nowS - lastEventSnapS < 60) return;
        anyEventSnap = true;
        lastEventSnapS = nowS;
    }
    static JitterBuffer::Stats prev;
    static uint32_t prevFailed = 0;
    JitterBuffer::Stats js = jitter.stats();
    wlink::StatusPacket st;
    portENTER_CRITICAL(&linkMux);
    memcpy(&st, &remoteStatus, sizeof(st));
    const bool remoteOk = haveStatus;
    portEXIT_CRITICAL(&linkMux);
    if (speakerMode()) {  // the audio buffer is at the remote: its playback counters
        js = prev;
        if (remoteOk && st.underrunsKnown) { js.lostBlocks = st.spkLost; js.underruns = st.spkUnderruns; }
        if (js.lostBlocks < prev.lostBlocks || js.underruns < prev.underruns) prev = JitterBuffer::Stats();  // remote restarted
    }
    const uint32_t failed = remoteOk ? st.queueFull + st.noAck : prevFailed;
    const uint32_t dFailed = failed >= prevFailed ? failed - prevFailed : failed;  // remote restarted
    if (remoteOk) prevFailed = failed;
    auto cap16 = [](uint32_t v) { return uint32_t(v > 65535 ? 65535 : v); };
    auto cap8 = [](uint32_t v) { return uint8_t(v > 255 ? 255 : v); };
    const uint8_t flags = (linked ? evlog::kHfLinked : 0) | (remoteOk ? evlog::kHfRemote : 0) |
                          (pcUsing() ? evlog::kHfRecording : 0) | (forEvent ? evlog::kHfEvent : 0);
    const board::MemInfo m = board::memInfo();
    const int16_t remoteT = remoteOk && st.temperatureOn ? st.tempDeciC : evlog::kTempNone;
    const int16_t localT = config.temperature ? localTemp.now : evlog::kTempNone;
    eventLog.add(evlog::kHealthRadio, nowS, uint8_t(int8_t(rssi)), uint16_t(uint8_t(st.rssi)) | uint16_t(flags) << 8,
                 cap16(st.radioQueued) << 16 | cap16(dFailed));
    eventLog.add(evlog::kHealthTemp, nowS, cap8(js.lateBlocks - prev.lateBlocks), uint16_t(remoteT),
                 uint32_t(uint16_t(localT)) << 16 | cap16(js.lostBlocks - prev.lostBlocks));
    eventLog.add(evlog::kHealthMem, nowS, cap8(js.underruns - prev.underruns), uint16_t(cap16(st.heapLargest / 1024)),
                 cap16(st.heapFree / 1024) << 16 | cap16(m.free / 1024));
    prev = js;
}

// ---------------------------------------------------------------- adapter details
// Asks the remote for the details of its latest adapter when its status shows an id we
// do not have; keeps the received record (and in flash, if it differs from the saved one).
static void syncAdapterInfo() {
    if (infoComplete) {
        static uint8_t rec[wlink::kInfoMax];
        uint16_t n;
        uint32_t id;
        portENTER_CRITICAL(&linkMux);
        n = infoBufTotal;
        id = infoBufId;
        memcpy(rec, infoBuf, n);
        infoComplete = false;
        infoBufPieces = 0;
        infoBufId = 0;
        portEXIT_CRITICAL(&linkMux);
        if (!adinfo::recordSize(rec, n)) {
            logf("adapter details: received record not understood (different firmware versions?)");
            adapterRecId = id;  // do not ask again for this one
            return;
        }
        // Flash only when the adapter or its outcome changed (not just the remote's uptime).
        constexpr size_t kUp = offsetof(adinfo::RecordHeader, remoteUptimeS);
        const bool same = n == adapterRecLen && memcmp(rec, adapterRec, kUp) == 0 &&
                          memcmp(rec + kUp + 4, adapterRec + kUp + 4, n - kUp - 4) == 0;
        memcpy(adapterRec, rec, n);
        adapterRecLen = n;
        adapterRecId = id;
        adapterRecS = upS();
        if (!same) settings.putBytes("adapter", adapterRec, adapterRecLen);
        adinfo::RecordHeader h;
        memcpy(&h, adapterRec, sizeof(h));
        h.product[sizeof(h.product) - 1] = 0;
        logf("adapter details received: %04x:%04x \"%s\", %u bytes%s (adapter.txt on the EXTENDER drive)",
             unsigned(h.device[8] | (h.device[9] << 8)), unsigned(h.device[10] | (h.device[11] << 8)), h.product,
             unsigned(n), same ? "" : ", saved");
        return;
    }
    if (!linked || !haveStatus) return;
    const uint32_t want = remoteStatus.adapterInfoId;
    static uint32_t lastAsk = 0;
    if (!want || want == adapterRecId || millis() - lastAsk < 1000) return;
    lastAsk = millis();
    wlink::InfoRequestPacket r;
    wlink::fill(r.h, wlink::kInfoRequest, pairId);
    r.id = want;
    esp_now_send(remoteMac, (const uint8_t*)&r, sizeof(r));
}

// ---------------------------------------------------------------- drive files
// status.txt: what is in effect right now (both boards). events.txt says what happened.
// Also the top of report.html. Written when the drive opens, after waiting for the remote.
static char noiseWhy[112] = "";  // why there is no noise.svg ("" = there is one)
static bool driveRemoteLate = false;  // the drive opened without a fresh status from the remote

static void renderStatus(evlog::Out& f) {
    f.printf("Wireless USB Audio Extender - status when the drive was opened (%s after start)\r\n", evlog::Log::dur(upS()).s);
    if (driveRemoteLate)
        f.printf("The remote did not answer within 15 s: its part below is from its last status before the drive was\r\n"
                 "opened, if any.\r\n");
    f.printf("\r\nLocal firmware %s on %s, MAC %s\r\n", kFirmware, board::kindName(), board::macText(myMac).c_str());
    f.printf("  the PC sees: \"Extender %s %u\" (USB %s)\r\n", speakerMode() ? "Speaker" : "Mic", board::micNumber(pairId),
             speakerMode() ? "speaker" : "microphone");
    f.printf("  mode setting: %s%s\r\n", cfg::modeName(config.mode),
             config.mode == wlink::kModeAuto ? " (the remote decides when an adapter is plugged in)" : "");
    f.printf("  mic %u, channel %u, tx power %s dBm, distance %s (%s)\r\n", board::micNumber(pairId), channel,
             cfg::txPowerText(config.txPower), wlink::distanceName(config.distance), wlink::distanceRateText(config.distance));
    if (config.lock)
        f.printf("  pairing: remote = %s: links only to that remote and locks it to this board\r\n", cfg::remoteText(config));
    else
        f.printf("  pairing: remote = any: links to any remote on mic %u (not locked)\r\n", board::micNumber(pairId));
    if (otherRemotes.load()) {
        uint8_t mac[6];
        portENTER_CRITICAL(&linkMux);
        memcpy(mac, otherRemoteMac, 6);
        portEXIT_CRITICAL(&linkMux);
        f.printf("  %s: another remote was heard on mic %u since start (%lu beacons, last from %s)%s\r\n",
                 config.lock ? "note" : "WARNING", board::micNumber(pairId), (unsigned long)otherRemotes.load(),
                 board::macText(mac).c_str(), config.lock ? ", ignored" : " - lock the pair with remote = <MAC>");
    }
    if (speakerMode()) f.printf("  audio feedback: off in speaker mode\r\n");
    else f.printf("  audio feedback: %s\r\n", announcer::modeText(announcer::mode()));
    char unusable[200];
    announcer::unusableText(unusable, sizeof(unusable));
    if (announcer::usableFiles() < int(announcer::Event::Count))
        f.printf("  voice files: %d of %d usable; not usable (the built-in voice plays instead): %s\r\n",
                 announcer::usableFiles(), int(announcer::Event::Count), unusable);
    else
        f.printf("  voice files: %d of %d usable\r\n", announcer::usableFiles(), int(announcer::Event::Count));
    f.printf("  config.txt: %s\r\n", configProblem[0] ? configProblem : "ok");
    f.printf("  started by: %s\r\n", board::resetReasonText());
    if (eventLog.ok()) f.printf("  event log: start %u, %lu events this start (events.txt)\r\n", eventLog.start(), (unsigned long)eventLog.written());
    else f.printf("  event log: none - flash the full image once to create its flash area\r\n");
    if (config.temperature)
        f.printf("  processor %u MHz, chip temperature %s (max %s)\r\n", unsigned(getCpuFrequencyMhz()),
                 board::tempText(localTemp.now).c_str(), maxText(localTemp).c_str());
    else
        f.printf("  processor %u MHz\r\n", unsigned(getCpuFrequencyMhz()));
    if (handoverPending)
        f.printf("  WAITING for the remote to move to mic %u / channel %u together: turn the remote on (this board\r\n"
                 "  stays on mic %u / channel %u until the remote confirms, or is heard on the new setting)\r\n",
                 board::micNumber(wantPair), wantChannel, board::micNumber(pairId), channel);
    f.printf("\r\nUSB connection to the PC (counts since start)\r\n");
    f.printf("  set up %lu, released %lu, suspended %lu, resumed %lu; audio stalls %lu, re-attaches %lu\r\n",
             (unsigned long)usbEvents[ARDUINO_USB_STARTED_EVENT].load(), (unsigned long)usbEvents[ARDUINO_USB_STOPPED_EVENT].load(),
             (unsigned long)usbEvents[ARDUINO_USB_SUSPEND_EVENT].load(), (unsigned long)usbEvents[ARDUINO_USB_RESUME_EVENT].load(),
             (unsigned long)usbStalls, (unsigned long)usbReattaches);
    f.printf("  last event: %s (%s after start)\r\n", usbLastEvent, evlog::Log::dur(usbLastEventS).s);
    f.printf("\r\nRemote: %s\r\n", linked ? "linked" : "not linked now");
    if (linked || remoteMac[0] || remoteMac[1] || remoteMac[2])
        f.printf("  MAC %s (%s)\r\n", board::macText(remoteMac).c_str(),
                 haveStatus && !wlink::macIsZero(remoteStatus.lockedTo) ? "locked to this local board" : "not locked");
    if (haveStatus) {
        wlink::StatusPacket s;
        portENTER_CRITICAL(&linkMux);
        memcpy(&s, &remoteStatus, sizeof(s));
        portEXIT_CRITICAL(&linkMux);
        s.firmware[sizeof(s.firmware) - 1] = 0;
        s.name[sizeof(s.name) - 1] = 0;
        f.printf("  board: %s%s\r\n", wlink::boardName(s.board), s.board == wlink::kBoardAtomS3U ? " (built-in microphone)" : "");
        f.printf("  firmware %s, RSSI %d dBm, tx power %s dBm, distance %s (%s)\r\n", s.firmware, rssi,
                 cfg::txPowerText(s.txPowerDbm), wlink::distanceName(s.distance), wlink::distanceRateText(s.distance));
        f.printf("  mode: %s (%s)\r\n", s.audioMode == wlink::kModeSpeaker ? "speaker" : "microphone", modeReasonText(s.modeReason));
        if (s.rejected)
            f.printf("  WARNING: other local boards tried to link to it: %lu hellos refused, last from %s%s\r\n",
                     (unsigned long)s.rejected, board::macText(s.rejectedMac).c_str(),
                     wlink::macIsZero(s.lockedTo) ? "" : " (it is locked)");
        if (s.temperatureOn)
            f.printf("  processor %u MHz, chip temperature %s (max %s)\r\n", unsigned(s.cpuMhz),
                     board::tempText(s.tempDeciC).c_str(), maxText(remoteTemp).c_str());
        else
            f.printf("  processor %u MHz\r\n", unsigned(s.cpuMhz));
        if (s.board == wlink::kBoardAtomS3U) f.printf("  microphone: %s %s\r\n", adapterText(s), s.name);
        else f.printf("  adapter: %s %s\r\n", adapterText(s), s.name);
        if (s.audioMode == wlink::kModeSpeaker) {
            f.printf("  noise filter and high-pass: not used in speaker mode\r\n");
            if (s.underrunsKnown)
                f.printf("  playback: %lu audio packets lost on the way, %lu buffer underruns (since the remote started)\r\n",
                         (unsigned long)s.spkLost, (unsigned long)s.spkUnderruns);
        } else {
            f.printf("  noise filter: %s, adapt %s, %s\r\n", filterText(s.filterMode), cfg::adaptName(s.adapt),
                     s.filterLearned ? "background noise learned" : "learning");
            if (s.highpassOn) f.printf("  high-pass filter: on, %u Hz\r\n", unsigned(s.highpassHz));
            else f.printf("  high-pass filter: off\r\n");
        }
        char lv[12];
        cfg::levelText(s.micLevel, lv, sizeof(lv));
        const char* what = s.audioMode == wlink::kModeSpeaker ? "speaker level" : "mic level";
        if (s.hasLevel && s.board == wlink::kBoardAtomS3U)
            f.printf("  %s: %s%s -> gain %.1f dB (range %.1f to %.1f dB)\r\n", what, lv, s.micLevel <= 100 ? " %" : "",
                     s.levelNow / 256.0, s.levelMin / 256.0, s.levelMax / 256.0);
        else if (s.hasLevel)
            f.printf("  %s: %s%s -> adapter at %.1f dB (its range %.1f to %.1f dB)\r\n", what, lv, s.micLevel <= 100 ? " %" : "",
                     s.levelNow / 256.0, s.levelMin / 256.0, s.levelMax / 256.0);
        else if (s.adapter == wlink::kAdapterStreaming)
            f.printf("  %s: %s%s (this adapter has no level control)\r\n", what, lv, s.micLevel <= 100 ? " %" : "");
        else
            f.printf("  %s: %s%s\r\n", what, lv, s.micLevel <= 100 ? " %" : "");
        if (s.noSignal) f.printf("  NO MICROPHONE SIGNAL: %s\r\n", s.board == wlink::kBoardAtomS3U
                                       ? "the built-in microphone sends only silence" : "the adapter sends only silence (mic plug / jack?)");
        if (s.board != wlink::kBoardAtomS3U) f.printf("  adapter details: adapter.txt\r\n");
    }
    if (noiseWhy[0]) f.printf("  noise graph: none - %s\r\n", noiseWhy);
    else f.printf("  noise graph: noise.svg (also in report.html)\r\n");
}

// Whether a noise graph can be drawn now; if not, why (noiseWhy). By design: no file when the
// filter is off or still learning, no remote is linked, no adapter streams, or the mic
// gives no signal.
static bool noiseWanted(const wlink::StatusPacket& s, bool fresh) {
    const char* why = nullptr;
    if (speakerMode() || s.audioMode == wlink::kModeSpeaker) why = "speaker mode (no microphone, no noise filter)";
    else if (!linked || !fresh) why = "the remote was not linked when the drive was opened";
    else if (s.filterMode == wlink::kFilterOff) why = "the noise filter is off (noise_filter = off)";
    else if (s.adapter != wlink::kAdapterStreaming) why = "no audio adapter is streaming at the remote";
    else if (s.noSignal) why = "no microphone signal";
    else if (!s.filterLearned) why = "the noise filter is still learning (about 2 minutes after the adapter starts)";
    snprintf(noiseWhy, sizeof(noiseWhy), "%s", why ? why : "");
    return !why;
}

static void renderNoiseSvg(noisesvg::Out& out) {
    wlink::StatusPacket s;
    portENTER_CRITICAL(&linkMux);
    memcpy(&s, &remoteStatus, sizeof(s));
    portEXIT_CRITICAL(&linkMux);
    s.name[sizeof(s.name) - 1] = 0;
    char title[96], line1[160], line2[200], lv[12], lvDb[40] = "";
    snprintf(title, sizeof(title), "Extender Mic %u - background noise learned by the remote's noise filter", board::micNumber(pairId));
    const bool atom = s.board == wlink::kBoardAtomS3U;
    snprintf(line1, sizeof(line1), "learned over %s | %s: %s | drive opened at start %u, %s", evlog::Log::dur(noise.learnedS).s,
             atom ? "microphone" : "adapter", s.name[0] ? s.name : "?", eventLog.start(), evlog::Log::dur(upS()).s);
    cfg::levelText(s.micLevel, lv, sizeof(lv));
    if (s.hasLevel) snprintf(lvDb, sizeof(lvDb), atom ? " (gain %.1f dB)" : " (adapter at %.1f dB)", s.levelNow / 256.0);
    char hp[24];
    if (s.highpassOn) snprintf(hp, sizeof(hp), "on, %u Hz", unsigned(s.highpassHz));
    else snprintf(hp, sizeof(hp), "off");
    snprintf(line2, sizeof(line2), "noise_filter %s | noise_adapt %s | highpass %s | mic_level %s%s%s",
             s.filterMode == wlink::kFilterStrong ? "strong (20 dB)" : s.filterMode == wlink::kFilterOff ? "off" : "on (12 dB)",
             cfg::adaptName(s.adapt), hp, lv, s.micLevel <= 100 ? " %" : "", lvDb);
    static int16_t bins[wlink::kNoiseBins];  // the packet is packed: copy to aligned values
    memcpy(bins, (const uint8_t*)&noise + offsetof(wlink::NoisePacket, deciDb), sizeof(bins));
    noisesvg::Info in;
    in.deciDb = bins;
    in.title = title;
    in.line1 = line1;
    in.line2 = line2;
    in.highpassOn = s.highpassOn;
    in.highpassHz = s.highpassHz;
    noisesvg::render(out, in);
}

// Writes one file on the drive without ever leaving it half-written: render() runs once
// to count the bytes and, if they fit in the free space, again into <path>.tmp, which is
// then renamed over the old file. render(write, ctx) calls write(ctx, text) for each piece.
using WriteFn = void (*)(void*, const char*);
template <class Render>
static bool writeDriveFile(const char* path, Render render, size_t* needOut = nullptr) {
    size_t need = 0;
    render([](void* c, const char* t) {
        *static_cast<size_t*>(c) += strlen(t);
        static uint32_t pieces = 0;
        if (++pieces % 256 == 0) esp_task_wdt_reset();  // counting a long events.txt takes a moment too
    }, &need);
    if (needOut) *needOut = need;
    size_t old = 0;
    if (FFat.exists(path)) {
        File o = FFat.open(path, FILE_READ);
        if (o) { old = o.size(); o.close(); }
    }
    const size_t freeNow = FFat.totalBytes() - FFat.usedBytes();
    constexpr size_t kMargin = 8192;  // FAT clusters and directory growth
    if (need + kMargin > freeNow + old) return false;
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FFat.remove(tmp);
    File f = FFat.open(tmp, FILE_WRITE);
    if (!f) return false;
    struct Ctx { File* f; size_t written; uint32_t pieces; } ctx{&f, 0, 0};
    render([](void* c, const char* t) {
        Ctx* x = static_cast<Ctx*>(c);
        x->written += x->f->print(t);
        if (++x->pieces % 64 == 0) esp_task_wdt_reset();  // a long file takes a few seconds
    }, &ctx);
    f.close();
    if (ctx.written != need) {  // the drive filled up after all
        FFat.remove(tmp);
        return false;
    }
    FFat.remove(path);
    return FFat.rename(tmp, path);
}

// A small file in place of one that did not fit, so a stale copy is never mistaken for it.
static void writeNotWritten(const char* path, const char* what) {
    char text[200];
    snprintf(text, sizeof(text), "%s was not written when the drive was opened: the drive is full (%u KB free).\r\n"
             "Delete files you added to the drive, then open it again.\r\n", what,
             unsigned((FFat.totalBytes() - FFat.usedBytes()) / 1024));
    if (!writeDriveFile(path, [&](WriteFn w, void* c) { w(c, text); })) FFat.remove(path);
    logf("drive: %s did not fit on the drive", what);
}

// The remote in use is an AtomS3U (its latest status says so): it has no USB adapter.
static bool remoteIsAtom(char* mac, size_t cap) {
    if (!statusCount.load()) return false;
    wlink::StatusPacket s;
    portENTER_CRITICAL(&linkMux);
    memcpy(&s, &remoteStatus, sizeof(s));
    portEXIT_CRITICAL(&linkMux);
    strlcpy(mac, board::macText(remoteMac).c_str(), cap);
    return s.board == wlink::kBoardAtomS3U;
}

static void writeAdapterText(WriteFn w, void* c) {
    char mac[24];
    if (remoteIsAtom(mac, sizeof(mac))) {  // never leave an older adapter's details to be taken for this remote's
        char text[320];
        snprintf(text, sizeof(text),
                 "Wireless USB Audio Extender - the remote's audio adapter\r\n\r\n"
                 "The remote (MAC %s) is an M5Stack AtomS3U: it uses its built-in microphone\r\n"
                 "(SPM1423, PDM) and has no USB audio adapter. See status.txt for its microphone and gain.\r\n", mac);
        w(c, text);
        return;
    }
    char seen[120];
    if (!adapterRecLen) {
        snprintf(seen, sizeof(seen), "written %lu s after the local board started", (unsigned long)upS());
    } else if (adapterRecId) {
        snprintf(seen, sizeof(seen), "received %s after the local board started; written at %s", evlog::Log::dur(adapterRecS).s,
                 evlog::Log::dur(upS()).s);
    } else {
        snprintf(seen, sizeof(seen), "saved copy: received before the local board last started");
    }
    adinfo::Out out{w, c};
    adinfo::render(adapterRec, adapterRecLen, seen, out);
}

// All the drive's files, most important first: status.txt, adapter.txt, noise.svg,
// events.txt, report.html (if space runs out, the small important ones are there).
static void writeDriveFiles(bool haveGraph) {
    const uint32_t t0 = millis();
    flushRemotePending();
    // A fresh local temperature for status.txt (the radio pause no longer matters: the board restarts next).
    if (config.temperature) localTemp.add(readTempRadioPaused(), upS(), "local");
    auto statusText = [](WriteFn w, void* c) { evlog::Out o{w, c}; renderStatus(o); };
    if (!writeDriveFile("/status.txt", statusText)) writeNotWritten("/status.txt", "status.txt");
    if (!writeDriveFile("/adapter.txt", writeAdapterText)) writeNotWritten("/adapter.txt", "adapter.txt");
    if (haveGraph) {
        if (!writeDriveFile("/noise.svg", [](WriteFn w, void* c) { noisesvg::Out o{w, c}; renderNoiseSvg(o); })) {
            FFat.remove("/noise.svg");
            logf("drive: noise.svg did not fit on the drive");
        }
    } else {
        FFat.remove("/noise.svg");  // never leave an old picture to be taken for the current one
    }
    // events.txt: if the whole log does not fit, the oldest lines are left out (with a note).
    {
        uint32_t skip = 0;
        size_t need = 0;
        bool ok = false;
        const uint32_t lines = eventLog.ok() ? eventLog.lineCount() : 0;
        for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
            ok = writeDriveFile("/events.txt", [&](WriteFn w, void* c) { evlog::Out o{w, c}; eventLog.render(o, upS(), skip); }, &need);
            if (ok || !lines) break;
            const size_t room = FFat.totalBytes() - FFat.usedBytes();
            const size_t perLine = need / (lines - skip + 1) + 1;
            const size_t over = need + 16384 > room ? need + 16384 - room : 4096;
            skip += uint32_t(over / perLine) + 50;
            if (skip >= lines) skip = lines - 1;
        }
        if (!ok) writeNotWritten("/events.txt", "events.txt");
        else if (skip) logf("drive: events.txt shortened: the oldest %lu lines left out (drive nearly full)", (unsigned long)skip);
    }
    // report.html: everything at a glance in one page.
    auto reportText = [haveGraph](WriteFn w, void* c) {
        report::Out o{w, c};
        char title[64];
        snprintf(title, sizeof(title), "Wireless USB Audio Extender - Extender %s %u", speakerMode() ? "Speaker" : "Mic", board::micNumber(pairId));
        report::begin(o, title);
        report::heading(o, "Status");
        o.printf("<pre>");
        struct Esc { WriteFn w; void* c; } esc{w, c};
        evlog::Out pre{[](void* x, const char* t) {
                           Esc* e = static_cast<Esc*>(x);
                           report::Out r{e->w, e->c};
                           report::escaped(r, t);
                       }, &esc};
        renderStatus(pre);
        o.printf("</pre>\n");
        report::heading(o, "Background noise (noise filter)");
        if (haveGraph) renderNoiseSvg(o);
        else {
            char why[160];
            snprintf(why, sizeof(why), "No graph: %s.", noiseWhy);
            report::note(o, why, "none");
        }
        report::logSections(o, eventLog, upS());
        report::end(o);
    };
    if (!writeDriveFile("/report.html", reportText)) writeNotWritten("/report.html", "report.html");
    logf("drive files written in %.1f s (%u KB free on the drive)", (millis() - t0) / 1000.0f,
         unsigned((FFat.totalBytes() - FFat.usedBytes()) / 1024));
}

// BOOT click: the drive's files should show the remote as it is now, so wait (up to 15 s,
// the loop keeps running) for the link, a fresh status from the remote and, when a graph
// can be drawn, its noise signature. Then write the files and restart as the drive.
static uint32_t driveClickMs = 0, driveStatusAt = 0;
static void serviceDriveOpening() {
    if (!driveOpening) return;
    const uint32_t now = millis();
    const bool fresh = linked && statusCount.load() != driveStatusAt;
    wlink::StatusPacket s;
    portENTER_CRITICAL(&linkMux);
    memcpy(&s, &remoteStatus, sizeof(s));
    portEXIT_CRITICAL(&linkMux);
    const bool graph = fresh && noiseWanted(s, fresh);
    static uint32_t lastAsk = 0;
    if (graph && !haveNoise && now - lastAsk > 500) {
        lastAsk = now;
        wlink::Header h;
        wlink::fill(h, wlink::kNoiseRequest, pairId);
        esp_now_send(remoteMac, (const uint8_t*)&h, sizeof(h));
    }
    const bool done = fresh && (!graph || haveNoise);
    if (!done && now - driveClickMs < 15000) return;
    driveRemoteLate = !fresh;
    if (!fresh) noiseWanted(s, false);  // sets noiseWhy
    bool haveGraph = graph && haveNoise && noise.valid;
    if (graph && haveNoise && !noise.valid) snprintf(noiseWhy, sizeof(noiseWhy), "the remote had no learned signature to send");
    else if (graph && !haveNoise) snprintf(noiseWhy, sizeof(noiseWhy), "the remote did not send its signature in time");
    if (!haveGraph && !noiseWhy[0]) snprintf(noiseWhy, sizeof(noiseWhy), "not available");
    logf("BOOT: %s; writing the drive files", done ? "remote status in" : "remote did not answer within 15 s");
    ev(evlog::kDriveOpened);
    writeDriveFiles(haveGraph);
    settings.setFlag("drive");
    restartNow("opening the EXTENDER drive (BOOT pressed)", 0);
}

// BOOT: click = EXTENDER drive (serviceDriveOpening()), hold 10 s = factory reset.
static void runButton() {
    switch (bootButton.update(millis(), digitalRead(board::buttonPin()) == LOW)) {
        case HoldButton::Action::Click:
            if (driveOpening) break;
            driveOpening = true;
            driveClickMs = millis();
            driveStatusAt = statusCount.load();
            haveNoise = false;
            logf("BOOT: opening the EXTENDER drive - waiting up to 15 s for the remote's status first");
            break;
        case HoldButton::Action::Hold10:
            esp_task_wdt_delete(nullptr);  // erasing files may take a few seconds
            ev(evlog::kFactoryReset);      // the event log itself is kept
            settings.factoryReset();
            announcer::eraseVoiceFiles();  // also removes config.txt: recreated with the defaults
            restartNow("factory reset (held BOOT 10 s): settings, config.txt and voice files", 3);
            break;
        default: break;
    }
}

// Keeps the remote on config.txt's settings. A new mic number or channel is a
// handover: sent on the current link until the remote confirms, then both
// restart on the new one. Filter and adapt speed are just kept in sync.
static void syncRemote() {
    const uint32_t now = millis();
    if (ackPending) {
        wlink::ConfigAckPacket a;
        portENTER_CRITICAL(&linkMux);
        memcpy(&a, &lastAck, sizeof(a));
        ackPending = false;
        portEXIT_CRITICAL(&linkMux);
        if (a.restarting) {
            settings.put("pair", a.pairId);
            settings.put("chan", a.channel);
            logEvent("remote switched to mic %u / channel %u", board::micNumber(a.pairId), a.channel);
            evText(0, "handover: now mic %u / channel %u, restarting", board::micNumber(a.pairId), a.channel);
            restartNow("handover done", 0);
        }
    }
    // Never move without the remote: wait on the current setting (where the remote is told
    // and confirms), and look for it on the new setting in case it is already there.
    if (handoverPending && heardOnNew) {
        heardOnNew = false;
        probing = false;
        logEvent("remote heard on mic %u / channel %u already: moving there", board::micNumber(wantPair), wantChannel);
        evText(0, "handover: remote already on mic %u / channel %u, moving there", board::micNumber(wantPair), wantChannel);
        settings.put("pair", wantPair);
        settings.put("chan", wantChannel);
        restartNow("handover done", 0);
    }
    // Locked board: its own remote heard on the factory setting (reset with BOOT 10 s).
    if (heardOnFactory && config.lock && !linked) {
        heardOnFactory = false;
        probing = false;
        probeKind = kProbeNone;
        logEvent("remote %s heard on the factory setting (mic 1 / channel %u, after its BOOT reset?): moving there to "
                 "hand it back to mic %u / channel %u", cfg::remoteText(config), wlink::kDefaultChannel, board::micNumber(wantPair), wantChannel);
        evText(0, "locked remote %s found on mic 1 / channel %u: moving there, then handing it over", cfg::remoteText(config),
               wlink::kDefaultChannel);
        settings.put("pair", 0);
        settings.put("chan", wlink::kDefaultChannel);
        restartNow("following the remote to the factory setting", 0);
    }
    heardOnFactory = false;
    static uint32_t probeEnd = 0, nextProbe = 0, unlinkedSince = 0;
    if (linked) unlinkedSince = 0;
    else if (!unlinkedSince) unlinkedSince = now ? now : 1;
    if (probing && int32_t(now - probeEnd) >= 0) {
        probing = false;
        probeKind = kProbeNone;
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    // 1.5 s on another channel every 30 s (the remote beacons every 0.2 s while unlinked):
    // the new channel of a pending handover, or (locked board, link down 30 s) the factory one.
    const bool atFactory = pairId == 0 && channel == wlink::kDefaultChannel;
    const bool wantFactory = config.lock && !handoverPending && !atFactory && unlinkedSince && now - unlinkedSince > 30000 &&
                             channel != wlink::kDefaultChannel;
    const bool wantHandover = handoverPending && wantChannel != channel && now - handoverSince > 20000;
    if (!linked && !probing && (wantHandover || wantFactory) && int32_t(now - nextProbe) >= 0) {
        static bool toldHandover = false, toldFactory = false;
        const uint8_t ch = wantHandover ? wantChannel : wlink::kDefaultChannel;
        if (wantHandover && !toldHandover) {
            toldHandover = true;
            logEvent("remote not found yet: also listening on channel %u every 30 s in case it is already there", wantChannel);
        } else if (!wantHandover && !toldFactory) {
            toldFactory = true;
            logEvent("remote %s not heard for 30 s: also listening on the factory channel %u every 30 s in case it was reset",
                     cfg::remoteText(config), wlink::kDefaultChannel);
        }
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        probeKind = wantHandover ? kProbeHandover : kProbeFactory;
        probing = true;
        probeEnd = now + 1500;
        nextProbe = now + 30000;
    }
    if (!linked) return;
    bool need = handoverPending || !haveStatus;
    if (!need) {
        const bool lockedHere = !wlink::macIsZero(remoteStatus.lockedTo) && memcmp(remoteStatus.lockedTo, myMac, 6) == 0;
        need = remoteStatus.filterMode != config.filter ||
               remoteStatus.adapt != config.adapt || remoteStatus.txPowerDbm != config.txPower ||
               remoteStatus.cpuMhz != config.cpuMhz || remoteStatus.highpassOn != config.highpass ||
               remoteStatus.highpassHz != config.highpassHz ||
               remoteStatus.temperatureOn != config.temperature || remoteStatus.micLevel != config.micLevel ||
               remoteStatus.distance != config.distance || remoteStatus.modeSetting != config.mode ||
               lockedHere != config.lock;
    }
    static uint32_t lastSent = 0;
    if (!need || now - lastSent < (handoverPending ? 500u : 2000u)) return;
    lastSent = now;
    wlink::ConfigPacket c;
    wlink::fill(c.h, wlink::kConfig, pairId);
    c.seq = ++configSeq;
    c.newPairId = wantPair;
    c.newChannel = wantChannel;
    c.filterMode = config.filter;
    c.adapt = config.adapt;
    c.txPowerDbm = config.txPower;
    c.cpuMhz = config.cpuMhz;
    c.highpassOn = config.highpass;
    c.highpassHz = config.highpassHz;
    c.temperatureOn = config.temperature;
    c.micLevel = config.micLevel;
    c.distance = config.distance;
    c.modeSetting = config.mode;
    c.lock = config.lock ? 1 : 0;
    esp_now_send(remoteMac, (const uint8_t*)&c, sizeof(c));
}

// The drive's volume label, "EXTENDER<mic number>", so several drives plugged in at once
// are told apart (Linux mounts it as /media/<user>/EXTENDER1, Windows shows the name).
// Set again whenever it differs: a new mic number renames the drive at the next start.
static void setDriveLabel(uint8_t mic) {
    char want[12];
    snprintf(want, sizeof(want), "EXTENDER%u", mic);
    for (int d = 0; d < 4; ++d) {  // the FatFs drive number FFat mounted ("0:", usually)
        char drive[3] = {char('0' + d), ':', 0};
        char label[24] = "";
        DWORD serial = 0;
        if (f_getlabel(drive, label, &serial) != FR_OK) continue;
        if (strcmp(label, want) == 0) return;
        char set[16];
        snprintf(set, sizeof(set), "%d:%s", d, want);
        const FRESULT r = f_setlabel(set);
        logf("drive label: %s -> %s%s", label[0] ? label : "(none)", want, r == FR_OK ? "" : " FAILED");
        return;
    }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
    // First of all: keep the lines from before this start (see LastLines), then start a new set.
    if (esp_reset_reason() != ESP_RST_POWERON && lastLines.magic == LastLines::kMagic &&
        lastLines.count <= LastLines::kLines && lastLines.next < LastLines::kLines) {
        memcpy(&prevLines, &lastLines, sizeof(prevLines));
        havePrevLines = prevLines.count > 0;
    }
    memset(&lastLines, 0, sizeof(lastLines));
    lastLines.magic = LastLines::kMagic;
    led.begin();
    pinMode(board::buttonPin(), INPUT_PULLUP);
    settings.begin();
    driveMode = settings.takeFlag("drive");
    startEventLog(driveMode);
    // The link in use: changes only when a handover completes.
    pairId = settings.get("pair", 0) < wlink::kPairIds ? settings.get("pair", 0) : 0;
    channel = settings.get("chan", wlink::kDefaultChannel);
    if (channel < wlink::kMinChannel || channel > wlink::kMaxChannel) channel = wlink::kDefaultChannel;
    // config.txt (not in drive mode: then the PC owns the FAT partition)
    if (!driveMode) {
        if (mountDrive()) readConfig(pairId, channel);
        else logf("config.txt: no FAT partition (flash the full image); using the saved settings");
        wantPair = uint8_t(config.mic - 1);
        wantChannel = config.channel;
        handoverPending = wantPair != pairId || wantChannel != channel;
        handoverSince = millis();
        board::setCpuMhz(config.cpuMhz);
        setDriveLabel(board::micNumber(pairId));
        // Microphone or speaker: config.txt; with auto, the remote's last decision (first start: microphone),
        // so the PC has its audio device from the first second, never waiting for the remote.
        const uint8_t saved = settings.get("amode", wlink::kModeMicrophone) == wlink::kModeSpeaker ? wlink::kModeSpeaker
                                                                                                    : wlink::kModeMicrophone;
        audioMode = config.mode == wlink::kModeSetSpeaker      ? wlink::kModeSpeaker
                    : config.mode == wlink::kModeSetMicrophone ? wlink::kModeMicrophone
                                                               : saved;
        settings.put("amode", audioMode);
    }

    // Name the USB device after its number, then start USB: microphone + serial
    // port normally, or drive + serial port in EXTENDER mode.
    static char name[32];
    if (driveMode) {
        snprintf(name, sizeof(name), "Extender Mic %u drive", board::micNumber(pairId));
    } else if (speakerMode()) {
        snprintf(name, sizeof(name), "Extender Speaker %u", board::micNumber(pairId));
        usbspkdev::registerInterface(name);
    } else {
        snprintf(name, sizeof(name), "Extender Mic %u", board::micNumber(pairId));
        usbmicdev::attach(&jitter);
        usbmicdev::attachMixer(&voiceMixer);
        usbmicdev::registerInterface(name);
    }
    USB.productName(name);
    USB.manufacturerName("Wireless USB Audio Extender");
    USB.firmwareVersion(0x0142);
    USB.onEvent(onUsbEvent);
    LogPort.setTxTimeoutMs(20);  // never stall when no terminal is open
    LogPort.onEvent(onSerialEvent);
    LogPort.begin();
    if (driveMode) {
        const bool ok = voicedrive::begin(logf);
        USB.begin();
        logf("Wireless USB Audio Extender local %s | EXTENDER DRIVE: edit config.txt or the voice files, then eject the drive",
             kFirmware);
        if (!ok) logf("drive: not available; press RESET to return to the microphone");
        esp_task_wdt_add(nullptr);  // loop() stopping for 5 s restarts the board
        return;
    }
    USB.begin();

    led.blinkMicNumber(board::micNumber(pairId));
    remoteLogs = xQueueCreate(24, sizeof(LogText));
    adapterRecLen = uint16_t(settings.getBytes("adapter", adapterRec, sizeof(adapterRec)));
    if (!adinfo::recordSize(adapterRec, adapterRecLen)) adapterRecLen = 0;  // none, or an older format

    const bool radioOk = board::startRadio(channel, onRecv, nullptr);
    board::setTxPower(config.txPower);
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    logf("Wireless USB Audio Extender local %s | %s | MAC %s", kFirmware, board::kindName(), board::macText(myMac).c_str());
    logf("local settings: %s %u | channel %u | %u MHz | distance %s (%s) | remote %s", speakerMode() ? "speaker" : "mic",
         board::micNumber(pairId), channel, unsigned(getCpuFrequencyMhz()), wlink::distanceName(config.distance),
         wlink::distanceRateText(config.distance), cfg::remoteText(config));
    if (!radioOk) logf("ERROR: ESP-NOW v2 start failed (needs Arduino-ESP32 core 3.2 or newer)");
    logf("started by: %s", board::resetReasonText());
    if (eventLog.ok()) logf("event log: start %u (events.txt on the EXTENDER drive)", eventLog.start());
    else logf("event log: no flash area for it - flash the full image once to create it");
    if (havePrevLines) {
        logf("---- last %u lines before this start ----", prevLines.count);
        const int first = (prevLines.next - prevLines.count + LastLines::kLines) % LastLines::kLines;
        for (int i = 0; i < prevLines.count; ++i) {
            char* l = prevLines.line[(first + i) % LastLines::kLines];
            l[sizeof(prevLines.line[0]) - 1] = 0;
            char line[160];
            snprintf(line, sizeof(line), "  | %s", l);
            emit(line);
        }
        logf("---- end of the lines before this start ----");
    }
    if (const char* why = board::unusualResetText()) logEvent("%s", why);
    if (handoverPending)
        logEvent("config.txt asks for mic %u / channel %u: waiting for the remote to move together (turn it on)",
             board::micNumber(wantPair), wantChannel);
    announcer::begin(&voiceMixer, speakerMode() ? pcNever : pcRecording, logf);
    announcer::setEventLog(logEvent);
    if (speakerMode()) {
        announcer::setMode(announcer::Mode::Off);
        logf("audio feedback: off in speaker mode");
        xTaskCreatePinnedToCore(speakerTask, "spk_tx", 4096, nullptr, 5, nullptr, 1);
    } else {
        announcer::setMode(config.feedback == cfg::kFeedbackOff ? announcer::Mode::Off : announcer::Mode::Voice);
        logf("audio feedback: %s (config.txt)", announcer::modeText(announcer::mode()));
    }
    logf("mode: %s (config.txt mode = %s)", speakerMode() ? "speaker" : "microphone", cfg::modeName(config.mode));
    if (config.lock) logf("pairing: links only to remote %s (config.txt remote)", cfg::remoteText(config));
    logf("waiting for remote %u", board::micNumber(pairId));
    esp_task_wdt_add(nullptr);  // loop() stopping for 5 s restarts the board
}

// Voice-files mode: only the drive, the log and the LED (on, short dark blink each second).
static void driveLoop() {
    esp_task_wdt_reset();
    if (replayRequested) { replayRequested = false; delay(100); replayHistory(); }
    led.raw((millis() % 1000) > 120);
    if (voicedrive::ejected()) {
        logf("voice files: drive ejected, restarting as the microphone");
        delay(500);
        ESP.restart();
    }
    delay(5);
}

void loop() {
    if (driveMode) { driveLoop(); return; }
    esp_task_wdt_reset();
    const uint32_t now = millis();
    static uint32_t lastPing = 0;
    static bool wasRecording = false;

    runButton();
    usbWatch(now);
    syncRemote();
    syncAdapterInfo();
    const bool recording = pcUsing();
    // The PC just opened the mic: say which one it is. Its software may open it several
    // times while starting, so at most once a minute.
    static uint32_t lastMicSaid = 0;
    if (!speakerMode() && recording && !wasRecording && (!lastMicSaid || now - lastMicSaid > 60000)) {
        lastMicSaid = now ? now : 1;
        announcer::say(announcer::Event(uint8_t(announcer::Event::Mic1) + pairId));
    }
    wasRecording = recording;

    if (candidatePending && !linked) {
        uint8_t mac[6];
        portENTER_CRITICAL(&linkMux);
        memcpy(mac, candidateMac, 6);
        candidatePending = false;
        portEXIT_CRITICAL(&linkMux);
        if (board::addPeer(mac, true, config.distance)) {
            memcpy(remoteMac, mac, 6);
            lastRxMs = now;
            linked = true;
            sendSmall(wlink::kHello);
            logEvent("linked to remote %s", board::macText(mac).c_str());
            ev(evlog::kLinkUp, board::micNumber(pairId), 0, droppedAtMs ? now - droppedAtMs : 0);
            if (droppedAtMs) {
                logEvent("link back after %.1f s (local radio restart %s; remote beacons heard while down: %lu)",
                         (now - droppedAtMs) / 1000.0f, radioRestartedInDrop ? "done before" : "not needed",
                         (unsigned long)heardWhileDown.load());
                droppedAtMs = 0;
            }
            announcer::say(announcer::Event::LinkUp);
        }
    }
    if (linked && helloAgain) {
        helloAgain = false;
        sendSmall(wlink::kHello);
    }
    if (linked && int32_t(millis() - lastRxMs) > int32_t(wlink::kLinkTimeoutMs)) dropLink("no packets for 2 s");
    // A link that had worked and stays down: restart the radio after 15 s, then every 5 min.
    static bool everLinked = false, wasLinked = false;
    static uint32_t nextRadioRestart = 0;
    if (linked) everLinked = true;
    if (wasLinked && !linked) nextRadioRestart = now + wlink::kRecoverAfterMs;
    wasLinked = linked;
    if (!linked && everLinked && int32_t(now - nextRadioRestart) >= 0) {
        restartRadio();
        radioRestartedInDrop = true;
        nextRadioRestart = now + wlink::kRecoverRepeatMs;
    }
    if (linked && now - lastPing >= wlink::kPingMs) {
        lastPing = now;
        sendSmall(wlink::kPing);
    }

    if (replayRequested) {
        replayRequested = false;
        delay(100);  // let the monitor finish opening
        replayHistory();
    }

    LogText t;
    while (remoteLogs && xQueueReceive(remoteLogs, &t, 0) == pdTRUE) {
        t.text[sizeof(t.text) - 1] = 0;
        char line[200];
        snprintf(line, sizeof(line), "        [remote] %s", t.text);
        emit(line);
        const char* txt = t.text;  // drop the remote's own uptime stamp ("    1.7 ...")
        while (*txt == ' ') txt++;
        const char* q = txt;
        while ((*q >= '0' && *q <= '9') || *q == '.') q++;
        if (q != txt && *q == ' ') txt = q + 1;
        // A long line arrives in pieces, the later ones starting with "... ": they are joined
        // back onto a critical first piece and logged as one event (flushRemotePending()).
        if (strncmp(txt, "... ", 4) == 0) {
            if (remotePending[0]) {
                const size_t len = strlen(remotePending);
                snprintf(remotePending + len, sizeof(remotePending) - len, " %s", txt + 4);
                remotePendingMs = millis();
            }
        } else {
            flushRemotePending();
            if (remoteLineIsCritical(txt)) {
                strlcpy(remotePending, txt, sizeof(remotePending));
                remotePendingAt = upS();
                remotePendingMs = millis();
            }
        }
    }
    if (remotePending[0] && millis() - remotePendingMs > 500) flushRemotePending();
    reportRemoteChanges();
    checkMode();
    reportPairing();
    serviceDriveOpening();
    reportStats();
    reportTemperatures();
    reportHourly();
    reportMemory();
    updateLed();
    delay(2);
}
