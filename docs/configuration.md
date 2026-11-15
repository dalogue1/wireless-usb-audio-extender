# Settings (config.txt)

[← Back to the main page](../README.md)

A bare pair of boards, straight from the web flasher, is **Mic 1** on **Wi-Fi channel 11**, with the **noise filter on**, the **high-pass filter on (200 Hz)** and **spoken feedback**. Most people never change anything. When you do, every setting lives in one text file on the local board.

## Changing a setting

1. With the local board plugged into the computer and running, **click its BOOT button** (press and release). It waits up to 15 s for the remote's latest status (LED: short blips, 3 a second), then restarts as a USB drive called **EXTENDER1** (the mic number; LED on with a short dark blink every second).
2. Open **`config.txt`**, change what you need, and save.
3. **Eject** the drive (or press RESET). The local board restarts as the microphone with the new settings and passes the remote's share on over the radio.

You can do this on any computer: the drive doesn't need the remote or any audio software. The drive holds more than settings; see [The EXTENDER drive](extender-drive.md).

## All settings

```
mic = 1              # 1-9: the PC sees "Extender Mic N" (both boards follow)
channel = 11         # Wi-Fi channel 1-11 (both boards follow); pick a quiet one, one pair per channel
feedback = voice     # voice or off: spoken announcements in the recording
noise_filter = on    # on (12 dB), strong (20 dB) or off (at the remote)
noise_adapt = normal # normal, slow or fast: how quickly the filter follows changes
tx_power = max       # max, or 2-20 (dBm): radio transmit power of both boards
cpu_mhz = 240        # 240 or 160: processor speed of both boards (160 runs a little cooler)
highpass = on        # on, off: remove rumble, wind and hum below highpass_hz (remote)
highpass_hz = 200    # 50-1000: high-pass cut-off in Hz
mic_level = 100      # 0-100: the adapter's microphone level, % of its range; adapter = leave it (AtomS3U: gain)
distance = normal    # near, normal or far: radio rate and audio format of the link (both boards)
mode = auto          # auto, microphone or speaker: which way the audio goes
remote = any         # any, or the remote's MAC address: lock the pair to each other
```

| Setting | More about it |
|---|---|
| `mic`, `channel`, `remote` | [Pairing and several mics](pairing.md) |
| `feedback` | [Voice feedback](voice-feedback.md) |
| `noise_filter`, `noise_adapt`, `highpass`, `highpass_hz` | [Noise and high-pass filters](noise-filter.md) |
| `distance`, `tx_power` | [Distance and range](distance.md) |
| `mode` | [Speaker mode](speaker-mode.md) |
| `cpu_mhz`, `mic_level` | below |

**How changes reach the remote.**

* **Most settings** (filters, adapt speed, transmit power, processor speed, mic level, distance, remote): the remote applies them at once and saves them, so it keeps them while the local board is off.
* **Mic number and channel**: a *handover*. Both boards move together, and the local board never moves without the remote. See [Pairing](pairing.md#changing-the-mic-number-or-channel).
* **Mode**: the remote saves it and restarts. If the audio direction changes, the local board restarts too, and the computer sees the new device. An [AtomS3U](atoms3u.md) remote is always a microphone.

A line the local board can't understand keeps the previous value; the log and `status.txt` name the line. **Delete `config.txt`** to return to the defaults (it is recreated with every setting explained).

### Processor speed and chip temperature

`cpu_mhz = 160` (default 240) runs both boards a little cooler; it changes on both at once, without a restart. Lower speeds aren't offered: the radio, USB host and noise filter need the headroom.

Each board reads its chip temperature **at its start** (before its radio starts) and **when you open the EXTENDER drive** (the audio stops then anyway); `status.txt` shows both boards' reading from the drive opening, and the start readings are in `events.txt`. A warning event marks either chip at 75 °C or more. It is the chip's own temperature, some 10–20 °C above the air. There are no readings while the audio runs: the radio uses the same sensor for its own calibration and has to be stopped for each reading, which costs audio and could make an AtomS3U remote restart. (Up to 1.4.3, `temperature = on` read both every 10 minutes; an old `temperature` line in `config.txt` is now ignored.)

### Mic level

Most USB audio adapters have a microphone level control: a gain stage inside the adapter. `mic_level = 0` to `100` sets it to that share of the adapter's range, in dB, when the adapter starts streaming and whenever it is plugged in again. The default is `100`, the top of the range (many adapters power up there anyway). Lower it if a sensitive mic clips, or if a lower level gives a quieter noise floor. `mic_level = adapter` leaves the level where the adapter puts it. `status.txt` shows the setting and the adapter's actual level and range in dB. Switching to `adapter` leaves the adapter at the last level set until it is unplugged. (`default` is still accepted and means `100`.) Adapters without a level control ignore the setting. In speaker mode it sets the adapter's speaker level.

On an [AtomS3U](atoms3u.md) remote, `mic_level` is the built-in microphone's digital gain instead: `0` to `100` = 0 to +30 dB, default `100` (+30 dB). Lower it if loud calls clip.

## BOOT button

| Board | Click (press and release) | Hold 10 s (LED goes dark), release |
|---|---|---|
| Local | open the EXTENDER drive | factory reset: settings, `config.txt` and voice files back to the defaults |
| Remote | — | factory reset: back to Mic 1, channel 11, filter on, unlocked (`remote = any`) |

Holding between 2 and 10 s does nothing. Don't hold BOOT while plugging a board in or pressing RESET: that starts the firmware-download mode instead (which is what you want only when [flashing the remote again](https://dalogue1.github.io/wireless-usb-audio-extender/flash/)).
