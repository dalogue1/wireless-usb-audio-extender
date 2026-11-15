#pragma once
// Wireless USB Audio Extender - board detection, saved settings, status LED and ESP-NOW radio setup.
//
// SHARED FILE: an identical copy lives in AudioExtender_Remote/ and
// AudioExtender_Local/. Edit shared/BoardCommon.h and run tools/sync_shared.sh.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <driver/temperature_sensor.h>
#include <esp_heap_caps.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include "HoldButton.h"
#include "LinkProtocol.h"

namespace board {

// ---------------------------------------------------------------- settings
// Settings come from the local board's config.txt (on its EXTENDER drive); the
// remote receives its share over the radio and saves it.
// Internally (radio packets) the pair ID is 0..8; people only ever see the
// mic number 1..9 = pair ID + 1.
// ---------------------------------------------------------------- board
// One firmware per role runs on both boards; which one it is comes from the chip's
// factory eFuses, before any pin is touched:
//   Seeed XIAO ESP32-S3: ESP32-S3R8 (8 MB PSRAM in the package, flash outside)
//   M5Stack AtomS3U:     ESP32-S3FN8 (8 MB flash in the package, no PSRAM)
// Anything else is treated as a XIAO.
enum class Kind : uint8_t { Xiao = wlink::kBoardXiao, AtomS3U = wlink::kBoardAtomS3U };
inline Kind kind() {
    static int8_t cached = -1;
    if (cached < 0) {
        uint8_t flashCap = 0, psramCap = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_FLASH_CAP, &flashCap, esp_efuse_get_field_size(ESP_EFUSE_FLASH_CAP));
        esp_efuse_read_field_blob(ESP_EFUSE_PSRAM_CAP, &psramCap, esp_efuse_get_field_size(ESP_EFUSE_PSRAM_CAP));
        cached = (flashCap != 0 && psramCap == 0) ? int8_t(Kind::AtomS3U) : int8_t(Kind::Xiao);
    }
    return Kind(cached);
}
inline bool atomS3U() { return kind() == Kind::AtomS3U; }
inline const char* kindName() { return wlink::boardName(uint8_t(kind())); }

// The button: XIAO BOOT (GPIO0); AtomS3U the button on the back (G41). A normal input
// once the sketch runs.
constexpr int kXiaoButton = 0, kAtomButton = 41;
inline int buttonPin() { return atomS3U() ? kAtomButton : kXiaoButton; }
// LED: XIAO user LED (orange, GPIO21, active low, PWM); AtomS3U RGB LED (WS2812, G35).
constexpr int kXiaoLed = 21, kAtomLed = 35;
// AtomS3U built-in PDM microphone (SPM1423): clock G39, data G38.
constexpr int kAtomMicClk = 39, kAtomMicData = 38;

inline uint8_t micNumber(uint8_t pairId) { return pairId + 1; }

// Settings kept in NVS (namespace "extender") across restarts.
//   remote: pair, chan, opt (filter), adapt, dist, mode, lock (6 bytes: the local board it is
//           locked to)  - as last received from the local board
//   local: pair, chan (the link in use: changes only after a handover),
//          adapter (the remote's latest adapter details, see AdapterInfo.h),
//          amode (the USB device it last presented: microphone or speaker)
class SavedSettings {
public:
    void begin() { prefs_.begin("extender", false); }
    uint8_t get(const char* key, uint8_t def) { return prefs_.getUChar(key, def); }
    void put(const char* key, uint8_t v) {
        if (prefs_.getUChar(key, 0xFF) != v) prefs_.putUChar(key, v);  // no flash write if unchanged
    }
    uint16_t get16(const char* key, uint16_t def) { return prefs_.getUShort(key, def); }
    void put16(const char* key, uint16_t v) {
        if (!prefs_.isKey(key) || prefs_.getUShort(key, 0) != v) prefs_.putUShort(key, v);
    }
    // Small binary records (the remote's latest adapter details on the local board).
    size_t getBytes(const char* key, void* buf, size_t cap) {
        const size_t n = prefs_.isKey(key) ? prefs_.getBytesLength(key) : 0;
        return n && n <= cap ? prefs_.getBytes(key, buf, cap) : 0;
    }
    void putBytes(const char* key, const void* buf, size_t len) { prefs_.putBytes(key, buf, len); }
    void remove(const char* key) { if (prefs_.isKey(key)) prefs_.remove(key); }
    void factoryReset() { prefs_.clear(); }
    // One-shot flags (e.g. "boot as the EXTENDER drive next time").
    void setFlag(const char* key) { prefs_.putUChar(key, 1); }
    bool takeFlag(const char* key) {
        const bool set = prefs_.getUChar(key, 0) != 0;
        if (set) prefs_.remove(key);
        return set;
    }
private:
    Preferences prefs_;
};

