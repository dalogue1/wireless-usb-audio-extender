// USB audio (UAC1) host driver, version 1.3.
#include "UsbMic.h"
#include "UacDescriptors.h"

#if USB_MIC_SUPPORTED
#include <atomic>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"

// Two tasks, as in Espressif's host examples:
//  - lib task: runs usb_host_lib_handle_events() (enumeration, port events);
//  - client task: owns the device. All control transfers, isochronous
//    callbacks and open/close run here, so device state has one owner.
// Capture: the isochronous callback converts each 1 ms packet to int32 samples and
// writes them to a stream buffer; the application's audio reader is its only reader.
// Playback: the isochronous OUT callback asks the application's source for the next
// samples of every 1 ms packet (normally 47-49, its clock-drift control) and resubmits.
namespace usbmic {
namespace {

constexpr int kTransfers = 4;          // in flight
constexpr int kPacketsPerTransfer = 8; // 8 ms per transfer, 32 ms queued at the device
constexpr size_t kStreamSamples = 4096; // ~85 ms at 48 kHz
constexpr uint8_t kReqSetCur = 0x01, kReqGetCur = 0x81, kReqGetMin = 0x82, kReqGetMax = 0x83, kReqGetRes = 0x84;
constexpr uint8_t kCsMute = 0x01, kCsVolume = 0x02, kCsAgc = 0x07, kCsSamplingFreq = 0x01;

struct LogLine { char text[160]; };

TaskHandle_t libTask = nullptr, clientTask = nullptr;
SemaphoreHandle_t libDone = nullptr, clientDone = nullptr, installed = nullptr;
StreamBufferHandle_t stream = nullptr;
QueueHandle_t logQueue = nullptr;
usb_host_client_handle_t client = nullptr;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

std::atomic<bool> hostRunning{false}, stopClient{false}, stopLib{false};
std::atomic<bool> isStreaming{false}, devPresent{false}, volumePending{false};
std::atomic<uint8_t> newDeviceAddr{0};
std::atomic<bool> deviceGone{false};
std::atomic<uint8_t> volumeSetting{kVolumeDeviceDefault};
std::atomic<uint8_t> wantDir{uint8_t(Direction::Capture)};
std::atomic<PlaybackSource> playSource{nullptr};
bool installOk = false;

// Client-task-owned device state.
usb_device_handle_t dev = nullptr;
uint8_t devAddr = 0;
uint8_t openDir = uint8_t(Direction::Capture);  // direction the device was opened in
uac::Capture cap;  // the stream in use (capture or playback)
bool ifClaimed = false;
usb_transfer_t* ctrl = nullptr;
usb_transfer_t* iso[kTransfers] = {};
std::atomic<int> inFlight{0};
std::atomic<bool> closing{false};
int32_t convertBuf[256];
volatile bool ctrlDone = false;

Status st; // guarded by statusMux
Descriptors desc; // guarded by statusMux; written when a device is opened

void usbLog(const char* fmt, ...) {
    if (!logQueue) return;
    LogLine line;
    va_list ap; va_start(ap, fmt);
    vsnprintf(line.text, sizeof(line.text), fmt, ap);
    va_end(ap);
    xQueueSend(logQueue, &line, 0);
}

void setProblem(const char* text) {
    portENTER_CRITICAL(&statusMux);
    strlcpy(st.problem, text, sizeof(st.problem));
    portEXIT_CRITICAL(&statusMux);
}

// ---------------- control transfers (client task only) ----------------
void ctrlCallback(usb_transfer_t*) { ctrlDone = true; }

// Sends one class request; data is copied in (OUT) or out (IN). Pumps client
// events while waiting, so isochronous callbacks keep running.
bool control(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
             uint8_t* data, uint16_t wLength) {
    if (!dev || !ctrl || wLength > 56) return false;
    usb_setup_packet_t* s = (usb_setup_packet_t*)ctrl->data_buffer;
    s->bmRequestType = bmRequestType;
    s->bRequest = bRequest;
    s->wValue = wValue;
    s->wIndex = wIndex;
    s->wLength = wLength;
    if (!(bmRequestType & 0x80) && wLength) memcpy(ctrl->data_buffer + sizeof(usb_setup_packet_t), data, wLength);
    ctrl->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    ctrl->device_handle = dev;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = ctrlCallback;
    ctrlDone = false;
    if (usb_host_transfer_submit_control(client, ctrl) != ESP_OK) return false;
    const uint32_t start = millis();
    while (!ctrlDone && millis() - start < 1000) usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    if (!ctrlDone || ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return false;
    if ((bmRequestType & 0x80) && wLength) {
        const int got = ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (got < wLength) return false;
        memcpy(data, ctrl->data_buffer + sizeof(usb_setup_packet_t), wLength);
    }
    return true;
}

bool setInterface(uint8_t ifNum, uint8_t alt) {
    // Standard SET_INTERFACE (bmRequestType 0x01: host-to-device, interface).
    return control(0x01, USB_B_REQUEST_SET_INTERFACE, alt, ifNum, nullptr, 0);
}

uint16_t fuIndex() { return uint16_t(cap.featureUnit) << 8 | cap.controlInterface; }

bool fuGet(uint8_t req, uint8_t cs, uint8_t ch, int16_t& v) {
    uint8_t b[2] = {0, 0};
    if (!control(0xA1, req, uint16_t(cs) << 8 | ch, fuIndex(), b, 2)) return false;
    v = int16_t(b[0] | (b[1] << 8));
    return true;
}

bool fuSet(uint8_t cs, uint8_t ch, int16_t v, uint8_t len) {
    uint8_t b[2] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF)};
    return control(0x21, kReqSetCur, uint16_t(cs) << 8 | ch, fuIndex(), b, len);
}

void applyVolume() {
    if (!cap.featureUnit || cap.volumeChannel == 0xFF) return;
    const uint8_t pct = volumeSetting.load();
    int16_t lo, hi, res = 0, now = 0;
    portENTER_CRITICAL(&statusMux);
    lo = st.volMin; hi = st.volMax; res = st.volRes;
    portEXIT_CRITICAL(&statusMux);
    if (pct != kVolumeDeviceDefault && hi > lo) {
        const int16_t want = uac::volumeForPercent(lo, hi, res, pct);
        // Every channel with a volume control (a stereo speaker: both sides alike).
        const uint8_t mask = cap.volumeMask ? cap.volumeMask : uint8_t(1u << (cap.volumeChannel & 7));
        for (uint8_t ch = 0; ch < 8; ++ch)
            if ((mask & (1u << ch)) && !fuSet(kCsVolume, ch, want, 2)) usbLog("USB audio: setting the level failed (channel %u)", ch);
    }
    if (fuGet(kReqGetCur, kCsVolume, cap.volumeChannel, now)) {
        portENTER_CRITICAL(&statusMux);
        st.volCur = now;
        portEXIT_CRITICAL(&statusMux);
    }
}

// ---------------- isochronous capture ----------------
void isoCallback(usb_transfer_t* t) {
    const bool ok = t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (ok) {
        uint32_t good = 0, bad = 0, over = 0;
        size_t offset = 0;
        for (int i = 0; i < t->num_isoc_packets; ++i) {
            const usb_isoc_packet_desc_t& p = t->isoc_packet_desc[i];
            if (p.status == USB_TRANSFER_STATUS_COMPLETED && p.actual_num_bytes > 0) {
                const size_t n = uac::unpack(t->data_buffer + offset, (size_t)p.actual_num_bytes,
                                             cap.channels, cap.subframe, convertBuf,
                                             sizeof(convertBuf) / sizeof(convertBuf[0]));
                const size_t bytes = n * sizeof(int32_t);
                if (n && xStreamBufferSend(stream, convertBuf, bytes, 0) != bytes) over++;
                good++;
            } else if (p.status != USB_TRANSFER_STATUS_COMPLETED) {
                bad++;
            }
            offset += (size_t)p.num_bytes;
        }
        portENTER_CRITICAL(&statusMux);
        st.packets += good; st.badPackets += bad; st.overflows += over;
        portEXIT_CRITICAL(&statusMux);
    } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) {
        // An erroneous ISOC transfer only has an overall status: count all its packets.
        portENTER_CRITICAL(&statusMux);
        st.badPackets += (uint32_t)t->num_isoc_packets;
        portEXIT_CRITICAL(&statusMux);
    }
    const bool resubmit = !closing.load() && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
                          t->status != USB_TRANSFER_STATUS_CANCELED;
    if (resubmit && usb_host_transfer_submit(t) == ESP_OK) return;
    inFlight.fetch_sub(1);
}

