#!/usr/bin/env python3
"""A small fake of the VitAdhoc room service for the tests of online_http (plain http:// and TLS), all on 127.0.0.1.

It checks what the real service checks of a request (the `v` query, a non-empty User-Agent, a JSON body of at most 1024 bytes with exactly
the known keys) so that a wrong request from the client is a test failure, and it answers every route with the shapes of
ONLINE-2.0-PLAN.md 1.2.  A room code that begins with `E` or is one of BIG / GARBAGE / SLOW / NOBODY is a script: the answer is the
named failure whatever the route (see SCRIPTS).  `GET /__stats` (plain port only) reports what the server saw.

    fake_service.py DIR [--tls-dir PKI]   writes DIR/ports.json = {"plain": N, "tls": {"ec12": N, ...}} and serves until killed
"""
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DESC_KEYS = {"kind", "console", "game", "rom", "title", "nick", "ver", "compat", "pub", "lan", "sym", "relayOnly"}
TOKEN = "AbCdEfGhIjKlMnOpQrStUv"   # 22 characters
STATS = {"rids": [], "requests": 0, "connections": 0, "bad": [], "ua": [], "paths": [], "tls": [], "hello_without_request": 0}
LOCK = threading.Lock()

# code -> (status, error code, extra members of the error object)
SCRIPTS = {
    "E404": (404, "not_found", {}),
    "E409F": (409, "room_full", {}),
    "E409V": (409, "version_mismatch", {"hostVer": "2.0.7"}),
    "E409X": (409, "something_else", {}),
    "E426": (426, "update_required", {"min": "2.1.0", "latest": "2.1.3"}),
    "E503O": (503, "over_budget", {"retryAfterSec": 120, "reason": "busy"}),
    "E503R": (503, "rooms_full", {}),
    "E503S": (503, "switched_off", {"message": "Maintenance"}),
    "E503U": (503, "relay_unavailable", {}),
    "E503B": (503, "relay_budget", {"retryAfterSec": 3600}),
    "E503Z": (503, "weird", {}),
    "E429D": (429, "rate_limited", {"retryAfterSec": 9, "reason": "daily_cap"}),
    "E429S": (429, "rate_limited", {"retryAfterSec": 1, "reason": "slow_down"}),
    "E401": (401, "bad_token", {}),
    "E403": (403, "relay_not_allowed", {}),
    "E502": (502, "turn_upstream", {}),
    "E400": (400, "bad_request", {}),
    "E400V": (400, "bad_version", {}),
    "E400C": (400, "bad_request", {"field": "console"}),       # an older service refusing the console id
    "E400F": (400, "bad_request", {"field": "nick"}),
    "E413": (413, "too_large", {}),
    "E500": (500, "internal", {}),
    "E504": (504, "gateway", {}),
    "E418": (418, "teapot", {}),
}


RID_RE = re.compile(r"[0-9a-f]{16}")


def desc_keys_problem(body):
    """The keys of a create / join body: the known ones, plus an optional well-formed request id (recorded for the tests)."""
    keys = set(body) - {"rid"}
    if "rid" in body and not (isinstance(body["rid"], str) and RID_RE.fullmatch(body["rid"])):
        return "rid"
    if keys != DESC_KEYS:
        return "keys " + ",".join(sorted(keys ^ DESC_KEYS))
    return None


