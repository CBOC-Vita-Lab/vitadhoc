#!/usr/bin/env python3
"""make_livearea.py -- Retro Adhoc's bubble and LiveArea pictures.

    python3 tools/make_livearea.py            (from gpsp/vita)

Writes sce_sys/icon0.png (128 x 128, the bubble), and
sce_sys/livearea/contents/bg.png (840 x 500) and startup.png (280 x 158, the
gate).  The system wants all three as 8-bit indexed PNG: they are drawn in
full colour and reduced to 256 colours at the end, dithered.

The pictures use the app's own palette (ui_vita_style.h): the navy page,
one glow per console (indigo, pea green, teal) and the mark -- two
handhelds, linked -- from vstyle_art.py.  The wordmark is Outfit Bold.
"""
import io
import os
import sys

try:
    import cairosvg
except ImportError:          # rsvg-convert does the same job
    cairosvg = None
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vstyle_art as A  # noqa: E402

VITA = os.path.dirname(HERE)
FONTS = os.path.join(VITA, "..", "assets", "fonts")
NAVY = "#070B18"


def svg_png(svg, w, h):
    if cairosvg is not None:
        data = cairosvg.svg2png(bytestring=svg.encode(), output_width=w,
                                output_height=h)
    else:
        import subprocess
        data = subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h)],
                              input=svg.encode(), capture_output=True,
                              check=True).stdout
    return Image.open(io.BytesIO(data)).convert("RGBA")


def glows(w, h, spots):
    """A navy field with soft colour glows: (cx, cy, r, colour, opacity)."""
    g = "".join(
        '<radialGradient id="g%d" cx="%f" cy="%f" r="%f" gradientUnits="userSpaceOnUse">'
        '<stop offset="0" stop-color="%s" stop-opacity="%f"/>'
        '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
        % (i, cx, cy, r, c, o, c) for i, (cx, cy, r, c, o) in enumerate(spots))
    rects = "".join('<rect width="%d" height="%d" fill="url(#g%d)"/>' % (w, h, i)
                    for i in range(len(spots)))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">'
            '<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#0E1530"/><stop offset="1" stop-color="%s"/>'
            '</linearGradient>%s</defs><rect width="%d" height="%d" fill="url(#bg)"/>%s</svg>'
            % (w, h, NAVY, g, w, h, rects))


def wordmark(px, color=(255, 255, 255)):
    k = 4
    f = ImageFont.truetype(os.path.join(FONTS, "Outfit-Bold.ttf"), px * k)
    w = int(f.getlength("Retro Adhoc")) + 4 * k
    asc, desc = f.getmetrics()
    im = Image.new("L", (w, asc + desc), 0)
    ImageDraw.Draw(im).text((2 * k, 0), "Retro Adhoc", font=f, fill=255)
    bb = im.getbbox()
    im = im.crop((bb[0], bb[1], bb[2], bb[3]))
    im = im.resize((max(1, im.width // k), max(1, im.height // k)), Image.LANCZOS)
    out = Image.new("RGBA", im.size, color + (0,))
    out.putalpha(im)
    return out


def indexed(im, path):
    """256 colours through pngquant when it is installed (its dithering is
    far quieter on smooth gradients), else Pillow's own."""
    import shutil
    import subprocess
    import tempfile
    pq = shutil.which("pngquant")
    if pq:
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.png")
            im.convert("RGB").save(src)
            subprocess.run([pq, "--force", "--quality=80-100", "--speed", "1",
                            "256", "--output", path, src], check=True)
        return
    im.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT,
                               dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def cart_svg(size):
    """The mark: one cartridge, a play triangle on its screen, gold contacts."""
    pins = "".join('<rect x="%d" y="86" width="3" height="12" rx="1" fill="#D6B246"/>'
                   % (49 + i * 5.4) for i in range(6))
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" '
            'width="%d" height="%d">'
            '<defs><filter id="b" x="-50%%" y="-50%%" width="200%%" height="200%%">'
            '<feGaussianBlur stdDeviation="9"/></filter></defs>'
            '<rect x="34" y="16" width="60" height="92" rx="10" fill="#7B6CFF" '
            'opacity="0.8" filter="url(#b)"/>'
            '<rect x="34" y="16" width="60" height="92" rx="9" fill="#EEF0FA"/>'
            '<rect x="40" y="24" width="48" height="46" rx="5" fill="#28245A"/>'
            '<polygon points="56,33 56,61 78,47" fill="#FFFFFF"/>'
            '<rect x="46" y="82" width="36" height="18" rx="3" fill="#3C3C50"/>%s'
            '</svg>' % (size, size, pins))


GLOWS6 = ["#7B6CFF", "#8CC63F", "#19C3B4", "#3B82F6", "#E0553F", "#B08CFF"]


def icon0():
    s = 128
    im = svg_png(glows(s, s, [(30, 20, 120, "#4B3BC4", 0.8),
                              (120, 120, 110, "#7B6CFF", 0.35)]), s, s)
    im.alpha_composite(svg_png(cart_svg(s), s, s), (0, 0))
    return im


def bg():
    w, h = 840, 500
    spots = [(60 + i * 150, 80 + (i % 2) * 340, 300, c, 0.28)
             for i, c in enumerate(GLOWS6)]
    im = svg_png(glows(w, h, spots), w, h)
    # the mark, very large and faint, off the right edge
    big = svg_png(cart_svg(512), 520, 520)
    a = big.getchannel("A").point(lambda v: v * 22 // 255)
    big.putalpha(a)
    im.alpha_composite(big, (w - 420, 40))
    return im


def startup():
    w, h = 280, 158
    im = svg_png(glows(w, h, [(40, 20, 220, "#4B3BC4", 0.75),
                              (260, 150, 180, "#7B6CFF", 0.35)]), w, h)
    mark = svg_png(cart_svg(72), 72, 72)
    word = wordmark(27)
    total = mark.width + 10 + word.width
    x = (w - total) // 2
    im.alpha_composite(mark, (x, (h - 72) // 2 - 14))
    im.alpha_composite(word, (x + mark.width + 10, (h - word.height) // 2 - 14))
    tag = ImageFont.truetype(os.path.join(FONTS, "Inter-Regular.ttf"), 13)
    d = ImageDraw.Draw(im)
    t = "GBA  GB  GBC  MD  NES  SNES"
    d.text(((w - tag.getlength(t)) / 2, h - 44), t, font=tag,
           fill=(200, 208, 226, 255))
    return im


def main():
    out = {
        os.path.join(VITA, "sce_sys", "icon0.png"): icon0(),
        os.path.join(VITA, "sce_sys", "livearea", "contents", "bg.png"): bg(),
        os.path.join(VITA, "sce_sys", "livearea", "contents", "startup.png"): startup(),
    }
    for path, im in out.items():
        indexed(im, path)
        print("wrote", path, im.size, Image.open(path).mode)


if __name__ == "__main__":
    main()