// ---------------- isochronous playback ----------------
// Fills every packet of an OUT transfer from the application's source (packed back
// to back, each packet's own size: 47-49 samples follow the source's drift control).
void fillOut(usb_transfer_t* t) {
    int16_t mono[64];
    size_t offset = 0;
    const PlaybackSource src = playSource.load();
    for (int i = 0; i < t->num_isoc_packets; ++i) {
        uint16_t n = src ? src(mono, 49) : 0;
        if (!n || n > 49) { n = 48; memset(mono, 0, sizeof(mono)); }
        const size_t bytes = uac::pack(mono, n, cap.channels, cap.subframe, t->data_buffer + offset, cap.maxPacket);
        t->isoc_packet_desc[i].num_bytes = int(bytes);
        offset += bytes;
    }
    t->num_bytes = int(offset);
}

void isoOutCallback(usb_transfer_t* t) {
    uint32_t good = 0, bad = 0;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i < t->num_isoc_packets; ++i) {
            if (t->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED) good++;
            else bad++;
        }
    } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED) {
        bad += uint32_t(t->num_isoc_packets);
    }
    portENTER_CRITICAL(&statusMux);
    st.packets += good; st.badPackets += bad;
    portEXIT_CRITICAL(&statusMux);
    const bool resubmit = !closing.load() && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
                          t->status != USB_TRANSFER_STATUS_CANCELED;
    if (resubmit) {
        fillOut(t);
        if (usb_host_transfer_submit(t) == ESP_OK) return;
    }
    inFlight.fetch_sub(1);
}

