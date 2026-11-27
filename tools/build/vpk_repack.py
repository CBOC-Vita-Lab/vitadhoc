#!/usr/bin/env python3
"""vpk_repack.py -- make a new VPK from the last one, replacing only what changed.

`make vpk` packs with vita-pack-vpk: a full zip written again from every input.
Most edits touch one member (new art, a new gamedb.txt, a new snes.bin, a new
eboot.bin), and the other 25 do not change.  This tool takes the arguments the
normal pack takes -- the same -s / -b / -a list, which `make vpk-repack`
passes from the Makefile -- reads the existing package, and writes a package that
is what a fresh pack would have written:

  * the members, in the order vita-pack-vpk writes them (sce_sys/param.sfo,
    eboot.bin, then each -a in order);
  * a member whose content is unchanged is copied as its stored, already
    compressed bytes (not decompressed and compressed again), with its
    header, date and permissions exactly as they were;
  * a member whose content changed (compared by SHA-256 of the uncompressed
    data against the input file) is compressed again from the file (deflate, or stored when deflate does not shrink it, as the packer does),
    mode 0100644, dated from the file as the packer does;
  * a member that is new is added, one that is no longer wanted is dropped
    (both are reported), so the result never carries a stale file.

It prints what changed, writes the output next to the old package and renames
it into place, and checks the result (zip structure and CRCs, member list
and order, every member's content against its input file).

Usage (what `make vpk-repack` runs):
    vpk_repack.py OLD.vpk [-o NEW.vpk] -s param.sfo -b eboot.bin -a src=dest ...
    vpk_repack.py OLD.vpk --list        # members, sizes, hashes (no input files)

Exit status 0 on success, 1 on any problem (the old package is left alone).
Standard library only.  Not for releases: a release is a full `make vpk` from
a clean tree (the repack trusts that the old package was built from the same
build settings).
"""
import argparse
import hashlib
import os
import struct
import sys
import time
import zipfile
import zlib

LEVEL = 9
ATTR = 0o100644 << 16          # what vita-pack-vpk stores: regular file, rw-r--r--


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_raw(fp, info):
    """The stored (compressed) bytes of a member, straight from the file."""
    fp.seek(info.header_offset)
    hdr = fp.read(30)
    if hdr[:4] != b"PK\x03\x04":
        raise ValueError("bad local header for %s" % info.filename)
    name_len, extra_len = struct.unpack("<HH", hdr[26:30])
    fp.seek(info.header_offset + 30 + name_len + extra_len)
    return fp.read(info.compress_size)


def dos_time(path):
    t = time.localtime(os.path.getmtime(path))
    return (max(t.tm_year, 1980), t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)


def wanted_members(args):
    """[(zip name, input path)] in the order the packer writes them."""
    out = []
    if args.sfo:
        out.append(("sce_sys/param.sfo", args.sfo))
    if args.eboot:
        out.append(("eboot.bin", args.eboot))
    for spec in args.add or []:
        src, _, dst = spec.partition("=")
        if not dst:
            raise SystemExit("-a needs src=dest, got %r" % spec)
        out.append((dst, src))
    names = [n for n, _ in out]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise SystemExit("duplicate member names: %s" % ", ".join(sorted(dup)))
    return out


def listing(path):
    with zipfile.ZipFile(path) as z:
        for i in z.infolist():
            print("%-48s %9d %9d  %s" % (i.filename, i.file_size, i.compress_size,
                                          sha(z.read(i.filename))[:16]))


