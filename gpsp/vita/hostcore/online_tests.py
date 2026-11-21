#!/usr/bin/env python3
"""online_tests.py -- two twins of the Vita app linked ONLINE: the room service of
online/service (node, on the loopback) and the NAT / TURN simulator of
netdrv/tests/natsim stand between them, and every step goes through the real menus
(the host shows a five character code, the joiner types it on the code pad).

    ./online_tests.py x86 [--hz 30]      both twins the x86 one
    ./online_tests.py armhf              both the armhf one under qemu-arm (the cheap set)
    options:  --only NAME[,NAME]   some of the scenarios
              --list               their names

What link_tests.py is to two consoles on one Wi-Fi this is to two consoles on two
home networks.  The pieces come from online_tools.py (FakeService, NatWorld, vita_for);
what is added here is the HttpProxy, a small stand-in between a twin and the service
that can lose an answer, answer instead of the service, or show the room card with
another version (the twins' own version is fixed, so that is how "the host is newer"
is made).

THE CODE.  The host's twin logs `EVT online_room code=XXXXX`; the test reads it and
types it into the joiner's pad (up from an empty box gives A, down gives 9: code_keys).
The pad is part of what is tested.

THE CARTRIDGES.  GBA: tools/linktest/linktest.gba, which decides its own verdict
(the result block, read through vita.ini link_peek).  GB: testrom/gblink.gb, whose
master is the console whose player presses A once the link is up (nobody pressed it in
M2's first smoke, which is why the cartridge's verdict read zeros there: the test's bug,
not the app's).  Their start windows are no issue: the link's session starts both
machines fresh, however long the code took to type.

Not exercised, and said so in the output: Wireless Adapter TRAFFIC (no homebrew speaks
the adapter's protocol; room, idle link and teardown are tested), and everything that
needs the real internet (Cloudflare, the real STUN servers, the Vita's TLS stack).
"""
import argparse
import http.client
import http.server
import json
import os
import re
import shutil
import signal
import socket
import sys
import threading
import time
import urllib.request

import link_tests as L
from link_tests import HERE, GP
from online_tools import FakeService, NatWorld, vita_for, net_for, Skip

ALPHA = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
REPO = os.path.normpath(os.path.join(GP, ".."))
UI_PSP_H = os.path.join(GP, "psp", "ui_psp.h")
ONLINE_VITA_H = os.path.join(GP, "vita", "online_vita.h")
LINK_GAMES_H = os.path.join(GP, "frontend-common", "link_games.h")


# ---- what the app's own headers say --------------------------------------------------

def _enum_names(path, first, prefix):
    """The names of the enum that contains `first`, in order, as {name: value}."""
    text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
    m = re.search(r"enum\s*(?:\w+\s*)?\{([^}]*\b%s\b[^}]*)\}" % first, text, re.S)
    out, n = {}, 0
    for item in m.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        name, _, val = item.partition("=")
        name = name.strip()
        if val.strip():
            n = int(val.strip(), 0)
        out[name] = n
        n += 1
    return out


_FAILS = {}


def ui_fail(name):
    """The value of UI_FAIL_<name> (ui_psp.h): what `link_online_failed ui=` and `online_card_bad why=` say."""
    if not _FAILS:
        _FAILS.update(_enum_names(UI_PSP_H, "UI_FAIL_NO_WIFI", "UI_FAIL_"))
    return _FAILS["UI_FAIL_" + name]


_STAGES = {}


def on_stage(name):
    if not _STAGES:
        text = open(ONLINE_VITA_H).read()
        _STAGES.update(_enum_names(ONLINE_VITA_H, "ON_IDLE", "ON_"))
    return _STAGES["ON_" + name]


def online_fail(name):
    """ONLINE_FAIL_<name> of online_vita.h: `online_fail why=`."""
    return _enum_names(ONLINE_VITA_H, "ONLINE_FAIL_NONE", "")["ONLINE_FAIL_" + name]


# ---- typing a code, reading one ----------------------------------------------------

def code_keys(code):
    """The d-pad presses that type `code` on the code pad (up from an empty box gives A, down 9)."""
    out = []
    for ch in code:
        k = ALPHA.index(ch)
        up, down = k + 1, len(ALPHA) - k
        out += ["up"] * up if up <= down else ["down"] * down
        out.append("right")
    return out


def wait_code(a, timeout=90):
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(r"online_room code=([A-Z0-9]{5})", a.log_now())
        if m:
            return m.group(1)
        if a.proc.poll() is not None:
            return None
        time.sleep(0.2)
    return None


def join_script(code, tail="", card_shot="card.png", press_join=True, hold=True):
    """B: the game's menu, Join a game, the code on the pad, the card; X on the card joins.  hold: every press that
    matters is held until the app has logged what it did (hold_code), so a busy machine cannot drop one and type another
    code; hold=False is the fixed script, for a scenario whose timed action must meet the script's own clock."""
    if hold:
        s = join_script_hold(code, "", card_shot)
        if press_join:
            s += "+%d:hold=cross=online_join code_given=1\n" % L.AFTER_SHOT + "+%d:shot=linking.png\n" % L.dur(1.5)
        return s + tail
    keys = code_keys(code)
    half = keys.index("right", keys.index("right") + 1) + 1          # two characters typed
    s = (L.open_play(L.EARLY) + "+20:down:%d\n" % L.PRESS + L.keys("cross") +
         "+30:shot=pad_empty.png\n" + L.keys(*keys[:half], first=L.AFTER_SHOT) +
         "+30:shot=pad_half.png\n" + L.keys(*keys[half:], first=L.AFTER_SHOT) + L.keys("triangle") +
         "+%d:shot=%s\n" % (L.dur(7), card_shot))
    if press_join:
        s += "+%d:cross:%d\n" % (L.AFTER_SHOT, L.PRESS) + "+%d:shot=linking.png\n" % L.dur(1.5)
    return s + tail


def join_script_hold(code, tail="", card_shot="card.png"):
    """join_script for a scenario that stops at the card: every press is held until the app logged what it did (the pad's
    keys, the card), so a busy machine cannot drop one and type another code"""
    return (L.open_play(L.EARLY) + "+20:down:%d\n" % L.PRESS + "+%d:hold=cross=online_pad_open\n" % L.GAP +
            "+30:shot=pad_empty.png\n" + hold_code(code) + "+%d:hold=triangle=online_card_begin\n" % L.GAP +
            "+0:wait=online_card state=\n" + "+%d:shot=%s\n" % (L.dur(1), card_shot) + tail)


