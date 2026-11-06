#!/usr/bin/env python3
"""audio_phases.py -- per-phase tone analysis of the host twin's WAV for the
audio test cartridge (hostcore/testrom/audio.c).  For each two-second phase:
peak, clipped samples, the fundamental's amplitude (frequency found by a fine
scan, so the follow's pitch trim does not matter), THD+N (everything but the
fundamental, relative to it) and the loudest component above 16 kHz-fs/2
images (energy above 20 kHz is ignored; the measure is 0..20 kHz).

  audio_phases.py file.wav [--json]
"""
import json, sys, wave
import numpy as np

F0 = {1: 1024, 2: 1024, 3: 440, 4: 3000, 5: 1024, 6: 1024, 7: 440, 8: 1024}
NAME = {1: "DS A 100% sine", 2: "DS A 50% sine", 3: "square 440 Hz",
        4: "square 3 kHz", 5: "DS A+B 100% (clips)", 6: "DS A 100% 32k feed",
        7: "DS + square + noise", 8: "DS A 100% sine again"}


def load(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    s = np.frombuffer(w.readframes(n), dtype=np.int16).reshape(-1, w.getnchannels())
    return s[:, 0].astype(float), w.getframerate()


def fit(seg, fs, f0):
    t = np.arange(len(seg)) / fs
    best = (0, f0)
    for f in np.arange(f0 * 0.99, f0 * 1.01, f0 * 0.0002):
        v = abs((seg * np.exp(-2j * np.pi * f * t)).mean())
        if v > best[0]:
            best = (v, f)
    f = best[1]
    for df in np.arange(-f0 * 0.0002, f0 * 0.0002, f0 * 0.00001):
        v = abs((seg * np.exp(-2j * np.pi * (f + df) * t)).mean())
        if v > best[0]:
            best = (v, f + df)
    f = best[1]
    c = (seg * np.exp(-2j * np.pi * f * t)).mean()
    fund = 2 * np.real(c * np.exp(2j * np.pi * f * t))
    return f, 2 * abs(c), seg - fund


def analyse(path):
    s, fs = load(path)
    out = {}
    for p in range(1, 9):
        a = int((1.0 + 2.0 * (p - 1) + 0.6) * fs)
        seg = s[a:a + int(1.2 * fs)]
        if len(seg) < fs:
            continue
        f, amp, res = fit(seg, fs, F0[p])
        sig = amp / np.sqrt(2)
        out[p] = dict(name=NAME[p], peak=int(np.abs(seg).max()),
                      clipped=int((np.abs(seg) >= 32767).sum()),
                      freq=round(f, 2), fund=round(amp),
                      thdn_pct=round(100 * np.sqrt((res ** 2).mean()) / sig, 2))
    return out


if __name__ == "__main__":
    r = analyse(sys.argv[1])
    if "--json" in sys.argv:
        print(json.dumps(r))
    else:
        for p, d in r.items():
            print("%d %-22s peak %6d clipped %6d f %8.2f fund %6d THD+N %7.2f%%" %
                  (p, d["name"], d["peak"], d["clipped"], d["freq"], d["fund"], d["thdn_pct"]))