void freeTransfers() {
    for (int i = 0; i < kTransfers; ++i) {
        if (iso[i]) { usb_host_transfer_free(iso[i]); iso[i] = nullptr; }
    }
}

void closeDevice(bool present) {
    if (!dev) return;
    closing = true;
    if (inFlight.load() > 0) {
        usb_host_endpoint_halt(dev, cap.endpoint);
        usb_host_endpoint_flush(dev, cap.endpoint);
        const uint32_t start = millis();
        while (inFlight.load() > 0 && millis() - start < 500) usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        usb_host_endpoint_clear(dev, cap.endpoint);
    }
    if (present && ifClaimed) setInterface(cap.streamInterface, 0);
    if (ifClaimed) usb_host_interface_release(client, dev, cap.streamInterface);
    ifClaimed = false;
    if (inFlight.load() == 0) freeTransfers();
    else usbLog("USB mic: %d transfers did not return; leaking them", inFlight.load());
    usb_host_device_close(client, dev);
    dev = nullptr;
    devAddr = 0;
    closing = false;
    isStreaming = false;
    xStreamBufferReset(stream);
    portENTER_CRITICAL(&statusMux);
    st.streaming = false;
    st.connected = false;
    st.hasVolume = false;
    st.channels = 0; st.bits = 0; st.rate = 0;
    st.canCapture = st.canPlayback = false;
    portEXIT_CRITICAL(&statusMux);
}

void copyName(const usb_str_desc_t* s, char* out, size_t cap) {
    out[0] = 0;
    if (!s || s->bLength < 2) return;
    size_t n = (s->bLength - 2) / 2, o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i) {
        const uint16_t c = s->wData[i];
        out[o++] = (c >= 32 && c < 127) ? char(c) : '?';
    }
    out[o] = 0;
}

esp_err_t lastOpenError = ESP_OK;
uint32_t openFailures = 0;

