#!/usr/bin/env python3
"""Two SNES players linked: the proof of ONLINE-2.0 package B3 on Linux (loopback, then through the NAT simulator).

  link_test.py --twin BUILD/link_twin --roms DIR [--only a,b] [--frames N] [--out DIR]

Every scenario starts two `link_twin` processes (a host and a guest) from launch files with `link.*` keys, the way the
main app starts snes.bin, and checks what the plan asks of a two-player session:
  - both ended DONE, on the same emulated frame count, every periodic hash matched, the final hashes agreed;
  - the machine's state CRC at chosen frames is identical on the two consoles (the machines are in lockstep);
  - the host's save was written, the guest's own save file and own game were left exactly as they were;
  - a different game on the other console is refused (FE_GBLINK_E_ROM_MISSING), nothing is written.
ROMs are the owner's private ones (never copied into the repo): pass --roms. Exit status 0 when every scenario passed.
Copyright CBOC-Vita. MIT licence (see snes/frontend/LICENSE).
"""
import argparse, hashlib, os, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "gpsp", "vita", "hostcore"))

FAILS = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(("  PASS " if cond else "  FAIL ") + what)
    if not cond:
        FAILS.append(what)
    return bool(cond)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def sha(path):
    try:
        return hashlib.sha1(open(path, "rb").read()).hexdigest()
    except OSError:
        return None


class Player:
    def __init__(self, base, name, role, rom, link, extra=None, args=None):
        self.name = name
        self.role = role
        self.root = os.path.join(base, name)
        os.makedirs(os.path.join(self.root, "launch"), exist_ok=True)
        os.makedirs(os.path.join(self.root, "saves"), exist_ok=True)
        self.srm = os.path.join(self.root, "saves", "game.srm")
        self.rom = rom
        self.args = args or []
        lines = ["version=1", "console=snes", "game=" + rom, "save=" + self.srm, "root=" + self.root, "start=new",
                 "session_id=lt-%s" % name, "link.role=" + role]
        for k, v in link.items():
            lines.append("link.%s=%s" % (k, v))
        for k, v in (extra or {}).items():
            lines.append("%s=%s" % (k, v))
        open(os.path.join(self.root, "launch", "snes.ini"), "w").write("\n".join(lines) + "\n")
        self.proc = None
        self.out = os.path.join(self.root, "out.txt")

    def seed_save(self, data):
        open(self.srm, "wb").write(data)
        self.srm_before = sha(self.srm)

    def start(self, twin):
        f = open(self.out, "w")
        self.proc = subprocess.Popen([twin, self.root] + self.args, stdout=f, stderr=subprocess.STDOUT)

    def wait(self, timeout):
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            return None
        return self.proc.returncode

    def text(self):
        return open(self.out, errors="replace").read()

    def kv(self, tag):
        m = re.findall(r"^LT %s (.*)$" % tag, self.text(), re.M)
        if not m:
            return None
        return dict(re.findall(r"(\w+)=(\S+)", m[-1]))

    def hashes(self):
        return dict((int(a), b) for a, b in re.findall(r"^LT hash frame=(\d+) crc=(\w+)", self.text(), re.M))


