# Noise filter (remote): technical description

[← Back to the noise filter](noise-filter.md) · [Main page](../README.md)

The remote board removes the steady background noise of the microphone and the site (capsule hiss, adapter preamp noise, hum, distant traffic, HVAC) before the audio goes over the radio. Sounds that come and go, such as bird calls, pass through. The user-level page is [noise-filter.md](noise-filter.md); this file explains how the filter works inside. The published methods it is based on, with references, are in [noise-filter-basis.md](noise-filter-basis.md).

Source: `AudioExtender_Remote/NoiseSignature.h` (the engine, plain C++, no Arduino dependencies), used by the audio task in `AudioExtender_Remote.ino`.

## Where it sits

```
USB adapter -> UsbMic (int32) -> int16 block (192 samples, 4 ms) -> level meter LED
            -> no-signal check -> high-pass filter -> noise filter (in place) -> ESP-NOW packet (3 blocks, 12 ms)
```

* The level meter and the no-signal check see the audio **as captured**, before the filter.
* A **high-pass filter** (`shared/HighPass.h`: 2nd-order Butterworth, float, default on at 200 Hz) runs first, so the noise filter's reduction is not spent on rumble below the cut-off. Changing it is not a reason to relearn: the noise floor below the cut-off simply drops and the tracker follows within one tracker window.
* The filter runs at 48 kHz, mono, 16-bit, the link's only format.

## Settings

| Setting | Values | Effect |
|---|---|---|
| `noise_filter` (config.txt) | **on** (default), strong, off | Maximum reduction per frequency bin: on = 12 dB, strong = 20 dB. Off passes the audio unchanged. |
| `noise_adapt` (config.txt) | **normal** (default), slow, fast | How quickly the learned background follows changes (table below). |

* The remote saves the mode and adapt speed in its own flash settings, so it keeps them while the local board is off.
* The **engine always runs and keeps learning**, even when the filter is off: "off" only discards its output. So switching between on, strong and off takes effect at once, without relearning, and the delay through the remote stays constant.
* Changing the strength or the adapt speed also keeps what has been learned (`nsig::setParams`).

## How it works

Each 256-sample hop (5.33 ms at 48 kHz), in integer math only:

1. **Analyse.**
   * Apply a sqrt-Hann window and a 512-point real FFT (int32 data, Q31 twiddles, one 32×32→high-word multiply per product, scaled by half at every stage).
   * Compute the power of each bin as log2 in Q8: 257 bins, 93.75 Hz each.
2. **Track the floor.**
   * Smooth each bin's level over about 40 ms, and track the minimum of that smoothed level over 8 subwindows (minimum statistics).
   * Calls come and go, while hiss is always there, so the minimum over the window sits on the hiss.
   * A bias turns the minimum into the mean noise power. It was measured on white noise: 7.65 dB at 94 frames per subwindow, +0.47 dB per doubling, and checked to within 0.1 dB at 4, 15 and 60 s subwindows.
3. **Learn the signature.**
   * At the end of each subwindow, the tracker floor is folded into a long-term signature: a plain average of the first 16, then a 1/16 update, so it keeps re-learning gradually.
   * The signature counts as **learned** after 2 minutes, or 2 subwindows if that is longer. The remote then reports it, and the local board logs `background noise learned`.

   | `noise_adapt` | Subwindow | Tracker window | Signature time constant |
   |---|---|---|---|
   | fast | 4 s | 32 s | ~1 min |
   | normal | 15 s | 2 min | ~4 min |
   | slow | 60 s | 8 min | ~16 min |

   Slower settings are less likely to mistake a long, nearly continuous song (a dawn chorus) for background. The cost is that real changes, such as HVAC cycling, are followed more slowly.
