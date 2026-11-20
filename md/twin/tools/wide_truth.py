#!/usr/bin/env python3
# Truth check of the Master System wide view on the test cartridge of sms_scroll_gen.py: every margin pixel the wide view shows at full
# weight as real picture (widekit provenance "level") must be the pixel the cartridge's world has at that place.
# usage: wide_truth.py MD_TWIN ROM OUTDIR [--frames N] [--margin M] [--noblank] [--gg] [extra md_twin args]
# Prints: TRUTH frames=.. real_px=.. checked=.. wrong=.. faded=.. unlocated=..   and exits 1 when wrong > 0 or nothing was checked.
import sys, os, struct, subprocess, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sms_scroll_gen as G

def world():
    T = G.tiles()
    W = [[0] * 2048 for _ in range(192)]
    for y in range(192):
        row = y >> 3; fy = y & 7; Wy = W[y]
        for wc in range(256):
            t = G.tile_of(wc, row); a = G.attr_of(wc, row)
            b = T[t * 32 + fy * 4: t * 32 + fy * 4 + 4]
            pb = ((a >> 3) & 1) << 4
            for x in range(8):
                bit = x if (a & 2) else 7 - x
                Wy[wc * 8 + x] = pb | ((b[0] >> bit) & 1) | (((b[1] >> bit) & 1) << 1) | (((b[2] >> bit) & 1) << 2) | (((b[3] >> bit) & 1) << 3)
    return W

def load(fn):
    d = open(fn, 'rb').read()
    ow, oh, M, f = struct.unpack_from('4i', d, 0); o = 16
    real = [struct.unpack_from('2i', d, o + 8 * y) for y in range(oh)]; o += 8 * oh
    pix = struct.unpack_from('%dH' % (ow * oh), d, o); o += 2 * ow * oh
    prov = d[o:o + ow * oh]
    return ow, oh, M, f, real, pix, prov

def main():
    twin, rom, out = sys.argv[1:4]
    rest = sys.argv[4:]
    frames, margin, blank, extra, gg = 3200, 41, True, [], False
    i = 0
    while i < len(rest):
        if rest[i] == '--frames': frames = int(rest[i + 1]); i += 2
        elif rest[i] == '--margin': margin = int(rest[i + 1]); i += 2
        elif rest[i] == '--noblank': blank = False; i += 1
        elif rest[i] == '--gg': gg = True; margin = 48; i += 1
        else: extra.append(rest[i]); i += 1
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, 'tr_*.wd')): os.remove(f)
    subprocess.check_call([twin, rom, '--frames', str(frames), '--wide', str(margin), '--wide-dump', os.path.join(out, 'tr') + ':40:23', '--quiet'] + extra, stdout=subprocess.DEVNULL)
    W = world()
    VW, X0, Y0 = (160, 48, 24) if gg else (256, 0, 0)
    nframes = real_px = checked = wrong = faded = unloc = bar_ok = bar_plain = bar_wrong = 0
    for fn in sorted(glob.glob(os.path.join(out, 'tr_*.wd'))):
        ow, oh, M, f, real, pix, prov = load(fn)
        # where is the camera: the vertical blank count is the frame number give or take the start-up; the centre must fit exactly
        found = None
        for n in range(max(0, f - 12), f + 3):
            cam = G.cam_at(n)
            x0 = 0 if gg else max(8 if blank else 0, (-cam) & 7)
            mp = {}; ok = True
            for y in (0, 37, 77, 111, 143):
                row = W[y + Y0]; base = y * ow + M
                for x in range(x0, VW):
                    idx = row[(cam + X0 + x) & 2047]; p = pix[base + x]
                    if mp.setdefault(idx, p) != p: ok = False; break
                if not ok: break
            if ok and len(mp) >= 16:
                # the whole centre
                for y in range(oh):
                    row = W[y + Y0]; base = y * ow + M
                    for x in range(x0, VW):
                        idx = row[(cam + X0 + x) & 2047]; p = pix[base + x]
                        if mp.setdefault(idx, p) != p: ok = False; break
                    if not ok: break
            if ok and len(mp) >= 16: found = (cam, mp); break
        if not found: unloc += 1; continue
        cam, mp = found
        nframes += 1
        if blank and not gg:
            # the masked first eight columns: either the border colour (one colour down the line) or, where the left strip is real, the world
            for y in range(oh):
                if real[y][0] <= 0: continue
                row = W[y]; base = y * ow + M
                px = [pix[base + x] for x in range(8)]
                tr = [mp.get(row[(cam + x) & 2047]) for x in range(8)]
                if px == tr: bar_ok += 1
                elif len(set(px)) == 1: bar_plain += 1
                else: bar_wrong += 1
        for y in range(oh):
            row = W[y + Y0]; base = y * ow
            real_px += real[y][0] + real[y][1]
            for x in list(range(0, M)) + list(range(M + VW, ow)):
                pv = prov[base + x] & 15
                if pv == 5: faded += 1
                if pv != 4: continue
                wx = cam + X0 + (x - M)
                idx = row[wx & 2047]
                checked += 1
                if idx not in mp or mp[idx] != pix[base + x]: wrong += 1
    print('TRUTH %s frames=%d real_px=%d checked=%d wrong=%d faded=%d unlocated=%d masked_column_lines: world=%d border=%d wrong=%d' % (os.path.basename(rom), nframes, real_px, checked, wrong, faded, unloc, bar_ok, bar_plain, bar_wrong))
    return 1 if (wrong or bar_wrong or not checked) else 0

if __name__ == '__main__':
    sys.exit(main())
