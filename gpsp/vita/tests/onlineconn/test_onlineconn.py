"""Tests of online_vita.c, the online connection state machine (package M1).

runner.c links online_vita.c as it is and fakes what it stands on: the room
client (online_http.h) as an in-process room service with the plan's routes,
modes, poll table and errors, and the NAT layer (transport_lan.h, section 2.1)
as status that follows a fake clock.  One scenario = one console on a 60 Hz
loop; a ten minute wait costs milliseconds because nothing sleeps.

  python3 test_onlineconn.py            every scenario (about 10 seconds)
  python3 test_onlineconn.py -v         ... and the runner's EVT / REQ lines
  ONLINECONN_SAN=0 python3 ...          without AddressSanitizer / UBSan

The cases that need the real service and the real NAT layer (two runners, the
node service, natsim and turnsim from online_tools.py) are listed at the end
of this file and wait for packages S, N and T1 to be merged.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
VITA = HERE.parents[1]            # gpsp/vita
GPSP = VITA.parent                # gpsp
CC = os.environ.get('CC', 'gcc')


def build(out_dir):
    out = Path(out_dir) / 'runner'
    san = [] if os.environ.get('ONLINECONN_SAN') == '0' else ['-fsanitize=address,undefined']
    cmd = [CC, '-std=gnu99', '-Wall', '-Wextra', '-O1', '-g', *san,
           '-I', str(VITA), '-I', str(GPSP / 'netdrv'), '-I', str(GPSP / 'frontend-common'),
           '-I', str(GPSP / 'common'),
           str(HERE / 'runner.c'), str(VITA / 'online_vita.c'), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('runner did not build:\n' + r.stdout + r.stderr)
    return out


class OnlineConn(unittest.TestCase):
    runner = None
    tmp = None

    @classmethod
    def setUpClass(cls):
        if not shutil.which(CC):
            raise unittest.SkipTest('no C compiler')
        cls.tmp = tempfile.mkdtemp(prefix='onlineconn-')
        cls.runner = build(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)


def scenarios():
    # The list lives in runner.c (one table); asking for it needs a build.
    tmp = tempfile.mkdtemp(prefix='onlineconn-list-')
    try:
        runner = build(tmp)
        out = subprocess.run([str(runner), '--list'], capture_output=True, text=True, check=True)
        return out.stdout.split()
    except Exception:
        return []
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_test(name):
    def test(self):
        args = [str(self.runner), name]
        if '-v' in sys.argv:
            args.append('-v')
        r = subprocess.run(args, capture_output=True, text=True, timeout=300)
        if '-v' in sys.argv:
            sys.stdout.write(r.stdout)
        self.assertIn('SCENARIO %s PASS' % name, r.stdout, r.stdout + r.stderr)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
    return test


for _name in scenarios():
    setattr(OnlineConn, 'test_' + _name, make_test(_name))


# The plan's M1 cases that only the real stack can prove (two runners against
# the node service and natsim/turnsim; none is claimed by the fakes above):
#   - port x port through natsim: both CONNECTED, the room gone after linked,
#     request count <= 12 for a 5 s wait (fakes: host_punch, host_wait_cadence)
#   - sym x port: no PUNCH on the wire at all, relayed within 3 s of the join,
#     the neither log holds the other's 203.0.113.x (fakes: join_relay_sym)
#   - the proxy dropping everything between the NATs: credentials already in
#     hand when the race ends (fakes: punch_race_falls_to_relay)
#   - TURN port 3478 dead / every port dead (fakes: relay_ports)
#   - service evicted while the host waits / between join and first poll: the
#     service's SQLite makes it invisible; only the real service can show it
#   - 426 from MIN_APP_VERSION=9.0.0, over_budget, room TTL 20 s via
#     ROOM_TTL_SEC (fakes: errors, host_ttl), --same-lan (fakes: same_lan)

if __name__ == '__main__':
    unittest.main(argv=[a for a in sys.argv if a != '-v'])