def run_pair(twin, base, name, rom_h, rom_g, link_h, link_g, args_h=None, args_g=None, start_gap=0.0, timeout=150,
             guest_first=False, seed_save=True):
    d = os.path.join(base, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    h = Player(d, "host", "host", rom_h, link_h, args=args_h)
    g = Player(d, "guest", "guest", rom_g, link_g, args=args_g)
    if seed_save:
        h.seed_save(bytes((i * 7 + 1) & 255 for i in range(2048)))
        g.seed_save(bytes((i * 13 + 5) & 255 for i in range(2048)))
    first, second = (g, h) if guest_first else (h, g)
    first.start(twin)
    time.sleep(start_gap)
    second.start(twin)
    rh = h.wait(timeout)
    rg = g.wait(timeout)
    return h, g, rh, rg


def common_checks(h, g, rh, rg, frames, hash_at):
    he, ge = h.kv("end"), g.kv("end")
    check(rh == 0 and rg == 0, "both players ended with the link DONE (exit %s / %s)" % (rh, rg))
    if not he or not ge:
        check(False, "both players printed their end line")
        print(h.text()[-1500:])
        print(g.text()[-1500:])
        return False
    check(he["ok"] == "1" and ge["ok"] == "1", "ok=1 on both (host %s, guest %s)" % (he.get("gerr"), ge.get("gerr")))
    fh = re.search(r"phase=\w+ frames=(\d+)", h.text()[h.text().rfind("LT final"):])
    fg = re.search(r"phase=\w+ frames=(\d+)", g.text()[g.text().rfind("LT final"):])
    fh, fg = int(fh.group(1)), int(fg.group(1))
    check(fh >= frames and fh == fg, "the same emulated frames on both: %d / %d (asked %d)" % (fh, fg, frames))
    hm = he["hashes"].split("/")
    gm = ge["hashes"].split("/")
    check(hm[0] == hm[1] and int(hm[0]) > 0 and gm[0] == gm[1] and int(gm[0]) > 0,
          "every periodic hash matched (host %s, guest %s)" % (he["hashes"], ge["hashes"]))
    hh, gh = h.hashes(), g.hashes()
    same = bool(hash_at) and all(f in hh and f in gh and hh[f] == gh[f] for f in hash_at)
    check(same, "state CRCs identical at frames %s (host %s / guest %s)" % (hash_at, [hh.get(f) for f in hash_at],
                                                                            [gh.get(f) for f in hash_at]))
    gf = g.kv("final")
    check(gf and gf["crc_before"] == gf["crc_after"] and ge["restored"] == "1",
          "the guest's own game was put back (state CRC %s -> %s)" % (gf and gf["crc_before"], gf and gf["crc_after"]))
    check(sha(g.srm) == g.srm_before, "the guest's own save file is untouched")
    # the host's resume point is the session's last frame (an older one would bring the older battery back on Continue);
    # the guest, whose machine never was its own game's, writes none
    def rsm(p):
        return [f for d, _, fs in os.walk(os.path.join(p.root, "states")) for f in fs if f.endswith(".rsm")]
    check(len(rsm(h)) == 1 and not rsm(g), "the host wrote its resume point at the agreed end, the guest none (%s / %s)" % (rsm(h), rsm(g)))
    return True


def lan_link(role, port, peer=None, **kw):
    d = {"mode": "lan", "local_port": port, "loopback": 1, "local_ip": "127.0.0.1", "delay": 4, "compat": "c0ffee01",
         "nick": role}
    if peer:
        d["peer"] = peer
    d.update(kw)
    return d


def scenario_lan(twin, base, roms, frames, name="lan", rom_name=None, **kw):
    rom = roms[rom_name] if rom_name else list(roms.values())[0]
    ph, pg = free_udp_port(), free_udp_port()
    hash_at = [frames // 3, frames // 2, frames - 40]
    ha = ["--frames", str(frames), "--seed", "7", "--render", "all", "--threads", "0"]
    ga = ["--frames", str(frames), "--seed", "7", "--render", "all", "--threads", "1"]
    for f in hash_at:
        ha += ["--hash-at", str(f)]
        ga += ["--hash-at", str(f)]
    print("[%s] %s" % (name, os.path.basename(rom)))
    h, g, rh, rg = run_pair(twin, base, name, rom, rom, lan_link("host", ph), lan_link("guest", pg, "127.0.0.1:%d" % ph),
                            args_h=ha + kw.get("extra_h", []), args_g=ga + kw.get("extra_g", []),
                            guest_first=kw.get("guest_first", False), start_gap=kw.get("gap", 0.3))
    ok = common_checks(h, g, rh, rg, frames, hash_at)
    he = h.kv("end")
    if he:
        print("  host: %s" % h.kv("final").get("link", "") if False else "  " + re.sub(r"^LT final ", "", [l for l in h.text().splitlines() if l.startswith("LT final")][-1]))
    return h, g


def scenario_all_roms(twin, base, roms, frames):
    """Every ROM: a state that fits, and two consoles that stay identical (one run each, short)."""
    print("[all_roms] %d games, %d frames each" % (len(roms), frames))
    rows = []
    for n, rom in roms.items():
        ph, pg = free_udp_port(), free_udp_port()
        hash_at = [frames // 2, frames - 20]
        ha = ["--frames", str(frames), "--seed", "3", "--render", "all", "--threads", "1"]
        ga = ["--frames", str(frames), "--seed", "3", "--render", "all", "--threads", "0"]
        for f in hash_at:
            ha += ["--hash-at", str(f)]
            ga += ["--hash-at", str(f)]
        h, g, rh, rg = run_pair(twin, base, "all_" + re.sub(r"\W+", "_", n), rom, rom, lan_link("host", ph),
                                lan_link("guest", pg, "127.0.0.1:%d" % ph), args_h=ha, args_g=ga, start_gap=0.2, timeout=120)
        st = h.kv("start") or {}
        hh, gh = h.hashes(), g.hashes()
        same = all(f in hh and hh.get(f) == gh.get(f) for f in hash_at)
        he = h.kv("end") or {}
        rows.append((n, st.get("state_size", "?"), st.get("sram", "?"), rh, rg, same, he.get("hashes", "?")))
        check(rh == 0 and rg == 0 and same, "%-34s state %s B, sram %s B, exit %s/%s, hashes %s, equal at %s" % (
            n, st.get("state_size"), st.get("sram"), rh, rg, he.get("hashes"), hash_at))
    sizes = [int(r[1]) for r in rows if r[1].isdigit()]
    if sizes:
        print("  state sizes: min %d, max %d (fe_gblink STATE_MAX 1048576)" % (min(sizes), max(sizes)))
        check(max(sizes) <= 1 << 20, "every state fits STATE_MAX (1 MiB)")


def scenario_desync(twin, base, roms):
    """A console that skips its pictures is NOT the same machine (clip counts, sprite range flags): the session must notice
    (periodic hash) and stop without writing anything. This is why the Vita loop draws every frame of a linked game."""
    rom = [v for k, v in roms.items() if "World" in k][0] if any("World" in k for k in roms) else list(roms.values())[0]
    ph, pg = free_udp_port(), free_udp_port()
    print("[desync] %s, the guest never draws" % os.path.basename(rom))
    h, g, rh, rg = run_pair(twin, base, "desync", rom, rom, lan_link("host", ph, hash_every=60),
                            lan_link("guest", pg, "127.0.0.1:%d" % ph, hash_every=60),
                            args_h=["--frames", "600", "--render", "all"], args_g=["--frames", "600", "--render", "none"], timeout=90)
    he, ge = h.kv("end"), g.kv("end")
    check(rh == 1 and rg == 1 and he and ge and he["ok"] == "0" and ge["ok"] == "0", "both stopped (exit %s / %s)" % (rh, rg))
    check(any("desync" in x.get("gerr", "") for x in (he or {}, ge or {})), "the error is DESYNC: %s / %s" % (he and he.get("gerr"), ge and ge.get("gerr")))
    check(sha(h.srm) == h.srm_before and sha(g.srm) == g.srm_before, "no save file was written")


def scenario_punch(twin, base, roms, frames, nat_a="port", nat_b="port", name="punch", expect_fail=False, **kw):
    """Through natsim: each console behind its own NAT, no LAN path; the peer's public address and the punch key come from
    the launch file (what the app wrote after the match). Re-punching with the same key must bring the link up."""
    import online_tools as ot
    rom = list(roms.values())[0]

    class T:
        dir = os.path.join(base, name + "_natsim")
        procs = []
    world = ot.NatWorld(T, nat_a=nat_a, nat_b=nat_b, turn=False, **kw)
    try:
        hash_at = [frames // 3, frames - 40]
        args = ["--frames", str(frames), "--seed", "5", "--render", "all"]
        for f in hash_at:
            args += ["--hash-at", str(f)]

        def lk(role, k):
            return {"mode": "punch", "local_port": 0, "delay": 4, "compat": "c0ffee01", "nick": role,
                    "sim": "127.0.0.1:%d" % world.port, "sim_id": k, "sim_lan_port": 19019,
                    "peer": "203.0.113.%d:19019" % (10 + (1 - k)), "punch_key": "0123456789abcdef", "wait_s": 12 if expect_fail else 30}
        print("[%s] %s, NATs %s / %s" % (name, os.path.basename(rom), nat_a, nat_b))
        h, g, rh, rg = run_pair(twin, base, name, rom, rom, lk("host", 0), lk("guest", 1), args_h=args + ["--threads", "0"],
                                args_g=args + ["--threads", "1"], start_gap=0.3, timeout=120)
        if expect_fail:
            he, ge = h.kv("end"), g.kv("end")
            check(rh == 1 and rg == 1 and he and ge and he["ok"] == "0" and ge["ok"] == "0",
                  "no direct path through a symmetric NAT: both end cleanly, no relay here (exit %s / %s, %s / %s)" % (
                      rh, rg, he and he.get("gerr"), ge and ge.get("gerr")))
            check(sha(h.srm) == h.srm_before and sha(g.srm) == g.srm_before, "no save file was written")
        else:
            common_checks(h, g, rh, rg, frames, hash_at)
        print("  natsim: %s" % world.stats())
    finally:
        world.stop()


def rest_cred(k, secret="turn-test-secret"):
    """The TURN REST credential natsim's TURN server expects (hmac.new, sha1, base64)."""
    import hmac, base64
    user = "%d:player%d" % (int(time.time()) + 3600, k)
    return user, base64.b64encode(hmac.new(secret.encode(), user.encode(), hashlib.sha1).digest()).decode()


def scenario_relay(twin, base, roms, frames, name="relay"):
    """Through natsim with symmetric NATs (no direct path): the app side (the twin's --form-relay does what link_vita /
    online_vita do) allocates on the relay, swaps relayed addresses, binds, and lets go WITHOUT releasing; snes.bin's Link then
    ADOPTS the allocation on the same local port (a Refresh and the same ChannelBind, no new Allocate) and the session runs
    through the relay. The server's counters prove it: two allocations in all, none released before the end."""
    import online_tools as ot
    rom = list(roms.values())[0]

    class T:
        dir = os.path.join(base, name + "_natsim")
        procs = []
    world = ot.NatWorld(T, nat_a="sym", nat_b="sym", turn=True)
    try:
        hash_at = [frames // 3, frames - 40]
        args = ["--frames", str(frames), "--seed", "9", "--render", "all"]
        for f in hash_at:
            args += ["--hash-at", str(f)]
        d = os.path.join(base, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

        def lk(role, k):
            user, pw = rest_cred(k)
            return {"mode": "relay", "local_port": 0, "delay": 4, "compat": "c0ffee01", "nick": role,
                    "sim": "127.0.0.1:%d" % world.port, "sim_id": k, "sim_lan_port": 19019, "relay_server": "198.51.100.9",
                    "relay_ports": "3478", "relay_user": user, "relay_pass": pw, "wait_s": 40}
        print("[%s] %s, NATs sym / sym" % (name, os.path.basename(rom)))
        h = Player(d, "host", "host", rom, lk("host", 0), args=args + ["--threads", "0", "--form-relay", os.path.join(d, "guest", "form.txt")])
        g = Player(d, "guest", "guest", rom, lk("guest", 1), args=args + ["--threads", "1", "--form-relay", os.path.join(d, "host", "form.txt")])
        h.seed_save(bytes((i * 7 + 1) & 255 for i in range(2048)))
        g.seed_save(bytes((i * 13 + 5) & 255 for i in range(2048)))
        h.start(twin)
        g.start(twin)
        rh, rg = h.wait(150), g.wait(150)
        common_checks(h, g, rh, rg, frames, hash_at)
        st = world.stats()
        print("  natsim: %s" % st)
        tn = st.get("turn", {})
        check(tn.get("allocations") == 2 and tn.get("mismatch", 0) == 0,
              "the adopt sent no new Allocate: 2 allocations in all, no 437 (%s)" % tn)
        check(any("adopted" in l for l in h.text().splitlines()) or "adopted" in h.text() + g.text(), "snes.bin's Link logged the adoption")
    finally:
        world.stop()


def scenario_wrong_game(twin, base, roms):
    names = list(roms)
    a, b = roms[names[0]], roms[names[1]]
    ph, pg = free_udp_port(), free_udp_port()
    print("[wrong_game] %s vs %s" % (os.path.basename(a), os.path.basename(b)))
    h, g, rh, rg = run_pair(twin, base, "wrong_game", a, b, lan_link("host", ph), lan_link("guest", pg, "127.0.0.1:%d" % ph),
                            args_h=["--frames", "600"], args_g=["--frames", "600"], timeout=90)
    he, ge = h.kv("end"), g.kv("end")
    check(rh == 1 and rg == 1, "both refused (exit %s / %s)" % (rh, rg))
    check(he and ge and he["ok"] == "0" and ge["ok"] == "0", "ok=0 on both")
    names_ = [x.get("gerr", "") for x in (he or {}, ge or {})]
    pr = [re.search(r"peer_reason=(-?\d+)", p.text()).group(1) for p in (h, g)]
    # the console that sees the other image refuses with ROM_MISSING; the other hears "peer aborted" carrying that reason (2)
    check(any("rom_missing" in n for n in names_) and all(("rom_missing" in n) or (pr[i] == "2") for i, n in enumerate(names_)),
          "FE_GBLINK_E_ROM_MISSING (own or as the peer's reason): %s, peer_reason %s" % (names_, pr))
    check(sha(h.srm) == h.srm_before and sha(g.srm) == g.srm_before, "no save file was written")
    check(int((he or {}).get("frames", "1")) == 0 and int((ge or {}).get("frames", "1")) == 0, "no frame ran")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--twin", required=True)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--out", default=None)
    ap.add_argument("--rom", default=None, help="a name fragment of the ROM the scenarios play")
    a = ap.parse_args()
    roms = {}
    for f in sorted(os.listdir(a.roms)):
        if f.lower().endswith((".smc", ".sfc")):
            roms[f] = os.path.join(a.roms, f)
    if len(roms) < 2:
        print("need at least two ROMs in %s" % a.roms)
        return 2
    if a.rom:
        pick = [k for k in roms if a.rom.lower() in k.lower()]
        rest = [k for k in roms if k not in pick]
        roms = dict((k, roms[k]) for k in pick + rest)
    base = a.out or tempfile.mkdtemp(prefix="link_test_")
    os.makedirs(base, exist_ok=True)
    only = [x for x in a.only.split(",") if x]
    scen = {
        "lan": lambda: scenario_lan(a.twin, base, roms, a.frames),
        "lan_guest_first": lambda: scenario_lan(a.twin, base, roms, a.frames, name="lan_guest_first", guest_first=True, gap=2.0),
        "all_roms": lambda: scenario_all_roms(a.twin, base, roms, min(a.frames, 600)),
        "desync": lambda: scenario_desync(a.twin, base, roms),
        "wrong_game": lambda: scenario_wrong_game(a.twin, base, roms),
        "punch": lambda: scenario_punch(a.twin, base, roms, min(a.frames, 900)),
        "relay": lambda: scenario_relay(a.twin, base, roms, min(a.frames, 900)),
        "punch_sym": lambda: scenario_punch(a.twin, base, roms, min(a.frames, 900), nat_a="port", nat_b="sym", name="punch_sym", expect_fail=True),
    }
    for k, fn in scen.items():
        if only and k not in only:
            continue
        fn()
    print("\n%d checks, %d failed (output in %s)" % (CHECKS, len(FAILS), base))
    for f in FAILS:
        print("FAILED: " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