// ---------------------------------------------------------------- LED
// One LED, dimmable so it can also show the sound level: the XIAO's orange user LED
// (GPIO21, lit when LOW, PWM) or the AtomS3U's RGB LED (WS2812), shown in orange too.
enum class LedMode : uint8_t {
    Off,
    Searching,   // slow blink: looking for the other board
    LinkedIdle,  // double blink: linked, no audio (no adapter / PC not recording)
    Streaming,   // solid: audio flowing
    Problem,     // fast blink: adapter plugged in but not usable
    Waiting,     // short blips, 3 a second: local, BOOT clicked, waiting for the remote before opening the drive
    // Level meter: brightness follows the sound (fed with feedPeak()).
    LevelGlow,   // ...with a faint glow in silence (remote: linked; local: PC recording)
    LevelFlash,  // ...with a quick full flash every 2 s (remote: not linked; local: PC not recording)
};

class StatusLed {
public:
    void begin() {
        rgb_ = atomS3U();
        if (!rgb_) ledcAttach(kXiaoLed, 5000, 8);
        set(false);
    }

    // Feed the loudest sample (0..32767) seen since the last call; call every
    // loop. The meter rises instantly and falls over ~0.3 s, like a VU meter.
    void feedPeak(uint16_t peak) {
        const uint32_t now = millis();
        const float elapsed = float(now - meterMs_);
        meterMs_ = now;
        const float target = levelToDuty(peak);
        meter_ = target > meter_ ? target : meter_ - elapsed * 0.85f;  // 255 -> 0 in ~300 ms
        if (meter_ < 0) meter_ = 0;
    }

    // Blinks the mic number at boot: 1 to 9 blinks.
    void blinkMicNumber(uint8_t number) {
        set(false);
        delay(400);
        for (uint8_t i = 0; i < number; ++i) {
            set(true);
            delay(250);
            set(false);
            delay(350);
        }
        delay(600);
    }

    void mode(LedMode m) {
        if (m != mode_) {
            mode_ = m;
            phaseStart_ = millis();
        }
    }

    // Call often from loop().
    void update() {
        const uint32_t t = millis() - phaseStart_;
        bool on = false;
        switch (mode_) {
            case LedMode::Off: on = false; break;
            case LedMode::Searching: on = (t % 1000) < 500; break;
            case LedMode::LinkedIdle: {
                const uint32_t p = t % 2000;
                on = p < 120 || (p >= 240 && p < 360);
                break;
            }
            case LedMode::Streaming: on = (millis() - flickerAt_) > 60; break;
            case LedMode::Problem: on = (t % 250) < 125; break;
            case LedMode::Waiting: on = (t % 333) < 50; break;
            case LedMode::LevelGlow:
            case LedMode::LevelFlash: {
                uint8_t d = uint8_t(meter_);
                if (mode_ == LedMode::LevelGlow && d < kGlow) d = kGlow;
                if (mode_ == LedMode::LevelFlash && (t % 2000) < 80) d = 255;
                if (millis() - flickerAt_ <= 60) d = 0;  // underrun flicker (local)
                duty(d);
                return;
            }
        }
        set(on);
    }

    // Peak sample (0..32767) -> LED duty: -60 dBFS = off, -6 dBFS and up = full,
    // with a gamma so quiet sounds are still visible as a dim glow.
    static uint8_t levelToDuty(uint16_t peak) {
        if (peak < 33) return 0;  // below -60 dBFS
        const float db = 20.0f * log10f(peak / 32768.0f);
        float x = (db + 60.0f) / 54.0f;
        if (x > 1) x = 1;
        return uint8_t(255.0f * powf(x, 2.2f) + 0.5f);
    }

