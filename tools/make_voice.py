#!/usr/bin/env python3
"""Builds the local board's built-in voice clips from voice/*.wav.

    python3 tools/make_voice.py

For each announcement it reads voice/<name>.wav (16-bit PCM, any sample rate,
mono or stereo), mixes to mono, resamples to 16 kHz, trims silence at both
ends, normalises the peak to -1 dBFS and encodes it as IMA ADPCM (4 bits per
sample). The result goes to AudioExtender_Local/voice_clips.h, which the
Arduino IDE compiles into the local board. Every announcement needs a file:
a missing or unusable one stops the script. The decoded clips are also written to build/voice-preview/ so you
can hear exactly what the local board will play.

Standard library only: works with any Python 3.8+ on Linux, macOS or Windows.
To make another language, replace the WAV files (same names) and rerun.
"""
import math
import os
import struct
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOICE_DIR = os.path.join(ROOT, "voice")
OUT_H = os.path.join(ROOT, "AudioExtender_Local", "voice_clips.h")
PREVIEW_DIR = os.path.join(ROOT, "build", "voice-preview")

# Order must match announcer::Event in AudioExtender_Local/Announcer.h.
CLIPS = [
    "mic_1", "mic_2", "mic_3", "mic_4", "mic_5", "mic_6", "mic_7", "mic_8", "mic_9",
    "link_up", "link_lost",
    "adapter_connected", "adapter_removed", "adapter_unsupported",
    "filter_on", "filter_strong", "filter_off",
    "no_signal",
]
RATE = 16000

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("not 16-bit PCM")
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch > 1:
        s = [sum(s[i:i + ch]) / ch for i in range(0, len(s) - ch + 1, ch)]
    return [float(v) for v in s], rate


def resample(x, src, dst):
    """Windowed-sinc resampler (Blackman, 32 taps per side), with an anti-alias cutoff."""
    if src == dst:
        return list(x)
    ratio = dst / src
    cutoff = min(1.0, ratio) * 0.95
    half = 32
    n_out = int(len(x) * ratio)
    out = []
    for i in range(n_out):
        t = i / ratio
        c = int(math.floor(t))
        acc = 0.0
        norm = 0.0
        for k in range(c - half + 1, c + half + 1):
            if 0 <= k < len(x):
                d = t - k
                arg = math.pi * d * cutoff
                sinc = 1.0 if abs(arg) < 1e-9 else math.sin(arg) / arg
                win = 0.42 + 0.5 * math.cos(math.pi * d / half) + 0.08 * math.cos(2 * math.pi * d / half)
                wgt = sinc * win
                acc += x[k] * wgt
                norm += wgt
        out.append(acc / norm if norm else 0.0)
    return out


def trim_normalise(x):
    peak = max((abs(v) for v in x), default=0.0)
    if peak < 1:
        raise ValueError("silent")
    thr = max(peak * 0.03, 60.0)
    first = next(i for i, v in enumerate(x) if abs(v) > thr)
    last = len(x) - next(i for i, v in enumerate(reversed(x)) if abs(v) > thr)
    pad = int(0.03 * RATE)
    x = x[max(0, first - pad):min(len(x), last + pad)]
    gain = 32767 * 0.891 / peak  # -1 dBFS
    y = [max(-32768, min(32767, int(round(v * gain)))) for v in x]
    fade = int(0.005 * RATE)  # 5 ms fades: no clicks at the ends
    for i in range(min(fade, len(y))):
        y[i] = int(y[i] * i / fade)
        y[-1 - i] = int(y[-1 - i] * i / fade)
    return y


def ima_encode(samples):
    pred, idx, out, nib = 0, 0, bytearray(), []
    decoded = []
    for s in samples:
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        # decode exactly like Adpcm.h
        d = step >> 3
        if code & 4: d += step
        if code & 2: d += step >> 1
        if code & 1: d += step >> 2
        pred = pred - d if code & 8 else pred + d
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code]))
        nib.append(code)
        decoded.append(pred)
    if len(nib) % 2:
        nib.append(0)
    for i in range(0, len(nib), 2):
        out.append(nib[i] | (nib[i + 1] << 4))
    return bytes(out), decoded


def main():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    lines = [
        "#pragma once",
        "// GENERATED by tools/make_voice.py from voice/*.wav - do not edit.",
        "// 16 kHz mono IMA ADPCM, low nibble first, decoder starts at 0 (see Adpcm.h).",
        "#include <stdint.h>",
        "",
        "struct BuiltinClip { const char* name; uint32_t samples; const uint8_t* data; };",
        "",
    ]
    entries, total = [], 0
    for name in CLIPS:
        path = os.path.join(VOICE_DIR, name + ".wav")
        if not os.path.exists(path):
            sys.exit("%s: missing (every announcement needs a built-in clip)" % path)
        try:
            x, rate = read_wav(path)
            y = trim_normalise(resample(x, rate, RATE))
        except Exception as e:  # noqa: BLE001 - report and stop
            sys.exit("%s: unusable (%s)" % (path, e))
        data, decoded = ima_encode(y)
        total += len(data)
        with wave.open(os.path.join(PREVIEW_DIR, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(struct.pack("<%dh" % len(decoded), *decoded))
        lines.append("static const uint8_t kClip_%s[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
        lines.append("};")
        entries.append('    {"%s", %d, kClip_%s},' % (name, len(y), name))
        print("%-20s %5.2f s  %6d bytes" % (name, len(y) / RATE, len(data)))
    lines += ["", "static const BuiltinClip kBuiltinClips[%d] = {" % len(CLIPS)] + entries + ["};", ""]
    with open(OUT_H, "w") as f:
        f.write("\n".join(lines))
    print("total %d bytes -> %s" % (total, os.path.relpath(OUT_H, ROOT)))
    print("preview WAVs  -> %s" % os.path.relpath(PREVIEW_DIR, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