void openDevice(uint8_t addr) {
    if (dev) return; // one microphone at a time
    const esp_err_t err = usb_host_device_open(client, addr, &dev);
    if (err != ESP_OK) {
        dev = nullptr;
        openFailures++;
        // Log the first failure and every change of error; the retry below repeats quietly.
        if (err != lastOpenError || openFailures == 1)
            usbLog("USB host: opening device at address %u failed: %s (0x%x)", addr, esp_err_to_name(err), err);
        lastOpenError = err;
        char why[64];
        snprintf(why, sizeof(why), "cannot open device: %s", esp_err_to_name(err));
        setProblem(why);
        return;
    }
    devAddr = addr;
    openDir = wantDir.load();
    const bool playback = openDir == uint8_t(Direction::Playback);
    if (openFailures) usbLog("USB host: device at address %u opened after %lu failed attempt(s)", addr,
                             (unsigned long)openFailures);
    openFailures = 0;
    lastOpenError = ESP_OK;
    const usb_device_desc_t* dd = nullptr;
    const usb_config_desc_t* cd = nullptr;
    usb_device_info_t info = {};
    usb_host_device_info(dev, &info);
    char product[48] = "", maker[32] = "";
    copyName(info.str_desc_product, product, sizeof(product));
    copyName(info.str_desc_manufacturer, maker, sizeof(maker));
    usb_host_get_device_descriptor(dev, &dd);
    if (usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK || !cd) {
        cap = uac::Capture();
        setProblem("could not read the configuration descriptor");
        return; // kept open (unused) so its removal is reported
    }
    const uac::Capture capIn = uac::parse((const uint8_t*)cd, cd->wTotalLength, uac::Dir::Capture);
    const uac::Capture capOut = uac::parse((const uint8_t*)cd, cd->wTotalLength, uac::Dir::Playback);
    cap = playback ? capOut : capIn;
    {   // Keep the descriptors for diagnostics (Descriptors / descriptors()).
        char serial[48] = "";
        copyName(info.str_desc_serial_num, serial, sizeof(serial));
        const size_t n = cd->wTotalLength < kMaxConfigDesc ? cd->wTotalLength : kMaxConfigDesc;
        portENTER_CRITICAL(&statusMux);
        desc.seq++;
        desc.speed = info.speed == USB_SPEED_LOW ? 0 : info.speed == USB_SPEED_FULL ? 1 : 2;
        if (dd) memcpy(desc.device, dd, sizeof(desc.device));
        else memset(desc.device, 0, sizeof(desc.device));
        desc.configTotal = cd->wTotalLength;
        desc.configLen = uint16_t(n);
        memcpy(desc.config, cd, n);
        strlcpy(desc.manufacturer, maker, sizeof(desc.manufacturer));
        strlcpy(desc.product, product, sizeof(desc.product));
        strlcpy(desc.serial, serial, sizeof(desc.serial));
        const bool usable = cap.found && cap.rate48k;
        desc.usedInterface = usable ? cap.streamInterface : 0xFF;
        desc.usedAlt = usable ? cap.alt : 0xFF;
        desc.usedEndpoint = usable ? cap.endpoint : 0xFF;
        portEXIT_CRITICAL(&statusMux);
    }
    portENTER_CRITICAL(&statusMux);
    st.connected = true;
    st.vid = dd ? dd->idVendor : 0; st.pid = dd ? dd->idProduct : 0;
    snprintf(st.name, sizeof(st.name), "%s%s%s", maker, (maker[0] && product[0]) ? " " : "", product);
    {   // some adapters pad their strings with spaces: collapse runs of spaces
        char* w = st.name;
        for (const char* r = st.name; *r; ++r)
            if (!(*r == ' ' && (w == st.name || w[-1] == ' '))) *w++ = *r;
        while (w > st.name && w[-1] == ' ') --w;
        *w = 0;
    }
    st.connects++;
    st.canCapture = capIn.found && capIn.rate48k;
    st.canPlayback = capOut.found && capOut.rate48k;
    st.direction = openDir;
    portEXIT_CRITICAL(&statusMux);
    devPresent = true;
    const char* why = nullptr;
    if (!cap.found && cap.uac2) why = "USB Audio Class 2 only (needs UAC1)";
    else if (!cap.found) why = playback ? "no USB audio speaker (playback) interface" : "no USB audio microphone interface";
    else if (!cap.rate48k) why = playback ? "speaker does not offer 48 kHz" : "microphone does not offer 48 kHz";
    else if (info.speed == USB_SPEED_LOW) why = "low-speed device";
    if (why) {
        setProblem(why);
        usbLog("USB device %04x:%04x \"%s\" not used: %s", st.vid, st.pid, st.name, why);
        return; // kept open (unused) so its removal is reported
    }
    if (usb_host_interface_claim(client, dev, cap.streamInterface, cap.alt) != ESP_OK) {
        setProblem("could not claim the audio interface");
        usbLog("USB audio: claiming interface %u alt %u failed", cap.streamInterface, cap.alt);
        return; // kept open (unused) so its removal is reported
    }
    ifClaimed = true;
    if (!setInterface(cap.streamInterface, cap.alt)) usbLog("USB mic: SET_INTERFACE failed (continuing)");
    if (cap.rateControl) {
        uint8_t f[3] = {uint8_t(uac::kRate & 0xFF), uint8_t((uac::kRate >> 8) & 0xFF), uint8_t(uac::kRate >> 16)};
        if (!control(0x22, kReqSetCur, uint16_t(kCsSamplingFreq) << 8, cap.endpoint, f, 3))
            usbLog("USB mic: setting 48 kHz failed (continuing)");
    }
    // Feature Unit: AGC off (keep a steady recording level), unmute, read range.
    bool hasVol = false;
    int16_t lo = 0, hi = 0, res = 0, now = 0;
    if (cap.featureUnit) {
        if (!playback && cap.agcChannel != 0xFF && !fuSet(kCsAgc, cap.agcChannel, 0, 1)) usbLog("USB mic: AGC off failed");
        if (cap.muteChannel != 0xFF) fuSet(kCsMute, cap.muteChannel, 0, 1);
        if (cap.volumeChannel != 0xFF &&
            fuGet(kReqGetMin, kCsVolume, cap.volumeChannel, lo) &&
            fuGet(kReqGetMax, kCsVolume, cap.volumeChannel, hi)) {
            hasVol = hi > lo;
            fuGet(kReqGetRes, kCsVolume, cap.volumeChannel, res);
            fuGet(kReqGetCur, kCsVolume, cap.volumeChannel, now);
        }
    }
    portENTER_CRITICAL(&statusMux);
    st.hasVolume = hasVol; st.volMin = lo; st.volMax = hi; st.volRes = res; st.volCur = now;
    portEXIT_CRITICAL(&statusMux);
    if (hasVol) applyVolume();
    // Isochronous IN (capture) or OUT (playback): one packet per (micro)frame interval.
    const size_t bytes = size_t(cap.maxPacket) * kPacketsPerTransfer;
    bool ok = true;
    for (int i = 0; i < kTransfers && ok; ++i) {
        ok = usb_host_transfer_alloc(bytes, kPacketsPerTransfer, &iso[i]) == ESP_OK;
        if (!ok) break;
        usb_transfer_t* t = iso[i];
        t->device_handle = dev;
        t->bEndpointAddress = cap.endpoint;
        t->callback = playback ? isoOutCallback : isoCallback;
        t->num_bytes = (int)bytes;
        for (int p = 0; p < kPacketsPerTransfer; ++p) t->isoc_packet_desc[p].num_bytes = cap.maxPacket;
        if (playback) fillOut(t);
    }
    xStreamBufferReset(stream);
    closing = false;
    for (int i = 0; i < kTransfers && ok; ++i) {
        inFlight.fetch_add(1);
        if (usb_host_transfer_submit(iso[i]) != ESP_OK) { inFlight.fetch_sub(1); ok = false; }
    }
    if (!ok) {
        setProblem("could not start the isochronous stream");
        usbLog("USB mic: starting the stream failed");
        // Stop what was submitted, but keep the device open so removal is reported.
        closing = true;
        usb_host_endpoint_halt(dev, cap.endpoint);
        usb_host_endpoint_flush(dev, cap.endpoint);
        const uint32_t t0 = millis();
        while (inFlight.load() > 0 && millis() - t0 < 500) usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        usb_host_endpoint_clear(dev, cap.endpoint);
        closing = false;
        return;
    }
    setProblem("");
    portENTER_CRITICAL(&statusMux);
    st.streaming = true;
    st.channels = cap.channels; st.bits = cap.bits; st.rate = uac::kRate;
    portEXIT_CRITICAL(&statusMux);
    isStreaming = true;
    char level[48] = "no level control";
    if (hasVol) snprintf(level, sizeof(level), "level %.1f dB (range %.1f..%.1f dB)",
                         now / 256.0f, lo / 256.0f, hi / 256.0f);
    static const char* const kSync[] = {"", ", asynchronous", ", adaptive", ", synchronous"};
    usbLog("USB %s connected: %04x:%04x \"%s\", 48000 Hz, %u ch, %u-bit%s, %s", playback ? "speaker" : "microphone",
         st.vid, st.pid, st.name, cap.channels, cap.bits, playback ? kSync[cap.syncType & 3] : "", level);
}