def advance(svc, ms):
    """The dev server's clock goes forward (POST /_test/advance): a room ages without waiting."""
    req = urllib.request.Request(svc.url + "/_test/advance", method="POST", data=json.dumps({"ms": ms}).encode(),
                                 headers={"User-Agent": "VitAdhoc/online_tests", "content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def svc_call(svc, method, path, body=None):
    """One request to the service straight from the test (to use up a budget or a bucket)."""
    req = urllib.request.Request(svc.url + path, method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"User-Agent": "VitAdhoc/online_tests", "content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")


# ---- the proxy between a twin and the service -----------------------------------------

class HttpProxy:
    """http://127.0.0.1:<port> -> the service.  `rule(method, path, n)` is asked before a request is
    forwarded and may answer ("drop_answer",) to forward it and then hang up without answering (the answer
    is lost), ("answer", status, dict) to answer in the service's place, or None.  `edit(method, path, status,
    obj)` may return a changed answer object.  `seen` lists (method, path, what happened)."""

    def __init__(self, T, svc, rule=None, edit=None):
        self.svc, self.rule, self.edit = svc, rule, edit
        self.seen, self.count = [], {}
        outer = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *a):
                pass

            def _go(self):
                n = int(self.headers.get("content-length") or 0)
                body = self.rfile.read(n) if n else None
                path = self.path
                key = (self.command, re.sub(r"[A-Z0-9]{5}", "CODE", path.split("?")[0]))
                outer.count[key] = outer.count.get(key, 0) + 1
                act = outer.rule(self.command, path, outer.count[key]) if outer.rule else None
                if act and act[0] == "answer":
                    outer.seen.append((self.command, path, "answered %d" % act[1]))
                    return self._send(act[1], json.dumps(act[2]).encode())
                conn = http.client.HTTPConnection("127.0.0.1", outer.svc.port, timeout=30)
                hdr = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection")}
                conn.request(self.command, path, body=body, headers=hdr)
                r = conn.getresponse()
                data, status = r.read(), r.status
                conn.close()
                if act and act[0] == "drop_answer":
                    outer.seen.append((self.command, path, "answer lost (%d)" % status))
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    return
                if outer.edit:
                    try:
                        obj = outer.edit(self.command, path, status, json.loads(data.decode()))
                    except ValueError:
                        obj = None
                    if obj is not None:
                        data = json.dumps(obj).encode()
                outer.seen.append((self.command, path, "forwarded %d" % status))
                self._send(status, data)

            def _send(self, status, data):
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("cache-control", "no-store")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = _go

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.url = "http://127.0.0.1:%d" % self.port
        self.t = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.t.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# ---- one scenario's rig --------------------------------------------------------------------

class Rig:
    """What a scenario ran against and left behind."""
    ra = rb = None
    code = None
    st = {}
    reqs = []
    a = b = None


def host_script(end, extra="", shots=(12,)):
    """A: Host a game, a picture while it waits, `extra`, the session page's Disconnect at `end`-8."""
    s = L.host(L.EARLY, "host.png")
    for t in shots:
        s += L.shot(t, "waiting.png")
    return s + extra


def pair(T, name, rom=L.LINKTEST, rom_b=None, nat=("port", "port"), svc_env=(), world_kw=None,
         a_cfg=None, b_cfg=None, a_vita=None, b_vita=None, a_extra=None, b_extra=None,
         a_script=None, b_script=None, a_end=78, b_end=76, actions=(), proxy_a=None, proxy_b=None,
         with_world=True, peek=None, wait_a=90, setup=None, kind="gba", nick=("Alice", "Bob"), b_noauto=False, keep=None, begin=True):
    """Start the service and the world, the host's twin, read the code, start the joiner's twin, wait.

    a_script: str, the host's whole script (default: host, a shot while it waits, then a session of
    70 s and Disconnect).  b_script: callable(code) -> the joiner's script (default: join_script +
    shots).  `actions`: (seconds of the JOINER's script, fn(rig)) run on a thread.  proxy_a / proxy_b:
    callables(rig) -> HttpProxy that make the twin's online_url."""
    if begin:
        T.begin(name)
    rig = Rig()
    rig.T = T
    port = T.port
    rig.svc = FakeService(T, *svc_env)
    rig.world = NatWorld(T, nat[0], nat[1], lan_port=port, **(world_kw or {})) if with_world else None
    rig.proxies = []
    if setup:
        setup(rig)
    urls = []
    for mk in (proxy_a, proxy_b):
        if mk:
            p = mk(rig)
            rig.proxies.append(p)
            urls.append(p)
        else:
            urls.append(None)
    rig.pa, rig.pb = urls
    pk = peek or (L.LT_PEEK if kind == "gba" else "C000:2")

    def vita(k, extra, p):
        v = vita_for(k, rig.world, rig.svc) if rig.world else {"online_url": rig.svc.url, "online_stun": "off"}
        v["link_peek"] = pk
        v.update(extra or {})
        if p:
            v["online_url"] = p.url
        return v

    cfg_a = {"net_conn": 3}
    cfg_a.update(a_cfg or {})
    cfg_b = {"net_conn": 3}
    cfg_b.update(b_cfg or {})
    sa = a_script if a_script is not None else (
        host_script(a_end) + L.shot(60, "running.png") + L.disconnect(70, "session.png") +
        L.shot(74, "ended.png") + L.quit_at(78))
    rig.a = T.twin("a", T.arch_a, rom, nick[0], sa, config=cfg_a, vita=vita(0, a_vita, rig.pa), net=net_for(0),
                   extra=a_extra, keep=bool(keep))
    rig.a.start()
    rig.code = wait_code(rig.a, wait_a)
    print("   host code:", rig.code, flush=True)
    T.check(rig.code is not None, "A: the host's room code is up")
    try:
        if not rig.code:
            T.kill_all()
            return rig
        rig.b = None
        bs = b_script(rig.code) if b_script else join_script(
            rig.code, L.shot(48, "running.png") + L.shot(70, "ended.png") + L.quit_at(76))
        rig.b = T.twin("b", T.arch_b, rom_b or rom, nick[1], bs, config=cfg_b, vita=vita(1, b_vita, rig.pb),
                       net=net_for(1), extra=b_extra, keep=bool(keep))
        if b_noauto:                    # B starts in its game list, not in a game
            os.remove(os.path.join(rig.b.root, "autoboot.txt"))
        rig.b.start()
        threads = []
        for when, fn in actions:
            def run(when=when, fn=fn):
                d = rig.b.t0 + (when + L.SHIFT) * T.k - time.time()
                if d > 0:
                    time.sleep(d)
                try:
                    fn(rig)
                except Exception as e:             # a failing action is a failing check, not a hang
                    T.check(False, "the timed action raised %r" % (e,))
            th = threading.Thread(target=run, daemon=True)
            th.start()
            threads.append(th)
        # (+300 s: a script that waits for the screen (hold=, wait=) runs longer than its frames say)
        rig.ra = rig.a.finish(rig.a.t0 + (a_end + L.SHIFT) * T.k + 300 - time.time())
        rig.rb = rig.b.finish(rig.b.t0 + (b_end + L.SHIFT) * T.k + 300 - time.time())
        rig.st = rig.world.stats() if rig.world else {}
        rig.reqs = rig.svc.requests()
        return rig
    finally:
        T.kill_all()
        for p in rig.proxies:
            p.stop()
        if rig.world:
            rig.world.stop()
        rig.svc.stop()


def solo(T, name, code="ZZZZZ", svc_env=(), b_vita=None, b_cfg=None, before=None, proxy_b=None, tail=None,
         b_end=40, press_join=False, world=False, rom=L.LINKTEST, b_extra=None, script=None, noauto=False):
    """One twin, no host: the joiner types `code` (or a hosting twin if code is None)."""
    T.begin(name)
    rig = Rig()
    rig.T = T
    rig.svc = FakeService(T, *svc_env)
    rig.world = NatWorld(T, "port", "port", lan_port=T.port) if world else None
    rig.proxies = []
    try:
        if before:
            before(rig)
        v = vita_for(1, rig.world, rig.svc) if rig.world else {"online_url": rig.svc.url, "online_stun": "off"}
        if proxy_b:
            p = proxy_b(rig)
            rig.proxies.append(p)
            v["online_url"] = p.url
        v.update(b_vita or {})
        cfg = {"net_conn": 3}
        cfg.update(b_cfg or {})
        if script is not None:
            sb = script
        elif code:
            sb = join_script(code, (tail if tail is not None else "") + L.quit_at(b_end), press_join=press_join)
        else:
            sb = L.host(L.EARLY, "host.png") + (tail or "") + L.quit_at(b_end)
        rig.b = T.twin("b", T.arch_b, rom, "Bob", sb, config=cfg, vita=v, net=net_for(1), extra=b_extra)
        if noauto:
            os.remove(os.path.join(rig.b.root, "autoboot.txt"))
        rig.b.start()
        rig.rb = rig.b.finish(rig.b.t0 + (b_end + L.SHIFT) * T.k + 300 - time.time())
        rig.reqs = rig.svc.requests()
        return rig
    finally:
        T.kill_all()
        for p in rig.proxies:
            p.stop()
        if rig.world:
            rig.world.stop()
        rig.svc.stop()


# ---- shared checks ----------------------------------------------------------------------------

def check_link(T, rig, relay, kind="gba", mode_a=None, delay_min=3, max_reqs=60, pictures=("running.png",)):
    """The link formed through the service, by the way the NAT allowed, and ran its game."""
    ra, rb, st, reqs, code = rig.ra, rig.rb, rig.st, rig.reqs, rig.code
    T.clean(ra, "A")
    T.clean(rb, "B")
    for r, who in ((ra, "A"), (rb, "B")):
        T.check(r.has(r"online_stage from=\d+ to=\d+"), "%s: online stages logged" % who)
    T.check(ra.has(r"online_room code=%s" % code), "A: the room was created (%s)" % code)
    T.check(rb.has(r"online_card state=1 .*compatible=1"), "B: the room card was found and is compatible")
    T.check(rb.has(r"online_peer nick=\"Alice\" mode=%s" % ("relay" if relay else "punch")),
            "B: the join answer says mode %s" % ("relay" if relay else "punch"))
    T.check(ra.has(r"online_peer nick=\"Bob\""), "A: the guest's nickname arrived")
    if relay:
        T.check(rb.has(r"online_relay alloc=") and ra.has(r"online_relay alloc="),
                "both consoles allocated on the relay")
        T.check(st.get("turn", {}).get("allocations", 0) >= 2, "turnsim saw both allocations (%s)" % st.get("turn"))
        T.check(rb.has(r"link_online_up relayed=1") and ra.has(r"online_quality .*relayed=1"),
                "both say the link is relayed")
        # nobody's public address reached the other: the service never gave it out in relay mode and the
        # logs of the two twins hold only relayed addresses
        for r, who, other in ((ra, "A", "203.0.113.11"), (rb, "B", "203.0.113.10")):
            T.check(not any(other in ln for ln in r.log), "%s: the other console's public address (%s) is in no log "
                    "line" % (who, other))
    else:
        T.check(rb.has(r"online_punch done=1") and not rb.has(r"online_relay alloc="),
                "B: the punch won and no relay was used")
        T.check(rb.has(r"link_online_up relayed=0"), "B: link_online_up, direct")
    T.check(rb.has(r"online_quality rtt_ms=\d+ q=\d"), "B: the connect stage ended with a quality line")
    qb = rb.evt("online_quality")
    if qb and qb[-1]["q"] == "0":
        # the badge needs echo answers; over a relay the joiner's first echoes can reach the host's allocation
        # before the host has made its permission for the joiner's relayed address (seen in 1 of 5 relay runs)
        T.S.note("%s: B measured no round trip (q=0, no badge); the link itself formed and ran" % T.S.test)
    else:
        T.check(qb and qb[-1]["q"] in ("1", "2", "3"), "B: the quality was measured")
    T.check(ra.has(r"link_running player=1") and rb.has(r"link_running player=2"),
            "the session reaches 'linked': A Player 1, B Player 2")
    T.check(ra.has(r"online_http op=7 status=200") or any(
        x["path"].endswith("/close") and x["status"] == 200 for x in reqs), "the room was closed on the service (linked)")
    n = len(reqs)
    T.check(n <= max_reqs, "the service saw %d requests (at most %d)" % (n, max_reqs))
    d = ra.evt("link_session_begin")[-1]
    T.check(int(d["delay"]) >= delay_min, "the input delay is at least %d frames (%s, %s)" % (delay_min, d["delay"], d["how"]))
    T.S.note("%s: delay %s (%s), %d service requests" % (T.S.test, d["delay"], d["how"], n))
    for r, who in ((ra, "A"), (rb, "B")):
        T.radio_free(r, who)
        for pn in pictures:
            T.picture(r, who, pn)
    if kind == "gba":
        T.same_machines(ra, rb)
        T.linktest_verdict(ra, "A", 1)
        T.linktest_verdict(rb, "B", 2)


def gb_press(sec=40):
    """A's player presses A (the cartridge's button) a few times once the link is up: that console is the master."""
    return "".join(L.press(sec + 10 * i, "a", 20) for i in range(4))


def gb_b_script(code, end=80):
    """B's side of the cable tests: its clock runs from the moment the link is up (as A's presses do), so A's Disconnect
    comes before B's quit however long each machine took to get there"""
    return join_script(code, "+0:wait=link_running player=2\n" + "+%d:shot=running.png\n" % L.dur(40) +
                       "+%d:shot=ended.png\n" % L.dur(30) + "+%d:quit\n" % L.dur(end - 70))


def gb_script(end):
    # the master's presses come once the link is up (the script waits for it: B's code takes as long as a busy machine
    # takes), not at a guessed time
    return (host_script(end) + "+0:wait=link_running player=1\n" + gb_press(40) + L.shot(60, "running.png") +
            L.disconnect(end - 8, "session.png") + L.shot(end - 4, "ended.png") + L.quit_at(end))


# ---- the scenarios --------------------------------------------------------------------------------

def t_punch(T):
    """Online, GBA cable, port-restricted NATs: the punch wins, direct"""
    check_link(T, pair(T, "online_punch", nat=("port", "port")), relay=False)


def t_relay(T):
    """Online, GBA cable, the host's NAT symmetric: the relay"""
    check_link(T, pair(T, "online_relay", nat=("sym", "port")), relay=True)


def t_relay_joiner(T):
    """Online, GBA cable, the joiner's NAT symmetric: the relay"""
    check_link(T, pair(T, "online_relay_joiner", nat=("port", "sym")), relay=True)


def t_relay_setting_host(T):
    """'Always use the relay' on the host (open NATs): the room goes relay"""
    rig = pair(T, "online_relay_host", nat=("full", "full"), a_cfg={"online_relay": 1})
    check_link(T, rig, relay=True)


def t_relay_setting_joiner(T):
    """'Always use the relay' on the joiner (open NATs): the room goes relay"""
    rig = pair(T, "online_relay_joiner_setting", nat=("full", "full"), b_cfg={"online_relay": 1})
    check_link(T, rig, relay=True)


def t_punch_opens_host(T):
    """The joiner's PUNCH opens the host before the host's next poll (a 10 s poll)"""
    def age(rig):
        # the room is 200 s old for the service: the host's next polls come every 10 s
        advance(rig.svc, 200000)
    # the host's NAT must let an unknown address in (full cone): behind a port-restricted NAT the joiner's PUNCH is
    # dropped until the host has sent to the joiner, and only the poll can open the host
    rig = pair(T, "online_punch_opens_host", nat=("full", "port"),
               actions=[(0, age)])
    # the clock is moved while the joiner starts, i.e. after the host's room exists
    check_link(T, rig, relay=False)
    a = rig.ra
    i_stage = a.index(r"online_stage from=%d to=%d" % (on_stage("WAITING"), on_stage("PUNCH")))
    i_peer = a.index(r"online_peer nick=\"Bob\"")
    T.check(i_stage >= 0 and i_peer >= 0 and i_stage < i_peer,
            "A went from waiting to the punch BEFORE its poll named the guest (line %d < %d)" % (i_stage, i_peer))


def t_gb_punch(T):
    """Online, GB cable over the punch: 200 bytes each way and the verdict"""
    rig = pair(T, "online_gb", rom=L.GBLINK, kind="gb", a_script=gb_script(78), b_script=gb_b_script)
    check_link(T, rig, relay=False, kind="gb")
    for r, who, n in ((rig.ra, "A", 1), (rig.rb, "B", 2)):
        T.check(r.has(r"gblink_done ") and r.has(r"gblink_save_commit .*ok=1"), "%s: agreed end, save committed" % who)
    T.same_machines(rig.ra, rig.rb)
    L.gb_verdict(T, rig.ra, "A", 1)
    L.gb_verdict(T, rig.rb, "B", 2)


def t_gb_relay(T):
    """Online, GB cable over the relay (symmetric host)"""
    rig = pair(T, "online_gb_relay", rom=L.GBLINK, kind="gb", nat=("sym", "port"), a_script=gb_script(78), b_script=gb_b_script)
    check_link(T, rig, relay=True, kind="gb")
    T.same_machines(rig.ra, rig.rb)
    L.gb_verdict(T, rig.ra, "A", 1)
    L.gb_verdict(T, rig.rb, "B", 2)


def t_gbc_punch(T):
    """Online, Game Boy Color cable over the punch: 200 bytes each way and the verdict"""
    rig = pair(T, "online_gbc", rom=L.GBLINK_CGB, kind="gb", a_script=gb_script(78), b_script=gb_b_script)
    check_link(T, rig, relay=False, kind="gb")
    for r, who in ((rig.ra, "A"), (rig.rb, "B")):
        T.check(r.has(r"gblink_done ") and r.has(r"gblink_save_commit .*ok=1"), "%s: agreed end, save committed" % who)
    T.same_machines(rig.ra, rig.rb)
    L.gb_verdict(T, rig.ra, "A", 1, save="gblink-cgb.gbc.sav")
    L.gb_verdict(T, rig.rb, "B", 2, save="gblink-cgb.gbc.sav")


def t_gbc_relay(T):
    """Online, Game Boy Color cable over the relay (symmetric host)"""
    rig = pair(T, "online_gbc_relay", rom=L.GBLINK_CGB, kind="gb", nat=("sym", "port"), a_script=gb_script(78), b_script=gb_b_script)
    check_link(T, rig, relay=True, kind="gb")
    T.same_machines(rig.ra, rig.rb)
    L.gb_verdict(T, rig.ra, "A", 1, save="gblink-cgb.gbc.sav")
    L.gb_verdict(T, rig.rb, "B", 2, save="gblink-cgb.gbc.sav")


def t_adapter(T):
    """Online, GBA Wireless Adapter: the room, an idle link, teardown (no adapter traffic exists)"""
    lm = {"roms/linkmode.txt": "linktest.gba = adapter\n"}
    a = (host_script(70) + L.shot(40, "linked.png") + L.press(41, "cross") + L.shot(43, "game.png") +
         L.disconnect(62) + L.shot(66, "after.png") + L.quit_at(70))
    b = lambda code: join_script(code, L.shot(36, "joined.png") + L.disconnect(48) + L.shot(54, "after.png") +
                                 L.quit_at(70), card_shot="card.png")
    rig = pair(T, "online_adapter", a_script=a, b_script=b, a_extra=lm, b_extra=lm, a_end=70, b_end=70)
    ra, rb = rig.ra, rig.rb
    T.clean(ra, "A")
    T.clean(rb, "B")
    c = rb.evt("online_card")
    T.check(c and c[-1]["kind"] == "0" and c[-1]["compatible"] == "1", "B's card says Wireless Adapter")
    T.check(ra.has(r"net_up role=host proto=\"gpSP") and rb.has(r"net_up role=join proto=\"gpSP"),
            "both run the core's own adapter protocol over netdrv")
    T.check(rb.has(r"session_start id=1") and ra.has(r"peer_connected id=1"), "the joiner is in the host's session")
    T.check(rb.has(r"link_disconnect") and ra.has(r"peer_disconnected id=1"),
            "the joiner leaves and the host sees it")
    for r, who in ((ra, "A"), (rb, "B")):
        T.radio_free(r, who)
        T.check(r.evt("link_cheats")[-1]["suspended"] == "0", "%s: cheats are back afterwards" % who)
    T.check(rb.has(r"online_peer nick=\"Alice\" mode=punch") and rb.has(r"online_punch done=1"),
            "B: the room was entered by the punch")
    for r, who, names in ((ra, "A", ("host.png", "linked.png", "after.png")), (rb, "B", ("joined.png", "after.png"))):
        for n in names:
            T.picture(r, who, n)
    T.S.note("Wireless Adapter: bring-up, an idle link and teardown only -- no adapter traffic crossed")


def t_share(T):
    """Share game data over Online: A shares (code on its page), B types the code and takes A's save"""
    T.begin("online_share")
    port = T.port
    svc = FakeService(T)
    world = NatWorld(T, "port", "port", lan_port=port)
    va, vb = vita_for(0, world, svc), vita_for(1, world, svc)
    sav = bytes((i * 7 + 3) & 0xFF for i in range(32768))
    down7 = L.keys(*(["down"] * 15))
    k = T.k
    try:
        a = T.twin("a", T.arch_a, L.LINKTEST, "Alice",
                   L.press(2, "start") + down7 + "+20:shot=settings.png\n" +
                   L.keys("cross", first=L.AFTER_SHOT) + L.shot(8, "sharing.png") +
                   L.shot(60, "shared.png") + L.press(78, "circle") + L.shot(80, "after.png") + L.quit_at(84),
                   config={"net_conn": 3}, vita=va, net=net_for(0), extra={"roms/linktest.sav": sav})
        os.remove(os.path.join(a.root, "autoboot.txt"))
        a.start()
        code = wait_code(a)
        print("   share code:", code, flush=True)
        T.check(code is not None, "A: the sharer's room code is up")
        if not code:
            return
        b = T.twin("b", T.arch_b, L.LINKTEST, "Bob",
                   L.press(3, "start") + down7 + L.keys("down") + "+20:shot=settings.png\n" +
                   L.keys("cross", first=L.AFTER_SHOT) + "+30:shot=pad.png\n" +
                   L.keys(*code_keys(code)) + L.keys("triangle") +
                   "+%d:shot=list.png\n" % L.dur(20) + "+30:cross:%d\n" % L.PRESS +
                   "+%d:shot=their.png\n" % L.dur(3) + "+30:cross:%d\n" % L.PRESS +
                   "+%d:shot=done.png\n" % L.dur(8) + "+30:circle:%d\n" % L.PRESS + "+30:circle:%d\n" % L.PRESS +
                   "+%d:shot=after.png\n" % L.dur(2) + "+%d:quit\n" % L.dur(3),
                   config={"net_conn": 3}, vita=vb, net=net_for(1))
        os.remove(os.path.join(b.root, "autoboot.txt"))
        b.start()
        ra = a.finish(a.t0 + (84 + L.SHIFT) * k + 150 - time.time())
        rb = b.finish(b.t0 + 150 * k + 150 - time.time())
    finally:
        T.kill_all()
        world.stop()
        svc.stop()
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"share_net_up host=1 online=1 rc=0"), "A opens the Online share room")
    T.check(ra.has(r"share_host_up .*saves=1"), "A shares one save")
    T.check(rb.has(r"share_net_up host=0 online=1 rc=0"), "B joined by the code")
    T.check(rb.has(r"share_join_welcome"), "B is welcomed")
    got = os.path.join(rb.root, "roms", "linktest.sav")
    data = open(got, "rb").read() if os.path.exists(got) else b""
    T.check(data == sav, "B has A's save, byte for byte (%d bytes)" % len(data))
    T.check(not any(n.endswith(".gsp") for n in os.listdir(os.path.join(rb.root, "roms"))),
            "no temporary file left behind")
    for r, who, names in ((ra, "A", ("sharing.png",)), (rb, "B", ("pad.png", "list.png", "done.png"))):
        for n in names:
            T.picture(r, who, n)


