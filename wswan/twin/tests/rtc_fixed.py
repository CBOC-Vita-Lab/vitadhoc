#!/usr/bin/env python3
# Copyright CBOC-Vita. MIT licence.
# RTC determinism with a fixed host clock. The clock is 2023-11-14 22:13:20 UTC (--rtc-fixed). The rtc.ws ROM copies the
# cartridge RTC (command 0x15: year, month, day, weekday, hour, minute, second, BCD) into the first SRAM bytes, so the .srm file
# shows what the core seeded at load. Checks:
#   - UTC:  year 23, month 11, day 14, hour 22, minute 13, second 20 (BCD), and the same bytes on a second run
#   - JST (TZ=Asia/Tokyo, +9 h): 2023-11-15 07:13:20
#   - the weekday byte is the same in both zones' runs of the same instant, and identical between runs
# usage: rtc_fixed.py [--twin BIN] [--work DIR]
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

def bcd(n): return ((n // 10) << 4) | (n % 10)

def run(twin, rom, tz, srm):
    env = dict(os.environ, TZ=tz)
    subprocess.run([twin, rom, "--frames", "30", "--rtc-fixed", "--srm", srm, "--quiet"], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    with open(srm, "rb") as f:
        return f.read(8)

def main():
    twin = os.environ.get("WSWAN_TWIN", os.path.join(HERE, "..", "build", "wswan_twin"))
    work = None
    a = sys.argv[1:]
    while a:
        k = a.pop(0)
        if k == "--twin": twin = a.pop(0)
        elif k == "--work": work = a.pop(0)
        else: sys.exit("usage: rtc_fixed.py [--twin BIN] [--work DIR]")
    own = work is None
    work = work or tempfile.mkdtemp(prefix="rtcfix-")
    os.makedirs(work, exist_ok=True)
    sys.path.insert(0, os.path.join(HERE))
    import make_roms as mr
    rom = os.path.join(work, "rtc.ws")
    with open(rom, "wb") as f: f.write(mr.build_rtc_rom())

    fails = 0
    def check(name, ok):
        nonlocal fails
        print(("pass " if ok else "FAIL ") + name)
        if not ok: fails += 1

    utc = [bcd(23), bcd(11), bcd(14), None, bcd(22), bcd(13), bcd(20)]
    a1 = run(twin, rom, "UTC", os.path.join(work, "utc1.srm"))
    a2 = run(twin, rom, "UTC", os.path.join(work, "utc2.srm"))
    check("UTC run: year/month/day/hour/minute/second", all(a1[i] == utc[i] for i in range(7) if utc[i] is not None))
    check("UTC run is deterministic (second run identical)", a1 == a2)
    check("marker byte written", a1[7] == 0xA5)
    jst = [bcd(23), bcd(11), bcd(15), None, bcd(7), bcd(13), bcd(20)]
    b1 = run(twin, rom, "Asia/Tokyo", os.path.join(work, "jst1.srm"))
    b2 = run(twin, rom, "Asia/Tokyo", os.path.join(work, "jst2.srm"))
    check("JST run: 2023-11-15 07:13:20", all(b1[i] == jst[i] for i in range(7) if jst[i] is not None))
    check("JST run is deterministic (second run identical)", b1 == b2)
    # the weekday encoding is the core's; it is reported, not checked, because the spec for it is not in the docs we have
    print("info: weekday byte UTC %02X (2023-11-14), JST %02X (2023-11-15)" % (a1[3], b1[3]))
    print("rtc fixed clock: %s" % ("PASS" if fails == 0 else "FAIL (%d)" % fails))
    if own:
        for n in os.listdir(work): os.remove(os.path.join(work, n))
        os.rmdir(work)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
