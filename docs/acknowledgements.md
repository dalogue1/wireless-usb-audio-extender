# Acknowledgements

[← Back to the main page](../README.md)

## esp32-birdnet-mic

This project owes its start to **[esp32-birdnet-mic](https://github.com/Sukecz/esp32-birdnet-mic)** by **Sukecz**. That project was the inspiration for this one, and it showed what the ESP32 platform can do as a bird-monitoring microphone feeding BirdNET.

The two projects take different approaches. The Wireless USB Audio Extender focuses on one thing: a reliable, simple USB audio extension, with a USB audio adapter at one end and an ordinary USB microphone at the computer. esp32-birdnet-mic is a networked microphone with a much broader feature set, including:

* support for many more ESP32 board types;
* digital MEMS microphones (I2S and PDM types);
* audio streaming over your Wi-Fi network (RTSP), so a BirdNET-Go or BirdNET-Pi server can listen from anywhere on the network;
* recording to an SD card;
* a web interface for settings and monitoring, and over-the-air updates.

The adaptive noise filter used here began as a contribution to esp32-birdnet-mic.

## Also

* **Kokoro TTS** ([hexgrad/kokoro](https://github.com/hexgrad/kokoro), Apache-2.0): the built-in voice and the voice packs.
* **ESP Web Tools** ([esphome/esp-web-tools](https://github.com/esphome/esp-web-tools)): the web flasher.
* **Robert ATS Duct**: the [Seeed XIAO ESP32-S3 case](https://www.printables.com/model/1275829-seeed-xiao-esp32-s3-case) on Printables.
* **Rainer Martin** and the other authors of the published methods behind the noise filter: see [Technical basis](noise-filter-basis.md#references).
* **Espressif** and the **Arduino-ESP32** project, for ESP-NOW, the USB host and the toolchain this is built on.