4. **Apply.**
   * Each bin gets a spectral-subtraction gain from a lookup table, `g = sqrt(1 − N/P)`, limited to the maximum reduction (12 or 20 dB).
   * Gains rise instantly, so call onsets are kept, and fall at up to 60 dB/s to avoid "musical noise".
   * Then the inverse FFT and a 50 % overlap-add.
   * The output is written back into the block in place, through a 3-hop FIFO. So the remote's 192-sample blocks work directly, with a **constant delay of 512 samples (10.7 ms)**.

**Steady tones.** A steady tone (mains hum, coil whine, a fan's whine) cannot be told apart from noise by this method, so it is removed too. With 94 Hz bins, mains hum (50/60 Hz and its low harmonics) shows up as a raised lowest bin or two, not a sharp line.

## Learning fresh, never saved

The signature is learned from scratch whenever the adapter's stream starts: at power-up, after plugging the adapter in again, and after the USB host reconnects it.

* It is **never written to flash**. A saved signature would only fit the same adapter, capsule, level and site, so starting fresh is simpler and nothing stale can be loaded.
* **The cost of starting fresh:**
  * The first subwindow (15 s on normal) passes unfiltered.
  * After that, the filter works from the short-term floor while the signature builds: learned at about 2 minutes, settled at about 4.
  * For bird detection over hours this is negligible.
* **Settling hold.** At start-up, minimum tracking pauses for 32 frames (about 0.2 s). So the first frames cannot set the floor too low; without the hold, the signature would be about 1 dB low for several minutes.

## Cost on the remote

| | |
|---|---|
| RAM | 21,872 bytes (`sizeof(nsig::Engine)`), allocated once at boot in internal RAM. The window and FFT tables are computed into it, not stored in flash. If the allocation fails, the remote logs an error and sends the audio unfiltered. |
| Flash | About 10 KB of code and tables (the log2 table). No libm. |
| CPU | Not measured on the remote yet. The same engine took about 20 % of one 240 MHz ESP32-S3 core at 48 kHz in an earlier measurement; the remote's audio task does little else. |
| Delay | 10.7 ms, constant, whether the filter is on or off. |

## Hearing it on a recording

`tools/noise_wav.cpp` runs the same engine on a WAV file. It learns from the whole clip, then applies the result from the start:

```sh
g++ -std=c++17 -O2 -IAudioExtender_Remote tools/noise_wav.cpp -o noise_wav
./noise_wav clip.wav clip-denoised.wav        # like "on" (12 dB)
./noise_wav clip.wav clip-strong.wav -20      # like "strong"
```

Record a clip with the filter **off** (for example with `arecord` from "Extender Mic 1"), run it through the tool, and listen to both. Running BirdNET on both files is the real test of whether the filter helps detection at your site. The tool also writes the learned signature as a CSV (dBFS per bin), which shows where the background noise sits in frequency.

## Tests

`tests/noise_signature_test.cpp` (part of `tests/run_tests.sh`) checks:

* The FFT matches a double-precision DFT (relative error about 1e-6).
* With all gains at 1, the output reconstructs the input within 1 LSB, from near-silence up to full-scale square waves (no overflow).
* On 30 s of white noise, the learned signature is within 0.2 dB of the true level, and the hiss is reduced by 11 dB.
* Intermittent 5–6 kHz chirps change by less than 0.1 dB. A steady tone shows up 45 dB above the floor in the signature.
* A 90 s continuous warbling song after 2 minutes of hiss: fast adapt raises the signature in the song's band by about 9 dB (it starts treating the song as background); normal raises it by under 1 dB.
* In-place mode with random block sizes from 1 to 8192 samples returns the input delayed by exactly 512 samples, within 1 LSB.
* Relearn: after a loud input is replaced by one 20 dB quieter, the floor matches a fresh start within 0.5 dB. Without the settling hold, a 0.1 s gap would drag it down by over 50 dB.

## Possible next steps

* Measure the remote's CPU time for the filter, and log it with the other status values.
* A several-day comparison in BirdNET-Go with the filter on and off at the same site.
* If steady low-frequency noise (fans, HVAC) dominates at a site, a 1024-point FFT would separate it more finely, at about twice the RAM.