# ---- failures -------------------------------------------------------------------------------

def card_failure(T, rig, want, who="B"):
    """The card (or the join) failed with UI_FAIL_<want>, said so in the log, and showed a picture."""
    r = rig.rb
    T.clean(r, who)
    bad = r.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail(want),
            "%s: the card reads UI_FAIL_%s (%s)" % (who, want, bad[-1]["_"] if bad else "no online_card_bad"))
    T.check(not r.has(r"online_stage from=\d+ to=%d" % on_stage("JOIN")), "%s: no join was attempted" % who)
    T.check(not r.has(r"link_running"), "%s: no link formed" % who)
    T.radio_free(r, who) if r.evt("link_net_down") else None
    T.picture(r, who, "card.png")


def t_wrong_code(T):
    """A code nobody opened: 'That code is wrong or has expired.'"""
    rig = solo(T, "online_wrong_code", code="ZZZZZ", tail=L.shot(10, "card.png") if False else "")
    r = rig.rb
    T.clean(r, "B")
    c = r.evt("online_card")
    T.check(any(x.get("state") == "2" for x in c), "B: the card ended in a failure")
    bad = r.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_CODE"), "B: the card reads 'wrong or expired' (%s)" %
            (bad[-1]["_"] if bad else "none"))
    T.check(any(x["status"] == 404 for x in rig.reqs), "the service answered 404 for it")
    T.check(len([x for x in rig.reqs if x["path"].startswith("/v1/rooms/")]) <= 6, "B did not hammer the service (%d requests)" %
            len(rig.reqs))


def t_expired(T):
    """A code that was good and has aged out (the service's clock moved 11 minutes)"""
    def age(rig):
        advance(rig.svc, 660000)
    rig = pair(T, "online_expired", b_script=lambda code: join_script(code, L.quit_at(40), press_join=False),
               a_script=host_script(60, shots=(12,)) + L.shot(50, "late.png") + L.quit_at(60),
               setup=None, a_end=60, b_end=40, actions=[(-5, age)])
    # the action runs 5 script seconds before B's start is over: the clock is moved while B types
    rb, ra = rig.rb, rig.ra
    T.clean(rb, "B")
    T.clean(ra, "A")
    bad = rb.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_CODE"), "B: 'wrong or expired' (%s)" % (bad[-1]["_"] if bad else "none"))
    T.check(ra.has(r"online_fail why=%d" % online_fail("EXPIRED")) or ra.has(r"link_online_failed .*host=1"),
            "A: the host's own room is reported expired")


def t_update_required(T):
    """A service that wants a newer app (426): both consoles say 'too old' and stop"""
    rig = solo(T, "online_update_required", code="ZZZZZ", svc_env=("MIN_APP_VERSION=2.9.0", "LATEST_APP_VERSION=2.9.0"))
    r = rig.rb
    T.clean(r, "B")
    bad = r.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_UPDATE"), "B: 'too old for online play' (%s)" % (bad[-1]["_"] if bad else "none"))
    rig = solo(T, "online_update_required_host", code=None, svc_env=("MIN_APP_VERSION=2.9.0", "LATEST_APP_VERSION=2.9.0"),
               tail=L.shot(14, "failed.png"), b_end=20)
    r = rig.rb
    T.clean(r, "A")
    f = r.evt("link_online_failed")
    T.check(f and int(f[-1]["ui"]) == ui_fail("ONLINE_UPDATE"), "A: the host's try reads 'too old' (%s)" % (f[-1]["_"] if f else "none"))
    T.picture(r, "A", "failed.png")


def _card_ver(newer):
    ver = "2.1.0" if newer else "1.9.0"

    def edit(method, path, status, obj):
        if method == "GET" and re.search(r"/v1/rooms/[A-Z0-9]{5}$", path.split("?")[0]) and "card" not in obj and "ver" in obj:
            obj["ver"] = ver
            return obj
        return None
    return edit


