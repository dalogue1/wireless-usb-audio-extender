# Adaptive Noise Filter: Technical Basis

*The established methods behind the Wireless USB Audio Extender's noise filter. dalogue1, October 2026; updated November 2026.*

The adaptive noise filter is not a new algorithm. It is a fixed-point, microcontroller-sized implementation of two well-established speech-enhancement techniques: **spectral subtraction with a spectral floor**, driven by a **minimum-statistics noise estimate**. This page names each technique, says where it comes from, and explains how the filter uses or simplifies it. How the filter is built into the remote board is described in [noise-filter-technical.md](noise-filter-technical.md).

## 1. Short-time Fourier analysis and overlap-add synthesis

The audio is processed in overlapping frames: 512 samples, a new frame every 256 samples (50 % overlap), with a square-root Hann window before the FFT and again after the inverse FFT. Because the squared windows sum to a constant, the output equals the input when all gains are 1 (perfect reconstruction). This is the standard weighted overlap-add (WOLA) framework used by essentially all frequency-domain noise suppressors.

* **In the filter:** 257 bands of 93.75 Hz at 48 kHz; 10.7 ms total delay; integer FFT with Q31 twiddles. Host tests confirm reconstruction within one least-significant bit.

## 2. Spectral subtraction (the filtering)

Spectral subtraction estimates the noise spectrum and removes it from each frame, band by band, while keeping the original phase. Boll (1979) introduced it for speech. Berouti, Schwartz and Makhoul (1979) added two refinements that almost every later implementation keeps:

* **over-subtraction:** subtracting somewhat more than the estimated noise;
* a **spectral floor:** never reducing a band below a fixed fraction of its level, which limits the characteristic artifacts.

How the filter uses them:

* **In the filter:** the per-band gain is the power-subtraction rule g = sqrt(1 − N/P), where N is the learned noise power and P the smoothed band power, read from a 256-entry table.
* **Spectral floor** = the `noise_filter` setting: **on** = 12 dB, **strong** = 20 dB maximum reduction.
* **Over-subtraction** is available as a parameter (default 1.0, i.e. none) and exposed in the WAV test tool (`tools/noise_wav.cpp`).

## 3. Minimum statistics (learning the noise)

Spectral subtraction needs a noise estimate that keeps up with changing conditions without a separate "silence detector". Rainer Martin's minimum-statistics method (1994, 2001) supplies it:

* In each band, track the minimum of the smoothed power over a sliding window of about 1–2 seconds or longer.
* The wanted signal (speech, or here bird calls) comes and goes while the noise is always present, so the minimum sits on the noise floor.
* The minimum is biased low, so it is multiplied by a bias-compensation factor.
* The window is split into subwindows so the minimum can be updated cheaply.

How the filter uses it:

* **In the filter:** about 40 ms of first-order smoothing per band, and the minimum over 8 subwindows of 4, 15 or 60 s (the `noise_adapt` setting: fast, normal, slow). That gives a 32 s, 2 min or 8 min window, much longer than in speech because bird song can last tens of seconds.
* **Simplified** compared with Martin (2001): fixed smoothing instead of his optimal, time-varying smoothing; and a fixed bias correction calibrated on white noise (7.65 dB at 94 frames per subwindow, +0.47 dB per doubling) instead of his analytic, variance-based one. This keeps the filter to integer arithmetic on the ESP32.
* **Added:** the minima are folded into a slowly updated long-term "signature" (a plain average for the first 16 updates, then 1/16 per update), so a long song does not immediately raise the noise estimate.

## 4. Musical noise and gain smoothing

The best-known weakness of spectral subtraction is **musical noise**: isolated time-frequency points of residual noise that randomly survive the subtraction and sound like faint twinkling tones. Cappé (1994) analysed why the Ephraim–Malah suppressor avoids it, showing that smoothing the gains over time is the key.

* **In the filter:**
    * decisions use the ~40 ms smoothed power rather than the raw periodogram;
    * gains rise instantly, so call onsets are kept, but fall at most 60 dB/s;
    * the spectral floor limits how deep any residual can be cut.
* Spectrograms of filtered test-bench recordings (cardinal and munia calls) showed no musical noise.

## Summary

| Filter component | Technique | Main reference |
|---|---|---|
| Framing and reconstruction | STFT, sqrt-Hann windows, 50 % overlap-add | standard (see Loizou) |
| Per-band gain | Power spectral subtraction, g = sqrt(1 − N/P) | Boll 1979 |
| Maximum reduction (on / strong) | Spectral floor | Berouti et al. 1979 |
| Over-subtraction parameter | Over-subtraction | Berouti et al. 1979 |
| Noise learning | Minimum statistics with bias compensation (simplified) | Martin 1994, 2001 |
| Gain release, smoothed decisions | Musical-noise reduction by gain smoothing | Cappé 1994 |

## References

1. S. F. Boll, "Suppression of acoustic noise in speech using spectral subtraction," *IEEE Transactions on Acoustics, Speech, and Signal Processing*, vol. 27, no. 2, pp. 113–120, 1979.
2. M. Berouti, R. Schwartz and J. Makhoul, "Enhancement of speech corrupted by acoustic noise," in *Proc. IEEE ICASSP*, 1979, pp. 208–211.
3. R. Martin, "Spectral subtraction based on minimum statistics," in *Proc. EUSIPCO*, Edinburgh, 1994, pp. 1182–1185.
4. R. Martin, "Noise power spectral density estimation based on optimal smoothing and minimum statistics," *IEEE Transactions on Speech and Audio Processing*, vol. 9, no. 5, pp. 504–512, 2001.
5. O. Cappé, "Elimination of the musical noise phenomenon with the Ephraim and Malah noise suppressor," *IEEE Transactions on Speech and Audio Processing*, vol. 2, no. 2, pp. 345–349, 1994.
6. P. C. Loizou, *Speech Enhancement: Theory and Practice*, 2nd ed., CRC Press, 2013 (chapters on spectral subtraction and noise estimation).
