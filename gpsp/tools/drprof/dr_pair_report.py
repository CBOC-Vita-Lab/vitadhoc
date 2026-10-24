#!/usr/bin/env python3
"""dr_pair_report.py RUNDIR SCENE FRAMES [--top N]

The pair twin's profile (tools/drprof/Makefile `pair`; gl_host under qemu with
drprof_plugin.so) as one table per machine.  All numbers are DYNAMIC MIPS
INSTRUCTIONS PER FRAME on the twin -- not time.

Machine A is the core as linked: its emitted code is split by the plugin
(jit:<class>:<mode>:<role>, stub:<offset>), its C code per symbol.  Machine B
is the renamed copy (gbab_*): its C code per symbol with the category of the
symbol it was renamed from; its emitted code is three buckets (the first
16 KiB of its ROM cache = the emitter's stubs plus the first blocks, the rest
of both caches = "?", its RAM cache), reported as one line.

Excluded from every "main CPU" total: the host (gl_host, the autopilot, the
per-frame hash lk_hash) and machine A's renderer (on a PSP it runs on the
Media Engine; the main CPU's capture side is not in this build).
"""
import collections
import re
import sys

CATS = ["jit", "jmem", "stub", "memc", "disp", "jdisp", "cyc", "upd", "xlat",
        "flush", "video", "sound", "core", "libc", "host", "other"]
HOSTY = {"lk_hash", "lk_fnv", "on_frame", "emit_hash"}
LINK = re.compile(r"^(gbalink_|lk_)")
IRQ = {"check_and_raise_interrupts", "set_cpu_mode", "execute_store_cpsr",
       "execute_store_cpsr_body", "execute_spsr_restore",
       "execute_spsr_restore_body", "check_interrupt", "flag_interrupt",
       "execute_swi", "execute_swi_hle_div", "cpu_set_irq"}
ROLE_GROUP = {"callmem": "memory: call setup in emitted code",
              "arg": "memory: call setup in emitted code",
              "dispj": "dispatch: jumps to the dispatcher (emitted)",
              "cyc": "cycles: update + budget check (emitted)",
              "cycchk": "cycles: update + budget check (emitted)"}


def main():
    run, scene, frames = sys.argv[1], sys.argv[2], float(sys.argv[3])
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 20
    cat = {}
    for l in open(run + "/symcat.txt"):
        f = l.split()
        if len(f) == 4:
            cat[f[3]] = CATS[int(f[2])]
    stubname = []
    try:
        for l in open(run + "/stubmap.txt"):
            o, n = l.split()
            stubname.append((int(o, 16), n))
        stubname.sort()
    except OSError:
        pass

    def stub_of(off):
        name = "stub(other)"
        for o, n in stubname:
            if o <= off:
                name = n
            else:
                break
        return name

    B = collections.Counter()
    for l in open(run + "/prof.txt", errors="replace"):
        f = l.split()
        if len(f) == 4 and f[0] == "S" and f[1] == scene:
            B[f[2]] += int(f[3])
    A = collections.Counter()      # machine A groups
    Bm = collections.Counter()     # machine B groups
    items = collections.Counter()  # (machine, item) -> instr
    calls = {}
    host = 0
    mc = collections.Counter()
    for k, v in B.items():
        kind, _, rest = k.partition(":")
        if kind == "ent":
            calls[rest] = v
            continue
        if kind == "gi":
            continue
        if kind == "mc":
            cls, _, off = rest.rpartition(":")
            mc[stub_of(int(off, 16))] += v
            continue
        if kind == "jit":
            role = rest.rsplit(":", 1)[-1]
            g = ROLE_GROUP.get(role, "translated guest code (emitted, guest work)")
            A[g] += v
            items[("A", g)] += v
            continue
        if kind == "stub":
            n = stub_of(int(rest, 16))
            A["memory: emitter stubs"] += v
            items[("A", "stub " + n)] += v
            continue
        if kind == "zone":
            A["translate / flush (zones)"] += v
            items[("A", "zone " + rest)] += v
            continue
        if kind != "sym":
            continue
        name = rest
        if name == "?" or name in ("gbab_rom_translation_cache",
                                   "gbab_ram_translation_cache"):
            Bm["emitted code + stubs (all)"] += v
            items[("B", "emitted code + stubs (all)")] += v
            continue
        isb = name.startswith("gbab_")
        base = name[5:] if isb else name
        c = cat.get(name, "host")
        tgt = Bm if isb else A
        m = "B" if isb else "A"
        if base in HOSTY or c == "host" or (c == "libc" and not isb and base in ("main",)):
            host += v
            continue
        if LINK.match(base):
            g = "cable hooks + pair stepper"
        elif base in IRQ:
            g = "interrupt entry/return (C: mode switch, CPSR, raise)"
        elif c == "upd":
            g = "event scheduler (update_gba, mips_update_gba)"
        elif c == "disp":
            g = "dispatch: indirect branch + block lookup"
        elif c == "memc":
            g = "memory: C slow paths (I/O writes ...)"
        elif c == "video":
            g = "video (renderer; A: on the ME on a PSP)"
        elif c == "sound":
            g = "sound (synthesis, timers' FIFO)"
        elif c in ("xlat", "flush"):
            g = "translate / flush"
        elif c == "libc":
            g = "libc"
        else:
            g = "other core (timers, dma, serial, ...)"
        tgt[g] += v
        items[(m, base)] += v

    def show(title, d, excl):
        tot = sum(v for k, v in d.items() if not k.startswith(excl))
        print("\n%s: %.0f instr/frame on the main CPU" % (title, tot / frames))
        for k, v in d.most_common():
            note = "  (not main CPU)" if k.startswith(excl) else ""
            print("  %9.0f %5.1f%%  %s%s" % (v / frames, 100.0 * v / tot, k, note))
        return tot

    print("scene %s, %d frames; MIPS instructions per frame (twin)" % (scene, frames))
    ta = show("MACHINE A (drawn)", A, "video")
    tb = show("MACHINE B (headless)", Bm, "\0")
    print("\nPAIR: %.0f instr/frame (A %.0f + B %.0f); host/hash excluded: %.0f"
          % ((ta + tb) / frames, ta / frames, tb / frames, host / frames))
    print("\ncalls per frame (ent:):")
    for n in ("update_gba", "gbab_update_gba", "mips_update_gba",
              "check_and_raise_interrupts", "set_cpu_mode", "execute_store_cpsr",
              "execute_spsr_restore", "write_io_register16", "write_io_register8",
              "write_io_register32", "mips_indirect_branch_arm",
              "mips_indirect_branch_thumb", "mips_indirect_branch_dual",
              "block_lookup_address_arm", "block_lookup_address_thumb",
              "block_lookup_address_dual", "update_scanline", "sound_timer",
              "render_gbc_sound", "gbalink_update_serial", "gbalink_next_event",
              "gbalink_entry_yield", "gbalink_bus_peek", "update_timers",
              "dma_transfer", "execute_swi"):
        if n in calls:
            print("  %9.1f  %s" % (calls[n] / frames, n))
    print("\nmemory stub calls per frame, machine A (mc:), by handler:")
    for n, v in mc.most_common(16):
        print("  %9.1f  %s" % (v / frames, n))
    print("\ntop %d items:" % top)
    tot = ta + tb
    for (m, n), v in items.most_common(top + 6):
        if n.startswith("_Z") or "render" in n:
            continue
        print("  %9.0f %5.1f%%  %s  %s" % (v / frames, 100.0 * v / tot, m, n))


if __name__ == "__main__":
    main()
