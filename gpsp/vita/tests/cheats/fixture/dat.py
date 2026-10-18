#!/usr/bin/env python3
"""dat.py -- make build/fixture, a libretro-database tree in miniature for
tools/make_cheatdb.py: the .cht files of fixture/cht, and No-Intro lists
naming the two test cartridges by the CRC-32 of the files just built.

    dat.py cheatrom.gba gbcheat.gb build/fixture
"""
import os
import shutil
import sys
import zlib

gba, gb, out = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
if os.path.isdir(os.path.join(out, "cht")):
    shutil.rmtree(os.path.join(out, "cht"))
shutil.copytree(os.path.join(here, "cht"), os.path.join(out, "cht"))
os.makedirs(os.path.join(out, "metadat", "no-intro"), exist_ok=True)


def crc(path):
    return "%08X" % (zlib.crc32(open(path, "rb").read()) & 0xFFFFFFFF)


def dat(system, games):
    path = os.path.join(out, "metadat", "no-intro", system + ".dat")
    with open(path, "w") as f:
        f.write('clrmamepro (\n\tname "%s"\n)\n' % system)
        for name, ext, c, serial in games:
            f.write('\ngame (\n\tname "%s"\n' % name)
            if serial:
                f.write('\tserial "%s"\n' % serial)
            f.write('\trom ( name "%s.%s" size 1 crc %s )\n)\n' % (name, ext, c))


dat("Nintendo - Game Boy Advance", [
    ("Cheat Test (World)", "gba", crc(gba), "ACHE"),
    # another title altogether, and the same title in another region
    ("Other Game (USA)", "gba", "11111111", "AOGE"),
    ("Cheat Test (Japan)", "gba", "22222222", "ACHJ"),
])
dat("Nintendo - Game Boy", [("GB Cheat (World)", "gb", crc(gb), "")])
dat("Nintendo - Game Boy Color", [("GB Cheat (World)", "gbc", crc(gb), "")])
