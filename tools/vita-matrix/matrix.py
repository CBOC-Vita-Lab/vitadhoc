#!/usr/bin/env python3
"""vita-matrix: run a playlist of timed measurements unattended and tabulate the logs.

One playlist line per measurement:  game | duration | key=value ... | label
(see README.md).  For each line the tool writes the overrides into the app's data
folder, starts the app, waits for it to leave, pulls the logs back and stores them.
Whatever happens (end of list, Ctrl-C, an error) the owner's own vita.ini,
snes/threads.ini, autoboot.txt and the two log files are put back exactly as they
were; a copy is kept on this machine first, so `--restore DIR` can finish the job
if this program itself is killed.

Backends (same interface):  vc   a real Vita through vitacompanion (env VITA = its
                                 address; FTP 1337, commands 1338)
                            v3k  the local Vita3K emulator (rehearsal only; its
                                 timing is not the Vita's)
                            dir  just a folder, nothing is launched (tests)
"""
import argparse, hashlib, json, os, re, shutil, signal, socket, subprocess, sys, time

APP_ID = "GBAD00001"
# Files this tool rewrites, relative to the app's data folder.  The two logs are
# included so that the logs pulled for an entry belong to that entry alone and the
# owner's own logs come back afterwards.
VITA_INI, THREADS_INI, AUTOBOOT = "vita.ini", "snes/threads.ini", "autoboot.txt"
CONFIG_INI = "config.ini"   # the owner's Settings: c.<key> overrides are MERGED into his file
GBA_LOG, SNES_LOG = "log/gbadhoc.log", "snes/log.txt"
MANAGED = [VITA_INI, THREADS_INI, AUTOBOOT, CONFIG_INI, GBA_LOG, SNES_LOG]
SNES_EXT = (".smc", ".sfc", ".swc", ".fig")
FPS = 60.0   # frames per second used to turn seconds into autoexit_frames


# ---------------------------------------------------------------- playlist ----
class PlaylistError(Exception):
    pass


class Entry:
    def __init__(self, line_no, game, secs, vita, threads, label, config=None):
        self.line_no, self.game, self.secs = line_no, game, secs
        self.vita, self.threads, self.label = vita, threads, label
        self.config = config or {}

    @property
    def is_snes(self):
        return self.game.lower().endswith(SNES_EXT)

    def to_json(self):
        return dict(line=self.line_no, game=self.game, secs=self.secs,
                    vita=self.vita, threads=self.threads, config=self.config,
                    label=self.label)


def parse_duration(text, default_secs):
    t = text.strip().lower()
    if not t:
        return float(default_secs)
    m = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?)\s*(s|sec|f|frames?)?", t)
    if not m:
        raise PlaylistError("bad duration %r (use 60s or 3600f)" % text)
    v = float(m.group(1))
    return v / FPS if (m.group(2) or "").startswith("f") else v


def parse_overrides(text, vita, threads, config=None):
    """key=value tokens; a `t.` prefix sends the key to snes/threads.ini, a `c.`
    prefix to config.ini (merged into the owner's own Settings file)."""
    for tok in text.split():
        if "=" not in tok:
            raise PlaylistError("override %r is not key=value" % tok)
        k, v = tok.split("=", 1)
        if not k:
            raise PlaylistError("empty key in %r" % tok)
        if k.startswith("t."):
            threads[k[2:]] = v
        elif k.startswith("c."):
            if config is None:
                raise PlaylistError("c. keys are not allowed here")
            config[k[2:]] = v
        else:
            vita[k] = v


def strip_comment(line):
    if line.lstrip().startswith("#"):
        return ""
    return re.split(r"\s#", line, maxsplit=1)[0].strip()


