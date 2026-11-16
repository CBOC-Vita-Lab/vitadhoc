#!/usr/bin/env python3
"""online_tools.py -- the outside world of the online scenarios (online_tests.py).

    FakeService(T, *env)   the room service of online/service/ under node, on the loopback
    NatWorld(T, nat_a, nat_b, ...)   the NAT / TURN simulator (netdrv/tests/natsim) as a UDP proxy
    net_for(k), vita_for(k, world, service, **kw)   the nethost.ini / vita.ini values of twin k

`T` is anything with `.dir` (where logs go) and `.procs` (a list the test kills in `finally`), as
link_tests.Tests is.  Nothing here touches a real network: every address is on 127.0.0.1.

Run `python3 online_tools.py` for the self-test: starts both, sends one wrapped STUN request through
the proxy and one /v1/info to the service, stops them.  Exit 0 = pass or clean SKIP (no node / g++).

Until online/service/dev/server.mjs exists (package S), FakeService runs
gpsp/netdrv/tests/natsim/service_standin.mjs, which only knows /v1/info and the two /_test/ routes
(`FakeService.standin` says which one runs).
"""
import json
import os
import queue
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import atexit
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GP = os.path.normpath(os.path.join(HERE, "..", ".."))
ROOT = os.path.normpath(os.path.join(GP, ".."))
NATSIM_DIR = os.path.join(GP, "netdrv", "tests", "natsim")
SERVICE_SERVER = os.path.join(ROOT, "online", "service", "dev", "server.mjs")
STANDIN = os.path.join(NATSIM_DIR, "service_standin.mjs")
PROXY = os.path.join(HERE, "build", "natsim", "natsim_proxy")
STUN_SERVERS = "198.51.100.1:3478,198.51.100.2:3478"


_LIVE = []          # every child started here (each leads its own process group)
_HOOKED = []


def _kill_group(p):
    """SIGTERM then SIGKILL to the child's whole group (node, the proxy, anything they started)."""
    if p.poll() is not None:
        try:
            os.killpg(p.pid, signal.SIGKILL)       # stragglers of a group whose leader already left
        except OSError:
            pass
        return
    for sig, wait in ((signal.SIGTERM, 3), (signal.SIGKILL, 5)):
        try:
            os.killpg(p.pid, sig)
        except OSError:
            break
        try:
            p.wait(timeout=wait)
            break
        except subprocess.TimeoutExpired:
            pass
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except OSError:
        pass


def kill_all_children():
    for p in list(_LIVE):
        _kill_group(p)
    del _LIVE[:]


def _hook():
    """Once: kill every child's group at exit and on SIGTERM / SIGINT of the runner (then die the usual way)."""
    if _HOOKED:
        return
    _HOOKED.append(1)
    atexit.register(kill_all_children)
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        old = signal.getsignal(sig)

        def handler(num, frame, old=old):
            kill_all_children()
            if callable(old):
                return old(num, frame)
            signal.signal(num, signal.SIG_DFL)
            os.kill(os.getpid(), num)
        try:
            signal.signal(sig, handler)
        except ValueError:          # not the main thread
            pass


class Skip(Exception):
    """A tool the scenario needs is missing (node, g++): the scenario SKIPs, naming it."""


