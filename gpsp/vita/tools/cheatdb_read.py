#!/usr/bin/env python3
"""cheatdb_read.py -- read a GBADCHT1 cheat database (see make_cheatdb.py,
cheatdb.c) in Python, for the tests of the NES and WonderSwan twins.

    read_db(path) -> (games, crcs, names)
      games: list of dict(name, sys, crc, cheats=[(title, code), ...])
      crcs:  {crc32: game index}        names: {key string: game index}
sys is the database's system number: 0 GBA, 1 GB, 2 GBC, 3 NES, 4 WS, 5 WSC.

Copyright CBOC-Vita. MIT licence.
"""
import struct
import sys


def read_db(path):
    b = open(path, "rb").read()
    (magic, ver, games, ncrc, ocrc, nser, oser, nname, oname, okeys, skeys,
     ogames, oblocks, size, total) = struct.unpack_from("<8sIIIIIIIIIIIIII", b, 0)
    if magic != b"GBADCHT1" or size != len(b):
        raise ValueError("not a GBADCHT1 database: %s" % path)
    out = []
    for i in range(games):
        off, sz, n, sysid, _, crc = struct.unpack_from("<IIHBBI", b, ogames + 16 * i)
        parts = b[off:off + sz].split(b"\0")
        out.append(dict(name=parts[0].decode(), sys=sysid, crc=crc,
                        cheats=[(parts[1 + 2 * k].decode(), parts[2 + 2 * k].decode())
                                for k in range(n)]))
    crcs = {}
    for i in range(ncrc):
        c, g = struct.unpack_from("<II", b, ocrc + 8 * i)
        crcs[c] = g
    keys = b[okeys:okeys + skeys]
    names = {}
    for i in range(nname):
        ko, g = struct.unpack_from("<II", b, oname + 8 * i)
        names[keys[ko:keys.index(b"\0", ko)].decode()] = g
    return out, crcs, names


if __name__ == "__main__":
    g, c, n = read_db(sys.argv[1])
    print(len(g), "games", len(c), "crcs", len(n), "names")
