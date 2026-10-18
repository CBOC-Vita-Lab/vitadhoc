#!/usr/bin/env python3
"""Run GB accuracy/restore regressions; optionally prove v4.0.0 failures.

From gpsp: python3 gbcore/tests/run_accuracy.py [--baseline]
Host GCC, ASan/UBSan and binutils are required (WSL works on Windows).
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
spec = importlib.util.spec_from_file_location("gb_tests", ROOT / "tools/run_gb_tests.py")
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")

def run(cmd, expected=0):
    p = subprocess.run(list(map(str,cmd)), env=env, text=True, capture_output=True)
    print(p.stdout,end="")
    if p.returncode != expected or (expected == 0 and ("runtime error:" in p.stderr or "Sanitizer" in p.stderr)):
        print(p.stderr,end="",file=sys.stderr)
        raise SystemExit(f"FAIL command exit {p.returncode}: {cmd}")
    return p

def compile_core(t, root, extra=()):
    objects=[]
    for name in g.CORE+[g.SHARED]:
        obj=t/(name+".o")
        run(["gcc","-std=gnu11",*g.WARN,*g.SAN,*g.CORE_FLAGS,*extra,"-ffunction-sections","-fdata-sections","-c",root/"gbcore/tgbdual"/name,"-o",obj])
        objects.append(str(obj))
    glue=t/"adapter.o"
    run(["gcc","-std=gnu11",*g.WARN,*g.SAN,*extra,"-ffunction-sections","-fdata-sections","-c",root/"gbcore/gbcore_tgbdual.c","-o",glue])
    return [str(glue),*objects]

def link(t, name, objects, extra=()):
    exe=t/name
    run(["gcc","-std=gnu11",*g.WARN,*g.SAN,*g.CORE_FLAGS,*extra,"-I.","-Ifrontend-common","-o",exe,ROOT/"gbcore/tests"/(name+".c"),*objects,"-lm"])
    return exe

with tempfile.TemporaryDirectory(prefix="gb-accuracy-") as directory:
    t=Path(directory)
    if "--baseline" in sys.argv:
        base=t/"baseline";base.mkdir()
        blob=subprocess.check_output(["git","archive","v4.0.0","gpsp/gbcore","gpsp/frontend-common/fe_console.h"],cwd=ROOT.parent)
        subprocess.run(["tar","-x","-C",str(base)],input=blob,check=True)
        b=t/"old-obj";b.mkdir()
        objects=compile_core(b,base/"gpsp")
        exe=link(b,"accuracy",objects)
        for case in ("mask","apu","state","packet","attr"):
            p=run([exe,case],expected=1)
            if "FAIL" not in p.stderr or "Sanitizer" in p.stderr:
                raise SystemExit("Expected behavioral assertion failure, got: "+p.stderr)
            print("EXPECTED v4.0.0 "+case+" failure: "+p.stderr.strip())
        p=run([exe,"packet-overrun"],expected=1)
        if "AddressSanitizer: global-buffer-overflow" not in p.stderr:
            raise SystemExit("Expected SGB packet buffer overrun, got: "+p.stderr)
        print("EXPECTED v4.0.0 repeated zero-length packets: AddressSanitizer global-buffer-overflow")
    for psp in (False,True):
        b=t/("psp-format" if psp else "host-format");b.mkdir()
        extra=["-DUSE_PSP_RGB565_FORMAT"] if psp else []
        objects=compile_core(b,ROOT,extra)
        run([link(b,"accuracy",objects,extra)])
        if psp or "--focused" in sys.argv: continue
        for name in ("smoke","rtc","serial","sgb_state"):
            run([link(b,name,objects)])
        a,bb=g.build_instances(b,objects[:-1])
        dual=b/"dual"
        run(["gcc","-std=gnu11",*g.WARN,*g.SAN,"-Igbcore","-o",dual,"gbcore/tests/dual.c","gbcore/gbcore_dual.c",a,bb,objects[-1],"-lm"])
        run([dual])
print("PASS accuracy, existing GB functional suites, dual/headless isolation, both RGB565 layouts")