def parse_playlist(text, default_secs=60):
    entries, base_v, base_t, base_c = [], {}, {}, {}
    for no, raw in enumerate(text.splitlines(), 1):
        line = strip_comment(raw)
        if not line:
            continue
        try:
            if line.startswith("@base"):
                # @base k=v ...  sets the overrides every later entry starts from
                # (an empty @base clears them); an entry's own keys win.
                base_v, base_t, base_c = {}, {}, {}
                parse_overrides(line[5:], base_v, base_t, base_c)
                continue
            f = [p.strip() for p in line.split("|")]
            if len(f) > 4 or not f[0]:
                raise PlaylistError("expected: game | duration | key=value ... | label")
            f += [""] * (4 - len(f))
            secs = parse_duration(f[1], default_secs)
            v, t, c = dict(base_v), dict(base_t), dict(base_c)
            parse_overrides(f[2], v, t, c)
            entries.append(Entry(no, f[0], secs, v, t, f[3], c))
        except PlaylistError as e:
            raise PlaylistError("playlist line %d: %s" % (no, e))
    return entries


def merge_config(owner, keys):
    """The owner's config.ini (bytes or None) with `keys` set: existing lines are
    rewritten in place, new keys appended; everything else is kept byte for byte."""
    lines = (owner or b"").decode("utf-8", "replace").splitlines()
    left = dict(keys)
    out = []
    for l in lines:
        k = l.split("=", 1)[0].strip() if "=" in l and not l.lstrip().startswith(("#", ";")) else None
        if k in left:
            out.append("%s = %s" % (k, left.pop(k)))
        else:
            out.append(l)
    out += ["%s = %s" % kv for kv in left.items()]
    return ("\n".join(out) + "\n").encode()


def render_ini(d):
    return "".join("%s=%s\n" % (k, v) for k, v in d.items()).encode()


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s).strip("-")[:48] or "entry"


# ---------------------------------------------------------------- backends ----
class Backend:
    """get/put/delete take paths relative to the data folder (ux0:data/GBAdhoc)."""
    name = "?"
    startup_s = 40       # time allowed for the app to come up

    def get(self, rel):          # -> bytes, or None when the file does not exist
        raise NotImplementedError

    def put(self, rel, data):
        raise NotImplementedError

    def delete(self, rel):       # no error when already absent
        raise NotImplementedError

    def launch(self):
        pass

    def running(self):           # True while the app is up (None = unknown)
        return None

    def stop(self):
        pass

    def extra_logs(self):        # {name: bytes} other logs worth keeping
        return {}

    def close(self):
        pass


