#pragma once
// USB descriptor of the local board's microphone function (UAC1, 48 kHz mono 16-bit,
// asynchronous isochronous IN). Kept apart so tests/usb_device_descriptor_test
// can check it with the remote's own UAC1 parser.
#include <stdint.h>
#include <string.h>
#include "tusb.h"

constexpr uint32_t kMicRate = 48000;
constexpr uint16_t kMicEpSize = 49 * 2;  // room for a 49-sample frame (drift correction)
constexpr uint8_t kMicTermInput = 0x01;   // microphone input terminal
constexpr uint8_t kMicTermOutput = 0x02;  // USB streaming output terminal
constexpr uint16_t kMicIadLen = 8;
constexpr uint16_t kMicDescLen = kMicIadLen + TUD_AUDIO10_DESC_STD_AC_LEN + TUD_AUDIO10_DESC_CS_AC_LEN(1) +
                                 TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN +
                                 TUD_AUDIO10_DESC_STD_AS_LEN * 2 + TUD_AUDIO10_DESC_CS_AS_INT_LEN +
                                 TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN +
                                 TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN;

// ac: audio control interface number (streaming is ac + 1); str: string index
// for the function name; epIn: endpoint address with 0x80 set.
inline uint16_t buildMicDescriptor(uint8_t* dst, uint8_t ac, uint8_t str, uint8_t epIn) {
    const uint8_t d[kMicDescLen] = {
        // Interface Association: one audio function = control + streaming interface
        kMicIadLen, TUSB_DESC_INTERFACE_ASSOCIATION, ac, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0x00, str,
        // Audio control: microphone -> USB streaming, no feature unit (no volume/mute requests)
        TUD_AUDIO10_DESC_STD_AC(ac, 0x00, str),
        TUD_AUDIO10_DESC_CS_AC(0x0100, TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN,
                               (uint8_t)(ac + 1)),
        TUD_AUDIO10_DESC_INPUT_TERM(kMicTermInput, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00, 0x01, 0x0000, 0x00, 0x00),
        TUD_AUDIO10_DESC_OUTPUT_TERM(kMicTermOutput, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, kMicTermInput, 0x00),
        // Audio streaming: alt 0 = idle, alt 1 = 48 kHz mono 16-bit
        TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)(ac + 1), 0x00, 0x00, 0x00),
        TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)(ac + 1), 0x01, 0x01, 0x00),
        TUD_AUDIO10_DESC_CS_AS_INT(kMicTermOutput, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
        TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x01, 2, 16, kMicRate),
        // Asynchronous: the device's clock (here: the remote adapter's) sets the rate
        TUD_AUDIO10_DESC_STD_AS_ISO_EP(epIn, (uint8_t)((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS),
                                       kMicEpSize, 0x01, 0x00),
        TUD_AUDIO10_DESC_CS_AS_ISO_EP(0x00, 0x00, 0x0000),
    };
    memcpy(dst, d, kMicDescLen);
    return kMicDescLen;
}