def repack(old, new, wanted, quiet=False):
    stats = {"same": 0, "changed": 0, "added": 0, "dropped": 0}
    report = []
    with zipfile.ZipFile(old) as zin, open(old, "rb") as rawin:
        old_names = {i.filename: i for i in zin.infolist()}
        for name, src in wanted:
            if not os.path.isfile(src):
                raise SystemExit("missing input for %s: %s" % (name, src))
        tmp = new + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
            zout._didModify = True      # raw copies below bypass zipfile's own bookkeeping
            for name, src in wanted:
                with open(src, "rb") as f:
                    data = f.read()
                info = old_names.get(name)
                if info is not None and info.file_size == len(data) \
                        and sha(zin.read(name)) == sha(data):
                    # unchanged: the stored bytes, the old header
                    zi = zipfile.ZipInfo(info.filename, info.date_time)
                    zi.compress_type = info.compress_type
                    zi.external_attr = info.external_attr
                    zi.create_system = info.create_system
                    zi.create_version, zi.extract_version = info.create_version, info.extract_version
                    zi.internal_attr = info.internal_attr
                    zi.flag_bits = info.flag_bits & ~0x08
                    zi.CRC, zi.compress_size, zi.file_size = info.CRC, info.compress_size, info.file_size
                    zi.header_offset = zout.fp.tell()
                    zout.fp.write(zi.FileHeader(zip64=False))
                    zout.fp.write(read_raw(rawin, info))
                    zout.start_dir = zout.fp.tell()     # where the central directory goes if this is the last
                    zout.filelist.append(zi)
                    zout.NameToInfo[zi.filename] = zi
                    stats["same"] += 1
                    continue
                zi = zipfile.ZipInfo(name, dos_time(src))
                # the packer stores a member that deflate does not make smaller
                c = zlib.compressobj(LEVEL, zlib.DEFLATED, -15)
                kind = zipfile.ZIP_DEFLATED if len(c.compress(data) + c.flush()) < len(data) else zipfile.ZIP_STORED
                zi.compress_type = kind
                zi.external_attr = ATTR
                zi.create_system = 3
                zout.writestr(zi, data, compress_type=kind, compresslevel=LEVEL)
                if info is None:
                    stats["added"] += 1
                    report.append("  added    %-46s %9d bytes" % (name, len(data)))
                else:
                    stats["changed"] += 1
                    report.append("  changed  %-46s %9d -> %d bytes" % (name, info.file_size, len(data)))
            for name in old_names:
                if name not in {n for n, _ in wanted}:
                    stats["dropped"] += 1
                    report.append("  dropped  %s" % name)
    return tmp, stats, report


def verify(path, wanted):
    """Structure, CRCs, member list and order, and every member against its input."""
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        if bad:
            return "bad CRC in %s" % bad
        names = [i.filename for i in z.infolist()]
        want = [n for n, _ in wanted]
        if names != want:
            return "member list/order differs from the wanted one: %r vs %r" % (names, want)
        for name, src in wanted:
            with open(src, "rb") as f:
                if sha(f.read()) != sha(z.read(name)):
                    return "content of %s differs from %s" % (name, src)
        for i in z.infolist():
            if i.external_attr != ATTR or i.compress_type not in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
                return "%s: attributes/compression differ from the packer's" % i.filename
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("old", help="the existing package (the last make vpk)")
    ap.add_argument("-o", "--out", help="output (default: replace OLD in place)")
    ap.add_argument("-s", dest="sfo", help="param.sfo, as vita-pack-vpk -s")
    ap.add_argument("-b", dest="eboot", help="eboot.bin, as vita-pack-vpk -b")
    ap.add_argument("-a", dest="add", action="append", metavar="SRC=DEST",
                    help="an extra member, as vita-pack-vpk -a (repeatable)")
    ap.add_argument("--list", action="store_true", help="list OLD and exit")
    args = ap.parse_args()
    if not os.path.isfile(args.old):
        print("vpk_repack: %s does not exist: run make vpk once first" % args.old, file=sys.stderr)
        return 1
    if args.list:
        listing(args.old)
        return 0
    wanted = wanted_members(args)
    if not wanted:
        print("vpk_repack: no inputs given", file=sys.stderr)
        return 1
    out = args.out or args.old
    t0 = time.time()
    tmp, stats, report = repack(args.old, out, wanted)
    problem = verify(tmp, wanted)
    if problem:
        os.unlink(tmp)
        print("vpk_repack: FAILED, %s; %s left as it was" % (problem, args.old), file=sys.stderr)
        return 1
    os.replace(tmp, out)
    for line in report:
        print(line)
    print("vpk_repack: %s: %d unchanged (stored bytes copied), %d changed, %d added, %d dropped; "
          "verified (CRCs, order, every member against its input), %.1fs"
          % (out, stats["same"], stats["changed"], stats["added"], stats["dropped"], time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
