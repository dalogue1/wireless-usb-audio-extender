# Voice feedback

[← Back to the main page](../README.md)

The local board can tell you what's happening by **speaking into the recording**: which microphone you're listening to, whether the link is up, whether the adapter is plugged in. That's handy when you're setting a mic up out of sight of the computer: put on headphones, and the extender tells you how it's doing.

## What it says, and when

| When | Announcement | File in `voice/` |
|---|---|---|
| the computer starts recording from the mic (at most once a minute) | "Microphone one" … "Microphone nine" | `mic_1.wav` … `mic_9.wav` |
| the radio link comes up | "Microphone connected" | `link_up.wav` |
| the radio link is lost | "Microphone link lost" | `link_lost.wav` |
| an adapter is plugged into the remote | "Adapter connected" | `adapter_connected.wav` |
| the adapter is unplugged | "Adapter removed" | `adapter_removed.wav` |
| the adapter can't be used (see `adapter.txt`) | "Adapter not supported" | `adapter_unsupported.wav` |
| the noise filter setting changes | "Noise filter on" / "strong" / "off" | `filter_on.wav`, `filter_strong.wav`, `filter_off.wav` |
| the adapter sends only silence for 20 s | "No microphone signal" | `no_signal.wav` |

How it sounds:

* **Only while the computer is recording.** Nothing is queued up for later: if nobody is recording, nobody hears it, and it isn't said.
* The voice is mixed in at about −12 dB, and the microphone is turned down by 12 dB underneath it (with short fades, so nothing clicks), then comes back.
* In [speaker mode](speaker-mode.md) there are no announcements.

## Turning it off, or using beeps

`feedback` in [`config.txt`](configuration.md):

* `feedback = voice` (the default): spoken announcements.
* `feedback = off`: no announcements at all.

**For unattended recording**, such as BirdNET-Go or BirdNET-Pi, consider `off`: the announcements become part of the recording, and spoken words can trigger their privacy (human voice) filters, which throw away detections around them. The **beeps** voice pack (below) is a middle way: tone patterns instead of words, so you still hear what happened.

## Other languages: voice packs

The announcements are ordinary WAV files in the `voice` folder on the [EXTENDER drive](extender-drive.md), so changing the language is just copying files. The [`voice-packs/`](../voice-packs/) folder of this repository has ready-made sets of all 18:

| Pack | Language |
|---|---|
| *(built in)* | English (American) |
| [`beeps`](../voice-packs/beeps/) | tone patterns instead of words |
| [`en-gb`](../voice-packs/en-gb/) | English (British) |
| [`es`](../voice-packs/es/) | Spanish |
| [`fr`](../voice-packs/fr/) | French |
| [`it`](../voice-packs/it/) | Italian |
| [`pt-br`](../voice-packs/pt-br/) | Portuguese (Brazilian) |
| [`hi`](../voice-packs/hi/) | Hindi |
| [`ja`](../voice-packs/ja/) | Japanese |
| [`zh`](../voice-packs/zh/) | Mandarin Chinese |

The spoken packs were made with [Kokoro TTS](https://github.com/hexgrad/kokoro) and machine-translated phrases; each folder's `phrases.txt` says what each file says. Native speakers may well find better wording: corrections are welcome.

**The beeps:** "Microphone 1 … 9" is 1 to 9 short beeps; link up / lost a rising / falling two-tone; adapter connected / removed / not supported two short high beeps / one low / a low buzz; noise filter on / strong / off a short / long / double beep; no microphone signal three low beeps.

### Installing a pack

1. **Download** the pack: open its folder above and download each `.wav` file, or download the whole repository (green **Code** button → **Download ZIP**) and find the pack inside `voice-packs/`.
2. **Open the drive:** click the local board's **BOOT** button; the EXTENDER drive appears.
3. **Copy** the pack's `.wav` files into the drive's `voice` folder, and let them **replace** the files with the same names.
4. **Eject** the drive (Windows: "Eject"; macOS: drag to the bin or ⏏; Linux: unmount). The local board restarts and speaks the new files.

Check `status.txt` the next time you open the drive: `voice files: 18 of 18 usable`. If a file couldn't be used, it is named there with the reason (for example `mic_4.wav (empty file)`), and that one announcement plays the built-in English voice instead.

**Back to the built-in English voice:** delete the files in `voice` and eject. The local board puts the built-in clips back at its next start. (A factory reset, BOOT held 10 s, does the same, but also resets `config.txt`.)

You can mix packs or replace single files: each file is used on its own.

## Your own voice

Any announcement can be your own recording: your voice, your words, any language.

1. **Record** each phrase on its own, in a quiet room: a phone's voice recorder is fine. Say just the phrase, about 1–2 seconds.
2. **Save each as a WAV file:** 16-bit PCM, mono or stereo, 8,000–48,000 Hz. Phones usually record in another format (`.m4a`); [Audacity](https://www.audacityteam.org/) (free) converts it: open the file, then *File → Export Audio → WAV, Signed 16-bit PCM*. 16 kHz mono is plenty for speech.
3. **Name each file** exactly like the one it replaces (the table at the top), and copy it into the drive's `voice` folder as for a pack.

No need to trim the silence or match the loudness: the local board trims the silence at both ends and evens out the level of every file. Keep the whole set under about **800 KB** (the built-in set is about 630 KB), so the drive still has room for the event log and reports.

## Sharing a pack

Put the 18 files and a `phrases.txt` (what each file says, and who or what made it) in a new folder named after the language code (for example `de`, `nl`, `pl`) and open a pull request on this repository. Recordings must be your own, or made with a voice whose licence allows sharing them. How the packs here were generated, and how to make one with Kokoro TTS: [voice-packs/README.md](../voice-packs/README.md#making-your-own-pack).

**A different built-in voice:** the voice built into the firmware comes from `voice/*.wav` in this repository (Kokoro TTS, voice af_heart, Apache-2.0). To make another set the default, copy it into `voice/`, run `python3 tools/make_voice.py`, and [build the local board's firmware](building.md).
