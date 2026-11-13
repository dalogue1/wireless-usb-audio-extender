# LEDs, logs and troubleshooting

[← Back to the main page](../README.md)

## LEDs

| Pattern | Remote | Local |
|---|---|---|
| N short blinks at power-up | mic number N | mic number N |
| slow blink (1 s) | looking for the local board | looking for the remote |
| double blink every 2 s | linked, no adapter plugged in | linked, no audio arriving |
| brightness follows the sound, faint glow in silence | **level meter**: adapter streaming, linked | **level meter**: audio arriving, the computer using it |
| level meter + quick flash every 2 s | adapter streaming, not linked yet | audio arriving, the computer not using it |
| fast blink | adapter not usable, or **no microphone signal** (20 s of silence) | the remote reports either of those |
| short blips, 3 a second | — | BOOT clicked: waiting for the remote before opening the drive |
| on, short dark blink every second | — | EXTENDER drive |
| dark while holding BOOT 10 s | release: factory reset | release: factory reset |

## Common problems

| Symptom | Check |
|---|---|
| The computer doesn't see the local board | Use a USB **data** cable (many are charge-only). On Linux, see the USB power-saving note below. |
| Local LED blinks slowly forever | The remote isn't powered, is out of range, or is on another mic number or channel ([Pairing](pairing.md#remote-and-local-on-different-settings)). Both boards need the same firmware version. |
| Fast blink, "No microphone signal" | The mic's plug doesn't match the adapter's jack (below), or it isn't pushed fully in. |
| Fast blink, "Adapter not supported" | `adapter.txt` says why: usually a USB Audio Class 2-only adapter, or no 48 kHz. |
| Audio drops out | Signal strength and lost audio in `report.html`; try [`distance`](distance.md), the antenna placement, or another `channel`. |
| BirdNET-Go: "No audio sources available" after replugging | Restart its container, or reboot the computer ([below](#restarting-your-software)). |
| A board seems stuck | Both recover by themselves (below). If in doubt, send `report.html` and `events.txt` from the [drive](extender-drive.md). |

### Microphone plugs

Adapters come with either one combined 4-pole jack (**TRRS**, for headsets) or separate 3-pole jacks (**TRS**: green for headphones, pink for the microphone). A TRRS microphone in a TRS mic jack, or the other way round, gives **silence**: the mic contact lands on ground. Use a mic with the plug that matches the jack, or a TRRS-to-mic/headphone splitter cable. After 20 s of silence the remote's LED blinks fast, the log says `NO MICROPHONE SIGNAL` and the local board says "No microphone signal".

## Restarting your software

Some software only finds audio devices when it starts. **BirdNET-Go in Docker** is one: the container gets the computer's sound devices when it starts, so if the local board disappears and comes back, BirdNET-Go keeps saying "No audio sources available" until the container is restarted.

The local board disappears from the computer and comes back when:

* it is unplugged and plugged in again;
* its EXTENDER drive is opened and ejected;
* it is updated or restarted, or a change of `mic`, `channel` or `mode` restarts it.

Problems on the remote side (link drops, remote restarts, adapter unplugged) do **not** do this: the computer keeps the microphone and gets silence until the audio is back.

Afterwards, restart the container in one of these ways:

* **From a browser,** if the computer has the Cockpit web console: open `http://<computer>:9090`, go to **Services**, open **birdnet-go** and choose **Restart**. Handy when the computer has no screen.
* **From a terminal,** on the computer itself or over SSH from another one (`ssh <user>@<computer>`): `sudo systemctl restart birdnet-go` (the standard BirdNET-Go install) or `docker restart <container name>`.
* **Reboot** the computer.

Other recording software may need the same: if audio doesn't come back after a replug, restart it.

## Automatic recovery

* **Short dropouts** (interference, someone walking past) recover by themselves within a second or two.
* **Remote radio stuck:** the remote recognises when its radio has stopped getting packets through and restarts itself; audio is back within about 3–6 s.
* **Link down for longer:** after 15 s the local board restarts its radio (not the board, so the computer keeps the microphone, sending silence); after 20 s the remote restarts itself completely, which also clears a stuck USB host.
* **Program hang:** on either board, a watchdog restarts it after 5 s.
* **USB to the computer:** if the computer hasn't set the local board up for 30 s, or stops collecting audio for 5 s while the stream is open, the board disconnects and reconnects itself in software, as if replugged. A sleeping computer (suspended USB) is left alone.

Everything is logged, with the reasons, in `events.txt`.

## Serial log

The local board also appears as a USB serial port (`/dev/ttyACM0` on Linux, a COM port on Windows, any baud rate). It shows the local board's log and, while linked, the remote's. When a serial monitor connects, the last 64 lines since boot are replayed first.

```
    0.9 [local] Wireless USB Audio Extender local 1.4.0-dev | XIAO ESP32-S3 | MAC 28:84:85:...
    0.9 [local] local settings: mic 1 | channel 11 | 240 MHz | distance normal (6 Mbit/s) | remote any
    1.7 [local] linked to remote 34:85:18:...
            [remote]     2.4 USB microphone connected: 12d1:0010 "KT USB Audio", 48000 Hz, 1 ch, 16-bit, ...
   11.0 [local] remote linked (streaming), RSSI -52 dBm | PC recording: yes | buffer 60 ms | drift +18 ppm | lost 0 late 0 | underruns 0
```

Every 10 s a stats line: **lost** = audio packets that never arrived (12 ms of silence each), **late** = duplicates dropped, **underruns** = the buffer ran dry while the computer was recording, **drift** = how far the adapter's clock and the computer's differ.

## Linux notes

* USB power saving (autosuspend) can drop USB audio devices. [`tools/linux/99-usb-audio-extender.rules`](../tools/linux/99-usb-audio-extender.rules) turns it off for the extender; installing it is described in the file.
* `arecord -l` lists the microphone (card `E1` for Mic 1). Listen with `arecord -D plughw:CARD=E1,DEV=0 -f S16_LE -r 48000 -c 1 | aplay`.

## Limits

* USB Audio Class 1 adapters only, with 48 kHz. "Hi-Res" USB Audio Class 2-only adapters are refused.
* One adapter per remote (no USB hub), and one channel (mono).
* One direction at a time: microphone or speaker.