void clientEvent(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) newDeviceAddr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) deviceGone = true;
}

void clientTaskFn(void*) {
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 5;
    cfg.async.client_event_callback = clientEvent;
    cfg.async.callback_arg = nullptr;
    if (usb_host_client_register(&cfg, &client) != ESP_OK) {
        client = nullptr;
        usbLog("USB host: client registration failed");
        xSemaphoreGive(clientDone);
        clientTask = nullptr;
        vTaskDelete(nullptr);
    }
    usb_host_transfer_alloc(64, 0, &ctrl);
    {   // A device enumerated before this client registered sends no NEW_DEV event.
        uint8_t addrs[4];
        int n = 0;
        if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK && n > 0) {
            usbLog("USB host: %d device(s) already on the bus", n);
            newDeviceAddr = addrs[0];
        }
    }
    // Per run of the host (the application may stop and restart the host at any time).
    uint32_t lastRetry = millis(), stuckSince = 0, attempts = 0;
    openFailures = 0;
    lastOpenError = ESP_OK;
    while (!stopClient.load()) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        if (deviceGone.exchange(false)) {
            const bool hadStream = isStreaming.load();
            closeDevice(false);
            devPresent = false;
            portENTER_CRITICAL(&statusMux);
            st.connected = false;
            st.name[0] = 0; st.vid = st.pid = 0;
            portEXIT_CRITICAL(&statusMux);
            setProblem("");
            usbLog(hadStream ? (openDir == uint8_t(Direction::Playback) ? "USB speaker disconnected"
                                                                        : "USB microphone disconnected; sending silence until it is plugged in again")
                           : "USB device removed");
        }
        const uint8_t addr = newDeviceAddr.exchange(0);
        if (addr) {
            portENTER_CRITICAL(&statusMux);
            st.plugIns++;
            portEXIT_CRITICAL(&statusMux);
            usbLog("USB host: new device at address %u", addr);
            openDevice(addr);
        }
        // A device on the bus that we have not opened (missed event, failed open): retry every 2 s.
        if (!dev && millis() - lastRetry > 2000) {
            lastRetry = millis();
            uint8_t addrs[4];
            int n = 0;
            if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK && n > 0) openDevice(addrs[0]);
            // A device that is on the bus but never finished enumerating (ESP-IDF
            // logs e.g. "CHECK_ADDR FAILED" and does not retry): switch the port
            // power off and on so it connects again from scratch. Back-off:
            // 3 s, 10 s, 30 s, then every 60 s while it keeps failing.
            usb_host_lib_info_t info = {};
            const bool stuck = n == 0 && usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0;
            if (!stuck) {
                stuckSince = 0;
                if (n > 0) attempts = 0;
            } else {
                if (!stuckSince) stuckSince = millis();
                const uint32_t wait = attempts == 0 ? 3000 : attempts == 1 ? 10000 : attempts == 2 ? 30000 : 60000;
                if (millis() - stuckSince >= wait) {
                    attempts++;
                    usbLog("USB host: device did not finish connecting; power-cycling the USB port (attempt %lu)",
                           (unsigned long)attempts);
                    usb_host_lib_set_root_port_power(false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    usb_host_lib_set_root_port_power(true);
                    stuckSince = 0;
                }
            }
        }
        if (volumePending.exchange(false) && dev && isStreaming.load()) applyVolume();
        // Direction changed (microphone <-> speaker): close the adapter and open it the other way.
        if (dev && openDir != wantDir.load()) {
            const uint8_t addr = devAddr;
            usbLog("USB audio: switching the adapter to %s", wantDir.load() == uint8_t(Direction::Playback) ? "playback" : "capture");
            closeDevice(true);
            if (addr) openDevice(addr);
        }
    }
    closeDevice(devPresent.load());
    if (ctrl) { usb_host_transfer_free(ctrl); ctrl = nullptr; }
    usb_host_client_deregister(client);
    client = nullptr;
    xSemaphoreGive(clientDone);
    clientTask = nullptr;
    vTaskDelete(nullptr);
}

