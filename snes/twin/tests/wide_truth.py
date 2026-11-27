#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Truth check for the synthetic scroller ROMs: every pixel the wide view claims to be real (wide_twin --dump) is compared with
the picture the level really has there. Prints wrong / checked / unchecked pixels and the real-pixel lead per side.

  wide_truth.py ROM.json DUMPDIR [prefix]
With the debug provenance (wide_twin --wopt debug=1) the claimed pixels are those the adapter marks as real level / exact
composite (prov 4 and 10); without it, the real range per line.
"""
import glob, json, os, struct, sys
import numpy as np

def c565(c):
    r, g, b = c
    return (r << 11) | (g << 6) | ((g & 0x10) << 1) | b

def tile_px(t):
    return np.array(t, dtype=np.int32).reshape(8, 8)

class World:
    def __init__(self, m):
        self.m = m
        self.cg = [c565(c) for c in m["cg"]]
        self.t1 = [tile_px(t) for t in m["t1"]]
        self.t2 = [tile_px(t) for t in m["t2"]]
        self.t3 = [tile_px(t) for t in m["t3"]]
        self.level = m["level"]; self.sky = m["sky"]; self.hud = m["hud"]
        self.lv = [(L["level"], [tile_px(t) for t in L["t1"]]) for L in m["levels"]] if "levels" in m else None
        self.sprites = [tuple(s) for s in m["sprites"]]
        self.M = 0

    def set_level(self, i):
        if self.lv: self.level, self.t1 = self.lv[i if i < len(self.lv) else 0]

    def mathpix(self, c565v):
        r, g, b = (c565v >> 11) & 31, (c565v >> 6) & 31, c565v & 31
        return c565((min(31, r + 12), min(31, g + 6), min(31, b + 3)))

    def pixel_win(self, cam, x, y):
        w = self.m["win"]
        if w.endswith("_a"): hidden = x <= 127 or x >= 200; math = x >= 128
        else: hidden = 8 <= x <= 247; math = True
        X = cam + x
        c = X >> 3
        if X < 0 or c >= 256: return None, None
        col = self.cg[0]
        if not hidden:
            t = self.level[c][y >> 3]
            p = self.t1[t][y & 7][X & 7] if t else 0
            if p: col = self.cg[32 + p]
        return (self.mathpix(col) if math else col), None

    def pixel(self, cam, x, y, hud_wrap=True):
        if self.m.get("win"): return self.pixel_win(cam, x, y)
        """colour at screen x (may be negative / >255), line y; None when the level has no truth there. Returns (colour, alt)"""
        for (sx, sy) in self.sprites:
            if sx <= x < sx + 8 and sy <= y < sy + 8: return self.cg[128 + 15], None
        if y < 24:
            hc = x >> 3
            if 0 <= x < 256:
                hv = self.hud[y >> 3][hc]
                p = self.t3[hv][y & 7][x & 7] if hv else 0
                if p: return self.cg[28 + p], None
            else:
                # the hud map is 32 wide: hardware repeats it; the world alone is also acceptable (unknown)
                hv = self.hud[y >> 3][hc & 31]
                p = self.t3[hv][y & 7][x & 7] if hv else 0
                alt = self.cg[28 + p] if p else None
                w = self.world(cam, x, y)
                return w, alt if alt is not None else None
        return self.world(cam, x, y), None

    def world(self, cam, x, y):
        X = cam + x
        c = X >> 3
        if X < 0 or c >= 256: return None
        t = self.level[c][y >> 3]
        p = self.t1[t][y & 7][X & 7] if t else 0
        if p: return self.cg[32 + p]
        X2 = (cam >> 1) + x
        s = self.sky[y >> 3][(X2 >> 3) & 31]
        return self.cg[48 + self.t2[s][y & 7][X2 & 7]]

class World7:
    """Mode 7 with the matrix per line (hardware / the core's formula), tilemap 128x128, 8bpp characters; screen x may be outside 0..255"""
    def __init__(self, m):
        self.m = m
        self.cg = [c565(c) for c in m["cg"]]
        self.tmap = m["tmap"]; self.chars = m["chars"]; self.mats = m["mats"]
        k = m["m7"]; self.H, self.V, self.CX, self.CY = k["H"], k["V"], k["CX"], k["CY"]

    @staticmethod
    def clip10(n):
        return (n | ~0x3ff) if (n & 0x2000) else (n & 0x3ff)

    def pixel(self, cam, x, y):
        a, b, c, d = self.mats[y]
        xx = self.clip10(self.H - self.CX); yy = self.clip10(self.V - self.CY)
        BB = ((b * (y + 1)) & ~63) + ((b * yy) & ~63) + (self.CX << 8)
        DD = ((d * (y + 1)) & ~63) + ((d * yy) & ~63) + (self.CY << 8)
        AA = a * x + ((a * xx) & ~63)
        CC = c * x + ((c * xx) & ~63)
        X = ((AA + BB) >> 8) & 0x3ff; Y = ((CC + DD) >> 8) & 0x3ff
        t = self.tmap[Y >> 3][X >> 3]
        p = self.chars[t][(Y & 7) * 8 + (X & 7)]
        return self.cg[p], None


def load(path):
    b = open(path, "rb").read()
    w, h, M, serial, mode7, layers, hud, frame = struct.unpack_from("<8i", b, 0)
    o = 32
    real = np.frombuffer(b, np.uint8, h * 2, o).reshape(h, 2); o += h * 2
    px = np.frombuffer(b, np.uint16, w * h, o).reshape(h, w); o += w * h * 2
    wram = b[o:o + 0x2000]; o += 0x2000
    has = struct.unpack_from("<I", b, o)[0]; o += 4
    prov = np.frombuffer(b, np.uint8, w * h, o).reshape(h, w) if has else None
    return dict(w=w, h=h, M=M, real=real, px=px, wram=wram, frame=frame, prov=prov)

def main():
    meta = json.load(open(sys.argv[1])); World_ = World7(meta) if meta.get('kind') == 'm7' else World(meta)
    files = sorted(glob.glob(os.path.join(sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "") + "*.wide")))
    tot_wrong = tot_checked = tot_unchecked = tot_hudalt = 0
    nunm = 0; nblank = 0
    lead_l = []; lead_r = []; avail_l = []
    worst = []
    for fp in files:
        d = load(fp)
        w, h, M = d["w"], d["h"], d["M"]
        counter = struct.unpack_from("<H", d["wram"], 2)[0]
        cands = [(0, 0)] if meta.get("kind") == "m7" else []
        for k in ([] if meta.get('kind') == 'm7' else range(counter - 4, counter + 2)):
            if k < 0: continue
            w16 = struct.unpack_from("<H", d["wram"], 0x1000 + 2 * (k & 0x3FF))[0]
            cands.append((w16 & 0xFFF, w16 >> 12))
        # find the camera (and level) whose truth equals the centre exactly
        best = None
        for cam, lvl in dict.fromkeys(cands):
            World_.set_level(lvl) if hasattr(World_, "set_level") else None
            bad = 0
            for y in range(0, min(h, 224), 7):
                for x in range(0, 256, 5):
                    t, alt = World_.pixel(cam, x, y)
                    if t is not None and t != d["px"][y, M + x]: bad += 1
            if bad == 0: best = cam; break
        if best is None:
            if not d["px"][:, M:M + 256].any(): nblank += 1      # a forced blank frame: nothing to check
            else: nunm += 1
            continue
        World_.set_level(lvl) if hasattr(World_, "set_level") else None
        wrong = checked = unchecked = hudalt = 0
        for y in range(min(h, 224)):
            L, R = int(d["real"][y, 0]), int(d["real"][y, 1])
            lead_l.append(L); lead_r.append(R)
            if d["prov"] is not None and meta.get("kind") != "m7":
                pr = d["prov"][y] & 0xF
                xs = [x for x in list(range(-M, 0)) + list(range(256, 256 + M)) if pr[M + x] in (4, 10)]
                lead_l[-1] = sum(1 for x in xs if x < 0); lead_r[-1] = sum(1 for x in xs if x >= 256)
            else:
                xs = list(range(-L, 0)) + list(range(256, 256 + R))
            for x in xs:
                t, alt = World_.pixel(best, x, y)
                got = int(d["px"][y, M + x])
                if t is None: unchecked += 1; continue
                checked += 1
                if got != t:
                    if alt is not None and got == alt: hudalt += 1; continue
                    wrong += 1
                    if len(worst) < 5: worst.append((os.path.basename(fp), y, x, got, t))
        tot_wrong += wrong; tot_checked += checked; tot_unchecked += unchecked; tot_hudalt += hudalt
    n = len(files)
    n -= nblank
    print("frames %d (centre unmatched %d): wrong real px %d, checked real px %d, real px outside the level %d, hud-line ambiguous %d" %
          (n, nunm, tot_wrong, tot_checked, tot_unchecked, tot_hudalt))
    if lead_l:
        print("mean real px per line: left %.1f right %.1f" % (np.mean(lead_l), np.mean(lead_r)))
    for wv in worst: print("  wrong:", wv)
    sys.exit(1 if tot_wrong or nunm == n else 0)

main()