def t_version(T):
    """Version mismatch: the host newer, the host older, same version with another setup: who must update"""
    for tag, edit, want, line in (
            ("newer", _card_ver(True), "ONLINE_VER_OLD_HERE", "the host runs a newer VitAdhoc"),
            ("older", _card_ver(False), "ONLINE_VER_OLD_THERE", "the host runs an older VitAdhoc")):
        rig = pair(T, "online_version_" + tag, with_world=False, a_end=45, b_end=40,
                   a_script=host_script(45) + L.quit_at(45),
                   b_script=lambda code: join_script(code, L.quit_at(40), press_join=False),
                   proxy_b=lambda rig, e=edit: HttpProxy(rig.T, rig.svc, edit=e))
        T.clean(rig.rb, "B " + tag)
        T.clean(rig.ra, "A " + tag)
        bad = rig.rb.evt("online_card_bad")
        T.check(bad and int(bad[-1]["why"]) == ui_fail(want), "B (%s): UI_FAIL_%s (%s)" % (line, want, bad[-1]["_"] if bad else "none"))
        T.check(not any(x["path"].endswith("/join") for x in rig.reqs), "(%s) no join request was made" % tag)
        T.check(not rig.ra.has(r"online_peer"), "(%s) the host never saw a guest" % tag)
        T.picture(rig.rb, "B", "card.png")
    # the same release, another BIOS / CPU setup: link_build makes the compat word differ
    rig = pair(T, "online_version_setup", with_world=False, a_end=45, b_end=40, b_vita={"link_build": "setup-x"},
               a_script=host_script(45) + L.quit_at(45),
               b_script=lambda code: join_script(code, L.quit_at(40), press_join=False))
    T.clean(rig.rb, "B setup")
    T.clean(rig.ra, "A setup")
    bad = rig.rb.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_SETUP"), "B (same version, other setup): UI_FAIL_ONLINE_SETUP (%s)" %
            (bad[-1]["_"] if bad else "none"))
    T.check(not any(x["path"].endswith("/join") for x in rig.reqs), "(setup) no join request was made")


