#pragma once
// Wireless USB Audio Extender - REMOTE (XIAO ESP32-S3): which microphone to use (1.4.9).
//
// Decided once, at start-up, before the USB host starts; to change microphones, restart the
// remote (unplug its power) with the new one in place. In this order:
//   1. A USB device in the OTG adapter (an audio adapter): the USB data lines get weak
//      pull-downs and are read. A full-speed device pulls D+ high, a low-speed one D-.
//      Either high = a USB device: the USB host starts, as before 1.4.9.
//   2. The External PDM Microphone board in the OTG adapter's USB-A socket. It is not a USB
//      device: its D- pin is the microphone's PDM clock and D+ its data (GPIO19 / GPIO20).
//      The USB pads are taken from the USB-Serial-JTAG controller (and its D+ pull-up,
//      which would sit on the DATA line, is turned off), the PDM clock is started on GPIO19
//      and GPIO20 is watched: a microphone answers with a bit stream (roughly half ones,
//      changing all the time); an empty socket stays low.
//   3. A PDM microphone wired to D3 = clock / D2 = data (the esp32-birdnet-mic wiring), then
//      the XIAO ESP32-S3 Sense's built-in microphone (clock GPIO42, data GPIO41). Checked the
//      same way. D2/D3 first: a mic wired to a Sense board is there on purpose, and wins
//      over the Sense's own.
//   4. None of these: the USB host starts and waits for an audio adapter (as before 1.4.9),
//      so an adapter plugged in later still works.
// While a PDM microphone is used the USB host never starts: the remote's USB-C port is then
// not USB at all (to flash it, unplug the microphone board and hold BOOT while plugging the
// XIAO into the computer, as always).
#include <Arduino.h>
#include <driver/gpio.h>
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#include "LinkProtocol.h"
#include "PdmMic.h"

namespace micdetect {

constexpr int kUsbDm = 19, kUsbDp = 20;          // USB D- / D+ = External PDM Microphone clock / data
constexpr int kSenseClk = 42, kSenseData = 41;   // XIAO ESP32-S3 Sense built-in microphone
constexpr int kPinClk = 4, kPinData = 3;         // D3 = GPIO4 (clock), D2 = GPIO3 (data)

inline pdmmic::Pins pins(uint8_t source) {
    pdmmic::Pins p;
    switch (source) {
        case wlink::kSrcAtomMic: p.clk = board::kAtomMicClk; p.data = board::kAtomMicData; break;
        case wlink::kSrcExternalPdm: p.clk = kUsbDm; p.data = kUsbDp; p.weakClock = true; break;
        case wlink::kSrcSensePdm: p.clk = kSenseClk; p.data = kSenseData; break;
        case wlink::kSrcPinPdm: p.clk = kPinClk; p.data = kPinData; break;
        default: break;
    }
    return p;
}

// What the data line did while the clock ran (sampled by the CPU, not in step with the clock).
struct Activity {
    uint32_t samples = 0, ones = 0, changes = 0;
    // A microphone: both levels seen in fair measure (2-98 % ones) and the line keeps changing.
    // An empty socket or pad: held low by the pull-down (0 % ones, no changes).
    bool present() const {
        return samples && ones * 50 >= samples && ones * 50 <= samples * 49 && changes * 100 >= samples;
    }
    unsigned onesPct() const { return samples ? unsigned((uint64_t(ones) * 100 + samples / 2) / samples) : 0; }
};

inline Activity listen(int dataPin, uint32_t n) {
    Activity a;
    int last = gpio_get_level(gpio_num_t(dataPin));
    for (uint32_t i = 0; i < n; ++i) {
        const int v = gpio_get_level(gpio_num_t(dataPin));
        a.ones += v;
        a.changes += v != last;
        last = v;
    }
    a.samples = n;
    return a;
}

// Runs the PDM clock on p.clk and watches p.data. The microphones wake within a few ms of
// the clock starting (IM73D122, SPM1423, the Sense's MSM261D3526H1CPM); one second look
// a little later covers a slow one. The pins are released afterwards.
inline bool probePdm(const pdmmic::Pins& p, Activity& a) {
    a = Activity{};
    if (!pdmmic::openChannel(p)) return false;
    delay(30);
    a = listen(p.data, 20000);
    if (!a.present()) {
        delay(70);
        a = listen(p.data, 20000);
    }
    pdmmic::closeChannel(p);
    return a.present();
}

// ---- the USB pads (GPIO19 / GPIO20)
inline uint32_t& savedConf0() { static uint32_t v = 0; return v; }

// Takes the pads from the USB-Serial-JTAG controller: pad off, and its own pull-ups and
// pull-downs held off (the D+ pull-up would otherwise sit on the PDM DATA line).
inline void releaseUsbPads() {
    savedConf0() = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
    uint32_t v = savedConf0();
    v |= USB_SERIAL_JTAG_PAD_PULL_OVERRIDE;
    v &= ~(USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_DM_PULLUP | USB_SERIAL_JTAG_DP_PULLDOWN |
           USB_SERIAL_JTAG_DM_PULLDOWN | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, v);
}

// Gives the pads back exactly as they were (before the USB host starts): no GPIO pulls left.
inline void restoreUsbPads() {
    for (int pin : {kUsbDm, kUsbDp}) {
        gpio_set_pull_mode(gpio_num_t(pin), GPIO_FLOATING);
        gpio_set_direction(gpio_num_t(pin), GPIO_MODE_DISABLE);
    }
    REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, savedConf0());
}

