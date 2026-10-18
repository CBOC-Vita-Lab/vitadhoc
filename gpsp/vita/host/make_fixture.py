#!/usr/bin/env python3
"""Build the fake data root the UI host test runs against.

    python3 make_fixture.py <root>

Everything here is SYNTHETIC.  The "ROMs" are empty (sparse) files that only
carry a file name, a size and -- for the GBA ones -- a four-letter game code
at 0xAC so the link-mode table can be exercised; no game data exists on this
machine and none is needed.  The art is drawn by this script.

Layout (what vita/pspcompat.h calls the data root, ux0:data/GBAdhoc on a Vita):

    roms/        *.gba *.gb *.gbc (+ a sub-folder), .sav files, save states
                 with their .thumb/.shot previews, favourites.txt
    boxart/      covers: big PNGs, a square one, a small one, and one PSP-pack
                 .565 (128-px texture) with no source beside it
    hero/        16:9 art: one at the Vita's 960x544, one PSP-size 480x272 PNG,
                 one PSP-pack .565 (480x272 at a 512 stride)
    config.ini   a fresh install's worth
    log/         where the UI writes its frame dumps
"""
import os
import struct
import sys
import time

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
GPSP = os.path.dirname(os.path.dirname(HERE))
FONT_SB = os.path.join(GPSP, 'assets', 'fonts', 'Inter-SemiBold.ttf')
FONT_RG = os.path.join(GPSP, 'assets', 'fonts', 'Inter-Regular.ttf')

# name, size MiB, GBA header code, palette (two colours), has cover, has hero
GBA = [
    ('Advance Wars 2 - Black Hole Rising (USA).gba', 8, 'AW2E',
     ((200, 60, 40), (250, 190, 60)), 'big', None),
    ('Castlevania - Aria of Sorrow (USA).gba', 8, 'A2CE',
     ((40, 30, 80), (150, 40, 60)), 'big', 'psp565'),
    ('Final Fantasy Tactics Advance (USA).gba', 16, 'AFXE',
     ((30, 90, 150), (240, 230, 200)), None, None),
    ('Golden Sun (USA, Europe).gba', 16, 'AGSE',
     ((230, 160, 40), (120, 50, 20)), 'square', None),
    ('Legend of Zelda, The - The Minish Cap (USA).gba', 16, 'BZME',
     ((40, 130, 70), (230, 220, 120)), 'big', 'psp'),
    ('Mario Kart - Super Circuit (USA).gba', 4, 'AMKE',
     ((220, 40, 40), (60, 120, 230)), 'small', None),
    ('Metroid Fusion (USA).gba', 8, 'AMTE',
     ((20, 40, 90), (240, 120, 30)), 'psp565', None),
    ('Pokemon - Emerald Version (USA, Europe).gba', 16, 'BPEE',
     ((20, 140, 90), (240, 220, 80)), 'big', 'vita'),
    ('Pokemon - FireRed Version (USA).gba', 16, 'BPRE',
     ((220, 80, 30), (250, 210, 90)), 'big', None),
    ('Pokemon Mystery Dungeon - Red Rescue Team (USA, Australia).gba', 32,
     'B24E', ((200, 50, 50), (250, 240, 230)), None, None),
    ('WarioWare, Inc. - Mega Microgames! (USA).gba', 8, 'AZWE',
     ((250, 220, 40), (150, 40, 150)), 'big', None),
    ('Homebrew/Anguna - Warriors of Virtue.gba', 2, 'ANGU',
     ((90, 60, 30), (200, 180, 120)), None, None),
]
GB = [
    ("Legend of Zelda, The - Link's Awakening (USA, Europe).gb", 1,
     ((60, 100, 40), (200, 220, 140)), 'big'),
    ('Pokemon - Red Version (USA, Europe).gb', 1,
     ((190, 40, 40), (240, 230, 220)), 'big'),
    ('Tetris (World) (Rev 1).gb', 1, ((40, 60, 120), (200, 200, 60)), None),
]
GBC = [
    ('Pokemon - Crystal Version (USA, Europe).gbc', 2,
     ((70, 150, 220), (230, 240, 250)), 'big'),
    ('Wario Land 3 (World).gbc', 2, ((230, 200, 40), (120, 60, 160)), None),
]


def stem(name):
    return os.path.splitext(os.path.basename(name))[0]


def title_of(name):
    t = stem(name)
    cut = t.find('(')
    return (t[:cut] if cut > 0 else t).strip()


