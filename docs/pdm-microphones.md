# PDM microphones

[← Back to the main page](../README.md)

Since 1.4.9, a XIAO ESP32-S3 remote can take its audio from a **PDM microphone** instead of a USB audio adapter. A PDM (pulse-density modulation) microphone is a MEMS mic with a digital output: the remote sends it a clock and reads its bit stream directly. It needs no USB audio adapter and no 3.5 mm plug, which makes it simpler to build, but it is not quieter: the analog capsule in the main build (AOM5024, SNR 80 dB) has a lower noise floor than any of these PDM mics.

Everything after the microphone stays the same: the radio link, the local board, "Extender Mic N" on the PC, the filters, `config.txt` and the EXTENDER drive.

## The types

| Type | What it is | Soldering |
|---|---|---|
| **External PDM Microphone** | A small USB-A plug board with an IM73D122 mic board standing on its back edge. It plugs into the OTG adapter in place of the USB audio adapter. | Assemble the board once (or get one ready-made) |
| **XIAO Sense built-in microphone** | The mic on the XIAO ESP32-S3 Sense's camera board (MSM261D3526H1CPM, SNR 64 dB(A)) | None: use a XIAO ESP32-S3 **Sense** as the remote |
| **A PDM mic on D2/D3** | Any 3.3 V PDM mic module wired to the XIAO: clock to **D3**, data to **D2**, plus 3V3 and GND | Yes |
| AtomS3U built-in microphone | See [AtomS3U boards](atoms3u.md) | None |

### External PDM Microphone

This is the one to use for the best sound without opening a USB audio adapter or wiring a capsule. The Infineon IM73D122 has a 73 dB signal-to-noise ratio and a very low noise floor for a MEMS mic. The board plugs into the same OTG adapter and power splitter as the main build, so the rest of the setup is unchanged. A passive USB-A extension of up to about 1 m can sit between the adapter and the board.

It is **not a USB device**. The board connects the mic to the USB socket's two data pins, and the remote uses those pins as plain inputs and outputs: D− carries the clock (GPIO19), D+ the data (GPIO20). Only a plain wired OTG adapter works; one with a hub chip inside does not.

How to make the board, the parts and how to order it: **[The External PDM Microphone board](pdm-mic-board.md)**.

### XIAO Sense built-in microphone

The XIAO ESP32-S3 Sense is the same board as the XIAO ESP32-S3 with a camera and microphone board clipped on. Used as the remote with nothing in its OTG adapter, it uses that microphone. The camera is not used. It is the quickest PDM option, but the mic sits on the board itself, next to the radio and in whatever case you use, and it is a noisier mic: its signal-to-noise ratio is 64 dB(A), against 73 dB for the IM73D122 on the External PDM Microphone and 80 dB for the AOM5024 in the main build.

### A PDM mic on D2/D3

For builders who already have a PDM mic module (an IM73D122 breakout, the Adafruit PDM mic, an MP34DT01 board): wire CLK to D3, DATA to D2, power to 3V3 and GND. The mic's SEL pin can be at either level. This needs soldering, so it is not part of the main build.

## Automatic detection

There is no setting. When the remote starts, it checks in this order and uses the first one it finds:

1. **A USB audio adapter** in the OTG adapter (as before 1.4.9).
2. **The External PDM Microphone** in the OTG adapter.
3. **A PDM mic on D2/D3.** This comes before the Sense's own mic, so a mic wired to a Sense board is the one used.
4. **The XIAO Sense's built-in microphone.**
5. **None of these:** the remote waits for a USB audio adapter to be plugged in, as before.

The remote chooses only when it starts. To change from one kind of mic to another, plug in the new one and restart the remote (unplug its power). A USB audio adapter always wins, so a Sense board or a wired mic doesn't get in the way of one.

The remote finds a PDM mic by starting its clock and watching the data line: a mic answers with a stream of changing bits, and an empty socket or pin stays low. A mic can answer on the left or the right channel (its SEL pin); the remote listens to both for the first half second and keeps the one carrying a signal.

## What you'll see

* **`adapter.txt`** on the EXTENDER drive starts with a `PDM microphone type:` line naming which kind was found, for example `External PDM Microphone (board in the OTG adapter's USB-A socket)`. It also gives the mic part, 48 kHz 16-bit mono, the channel (left or right), and the gain.
* **`status.txt`** names the microphone in its remote section.
* **The remote's log** has a line starting `microphone:` saying what it found at start-up.

## Settings that change

* **`mic_level`** is the PDM mic's gain, applied in the remote: 0–100 = 0 to +30 dB, default +30 dB. If loud birds close to the mic clip, lower it.
* **`mode`** has no effect: a PDM mic is always a microphone (there's no speaker output).
* Everything else (`distance`, the filters, `audio_compression`, channels, pairing) works the same as with a USB adapter.

## Good to know

* **Flashing:** with the External PDM Microphone plugged in, the remote's USB port isn't USB. To flash, unplug the mic board, then hold BOOT while plugging the XIAO into the computer, as always.
* **Unplugging while running:** if the mic board comes out while the remote runs, `status.txt` shows "NO MICROPHONE SIGNAL" after 20 s. Plug it back in and restart the remote.
* **Both boards must run 1.4.9-dev or later.**