def err_body(status, code, extra):
    e = {"code": code, "message": "scripted " + code}
    e.update(extra)
    return {"error": e}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake"
    sys_version = ""

    def log_message(self, *a):
        pass

    def send_json(self, status, obj, raw=None):
        body = raw if raw is not None else json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("cache-control", "no-store")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def bad(self, why):
        with LOCK:
            STATS["bad"].append(why)
        self.send_json(400, err_body(400, "bad_request", {"message": why}))

    def handle_any(self, method):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/__stats" and not getattr(self.server, "is_tls", False):
            with LOCK:
                return self.send_json(200, STATS)
        n = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(n) if n else b""
        ua = self.headers.get("user-agent", "")
        with LOCK:
            STATS["requests"] += 1
            STATS["ua"].append(ua)
            STATS["paths"].append(method + " " + u.path)
        if not ua:
            return self.bad("empty user agent")
        if not re.fullmatch(r"\d{1,4}(\.\d{1,4}){0,3}", (q.get("v") or [""])[0]):
            return self.send_json(400, err_body(400, "bad_version", {}))
        if len(raw) > 1024:
            return self.send_json(413, err_body(413, "too_large", {}))
        body = None
        if method == "POST":
            if self.headers.get("content-type") != "application/json":
                return self.bad("content-type")
            try:
                body = json.loads(raw)
            except Exception:
                return self.bad("json")
            if not isinstance(body, dict):
                return self.bad("not an object")
        elif raw:
            return self.bad("a GET with a body")
        m = re.fullmatch(r"/v1/rooms(?:/([A-Za-z0-9]{1,8})(?:/(join|poll|relay|endpoint|close))?)?", u.path)
        if u.path == "/v1/info" and method == "GET":
            return self.send_json(200, {"service": "vitadhoc", "min": "2.0.0", "latest": "2.0.4", "relay": True, "busy": False, "roomTtlSec": 600})
        if not m:
            return self.send_json(404, err_body(404, "not_found", {}))
        code, sub = m.group(1), m.group(2)
        if code == "BIG":
            return self.send_json(200, None, raw=b'{"pad":"' + b"x" * 5000 + b'"}')
        if code == "GARBAGE":
            return self.send_json(200, None, raw=b"<html>not json</html>")
        if code == "SLOW":
            time.sleep(0.4)
        if code == "NOBODY":   # the poll of a room that has nobody yet
            pass
        if code in SCRIPTS:
            st, c, extra = SCRIPTS[code]
            return self.send_json(st, err_body(st, c, extra))
        if code is None:   # POST /v1/rooms
            if method != "POST":
                return self.send_json(404, err_body(404, "not_found", {}))
            why = desc_keys_problem(body)
            if why:
                return self.bad(why)
            with LOCK:
                STATS["rids"].append("create " + body.get("rid", "-"))
            return self.send_json(201, {"code": "K7M2Q", "token": TOKEN, "punchKey": "0123456789abcdef", "ttlSec": 600, "pollMs": 2000})
        if sub is None:
            if method != "GET":
                return self.send_json(404, err_body(404, "not_found", {}))
            return self.send_json(200, {"code": code.upper(), "kind": 1, "console": "gba", "game": "BPEE", "rom": "9f3a01c2", "title": "Pokemon Emerald",
                                        "nick": "Alice", "ver": "2.0.0", "compat": "1a2b3c4d", "state": "full" if code == "FULL" else "open", "ageSec": 42})
        if method != "POST":
            return self.send_json(404, err_body(404, "not_found", {}))
        if sub == "join":
            why = desc_keys_problem(body)
            if why:
                return self.bad(why)
            with LOCK:
                STATS["rids"].append("join " + body.get("rid", "-"))
            return self.send_json(200, {"token": TOKEN, "punchKey": "0123456789abcdef", "mode": "relay" if code == "RELAYM" else "punch",
                                        "peer": {"nick": "Alice", "pub": "203.0.113.7:19019", "lan": "192.168.1.23:19019", "sym": 1}, "punchMs": 6000, "pollMs": 1500})
        if body.get("token") != TOKEN:
            return self.send_json(401, err_body(401, "bad_token", {}))
        if sub == "poll":
            extra = set(body) - {"token", "pub"}
            if extra:
                return self.bad("poll keys")
            if code == "NOBODY":
                return self.send_json(200, {"state": "open", "mode": "punch", "peer": None, "relayPeer": "", "reason": "", "punchMs": 4000, "pollMs": 2000})
            if code == "CLOSED":
                return self.send_json(200, {"state": "closed", "mode": "punch", "peer": None, "relayPeer": "", "reason": "cancel", "pollMs": 99999})
            return self.send_json(200, {"state": "relay" if code == "RELAYM" else "joined", "mode": "relay" if code == "RELAYM" else "punch",
                                        "peer": {"nick": "Bob", "pub": "198.51.100.9:40000", "lan": "", "sym": 0}, "relayPeer": "203.0.113.50:49152" if code == "RELAYM" else "",
                                        "reason": "", "punchMs": 4000, "pollMs": 1500, "echo_pub": body.get("pub", "")})
        if sub == "relay":
            if set(body) != {"token"}:
                return self.bad("relay keys")
            servers = [{"urls": ["stun:stun.cloudflare.com:3478", "turn:turn.cloudflare.com:53?transport=udp", "turn:turn.cloudflare.com:443?transport=udp",
                                 "turn:turn.cloudflare.com:3478?transport=udp", "turn:turn.cloudflare.com:80?transport=tcp", "turns:turn.cloudflare.com:5349?transport=tcp"],
                        "username": "u-1700000000:abc", "credential": "c" * 40}]
            if code == "NOCRED":
                servers = [{"urls": ["stun:stun.cloudflare.com:3478"]}]
            return self.send_json(200, {"ttlSec": 3600, "servers": servers})
        if sub == "endpoint":
            if set(body) != {"token", "relay"}:
                return self.bad("endpoint keys")
            return self.send_json(200, {"relayPeer": "203.0.113.50:49152"})
        if sub == "close":
            if set(body) != {"token", "reason"} or body["reason"] not in ("linked", "cancel", "failed"):
                return self.bad("close keys")
            return self.send_json(200, {})
        return self.send_json(404, err_body(404, "not_found", {}))

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")

    def setup(self):
        super().setup()
        with LOCK:
            STATS["connections"] += 1


