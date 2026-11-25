#!/usr/bin/env python3
"""Make the three font files the UI draws non-ASCII text from (SIL OFL fonts).

  cjkfont.otf        Noto Sans CJK JP Regular, cut down to: hiragana, katakana,
                     the JIS X 0208 kanji (levels 1 and 2, 6355 characters) and
                     symbols, CJK punctuation, fullwidth and halfwidth forms,
                     the 2350 Hangul syllables of KS X 1001, and the Latin /
                     Greek / Cyrillic letters the font draws.
  cjkfont-sc.otf     Noto Sans CJK SC Regular, cut down to: the 6763 hanzi of
                     GB 2312 (their Chinese forms: the interface drawn in
                     Simplified Chinese tries this file first and falls back
                     to cjkfont.otf), the kana, symbols, punctuation,
                     fullwidth forms, Bopomofo and the same Latin / Greek /
                     Cyrillic.
  latinfont.ttf      Inter Regular   cut to Latin-1, Latin Extended A/B, Greek,
  latinfont-sb.ttf   Inter SemiBold  Cyrillic, general punctuation (the players
                     draw their accented letters from these; the main app has
                     Inter inside it already).

The four files are packed into the VPK as app0:cjkfont.otf, app0:cjkfont-sc.otf,
app0:latinfont.ttf and app0:latinfont-sb.ttf. Needs fontTools (pip install fonttools) and, for the
CJK one, the Noto Sans CJK Regular collection (NotoSansCJK-Regular.ttc, the
JP face is member 0) from https://github.com/notofonts/noto-cjk.

  python3 tools/font/make_fonts.py --noto NotoSansCJK-Regular.ttc --out gpsp/vita/assets

Hinting, OpenType layout and the vertical tables are dropped: the UI
rasterises with stb_truetype, which uses none of them.
"""
import argparse, os, sys
from fontTools import subset
from fontTools.ttLib import TTFont, TTCollection


def jis0208():
    s = set()
    for hi in range(0x81, 0xF0):
        for lo in range(0x40, 0xFD):
            if lo == 0x7F:
                continue
            try:
                ch = bytes([hi, lo]).decode('shift_jis')
            except Exception:
                continue
            if len(ch) == 1:
                s.add(ord(ch))
    return s


def ksx1001_hangul():
    s = set()
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('euc_kr')
            except Exception:
                continue
            if len(ch) == 1:
                s.add(ord(ch))
    return s


def gb2312():
    s = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode('gb2312')
            except Exception:
                continue
            if len(ch) == 1:
                s.add(ord(ch))
    return s


def rng(*pairs):
    s = set()
    for a, b in pairs:
        s |= set(range(a, b + 1))
    return s


COMMON = rng((0xA0, 0x24F), (0x370, 0x3FF), (0x400, 0x52F), (0x1E00, 0x1EFF),
             (0x2010, 0x2027), (0x2030, 0x205E), (0x20A0, 0x20BF),
             (0x2190, 0x21FF), (0x2460, 0x2473)) | {0x2103, 0x2116, 0x2122, 0x2212, 0x2260}
CJK_EXTRA = rng((0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF), (0x31F0, 0x31FF),
                (0x2500, 0x257F), (0x25A0, 0x25FF), (0x2600, 0x260F),
                (0xFF01, 0xFF5E), (0xFF61, 0xFF9F), (0xFFE0, 0xFFE6))


def cut(font, cps, out, keep_names=True):
    o = subset.Options()
    o.layout_features = []
    o.name_IDs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17]
    o.notdef_outline = True
    o.hinting = False
    o.glyph_names = False
    o.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'vhea', 'vmtx', 'VORG', 'BASE', 'DSIG',
                      'hdmx', 'kern', 'LTSH', 'VDMX', 'gasp', 'fpgm', 'prep', 'cvt ']
    s = subset.Subsetter(o)
    s.populate(unicodes=sorted(cps))
    s.subset(font)
    font.save(out)
    print('%s: %d bytes, %d glyphs' % (out, os.path.getsize(out), len(font.getGlyphOrder())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--noto', help='NotoSansCJK-Regular.ttc')
    ap.add_argument('--inter', default='gpsp/assets/fonts', help='directory with Inter-Regular.ttf, Inter-SemiBold.ttf')
    ap.add_argument('--out', default='gpsp/vita/assets')
    a = ap.parse_args()
    if a.noto:
        coll = TTCollection(a.noto)
        f = coll.fonts[0]
        assert 'JP' in f['name'].getDebugName(1), f['name'].getDebugName(1)
        cut(f, jis0208() | ksx1001_hangul() | CJK_EXTRA | COMMON, os.path.join(a.out, 'cjkfont.otf'))
        sc = TTCollection(a.noto).fonts[2]
        assert 'SC' in sc['name'].getDebugName(1) and 'Mono' not in sc['name'].getDebugName(1), sc['name'].getDebugName(1)
        cut(sc, gb2312() | CJK_EXTRA | COMMON | rng((0x3100, 0x312F)), os.path.join(a.out, 'cjkfont-sc.otf'))
    for src, dst in (('Inter-Regular.ttf', 'latinfont.ttf'), ('Inter-SemiBold.ttf', 'latinfont-sb.ttf')):
        cut(TTFont(os.path.join(a.inter, src)), COMMON, os.path.join(a.out, dst))


if __name__ == '__main__':
    main()
