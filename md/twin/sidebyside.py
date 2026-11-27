#!/usr/bin/env python3
# From the raw dumps of the sidebyside tool: the xBR 2x and the xBR 3x picture, each fitted to a 960x544 screen the way the player's Original
# shape does (bilinear texture draw into the display aspect rectangle), a side-by-side PNG (2x | 3x | difference x8), and per image numbers:
# the mean absolute difference and the 99.9th percentile over the picture's pixels (all three channels, 0..255).
# usage: sidebyside.py PREFIX [OUTPNG]
import sys, numpy as np
from PIL import Image
def rd(p, w, h):
    a = np.fromfile(p, dtype='<u2').reshape(h, w)
    r = ((a >> 11) & 31).astype(np.float32); g = ((a >> 5) & 63).astype(np.float32); b = (a & 31).astype(np.float32)
    return np.stack([r * 255 / 31, g * 255 / 63, b * 255 / 31], -1)
def bilinear(img, dw, dh):
    sh, sw = img.shape[:2]
    xs = np.clip((np.arange(dw) + 0.5) * sw / dw - 0.5, 0, sw - 1); ys = np.clip((np.arange(dh) + 0.5) * sh / dh - 0.5, 0, sh - 1)
    x0 = np.floor(xs).astype(int); y0 = np.floor(ys).astype(int); x1 = np.minimum(x0 + 1, sw - 1); y1 = np.minimum(y0 + 1, sh - 1)
    fx = (xs - x0)[None, :, None]; fy = (ys - y0)[:, None, None]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bot = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy
def main():
    pre = sys.argv[1]
    m = dict(l.strip().split('=', 1) for l in open(pre + '.meta'))
    w, h = int(m['w']), int(m['h'])
    dar = float(m['dar']) or w / h
    SW, SH = 960, 544
    dh = SH; dw = int(round(SH * dar))
    if dw > SW: dh = int(round(SW / dar)); dw = SW
    a = bilinear(rd(pre + '.x2', w * 2, h * 2), dw, dh)
    b = bilinear(rd(pre + '.x3', w * 3, h * 3), dw, dh)
    d = np.abs(a - b)
    mean = float(d.mean()); p999 = float(np.percentile(d, 99.9)); pmax = float(d.max())
    mx = d.max(-1); frac16 = float((mx > 16).mean() * 100)
    def canvas(x):
        c = np.zeros((SH, SW, 3), np.float32); ox = (SW - dw) // 2; oy = (SH - dh) // 2
        c[oy:oy + dh, ox:ox + dw] = x; return c
    sheet = np.concatenate([canvas(a), canvas(b), canvas(np.clip(d * 8, 0, 255))], 1)
    out = sys.argv[2] if len(sys.argv) > 2 else pre + '.png'
    Image.fromarray(np.clip(sheet + 0.5, 0, 255).astype(np.uint8)).save(out)
    print("SBS %s %dx%d (native %sx%s) fit %dx%d mean_abs_diff=%.3f p99.9=%.1f max=%.0f px_over_16=%.3f%%" % (pre.split('/')[-1], w, h, m['native_w'], m['native_h'], dw, dh, mean, p999, pmax, frac16))
main()
