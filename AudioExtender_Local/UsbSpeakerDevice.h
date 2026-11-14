#pragma once
// USB speaker presented to the PC by the local board in speaker mode (config.txt mode):
// USB Audio Class 1, stereo (1.4.2), 16-bit, 48 kHz, adaptive isochronous OUT endpoint.
// The PC's samples land in a ring (USB ISR); the sketch sends them to the remote in
// 12 ms radio packets (IMA ADPCM), and the remote plays them through its adapter.
// Counts below are frames (one left + one right sample); read() gives them interleaved.
#include <Arduino.h>

namespace usbspkdev {

// Adds the audio interface to the USB configuration (instead of the microphone). Must
// be called before USB.begin(). The name appears as the device name on the PC.
bool registerInterface(const char* name);
bool active();               // registered: this start is a speaker

bool pcPlaying();            // PC has the stream open (alternate setting 1)
uint32_t packetsReceived();  // USB packets received from the PC since boot
uint32_t available();        // frames waiting to be sent
uint32_t read(int16_t* lr, uint32_t frames);
void clear();                // drop what is waiting (link down, stream reopened)
uint32_t dropped();          // frames lost because the sender fell behind
uint32_t takeStreamStart();  // >0 once after the PC (re)opened the stream

// Called by the shared TinyUSB audio callbacks (UsbMicDevice.cpp).
bool onSetInterface(uint8_t itf, uint8_t alt);
void onClose(uint8_t itf);

}  // namespace usbspkdev
