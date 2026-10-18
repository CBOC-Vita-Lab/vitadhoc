#!/usr/bin/env python3
"""rewind_check.py HASHFILE [--ref REFFILE] [--min-redo N] [--max-step K]
[--new-timeline] [--allow-cut N] -- the rewind determinism verdict.

Reads the per-frame log a REWIND build writes with --rewind-hash (fe_host.c,
rw_after_frame / fe_host_rewind_tick):

    F frame state_crc video_crc audio_rev_crc audio_n input save_crc
                                                               a frame was RUN
                                                               (save_crc: the
                                                               cartridge save
                                                               in memory)
    R frame video_crc audio_crc audio_n                        a frame was SHOWN
                                                               backwards
    S frame                                                    the same step
                                                               back on a build
                                                               that keeps no
                                                               pictures yet
                                                               (the PSP)
    Q frame state_crc [save_crc]                               a frame was
                                                               RE-RUN for the
                                                               rewind
    E frame [save_crc]                                         play resumed
                                                               (save_crc: the
                                                               save in memory
                                                               as play goes on)
    W frame                                                    the history was
                                                               lost (a sleep);
                                                               play goes on
                                                               from here
    B frame                                                    a frame was run
                                                               as a BARRIER (a
                                                               cartridge save
                                                               write, ...)

and asserts the properties the feature stands on:

  1. ONE TIMELINE.  Every line continues from the one before: a frame shown
     backwards is exactly one before the frame that was on screen, a frame
     run is exactly one after it, and play resumes on the frame last shown.
     Nothing is skipped, repeated or shown out of order, whatever the
     pattern of presses was.  And no frame at or before a barrier is ever
     shown again.
  2. REVERSE.  Every frame shown during a rewind is, pixel for pixel, the
     frame the forward pass showed under that number, and its sound is that
     frame's sound sample-reversed.
  3. REPLAY.  Every frame run again after a rewind leaves the machine in the
     same state, bit for bit (CRC of the whole rewind image: the save-state
     image plus the sound in flight), with the same picture, sound and
     input as the reference.
  4. THE SAVE GOES BACK WITH THE GAME.  The cartridge save in memory is the
     reference's save of that frame: after every frame run (part of 3),
     after every frame re-run for the rewind, and at the moment play
     resumes.  The summary says how many frames wrote the save and how many
     of those writes rewinds went back over (a run that crossed none has
     not tested this).

The reference is the first time each frame number was run in HASHFILE
itself, or -- with --ref -- a separate run of the same game and inputs that
never rewound (then EVERY frame is compared, including the ones played for
the first time after a rewind).

--new-timeline: a frame run again with a DIFFERENT input than the first time
is not an error but the start of a new timeline (a run that rewinds and then
plays something else, tools/rewind_savetest.py): the reference from that
frame on is forgotten and built again.  Without --ref only.

--allow-cut N: up to N frames RE-RUN for the rewind may leave a machine that
differs from the first pass in its state while its SAVE is the same.  That
is the translator's frame cut (docs/REWIND.md 6 and 15.4): a restore that
finds the game's code in RAM changed has to throw the RAM translation cache
away, the blocks are rebuilt from other entry points, and the frame ends one
block earlier or later -- a few cycles, the same guest timeline.  A game
that copies its flash routines into RAM to save does this.  Counted and
printed as `cut_moved`; a re-run frame whose save differs always fails.

Exit 0 and "PASS" only if all hold and at least --min-redo frames were
actually compared (a test that compared nothing has not passed).
"""
import sys


def load_ref(path):
    ref = {}
    for line in open(path):
        t = line.split()
        if t and t[0] == "F":
            ref.setdefault(int(t[1]), tuple(t[2:8]))
    return ref


NOPIC = "--------"


