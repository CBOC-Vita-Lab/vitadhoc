#!/usr/bin/env python3
"""audio_measure.py -- objective checks on a WAV written by the host twin
(gbadhoc_host --wav).  Prints clipping count, int16 wrap jumps, peak, DC
offset, largest step, and exact-silence stretches.

  audio_measure.py file.wav [--json]

A "wrap jump" is a step between neighbouring samples of more than 40000 whose
two sides have opposite sign: what an int16 overflow produces.  A "click" is a
step above 12000 that is not part of a loud wave (both neighbours' steps are
small), counted separately."""
import array, json, struct, sys, wave


def measure(path):
    w = wave.open(path, "rb")
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    s = array.array("h")
    s.frombytes(raw)
    res = {"rate": rate, "frames": len(s) // ch}
    chans = []
    for c in range(ch):
        x = s[c::ch]
        chans.append(x)
    clip = wrap = click = 0
    peak = 0
    dc = 0.0
    maxstep = 0
    sq = 0.0
    for x in chans:
        if not len(x):
            continue
        dc += sum(x) / len(x)
        prev = 0
        pstep = 0
        for v in x:
            a = v if v >= 0 else -v
            if a > peak:
                peak = a
            if v >= 32767 or v <= -32768:
                clip += 1
            d = v - prev
            ad = d if d >= 0 else -d
            if ad > maxstep:
                maxstep = ad
            if ad > 40000 and (v ^ prev) < 0:
                wrap += 1
            elif ad > 12000 and pstep < 3000:
                click += 1
            pstep = ad
            prev = v
            sq += v * v
    res.update(peak=peak, clip_samples=clip, wrap_jumps=wrap, clicks=click,
               dc_offset=round(dc / ch, 2), max_step=maxstep,
               rms=round((sq / max(1, len(s))) ** 0.5, 1))
    # runs of digital silence of >= 480 frames after sound started
    z = run = 0
    started = False
    gaps = 0
    L = chans[0]
    R = chans[1] if ch > 1 else chans[0]
    for l, r in zip(L, R):
        if l == 0 and r == 0:
            run += 1
        else:
            if started and run >= 480:
                gaps += 1
            run = 0
            started = True
    res["silent_gaps_10ms"] = gaps
    return res


if __name__ == "__main__":
    r = measure(sys.argv[1])
    if "--json" in sys.argv:
        print(json.dumps(r))
    else:
        for k, v in r.items():
            print("%-18s %s" % (k, v))