// The Arduino-ESP32 core's ESP-IDF is built with
// CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK. With that option, a host
// installed WITHOUT a filter callback cancels the enumeration of EVERY device
// (enum.c: enum_proceed stays false), and only says so in a warning that the
// core compiles out. Symptom: the device is "on the bus" but never appears.
// Accept every device with its first configuration.
bool acceptAllDevices(const usb_device_desc_t*, uint8_t* bConfigurationValue) {
    *bConfigurationValue = 1;
    return true;
}

void libTaskFn(void*) {
    usb_host_config_t hc = {};
    hc.skip_phy_setup = false;
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
    hc.enum_filter_cb = acceptAllDevices;
#endif
    installOk = usb_host_install(&hc) == ESP_OK;
    xSemaphoreGive(installed);
    if (!installOk) { libTask = nullptr; vTaskDelete(nullptr); }
    // Shutdown: wait for the client to deregister, free the devices, then
    // uninstall. Counts come from usb_host_lib_info(), so an event flag that
    // arrived before stop was requested cannot be missed.
    bool freeRequested = false, allFree = false;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) allFree = true;
        if (!stopLib.load()) continue;
        usb_host_lib_info_t info = {};
        if (usb_host_lib_info(&info) != ESP_OK || info.num_clients > 0) continue;
        if (!freeRequested) {
            freeRequested = true;
            allFree = usb_host_device_free_all() == ESP_OK;
            continue;
        }
        if (allFree || info.num_devices == 0) break;
    }
    usb_host_uninstall();
    xSemaphoreGive(libDone);
    libTask = nullptr;
    vTaskDelete(nullptr);
}

