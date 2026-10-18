"""contact.py DIR OUT.png [every] -- side-by-side contact sheet of a gl_host
run's dumps: one row per frame number, slot 0 left, slot 1 right."""
import sys, os, re
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
every = int(sys.argv[3]) if len(sys.argv) > 3 else 1
fr = sorted({int(m.group(1)) for f in os.listdir(d) for m in [re.match(r's\d_(\d+)\.bmp', f)] if m})
fr = [f for f in fr if f % every == 0] if every > 1 else fr
cols = 4
rows = (len(fr) + cols - 1) // cols
W, H = 2 * 240 + 8, 160 + 14
sheet = Image.new('RGB', (cols * W, rows * H), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fr):
    x, y = (i % cols) * W, (i // cols) * H
    for s in (0, 1):
        p = os.path.join(d, 's%d_%06u.bmp' % (s, f))
        if os.path.exists(p):
            sheet.paste(Image.open(p), (x + s * 244, y + 14))
    dr.text((x + 2, y), 'f=%d' % f, fill=(255, 255, 0))
sheet.save(out)
print(out, len(fr), 'rows')