struct UsbLines {
    bool dp = false, dm = false;
};
// Both lines as inputs with weak pull-downs: a USB device's 1.5 kOhm pull-up wins.
inline UsbLines readUsbLines() {
    gpio_config_t c = {};
    c.pin_bit_mask = (1ULL << kUsbDm) | (1ULL << kUsbDp);
    c.mode = GPIO_MODE_INPUT;
    c.pull_down_en = GPIO_PULLDOWN_ENABLE;
    c.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&c);
    delay(5);
    UsbLines u;
    // Read a few times: a device that is just attaching settles within a millisecond.
    for (int i = 0; i < 5; ++i) {
        u.dp |= gpio_get_level(gpio_num_t(kUsbDp)) != 0;
        u.dm |= gpio_get_level(gpio_num_t(kUsbDm)) != 0;
        delay(1);
    }
    return u;
}

struct Result {
    uint8_t source = wlink::kSrcUsbAdapter;
    UsbLines usb;
    Activity activity;  // of the microphone found (or of the last place checked)
    Activity checked[3];  // USB-A socket, Sense, D3/D2 (what each place's data line did; for the log)
    char text[200] = "";
};

// The XIAO remote's microphone (not for the AtomS3U: always its built-in microphone).
inline Result detect() {
    Result r;
    releaseUsbPads();
    r.usb = readUsbLines();
    if (r.usb.dp || r.usb.dm) {
        restoreUsbPads();
        snprintf(r.text, sizeof(r.text), "a USB device on the USB-C port (%s high): USB audio adapter",
                 r.usb.dp ? "D+" : "D-");
        return r;
    }
    const bool ext = probePdm(pins(wlink::kSrcExternalPdm), r.activity);
    r.checked[0] = r.activity;
    if (ext) {  // the pads stay with the microphone
        r.source = wlink::kSrcExternalPdm;
        snprintf(r.text, sizeof(r.text), "External PDM Microphone in the USB-A socket (clock GPIO19, data GPIO20; %u %% ones)",
                 r.activity.onesPct());
        return r;
    }
    restoreUsbPads();
    const bool wired = probePdm(pins(wlink::kSrcPinPdm), r.activity);
    r.checked[2] = r.activity;
    if (wired) {
        r.source = wlink::kSrcPinPdm;
        snprintf(r.text, sizeof(r.text), "PDM microphone on D3/D2 (clock D3 = GPIO4, data D2 = GPIO3; %u %% ones)",
                 r.activity.onesPct());
        return r;
    }
    const bool sense = probePdm(pins(wlink::kSrcSensePdm), r.activity);
    r.checked[1] = r.activity;
    if (sense) {
        r.source = wlink::kSrcSensePdm;
        snprintf(r.text, sizeof(r.text), "XIAO ESP32-S3 Sense built-in microphone (clock GPIO42, data GPIO41; %u %% ones)",
                 r.activity.onesPct());
        return r;
    }
    snprintf(r.text, sizeof(r.text),
             "no USB device and no PDM microphone (data lines: USB-A %u %% ones / %lu changes, Sense %u %% / %lu, "
             "D2 %u %% / %lu): USB host started, waiting for an audio adapter",
             r.checked[0].onesPct(), (unsigned long)r.checked[0].changes, r.checked[1].onesPct(),
             (unsigned long)r.checked[1].changes, r.checked[2].onesPct(), (unsigned long)r.checked[2].changes);
    return r;
}

}  // namespace micdetect