bool ensureStatics() {
    if (!libDone) libDone = xSemaphoreCreateBinary();
    if (!clientDone) clientDone = xSemaphoreCreateBinary();
    if (!installed) installed = xSemaphoreCreateBinary();
    if (!logQueue) logQueue = xQueueCreate(8, sizeof(LogLine));
    // Kept for the life of the firmware so the audio reader never reads a freed buffer.
    if (!stream) {
        uint8_t* storage = (uint8_t*)heap_caps_malloc(kStreamSamples * sizeof(int32_t) + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        static StaticStreamBuffer_t sbStruct;
        if (storage) stream = xStreamBufferCreateStatic(kStreamSamples * sizeof(int32_t), sizeof(int32_t), storage, &sbStruct);
    }
    return libDone && clientDone && installed && logQueue && stream;
}

} // namespace

bool supported() { return true; }
int devicesOnBus() {
    if (!hostRunning.load()) return -1;
    usb_host_lib_info_t info = {};
    if (usb_host_lib_info(&info) != ESP_OK) return -1;
    return info.num_devices;
}

bool running() { return hostRunning.load(); }
bool streaming() { return isStreaming.load(); }

void setDirection(Direction dir) {
    wantDir = uint8_t(dir);
    if (client) usb_host_client_unblock(client);
}
Direction direction() { return Direction(wantDir.load()); }
void setPlaybackSource(PlaybackSource src) { playSource = src; }

bool start(uint8_t volumePercent, Direction dir) {
    wantDir = uint8_t(dir);
    volumeSetting = volumePercent;
    portENTER_CRITICAL(&statusMux);
    st.volumeSetting = volumePercent;
    portEXIT_CRITICAL(&statusMux);
    if (hostRunning.load()) { volumePending = true; flush(); return true; }
    if (!ensureStatics()) return false;
    stopClient = false; stopLib = false;
    xSemaphoreTake(libDone, 0); xSemaphoreTake(clientDone, 0); xSemaphoreTake(installed, 0);
    xStreamBufferReset(stream);
    // Core 0 sits with Wi-Fi; audio processing and loop() run on core 1.
    if (xTaskCreatePinnedToCore(libTaskFn, "usb_lib", 4096, nullptr, 6, &libTask, 0) != pdPASS) return false;
    if (xSemaphoreTake(installed, pdMS_TO_TICKS(2000)) != pdTRUE || !installOk) {
        usbLog("USB host: install failed (is USB Mode set to Hardware CDC and JTAG?)");
        return false;
    }
    if (xTaskCreatePinnedToCore(clientTaskFn, "usb_mic", 4096, nullptr, 7, &clientTask, 0) != pdPASS) {
        stopLib = true;
        xSemaphoreTake(libDone, pdMS_TO_TICKS(2000));
        return false;
    }
    hostRunning = true;
    portENTER_CRITICAL(&statusMux);
    st.running = true;
    portEXIT_CRITICAL(&statusMux);
    usbLog("USB host started (%s): waiting for a USB audio adapter (USB serial is off while the host runs)",
           dir == Direction::Playback ? "speaker" : "microphone");
    return true;
}

void stop() {
    if (!hostRunning.load()) return;
    stopClient = true;
    if (client) usb_host_client_unblock(client);
    if (xSemaphoreTake(clientDone, pdMS_TO_TICKS(3000)) != pdTRUE) usbLog("USB host: client did not stop");
    stopLib = true;
    usb_host_lib_unblock();
    if (xSemaphoreTake(libDone, pdMS_TO_TICKS(3000)) != pdTRUE) usbLog("USB host: library did not stop");
    hostRunning = false;
    isStreaming = false;
    devPresent = false;
    portENTER_CRITICAL(&statusMux);
    const uint8_t keep = st.volumeSetting;
    const uint32_t connects = st.connects, plugIns = st.plugIns;
    st = Status();
    st.volumeSetting = keep; st.connects = connects; st.plugIns = plugIns;
    portEXIT_CRITICAL(&statusMux);
    if (stream) xStreamBufferReset(stream);
    usbLog("USB host stopped");
}

