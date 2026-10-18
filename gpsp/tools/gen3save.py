#!/usr/bin/env python3
"""gen3save.py -- validate a Pokemon generation 3 (.sav) flash image.

Ruby / Sapphire / Emerald / FireRed / LeafGreen keep two rotating save slots
of 14 sectors of 4 KiB (physical sectors 0-13 and 14-27 of a 128 KiB flash).
Every sector ends in a footer:

    0xFF4  u16  sector id (0..13: which logical chunk this physical sector holds)
    0xFF6  u16  checksum  (32-bit word sum of the chunk, high half + low half)
    0xFF8  u32  signature 0x08012025
    0xFFC  u32  save index (counter; the slot written is counter & 1)

At boot the game walks each slot (pokeruby/pokeemerald GetSaveValidStatus):
a slot with no signature anywhere is EMPTY; a slot whose 14 sectors all carry
the signature, a good checksum and between them every id 0..13 is VALID;
anything else is CORRUPT.  Both valid: the higher index is loaded.  One valid
and the other CORRUPT: the valid one is loaded and the game says "The save
file is corrupted.  The previous save will be loaded."  Neither valid: "The
save file is corrupted" / has been erased.

Usage:  gen3save.py FILE.sav [FILE2.sav ...]      (-q: one line per file)
Exit status: 0 = every file would load its newest slot cleanly, 1 otherwise.
"""
import struct
import sys

SECTOR = 4096
SLOT_SECTORS = 14
SIGNATURE = 0x08012025
FOOTER = 0xFF4

# Bytes of each logical chunk that the game checksums, per game family.
CHUNK_SIZES = {
    "RS":   [0x890, 0xF80, 0xF80, 0xF80, 0xC40] + [0xF80] * 8 + [0x7D0],
    "E":    [0xF2C, 0xF80, 0xF80, 0xF80, 0xF08] + [0xF80] * 8 + [0x7D0],
    "FRLG": [0xF24, 0xF80, 0xF80, 0xF80, 0xEE8] + [0xF80] * 8 + [0x7D0],
}


def checksum(data, size):
    total = 0
    for (w,) in struct.iter_unpack("<I", data[: size & ~3]):
        total = (total + w) & 0xFFFFFFFF
    return ((total >> 16) + total) & 0xFFFF


def blank(data):
    if data == b"\xff" * len(data):
        return "erased (all FF)"
    if data == b"\x00" * len(data):
        return "all 00"
    return None


class Sector:
    def __init__(self, phys, raw):
        self.phys = phys
        self.raw = raw
        self.id, self.sum, self.sig, self.index = struct.unpack_from("<HHII", raw, FOOTER)
        self.has_sig = self.sig == SIGNATURE
        self.blank = blank(raw)
        self.families = []
        if self.has_sig and self.id < SLOT_SECTORS:
            self.families = [f for f, sz in CHUNK_SIZES.items()
                             if checksum(raw, sz[self.id]) == self.sum]
        self.sum_ok = bool(self.families)

    def describe(self):
        if self.blank:
            return "phys %2d  %s" % (self.phys, self.blank)
        if not self.has_sig:
            return "phys %2d  NO SIGNATURE (sig=%08x id=%04x idx=%08x)" % (
                self.phys, self.sig, self.id, self.index)
        return "phys %2d  id=%2d  index=%-6d sum=%04x %s" % (
            self.phys, self.id, self.index, self.sum,
            "ok [%s]" % "/".join(self.families) if self.sum_ok else
            "BAD CHECKSUM (computed RS=%04x E=%04x FRLG=%04x)" % tuple(
                checksum(self.raw, CHUNK_SIZES[f][self.id] if self.id < 14 else 0xF80)
                for f in ("RS", "E", "FRLG")))