class _Proc:
    """A child process whose stdout lines are collected by a thread."""

    def __init__(self, T, cmd, log, **kw):
        self.lines = queue.Queue()
        self.all = []
        self.log = open(log, "w")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1,
                                  start_new_session=True, **kw)
        _hook()
        _LIVE.append(self.p)
        if hasattr(T, "procs"):
            T.procs.append(self.p)
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self):
        for line in self.p.stdout:
            line = line.rstrip("\n")
            self.all.append(line)
            self.log.write(line + "\n")
            self.log.flush()
            self.lines.put(line)

    def wait_line(self, rx, timeout):
        """The first new line matching rx (a match object), or None."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.05, min(0.2, end - time.time())))
            except queue.Empty:
                if self.p.poll() is not None and self.lines.empty():
                    return None
                continue
            m = re.search(rx, line)
            if m:
                return m
        return None

    def say(self, text):
        try:
            self.p.stdin.write(text + "\n")
            self.p.stdin.flush()
        except (OSError, ValueError):
            pass

    def stop(self):
        _kill_group(self.p)
        if self.p in _LIVE:
            _LIVE.remove(self.p)
        self.t.join(timeout=2)
        self.log.close()


def _outdir(T):
    d = getattr(T, "dir", None) or tempfile.mkdtemp(prefix="online_tools_")
    os.makedirs(d, exist_ok=True)
    return d


# ---- the service -----------------------------------------------------------------

class FakeService:
    """node online/service/dev/server.mjs --port 0 [--env K=V ...]; `env` are "K=V" strings (or
    "--clock-skew-ms=N" style flags are passed as ("--clock-skew-ms", "N") tuples)."""

    def __init__(self, T, *env):
        node = shutil.which("node")
        if not node:
            raise Skip("node")
        self.standin = not os.path.isfile(SERVICE_SERVER)
        args = []
        for e in env:
            if isinstance(e, (tuple, list)):
                args += [str(x) for x in e]
            else:
                args += ["--env", e]
        self.out = _outdir(T)
        self.proc = _Proc(T, [node, STANDIN if self.standin else SERVICE_SERVER,
                              "--port", "0"] + args, os.path.join(self.out, "service.log"))
        m = self.proc.wait_line(r"^READY (\d+)", 20)
        if not m:
            self.proc.stop()
            raise RuntimeError("the service did not print READY (see service.log)")
        self.port = int(m.group(1))
        self.url = "http://127.0.0.1:%d" % self.port

    def requests(self):
        """Every REQ line so far, parsed: dicts method, path, status, code ('-' = no error)."""
        out = []
        for line in list(self.proc.all):
            m = re.match(r"^REQ (\S+) (\S+) (\d+) (\S+)", line)
            if m:
                out.append({"method": m.group(1), "path": m.group(2),
                            "status": int(m.group(3)), "code": m.group(4)})
        return out

    def _call(self, method, path):
        req = urllib.request.Request(self.url + path, method=method,
                                     data=b"{}" if method == "POST" else None,
                                     headers={"User-Agent": "VitAdhoc/online_tools"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read().decode())

    def evict(self):
        """The object leaves memory and comes back over the same database."""
        return self._call("POST", "/_test/evict")

    def stats(self):
        return self._call("GET", "/_test/stats")

    def stop(self):
        self.proc.stop()


# ---- the NAT world ---------------------------------------------------------------

def build_proxy():
    """Build natsim_proxy once into build/natsim/ (again when a source is newer)."""
    if not shutil.which("g++"):
        raise Skip("g++")
    srcs = [os.path.join(NATSIM_DIR, f) for f in os.listdir(NATSIM_DIR)
            if f.endswith((".cpp", ".h", ".sh"))]
    if os.path.isfile(PROXY) and os.path.getmtime(PROXY) >= max(os.path.getmtime(s) for s in srcs):
        return PROXY
    r = subprocess.run(["bash", os.path.join(NATSIM_DIR, "build.sh"), PROXY],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise RuntimeError("natsim build failed:\n" + r.stdout[-2000:])
    return PROXY


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class NatWorld:
    """nat_a / nat_b: natsim NAT names (full addr port sym symaddr open).  Client k is the twin
    with `link_natsim_id = k`; it is 192.168.<k+1>.10:19019 behind 203.0.113.<10+k>."""

    def __init__(self, T, nat_a="port", nat_b="port", turn=True, same_lan=False,
                 loss=0, delay=0, jitter=0, idle=None, lan_port=19019, extra=()):
        self.out = _outdir(T)
        self.port = free_udp_port()
        args = ["--port", str(self.port), "--nat0", nat_a, "--nat1", nat_b,
                "--lan-port", str(lan_port)]
        if turn:
            args.append("--turn")
        if same_lan:
            args.append("--same-lan")
        if loss:
            args += ["--loss", str(loss)]
        if delay:
            args += ["--delay", str(delay)]
        if jitter:
            args += ["--jitter", str(jitter)]
        if idle is not None:
            args += ["--idle", str(idle)]
        args += [str(x) for x in extra]
        self.proc = _Proc(T, [build_proxy()] + args, os.path.join(self.out, "natsim.log"))
        if not self.proc.wait_line(r"^ready on", 20):
            self.proc.stop()
            raise RuntimeError("natsim_proxy did not print ready (see natsim.log)")

    def command(self, text):
        """stdin command: 'down 0', 'up 0', 'flush 1', 'impair loss delay jitter'."""
        self.proc.say(text)

    def stats(self):
        """The simulator's counters: {'sim': {sent: .., ...}, 'turn': {allocations: .., ...}}."""
        while not self.proc.lines.empty():
            self.proc.lines.get_nowait()
        self.proc.say("stats")
        out = {}
        m = self.proc.wait_line(r"^stats: (.*)", 5)
        if m:
            out["sim"] = {k: int(v) for k, v in re.findall(r"(\w+) (\d+)", m.group(1))}
        t = self.proc.wait_line(r"^turn: (.*)", 1)
        if t:
            s = t.group(1)
            d = {k: int(v) for k, v in re.findall(r"(\w+) (\d+)", s)}
            d.update({k.replace("-", "_"): int(v) for k, v in re.findall(r"([a-z]+(?:-[a-z]+)*) (\d+)", s)})
            out["turn"] = d
        return out

    def stop(self):
        self.proc.stop()


# ---- per-twin settings ---------------------------------------------------------------

