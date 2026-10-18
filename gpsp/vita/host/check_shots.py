#!/usr/bin/env python3
"""Pixel checks on the host test's game-picture screenshots.

    python3 check_shots.py <shots dir>

What must hold for the scaler, measured on the composed 960x544 frames:

  * every integer mode (2x, 3x, cropped 4x) is EXACT whatever the filter says:
    each source pixel is one k x k block, the picture is where the geometry
    says, and (except "LCD grid") nearest == bilinear == sharp, byte for byte;
  * the cropped 4x shows source rows 12..147 (GBA) / 4..139 (GB);
  * fit / stretch: the picture's outer pixels are the source's outer pixels
    (the 1-px white frame of the test pattern) -- nothing cropped, and no
    black bleeding in at the edge under bilinear or sharp;
  * sharp bilinear only blends ACROSS source-pixel edges: every output pixel
    lies between the min and max of the nearest result's 3x3 neighbourhood;
  * the LCD grid only darkens;
  * outside the picture the frame is black.
"""
import os
import sys

from PIL import Image

GEO = {   # mode -> (w, h, x, y) of the picture, native px; GBA then GB
    'gba': {'2x': (480, 320, 240, 112), '3x': (720, 480, 120, 32),
            'fit': (816, 544, 72, 0), '4xcrop': (960, 544, 0, 0),
            'stretch': (960, 544, 0, 0)},
    'gb': {'2x': (320, 288, 320, 128), '3x': (480, 432, 240, 56),
           'fit': (604, 544, 178, 0), '4xcrop': (640, 544, 160, 0),
           'stretch': (960, 544, 0, 0)},
}
INT_K = {'2x': 2, '3x': 3, '4xcrop': 4}
FILTERS = ['nearest', 'bilinear', 'sharp', 'lcd']
WHITE = (255, 255, 255)

fails = 0


def bad(msg):
    global fails
    fails += 1
    print('FAIL', msg)


def load(d, con, mode, filt):
    n = os.path.join(d, '5%d_pic_%s_%s_%s.png' % (0 if con == 'gba' else 1,
                                                  con, mode, filt))
    return Image.open(n).convert('RGB').load()


def main(d):
    for con, (sw, sh) in (('gba', (240, 160)), ('gb', (160, 144))):
        for mode, (w, h, ox, oy) in GEO[con].items():
            px = {f: load(d, con, mode, f) for f in FILTERS}
            near = px['nearest']
            # black outside
            for f in FILTERS:
                for (x, y) in ((ox - 1, oy + h // 2), (ox + w, oy + h // 2),
                               (ox + w // 2, oy - 1), (ox + w // 2, oy + h)):
                    if 0 <= x < 960 and 0 <= y < 544 and px[f][x, y] != (0, 0, 0):
                        bad('%s %s %s: not black outside at %d,%d' %
                            (con, mode, f, x, y))
            if mode in INT_K:
                k = INT_K[mode]
                blocks = True
                for y in range(oy, oy + h):
                    for x in range(ox, ox + w):
                        if near[x, y] != near[ox + (x - ox) // k * k,
                                              oy + (y - oy) // k * k]:
                            blocks = False
                if not blocks:
                    bad('%s %s: not exact %dx%d blocks' % (con, mode, k, k))
                for f in ('bilinear', 'sharp'):
                    same = all(px[f][x, y] == near[x, y]
                               for y in range(oy, oy + h, 1)
                               for x in range(ox, ox + w, 1))
                    if not same:
                        bad('%s %s %s differs from nearest' % (con, mode, f))
                if mode == '4xcrop':
                    # the white frame's top/bottom rows are cropped away, its
                    # left/right columns are not
                    if near[ox, oy + 10] != WHITE or near[ox + w - 1, oy + 10] != WHITE:
                        bad('%s 4xcrop: side frame missing' % con)
                    if near[ox + w // 2, oy] == WHITE:
                        bad('%s 4xcrop: top row not cropped' % con)
                else:
                    for (x, y) in ((ox + w // 2, oy), (ox + w // 2, oy + h - 1),
                                   (ox, oy + h // 2), (ox + w - 1, oy + h // 2)):
                        if near[x, y] != WHITE:
                            bad('%s %s: frame missing at %d,%d' % (con, mode, x, y))
            else:
                for f in ('nearest', 'bilinear', 'sharp'):
                    for (x, y) in ((ox + w // 2, oy), (ox + w // 2, oy + h - 1),
                                   (ox, oy + h // 2), (ox + w - 1, oy + h // 2)):
                        c = px[f][x, y]
                        if min(c) < 200:
                            bad('%s %s %s: edge dark at %d,%d %s' %
                                (con, mode, f, x, y, c))
                # sharp stays within the nearest neighbourhood
                sharp, worst = px['sharp'], 0
                for y in range(oy + 1, oy + h - 1):
                    for x in range(ox + 1, ox + w - 1):
                        s = sharp[x, y]
                        for ch in range(3):
                            vals = [near[x + dx, y + dy][ch]
                                    for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
                            over = max(min(vals) - s[ch], s[ch] - max(vals))
                            if over > worst:
                                worst = over
                if worst > 2:
                    bad('%s %s sharp: outside nearest neighbourhood by %d' %
                        (con, mode, worst))
            lcd, ref = px['lcd'], (near if mode in INT_K else px['sharp'])
            dark = 0
            for y in range(oy, oy + h):
                for x in range(ox, ox + w):
                    a, b = lcd[x, y], ref[x, y]
                    if a[0] > b[0] + 1 or a[1] > b[1] + 1 or a[2] > b[2] + 1:
                        bad('%s %s lcd brighter at %d,%d' % (con, mode, x, y))
                        break
                    if a != b:
                        dark += 1
                else:
                    continue
                break
            frac = dark / float(w * h)
            print('ok   %-3s %-7s %dx%d at %d,%d  lcd darkens %.0f%% of pixels'
                  % (con, mode, w, h, ox, oy, 100 * frac))
    # the pair layouts: exact 2x / 3x+1x
    p = Image.open(os.path.join(d, '53_pair_gba_side_pattern.png')).convert('RGB').load()
    for ox in (0, 480):
        if not all(p[x, y] == p[ox + (x - ox) // 2 * 2, 112 + (y - 112) // 2 * 2]
                   for y in range(112, 432) for x in range(ox, ox + 480)):
            bad('pair side: not exact 2x at x=%d' % ox)
        for (x, y) in ((ox, 200), (ox + 479, 200), (ox + 200, 112), (ox + 200, 431)):
            if p[x, y] != WHITE:
                bad('pair side: frame missing at %d,%d' % (x, y))
    print('pair side: two exact 2x pictures, 480x320 each')
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'out/shots_all'))