    // Direct LED control (settings menu, drive mode).
    void raw(bool on) { set(on); }

    // Short off-flicker while Streaming or metering (used for buffer underruns).
    void flicker() { flickerAt_ = millis(); }

private:
    static constexpr uint8_t kGlow = 6;  // faint "linked" glow under the level meter
    void set(bool on) { duty(on ? 255 : 0); }
    void duty(uint8_t d) {
        if (!rgb_) { ledcWrite(kXiaoLed, 255 - d); return; }  // active low
        if (d == lastDuty_) return;  // the WS2812 keeps its colour: write changes only
        lastDuty_ = d;
        // Orange, at most ~1/3 brightness (a WS2812 at full power is glaring).
        const uint8_t r = uint8_t((d * 80u + 127) / 255), g = uint8_t((d * 28u + 127) / 255);
        rgbLedWrite(kAtomLed, r, g, 0);
    }
    bool rgb_ = false;
    int16_t lastDuty_ = -1;
    LedMode mode_ = LedMode::Off;
    float meter_ = 0;
    uint32_t meterMs_ = 0;
    uint32_t phaseStart_ = 0;
    uint32_t flickerAt_ = 0;
};

// ---------------------------------------------------------------- radio
// Radio rate of the paired link, from config.txt `distance` (wlink::Distance):
//   near   = 12 Mbit/s OFDM: ~7 % of the channel for the audio stream; most robust where Wi-Fi is busy
//   normal = 6 Mbit/s OFDM (default): ~14 % airtime, about 2-3 dB more reach than 12 Mbit/s
//   far    = 2 Mbit/s DSSS: ~45 % airtime, the longest reach, least tolerant of other Wi-Fi traffic
// Beacons (broadcast) always go at the ESP-NOW default 1 Mbit/s.
inline void linkRate(uint8_t distance, wifi_phy_mode_t& mode, wifi_phy_rate_t& rate) {
    if (distance == wlink::kDistanceNear) { mode = WIFI_PHY_MODE_11G; rate = WIFI_PHY_RATE_12M; }
    else if (distance == wlink::kDistanceFar) { mode = WIFI_PHY_MODE_11B; rate = WIFI_PHY_RATE_2M_L; }
    else { mode = WIFI_PHY_MODE_11G; rate = WIFI_PHY_RATE_6M; }
}

// Wi-Fi in station mode, never connected: only ESP-NOW is used. Power save
// off (it adds 100 ms latency spikes); fixed channel from the pair ID.
// Transmit power in dBm (2..20; 20 = the chip's maximum, the default). The
// radio still limits it per data rate (about 20 dBm at the 6-12 Mbit/s link rates).
// Why the chip last started, for the log; nullptr for an ordinary power-up, reset
// button or deliberate restart.
inline const char* unusualResetText() {
    switch (esp_reset_reason()) {
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT: return "restarted by the watchdog: the program had stopped running";
        case ESP_RST_PANIC: return "restarted after a crash";
        case ESP_RST_BROWNOUT: return "restarted after a power dip (brownout): check the 5 V supply and cable";
        default: return nullptr;
    }
}

// Why the chip last started, always (the log and status.txt).
inline const char* resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on (plugged in, or the USB port's power was cut)";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "restart by the firmware (drive, settings or recovery)";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog (the program had stopped running)";
        case ESP_RST_BROWNOUT: return "power dip (brownout)";
        case ESP_RST_USB: return "reset through USB by the PC";
        case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
        default: return "other";
    }
}

