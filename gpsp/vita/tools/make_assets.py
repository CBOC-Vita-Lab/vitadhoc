#!/usr/bin/env python3
"""make_assets.py -- the VPK's LiveArea pictures, from the repo's logo.

    python3 tools/make_assets.py [--logo ../../assets/gbadhoc-logo-3-dark@2x.png]
                                 [--out sce_sys]

Writes
    sce_sys/icon0.png                         128 x 128   the bubble
    sce_sys/livearea/contents/bg.png          840 x 500   LiveArea background
    sce_sys/livearea/contents/startup.png     280 x 158   the "gate" picture

Every one is an 8-bit INDEXED PNG.  That is not a style choice: the Vita's
package installer rejects true-colour LiveArea images ("the bubble fails to
install"), so each picture is quantised to a palette and the result is
checked before it is written -- size, mode and file size -- and the script
fails loudly rather than produce a VPK that will not install.

The source is the dark-background wordmark (light letters, the rainbow link
arc over the A).  Needs Pillow.
"""
import argparse
import os
import sys

from PIL import Image

BG = (18, 18, 26)            # the app's dark background
# (path under --out, width, height, largest file the installer is known to
# take: Sony's documented limits for these three are far above anything a
# flat logo produces, so the cap here is a sanity check, not a tuning knob)
TARGETS = (
    ("icon0.png", 128, 128, 420 * 1024),
    ("livearea/contents/bg.png", 840, 500, 1024 * 1024),
    ("livearea/contents/startup.png", 280, 158, 512 * 1024),
)


def trim(im):
    """Crop to the non-transparent part."""
    return im.crop(im.getchannel("A").getbbox())


def rainbow_of(logo):
    """The link arc alone: the saturated pixels in the top half."""
    w, h = logo.size
    px = logo.load()
    xs, ys = [], []
    for y in range(h // 2):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a > 128 and max(r, g, b) - min(r, g, b) > 60:
                xs.append(x)
                ys.append(y)
    if not xs:
        raise SystemExit("no rainbow arc found in the logo's top half")
    pad = 4
    return logo.crop((min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad))


def fit(im, w, h):
    """Scale to fit inside w x h, keeping the aspect."""
    s = min(w / im.width, h / im.height)
    return im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))),
                     Image.LANCZOS)


def canvas(w, h):
    return Image.new("RGBA", (w, h), BG + (255,))


def paste_centre(dst, im, cx, cy):
    dst.alpha_composite(im, (round(cx - im.width / 2), round(cy - im.height / 2)))


def make_icon(logo):
    # A 2.75:1 wordmark at 128 pixels is too small to be the whole bubble, so
    # the arc -- the part that says "link" -- is drawn large above it.
    im = canvas(128, 128)
    paste_centre(im, fit(rainbow_of(logo), 84, 50), 64, 44)
    paste_centre(im, fit(trim(logo), 116, 44), 64, 92)
    return im


def make_bg(logo):
    im = canvas(840, 500)
    paste_centre(im, fit(trim(logo), 620, 300), 420, 230)
    return im


def make_startup(logo):
    im = canvas(280, 158)
    paste_centre(im, fit(trim(logo), 236, 110), 140, 79)
    return im


def indexed(im):
    """RGBA on an opaque background -> 8-bit palette, no dithering (flat art)."""
    return im.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT,
                                      dither=Image.NONE)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--logo", default=os.path.join(
        here, "..", "..", "..", "assets", "gbadhoc-logo-3-dark@2x.png"))
    ap.add_argument("--out", default=os.path.join(here, "..", "sce_sys"))
    args = ap.parse_args()

    logo = Image.open(args.logo).convert("RGBA")
    made = {"icon0.png": make_icon(logo),
            "livearea/contents/bg.png": make_bg(logo),
            "livearea/contents/startup.png": make_startup(logo)}
    bad = 0
    for rel, w, h, cap in TARGETS:
        path = os.path.join(args.out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        indexed(made[rel]).save(path, optimize=True)
        chk = Image.open(path)
        size = os.path.getsize(path)
        ok = chk.size == (w, h) and chk.mode == "P" and size <= cap
        print(f"{rel:32} {chk.size[0]}x{chk.size[1]} mode={chk.mode} "
              f"{size} bytes {'ok' if ok else 'REJECTED'}")
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