def psp565(img):
    """RGB image -> PSP-order 565 words (R in bits 0-4), row major."""
    px = img.convert('RGB').tobytes()
    out = bytearray(len(px) // 3 * 2)
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
        out[i // 3 * 2] = v & 0xFF
        out[i // 3 * 2 + 1] = v >> 8
    return bytes(out)


def gradient(w, h, c0, c1, horizontal=False):
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = (x / max(1, w - 1)) if horizontal else (y / max(1, h - 1))
            px[x, y] = tuple(int(c0[k] + (c1[k] - c0[k]) * t) for k in range(3))
    return img


def wrap(draw, text, font, width):
    words, lines, cur = text.split(), [], ''
    for w in words:
        t = (cur + ' ' + w).strip()
        if draw.textlength(t, font=font) <= width or not cur:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def cover(name, pal, w, h):
    """A made-up box: a gradient, a few shapes, the title set in Inter."""
    c0, c1 = pal
    img = gradient(w, h, c0, c1)
    d = ImageDraw.Draw(img)
    s = w / 512.0
    for i in range(5):
        r = int((220 - i * 36) * s)
        cx, cy = int(w * 0.62), int(h * 0.60)
        col = tuple(min(255, int(c1[k] * (0.55 + 0.1 * i))) for k in range(3))
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=col,
                  width=max(1, int(10 * s)))
    d.polygon([(0, h), (int(w * 0.55), int(h * 0.45)), (int(w * 0.9), h)],
              fill=tuple(int(c0[k] * 0.5) for k in range(3)))
    d.rectangle((0, 0, w, int(46 * s)), fill=(16, 16, 20))
    f_small = ImageFont.truetype(FONT_SB, max(8, int(22 * s)))
    d.text((int(16 * s), int(10 * s)), 'GBADHOC TEST ART', font=f_small,
           fill=(230, 230, 230))
    f = ImageFont.truetype(FONT_SB, max(9, int(54 * s)))
    y = int(70 * s)
    for line in wrap(d, title_of(name), f, w - int(48 * s)):
        d.text((int(26 * s), y + int(3 * s)), line, font=f, fill=(0, 0, 0))
        d.text((int(24 * s), y), line, font=f, fill=(255, 255, 255))
        y += int(62 * s)
    return img


def hero(name, pal, w, h):
    """16:9 art composed for the marquee: quiet and dark on the left."""
    c0, c1 = pal
    img = gradient(w, h, tuple(int(v * 0.18) for v in c0),
                   tuple(int(v * 0.9) for v in c0), horizontal=True)
    d = ImageDraw.Draw(img)
    s = w / 960.0
    for i in range(7):
        r = int((300 - i * 38) * s)
        cx, cy = int(w * 0.74), int(h * 0.52)
        col = tuple(min(255, int(c1[k] * (0.35 + 0.1 * i))) for k in range(3))
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=col,
                  width=max(1, int(14 * s)))
    for i in range(9):
        x0 = int(w * 0.45) + int(i * 60 * s)
        d.line((x0, h, x0 + int(180 * s), int(h * 0.30)),
               fill=tuple(int(c1[k] * 0.6) for k in range(3)),
               width=max(1, int(3 * s)))
    img = img.filter(ImageFilter.GaussianBlur(0.6 * s))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT_SB, max(8, int(20 * s)))
    t = title_of(name).upper()
    d.text((w - int(24 * s) - d.textlength(t, font=f), int(h * 0.80)), t,
           font=f, fill=tuple(min(255, int(v * 1.1)) for v in c1))
    return img


def write_565(path, img, stride):
    """tools/heroforge/raw565.py's format: '565R', w, h, stride, 16 bytes."""
    w, h = img.size
    px = psp565(img)
    with open(path, 'wb') as f:
        f.write(b'565R' + struct.pack('<HHH', w, h, stride) + bytes(6))
        for y in range(h):
            f.write(px[y * w * 2:(y + 1) * w * 2])
            f.write(bytes((stride - w) * 2))


