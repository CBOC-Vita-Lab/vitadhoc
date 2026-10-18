"""Guest-instruction timer regressions; --baseline proves checkpoint failures."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
spec=importlib.util.spec_from_file_location('g',ROOT/'tools/run_gb_tests.py')
g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
def run(cmd):
    p=subprocess.run(list(map(str,cmd)),env=env,text=True,capture_output=True)
    print(p.stdout,end='');print(p.stderr,end='',file=sys.stderr)
    if p.returncode: raise SystemExit(p.returncode)
with tempfile.TemporaryDirectory() as directory:
    t=Path(directory);objects=[]
    for name in g.CORE+[g.SHARED]:
        source=ROOT/'gbcore/tgbdual'/name
        if name=='cpu.c' and '--baseline' in sys.argv:
            source=t/'cpu.c'
            source.write_bytes(subprocess.check_output(['git','show','4cd524c:gpsp/gbcore/tgbdual/cpu.c'],cwd=ROOT.parent)+b'''
/* Link-only compatibility for newer test/state callers; old CPU has no reload state. */
void cpu_save_timer_timing(byte *out) { memset(out,0,8); }
void cpu_load_timer_timing(const byte *in) { (void)in; }
int cpu_timer_timing_valid(const byte *in) { (void)in; return 1; }
''')
            (t/'cpu_io.h').write_bytes(subprocess.check_output(['git','show','4cd524c:gpsp/gbcore/tgbdual/cpu_io.h'],cwd=ROOT.parent))
        o=t/(name+'.o');run(['gcc','-std=gnu11',*g.WARN,*g.SAN,*g.CORE_FLAGS,'-Igbcore/tgbdual','-c',source,'-o',o]);objects.append(o)
    o=t/'adapter.o';run(['gcc','-std=gnu11',*g.WARN,*g.SAN,'-c',ROOT/'gbcore/gbcore_tgbdual.c','-o',o]);objects.append(o)
    exe=t/'timer';run(['gcc','-std=gnu11',*g.WARN,*g.SAN,*g.CORE_FLAGS,'-I.',ROOT/'gbcore/tests/timer.c',*objects,'-lm','-o',exe])

    failures=0
    for case in ('div','boundary','multi','phase','tac','reload','reload-state','halt'):
        p=subprocess.run([str(exe),case],env=env,text=True,capture_output=True)
        print(case+': '+p.stdout+p.stderr,end='')
        if 'Sanitizer' in p.stderr or 'runtime error:' in p.stderr:
            raise SystemExit('Unexpected sanitizer diagnostic')
        if p.returncode: failures+=1
        if '--baseline' in sys.argv and (p.returncode!=1 or 'FAIL line' not in p.stderr): raise SystemExit('Expected baseline assertion failure')


    if failures and '--baseline' not in sys.argv: raise SystemExit(1)
