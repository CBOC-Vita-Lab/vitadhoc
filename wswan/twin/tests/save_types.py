#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
"""Save data for every save type the cartridge footer can declare, on synthetic ROMs (make_roms.py). Per type:
  - the file is the cartridge bytes followed by "WSiE", u32 LE 1024 and the 1024 internal EEPROM bytes (INTEGRATION.md),
    so the cartridge part is exactly the declared size (SRAM 8 KiB .. 512 KiB, EEPROM 128 B / 1 KiB / 2 KiB)
  - persistence: a second run finds the boot counter of the first one higher (byte 0, or the bank chosen by port C1)
  - the last byte of the whole cartridge region is kept (SRAM types, through a cheat at 0x10000 + size - 1)
  - a forced write failure (a directory where the temporary file goes) leaves the original bytes and exits non-zero
  - no save memory (footer 0): the file holds only the internal EEPROM part
usage: save_types.py [--twin BIN] [--work DIR]
"""
import filecmp, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_roms as mr

# name, save kind, cartridge part size (0 = none), footer code, bank the ROM writes its counter in (SRAM), counter offset
TYPES = [
    ("h_nosave.ws",     "none",   0,       0x00, 0, 0),
    ("h_sram.ws",       "sram",   8192,    0x01, 0, 0),
    ("sram_32k.ws",     "sram",   32768,   0x02, 0, 0),
    ("sram_128k.ws",    "sram",   131072,  0x03, 0, 0),
    ("sram_256k.ws",    "sram",   262144,  0x04, 0, 0),
    ("sram_512k.ws",    "sram",   524288,  0x05, 0, 0),
    ("sram_512k_b5.ws", "sram",   524288,  0x05, 5, 5 << 16),
    ("eeprom_128.ws",   "eeprom", 128,     0x10, 0, 0),
    ("h_eeprom.ws",     "eeprom", 2048,    0x20, 0, 0),
    ("eeprom_1k.ws",    "eeprom", 1024,    0x50, 0, 0),
]
IEE = 1024
TRAILER = 8 + IEE


def run(twin, args):
    p = subprocess.run([twin] + args, capture_output=True, text=True, errors="replace", timeout=600)
    return p.returncode, p.stdout + p.stderr


def main():
    twin = os.environ.get("WSWAN_TWIN", os.path.join(HERE, "..", "build", "wswan_twin"))
    work = None
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--twin": twin = a.pop(0)
        elif k == "--work": work = a.pop(0)
        else: print(__doc__); return 2
    work = work or tempfile.mkdtemp(prefix="save_types_")
    roms = os.path.join(work, "roms"); os.makedirs(roms, exist_ok=True)
    mr_roms = {n: mr.build(n, **kw) for n, kw in mr.ROMS.items()}
    # the footer code of each ROM is what the test declares: rebuild the ones whose code differs from the table
    for name, kind, size, code, bank, coff in TYPES:
        kw = dict(mr.ROMS[name])
        kw["save_code"] = code
        kw["sram_bank"] = bank
        with open(os.path.join(roms, name), "wb") as f:
            f.write(mr.build(name, **kw))
    fails = 0; checks = 0

    def check(ok, msg):
        nonlocal fails, checks
        checks += 1
        if not ok:
            fails += 1
            print("FAIL", msg)
        else:
            print("pass", msg)

    for name, kind, size, code, bank, coff in TYPES:
        rom = os.path.join(roms, name)
        d = os.path.join(work, name[:-3]); shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
        srm = os.path.join(d, "game.srm"); bk = os.path.join(d, "bk"); os.makedirs(bk)
        rc, out = run(twin, [rom, "--frames", "40", "--srm", srm, "--backup-dir", bk, "--quiet"])
        check(rc == 0, "%s (footer %02X %s %d): first run rc=%d" % (name, code, kind, size, rc))
        if kind == "none":
            sz = os.path.getsize(srm) if os.path.exists(srm) else 0
            check(sz <= TRAILER, "%s: no cartridge part (file %d bytes, internal EEPROM part only, %d)" % (name, sz, TRAILER))
            continue
        data = open(srm, "rb").read()
        check(len(data) == size + TRAILER, "%s: file is %d = cartridge %d + trailer %d" % (name, len(data), size, TRAILER))
        check(data[size:size + 4] == b"WSiE" and int.from_bytes(data[size + 4:size + 8], "little") == IEE,
              "%s: trailer WSiE / 1024 at offset %d" % (name, size))
        c1 = data[coff]
        rc, _ = run(twin, [rom, "--frames", "40", "--srm", srm, "--backup-dir", bk, "--quiet"])
        data2 = open(srm, "rb").read()
        c2 = data2[coff]
        check(rc == 0 and c2 == (c1 + 1) & 0xFF and len(data2) == len(data),
              "%s: counter persisted across runs (%d -> %d at offset %#x), size stable" % (name, c1, c2, coff))
        check(os.path.exists(bk) and len(os.listdir(bk)) >= 1, "%s: previous file kept in the backup folder" % name)
        check(not os.path.exists(srm + ".tmp") and not os.path.exists(srm + ".old"), "%s: no .tmp / .old left" % name)
        if coff:
            check(data[0] == 0, "%s: bank %d selected through port C1: byte 0 untouched" % (name, bank))
        if kind == "sram":
            # the last byte of the cartridge region, written through the RA address space by a cheat each frame
            last = 0x10000 + size - 1
            srm2 = os.path.join(d, "cheat.srm")
            rc, _ = run(twin, [rom, "--frames", "5", "--srm", srm2, "--cheat", "%05X:5A" % last, "--quiet"])
            data3 = open(srm2, "rb").read() if os.path.exists(srm2) else b""
            check(rc == 0 and len(data3) == size + TRAILER and data3[size - 1] == 0x5A,
                  "%s: last SRAM byte (cart offset %d) written through address %05X and saved" % (name, size - 1, last))
            # and the first byte past the region is not accepted (the cheat must be refused, nothing written there)
            rc, out = run(twin, [rom, "--frames", "3", "--cheat", "%05X:5A" % (0x10000 + size), "--quiet"])
            check("cheats accepted: 0" in out, "%s: cheat past the SRAM end refused" % name)
        # forced write failure: the original must survive, and the exit code says so
        keep = os.path.join(d, "keep.srm"); shutil.copy(srm, keep)
        os.makedirs(srm + ".tmp", exist_ok=True)
        rc, _ = run(twin, [rom, "--frames", "40", "--srm", srm, "--backup-dir", bk, "--quiet"])
        os.rmdir(srm + ".tmp")
        check(rc != 0 and filecmp.cmp(srm, keep, shallow=False),
              "%s: forced write failure keeps the original bytes (rc=%d)" % (name, rc))
    print("save types: checks=%d failures=%d" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
