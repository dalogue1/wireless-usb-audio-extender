#pragma once
// USB microphone presented to the PC by the local board: USB Audio Class 1,
// mono, 16-bit, 48 kHz, asynchronous isochronous IN endpoint.
//
// Each 1 ms USB frame the TinyUSB audio ISR asks for the next packet; we take
// 47, 48 or 49 samples from the JitterBuffer (see JitterBuffer.h).
#include <Arduino.h>

template <uint8_t kCh> class JitterBufferT;
using JitterBuffer = JitterBufferT<1>;  // JitterBuffer.h
class VoiceMixer;

namespace usbmicdev {

// Adds the audio interface to the USB configuration. Must be called before
// USB.begin(). The name appears as the device name on the PC.
bool registerInterface(const char* name);

// Audio source for the USB frames. Without one, the PC records silence.
void attach(JitterBuffer* jb);

// Announcements mixed into every frame (optional).
void attachMixer(VoiceMixer* mixer);

bool pcRecording();         // PC has the stream open (alternate setting 1)
uint32_t recordingStarts(); // times the PC opened the stream
uint32_t framesServed();    // USB frames filled since boot

// USB connection state (USB robustness watch in the sketch).
bool pcConfigured();        // the PC has set the device up (enumerated and configured)
bool pcSuspended();         // the bus is suspended (PC asleep, or its port stopped)
void streamReset();         // forget an open stream (the PC set the device up again)
// Soft unplug: disconnects from the bus for ms, then connects again, so the PC sees
// a new device and sets it up from scratch. Blocks for ms.
void reattach(uint32_t ms);

}  // namespace usbmicdev