def net_for(k):
    """nethost.ini values (Twin(net=...)): twin k is 192.168.<k+1>.10 on its own LAN."""
    return {"ip": "192.168.%d.10" % (k + 1), "mask": "255.255.255.0"}


def vita_for(k, world, service, **kw):
    """vita.ini values (Twin(vita=...)) for twin k: the service, the two STUN servers, the proxy.
    `kw` adds or overrides (online_relay=1, link_latency=100, ...)."""
    v = {"online_url": service.url if hasattr(service, "url") else service,
         "online_stun": STUN_SERVERS,
         "link_natsim": "127.0.0.1:%d" % world.port,
         "link_natsim_id": k}
    v.update(kw)
    return v


def stun_request_through(world, k=0, server=("198.51.100.1", 3478), timeout=5):
    """One STUN binding request wrapped for the proxy as client k; returns the mapped (ip, port)
    the NAT showed (what the app's own STUN step would learn), or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    tid = os.urandom(12)
    req = struct.pack(">HHI", 0x0001, 0, 0x2112A442) + tid
    wrapped = (bytes([0xFE, k]) + socket.inet_aton(server[0]) + struct.pack(">H", server[1]) + req)
    s.sendto(wrapped, ("127.0.0.1", world.port))
    try:
        data, _ = s.recvfrom(2048)
    except socket.timeout:
        return None
    finally:
        s.close()
    body = data[6:]                      # src ip[4] | src port[2] | payload
    if data[:4] != socket.inet_aton(server[0]) or len(body) < 20:
        return None
    mtype, mlen, cookie = struct.unpack(">HHI", body[:8])
    if mtype != 0x0101 or body[8:20] != tid:
        return None
    i = 20
    while i + 4 <= 20 + mlen:
        at, al = struct.unpack(">HH", body[i:i + 4])
        v = body[i + 4:i + 4 + al]
        if at == 0x0020 and al >= 8:     # XOR-MAPPED-ADDRESS
            port = struct.unpack(">H", v[2:4])[0] ^ 0x2112
            ip = bytes(a ^ b for a, b in zip(v[4:8], struct.pack(">I", 0x2112A442)))
            return socket.inet_ntoa(ip), port
        if at == 0x0001 and al >= 8:     # MAPPED-ADDRESS
            return socket.inet_ntoa(v[4:8]), struct.unpack(">H", v[2:4])[0]
        i += 4 + ((al + 3) & ~3)
    return None


# ---- self-test ---------------------------------------------------------------------------

def selftest():
    class T:
        dir = tempfile.mkdtemp(prefix="online_tools_")
        procs = []
    ok = True

    def check(c, what):
        nonlocal ok
        print(("PASS " if c else "FAIL ") + what)
        ok = ok and bool(c)

    try:
        svc = world = None
        try:
            svc = FakeService(T)
            world = NatWorld(T, "port", "sym")
        except Skip as e:
            print("SKIP online_tools self-test: %s is not installed" % e)
            return 0
        print("service: %s (%s)" % (svc.url, "stand-in" if svc.standin else "online/service"))
        ip, port = None, None
        got = stun_request_through(world, 0)
        check(got and got[0] == "203.0.113.10" and got[1] > 0,
              "STUN through the proxy: client 0 is seen as 203.0.113.10 (%s)" % (got,))
        got1 = stun_request_through(world, 1)
        check(got1 and got1[0] == "203.0.113.11", "client 1 is 203.0.113.11 (%s)" % (got1,))
        st = world.stats()
        check(st.get("sim", {}).get("stun", 0) >= 2, "stats: the simulator answered STUN (%s)" % st.get("sim"))
        check("turn" in st, "stats: the TURN server is up")
        world.command("down 0")
        world.command("up 0")
        with urllib.request.urlopen(urllib.request.Request(
                svc.url + "/v1/info?v=2.0.0", headers={"User-Agent": "VitAdhoc/selftest"}), timeout=10) as r:
            body = json.loads(r.read().decode())
        check(r.status == 200 and isinstance(body, dict), "GET /v1/info -> 200")
        reqs = svc.requests()
        time.sleep(0.2)
        reqs = svc.requests()
        check(len(reqs) == 1 and reqs[0]["path"].startswith("/v1/info") and reqs[0]["status"] == 200,
              "requests() parsed one REQ line (%s)" % reqs)
        check(svc.evict().get("ok"), "evict()")
        check(svc.stats().get("requests") == 1, "stats() counts the request")
        v = vita_for(1, world, svc, online_relay=1)
        check(v["link_natsim_id"] == 1 and v["online_url"] == svc.url and v["online_relay"] == 1,
              "vita_for / net_for values: %s %s" % (v, net_for(1)))
        svc.stop()
        world.stop()
    finally:
        for p in T.procs:
            if p.poll() is None:
                p.kill()
        shutil.rmtree(T.dir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(selftest())
