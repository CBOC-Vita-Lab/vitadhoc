#!/usr/bin/env python3
"""Contact sheets of the screenshots, for reviewing many at a glance:
    python3 contact.py <shots dir> <out dir> [substring ...]"""
import os, sys
from PIL import Image, ImageDraw, ImageFont
src, out = sys.argv[1], sys.argv[2]
filt = sys.argv[3:]
names = sorted(n for n in os.listdir(src) if n.endswith('.png')
               and (not filt or any(f in n for f in filt)))
os.makedirs(out, exist_ok=True)
font = ImageFont.truetype(os.path.join(os.path.dirname(os.path.abspath(__file__)),
       '..', '..', 'assets', 'fonts', 'Inter-Regular.ttf'), 16)
PER, COLS, TW, TH = 9, 3, 640, 363
for i in range(0, len(names), PER):
    part = names[i:i + PER]
    rows = (len(part) + COLS - 1) // COLS
    sheet = Image.new('RGB', (COLS * TW, rows * (TH + 22)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for k, n in enumerate(part):
        im = Image.open(os.path.join(src, n)).resize((TW, TH), Image.LANCZOS)
        x, y = (k % COLS) * TW, (k // COLS) * (TH + 22)
        sheet.paste(im, (x, y + 22))
        d.text((x + 4, y + 2), n[:-4], font=font, fill=(255, 255, 0))
    sheet.save(os.path.join(out, 'sheet_%02d.png' % (i // PER)))
print(len(names), 'shots ->', out)
