# Distance and range

[← Back to the main page](../README.md)

## The distance setting

`distance` in [`config.txt`](configuration.md) sets the radio rate of the paired link, on both boards. The slower the rate, the further it reaches, but the longer each packet occupies the channel and the more easily other Wi-Fi traffic gets in the way. The audio stream is 83 packets a second of about 1.2 KB.

| Setting | Radio rate | Air time used | Best for |
|:---|:---:|:---:|:---|
| `distance = near` | 12 Mbit/s | about 7 % | busy Wi-Fi around; the most robust |
| `distance = normal` (default) | 6 Mbit/s | about 14 % | most sites: 2–3 dB more reach than `near` |
| `distance = far` | 2 Mbit/s | about 45 % | the longest reach, where Wi-Fi is quiet |

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

All pairs start on Wi-Fi **channel 11**. If a router nearby uses channel 9–11, a quieter channel helps: set `channel` in `config.txt` (both boards move together). On Linux, `nmcli -f SSID,CHAN,SIGNAL dev wifi list` shows which channels nearby networks use. The extender needs no Wi-Fi network of its own; it only shares the air.
