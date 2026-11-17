# Building from source, and how it works

[← Back to the main page](../README.md)

Most people only need the [web flasher](https://dalogue1.github.io/wireless-usb-audio-extender/flash/). This page is for changing the firmware.

## Firmware images

`firmware/` holds the prebuilt images ([checksums](../firmware/SHA256SUMS)):

| File | Flash at | Use |
|---|---|---|
| `…-full.bin` | `0x0` | first install: the whole flash; **resets** settings, `config.txt`, voice files and the event log |
| `…-update.bin` | `0x10000` | updates: the program only; keeps all of those |
| `…-boot.bin` | `0x0` | bootloader and partition table (stops before the settings at `0x9000`); the web flasher's Update writes it with the update image, so a board still starts if the flash was erased |

With esptool: `esptool --chip esp32s3 write-flash 0x0 wireless-usb-audio-extender-local-1.4.7-dev-full.bin`. **Both boards must run the same version**: the radio protocol changes between versions.

The same two images run on the Seeed XIAO ESP32-S3 and the M5Stack [AtomS3U](atoms3u.md): at start-up each reads the chip's factory eFuses (the XIAO's ESP32-S3R8 has PSRAM in the package, the AtomS3U's ESP32-S3FN8 has flash there instead) and picks that board's LED, button and, on the remote, its microphone: a USB audio adapter on the XIAO, the built-in PDM microphone (read through the S3's hardware PDM-to-PCM filter) on the AtomS3U.

## Arduino IDE

Install **esp32 by Espressif, version 3.3.x** in Boards Manager, and select the board **XIAO_ESP32S3**, for an AtomS3U too (the firmware adapts at start-up).

| Tools menu | Remote (`AudioExtender_Remote`) | Local (`AudioExtender_Local`) |
|---|---|---|
| USB Mode | **Hardware CDC and JTAG** | **USB-OTG (TinyUSB)** |
| USB CDC On Boot | Disabled | **Disabled** (required) |
| Everything else | default | default (the sketch's `partitions.csv` sets its flash layout) |

If a setting is wrong, the sketch stops at compile time with a message naming the setting to change.

* **Uploading to the remote:** once its firmware runs, its USB-C port is a USB *host* for the audio adapter, so **hold BOOT while plugging it into the computer**, then upload.
* **Uploading to an AtomS3U:** plug it in and hold its side button for about 2 s until the green LED inside lights up, then upload.
* **Uploading to the local board:** upload as usual; the IDE resets it through its USB serial port. An IDE upload keeps settings and voice files, like an update image.

## Command line

* `tools/build.sh` builds both images into `firmware/` with arduino-cli (esp32 core 3.3.12).
* `tests/run_tests.sh` runs the host tests with g++: clock drift and jitter buffer, radio protocol, USB descriptors (with a real adapter's), settings file, voice mixer and WAV checks, event log and reports, noise graph, mode decision, noise filter.
* After editing anything in `shared/` or `usbmic/`, run `tools/sync_shared.sh`: Arduino can only include files from inside a sketch folder, so each sketch carries its own copy (the tests check that the copies match).

## Files

```
AudioExtender_Remote/   remote: USB host -> high-pass + noise filter -> radio (microphone); radio -> USB host (speaker)
AudioExtender_Local/    local: radio -> jitter buffer -> USB microphone + announcements; USB speaker -> radio;
                        EXTENDER drive, config.txt, event log, reports (status, noise graph, report.html)
shared/                 radio protocol, board helpers, high-pass filter, jitter buffer, adapter details
usbmic/                 the USB audio host driver (also usable in other firmware: tools/export_usbmic.sh)
flash/                  the web flasher page (GitHub Pages)
docs/                   these pages; the noise filter's technical description and basis
voice/                  the built-in announcements as WAV files; voice-packs/: other sets
tests/, tools/          host tests; build, voice and noise tools; Linux udev rule
firmware/               prebuilt images
```

## How it works

**Radio.** The two boards talk directly with ESP-NOW, Espressif's connectionless Wi-Fi protocol: no router, access point or IP addresses. The remote announces itself (5 times a second) on its channel; a local board with the same mic number answers, and from then on they use acknowledged, retried unicast. Each side pings and reports status; after 2 s of silence both drop the link and look again. The paired link runs at the [`distance`](distance.md) rate. Audio goes in 12 ms packets (576 samples, 83 a second) with a sequence number; a lost packet becomes 12 ms of silence, so timing never slips.

**Microphone path.** The remote reads the adapter through its own USB host driver (`usbmic/`), applies the [filters](noise-filter.md) and sends 48 kHz mono audio in 12 ms packets. The [`distance`](distance.md) setting picks the format: uncompressed 16-bit at `near`, 8-bit µ-law at `normal`, IMA ADPCM at `far` (1.4.5 and later); [`audio_compression`](distance.md#audio-compression) can override it (1.4.7). Each packet decodes on its own, so a lost packet costs 12 ms and nothing more. The local board presents a standard USB Audio Class 1 microphone, so no drivers are needed on any computer.

**Clock drift ("47/49").** The adapter's crystal and the computer's USB clock never match exactly. The local board keeps about 150 ms of audio in a buffer (68 ms before 1.4.5), enough to ride through short radio hold-ups; each 1 ms USB frame it gives the computer 48 samples, or 49 when the buffer is running full and 47 when it is running empty, the way real asynchronous USB microphones do it. No sample is changed, no resampling. Announcements are mixed in last.

**Speaker path.** The same, reversed: the local board presents a stereo USB speaker and sends the computer's samples in 12 ms packets, each channel as IMA ADPCM (4 bits per sample, 596-byte packets: half of a mono 16-bit packet). Every packet carries the encoder's state at its start, so a lost packet costs 12 ms of silence and nothing more. The remote decodes, keeps the buffer and gives the adapter 47, 48 or 49 stereo frames per millisecond, following the computer's clock.

**Settings.** The local board reads `config.txt` at start and sends the remote its share until the remote confirms. A new mic number or channel is saved by the remote, confirmed, and then both boards restart on it.

**Adapter details.** The remote's status carries an id for its latest adapter; when the local board sees one it doesn't have, it asks, and the remote sends the descriptions in pieces. The local board keeps them in flash for `adapter.txt`.

**USB host note.** The Arduino-ESP32 core's ESP-IDF is built with the USB host's enumeration filter enabled, and a host installed without a filter function silently refuses every device. `usbmic/UsbMic.cpp` installs one that accepts all devices.
