"""Analyse a batch_x20 run: census exactness and per-session frame identity."""
import os
import re
import sys

D = sys.argv[1]
ROMMAP = {'aw2.gba': 'aw2', 'red.gb': 'red', 'emerald.gba': 'emerald',
          'crystal.gbc': 'crystal', 'hns.gba': 'hns'}


def shash_sessions(p):
    out, cur, last = [], None, 0
    for l in open(p):
        if not l.startswith('f='):
            continue
        parts = l.split()
        f = int(parts[0][2:])
        if cur is None or f <= last:
            cur = {}
            out.append(cur)
        last = f
        cur[f] = ' '.join(x for x in parts[1:] if not x.startswith('c=') and not x.startswith('a=') and not x.startswith('n='))
    return out


def log_sessions(p):
    """[(rom, {f: vhash})] in session order, plus census lines."""
    sess, census, other = [], [], []
    for l in open(p, errors='replace'):
        m = re.match(r'EVT inproc_session n=(\d+) rom=.*/([^/]+)$', l.strip())
        if m:
            sess.append((m.group(2), {}))
            continue
        m = re.match(r'EVT vhash f=(\d+) h=([0-9a-f]+)', l)
        if m and sess:
            sess[-1][1][int(m.group(1))] = m.group(2)
            continue
        if l.startswith('EVT heap_census'):
            census.append(l.strip())
        if re.match(r'EVT (inproc_(refused|census_mismatch|return)|exit)', l):
            other.append(l.strip())
    return sess, census, other


def cmp(a, b):
    common = sorted(set(a) & set(b))
    diff = [f for f in common if a[f] != b[f]]
    return len(common), diff


sess, census, other = log_sessions(os.path.join(D, 'X20', 'frontend.log'))
print('sessions:', len(sess), ' returns:', sum('inproc_return' in o for o in other),
      ' refused:', sum('inproc_refused' in o for o in other))
# census exactness
by = {}
for c in census:
    at = re.search(r'at=(\w+)', c).group(1)
    body = c.split(' ', 3)[3]
    by.setdefault(at, []).append(body)
first_pre = by['pre_select'][0]
for at in ('pre_select', 'post_select', 'post_unload'):
    vals = set(by.get(at, []))
    print('census %-11s n=%2d distinct=%d %s' % (at, len(by.get(at, [])), len(vals),
          'EQUAL to first pre_select' if vals == {first_pre} else ''))
print('  value:', first_pre)
for at in ('post_load',):
    vals = by.get(at, [])
    print('census post_load per session (largest/free):')
    for i, v in enumerate(vals):
        m = re.search(r'free=(\d+) top=\d+ largest=(\d+)', v)
        print('   s%02d %-12s free=%s largest=%s' % (i, sess[i][0] if i < len(sess) else '?', m.group(1), m.group(2)))

sh = shash_sessions(os.path.join(D, 'X20', 'shash.txt'))
fresh_sh, fresh_vh = {}, {}
for rom, tag in ROMMAP.items():
    fd = os.path.join(D, 'F-' + tag)
    fresh_sh[rom] = shash_sessions(os.path.join(fd, 'shash.txt'))[-1]
    s2, _, _ = log_sessions(os.path.join(fd, 'frontend.log'))
    fresh_vh[rom] = s2[-1][1]
print('session  rom          vhash(common/diff)   shash(common/diff, GBA only)')
for i, (rom, vh) in enumerate(sess):
    vc, vd = cmp(fresh_vh[rom], vh)
    line = '  s%02d   %-12s %4d/%-3d %-14s' % (i, rom, vc, len(vd), ('f%d..%d' % (vd[0], vd[-1])) if vd else '')
    if rom.endswith('.gba') and i < len(sh):
        sc, sd = cmp(fresh_sh[rom], sh[i])
        line += ' %4d/%-3d %s' % (sc, len(sd), ('f%d..%d' % (sd[0], sd[-1])) if sd else '')
    print(line)
