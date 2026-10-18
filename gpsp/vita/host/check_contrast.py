#!/usr/bin/env python3
"""Contrast of the menus' text over the worst backdrop.

    python3 check_contrast.py <shots dir>

ui_host_test's "contrast" group draws the list pages over a white frame
(dark theme) and over a black one (light theme): the picture hardest on each
theme's text.  For every such screenshot this finds the page's background
(the commonest colour between the header and the footer) and each of the
theme's text colours that the page actually used (exact pixels: text is
drawn in flat colours), and computes the WCAG 2 contrast ratio of each
against the background.  Body text must reach 4.5:1.

The colours are the ones psp/ui_psp.c's theme tables decode to on the Vita
(RGB565 expanded as the renderer expands it); a colour not found on a page is
simply not used there.
"""
import os
import sys
from collections import Counter

from PIL import Image


def c565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


# role -> RGB565 (psp/ui_psp.c THM_DARK / THM_LIGHT, -DVITA)
THEMES = {
    "dark": {"selected": 0xFFFF, "item": 0xCE59, "value": 0xEF5D,
             "value at rest": 0xAD55, "dim": 0x9CD3},
    "light": {"selected": 0x0000, "item": 0x2945, "value": 0x1082,
              "value at rest": 0x4A49, "dim": 0x528A},
}
NEED = 4.5


def lum(rgb):
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(c) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


def main(d):
    fails = 0
    names = sorted(n for n in os.listdir(d) if n.startswith("90_contrast_"))
    if not names:
        print("FAIL no 90_contrast_*.png in %s" % d)
        return 1
    for n in names:
        theme = "light" if n.endswith("_light.png") else "dark"
        im = Image.open(os.path.join(d, n)).convert("RGB")
        # between the header (the wordmark's line) and the bottom ramp
        # (Save states: the left of the page, its preview is on the right)
        body = im.crop((0, 130, 440 if "_states_" in n else 960, 300))
        count = Counter(body.get_flattened_data()
                        if hasattr(body, "get_flattened_data")
                        else body.getdata())
        bg = count.most_common(1)[0][0]
        row = []
        worst = 99.0
        for role, v in THEMES[theme].items():
            rgb = c565(v)
            px = sum(count[c] for c in count
                     if max(abs(c[i] - rgb[i]) for i in range(3)) <= 1)
            if px < 40:
                continue
            r = ratio(rgb, bg)
            # the cursor row's own band is lighter; its text is the
            # "selected" colour, measured against the band below
            if role != "selected":
                worst = min(worst, r)
            row.append("%s %.1f" % (role, r))
        # the selected row: the band is the accent at alpha 40 over bg
        acc = (255, 255, 255) if theme == "dark" else (0, 0, 0)
        band = tuple((acc[i] * 40 + bg[i] * 215) // 255 for i in range(3))
        rs = ratio(c565(THEMES[theme]["selected"]), band)
        worst = min(worst, rs)
        ok = worst >= NEED
        fails += not ok
        print("%s %-34s background #%02x%02x%02x: %s; on the cursor band "
              "%.1f" % ("ok  " if ok else "FAIL", n[:-4], bg[0], bg[1],
                        bg[2], ", ".join(row), rs))
    print("contrast: %d page(s) below %.1f:1" % (fails, NEED) if fails else
          "contrast: every page's text is %.1f:1 or better on the worst "
          "backdrop" % NEED)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "out/shots_all"))
