# Voice packs

The local board announces events into the recording ("Microphone one", "Microphone connected", …). It speaks whatever WAV files are in the `voice` folder on its EXTENDER drive. A voice pack is a ready-made set of those 18 files.

| Pack | Language | Kokoro voice |
|---|---|---|
| *(built in)* | English (American) | af_heart |
| `beeps` | tone patterns instead of words | — |
| `en-gb` | English (British) | bf_emma |
| `es` | Spanish | ef_dora |
| `fr` | French | ff_siwis |
| `it` | Italian | if_sara |
| `pt-br` | Portuguese (Brazilian) | pf_dora |
| `hi` | Hindi | hf_alpha |
| `ja` | Japanese | jf_alpha |
| `zh` | Mandarin Chinese | zf_xiaobei |

Each language folder has a `phrases.txt` listing what each file says. The spoken packs were made with [Kokoro TTS](https://github.com/hexgrad/kokoro) (Apache-2.0) by `tools/kokoro_wavs.py`. The phrases were machine-translated, so native speakers may find better wording: corrections are welcome.

## Using a pack

1. Click the local board's **BOOT** button: the **EXTENDER** drive opens.
2. Copy the pack's `.wav` files into the drive's `voice` folder, replacing the files there.
3. **Eject** the drive. The local board restarts and speaks the new files. Its log and `status.txt` show how many files it could use (`voice drive: 18 of 18 clips usable`).

**Back to the built-in English voice:** delete the files in `voice` and eject. The local board puts the built-in clips back at the next start. A factory reset (BOOT held 10 s) does the same, but also resets `config.txt`.

You can mix packs, or replace single files: each file is used on its own, and a missing or unusable file plays the built-in clip.

## The beeps pack

| Announcement | Beeps |
|---|---|
| Microphone 1 … 9 (`mic_1` … `mic_9`) | 1 to 9 short beeps |
| link up / lost | rising / falling two-tone |
| adapter connected / removed / not supported | two short high / one low / low buzz |
| noise filter on / strong / off | short / long / two beeps |
| no microphone signal | three low beeps |

Beeps are less likely than words to trip speech-detecting software (e.g. BirdNET-Go's privacy filter). To silence announcements entirely, set `feedback = off` in `config.txt`.

## Making your own pack

**File names** (all 18 are needed for a complete pack; the names must match exactly):

```
mic_1.wav … mic_9.wav     "Microphone one" … "Microphone nine"
link_up.wav               "Microphone connected"
link_lost.wav             "Microphone link lost"
adapter_connected.wav     "Adapter connected"
adapter_removed.wav       "Adapter removed"
adapter_unsupported.wav   "Adapter not supported"
filter_on.wav             "Noise filter on"
filter_strong.wav         "Noise filter strong"
filter_off.wav            "Noise filter off"
no_signal.wav             "No microphone signal"
```

**Format:** WAV, 16-bit PCM, mono or stereo, 8000 to 48000 Hz, about 1–2 s each. The local board trims silence at the ends and evens out the loudness, so there is no need to normalise. Keep the whole set under about 800 KB (the built-in set is about 630 KB): the drive holds about 1.4 MB, and it also keeps `config.txt`, the reports and `events.txt`, which can grow to a few hundred KB. 16 kHz mono (the rate the packs here use) is plenty for speech: about 32 KB per second.

**Recording your own voice:** any recorder will do. Say each phrase on its own, in a quiet room, and save each as its own file.

**With Kokoro TTS:** add your language to `PACKS` in `tools/kokoro_wavs.py` (a Kokoro voice, its language code and the 18 phrases in order), then run it as its header describes. Kokoro's voices and languages: [VOICES.md](https://huggingface.co/hexgrad/Kokoro-82M/blob/main/VOICES.md).

**Contributing a pack:** put the 18 files and a `phrases.txt` (what each file says, and who or what made it) in a new folder named after the language code (e.g. `de`, `nl`, `pl`), and open a pull request. Recordings must be your own, or made with a voice whose licence allows sharing them.

**Building a pack into the firmware** (as the default instead of English): copy its files into the project's `voice/` folder and run `python3 tools/make_voice.py`, then build the local sketch.