def main():
    args = sys.argv[1:]
    min_redo = 1
    ref = None
    if "--min-redo" in args:
        i = args.index("--min-redo")
        min_redo = int(args[i + 1])
        del args[i:i + 2]
    max_step = 1                # frames one rewind tick may go back
    if "--max-step" in args:
        i = args.index("--max-step")
        max_step = int(args[i + 1])
        del args[i:i + 2]
    if "--ref" in args:
        i = args.index("--ref")
        ref = load_ref(args[i + 1])
        del args[i:i + 2]
    # --ref-after-landing FILE (docs/REWIND.md 20.2): frames played after a
    # landing are held to FILE, a run of the same game and inputs that never
    # rewound, instead of to this run's first pass.  For the case the first
    # pass cannot be the reference: the dynarec's translation cache was
    # dropped by a restore, and a never-rewound run whose cache is dropped
    # at that frame (the twin, --flush-ram-at) is the game the rewound run
    # must then be, picture and sound to the sample.
    ref_land = None
    if "--ref-after-landing" in args:
        i = args.index("--ref-after-landing")
        ref_land = load_ref(args[i + 1])
        del args[i:i + 2]
    land_ref_n = 0
    twin = set()
    new_timeline = "--new-timeline" in args
    if new_timeline:
        args.remove("--new-timeline")
    allow_cut = 0
    if "--allow-cut" in args:
        i = args.index("--allow-cut")
        allow_cut = int(args[i + 1])
        del args[i:i + 2]
    cut_moved = 0
    # --allow-timing N (docs/REWIND.md 16.3): a frame whose STATE differs
    # from the reference while its picture, sound, input and save are the
    # reference's is counted as timing_only instead of failing, up to N
    # (-1 = any number).  Likewise a re-run frame whose state differs and
    # whose save does not (rerun_timing_only; its picture and sound are
    # checked by the R line when it is shown).  This is the dynarec taking
    # an interrupt a few cycles earlier or later with other translated
    # blocks in its cache; anything a player can see or hear still fails.
    allow_timing = 0
    if "--allow-timing" in args:
        i = args.index("--allow-timing")
        allow_timing = int(args[i + 1])
        del args[i:i + 2]
    timing = timing_q = 0
    timelines = 0
    # --save-quiet N: the emulator's N (frames the save chip must have been
    # left alone before play may continue from a frame a rewind stops on).
    save_quiet = 60
    if "--save-quiet" in args:
        i = args.index("--save-quiet")
        save_quiet = int(args[i + 1])
        del args[i:i + 2]
    own = False                 # the reference no longer applies (see F)
    nopic_f = nopic_r = 0
    sav_newer = sav_split = 0
    last_f = None               # the last frame run as play: (frame, save)
    last_change = 0             # frames below this have a save change after them
    reads = {}                  # frame -> save bytes read so far, latest run
    busyf = {}                  # frame -> the chip was busy in it, latest run
    land_n = land_inside = land_busy = snapped = 0
    sav_touched = 0             # E/Q lines whose save is not the last F's
    path = args[0]
    external = ref is not None

    first = ref if external else {}
    seen = set()
    cmp_n = cmp_bad = 0         # frames compared against the reference
    redo = 0                    # frames run more than once
    rev = rev_bad = silent = 0
    prev_cur = None
    order_bad = 0
    presses = 0
    cur = None                  # frame on screen
    barrier = -1                # newest frame run as a barrier
    barriers = cross = 0
    steps = 0                   # steps back without a picture
    q_n = q_bad = 0             # frames re-run for the rewind, compared
    latest = {}                 # frame -> (state, save) CRC of its latest normal run
    sav_cross = 0               # save-writing frames rewinds went back over
    sav_cross_set = set()
    e_n = e_bad = 0             # resumes whose save was compared
    q_sav = 0
    in_rewind = False
    problems = []
    names = ("state", "video", "audio", "audio_n", "input", "save")

    def bad(msg):
        if len(problems) < 12:
            problems.append(msg)

    for ln, line in enumerate(open(path), 1):
        t = line.split()
        if not t:
            continue
        if t[0] == "F":
            f = int(t[1])
            rec = tuple(t[2:8])
            rd = int(t[8]) if len(t) > 8 else 0
            if cur is not None and f != cur + 1:
                order_bad += 1
                bad("line %d: ran frame %d after frame %d" % (ln, f, cur))
            if f in seen:
                redo += 1
            # THE SAVE IS PERMANENT: after a rewind the save in memory can be
            # NEWER than the reference has it at this frame.  That alone
            # changes nothing else, so everything but the save field is
            # still compared -- until the game READS its save (the count of
            # save bytes read moved in this frame): from there on this is
            # legitimately another game, and the run is its own reference
            # (frames re-run and pictures shown backwards are still held to
            # it).
            # (the save the frame STARTED with: the reference's after frame
            # f - 1 against the live one, which is what the last frame run
            # as play left, wherever the timeline has gone since)
            if ((f in first and len(first[f]) > 5 and len(rec) > 5 and
                 first[f][5] != rec[5]) or
                    (f - 1 in first and len(first[f - 1]) > 5 and
                     last_f is not None and last_f[1] is not None and
                     first[f - 1][5] != last_f[1])):
                sav_newer += 1
                if rd != reads.get(f - 1, rd):
                    sav_split += 1
                    own = True
                    for g in [g for g in first if g >= f]:
                        del first[g]
                    for g in [g for g in latest if g >= f]:
                        del latest[g]
            if last_f is not None and f - 1 < last_change:
                last_change = min(last_change, f - 1) if f - 1 >= 0 else 0
            if last_f is not None and len(rec) > 5 and rec[5] != last_f[1]:
                last_change = f
            last_f = (f, rec[5] if len(rec) > 5 else None)
            reads[f] = rd
            busyf[f] = len(t) > 9 and t[9] != "0"
            if (new_timeline and not external and f in first and
                    first[f][4] != rec[4]):
                # other buttons than the first time: from here on this is
                # another game
                timelines += 1
                for g in [g for g in first if g >= f]:
                    del first[g]
                for g in [g for g in latest if g >= f]:
                    del latest[g]
            if f in first:
                if external or f in seen or f in twin:
                    cmp_n += 1
                    fa, fb = list(first[f][:5]), list(rec[:5])
                    if NOPIC in (fa[1], fb[1]):
                        # a frame without a kept picture on a platform whose
                        # pictures do not come from the core: nothing to hash
                        nopic_f += 1
                        fa[1] = fb[1] = NOPIC
                    if fa != fb:
                        diff = [n for n, a, b in zip(names, fa, fb) if a != b]
                        if diff == ["state"] and (allow_timing < 0 or
                                                  timing < allow_timing):
                            # picture, sound, input and save are the
                            # reference's; the machine is not, bit for bit
                            timing += 1
                        else:
                            cmp_bad += 1
                            bad("line %d: frame %d differs from the reference: %s"
                                % (ln, f, ",".join(diff)))
            elif external and not own:
                cmp_bad += 1
                bad("line %d: frame %d is not in the reference run" % (ln, f))
            else:
                first[f] = rec
            seen.add(f)
            latest[f] = (rec[0], rec[5] if len(rec) > 5 else None)
            cur = f
            in_rewind = False
        elif t[0] == "S":
            f = int(t[1])
            if not in_rewind:
                presses += 1
                in_rewind = True
            if cur is not None and not (cur - max_step <= f <= cur - 1):
                order_bad += 1
                bad("line %d: stepped to frame %d after frame %d" % (ln, f, cur))
            if cur is not None:
                for g in range(f + 1, cur + 1):
                    if g in latest and g - 1 in latest and latest[g][1] != latest[g - 1][1]:
                        sav_cross += 1
                        sav_cross_set.add(g)
            cur = f
            steps += 1
            if f <= barrier + 1:
                cross += 1
                bad("line %d: stepped to frame %d, at or before the barrier at %d"
                    % (ln, f, barrier))
        elif t[0] == "R":
            f = int(t[1])
            if not in_rewind:
                presses += 1
                in_rewind = True
            if cur is not None and not (cur - max_step <= f <= cur - 1):
                order_bad += 1
                bad("line %d: showed frame %d after frame %d" % (ln, f, cur))
            prev_cur = cur
            if cur is not None:
                for g in range(f + 1, cur + 1):
                    if g in latest and g - 1 in latest and latest[g][1] != latest[g - 1][1]:
                        sav_cross += 1
                        sav_cross_set.add(g)
            cur = f
            rev += 1
            if f <= barrier + 1:
                cross += 1
                bad("line %d: showed frame %d, at or before the barrier at %d"
                    % (ln, f, barrier))
            if f not in first:
                rev_bad += 1
                bad("line %d: showed frame %d, which has no reference" % (ln, f))
            else:
                video, aud, aud_n = first[f][1:4]
                if video == NOPIC or (t[3] == "silent" and len(t) > 5 and
                                      int(t[5]) in first and first[int(t[5])][1] == NOPIC):
                    nopic_r += 1        # shown, but its forward frame had no hash
                elif t[3] == "silent":
                    # a rewind that keeps no sound: the picture only.  With
                    # a picture every few frames it may be the picture of a
                    # frame passed on the way to this one, never of another.
                    silent += 1
                    pf = int(t[5]) if len(t) > 5 else f
                    if pf != f and not (prev_cur is not None and f < pf < prev_cur):
                        rev_bad += 1
                        bad("line %d: frame %d shown with the picture of frame %d" % (ln, f, pf))
                    elif pf not in first or first[pf][1] != t[2]:
                        rev_bad += 1
                        bad("line %d: frame %d shown backwards differs in video" % (ln, f))
                elif (video, aud, aud_n) != (t[2], t[3], t[4]):
                    rev_bad += 1
                    what = "video" if video != t[2] else "audio"
                    bad("line %d: frame %d shown backwards differs in %s" % (ln, f, what))
        elif t[0] == "B":
            f = int(t[1])
            if cur is not None and f != cur + 1:
                order_bad += 1
                bad("line %d: barrier frame %d after frame %d" % (ln, f, cur))
            cur = f
            barrier = f
            barriers += 1
            in_rewind = False
        elif t[0] == "Q":
            # a frame RE-RUN for the rewind (never on screen as play): the
            # machine it leaves must be the one the first pass left
            # (the one this timeline's own latest run of that frame left: a
            # frame played again after a rewind is the timeline from then on)
            f = int(t[1])
            if len(t) > 3 and last_f is not None and last_f[1] is not None:
                # a re-run frame never changes the save
                q_sav += 1
                if t[3] != last_f[1]:
                    sav_touched += 1
                    bad("line %d: frame %d re-run for the rewind CHANGED THE "
                        "SAVE" % (ln, f))
            if f in latest:
                q_n += 1
                sav_same = True
                if latest[f][0] != t[2]:
                    if sav_same and (allow_timing < 0 or timing_q < allow_timing):
                        timing_q += 1
                    elif sav_same and cut_moved < allow_cut:
                        cut_moved += 1
                    else:
                        q_bad += 1
                        bad("line %d: frame %d re-run for the rewind left a "
                            "different state" % (ln, f))
        elif t[0] == "W":
            # The history was lost (a sleep): play goes on from the frame
            # the MACHINE was at, which need not be the one on screen, and
            # nothing at or before it can be shown again.
            cur = int(t[1])
            barrier = cur
            barriers += 1
            in_rewind = False
        elif t[0] == "E":
            to = int(t[1])
            if cur is not None and to != cur:
                # a release on a frame play may not continue from moves to
                # the nearest one it may; only then
                snapped += 1
                if not (last_f is not None and cur < last_change and
                        any(busyf.get(g) for g in range(max(0, cur - save_quiet + 1), cur + 1))):
                    order_bad += 1
                    bad("line %d: play resumed at %d but %d was on screen" % (ln, to, cur))
                cur = to
            if ref_land is not None:
                for g in [g for g in first if g > to]:
                    del first[g]
                for g, r in ref_land.items():
                    if g > to:
                        first[g] = r
                        twin.add(g)
                        land_ref_n += 1
            if len(t) > 3 and last_f is not None and last_f[1] is not None:
                # the save is what it was when the rewind began
                e_n += 1
                if t[3] != last_f[1]:
                    e_bad += 1
                    sav_touched += 1
                    bad("line %d: play resumed at frame %d with a save the "
                        "rewind CHANGED" % (ln, to))
            if len(t) > 2 and last_f is not None:
                land_n += 1
                if to < last_change:
                    # the save has changed since this frame: the chip must be
                    # idle and must have been left alone for save_quiet frames
                    if t[2] != "0":
                        land_busy += 1
                        bad("line %d: play resumed at frame %d with the save "
                            "chip in the middle of something" % (ln, to))
                    if any(busyf.get(g) for g in range(max(0, to - save_quiet + 1), to + 1)):
                        land_inside += 1
                        bad("line %d: play resumed at frame %d, inside a save "
                            "in progress" % (ln, to))
                if cur is not None and to != cur:
                    pass
            in_rewind = False

    ok = (cmp_bad == 0 and rev_bad == 0 and order_bad == 0 and cross == 0 and
          q_bad == 0 and e_bad == 0 and sav_touched == 0 and
          land_inside == 0 and land_busy == 0 and
          redo >= min_redo and rev + steps >= 1)
    print("rewind_check: frames=%d presses=%d shown_backwards=%d (bad %d) "
          "order_bad=%d barriers=%d (crossed %d) rerun=%d compared=%d (bad %d) "
          "reference=%s%s"
          % (len(seen), presses, rev, rev_bad, order_bad, barriers, cross, redo,
             cmp_n, cmp_bad, "clean run" if external else "first pass",
             " steps_back_without_picture=%d" % steps if steps else "")
          + " rerun_for_rewind=%d (bad %d)" % (q_n, q_bad)
          + (" cut_moved=%d" % cut_moved if allow_cut or cut_moved else "")
          + (" timing_only=%d rerun_timing_only=%d" % (timing, timing_q)
             if allow_timing else "")
          + (" new_timelines=%d" % timelines if new_timeline else ""))
    if ref_land is not None:
        print("rewind_check twin: frames after a landing held to the "
              "never-rewound twin instead of the first pass: %d offered"
              % land_ref_n)
    sav_frames = sum(1 for g in sorted(first) if g - 1 in first and
                     len(first[g]) > 5 and first[g][5] != first[g - 1][5])
    print("rewind_check save: frames_that_wrote_it=%d gone_back_over=%d "
          "(%d distinct) resumes_compared=%d (bad %d) rerun_saves_compared=%d "
          "changed_by_rewind=%d landings=%d moved_off_a_save=%d inside=%d "
          "chip_busy=%d frames_with_newer_save=%d own_timeline_from_save_read=%d"
          % (sav_frames, sav_cross, len(sav_cross_set), e_n, e_bad, q_sav,
             sav_touched, land_n, snapped, land_inside, land_busy, sav_newer,
             sav_split))
    if nopic_f or nopic_r:
        print("rewind_check pictures WITHOUT a hash (not checked): forward "
              "frames %d, shown backwards %d of %d" % (nopic_f, nopic_r, rev))
    for p in problems:
        print("  " + p)
    if redo < min_redo:
        print("  only %d frames were run again (need %d)" % (redo, min_redo))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
