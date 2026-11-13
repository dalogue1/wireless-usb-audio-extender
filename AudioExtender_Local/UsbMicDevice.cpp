#include "UsbMicDevice.h"
#include "JitterBuffer.h"
#include "LinkProtocol.h"
#include "VoiceMixer.h"
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "UsbMicDescriptor.h"
#include "UsbSpeakerDevice.h"

static_assert(kMicEpSize == JitterBuffer::kMaxFrame * 2, "endpoint must fit the largest frame");
#include <atomic>

#if !CONFIG_TINYUSB_AUDIO_ENABLED
#error "This Arduino-ESP32 core was built without the TinyUSB audio class (need core 3.3 or newer)"
#endif

namespace {

constexpr uint8_t kBytesPerSample = 2;
char s_name[33] = "Extender Mic";
uint8_t s_streamItf = 0xFF;
bool s_registered = false;
JitterBuffer* s_jb = nullptr;
VoiceMixer* s_mixer = nullptr;
std::atomic<bool> s_streaming{false};
std::atomic<uint32_t> s_starts{0};
std::atomic<uint32_t> s_frames{0};

}  // namespace

extern "C" uint16_t wum_audio_load_descriptor(uint8_t* dst, uint8_t* itf) {
    const uint8_t str = tinyusb_add_string_descriptor(s_name);
    const uint8_t ep = tinyusb_get_free_in_endpoint();
    TU_VERIFY(ep != 0);
    const uint8_t epIn = 0x80 | ep;
    const uint8_t ac = *itf;
    s_streamItf = ac + 1;
    buildMicDescriptor(dst, ac, str, epIn);
    *itf += 2;
    return kMicDescLen;
}

// ---------------------------------------------------------------- TinyUSB callbacks

// PC opens (alt 1) or closes (alt 0) the stream. (Shared by both functions: in speaker
// mode the speaker's own handler, UsbSpeakerDevice.cpp, takes it.)
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* req) {
    (void)rhport;
    const uint8_t itf = TU_U16_LOW(req->wIndex);
    const uint8_t alt = TU_U16_LOW(req->wValue);
    if (usbspkdev::active()) return usbspkdev::onSetInterface(itf, alt);
    if (itf != s_streamItf) return true;
    if (alt == 0) {
        s_streaming = false;
        return true;
    }
    if (s_jb) s_jb->requestFlush();  // start from fresh audio, 68 ms prefill
    // TinyUSB's own flow control keeps its FIFO at a threshold (half full) by
    // sending 47/48/49-sample packets. Start it exactly there with silence, so
    // it simply passes on the packet sizes JitterBuffer::frame() chooses.
    tud_audio_clear_ep_in_ff();
    static const int16_t zeros[64] = {0};
    uint16_t want = tud_audio_n_get_ep_in_fifo_threshold(0) & ~1u;
    while (want) {
        const uint16_t chunk = want > sizeof(zeros) ? sizeof(zeros) : want;
        const uint16_t put = tud_audio_write(zeros, chunk);
        if (!put) break;
        want -= put;
    }
    s_starts++;
    s_streaming = true;
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* req) {
    (void)rhport;
    if (usbspkdev::active()) usbspkdev::onClose(TU_U16_LOW(req->wIndex));
    else if (TU_U16_LOW(req->wIndex) == s_streamItf) s_streaming = false;
    return true;
}

// Called in the USB interrupt once per frame after a packet was handed to the
// hardware: queue the next frame's samples.
bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in,
                           uint8_t cur_alt_setting) {
    (void)rhport; (void)n_bytes_sent; (void)func_id; (void)ep_in; (void)cur_alt_setting;
    if (!s_streaming.load(std::memory_order_acquire)) return true;
    int16_t pcm[JitterBuffer::kMaxFrame];
    uint16_t n;
    if (s_jb) {
        n = s_jb->frame(pcm);
    } else {
        n = JitterBuffer::kNominal;
        memset(pcm, 0, sizeof(pcm));
    }
    if (s_mixer) s_mixer->mix(pcm, n);
    tud_audio_write(pcm, n * kBytesPerSample);
    s_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------- API

namespace usbmicdev {

bool registerInterface(const char* name) {
    if (s_registered) return true;
    if (name && name[0]) strlcpy(s_name, name, sizeof(s_name));
    s_registered = tinyusb_enable_interface(USB_INTERFACE_AUDIO, kMicDescLen, wum_audio_load_descriptor) == ESP_OK;
    return s_registered;
}

void attach(JitterBuffer* jb) { s_jb = jb; }
void attachMixer(VoiceMixer* mixer) { s_mixer = mixer; }
bool pcRecording() { return s_streaming.load(); }
void streamReset() { s_streaming = false; }
bool pcConfigured() { return tud_mounted(); }
bool pcSuspended() { return tud_suspended(); }
void reattach(uint32_t ms) {
    s_streaming = false;
    tud_disconnect();
    delay(ms);
    tud_connect();
}
uint32_t recordingStarts() { return s_starts.load(); }
uint32_t framesServed() { return s_frames.load(); }

}  // namespace usbmicdev