size_t read(int32_t* out, size_t n, uint32_t timeoutMs) {
    if (!stream || !n) return 0;
    size_t got = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    while (got < n) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        const size_t bytes = xStreamBufferReceive(stream, out + got, (n - got) * sizeof(int32_t), deadline - now);
        got += bytes / sizeof(int32_t);
        if (!bytes) break;
    }
    return got;
}

void flush() { if (stream) xStreamBufferReset(stream); }

void setVolume(uint8_t percent) {
    volumeSetting = percent;
    portENTER_CRITICAL(&statusMux);
    st.volumeSetting = percent;
    portEXIT_CRITICAL(&statusMux);
    volumePending = true;
}

Status status() {
    portENTER_CRITICAL(&statusMux);
    Status s = st;
    portEXIT_CRITICAL(&statusMux);
    s.supported = true;
    s.running = hostRunning.load();
    return s;
}

bool descriptors(Descriptors& out) {
    portENTER_CRITICAL(&statusMux);
    const bool any = desc.seq != 0;
    if (any) out = desc;
    portEXIT_CRITICAL(&statusMux);
    return any;
}

void pollLog(void (*log)(const String&)) {
    if (!logQueue) return;
    LogLine line;
    while (xQueueReceive(logQueue, &line, 0) == pdTRUE) log(String(line.text));
}

} // namespace usbmic

#else // !USB_MIC_SUPPORTED

namespace usbmic {
bool supported() { return false; }
bool start(uint8_t, Direction) { return false; }
void setDirection(Direction) {}
Direction direction() { return Direction::Capture; }
void setPlaybackSource(PlaybackSource) {}
void stop() {}
int devicesOnBus() { return -1; }
bool running() { return false; }
bool streaming() { return false; }
size_t read(int32_t*, size_t, uint32_t) { return 0; }
void flush() {}
void setVolume(uint8_t) {}
Status status() { return Status(); }
bool descriptors(Descriptors&) { return false; }
void pollLog(void (*)(const String&)) {}
} // namespace usbmic

#endif

namespace usbmic {
static String jsonText(const char* s) {
    String o;
    for (; *s; ++s) { if (*s == '"' || *s == '\\') o += '\\'; if ((uint8_t)*s >= 32) o += *s; }
    return o;
}

String statusJson() {
    const Status s = status();
    String j = "{\"supported\":" + String(s.supported ? "true" : "false");
    j += ",\"running\":" + String(s.running ? "true" : "false");
    j += ",\"connected\":" + String(s.connected ? "true" : "false");
    j += ",\"streaming\":" + String(s.streaming ? "true" : "false");
    char id[12];
    snprintf(id, sizeof(id), "%04x:%04x", s.vid, s.pid);
    j += ",\"id\":\"" + String(s.connected ? id : "") + "\"";
    j += ",\"name\":\"" + jsonText(s.name) + "\"";
    j += ",\"problem\":\"" + jsonText(s.problem) + "\"";
    j += ",\"rate\":" + String(s.rate) + ",\"channels\":" + String(s.channels) + ",\"bits\":" + String(s.bits);
    j += ",\"level_control\":" + String(s.hasVolume ? "true" : "false");
    j += ",\"level_db\":" + (s.hasVolume ? String(s.volCur / 256.0f, 1) : String("null"));
    j += ",\"level_min_db\":" + (s.hasVolume ? String(s.volMin / 256.0f, 1) : String("null"));
    j += ",\"level_max_db\":" + (s.hasVolume ? String(s.volMax / 256.0f, 1) : String("null"));
    j += ",\"level_pct\":" + (s.hasVolume ? String(uac::percentForVolume(s.volMin, s.volMax, s.volCur)) : String("null"));
    j += ",\"level_setting\":" + (s.volumeSetting == kVolumeDeviceDefault ? String("null") : String(s.volumeSetting));
    j += ",\"packets\":" + String(s.packets) + ",\"bad_packets\":" + String(s.badPackets);
    j += ",\"overflows\":" + String(s.overflows) + ",\"connects\":" + String(s.connects) + "}";
    return j;
}
} // namespace usbmic
