#!/usr/bin/env python3
"""Generates the "beeps" voice pack: tone patterns instead of spoken words.

    python3 tools/make_beeps.py          -> voice-packs/beeps/*.wav

The patterns are the ones the firmware played itself before 1.3 (feedback = beeps).
To use them, copy the WAV files into the voice folder on the EXTENDER drive.
16 kHz (like the built-in voice), 16-bit, mono, 5 ms fades. The local board evens out the level of every
announcement, so the beeps play as loud as the voice did (about -13 dBFS).
Standard library only.
"""
import math
import os
import struct
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "voice-packs", "beeps")
RATE = 16000
LEVEL = 0.89  # -1 dBFS peak

# {Hz, ms} pairs; Hz 0 = pause.
SHORT = [(1500, 80)]
PATTERNS = {
    "link_up": [(660, 120), (0, 40), (880, 160)],          # rising two-tone
    "link_lost": [(880, 120), (0, 40), (660, 160)],        # falling two-tone
    "adapter_connected": [(1200, 60), (0, 60), (1200, 60)],
    "adapter_removed": [(440, 200)],
    "adapter_unsupported": [(220, 400)],                   # low buzz
    "filter_on": [(1000, 100)],
    "filter_strong": [(1000, 300)],
    "filter_off": [(1000, 100), (0, 100), (1000, 100)],
    "no_signal": [(330, 150), (0, 100), (330, 150), (0, 100), (330, 150)],
}
for n in range(1, 10):  # mic N: N short beeps
    p = []
    for i in range(n):
        if i:
            p.append((0, 120))
        p += SHORT
    PATTERNS["mic_%d" % n] = p


def render(pattern):
    out = []
    fade = RATE * 5 // 1000
    for hz, ms in pattern:
        total = RATE * ms // 1000
        for k in range(total):
            if not hz:
                out.append(0)
                continue
            env = min(1.0, k / fade, (total - k) / fade)
            out.append(int(32767 * LEVEL * env * math.sin(2 * math.pi * hz * k / RATE)))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in sorted(PATTERNS):
        samples = render(PATTERNS[name])
        with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(struct.pack("<%dh" % len(samples), *samples))
    print("%d files in %s" % (len(PATTERNS), OUT))


if __name__ == "__main__":
    main()
