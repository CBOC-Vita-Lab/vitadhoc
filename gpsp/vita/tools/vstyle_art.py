"""vstyle_art.py -- the Vita UI's icons and wordmark, as SVG.

One source for three uses: tools/bake_vstyle_icons.py bakes them into
vstyle_icons.h (8-bit coverage atlases the app tints at draw time),
tools/make_livearea.py draws the LiveArea pictures with them, and the
design mock-ups use them.

Icons are drawn on a 48 x 48 grid: 3.4 px strokes with round ends, the
weight of the system's own line icons, optically centred.  The order is
vstyle.h's VS_I_* enum.
"""

S = 'fill="none" stroke="#fff" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"'
F = 'fill="#fff"'


def _star(cx, cy, r_out, r_in):
    import math
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = -math.pi / 2 + i * math.pi / 5
        pts.append("%.2f,%.2f" % (cx + r * math.cos(a), cy + r * math.sin(a) + 1.2))
    return " ".join(pts)


ICONS = [
    # VS_I_PLAY: a rounded triangle
    ('play', '<path d="M17 11.5 L37 24 L17 36.5 Z" %s stroke="#fff" stroke-width="3.4" stroke-linejoin="round"/>' % F),
    # VS_I_GEAR
    ('gear', '<g %s><circle cx="24" cy="24" r="6"/>'
             '<path d="M24 6.5v5 M24 36.5v5 M6.5 24h5 M36.5 24h5 M11.6 11.6l3.5 3.5 M32.9 32.9l3.5 3.5 '
             'M11.6 36.4l3.5-3.5 M32.9 15.1l3.5-3.5"/><circle cx="24" cy="24" r="12.5"/></g>' % S),
    # VS_I_LINK: two rounded handhelds joined by a cable
    ('link', '<g %s><rect x="6" y="13" width="14" height="22" rx="3.5"/><rect x="28" y="13" width="14" height="22" rx="3.5"/>'
             '<path d="M20 24 h8"/><circle cx="13" cy="20" r="0.6"/><circle cx="35" cy="20" r="0.6"/></g>' % S),
    # VS_I_SPARK: cheats, a four-point sparkle and a small one
    ('spark', '<g %s><path d="M22 8 C23.5 17 25 18.5 34 20 C25 21.5 23.5 23 22 32 C20.5 23 19 21.5 10 20 C19 18.5 20.5 17 22 8 Z" %s/>'
              '<path d="M36 30 C36.6 33.4 37.6 34.4 41 35 C37.6 35.6 36.6 36.6 36 40 C35.4 36.6 34.4 35.6 31 35 C34.4 34.4 35.4 33.4 36 30 Z" %s/></g>'
              % ('stroke="#fff" stroke-width="2.4" stroke-linejoin="round"', F, F)),
    # VS_I_LAYERS: save states, three stacked sheets
    ('layers', '<g %s><path d="M24 8 L41 17 L24 26 L7 17 Z"/><path d="M7 24 L24 33 L41 24"/><path d="M7 31 L24 40 L41 31"/></g>' % S),
    # VS_I_POWER
    ('power', '<g %s><path d="M24 7 v15"/><path d="M15.5 12.5 A14 14 0 1 0 32.5 12.5"/></g>' % S),
    # VS_I_GRID: the game list
    ('grid', '<g %s><rect x="8" y="8" width="13" height="13" rx="3"/><rect x="27" y="8" width="13" height="13" rx="3"/>'
             '<rect x="8" y="27" width="13" height="13" rx="3"/><rect x="27" y="27" width="13" height="13" rx="3"/></g>' % S),
    # VS_I_STAR (filled) and VS_I_STAR_O (outline)
    ('star', '<polygon points="%s" %s stroke="#fff" stroke-width="2.6" stroke-linejoin="round"/>' % (_star(24, 24, 17, 7.5), F)),
    ('star_o', '<polygon points="%s" %s/>' % (_star(24, 24, 17, 7.5), S)),
    # VS_I_WIFI: three arcs and a dot
    ('wifi', '<g %s><path d="M8 19 A23 23 0 0 1 40 19"/><path d="M13.5 25 A15 15 0 0 1 34.5 25"/>'
             '<path d="M19 31 A7.5 7.5 0 0 1 29 31"/></g><circle cx="24" cy="37" r="2.8" %s/>' % (S, F)),
    # VS_I_BACK
    ('back', '<g %s><path d="M28 12 L16 24 L28 36"/></g>' % S),
    # VS_I_MORE: three dots
    ('more', '<g %s><circle cx="12" cy="24" r="3.2"/><circle cx="24" cy="24" r="3.2"/><circle cx="36" cy="24" r="3.2"/></g>' % F),
    # VS_I_PAD: controls
    ('pad', '<g %s><path d="M14 14 h20 a9 9 0 0 1 9 9 v2 a9 9 0 0 1 -9 9 h-20 a9 9 0 0 1 -9 -9 v-2 a9 9 0 0 1 9 -9 Z"/>'
            '<path d="M15 20.5 v7 M11.5 24 h7"/></g><circle cx="31" cy="21.5" r="2.2" %s/><circle cx="35.5" cy="26.5" r="2.2" %s/>' % (S, F, F)),
    # VS_I_SCREEN: display / video
    ('screen', '<g %s><rect x="6" y="10" width="36" height="24" rx="4"/><path d="M17 40 h14 M24 34 v6"/></g>' % S),
    # VS_I_REWIND
    ('rewind', '<g %s><path d="M23 14 L11 24 L23 34 Z"/><path d="M37 14 L25 24 L37 34 Z"/></g>' % ('fill="#fff" stroke="#fff" stroke-width="2.6" stroke-linejoin="round"')),
    # VS_I_PEOPLE: two players
    ('people', '<g %s><circle cx="18" cy="17" r="6"/><path d="M7 38 c0-7 5-11 11-11 s11 4 11 11"/>'
               '<path d="M30 11.5 a6 6 0 0 1 0 11.5"/><path d="M33 27.5 c5 1.2 8 5 8 10.5"/></g>' % S),
    # VS_I_FOLDER
    ('folder', '<g %s><path d="M6 15 a4 4 0 0 1 4 -4 h8 l4 4 h16 a4 4 0 0 1 4 4 v15 a4 4 0 0 1 -4 4 h-28 a4 4 0 0 1 -4 -4 Z"/></g>' % S),
    # VS_I_CHECK
    ('check', '<g %s><path d="M11 25 L20 34 L37 15"/></g>' % S),
    # VS_I_SAVE: a down arrow into a tray
    ('save', '<g %s><path d="M24 8 v20 M15 20 l9 9 l9 -9"/><path d="M9 32 v4 a4 4 0 0 0 4 4 h22 a4 4 0 0 0 4 -4 v-4"/></g>' % S),
    # VS_I_FAST
    ('fast', '<g %s><path d="M11 14 L23 24 L11 34 Z"/><path d="M25 14 L37 24 L25 34 Z"/></g>' % ('fill="#fff" stroke="#fff" stroke-width="2.6" stroke-linejoin="round"')),
    # VS_I_TRASH
    ('trash', '<g %s><path d="M9 13 h30 M19 13 v-4 h10 v4 M13 13 l2 25 a3 3 0 0 0 3 3 h12 a3 3 0 0 0 3 -3 l2 -25"/>'
              '<path d="M21 20 v13 M27 20 v13"/></g>' % S),
    # VS_I_PLUS
    ('plus', '<g %s><path d="M24 10 v28 M10 24 h28"/></g>' % S),
    # the pad's four symbols, as the system draws them
    ('btn_x', '<g %s><path d="M14 14 L34 34 M34 14 L14 34"/></g>' % S),
    ('btn_o', '<g %s><circle cx="24" cy="24" r="12.5"/></g>' % S),
    ('btn_tri', '<g %s><path d="M24 11 L37.5 34.5 L10.5 34.5 Z"/></g>' % S),
    ('btn_sq', '<g %s><rect x="12.5" y="12.5" width="23" height="23" rx="1.5"/></g>' % S),
    # VS_I_REFRESH
    ('refresh', '<g %s><path d="M37 21 A13.5 13.5 0 1 0 36 31"/><path d="M38 11 v10 h-10"/></g>' % S),
    # VS_I_CART: a cartridge
    ('cart', '<g %s><path d="M11 8 h20 l6 6 v26 a2 2 0 0 1 -2 2 h-24 a2 2 0 0 1 -2 -2 v-30 a2 2 0 0 1 2 -2 Z"/>'
             '<rect x="15" y="15" width="18" height="13" rx="2"/><path d="M16 42 v-6 M21 42 v-6 M26 42 v-6 M31 42 v-6"/></g>' % S),
    # VS_I_HOME
    ('home', '<g %s><path d="M8 22 L24 9 L40 22"/><path d="M12 19 v19 a2 2 0 0 0 2 2 h7 v-11 h6 v11 h7 a2 2 0 0 0 2 -2 v-19"/></g>' % S),
    # VS_I_PAUSE
    ('pause', '<g %s><rect x="13" y="11" width="7" height="26" rx="2"/><rect x="28" y="11" width="7" height="26" rx="2"/></g>' % F),
    # VS_I_CAMERA: screenshot
    ('camera', '<g %s><path d="M8 17 a4 4 0 0 1 4 -4 h5 l3 -4 h8 l3 4 h5 a4 4 0 0 1 4 4 v17 a4 4 0 0 1 -4 4 h-24 a4 4 0 0 1 -4 -4 Z"/>'
               '<circle cx="24" cy="25" r="7"/></g>' % S),
    # VS_I_SIGNAL: ad hoc / rooms, a beacon
    ('signal', '<g %s><circle cx="24" cy="26" r="3"/><path d="M16.5 33.5 a10.6 10.6 0 0 1 0 -15"/><path d="M31.5 18.5 a10.6 10.6 0 0 1 0 15"/>'
               '<path d="M11 39 a18.4 18.4 0 0 1 0 -26"/><path d="M37 13 a18.4 18.4 0 0 1 0 26"/></g>' % S),
    # VS_I_SEARCH
    ('search', '<g %s><circle cx="21" cy="21" r="13"/><path d="M31 31 L40 40"/></g>' % S),
    # VS_I_CLOSE
    ('close', '<g %s><path d="M14 14 L34 34 M34 14 L14 34"/></g>' % S),
]


def icon_svg(i, px=48, color="#ffffff"):
    name, body = ICONS[i]
    body = body.replace("#fff", color)
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 48 48">%s</svg>'
            % (px, px, body))


# The mark: two handhelds, one a step behind the other, joined -- the app's
# point (two consoles, one game) in the shape of a Vita's rounded slab.
def mark_svg(px=128, front="#ffffff", back="#7B6CFF", accent="#19C3B4"):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 64 64">'
            '<rect x="6" y="17" width="34" height="22" rx="8" fill="%s" opacity="0.9"/>'
            '<rect x="24" y="25" width="34" height="22" rx="8" fill="%s"/>'
            '<rect x="35" y="30.5" width="12" height="11" rx="2.5" fill="%s"/>'
            '<circle cx="29.5" cy="36" r="1.8" fill="%s"/><circle cx="52.5" cy="36" r="1.8" fill="%s"/>'
            '</svg>' % (px, px, back, front, accent, accent, accent))
