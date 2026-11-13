#include "UsbSpeakerDevice.h"
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "UsbSpeakerDescriptor.h"
#include "SampleRing.h"
#include <atomic>

static_assert(kSpkEpSize <= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, "TinyUSB's OUT endpoint buffer must fit a frame");

namespace {

char s_name[33] = "Extender Speaker";
uint8_t s_streamItf = 0xFF;
bool s_registered = false;
std::atomic<bool> s_streaming{false};
std::atomic<uint32_t> s_packets{0}, s_starts{0}, s_startsTaken{0};
SampleRing<4096> s_ring;  // 85 ms (the sender takes 12 ms at a time)

}  // namespace

extern "C" uint16_t wum_speaker_load_descriptor(uint8_t* dst, uint8_t* itf) {
    const uint8_t str = tinyusb_add_string_descriptor(s_name);
    const uint8_t ep = tinyusb_get_free_out_endpoint();
    TU_VERIFY(ep != 0);
    const uint8_t ac = *itf;
    s_streamItf = ac + 1;
    buildSpeakerDescriptor(dst, ac, str, ep & 0x7F);
    *itf += 2;
    return kSpkDescLen;
}

// Called in the USB interrupt once per received packet (normally 48 samples per 1 ms).
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                           uint8_t cur_alt_setting) {
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    int16_t pcm[64];
    uint16_t left = n_bytes_received;
    while (left) {
        const uint16_t want = left > sizeof(pcm) ? sizeof(pcm) : left;
        const uint16_t got = tud_audio_read(pcm, want);
        if (!got) break;
        if (s_streaming.load(std::memory_order_relaxed)) s_ring.push(pcm, got / 2);
        left -= got;
    }
    s_packets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace usbspkdev {

bool registerInterface(const char* name) {
    if (s_registered) return true;
    if (name && name[0]) strlcpy(s_name, name, sizeof(s_name));
    s_registered = tinyusb_enable_interface(USB_INTERFACE_AUDIO, kSpkDescLen, wum_speaker_load_descriptor) == ESP_OK;
    return s_registered;
}
bool active() { return s_registered; }

bool onSetInterface(uint8_t itf, uint8_t alt) {
    if (itf != s_streamItf) return true;
    if (alt == 0) {
        s_streaming = false;
        return true;
    }
    s_starts++;
    s_streaming = true;
    return true;
}
void onClose(uint8_t itf) {
    if (itf == s_streamItf) s_streaming = false;
}

bool pcPlaying() { return s_streaming.load(); }
uint32_t packetsReceived() { return s_packets.load(); }
uint32_t available() { return s_ring.available(); }
uint32_t read(int16_t* out, uint32_t n) { return s_ring.pop(out, n); }
void clear() { s_ring.clear(); }
uint32_t dropped() { return s_ring.dropped(); }
uint32_t takeStreamStart() {
    const uint32_t n = s_starts.load();
    const uint32_t was = s_startsTaken.exchange(n);
    return n - was;
}

}  // namespace usbspkdev