def game_frame(w, h, seed):
    """A made-up game screen for the save-state previews."""
    img = Image.new('RGB', (w, h), (96, 168, 232))
    d = ImageDraw.Draw(img)
    d.rectangle((0, h * 2 // 3, w, h), fill=(72, 160, 72))
    for i in range(6):
        x = (seed * 37 + i * 43) % w
        d.rectangle((x, h * 2 // 3 - 18 - (i * 7) % 20, x + 14, h * 2 // 3),
                    fill=(150 + (i * 17) % 80, 90, 60))
    d.ellipse((w - 50, 10, w - 20, 40), fill=(250, 230, 90))
    d.rectangle((8, h - 34, w - 8, h - 6), fill=(248, 248, 248),
                outline=(40, 40, 40))
    f = ImageFont.truetype(FONT_RG, 11)
    d.text((14, h - 30), 'SAVE STATE %d  -  TEST FRAME' % seed, font=f,
           fill=(20, 20, 20))
    return img


def write_state(base_st0, slot, w, h, thumb=True, shot=True, when=None):
    path = base_st0[:-1] + str(slot - 1)
    with open(path, 'wb') as f:
        f.write(b'GBADHOC-TEST-STATE' + bytes(4096))
    frame = game_frame(w, h, slot)
    if thumb:
        t = frame.resize((64, 42), Image.BOX)
        with open(path + '.thumb', 'wb') as f:
            f.write(b'STH1' + struct.pack('<HHH', 64, 42, 64) + bytes(6))
            f.write(psp565(t))
    if shot:
        px = psp565(frame)
        with open(path + '.shot', 'wb') as f:
            f.write(b'STH1' + struct.pack('<HHH', w, h, 256) + bytes(6))
            for y in range(h):
                f.write(px[y * w * 2:(y + 1) * w * 2])
                f.write(bytes((256 - w) * 2))
    if when:
        for p in (path, path + '.thumb', path + '.shot'):
            if os.path.exists(p):
                os.utime(p, (when, when))


def sparse(path, mib, code=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        if code:
            f.seek(0xAC)
            f.write(code.encode('ascii'))
        f.truncate(mib * 1024 * 1024)


def main(root):
    for d in ('roms', 'boxart', 'hero', 'saves', 'log'):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    roms = os.path.join(root, 'roms')

    for name, mib, code, pal, cov, her in GBA:
        sparse(os.path.join(roms, name), mib, code)
        st = stem(name)
        if cov == 'big':
            cover(name, pal, 512, 712).save(
                os.path.join(root, 'boxart', st + '.png'))
        elif cov == 'square':
            cover(name, pal, 600, 600).save(
                os.path.join(root, 'boxart', st + '.png'))
        elif cov == 'small':
            cover(name, pal, 160, 222).save(
                os.path.join(root, 'boxart', st + '.png'))
        elif cov == 'psp565':
            # What a PSP leaves behind: the 128-px texture, no source.
            write_565(os.path.join(root, 'boxart', st + '.565'),
                      cover(name, pal, 512, 712).resize((126, 176), Image.BOX),
                      128)
        if her == 'vita':
            hero(name, pal, 960, 544).save(
                os.path.join(root, 'hero', st + '.png'))
        elif her == 'psp':
            hero(name, pal, 480, 272).save(
                os.path.join(root, 'hero', st + '.png'))
        elif her == 'psp565':
            write_565(os.path.join(root, 'hero', st + '.565'),
                      hero(name, pal, 480, 272), 512)
    for name, mib, pal, cov in GB + GBC:
        sparse(os.path.join(roms, name), mib)
        if cov:
            cover(name, pal, 500, 500).save(
                os.path.join(root, 'boxart', stem(name) + '.png'))

    # Battery saves: GBA "<stem>.sav", GB/GBC "<name>.sav".
    for name in ('Pokemon - Emerald Version (USA, Europe)',
                 'Golden Sun (USA, Europe)', 'Metroid Fusion (USA)',
                 'Advance Wars 2 - Black Hole Rising (USA)'):
        open(os.path.join(roms, name + '.sav'), 'wb').write(bytes(1024))
    open(os.path.join(roms, 'Pokemon - Red Version (USA, Europe).gb.sav'),
         'wb').write(bytes(1024))

    # Save states.  Emerald: slot 1 and 2 with both previews, slot 3 an old
    # state with the small preview only, slot 5 with nothing but the state.
    t0 = time.mktime((2026, 10, 3, 14, 32, 0, 0, 0, -1))
    em = os.path.join(roms, 'Pokemon - Emerald Version (USA, Europe).st0')
    write_state(em, 1, 240, 160, when=t0)
    write_state(em, 2, 240, 160, when=t0 + 3600 * 5 + 240)
    write_state(em, 3, 240, 160, shot=False, when=t0 - 86400 * 9)
    write_state(em, 5, 240, 160, thumb=False, shot=False,
                when=t0 - 86400 * 40)
    rd = os.path.join(roms, 'Pokemon - Red Version (USA, Europe).gb.st0')
    write_state(rd, 1, 160, 144, when=t0 + 86400)

    open(os.path.join(roms, 'favourites.txt'), 'w').write(
        'Pokemon - Emerald Version (USA, Europe).gba\n'
        'Golden Sun (USA, Europe).gba\n'
        'Tetris (World) (Rev 1).gb\n')

    open(os.path.join(root, 'config.ini'), 'w').write(
        'group = GPSP07\nconsole = 0\ntheme = 0\nui_shell = 1\n'
        'last_rom = Pokemon - Emerald Version (USA, Europe).gba\n')
    print('fixture: %s (%d GBA, %d GB, %d GBC)' % (root, len(GBA), len(GB),
                                                   len(GBC)))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'vita_root')
