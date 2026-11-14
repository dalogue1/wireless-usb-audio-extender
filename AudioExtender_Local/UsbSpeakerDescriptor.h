#pragma once
// USB descriptor of the local board's speaker function (UAC1, 48 kHz stereo 16-bit since 1.4.2,
// adaptive isochronous OUT: the board takes the PC's rate, the remote follows it).
// Kept apart so tests/usb_device_descriptor_test can check it with the remote's own
// UAC1 parser (playback direction).
#include <stdint.h>
#include <string.h>
#include "tusb.h"

constexpr uint32_t kSpkRate = 48000;
constexpr uint8_t kSpkChannels = 2;       // stereo (left, right)
constexpr uint16_t kSpkEpSize = 49 * 2 * kSpkChannels;  // room for 49 frames in 1 ms (the PC's own drift)
constexpr uint8_t kSpkTermInput = 0x01;   // USB streaming input terminal
constexpr uint8_t kSpkTermOutput = 0x02;  // speaker output terminal
constexpr uint16_t kSpkIadLen = 8;
constexpr uint16_t kSpkDescLen = kSpkIadLen + TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(1) +
                                 TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN +
                                 TUD_AUDIO10_DESC_STD_AS_LEN * 2 + TUD_AUDIO10_DESC_CS_AS_INT_LEN +
                                 TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN +
                                 TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN;

// ac: audio control interface number (streaming is ac + 1); str: string index
// for the function name; epOut: endpoint address (bit 7 clear).
inline uint16_t buildSpeakerDescriptor(uint8_t* dst, uint8_t ac, uint8_t str, uint8_t epOut) {
    const uint8_t d[kSpkDescLen] = {
        // Interface Association: one audio function = control + streaming interface
        kSpkIadLen, TUSB_DESC_INTERFACE_ASSOCIATION, ac, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0x00, str,
        // Audio control: USB streaming -> speaker, no feature unit (the PC's volume is software)
        TUD_AUDIO10_DESC_STD_AC(ac, 0x00, str),
        TUD_AUDIO10_DESC_CS_AC(0x0100, TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN,
                               (uint8_t)(ac + 1)),
        TUD_AUDIO10_DESC_INPUT_TERM(kSpkTermInput, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, kSpkChannels, 0x0003 /* left, right front */,
                                    0x00, 0x00),
        TUD_AUDIO10_DESC_OUTPUT_TERM(kSpkTermOutput, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00, kSpkTermInput, 0x00),
        // Audio streaming: alt 0 = idle, alt 1 = 48 kHz stereo 16-bit
        TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)(ac + 1), 0x00, 0x00, 0x00),
        TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)(ac + 1), 0x01, 0x01, 0x00),
        TUD_AUDIO10_DESC_CS_AS_INT(kSpkTermInput, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
        TUD_AUDIO10_DESC_TYPE_I_FORMAT(kSpkChannels, 2, 16, kSpkRate),
        // Adaptive: the board follows the PC's clock (the remote's playback buffer does the rest)
        TUD_AUDIO10_DESC_STD_AS_ISO_EP(epOut, (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE),
                                       kSpkEpSize, 0x01, 0x00),
        TUD_AUDIO10_DESC_CS_AS_ISO_EP(0x00, 0x00, 0x0000),
    };
    memcpy(dst, d, kSpkDescLen);
    return kSpkDescLen;
}
