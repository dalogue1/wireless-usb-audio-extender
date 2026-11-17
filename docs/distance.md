# Distance and range

[← Back to the main page](../README.md)

## The distance setting

`distance` in [`config.txt`](configuration.md) sets the radio rate of the paired link, on both boards. The slower the rate, the further it reaches, but the longer each packet occupies the channel and the more easily other Wi-Fi traffic gets in the way. The audio stream is 83 packets a second of about 1.2 KB.

| Setting | Radio rate | Air time used | Best for |
|:---|:---:|:---:|:---|
| `distance = near` | 12 Mbit/s | about 7 % | busy Wi-Fi around; the most robust |
| `distance = normal` (default) | 6 Mbit/s, µ-law audio | about 10 % | most sites: 2–3 dB more reach than `near` |
| `distance = far` | 2 Mbit/s, ADPCM audio | about 16 % | the longest reach |

Since 1.4.5 the remote compresses the audio at `normal` and `far`, so it takes less of the channel and leaves room for retries and other Wi-Fi:

* **`near`:** uncompressed, 16 bits per sample.
* **`normal`:** 8-bit µ-law (the telephone code), half the size. Its noise stays about 31 dB below the background in a real outdoor recording: no practical loss.
* **`far`:** IMA ADPCM, 4 bits per sample, a quarter of the size; uncompressed, the slow rate took about 45 % of the channel. Its noise sits about 14–19 dB below the background in the 3–10 kHz bird band, which lowers a faint call's margin by roughly 0.1–0.2 dB.

## Audio compression

`audio_compression` in [`config.txt`](configuration.md) (1.4.7 and later, both boards) chooses the microphone audio format on its own. Most people leave it at `auto`.

| Setting | Format | Packet | Sound |
|:---|:---|:---:|:---|
| `auto` (default) | follows `distance`: `near` = none, `normal` = low, `far` = high | | |
| `none` | uncompressed, 16 bits per sample | 1166 bytes | exactly as captured |
| `low` | 8-bit µ-law | 590 bytes | added noise far below any outdoor background |
| `high` | IMA ADPCM, 4 bits per sample | 305 bytes | a small loss on faint, high calls |

"High" means the most compression: the smallest packets, and the most loss, **not** the best sound. Smaller packets leave more of the channel free for retries and other Wi-Fi.

When to change it:

* **To compare.** Set `distance = far` with `audio_compression = none`, then `high`, to hear what the compression does at the same radio rate.
* **To save air time** on a busy channel, for example `distance = near` with `audio_compression = low`.
* **Not** `none` at `distance = far` for long: uncompressed at 2 Mbit/s uses about 45 % of the channel, which was fragile in testing.

`status.txt` and the log show the format in use, and name the setting when it isn't `auto`: `distance far (2 Mbit/s, mu-law audio, audio_compression low)`. Speaker mode always uses ADPCM, whatever this setting.

## Which distance to choose

The default is the furthest-reaching rate that comfortably fits the audio. If the link drops audio where Wi-Fi is busy (the stats lines or `report.html` show lost audio), try `near`; if the remote is simply too far away, try `far`.

## Antennas and placement

These matter more than any setting:

* The XIAO ESP32-S3 has **no antenna on the board**, only a small connector. Snap the supplied antenna on **before powering up**, and secure it with a dab of hot glue or tape.
* Keep the local board's antenna **outside the computer's metal case**: a short USB extension cable helps.
* Keep the two boards **at least about 1 m apart**: closer than that, the radios overload each other.
* Height and a clear line of sight help a lot; walls, trees full of leaves, and the ground all eat range.

**Reading the signal strength** (RSSI in `status.txt`, `report.html` and the serial log): −40 to −70 dBm good, −70 to −80 fair; below about −85 the link starts to drop.

## Transmit power

`tx_power = max` (the default) is the chip's full output, about 20 dBm; both boards use it. A lower value (2–20 dBm) is only useful to keep several pairs that sit close together from shouting over each other, or to be polite on a crowded channel. More range comes from antennas and `distance`, not from this setting.

## Choosing a channel

All pairs start on Wi-Fi **channel 11**. If a router nearby uses channel 9–11, a quieter channel helps: set `channel` in `config.txt` (both boards move together). **With more than one pair, give each its own channel** (1, 6, 11): pairs on one channel share its air time ([Pairing](pairing.md#several-mics)). On Linux, `nmcli -f SSID,CHAN,SIGNAL dev wifi list` shows which channels nearby networks use. The extender needs no Wi-Fi network of its own; it only shares the air.
