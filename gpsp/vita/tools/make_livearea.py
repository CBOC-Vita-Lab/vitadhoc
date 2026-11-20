#!/usr/bin/env python3
"""Render the local Neon Link artwork masters as Vita-indexed PNGs."""

import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

VITA = Path(__file__).resolve().parents[1]
MASTERS = VITA / "artwork" / "livearea"
FONTS = VITA.parent / "assets" / "fonts"
WHITE, MINT, CORAL, GOLD = "#f1faf5", "#71f4ef", "#ff8f79", "#f3c56a"


def text(font, size, color, position, words):
    x, y = position
    return ["-font", str(FONTS / (font + ".ttf")), "-pointsize", str(size),
            "-fill", color, "-draw", f"text {x},{y} '{words}'"]


def logo(mark, size, position):
    x, y = position
    return ["(", "-background", "none", str(mark), "-resize", f"{size}x{size}", ")",
            "-gravity", "NorthWest", "-geometry", f"+{x}+{y}", "-compose", "Over",
            "-composite", "+geometry", "-gravity", "Forget"]


def background(mark):
    # The a1 template draws the Start gate over the middle of this picture.
    args = logo(mark, 73, (21, 10))
    args += text("Outfit-Bold", 39, WHITE, (99, 50), "VitAdhoc")
    args += text("Inter-SemiBold", 14, MINT, (101, 72), "PLAY TOGETHER")
    args += ["-fill", "none", "-stroke", "#356269", "-strokewidth", "1",
             "-draw", "line 40,425 800,425", "-stroke", "none"]
    families = [(42, "GB/GBC", MINT), (190, "GBA", GOLD), (326, "NES", CORAL),
                (461, "SNES", MINT), (601, "WonderSwan", CORAL)]
    for x, label, color in families:
        args += ["-fill", color, "-draw", f"rectangle {x},446 {x + 4},460"]
        args += text("Outfit-Bold", 21, WHITE, (x + 13, 461), label)
    return args


def gate(mark):
    # LiveArea adds the blue Start button at the bottom; don't draw a second one.
    return (logo(mark, 80, (18, 22))
            + text("Outfit-Bold", 34, WHITE, (102, 64), "VitAdhoc")
            + text("Inter-SemiBold", 12, MINT, (104, 84), "MULTIPLAYER"))


def main():
    magick = shutil.which("magick")
    if magick is None:
        raise SystemExit("Install ImageMagick 7 to regenerate LiveArea art (not needed for normal VPK builds).")
    mark = MASTERS / "vitadhoc-mark.svg"
    with tempfile.TemporaryDirectory(dir=VITA) as temp:
        jobs = [
            (None, "sce_sys/icon0.png", (128, 128), logo(mark, 106, (11, 11))),
            ("background-master.png", "sce_sys/livearea/contents/bg.png", (840, 500), background(mark)),
            ("gate-master.png", "sce_sys/livearea/contents/startup.png", (280, 158), gate(mark)),
        ]
        staged = []
        for master, destination, (width, height), overlay in jobs:
            out = Path(temp) / Path(destination).name
            size = f"{width}x{height}"
            source = str(MASTERS / master) if master else "xc:#091422"
            subprocess.run([magick, source, "-filter", "Lanczos", "-resize", size + "^",
                            "-gravity", "Center", "-extent", size, "-gravity", "Forget",
                            *overlay, "-alpha", "off", "-dither", "Riemersma", "-colors", "256",
                            "-strip", "-interlace", "none", f"PNG8:{out}"], check=True)
            header = out.read_bytes()[:29]
            if header[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" or struct.unpack(">IIBBBBB", header[16:]) != (width, height, 8, 3, 0, 0, 0):
                raise ValueError(f"Unexpected Vita artwork format: {out.name}")
            staged.append((out, VITA / destination))
        for out, destination in staged:
            out.replace(destination)
            print("wrote", destination.relative_to(VITA))


if __name__ == "__main__":
    main()