class TlsServer(ThreadingHTTPServer):
    """Wraps each accepted socket in its own thread, so a failed handshake blocks nobody."""
    daemon_threads = True
    is_tls = True
    allow_reuse_address = True

    def __init__(self, addr, ctx, name):
        super().__init__(addr, Handler)
        self.ctx = ctx
        self.name = name

    def get_request(self):
        sock, addr = self.socket.accept()
        return sock, addr

    def finish_request(self, request, client_address):
        try:
            request.settimeout(10)
            tls = self.ctx.wrap_socket(request, server_side=True)
        except Exception:
            with LOCK:
                STATS["tls"].append({"server": self.name, "failed": True})
            return
        with LOCK:
            STATS["tls"].append({"server": self.name, "version": tls.version(), "cipher": tls.cipher()[0]})
        try:
            Handler(tls, client_address, self)
        except Exception:
            pass


class PlainServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def blackhole(port_holder):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    port_holder.append(s.getsockname()[1])
    conns = []
    while True:
        c, _ = s.accept()
        conns.append(c)   # accepted and never answered


def sh(*a, **k):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **k)


def make_pki(d):
    """A private test CA, a second (untrusted) CA, and leaves: EC and RSA for localhost / 127.0.0.1, one for another name."""
    os.makedirs(d, exist_ok=True)
    def ca(name):
        sh("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", f"{d}/{name}.key", "-out", f"{d}/{name}.pem",
           "-days", "30", "-subj", f"/O=online_http test/CN={name}", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    def leaf(name, issuer, keytype, san):
        if keytype == "ec":
            sh("openssl", "req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", f"{d}/{name}.key", "-out", f"{d}/{name}.csr", "-subj", f"/CN={name}")
        else:
            sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{d}/{name}.key", "-out", f"{d}/{name}.csr", "-subj", f"/CN={name}")
        with open(f"{d}/{name}.ext", "w") as f:
            f.write(f"basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName={san}\n")
        sh("openssl", "x509", "-req", "-in", f"{d}/{name}.csr", "-CA", f"{d}/{issuer}.pem", "-CAkey", f"{d}/{issuer}.key", "-CAcreateserial", "-out", f"{d}/{name}.pem",
           "-days", "30", "-extfile", f"{d}/{name}.ext")
    ca("rootA")
    ca("rootB")
    leaf("ec", "rootA", "ec", "DNS:localhost,IP:127.0.0.1")
    leaf("rsa", "rootA", "rsa", "DNS:localhost,IP:127.0.0.1")
    leaf("wrong", "rootA", "ec", "DNS:other.test")
    leaf("untrusted", "rootB", "ec", "DNS:localhost,IP:127.0.0.1")


def tls_ctx(d, leaf, lo, hi):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = lo
    ctx.maximum_version = hi
    ctx.load_cert_chain(f"{d}/{leaf}.pem", f"{d}/{leaf}.key")
    return ctx


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    ports = {"tls": {}}
    plain = PlainServer(("127.0.0.1", 0), Handler)
    ports["plain"] = plain.server_address[1]
    threading.Thread(target=plain.serve_forever, daemon=True).start()
    if "--tls-dir" in sys.argv:
        d = sys.argv[sys.argv.index("--tls-dir") + 1]
        make_pki(d)
        T12, T13 = ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3
        for name, leaf, lo, hi in (("ec12", "ec", T12, T12), ("ec13", "ec", T13, T13), ("rsa12", "rsa", T12, T12), ("rsa13", "rsa", T13, T13),
                                   ("wrong", "wrong", T12, T13), ("untrusted", "untrusted", T12, T13), ("ecany", "ec", T12, T13)):
            srv = TlsServer(("127.0.0.1", 0), tls_ctx(d, leaf, lo, hi), name)
            ports["tls"][name] = srv.server_address[1]
            threading.Thread(target=srv.serve_forever, daemon=True).start()
    holder = []
    threading.Thread(target=blackhole, args=(holder,), daemon=True).start()
    while not holder:
        time.sleep(0.01)
    ports["blackhole"] = holder[0]
    with open(os.path.join(out, "ports.json.tmp"), "w") as f:
        json.dump(ports, f)
    os.rename(os.path.join(out, "ports.json.tmp"), os.path.join(out, "ports.json"))
    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