def t_different_game(T):
    """A cable game whose file differs from the host's, joined from Play together: the session's own check has the last word"""
    # B's copy: the same header code, one byte changed deep in the file, so the file's identity (rom) differs
    other = bytearray(open(L.LINKTEST, "rb").read())
    other[len(other) // 2] ^= 0x5A
    rig = pair(T, "online_different_game", b_extra={"roms/linktest.gba": bytes(other)}, with_world=False,
               a_end=70, b_end=64, a_script=host_script(70) + L.shot(50, "late.png") + L.quit_at(70),
               b_script=lambda code: join_script(code, L.shot(30, "after.png") + L.quit_at(64), press_join=True))
    T.clean(rig.rb, "B")
    T.clean(rig.ra, "A")
    c = rig.rb.evt("online_card")
    T.check(c and c[-1].get("compatible") == "1", "B: the card is compatible (same build)")
    # the room card goes by the game's code; the files are compared by the cable session itself (HELLO / NEED):
    # the outcome (a refusal, or the host's file carried over) must be a clean one on both consoles
    T.check(rig.rb.has(r"link_running") or rig.rb.has(r"gblink_(refused|end|need)|link_ended|link_failed|link_fail_screen"),
            "B: the session said what it made of another file")


# ---- the service's limits ------------------------------------------------------------------------

def t_busy(T):
    """The service's day budget is spent: 'Online play is busy for today.' (join and host)"""
    def spend(rig):
        for i in range(8):
            svc_call(rig.svc, "GET", "/v1/info?v=2.0.0")
            svc_call(rig.svc, "GET", "/v1/rooms/AAAAA?v=2.0.0")
    rig = solo(T, "online_busy_join", code="ZZZZZ", svc_env=("DAILY_REQUEST_BUDGET=6",), before=spend)
    r = rig.rb
    T.clean(r, "B")
    bad = r.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_BUSY"), "B: 'busy for today' on the card (%s)" % (bad[-1]["_"] if bad else "none"))
    T.check(any(x["status"] == 503 and x["code"] == "over_budget" for x in rig.reqs), "the service answered 503 over_budget")
    rig = solo(T, "online_busy_host", code=None, svc_env=("DAILY_REQUEST_BUDGET=6",), before=spend, tail=L.shot(14, "failed.png"), b_end=20)
    r = rig.rb
    T.clean(r, "A")
    f = r.evt("link_online_failed")
    T.check(f and int(f[-1]["ui"]) == ui_fail("ONLINE_BUSY"), "A: the host's try reads 'busy' (%s)" % (f[-1]["_"] if f else "none"))
    T.picture(r, "A", "failed.png")


DESC = {"kind": 1, "console": "gba", "game": "TEST", "rom": "", "title": "Test", "nick": "Tester", "ver": "2.0.0",
        "compat": "00000000", "pub": "203.0.113.50:19019", "lan": "", "sym": 0, "relayOnly": 0}


def known_bug(T, cond, what, why):
    """A check of what the app SHOULD do where the app is known not to (reported to the coordinator): a failure
    is printed as a note and does not fail the run, unless ONLINE_TESTS_STRICT=1; when the app is fixed it is an
    ordinary check."""
    if cond or os.environ.get("ONLINE_TESTS_STRICT") == "1":
        return T.check(cond, what)
    T.S.note("KNOWN PRODUCT BUG, not counted: %s (%s)" % (what, why))
    return False


def t_slow_down(T):
    """Too many tries from this network (a short 429): its own sentence, not 'busy'"""
    def spend_lookup(rig):
        # one token in the lookup bucket and one more only after 110 s: the joiner's lookup (a minute or so
        # later) is told to slow down, and the wait it is given is short
        svc_call(rig.svc, "GET", "/v1/rooms/AAAAA?v=2.0.0")
    env = ("RATE_LIMIT_SCALE=1", "LOOKUP_BURST=1", "LOOKUP_EVERY_SEC=110", "CREATE_BURST=1", "CREATE_EVERY_SEC=110")
    rig = solo(T, "online_slow_down", code="ZZZZZ", svc_env=env, before=spend_lookup)
    r = rig.rb
    T.clean(r, "B")
    T.check(any(x["status"] == 429 for x in rig.reqs), "the service answered the joiner's lookup 429")
    bad = r.evt("online_card_bad")
    known_bug(T, bad and int(bad[-1]["why"]) == ui_fail("ONLINE_SLOW"),
              "B: the card's 429 (retryAfterSec <= 120) reads the short sentence (%s)" % (bad[-1]["_"] if bad else "none"),
              "link_vita.c card_from_reply maps every ONLINE_E_RATE to UI_FAIL_ONLINE_BUSY and drops retry_after")

    def spend_create(rig):
        svc_call(rig.svc, "POST", "/v1/rooms?v=2.0.0", DESC)
    rig = solo(T, "online_slow_down_host", code=None, svc_env=env, before=spend_create, tail=L.shot(14, "failed.png"), b_end=20)
    r = rig.rb
    T.clean(r, "A")
    T.check(any(x["status"] == 429 for x in rig.reqs), "the service answered the host's create 429")
    f = r.evt("link_online_failed")
    T.check(f and int(f[-1]["ui"]) == ui_fail("ONLINE_SLOW"), "A: the host's 429 reads the short sentence (%s)" %
            (f[-1]["_"] if f else "none"))
    T.picture(r, "A", "failed.png")


def t_service_down(T):
    """Nothing answers at the service's address: 'The online service could not be reached.'"""
    rig = solo(T, "online_service_down", code="ZZZZZ", b_vita={"online_url": "http://127.0.0.1:1"}, b_end=60)
    r = rig.rb
    T.clean(r, "B")
    bad = r.evt("online_card_bad")
    c = r.evt("online_lookup")
    T.check((bad and int(bad[-1]["why"]) == ui_fail("ONLINE_SERVICE")) or (c and int(c[-1]["rc"]) < 0),
            "B: 'could not be reached' (%s)" % ((bad or c or [{"_": "none"}])[-1]["_"]))


# ---- losses, cancels ----------------------------------------------------------------------------

def t_lost_answer(T):
    """The join's first answer is lost on the way: the request id makes the retry succeed (and the create's too)"""
    def drop_join(method, path, n):
        if method == "POST" and path.split("?")[0].endswith("/join") and n == 1:
            return ("drop_answer",)
        return None

    def drop_create(method, path, n):
        if method == "POST" and path.split("?")[0] == "/v1/rooms" and n == 1:
            return ("drop_answer",)
        return None
    rig = pair(T, "online_lost_answer", nat=("port", "port"),
               proxy_a=lambda rig: HttpProxy(rig.T, rig.svc, rule=drop_create),
               proxy_b=lambda rig: HttpProxy(rig.T, rig.svc, rule=drop_join))
    check_link(T, rig, relay=False, max_reqs=50)
    joins = [x for x in rig.reqs if x["path"].endswith("/join")]
    creates = [x for x in rig.reqs if x["method"] == "POST" and x["path"] == "/v1/rooms"]
    T.check(len(joins) == 2 and all(j["status"] == 200 for j in joins),
            "the service saw the join twice and answered both 200 (%s)" % [(j["status"], j["code"]) for j in joins])
    T.check(len(creates) == 2 and all(j["status"] in (200, 201) for j in creates),
            "the service saw the create twice and answered both (%s)" % [(j["status"], j["code"]) for j in creates])
    T.check(len([1 for p in rig.proxies for s in p.seen if "lost" in s[2]]) == 2, "the proxies lost exactly those two answers")


def t_cancel_after_failure(T):
    """A join that fails at the service, then Cancel at once: back to the menus, nothing left running"""
    def not_found(method, path, n):
        if method == "POST" and path.split("?")[0].endswith("/join"):
            return ("answer", 404, {"error": {"code": "not_found", "message": "no such room"}})
        return None
    rig = pair(T, "online_cancel_failure", nat=("port", "port"), a_end=60, b_end=50,
               a_script=host_script(60) + L.shot(40, "late.png") + L.quit_at(60),
               b_script=lambda code: join_script(code, "+%d:shot=failed.png\n" % L.dur(8) + L.keys("circle") +
                                                 L.shot(48, "after.png") + L.quit_at(50)),
               proxy_b=lambda rig: HttpProxy(rig.T, rig.svc, rule=not_found))
    T.clean(rig.rb, "B")
    T.clean(rig.ra, "A")
    f = rig.rb.evt("link_online_failed")
    T.check(f and int(f[-1]["ui"]) == ui_fail("ONLINE_CODE"), "B: the join's own 404 reads 'wrong or expired' (%s)" %
            (f[-1]["_"] if f else "none"))
    T.check(rig.rb.has(r"link_failed code=%d host=0" % ui_fail("ONLINE_CODE")), "B: the join failed into the LINKING screen's failure page")
    T.radio_free(rig.rb, "B")
    T.picture(rig.rb, "B", "failed.png")
    T.picture(rig.rb, "B", "after.png")
    T.check(not rig.rb.has(r"link_running"), "B never linked")
    sh = rig.rb.shot_evt("after.png")
    T.check(sh and int(sh["emu"]) > 0,
            "B: Cancel brought the game back (it runs after the page)")


def t_drop_midlink(T):
    """The path dies in the middle of the game: the link ends, each game carries on"""
    def cut(rig):
        rig.world.command("down 0")
        rig.world.command("down 1")
        rig.cut_at = time.time()
    rig = pair(T, "online_drop", nat=("port", "port"), a_end=130, b_end=118,
               a_script=host_script(130) + L.shot(60, "running.png") + L.shot(100, "waiting_for.png") +
               L.shot(124, "after.png") + L.quit_at(130),
               b_script=lambda code: join_script(code, L.shot(60, "waiting_for.png") + L.shot(112, "after.png") +
                                                 L.quit_at(118), hold=False),
               actions=[(40, cut)])
    for r, who in ((rig.ra, "A"), (rig.rb, "B")):
        T.clean(r, who)
        T.check(r.has(r"link_running"), "%s: the link had been running" % who)
        T.check(r.has(r"link_ended .*ran=1 err=peer_lost"), "%s: the link ended, peer lost" % who)
        T.check(r.has(r"gblink_resume how=carry_on rc=0"), "%s: its game carries on from where it stands" % who)
        T.radio_free(r, who)
        T.picture(r, who, "after.png")
        sh = r.shot_evt("after.png")
        T.check(sh and int(sh["emu"]) > 0, "%s: the game goes on after the link ended" % who)


# ---- delay: round trips of 100 / 150 / 200 ms ----------------------------------------------------------

# The plan's estimate was 6..7 / 8..9 / 9..11 frames.  The measured trip's minimum and maximum are what
# delay_pick uses, so the number depends on the jitter that happened to be drawn: the floor here is the
# one-way trip alone (1 + ceil(rtt / 2 / 16.7 ms)), the ceiling is the Online cap.
RTTS = {r: (1 + -(-(r * 1000 // 2) // 16667), 12) for r in (100, 150, 200)}


def rtt_run(T, rtt, how):
    """Both consoles' datagrams are delayed by rtt/2 (+ jitter, loss) by natsim ('proxy') or by the transport's own shim."""
    half = rtt // 2
    name = "online_rtt_%d_%s" % (rtt, how)
    if how == "proxy":
        rig = pair(T, name, nat=("port", "port"), world_kw={"delay": half, "jitter": 10, "loss": 2}, a_end=90, b_end=88,
                   a_script=host_script(90) + L.shot(70, "running.png") + L.disconnect(80, "session.png") +
                   L.shot(84, "ended.png") + L.quit_at(90),
                   b_script=lambda code: join_script(code, L.shot(60, "running.png") + L.shot(82, "ended.png") +
                                                     L.quit_at(88)))
    else:
        fault = {"link_latency": half, "link_jitter": 10, "link_loss": 2}
        rig = pair(T, name, nat=("port", "port"), a_vita=fault, b_vita=fault, a_end=90, b_end=88,
                   a_script=host_script(90) + L.shot(70, "running.png") + L.disconnect(80, "session.png") +
                   L.shot(84, "ended.png") + L.quit_at(90),
                   b_script=lambda code: join_script(code, L.shot(60, "running.png") + L.shot(82, "ended.png") +
                                                     L.quit_at(88)))
    return rig


def check_rtt(T, rig, rtt, how):
    lo, hi = RTTS[rtt]
    T.clean(rig.ra, "A")
    T.clean(rig.rb, "B")
    T.check(rig.ra.has(r"link_running player=1") and rig.rb.has(r"link_running player=2"), "linked: A Player 1, B Player 2")
    d = rig.ra.evt("link_session_begin")[-1]
    rt = rig.ra.evt("link_rtt")
    want = int(rt[-1]["want"]) if rt else -1
    delay = int(d["delay"])
    T.check(d["how"] == "measured" and rt, "the delay was measured (%s)" % d["_"])
    T.check(delay <= 12, "the delay is within the Online cap of 12 (%d, want=%d)" % (delay, want))
    T.check(lo <= delay <= hi and want >= lo,
            "the delay for %d ms round trip covers the one-way trip: %d..%d frames (picked %d, want=%d)" %
            (rtt, lo, hi, delay, want))
    T.same_machines(rig.ra, rig.rb)
    T.linktest_verdict(rig.ra, "A", 1, loops=0)
    T.linktest_verdict(rig.rb, "B", 2, loops=0)
    q = rig.rb.evt("online_quality")
    sa, sb = rig.ra.evt("gblink_stats")[-1], rig.rb.evt("gblink_stats")[-1]
    T.S.note("%s ms round trip (%s): delay %d (want %d), badge q=%s, stalls A %s B %s, stalls/min %.1f" %
             (rtt, how, delay, want, q[-1]["q"] if q else "?", sa["stalls"], sb["stalls"],
              (int(sa["stalls"]) + int(sb["stalls"])) / 2 / max(0.1, int(sa["f0"]) / 3600.0)))
    for r, who in ((rig.ra, "A"), (rig.rb, "B")):
        T.radio_free(r, who)
        T.picture(r, who, "running.png")


def t_rtt(T, rtt):
    for how in ("proxy", "shim"):
        check_rtt(T, rtt_run(T, rtt, how), rtt, how)


def t_rtt_100(T):
    """Online at 100 ms round trip, jitter 20, loss 2%: delay picked within the cap, machines equal"""
    t_rtt(T, 100)


def t_rtt_150(T):
    """Online at 150 ms round trip: the delay asks for 8-9 and gets it"""
    t_rtt(T, 150)


def t_rtt_200(T):
    """Online at 200 ms round trip: the delay asks for 9-11 and gets it (the cap is 12)"""
    t_rtt(T, 200)


def t_delay_cap(T):
    """The same long delay (150 ms each way) on Same Wi-Fi's shim clamps at 8: the old cap is untouched"""
    T.begin("online_delay_cap")
    fault = {"link_latency": 150, "link_jitter": 20, "link_loss": 2, "link_peek": L.LT_PEEK}
    a = T.twin("a", T.arch_a, L.LINKTEST, "Alice", L.host(L.EARLY) + L.shot(50, "running.png") + L.disconnect(60) +
               L.shot(64, "after.png") + L.quit_at(66), vita=fault)
    b = T.twin("b", T.arch_b, L.LINKTEST, "Bob", L.join_first(L.EARLY, wait=2.5) + L.shot(50, "running.png") +
               L.quit_at(66), vita=fault)
    ra, rb = T.run([a, b], 66)
    T.clean(ra, "A")
    T.clean(rb, "B")
    d = ra.evt("link_session_begin")[-1]
    rt = ra.evt("link_rtt")
    T.check(rt and int(rt[-1]["want"]) > 8, "Wi-Fi: the measurement asks for more than 8 (want=%s)" % (rt[-1]["want"] if rt else "?"))
    T.check(d["how"] == "measured" and int(d["delay"]) == 8, "Wi-Fi: the delay is held at 8, the old cap (%s)" % d["_"])
    T.same_machines(ra, rb)


# ---- determinism ----------------------------------------------------------------------------------

def private_cable_rom():
    """The first ROM of $GATE_GBA_ROMS whose game the link table marks Link Cable (None without)."""
    d = os.environ.get("GATE_GBA_ROMS", "")
    if not d or not os.path.isdir(d):
        return None
    cable = set(re.findall(r"\{\s*\{'(.)','(.)','(.)'\}\s*,\s*LINK_CABLE", open(LINK_GAMES_H).read()))
    cable = {"".join(c) for c in cable}
    for f in sorted(os.listdir(d)):
        if not f.lower().endswith(".gba"):
            continue
        with open(os.path.join(d, f), "rb") as fh:
            fh.seek(0xAC)
            code = fh.read(3).decode("latin-1")
        if code in cable:
            return os.path.join(d, f)
    return None


def pattern(a_first):
    """A scripted button pattern for 80 s: a different one for each console."""
    s = ""
    t = 30
    btn = ["a", "b", "start", "right", "left", "up", "down", "a"]
    i = 0 if a_first else 3
    while t < 100:
        s += L.press(t, btn[i % len(btn)], 14)
        t += 1.7 + (i % 3) * 0.4
        i += 1
    return s


def determ(T, rom, tag):
    rtt = 150
    fault = {"delay": rtt // 2, "jitter": 10, "loss": 2}
    rig = pair(T, "online_determinism_" + tag, rom=rom, nat=("port", "port"), world_kw=fault, a_end=140, b_end=138,
               a_script=host_script(140) + pattern(True) + L.shot(110, "running.png") + L.disconnect(125, "session.png") +
               L.shot(130, "ended.png") + L.quit_at(140),
               b_script=lambda code: join_script(code, pattern(False) + L.shot(108, "running.png") +
                                                 L.shot(132, "ended.png") + L.quit_at(138)),
               peek=L.LT_PEEK if tag == "linktest" else "02000000:4")
    T.clean(rig.ra, "A")
    T.clean(rig.rb, "B")
    T.check(rig.ra.has(r"link_running player=1") and rig.rb.has(r"link_running player=2"), "linked (%s)" % tag)
    T.same_machines(rig.ra, rig.rb)
    ma, sa = T.hashes(rig.ra)
    T.check(sa >= 8, "%d periodic hashes were exchanged on the 150 ms link" % sa)
    d = rig.ra.evt("link_session_begin")[-1]
    T.S.note("determinism (%s): delay %s, %d/%d periodic hashes equal, final %s" %
             (tag, d["delay"], ma, sa, (rig.ra.evt("gblink_final") or [{"hash": "-"}])[-1]["hash"]))


def t_determinism(T):
    """150 ms round trip + jitter + loss, scripted buttons on both: every hash agrees (linktest, and a private cable game)"""
    determ(T, L.LINKTEST, "linktest")
    rom = private_cable_rom()
    if rom:
        determ(T, rom, "private")
    else:
        print("   SKIP the private cable game: GATE_GBA_ROMS has none (or is unset)")


# ---- the library path ----------------------------------------------------------------------------------

def settings_join_online():
    """From the game list: START opens Settings, DOWN from the Theme row it opens on to "Join online" (SET_JOIN_ONLINE = row 10,
    browser Settings only).  Every step waits for the row's own log line (settings_row) and the last names the row it must
    land on, so a press a busy machine dropped or doubled, or a row added to the list, cannot send the walk elsewhere."""
    downs = "".join("+%d:hold=down=settings_row row=%s\n" % (L.GAP, "10" if i == 17 else "") for i in range(18))
    return L.press(2, "start") + downs + "+20:shot=settings.png\n"


def lib_b_script(code, tail, shots=("card.png",), press=True):
    keys = code_keys(code)
    half = keys.index("right", keys.index("right") + 1) + 1
    s = (settings_join_online() + L.keys("cross", first=L.AFTER_SHOT) + "+30:shot=pad_empty.png\n" +
         L.keys(*keys[:half], first=L.AFTER_SHOT) + "+30:shot=pad_half.png\n" +
         L.keys(*keys[half:], first=L.AFTER_SHOT) + L.keys("triangle") + "+%d:shot=card.png\n" % L.dur(8))
    return s + tail


def t_library_join(T):
    """Join online from the game list: the matching game opens and joins; a cable game with another file asks"""
    src = open(L.LINKTEST, "rb").read()
    other = bytearray(src)
    other[len(other) // 2] ^= 0x5A
    # 1. B has the host's file: the card offers it directly, X opens it and joins
    def b_open(code):
        return lib_b_script(code, L.keys("cross", first=L.AFTER_SHOT) + "+%d:shot=linking.png\n" % L.dur(6) +
                            L.shot(80, "running.png") + L.shot(100, "ended.png") + L.quit_at(106))
    rig = pair(T, "online_library_open", b_script=b_open, b_noauto=True, a_end=110, b_end=106,
               a_script=host_script(110) + L.shot(80, "running.png") + L.disconnect(94, "session.png") +
               L.shot(100, "ended.png") + L.quit_at(110))
    for r in (rig.ra, rig.rb):
        pass
    os.path.exists(os.path.join(rig.b.root, "autoboot.txt")) if rig.b else None
    T.clean(rig.ra, "A")
    T.clean(rig.rb, "B")
    rb = rig.rb
    T.check(rb.has(r"online_lib_done hits=1 pick=\d+") and rb.has(r"online_open_join rom_match=1"),
            "B: the library found the host's file and opens it (%s)" % ([x["_"] for x in rb.evt("online_lib_done")][-1:]))
    T.check(rb.has(r"link_running player=2") and rig.ra.has(r"link_running player=1"), "the opened game joined and linked")
    T.check(not rb.has(r"online_lib_done .*pick=%d" % 99), "nothing was decided without a match")
    T.linktest_verdict(rig.ra, "A", 1, loops=0)
    T.linktest_verdict(rb, "B", 2, loops=0)
    T.same_machines(rig.ra, rb)
    T.picture(rb, "B", "card.png")
    # 2. B has only a different file with the host's game code: it asks, shows the file, never opens by itself
    def b_ask(code):
        return lib_b_script(code, "+%d:shot=asked.png\n" % L.dur(6) + L.keys("circle", "circle", first=L.AFTER_SHOT) +
                            "+%d:quit\n" % L.dur(3))
    rig = pair(T, "online_library_other_file", b_script=b_ask, b_noauto=True, a_end=100, b_end=100, rom_b=None,
               b_extra={"roms/linktest.gba": bytes(other)},
               a_script=host_script(100) + L.quit_at(100), with_world=False)
    rb = rig.rb
    T.clean(rig.rb, "B")
    T.clean(rig.ra, "A")
    done = rb.evt("online_lib_done")
    T.check(done and done[-1]["cable"] == "1" and done[-1]["hits"] == "1" and int(done[-1]["pick"]) == 2,
            "B: one file with the game's code was found and the page asks (pick=ASK) (%s)" % (done[-1]["_"] if done else "none"))
    T.check(not rb.has(r"online_open_join") and not rb.has(r"link_running"), "B: nothing was opened by itself")
    T.picture(rb, "B", "asked.png")
    # 3. B has the other file AND the host's: the host's is offered directly (the other is ignored)
    def b_both(code):
        return lib_b_script(code, "+%d:shot=search.png\n" % L.dur(5) + L.keys("cross", first=L.AFTER_SHOT) +
                            "+%d:shot=linking.png\n" % L.dur(6) + "+%d:shot=after.png\n" % L.dur(20) +
                            "+%d:quit\n" % L.dur(2))
    rig = pair(T, "online_library_both_files", b_script=b_both, b_noauto=True, a_end=110, b_end=110,
               b_extra={"roms/zz_other/linktest.gba": bytes(other)},
               a_script=host_script(110) + L.quit_at(110))
    rb = rig.rb
    T.clean(rig.rb, "B")
    T.clean(rig.ra, "A")
    T.check(rb.has(r"online_open_join rom_match=1"), "B: with the host's file among two, it is the one that opens")



# ---- Two players Online: a NES game here, a SNES game handed to snes.bin -----------------------------

def t_nes_relay(T):
    """Online, Two players on a NES game through the relay: one machine on both consoles, in step"""
    pad_online(T, "online_nes_relay", ("sym", "port"), True, "nes")


def t_nes_punch(T):
    """Online, Two players on a NES game over the direct punch"""
    pad_online(T, "online_nes_punch", ("port", "port"), False, "nes")


def t_ws_relay(T):
    """Online, Two players on a WonderSwan game through the relay: one machine on both consoles, in step"""
    pad_online(T, "online_ws_relay", ("sym", "port"), True, "ws")


def t_ws_punch(T):
    """Online, Two players on a WonderSwan game over the direct punch"""
    pad_online(T, "online_ws_punch", ("port", "port"), False, "ws")


PAD_KINDS = {"nes": ("NES", "kind=5", "GATE_NES_ROMS (nes15-NTSC.nes) or VITADHOC_NES_ROM"),
             "ws": ("WonderSwan", "kind=5", "GATE_WS_ROM")}


def pad_online(T, name, nat, relay, fam):
    rom = L.nes_rom() if fam == "nes" else L.ws_rom()
    if not rom:
        T.S.note("no %s game (%s): skipped" % (PAD_KINDS[fam][0], PAD_KINDS[fam][2]))
        return
    keys = ["a", "right", "down", "start", "left", "b", "up", "a"]
    pr = "".join(L.press(40 + i, k, 12) for i, k in enumerate(keys))
    rig = pair(T, name, rom=rom, nat=nat, kind="nes",
               a_script=host_script(78) + "+0:wait=link_running player=1\n" + pr + L.shot(60, "running.png") +
               L.disconnect(70, "session.png") + L.shot(74, "ended.png") + L.quit_at(78), b_script=gb_b_script)
    if not rig.ra or not rig.rb:
        return
    T.clean(rig.ra, "A")
    T.clean(rig.rb, "B")
    T.check(rig.rb.has(r"online_card state=1 .*kind=5 .*compatible=1"), "B: the card is a Two players room it can join")
    T.check(rig.rb.has(r"link_online_up relayed=%d" % (1 if relay else 0)), "B: the link is %s" % ("relayed" if relay else "direct"))
    L.pad_checks(T, rig.ra, rig.rb)


def snes_link_keys(root, pid="snes"):
    f = os.path.join(root, "launch", pid + ".ini")
    if not os.path.exists(f):
        return {}
    return dict(l.rstrip("\n").split("=", 1) for l in open(f) if l.startswith("link.") and "=" in l)


SEGA = {"md": ("Alpha Quest (USA).md", 6), "sms": ("Master One.sms", 7), "sg": ("Tiny One.sg", 9)}


def tp_rom(T, con, other=False):
    """The cartridge of a Two players test (written to the build folder, never committed): a synthetic image, or for the
    SG-1000 the public homebrew of GATE_SG_ROM / GATE_MD_SMS_ROMS when there is one.  other=True: another game of the
    same console (another name, other bytes)."""
    os.makedirs(T.S.outdir, exist_ok=True)
    sega = SEGA.get(con)
    if sega:
        from app_tests import sega_rom
        real = L.sg_rom() if con == "sg" and not other else None
        name = os.path.basename(real) if real else sega[0]
        if other:
            name = name.replace("Alpha Quest", "Other Game").replace("Master One", "Other Game").replace("Tiny One", "Other Game")
        data = open(real, "rb").read() if real else sega_rom("md" if con == "md" else "sms", 0x20000 if con == "md" else 0x8000,
                                                             seed=2 if other else 1)
    else:
        name = "Super Two (USA).sfc" if other else "Super One (USA).sfc"
        data = bytes((i * 7 + 3) & 255 for i in range(0x8000)) if other else bytes(range(256)) * 128   # (a CRC with letters in it: the room's `rom` is lower-case)
    path = os.path.join(T.S.outdir, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


def tp_walk(con):
    """The console's list, START for Settings, the rows down to Two players, X: the page.  Every press waits for the line
    the app logs for it (the Settings page's row moves, the page opening), so a press a busy machine did not see, or saw
    twice, cannot send the walk to another row."""
    from app_tests import SNES_TWO_DOWNS
    downs = "+%d:hold=down=settings_row row=\n" % L.GAP * SNES_TWO_DOWNS
    if con in SEGA:
        w = ("90:hold=start=ui_screen name=settings\n+30:shot=settings.png\n" + downs +
             "+20:shot=row.png\n+10:hold=cross=snes_two_open\n+30:shot=page.png\n")
    else:
        w = ("%d:triangle:%d\n" % (L.at(1), L.PRESS) + L.keys("triangle", "triangle", first=40) +
             "+40:hold=start=ui_screen name=settings\n" + downs + "+30:hold=cross=snes_two_open\n+30:shot=page.png\n")
    return w


def hold_code(code):
    """The code typed on the pad with every press held until the pad has logged what it did (online_pad_key box= sym=):
    a press a busy machine would not have seen is held until it has been."""
    out, box, typed = "", 0, {}
    for ch in code:
        k = ALPHA.index(ch)
        up, down = k + 1, len(ALPHA) - k
        d, n = ("up", up) if up <= down else ("down", down)
        cur = None
        for _ in range(n):
            cur = ((-1 if d == "up" else 0) if cur is None else cur) + (1 if d == "up" else -1)
            cur %= len(ALPHA)
            out += "+%d:hold=%s=online_pad_key box=%d sym=%s\n" % (L.GAP, d, box, ALPHA[cur])
        typed[box] = ch
        box = min(box + 1, 4)
        out += "+%d:hold=right=online_pad_key box=%d sym=%s\n" % (L.GAP, box, typed.get(box, "-"))
    return out


def tp_pair(T, name, con, nat, rom, rom_b=None, host_end=85, b_end=80, join=True, b_tail="", lib=False, **kw):
    """Both consoles in their game list: A hosts a game of `con` Online from Settings > Two players, B joins with the code
    from the same page (join=False: B stops at the room's card).  The app forms the link with no game running."""
    walk = tp_walk(con)
    sa = (walk + "+%d:cross:20\n" % L.GAP + "+%d:down:%d\n" % (L.GAP, L.PRESS) + "+%d:hold=cross=snes_two_go role=host conn=3\n" % L.GAP)
    if join:                     # until the link is handed over, then a moment, then the end
        sa += "+0:wait=snes_link_ready\n+%d:shot=hosting.png\n+120:quit\n" % L.dur(2)
    else:
        sa += L.shot(14, "hosting.png") + L.quit_at(host_end)

    def sb(code):
        # lib: B takes the game list's own Settings > Join online row instead of the Two players page
        s = ((settings_join_online() if lib else walk + "+%d:down:%d\n" % (L.GAP, L.PRESS) + "+%d:cross:20\n" % L.GAP + "+%d:down:%d\n" % (L.GAP, L.PRESS)) +
             "+%d:hold=cross=online_pad_open\n" % L.GAP + "+30:shot=pad.png\n" + hold_code(code) +
             "+%d:hold=triangle=online_card_begin\n" % L.GAP + "+0:wait=online_card state=\n" +
             "+%d:shot=card.png\n" % L.dur(1))
        if join:
            s += ("+%d:hold=cross=online_open_join\n" % L.AFTER_SHOT + "+0:wait=snes_link_ready\n" + "+%d:shot=linking.png\n" % L.dur(2) +
                  b_tail + "+120:quit\n")
            return s
        return s + (b_tail or L.quit_at(b_end))

    def no_autoboot(rig):                            # A starts in its game list too
        real = T.twin

        def twin(nm, *a, **k):
            t = real(nm, *a, **k)
            ab = os.path.join(t.root, "autoboot.txt")
            if os.path.exists(ab):
                os.remove(ab)
            return t
        T.twin = twin
    cfg = {"ui_home": 0, "hints": 0}
    if con in SEGA:
        cfg["console"] = SEGA[con][1]
    try:
        return pair(T, name, rom=rom, rom_b=rom_b, nat=nat, a_script=sa, b_script=sb, a_cfg=dict(cfg), b_cfg=dict(cfg),
                    a_end=host_end, b_end=b_end, setup=no_autoboot, **kw)
    finally:
        if "twin" in T.__dict__:
            del T.__dict__["twin"]


def snes_online(T, name, nat, relay, con="snes", lib=False):
    """Both consoles in the game list: A hosts a SNES game Online from Settings > Two players, B joins with the code
    from the same page.  The app forms the link (room, punch or relay) with no game running and writes it into
    launch/snes.ini for snes.bin (the twin cannot start that program: snes/twin/link_test.py plays the files' kind)."""
    # con: "snes", or a Sega console of md.bin ("md", "sms", "sg"): the same page and the same hand-over, the file a synthetic
    # cartridge image of that console, or for sg the public homebrew (the app only forms the link; the player starts afterwards)
    rom = tp_rom(T, con)
    rig = tp_pair(T, name, con, nat, rom, lib=lib)
    ra, rb = rig.ra, rig.rb
    if not ra or not rb:
        return
    T.clean(ra, "A", allow="could not be started")
    T.clean(rb, "B", allow="could not be started")
    T.check(ra.has(r"online_room code=%s" % rig.code), "A: the service made the room for a %s game (%s)" % (con, rig.code))
    T.check(ra.has(r"link_snes_begin role=host .*console=%s" % con), "A: the room's console is %s" % con)
    T.check(rb.has(r"online_card state=1 .*kind=5 .*compatible=1") and rb.has(r"online_open_join rom_match=1"),
            "B: the card is a Two players room and the library has the game")
    mode = "relay" if relay else "punch"
    pid = con                                       # launch/<console id>.ini
    ka, kb = snes_link_keys(ra.root, pid), snes_link_keys(rb.root, pid)
    for r, who, k, role in ((ra, "A", ka, "host"), (rb, "B", kb, "guest")):
        rd = r.evt("snes_link_ready")
        T.check(rd and rd[0].get("ok") == "1" and rd[0].get("mode") == mode,
                "%s: the link formed and was handed over (snes_link_ready mode=%s: %s)" % (who, mode, [e["_"] for e in rd]))
        T.check(r.has(r"^EVT %s_launch_file ok=1" % ("snes" if con == "snes" else "sega")) and k.get("link.role") == role and k.get("link.mode") == mode,
                "%s: launch/%s.ini carries the link (role %s, mode %s)" % (who, pid, k.get("link.role"), k.get("link.mode")))
        T.radio_free(r, who)
    T.check(ka.get("link.punch_key") and ka.get("link.punch_key") == kb.get("link.punch_key") and
            ka.get("link.compat") == kb.get("link.compat"), "both carry the same punch key and pad compat word")
    hs = ra.evt("link_snes_session")
    T.check(hs and hs[-1].get("how") == "measured" and int(ka.get("link.delay", "0")) >= 3,
            "A: the host's input delay was measured (%s, link.delay %s)" % (hs and hs[-1]["_"], ka.get("link.delay")))
    if relay:
        T.check(ka.get("link.relay_addr") and ka.get("link.relay_addr") == kb.get("link.relay_peer") and
                kb.get("link.relay_addr") == ka.get("link.relay_peer") and ka.get("link.relay_user"),
                "each file names its own allocation and the other's as the peer (%s / %s)" %
                (ka.get("link.relay_addr"), kb.get("link.relay_addr")))
        tn = rig.st.get("turn", {})
        T.check(tn.get("allocations") == 2 and tn.get("releases") == 0 and tn.get("live") == 2,
                "the relay keeps both allocations for snes.bin: none was released (%s)" %
                dict((k, tn.get(k)) for k in ("allocations", "releases", "live")))
    else:
        T.check(ka.get("link.peer", "").startswith("203.0.113.11:") and kb.get("link.peer", "").startswith("203.0.113.10:"),
                "each file names the other's public address (%s / %s)" % (ka.get("link.peer"), kb.get("link.peer")))
    T.picture(ra, "A", "hosting.png")


def t_snes_online(T):
    """Online, SNES Two players from the game list: the app forms the link (punch, then relay) and hands it to snes.bin"""
    snes_online(T, "online_snes_punch", ("port", "port"), False)
    snes_online(T, "online_snes_relay", ("sym", "port"), True)


def t_sega_online(T):
    """Online, Two players on a Genesis, a Master System and an SG-1000 game: the room names the console, the link forms
    (punch, then relay) and is handed to md.bin"""
    for con in os.environ.get("SEGA_ONLINE_ONLY", "md,sms,sg").split(","):     # (to rerun one console)
        snes_online(T, "online_%s_punch" % con, ("port", "port"), False, con)
        snes_online(T, "online_%s_relay" % con, ("sym", "port"), True, con)


def t_sega_library_join(T):
    """Join online from the game list's Settings (not the Two players page) on a Genesis, a Master System and an SG-1000 game:
    the library finds the game by its room code, it opens, and the link is handed to md.bin as a guest"""
    for con in os.environ.get("SEGA_ONLINE_ONLY", "md,sms,sg").split(","):
        snes_online(T, "online_%s_library" % con, ("port", "port"), False, con, lib=True)


# ---- every console: another game, a version that does not fit, a service that wants an update -------------------

CORE_FAMS = ("gba", "gb", "gbc", "nes", "ws")          # played in the app itself: the room's card comes from the game's menu
TP_FAMS = ("snes", "md", "sms", "sg")                 # the player programs: Settings > Two players, the card from the list
FAM_NAMES = {"gba": "GBA", "gb": "GB", "gbc": "GBC", "nes": "NES", "ws": "WonderSwan", "snes": "SNES",
             "md": "Genesis", "sms": "Master System", "sg": "SG-1000"}


def _mutated(T, src, tag):
    """The same console's game with one byte changed in the middle: another file (and, with the identity taken from the
    bytes, another game).  Written to the build folder."""
    d = os.path.join(T.S.outdir, "refuse_roms")
    os.makedirs(d, exist_ok=True)
    data = bytearray(open(src, "rb").read())
    data[len(data) // 2] ^= 0x5A
    out = os.path.join(d, tag + "_" + os.path.basename(src))
    with open(out, "wb") as f:
        f.write(data)
    return out


def refuse_roms(T, fam):
    """(the host's game, another game of the same console) for `fam`; (None, None): no game to test with (skipped)."""
    if fam == "gba":
        return L.LINKTEST, L.LINKTEST_B
    if fam == "gb":
        return L.GBLINK, L.GBLOCAL
    if fam == "gbc":
        return L.GBLINK_CGB, os.path.join(HERE, "testrom", "gblocal-cgb.gbc")
    if fam == "nes":
        a = L.nes_rom()
        return (a, _mutated(T, a, "other")) if a else (None, None)
    if fam == "ws":
        a = L.ws_rom()
        return (a, _mutated(T, a, "other")) if a else (None, None)
    return tp_rom(T, fam), tp_rom(T, fam, other=True)


def refuse_pair(T, fam, name, rom_b=None, edit=None, svc_env=(), **kw):
    """A hosts a game of `fam`; B (its library: rom_b, else the same game) types the code and stops at the card."""
    a, other = refuse_roms(T, fam)
    if not a:
        T.S.note("%s: no game for this console (see the gate README): skipped" % FAM_NAMES[fam])
        return None
    if rom_b == "other":
        rom_b = other
    proxy = (lambda rig, e=edit: HttpProxy(rig.T, rig.svc, edit=e)) if edit else None
    if fam in CORE_FAMS:
        kind = "gba" if fam == "gba" else "gb" if fam in ("gb", "gbc") else "nes"
        return pair(T, name, rom=a, rom_b=rom_b, with_world=False, kind=kind, a_end=40, b_end=30, svc_env=svc_env,
                    a_script=host_script(40) + L.quit_at(40),
                    b_script=lambda code: join_script_hold(code, "+60:quit\n"), proxy_b=proxy, **kw)
    return tp_pair(T, name, fam, ("port", "port"), a, rom_b=rom_b, host_end=45, b_end=30, join=False, with_world=False,
                   svc_env=svc_env, proxy_b=proxy, b_tail="+60:quit\n", **kw)


def refuse_check(T, rig, fam, want, tag, no_join=True):
    """B's card failed with UI_FAIL_<want>: no join was made, no link formed, the host never saw a guest."""
    who = "B %s %s" % (FAM_NAMES[fam], tag)
    T.clean(rig.rb, who, allow="could not be started")
    T.clean(rig.ra, "A %s %s" % (FAM_NAMES[fam], tag), allow="could not be started")
    bad = rig.rb.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail(want), "%s: UI_FAIL_%s (%s)" % (who, want, bad[-1]["_"] if bad else "none"))
    if no_join:
        T.check(not any(x["path"].endswith("/join") for x in rig.reqs), "%s: no join request was made" % who)
        T.check(not rig.ra.has(r"online_peer") and not rig.rb.has(r"link_running") and not rig.rb.has(r"snes_link_ready"),
                "%s: no link formed, the host never saw a guest" % who)
    T.picture(rig.rb, "B", "card.png")


def different_file(T, fam):
    """Link Cable consoles (GBA, GB, GBC) carry a partner's cartridge across the link (Ruby with Sapphire), so a joiner whose
    copy of the game differs from the host's is not refused at the card: the session's own check has the last word, and
    on both consoles the outcome must be a clean one (the host's file carried over, or a refusal)"""
    a, _ = refuse_roms(T, fam)
    b = _mutated(T, a, "other")
    kind = "gba" if fam == "gba" else "gb"
    rig = pair(T, "online_file_" + fam, rom=a, rom_b=b, kind=kind, a_end=78, b_end=76,
               a_script=host_script(78) + "+0:wait=online_peer nick=\n" + "+%d:shot=late.png\n" % L.dur(40) +
               "+%d:quit\n" % L.dur(70),                # from the guest's join, not from a guessed time
               b_script=lambda code: join_script(code, L.shot(60, "after.png") + L.quit_at(76), press_join=True))
    who = FAM_NAMES[fam]
    T.S.note("%s: another file with the host's game code: %s" % (who, "the link formed (the host's file carried over)"
             if rig.rb.has(r"link_running") else "the session refused it"))
    T.clean(rig.rb, "B " + who)
    T.clean(rig.ra, "A " + who)
    c = rig.rb.evt("online_card")
    T.check(c and c[-1].get("compatible") == "1", "B %s: the card is compatible (same game)" % who)
    T.check(rig.rb.has(r"link_running") or rig.rb.has(r"gblink_(refused|end|need|rom)|link_ended|link_failed|link_fail_screen"),
            "B %s: the session said what it made of another file" % who)
    T.check(rig.ra.has(r"link_running") or rig.ra.has(r"gblink_(refused|end|need|rom)|link_ended|link_failed|link_fail_screen") or
            rig.ra.has(r"link_online_failed"), "A %s: and the host ended cleanly too" % who)


def refuse_game(T, fam):
    """Another game: B's library has no copy of the host's game, so the card says to add it and nothing joins (Link Cable
    consoles: the copy that differs is carried across the link, see different_file)"""
    if fam in ("gba", "gb", "gbc"):
        return different_file(T, fam)
    rig = refuse_pair(T, fam, "online_game_" + fam, rom_b="other")
    if rig:
        refuse_check(T, rig, fam, "ONLINE_GAME", "(another game)")
        T.check(not rig.rb.has(r"online_open_join"), "B %s: nothing was opened to join with" % FAM_NAMES[fam])


def refuse_version(T, fam):
    """Version: the host newer ('update this'), the host older ('your friend must update'), and a service that wants an
    update (426) for a joiner and a host"""
    for tag, edit, want in (("newer", _card_ver(True), "ONLINE_VER_OLD_HERE"), ("older", _card_ver(False), "ONLINE_VER_OLD_THERE")):
        rig = refuse_pair(T, fam, "online_version_%s_%s" % (tag, fam), edit=edit)
        if not rig:
            return
        refuse_check(T, rig, fam, want, "(host %s)" % tag)
    # the service itself wants a newer app: a joiner types a code (any), a host asks for a room
    a, _ = refuse_roms(T, fam)
    env = ("MIN_APP_VERSION=2.9.0", "LATEST_APP_VERSION=2.9.0")
    if fam in CORE_FAMS:
        rig = solo(T, "online_update_%s" % fam, code="ZZZZZ", svc_env=env, rom=a, b_end=40)
    else:
        walk = tp_walk(fam)
        sb = (walk + "+%d:down:%d\n" % (L.GAP, L.PRESS) + "+%d:cross:20\n" % L.GAP + "+%d:down:%d\n" % (L.GAP, L.PRESS) +
              "+%d:hold=cross=online_pad_open\n" % L.GAP + "+30:shot=pad.png\n" + hold_code("ZZZZZ") +
              "+%d:hold=triangle=online_card_begin\n" % L.GAP + "+0:wait=online_card state=\n" +
              "+%d:shot=card.png\n" % L.dur(1) + "+60:quit\n")
        cfg = {"ui_home": 0, "hints": 0}
        if fam in SEGA:
            cfg["console"] = SEGA[fam][1]
        rig = solo(T, "online_update_%s" % fam, code="ZZZZZ", svc_env=env, rom=a, script=sb, b_cfg=cfg, noauto=True, b_end=30)
    r = rig.rb
    T.clean(r, "B %s update" % FAM_NAMES[fam], allow="could not be started")
    bad = r.evt("online_card_bad")
    T.check(bad and int(bad[-1]["why"]) == ui_fail("ONLINE_UPDATE"), "B %s: 'too old for online play' (%s)" %
            (FAM_NAMES[fam], bad[-1]["_"] if bad else "none"))
    T.picture(r, "B", "card.png")
    if fam in CORE_FAMS:
        rig = solo(T, "online_update_host_%s" % fam, code=None, svc_env=env, tail=L.shot(14, "failed.png"), b_end=20, rom=a)
        r = rig.rb
        T.clean(r, "A %s update" % FAM_NAMES[fam])
        f = r.evt("link_online_failed")
        T.check(f and int(f[-1]["ui"]) == ui_fail("ONLINE_UPDATE"), "A %s: the host's try reads 'too old' (%s)" %
                (FAM_NAMES[fam], f[-1]["_"] if f else "none"))
        T.picture(r, "A", "failed.png")


def _fam_test(fn, fam, what):
    def t(T):
        fn(T, fam)
    t.__doc__ = "%s: %s" % (FAM_NAMES[fam], what)
    return t


# ---- two DIFFERENT cartridges (Pokemon Ruby with Sapphire) over Online ------------------------------------

def two_games_online(T, tag, nat, relay, rom_a, rom_b, end, cached=False):
    """One session: A plays rom_a, B plays rom_b.  The partner's cartridge crosses the link once (cached under its
    identity) and the second session of the same two data roots finds it in the cache."""
    name = "online_two_games_" + tag
    a = (host_script(end) + L.shot(end - 14, "running.png") + L.disconnect(end - 8, "session.png") +
         L.shot(end - 4, "ended.png") + L.quit_at(end))
    b = lambda code: join_script(code, L.shot(end - 16, "running.png") + L.shot(end - 6, "ended.png") +
                                 L.quit_at(end - 2))
    rig = pair(T, name, rom=rom_a, rom_b=rom_b, nat=nat, a_script=a, b_script=b, a_end=end, b_end=end - 2,
               keep=cached, begin=not cached, wait_a=120)
    ra, rb = rig.ra, rig.rb
    T.clean(ra, "A " + tag)
    T.clean(rb, "B " + tag)
    T.check(ra.has(r"link_running player=1") and rb.has(r"link_running player=2"), "%s: linked" % tag)
    T.check(rb.has(r"online_peer nick=\"Alice\" mode=%s" % ("relay" if relay else "punch")), "%s: the path was the %s" %
            (tag, "relay" if relay else "punch"))
    for r, who, mine, theirs in ((ra, "A", rom_a, rom_b), (rb, "B", rom_b, rom_a)):
        if not cached:
            rec = r.evt("gblink_rom_received")
            T.check(r.has(r"gblink_partner_rom source=transfer") and r.has(r"gblink_rom_received size=%d ok=1" %
                                                                           os.path.getsize(theirs)) and
                    r.has(r"gblink_rom_send done size=%d" % os.path.getsize(mine)),
                    "%s, %s: the other's cartridge crossed once (%d bytes, checked) and its own went the other way" %
                    (tag, who, os.path.getsize(theirs)))
            T.check(len(rec) == 1 and r.has(r"gbadual_session_partner_rom rc=0") and r.has(r"gblink_start .*partner=received"),
                    "%s, %s: received once, kept in the cache, the partner machine loaded it" % (tag, who))
            T.S.note("%s, %s: transfer of %d bytes took %.1f s (%s)" % (
                tag, who, os.path.getsize(theirs), int(rec[-1]["us"]) / 1e6 if rec else -1, rec[-1]["_"][:120] if rec else ""))
        else:
            T.check(r.has(r"gblink_partner_rom source=cache") and not r.has(r"gblink_rom_send begin") and
                    not r.has(r"gblink_rom_received") and r.has(r"gblink_start .*partner=cache"),
                    "%s, %s: the second session used the cache: nothing crossed the link" % (tag, who))
        T.radio_free(r, who)
    T.same_machines(ra, rb)
    return rig


def t_two_games(T):
    """Two DIFFERENT cartridges over Online (Ruby with Sapphire): the partner's game crosses once, then the cache"""
    if not os.path.exists(L.LINKTEST_B):
        T.S.note("no tools/linktest/linktest_b.gba: skipped")
        return
    d = os.path.join(T.S.outdir, "roms_two_games")
    os.makedirs(d, exist_ok=True)
    for tag, nat, relay, size in (("punch", ("port", "port"), False, 16 << 20), ("relay", ("sym", "port"), True, 4 << 20)):
        a, b = os.path.join(d, "ruby_%s.gba" % tag), os.path.join(d, "sapphire_%s.gba" % tag)
        L.padded(L.LINKTEST, a, size, 1)
        L.padded(L.LINKTEST_B, b, size + 4096, 2)
        end = 330 if size > (8 << 20) else 160
        two_games_online(T, tag, nat, relay, a, b, end)
        two_games_online(T, tag + "_again", nat, relay, a, b, 130 if size < (8 << 20) else 130, cached=True)
        for f in (a, b):
            os.remove(f)


# ---- the Connection page ----------------------------------------------------------------------------------

def t_direct_hidden(T):
    """Connection page: no 'Direct address' row by default; with link_direct = 1 it is there"""
    T.begin("online_direct_hidden")
    shots = (L.open_play(L.EARLY) + "+20:down:%d\n" % L.PRESS + "+20:down:%d\n" % L.PRESS +
             "+20:down:%d\n" % L.PRESS + L.keys("cross") + "+20:shot=conn.png\n" + L.quit_at(14))
    a = T.twin("a", T.arch_a, L.LINKTEST, "Alice", shots, config={"net_conn": 3})
    b = T.twin("b", T.arch_b, L.LINKTEST, "Bob", shots, config={"net_conn": 3}, vita={"link_direct": 1})
    ra, rb = T.run([a, b], 14)
    T.clean(ra, "A")
    T.clean(rb, "B")
    T.check(ra.has(r"online_boot .*direct=0") and rb.has(r"online_boot .*direct=1"), "the switch reaches the app (direct=0 / direct=1)")
    sa, sb = ra.shot("conn.png"), rb.shot("conn.png")
    from PIL import ImageChops
    diff = ImageChops.difference(sa, sb).getbbox()
    T.check(diff is not None, "the two Connection pages differ (the Direct address row)")
    T.picture(ra, "A", "conn.png")
    T.picture(rb, "B", "conn.png")


TESTS = [
    ("punch", t_punch), ("relay", t_relay), ("relay_joiner", t_relay_joiner),
    ("relay_host_setting", t_relay_setting_host), ("relay_joiner_setting", t_relay_setting_joiner),
    ("punch_opens_host", t_punch_opens_host),
    ("gb", t_gb_punch), ("gb_relay", t_gb_relay), ("gbc", t_gbc_punch), ("gbc_relay", t_gbc_relay), ("adapter", t_adapter), ("share", t_share),
    ("wrong_code", t_wrong_code), ("expired", t_expired), ("update_required", t_update_required),
    ("version", t_version), ("different_game", t_different_game), ("library_join", t_library_join),
    ("busy", t_busy), ("slow_down", t_slow_down), ("service_down", t_service_down),
    ("lost_answer", t_lost_answer), ("cancel_after_failure", t_cancel_after_failure), ("drop", t_drop_midlink),
    ("rtt_100", t_rtt_100), ("rtt_150", t_rtt_150), ("rtt_200", t_rtt_200), ("delay_cap", t_delay_cap),
    ("determinism", t_determinism), ("two_games", t_two_games), ("direct_hidden", t_direct_hidden),
    ("nes_relay", t_nes_relay), ("nes_punch", t_nes_punch), ("ws_relay", t_ws_relay), ("ws_punch", t_ws_punch),
    ("snes_online", t_snes_online), ("sega_online", t_sega_online), ("sega_library_join", t_sega_library_join),
]
for _fam in CORE_FAMS + TP_FAMS:
    TESTS.append(("game_" + _fam, _fam_test(refuse_game, _fam, (
        "Online with another copy of the game: the host's file is carried across the link, both end cleanly"
        if _fam in ("gba", "gb", "gbc") else "Online with another game is refused cleanly (the card names the game to add)"))))
    TESTS.append(("version_" + _fam, _fam_test(refuse_version, _fam, "Online version mismatch and a service that wants an update are refused cleanly")))
# armhf (under qemu, several times the wall time): the sessions
RUNS = {
    "x86": [n for n, _ in TESTS],
    "armhf": ["punch", "relay", "gb", "lost_answer", "library_join"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", nargs="?", default="x86", choices=("x86", "armhf"))
    ap.add_argument("--only")
    ap.add_argument("--hz", type=int, default=0, help="the twins' display rate (60; 12 under qemu)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        print(" ".join(n for n, _ in TESTS))
        return
    T = L.Tests(a.mode, a.hz)
    T.S.outdir = os.path.join(HERE, "build", "onlinetest", a.mode)
    for arch in {T.arch_a, T.arch_b}:
        if not os.path.exists(L.exe_of(arch)):
            sys.exit("no %s: make ARCH=%s first" % (L.exe_of(arch), arch))
    for f in (L.LINKTEST, L.GBLINK):
        if not os.path.exists(f):
            sys.exit("no %s (make -C gpsp/tools/linktest; make -C testrom gb RGBDS=...)" % f)
    only = a.only.split(",") if a.only else None
    skipped = []
    try:
        for name, fn in TESTS:
            if only and name not in only:
                continue
            if not only and name not in RUNS[a.mode]:
                continue
            T.S.test = name
            t0 = time.time()
            print("%s: %s" % (name, fn.__doc__.split("\n")[0]), flush=True)
            before = len(T.S.fails)
            try:
                fn(T)
            except Skip as e:
                print("    SKIP: %s is not installed" % e, flush=True)
                skipped.append(name)
                continue
            except Exception as e:
                import traceback
                traceback.print_exc()
                T.S.check(False, "the scenario itself raised %r" % (e,))
            finally:
                T.kill_all()
            print("    %s  (%.0f s)" % ("ok" if len(T.S.fails) == before else "FAILED", time.time() - t0), flush=True)
    finally:
        T.kill_all()
    if skipped:
        print("SKIPPED: %s" % ", ".join(skipped))
    sys.exit(T.finish())


if __name__ == "__main__":
    main()