class Slot:
    def __init__(self, n, sectors):
        self.n = n
        self.sectors = sectors
        signed = [s for s in sectors if s.has_sig]
        mask = 0
        for s in signed:
            if s.sum_ok:
                mask |= 1 << s.id
        self.mask = mask
        self.index = None
        if not signed:
            self.status = "EMPTY"
        elif mask == 0x3FFF:
            self.status = "VALID"
        else:
            self.status = "CORRUPT"
        # The game takes the counter from the last signed sector it reads.
        if signed:
            self.index = signed[-1].index
        self.indices = sorted(set(s.index for s in signed))
        fams = None
        for s in signed:
            if s.sum_ok:
                fams = set(s.families) if fams is None else fams & set(s.families)
        # A checksum over a longer span also passes when the tail is zero, so
        # several families can match: the one with the smallest chunks is it.
        self.family = next((f for f in ("RS", "FRLG", "E") if fams and f in fams), "?")

    def problems(self):
        out = []
        if self.status != "CORRUPT":
            return out
        for s in self.sectors:
            if s.blank:
                out.append("phys %d is %s" % (s.phys, s.blank))
            elif not s.has_sig:
                out.append("phys %d has no signature" % s.phys)
            elif not s.sum_ok:
                out.append("phys %d (id %d) fails its checksum" % (s.phys, s.id))
        missing = [i for i in range(SLOT_SECTORS) if not self.mask & (1 << i)]
        if missing:
            out.append("no good copy of chunk id(s) %s" % ", ".join(map(str, missing)))
        if len(self.indices) > 1:
            out.append("mixed save indices %s (an interrupted or half-persisted save)"
                       % self.indices)
        return out


def verdict(a, b):
    """What the game does at boot.  Returns (text, clean)."""
    if a.status == "VALID" and b.status == "VALID":
        # pokeruby: the 0/-1 wrap case aside, the higher counter wins.
        if (a.index == 0xFFFFFFFF and b.index == 0) or (a.index == 0 and b.index == 0xFFFFFFFF):
            pick = a if a.index == 0 else b
        else:
            pick = a if a.index >= b.index else b
        return ("loads slot %d (index %d); no message" % (pick.n, pick.index), True)
    for good, other in ((a, b), (b, a)):
        if good.status == "VALID":
            if other.status == "CORRUPT":
                return ("loads slot %d (index %d) and shows \"The save file is corrupted. "
                        "The previous save will be loaded.\" -- slot %d is damaged"
                        % (good.n, good.index, other.n), False)
            return ("loads slot %d (index %d); no message (slot %d empty)"
                    % (good.n, good.index, other.n), True)
    if a.status == "EMPTY" and b.status == "EMPTY":
        return ("no save: starts as a new cartridge", True)
    return ("NO loadable slot: \"The save file is corrupted\" / new game", False)


def analyse(path, quiet=False):
    data = open(path, "rb").read()
    notes = []
    if len(data) != 131072:
        notes.append("file is %d bytes, not 131072 (128 KiB)" % len(data))
    if len(data) < 28 * SECTOR:
        data = data + b"\xff" * (28 * SECTOR - len(data))
        notes.append("short file: missing sectors treated as erased")
    sectors = [Sector(i, data[i * SECTOR:(i + 1) * SECTOR]) for i in range(28)]
    slots = [Slot(1, sectors[:14]), Slot(2, sectors[14:])]
    text, clean = verdict(*slots)
    if quiet:
        print("%s: %s | slot1 %s idx=%s, slot2 %s idx=%s%s" % (
            path, text, slots[0].status, slots[0].index, slots[1].status,
            slots[1].index, " | " + "; ".join(notes) if notes else ""))
        return clean and not notes
    print("== %s" % path)
    print("   size %d bytes%s" % (len(open(path, 'rb').read()),
                                 "" if not notes else "   ** " + "; ".join(notes)))
    for sl in slots:
        print("   slot %d (phys %d-%d): %-7s index=%s game=%s" % (
            sl.n, sl.sectors[0].phys, sl.sectors[-1].phys, sl.status,
            sl.index, sl.family))
        for s in sl.sectors:
            print("      " + s.describe())
        for p in sl.problems():
            print("      !! " + p)
    extra = []
    for i, name in ((28, "hall of fame 1"), (29, "hall of fame 2"),
                    (30, "mystery/e-reader"), (31, "battle record")):
        raw = data[i * SECTOR:(i + 1) * SECTOR]
        if len(raw) == SECTOR:
            extra.append("%d %s: %s" % (i, name, blank(raw) or "has data"))
    if extra:
        print("   other sectors: " + "; ".join(extra))
    print("   GAME: " + text)
    return clean and not notes


def main(argv):
    quiet = "-q" in argv
    files = [a for a in argv if a != "-q"]
    if not files:
        print(__doc__)
        return 2
    ok = True
    for f in files:
        ok &= analyse(f, quiet)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