class DirBackend(Backend):
    name = "dir"

    def __init__(self, root):
        self.root = root

    def _p(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def get(self, rel):
        try:
            with open(self._p(rel), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def put(self, rel, data):
        p = self._p(rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def delete(self, rel):
        try:
            os.remove(self._p(rel))
        except FileNotFoundError:
            pass


class V3kBackend(DirBackend):
    """Vita3K on a virtual display, started the way the emulator kit does (virtual display, software Vulkan)."""
    name = "v3k"
    startup_s = 60
    PREF = os.environ.get("V3K_PREF", os.path.expanduser("~/.local/share/Vita3K/Vita3K"))
    BIN = os.environ.get("V3K_BIN", "")      # the Vita3K AppRun / executable

    def __init__(self, root=None):
        super().__init__(root or os.path.join(self.PREF, "ux0", "data", "GBAdhoc"))
        self.proc = self.xvfb = None
        self.out_log = None

    def launch(self):
        if not self.BIN:
            raise RuntimeError("set V3K_BIN to the Vita3K executable")
        d = 50 + os.getpid() % 400
        env = dict(os.environ, VK_ICD_FILENAMES="/opt/lavapipe/lvp_icd.json",
                   VK_DRIVER_FILES="/opt/lavapipe/lvp_icd.json", SHARUN_ALLOW_SYS_VKICD="1",
                   GALLIVM_PERF="no_aos_sampling", XDG_RUNTIME_DIR="/tmp/xdg",
                   SDL_AUDIODRIVER="dummy", DISPLAY=":%d" % d)
        os.makedirs("/tmp/xdg", mode=0o700, exist_ok=True)
        self.xvfb = subprocess.Popen(["Xvfb", ":%d" % d, "-screen", "0", "960x544x24", "-nolisten", "tcp"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        self.out_log = open("/tmp/vita-matrix-v3k-%d.log" % os.getpid(), "wb")
        self.proc = subprocess.Popen([self.BIN, "--frontend", "sdl", "-r", APP_ID], env=env,
                                     stdout=self.out_log, stderr=subprocess.STDOUT,
                                     start_new_session=True)

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGINT)
                self.proc.wait(15)
            except Exception:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except Exception:
                    pass
        if self.xvfb is not None:
            self.xvfb.terminate()
        self.proc = self.xvfb = None

    def extra_logs(self):
        if self.out_log is None:
            return {}
        self.out_log.flush()
        try:
            with open(self.out_log.name, "rb") as f:
                return {"vita3k.log": f.read()}
        finally:
            self.out_log.close()
            self.out_log = None


class VcBackend(Backend):
    """A real Vita through vitacompanion: FTP on 1337, command server on 1338."""
    name = "vc"
    startup_s = 40
    DATA = "ux0:/data/GBAdhoc"

    def __init__(self, host):
        if not host:
            raise SystemExit("set the Vita's address in the environment variable VITA")
        self.host = host

    def _ftp(self):
        import ftplib
        last = None
        for _ in range(4):               # the Vita's FTP drops connections now and then
            try:
                f = ftplib.FTP()
                f.connect(self.host, 1337, timeout=30)
                f.login()
                return f
            except Exception as e:
                last = e
                time.sleep(2)
        raise RuntimeError("cannot reach the Vita's FTP: %s" % last)

    def _path(self, rel):
        return "%s/%s" % (self.DATA, rel)

    def _listing(self, f, rel):
        d, _, name = rel.rpartition("/")
        lines = []
        try:
            f.retrlines("LIST " + self._path(d) if d else "LIST " + self.DATA, lines.append)
        except Exception:
            return False
        return any(l.split()[-1:] == [name] or l.endswith(" " + name) for l in lines)

    def get(self, rel):
        import ftplib
        f = self._ftp()
        try:
            buf = []
            try:
                f.retrbinary("RETR " + self._path(rel), buf.append)
            except ftplib.error_perm:
                if self._listing(f, rel):
                    raise
                return None
            return b"".join(buf)
        finally:
            try:
                f.close()
            except Exception:
                pass

    def put(self, rel, data):
        import io
        f = self._ftp()
        try:
            d = rel.rpartition("/")[0]
            if d:
                try:
                    f.mkd(self._path(d))
                except Exception:
                    pass
            f.storbinary("STOR " + self._path(rel), io.BytesIO(data))
        finally:
            f.close()
        if self.get(rel) != data:
            raise RuntimeError("upload of %s did not read back identically" % rel)

    def delete(self, rel):
        import ftplib
        if self.get(rel) is None:
            return
        f = self._ftp()
        try:
            f.delete(self._path(rel))
        except ftplib.error_perm as e:
            raise RuntimeError("cannot delete %s on the Vita: %s" % (rel, e))
        finally:
            f.close()
        if self.get(rel) is not None:
            raise RuntimeError("%s is still there after delete" % rel)

    def cmd(self, line):
        s = socket.create_connection((self.host, 1338), timeout=20)
        try:
            s.sendall((line + "\n").encode())
            s.settimeout(5)
            out = b""
            try:
                while True:
                    c = s.recv(4096)
                    if not c:
                        break
                    out += c
            except socket.timeout:
                pass
            return out.decode(errors="replace").strip()
        finally:
            s.close()

    def launch(self):
        self.cmd("quit all")
        time.sleep(3)
        self.cmd("launch " + APP_ID)

    def stop(self):
        try:
            self.cmd("quit all")
        except Exception:
            pass


# ------------------------------------------------------------ backup/restore ----
class Session:
    """Keeps the owner's files safe.  backup() writes DIR/manifest.json plus a copy
    of each file BEFORE anything is changed; restore() puts every file back (or
    removes it again if it did not exist) and checks by reading it back."""

    def __init__(self, backend, bdir):
        self.b, self.dir = backend, bdir

    def backup(self):
        os.makedirs(os.path.join(self.dir, "files"), exist_ok=True)
        man = {}
        for rel in MANAGED:
            data = self.b.get(rel)
            if data is None:
                man[rel] = dict(existed=False)
            else:
                with open(os.path.join(self.dir, "files", slug(rel)), "wb") as f:
                    f.write(data)
                man[rel] = dict(existed=True, sha256=hashlib.sha256(data).hexdigest(), size=len(data))
        tmp = os.path.join(self.dir, "manifest.json.tmp")
        with open(tmp, "w") as f:
            json.dump(man, f, indent=1)
        os.replace(tmp, os.path.join(self.dir, "manifest.json"))
        return man

    def restore(self):
        """returns a list of problems (empty = all back as they were)"""
        with open(os.path.join(self.dir, "manifest.json")) as f:
            man = json.load(f)
        problems = []
        for rel, m in man.items():
            try:
                if m["existed"]:
                    with open(os.path.join(self.dir, "files", slug(rel)), "rb") as f:
                        want = f.read()
                    if self.b.get(rel) != want:
                        self.b.put(rel, want)
                    if self.b.get(rel) != want:
                        problems.append("%s: differs after restore" % rel)
                else:
                    self.b.delete(rel)
                    if self.b.get(rel) is not None:
                        problems.append("%s: should not exist but does" % rel)
            except Exception as e:
                problems.append("%s: %s" % (rel, e))
        return problems


# ------------------------------------------------------------------ log parsing ----
KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s|]+)")
# The counter of missed display refreshes is still being added by another agent;
# any of these names is accepted (first one found wins).  Add the final name here.
VC_MISS_KEYS = ["vc_miss", "vcount_miss", "vcount_misses", "vc_missed", "vcmiss", "vcmisses"]
FAULT_PATTERNS = [r"invalid_access", r"\|C\|", r"Unhandled access", r"\bEVT\s+(?:crash|fatal|abort|panic)",
                  r"\bfault=(?!0(?:/0)*(?:\s|$))\d", r"autoboot missing", r"cannot start"]


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


class Logs:
    def __init__(self, gba="", snes="", host="", secs=0):
        self.gba, self.snes, self.host, self.secs = gba or "", snes or "", host or "", secs
        self.evts = []     # (name, {k: v}, ts_s or None)
        for ln in self.gba.splitlines():
            m = re.match(r"\s*EVT\s+([A-Za-z0-9_]+):?\s*(.*)", ln)
            if not m:
                continue
            body = m.group(2)
            kv = dict(KV.findall(body))
            if m.group(1) == "vita_jit_worst":     # only the part before '|' is the worst frame
                kv = dict(KV.findall(body.split("|", 1)[0]))
            ts = num(kv.get("ts_us"))
            self.evts.append((m.group(1), kv, ts / 1e6 if ts is not None else None))
        self.exited = bool(re.search(r"\bEVT exit\b", self.gba))

    def of(self, name):
        return [(kv, ts) for n, kv, ts in self.evts if n == name]


def fmt(v, p=1):
    return "-" if v is None else ("%.*f" % (p, v))


def windows_rate(samples, key_list):
    """samples: [(kv, ts)].  Value per second, averaged and the worst window."""
    vals = []
    for kv, ts in samples:
        for k in key_list:
            if k in kv and num(kv[k]) is not None:
                vals.append((num(kv[k]), ts, kv))
                break
    if not vals:
        return None
    rates = []
    for i, (v, ts, kv) in enumerate(vals):
        if ts is not None and i > 0 and vals[i - 1][1] is not None and ts > vals[i - 1][1]:
            d = ts - vals[i - 1][1]
        elif num(kv.get("dt_us")):
            d = num(kv["dt_us"]) / 1e6
        elif num(kv.get("loops")):
            d = num(kv["loops"]) / FPS
        else:
            d = 1.0     # the once-a-second lines
        rates.append(v / d)
    return sum(rates) / len(rates), max(rates)


def window_counts(samples, key_list):
    """samples: [(kv, ts)] of a line that is printed once a second with a count
    for that second (handoff: vc_miss=): the mean and the worst second."""
    v = []
    for kv, _ in samples:
        for k in key_list:
            if k in kv and num(kv[k]) is not None:
                v.append(num(kv[k]))
                break
    return None if not v else (sum(v) / len(v), max(v))


def col_miss(L):
    # vc_miss= is the count of refreshes with no new picture in the second
    # the handoff: line covers (main_vita.c); vita_gxm repeats it, so only
    # the handoff: lines are read, and nothing is divided by line spacing.
    r = window_counts(L.of("handoff:"), VC_MISS_KEYS) or window_counts(L.of("handoff"), VC_MISS_KEYS)
    tag = ""
    if r is None:
        r, tag = windows_rate(L.of("vita_win"), ["late"]), " L"      # fallback: late= of vita_win
    if r is None:
        r, tag = windows_rate(L.of("fps"), ["skipped"]), " S"        # fallback: fps skipped=
    return "-" if r is None else "%s (max %s)%s" % (fmt(r[0], 2), fmt(r[1], 2), tag)


def col_emu(L):
    v = [num(kv["emu"]) for kv, _ in L.of("fps") if num(kv.get("emu")) is not None]
    return "-" if not v else "%s (min %s)" % (fmt(sum(v) / len(v)), fmt(min(v)))


def col_handoff(L):
    h = L.of("handoff")
    if not h:
        return "-"
    t = sum(num(kv.get("timeouts")) or 0 for kv, _ in h)
    d = sum(num(kv.get("dropped")) or 0 for kv, _ in h)
    return "t%d/d%d" % (t, d)


def col_look(L):
    best = None
    for kv, _ in L.of("look"):
        m = re.match(r"(\d+)/(\d+)", kv.get("lvl", ""))
        if m and (best is None or int(m.group(1)) > best[0]):
            best = (int(m.group(1)), m.group(2))
    return "-" if best is None else "%d/%s" % best


def col_jit(L):
    j = L.of("vita_jit_worst")
    if not j:
        return "-"
    x = [num(kv.get("xlat_us")) for kv, _ in j if num(kv.get("xlat_us")) is not None]
    s = [num(kv.get("sync_us")) for kv, _ in j if num(kv.get("sync_us")) is not None]
    return "%s/%s" % (("%d" % max(x)) if x else "-", ("%d" % max(s)) if s else "-")


def col_snes_fps(L):
    v = [float(m.group(1)) for m in re.finditer(r"(?:^|\s)t=\d+ fps=([0-9.]+)", L.snes, re.M)]
    return "-" if not v else "%s (min %s)" % (fmt(sum(v) / len(v)), fmt(min(v)))


def col_snes_audio(L):
    rs, ur = [], None
    for m in re.finditer(r"(?:^|\s)A t=\d+ .*?ratio=([+-]?\d+)/([+-]?\d+)/([+-]?\d+)", L.snes, re.M):
        rs.append(int(m.group(2)))
    for m in re.finditer(r"\bur=(\d+)/(\d+)", L.snes):
        ur = m.group(2)
    if not rs:
        return "-"
    return "r%+d ur%s" % (round(sum(rs) / len(rs)), ur if ur is not None else "-")


def col_faults(L):
    n, first = 0, None
    for blob in (L.gba, L.snes, L.host):
        for ln in blob.splitlines():
            for p in FAULT_PATTERNS:
                if re.search(p, ln):
                    n += 1
                    first = first or ln.strip()[:40]
                    break
    if not (L.gba or L.snes or L.host):
        return "-"
    return "0" if n == 0 else "%d: %s" % (n, first)


# one row of the table = one entry: (heading, function).  Add a column here.
COLUMNS = [("missed/s", col_miss), ("emu fps", col_emu), ("handoff", col_handoff),
           ("look", col_look), ("jit xlat/sync us", col_jit),
           ("snes fps", col_snes_fps), ("snes audio", col_snes_audio), ("faults", col_faults)]


def summarize_row(meta, L):
    row = [meta.get("name", "?"), meta.get("status", "-")]
    for _, fn in COLUMNS:
        try:
            row.append(fn(L))
        except Exception:
            row.append("-")
    return row


def render_table(rows):
    head = ["entry", "end"] + [c[0] for c in COLUMNS]
    w = [max(len(str(r[i])) for r in [head] + rows) for i in range(len(head))]
    line = lambda r: "  ".join(str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip()
    return "\n".join([line(head), line(["-" * x for x in w])] + [line(r) for r in rows])


# ---------------------------------------------------------------------- the run ----
def decode(b):
    return b.decode("utf-8", "replace") if b else ""


def wait_for_exit(be, entry, log):
    """returns 'ok' (app left by itself), 'timeout' (we stopped it) or 'timed' (SNES: stopped at the time)"""
    start = time.time()
    limit = be.startup_s + entry.secs * 1.3 + 20
    if entry.is_snes:
        limit = be.startup_s + entry.secs
    while True:
        time.sleep(min(5, max(0.5, entry.secs / 20.0)) if be.name != "dir" else 0.2)
        el = time.time() - start
        run = be.running()
        if run is False:
            return "ok"
        if run is None or be.name == "vc":
            g = be.get(GBA_LOG)
            if g and re.search(rb"\bEVT exit\b", g):
                return "ok"
        if el >= limit:
            return "timed" if entry.is_snes else "timeout"


def run_entry(be, entry, idx, outdir, merge_base=None, log=print, owner_config=None):
    name = "%02d-%s" % (idx, slug(entry.game))
    if entry.label:
        name += "-" + slug(entry.label)
    edir = os.path.join(outdir, name)
    os.makedirs(edir, exist_ok=True)
    meta = dict(entry.to_json(), name=name, index=idx)
    # a clean slate: the app's own old logs must not be mistaken for this entry's
    be.delete(GBA_LOG)
    be.delete(SNES_LOG)
    vita = dict(merge_base or {})
    vita.update(entry.vita)
    if "autoexit_frames" not in vita:
        vita["autoexit_frames"] = str(int(round(entry.secs * FPS)))
    be.put(VITA_INI, render_ini(vita))
    if entry.threads:
        be.put(THREADS_INI, render_ini(entry.threads))
    else:
        be.delete(THREADS_INI)       # the owner's own threads.ini must not colour a baseline
    # config.ini: the owner's own file every time, plus this entry's c. keys
    # (a previous entry's keys never leak into the next one)
    if entry.config:
        be.put(CONFIG_INI, merge_config(owner_config, entry.config))
    elif owner_config is not None:
        be.put(CONFIG_INI, owner_config)
    be.put(AUTOBOOT, (entry.game + "\n").encode())
    meta["vita_ini"], meta["threads_ini"] = vita, entry.threads
    meta["config_keys"] = entry.config
    status = "ok"
    try:
        be.launch()
        status = wait_for_exit(be, entry, log)
    finally:
        be.stop()
        time.sleep(1 if be.name != "dir" else 0)
    g, s = be.get(GBA_LOG), be.get(SNES_LOG)
    for fn, data in (("gbadhoc.log", g), ("snes-log.txt", s)):
        if data is not None:
            with open(os.path.join(edir, fn), "wb") as f:
                f.write(data)
    host = ""
    for fn, data in be.extra_logs().items():
        with open(os.path.join(edir, fn), "wb") as f:
            f.write(data)
        host += decode(data)
    if g is None and s is None:
        status = "no-log"
    meta["status"] = status
    with open(os.path.join(edir, "entry.json"), "w") as f:
        json.dump(meta, f, indent=1)
    return meta, Logs(decode(g), decode(s), host, entry.secs)


def load_dir(outdir):
    rows = []
    for name in sorted(os.listdir(outdir)):
        mp = os.path.join(outdir, name, "entry.json")
        if not os.path.isfile(mp):
            continue
        with open(mp) as f:
            meta = json.load(f)

        def rd(fn):
            p = os.path.join(outdir, name, fn)
            return open(p, "rb").read().decode("utf-8", "replace") if os.path.isfile(p) else ""
        rows.append(summarize_row(meta, Logs(rd("gbadhoc.log"), rd("snes-log.txt"), rd("vita3k.log"), meta.get("secs", 0))))
    return rows


def make_backend(a):
    if a.backend == "vc":
        return VcBackend(os.environ.get("VITA", ""))
    if a.backend == "v3k":
        return V3kBackend(a.data_dir)
    if not a.data_dir:
        raise SystemExit("--backend dir needs --data-dir")
    return DirBackend(a.data_dir)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("playlist", nargs="?")
    ap.add_argument("--backend", choices=["vc", "v3k", "dir"], default="vc")
    ap.add_argument("--data-dir", help="v3k/dir: the GBAdhoc data folder (v3k default: Vita3K's)")
    ap.add_argument("--out", help="results folder (default: ./vita-matrix-out/<time>)")
    ap.add_argument("--default-secs", type=float, default=60)
    ap.add_argument("--only", help="run only these entry numbers, e.g. 1,3,5-7")
    ap.add_argument("--merge", action="store_true",
                    help="start each vita.ini from the owner's own instead of from empty")
    ap.add_argument("--keep-going", action="store_true", help="after a failed entry go on with the next")
    ap.add_argument("--dry-run", action="store_true", help="parse and print the plan, touch nothing")
    ap.add_argument("--restore", metavar="OUTDIR", help="put the owner's files back from OUTDIR/backup")
    ap.add_argument("--summarize", metavar="OUTDIR", help="re-print the table of an earlier run")
    a = ap.parse_args(argv)

    if a.summarize:
        print(render_table(load_dir(a.summarize)))
        return 0
    if a.restore:
        be = make_backend(a)
        probs = Session(be, os.path.join(a.restore, "backup")).restore()
        print("restore: " + ("all files back as they were" if not probs else "PROBLEMS:\n  " + "\n  ".join(probs)))
        return 0 if not probs else 3
    if not a.playlist:
        ap.error("playlist needed")
    with open(a.playlist) as f:
        entries = parse_playlist(f.read(), a.default_secs)
    sel = None
    if a.only:
        sel = set()
        for part in a.only.split(","):
            lo, _, hi = part.partition("-")
            sel.update(range(int(lo), int(hi or lo) + 1))
    plan = [(i, e) for i, e in enumerate(entries, 1) if sel is None or i in sel]
    total = sum(e.secs + 60 for _, e in plan)
    for i, e in plan:
        print("%2d  %-40s %5.0fs  %s %s" % (i, e.game, e.secs,
              " ".join("%s=%s" % kv for kv in e.vita.items()),
              " ".join("t.%s=%s" % kv for kv in e.threads.items())))
    print("%d entries, about %d minutes" % (len(plan), total / 60))
    if a.dry_run or not plan:
        return 0

    out = a.out or os.path.join("vita-matrix-out", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    be = make_backend(a)
    sess = Session(be, os.path.join(out, "backup"))
    rows, rc = [], 0
    owner = {}
    prev = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}

    def on_term(sig, frm):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, on_term)
    try:
        sess.backup()
        owner_cfg = be.get(CONFIG_INI)
        print("owner's files saved in %s (restore with --restore %s)" % (os.path.join(out, "backup"), out))
        if a.merge:
            owner = dict(l.split("=", 1) for l in decode(be.get(VITA_INI)).replace(" = ", "=").splitlines() if "=" in l)
        for i, e in plan:
            print("[%d/%d] %s  (%.0fs)" % (i, len(entries), e.game, e.secs), flush=True)
            try:
                meta, L = run_entry(be, e, i, out, owner, owner_config=owner_cfg)
                rows.append(summarize_row(meta, L))
            except KeyboardInterrupt:
                raise
            except Exception as ex:
                print("  FAILED: %s" % ex)
                rows.append(["%02d-%s" % (i, slug(e.game)), "ERR"] + ["-"] * len(COLUMNS))
                rc = 2
                if not a.keep_going:
                    break
    except KeyboardInterrupt:
        print("\ninterrupted: stopping the app and restoring")
        rc = 130
    except Exception as ex:
        print("FAILED: %s" % ex)
        rc = 2
    finally:
        for s in prev:                       # no second Ctrl-C may cut the restore short
            signal.signal(s, signal.SIG_IGN)
        try:
            be.stop()
        except Exception:
            pass
        probs = []
        if os.path.isfile(os.path.join(out, "backup", "manifest.json")):
            try:
                probs = sess.restore()
            except Exception as ex:
                probs = [str(ex)]
        if probs:
            print("RESTORE PROBLEMS (fix by hand or run --restore %s):\n  %s" % (out, "\n  ".join(probs)))
            rc = 3
        else:
            print("restore: the owner's files are back exactly as they were")
        for s, h in prev.items():
            signal.signal(s, h)
        be.close()
    if rows:
        table = render_table(rows)
        print("\n" + table)
        with open(os.path.join(out, "summary.txt"), "w") as f:
            f.write(table + "\n")
    print("results in %s" % out)
    return rc


if __name__ == "__main__":
    sys.exit(main())