// Chip (die) temperature in 0.1 C, or wlink::kTempUnknown. 1.4.4: read only at start
// (before the radio starts) and when the local board's drive opens (the audio stops then
// anyway), never while audio streams: the Wi-Fi PHY uses the same sensor for its own
// calibration, and the radio had to be stopped for each read; restarting it sometimes
// hung the AtomS3U remote. The sensor is installed, read once and released again. The 20..100 C range
// (Arduino's temperatureRead() uses 10..50 C, inaccurate when it matters).
inline int16_t readChipTempOnce() {
    temperature_sensor_handle_t sensor = nullptr;
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&c, &sensor) != ESP_OK) return wlink::kTempUnknown;
    float t = 0;
    bool ok = false;
    if (temperature_sensor_enable(sensor) == ESP_OK) {
        ok = temperature_sensor_get_celsius(sensor, &t) == ESP_OK;
        temperature_sensor_disable(sensor);
    }
    temperature_sensor_uninstall(sensor);
    return ok ? int16_t(lroundf(t * 10.0f)) : wlink::kTempUnknown;
}


// "45.2 C" or "unknown".
inline String tempText(int16_t deciC) {
    if (deciC == wlink::kTempUnknown) return "unknown";
    char b[16];
    snprintf(b, sizeof(b), "%s%d.%d C", deciC < 0 ? "-" : "", abs(deciC) / 10, abs(deciC) % 10);
    return b;
}

// Processor speed, 240 or 160 MHz. Both run from the same PLL with an 80 MHz bus
// clock, so the radio, USB and serial port are unaffected and it can change live.
inline bool setCpuMhz(uint8_t mhz) {
    if (!wlink::cpuMhzValid(mhz)) mhz = wlink::kCpuMhzDefault;
    return getCpuFrequencyMhz() == mhz || setCpuFrequencyMhz(mhz);
}

inline void setTxPower(uint8_t dbm) {
    if (dbm < wlink::kMinTxPowerDbm) dbm = wlink::kMinTxPowerDbm;
    if (dbm > wlink::kMaxTxPowerDbm) dbm = wlink::kMaxTxPowerDbm;
    esp_wifi_set_max_tx_power(int8_t(dbm * 4));
}


inline bool startRadio(uint8_t channel, esp_now_recv_cb_t onRecv, esp_now_send_cb_t onSent) {
    WiFi.persistent(false);  // never touch Wi-Fi settings saved by other firmware
    WiFi.mode(WIFI_STA);
    esp_wifi_disconnect();   // and never join a saved network (it would move the channel)
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_max_tx_power(80);  // 20 dBm (units of 0.25 dBm); clamped to the legal max; see setTxPower()
    if (esp_now_init() != ESP_OK) return false;
    uint32_t version = 0;
    esp_now_get_version(&version);
    if (version < 2) return false;  // audio packets need ESP-NOW v2 (up to 1470 bytes)
    esp_now_register_recv_cb(onRecv);
    if (onSent) esp_now_register_send_cb(onSent);
    return true;
}

// The paired peer (fastRate: at the `distance` rate) or the broadcast peer (ESP-NOW default rate).
// The ESP-NOW default (1 Mbit/s) would need ~85 % of the channel for the audio stream
// (83 packets/s of ~1.2 KB); see linkRate() for the rates used.
inline bool setPeerRate(const uint8_t mac[6], uint8_t distance) {
    esp_now_rate_config_t rate = {};
    linkRate(distance, rate.phymode, rate.rate);
    rate.ersu = false;
    rate.dcm = false;
    return esp_now_set_peer_rate_config(mac, &rate) == ESP_OK;
}

inline bool addPeer(const uint8_t mac[6], bool fastRate, uint8_t distance = wlink::kDistanceNormal) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;  // current channel
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) return false;
    if (fastRate) setPeerRate(mac, distance);
    return true;
}

// Internal RAM (what the radio driver allocates its packet buffers from).
struct MemInfo { uint32_t free, min, largest; };
inline MemInfo memInfo() {
    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    return {uint32_t(heap_caps_get_free_size(caps)), uint32_t(heap_caps_get_minimum_free_size(caps)),
            uint32_t(heap_caps_get_largest_free_block(caps))};
}
inline String memText(const MemInfo& m) {
    char b[64];
    snprintf(b, sizeof(b), "%lu KB free (largest block %lu KB, lowest %lu KB)", (unsigned long)(m.free / 1024),
             (unsigned long)(m.largest / 1024), (unsigned long)(m.min / 1024));
    return b;
}

inline String macText(const uint8_t m[6]) {
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(b);
}

}  // namespace board
