"""sheet.py DUMPDIR OUT.png [--from F] [--to F] [--every N] [--cols C]

Contact sheet of a gl_host run's dumps (s0_FFFFFF.bmp / s1_FFFFFF.bmp):
one cell per frame, slot 0 (host's machine) left, slot 1 right.  Runs on
any Python with Pillow (the Windows one reads \\\\wsl.localhost paths)."""
import argparse
import os
import re

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("out")
ap.add_argument("--from", dest="f0", type=int, default=0)
ap.add_argument("--to", dest="f1", type=int, default=1 << 30)
ap.add_argument("--every", type=int, default=1)
ap.add_argument("--cols", type=int, default=3)
ap.add_argument("--max", type=int, default=24)
a = ap.parse_args()
fr = sorted({int(m.group(1)) for f in os.listdir(a.dir)
             for m in [re.match(r"s\d_(\d+)\.bmp$", f)] if m})
fr = [f for f in fr if a.f0 <= f <= a.f1]
fr = fr[::a.every][: a.max]
cols = a.cols
rows = max(1, (len(fr) + cols - 1) // cols)
W, H = 2 * 240 + 12, 160 + 14
sheet = Image.new("RGB", (cols * W, rows * H), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fr):
    x, y = (i % cols) * W, (i // cols) * H
    for s in (0, 1):
        p = os.path.join(a.dir, "s%d_%06u.bmp" % (s, f))
        if os.path.exists(p):
            sheet.paste(Image.open(p), (x + s * 244, y + 14))
    dr.text((x + 2, y), "f=%d" % f, fill=(255, 255, 0))
sheet.save(a.out)
print(a.out, len(fr), "frames", fr[:1], fr[-1:])
