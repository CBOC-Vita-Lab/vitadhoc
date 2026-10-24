#!/usr/bin/env python3
"""audio_follow_sim.py -- the GB link session's audio ring and rate follower
(psp/main_psp.c gblink_audio_frame), simulated: how the gains and the
cushion behave under CPU-bound sessions and stalls (docs/GB-LINK.md 14.4).

The model: the loop runs at the PSP's 59.94 Hz vblank; the machines emulate
frames at `fps` (a CPU-bound session runs slower), each frame adds 548.6
samples (32768 Hz / 59.7275) unless it was lost to a stall (`stall` = share
of frames); the controller runs once per loop on a 16-frame moving average
of the ring level; the audio thread takes 1024 output frames at 44100 Hz
every 23.2 ms at the step it sets.  A pull that finds the ring short is a
gap (the PSP fades it out and back in since claude/gblink-perf).

    python tools/gblink/audio_follow_sim.py
"""
import random
import statistics

SCENARIOS = [(59.94, 0.0), (59.94, 0.005), (59.94, 0.01), (59.94, 0.03),
             (58.0, 0.0), (58.0, 0.01), (57.0, 0.0), (56.0, 0.0)]


def sim(kp_div, ki_div, fps, stall, cushion, follow=30, secs=180, seed=1):
    """kp_div / ki_div as in main_psp.c (ppm = I + err/kp_div, I += err/ki_div
    per loop, err in ppm of the cushion); 0 = term off.  Returns gaps a
    minute, silence %, pitch standard deviation (ppm, last 2/3)."""
    random.seed(seed)
    step = 32768 / 44100.0
    level, ema, integ = cushion, cushion * 16.0, 0.0
    t, tf, pull = 0.0, 0.0, 0.0
    lo, hi = -follow * 1000, 5000
    gaps, starved, dry, ppms = 0, 0.0, False, []
    for _ in range(int(secs * 59.94)):
        t += 1 / 59.94
        while tf + 1 / fps <= t:
            tf += 1 / fps
            if random.random() >= stall:
                level += 548.6
        ema += level - ema / 16
        err = min((ema / 16 - cushion) * 1e6 / cushion, 1e6)
        if ki_div:
            integ = max(lo, min(hi, integ + err / ki_div))
        ppm = max(lo, min(hi, integ + (err / kp_div if kp_div else 0)))
        ppms.append(ppm)
        while pull <= t:
            pull += 1024 / 44100.0
            need = 1024 * step * (1 + ppm / 1e6)
            if level < need:
                gaps += 0 if dry else 1
                dry = True
                starved += (need - level) / step
                level = 0
            else:
                dry = False
                level -= need
    tail = ppms[len(ppms) // 3:]
    return gaps * 60.0 / secs, 100 * starved / (secs * 44100), statistics.pstdev(tail)


def main():
    print('scenarios (fps, share of frames lost to stalls): %s' % SCENARIOS)
    for name, cushion, kp_div, ki_div, follow in (
            ('no follow', 2048, 0, 0, 30),
            ('P only, 3 % full scale', 2048, 33, 0, 30),
            ('P 1 % + I 1 %/s, 3 % bound', 2048, 100, 3000, 30),
            ('P 2 % + I 1 % in 6 s, 3 % bound', 3072, 50, 18000, 30),
            ('P 2 % + I 1 % in 6 s, 6 % (shipped)', 3072, 50, 18000, 60)):
        rows = [sim(kp_div, ki_div, fps, st, cushion, follow=follow)
                for fps, st in SCENARIOS]
        print('%-36s cushion %4d  gaps/min %s | silence %% %s | pitch sd ppm %s' % (
            name, cushion, ' '.join('%4.0f' % r[0] for r in rows),
            ' '.join('%4.2f' % r[1] for r in rows),
            ' '.join('%5.0f' % r[2] for r in rows)))


if __name__ == '__main__':
    main()
