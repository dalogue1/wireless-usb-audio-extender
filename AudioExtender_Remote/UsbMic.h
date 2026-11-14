#pragma once
// USB audio (UAC1) host driver, version 1.4.
#include <Arduino.h>
#include "sdkconfig.h"

// USB audio (UAC1) adapter through the ESP32-S3 USB OTG port in host mode:
// its microphone (capture) or, since 1.3, its speaker/headphone output (playback).
// Only the S3 has an OTG controller that can be a host; C3/C5/C6 compile a
// stub. The Arduino "USB Mode" must be "Hardware CDC and JTAG" (the default
// for the XIAO ESP32-S3) so TinyUSB does not own the OTG controller.
// Power: the XIAO does not drive VBUS; feed 5 V through an OTG splitter (or
// the 5V pin) so the adapter is powered.
#if defined(CONFIG_IDF_TARGET_ESP32S3) && (!defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE == 1)
#define USB_MIC_SUPPORTED 1
#else
#define USB_MIC_SUPPORTED 0
#endif

namespace usbmic {

static constexpr uint32_t kSampleRate = 48000; // fixed, like PDM
static constexpr uint8_t kVolumeDeviceDefault = 255; // leave the adapter's level as it is

// Which side of the adapter is used: one at a time.
enum class Direction : uint8_t { Capture = 0, Playback = 1 };

struct Status {
    bool supported = false;
    bool running = false;      // host stack installed
    bool connected = false;    // a USB device is attached
    bool streaming = false;    // a UAC1 stream is open (in the direction in use)
    bool canCapture = false;   // the latest device has a usable 48 kHz microphone interface
    bool canPlayback = false;  // ...and/or a usable 48 kHz speaker interface
    uint8_t direction = 0;     // Direction in use
    bool hasVolume = false;
    uint16_t vid = 0, pid = 0;
    uint8_t channels = 0, bits = 0;
    uint32_t rate = 0;
    int16_t volMin = 0, volMax = 0, volCur = 0, volRes = 0; // 1/256 dB
    uint8_t volumeSetting = kVolumeDeviceDefault;
    uint32_t packets = 0, badPackets = 0, overflows = 0, connects = 0;
    uint32_t plugIns = 0;      // devices plugged in (not counting re-opens, e.g. a direction change)
    char name[48] = {0};
    char problem[64] = {0};    // last reason a device was not used
};

bool supported();
// Devices the USB Host Library has enumerated (any class), or -1 if the host is not running.
int devicesOnBus();
// Install the host stack (if needed) and capture from (or play to) any UAC1 adapter
// that is (or later gets) plugged in. Safe to call again: only re-applies the level.
bool start(uint8_t volumePercent, Direction dir = Direction::Capture);
// Switch direction: an open device is closed and opened again the other way.
void setDirection(Direction dir);
Direction direction();
// Playback: called in the USB task once per 1 ms packet. Fill up to maxFrames stereo
// frames (left, right interleaved; normally 47-49 frames) and return how many; 0 sends
// 1 ms of silence. A mono device gets (left + right) / 2 (1.4; was mono in 1.3).
using PlaybackSource = uint16_t (*)(int16_t* lr, uint16_t maxFrames);
void setPlaybackSource(PlaybackSource src);
// Close the device and uninstall the host stack (frees the OTG port).
void stop();
bool running();
bool streaming();
// Capture: blocking read of up to n left-aligned int32 samples (first channel).
// Returns the count read before timeoutMs; 0 on timeout.
size_t read(int32_t* out, size_t n, uint32_t timeoutMs);
void flush();
// 0..100 % of the adapter's level range (its mic, or its speaker in playback), or kVolumeDeviceDefault.
void setVolume(uint8_t percent);
Status status();

// The latest device's descriptors, as read when it was opened (kept after it is
// unplugged). For diagnostics: the application can report them to the user.
static constexpr size_t kMaxConfigDesc = 2048;
struct Descriptors {
    uint32_t seq = 0;              // +1 for every device opened; 0 = none since start
    uint8_t speed = 0;             // 0 low, 1 full, 2 high
    uint8_t device[18] = {0};      // device descriptor
    uint16_t configLen = 0;        // bytes in config (wTotalLength, capped)
    uint16_t configTotal = 0;      // wTotalLength as reported
    uint8_t config[kMaxConfigDesc];
    char manufacturer[48] = {0}, product[48] = {0}, serial[48] = {0};
    // The driver's choice (0xFF: no usable microphone interface).
    uint8_t usedInterface = 0xFF, usedAlt = 0xFF, usedEndpoint = 0xFF;
};
// Copies the latest descriptors; false if no device was opened since start.
bool descriptors(Descriptors& out);
String statusJson();
// Drains messages queued by the USB task (call from loop()).
void pollLog(void (*log)(const String&));

} // namespace usbmic
