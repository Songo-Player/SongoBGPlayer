#!/usr/bin/env python3
# Writes the test tones the Godot tests play: steady sines, so the spectrum
# has one obvious peak to check.
#
#   make_tones.py <output dir>

import math
import os
import struct
import sys
import wave


def tone(path, freq, seconds, rate=44100, amplitude=0.5):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(seconds * rate)):
            v = int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
tone(os.path.join(out, "tone_1000.wav"), 1000, 3)
tone(os.path.join(out, "tone_440.wav"), 440, 3)
tone(os.path.join(out, "tone_5000_48k.wav"), 5000, 3, rate=48000)
