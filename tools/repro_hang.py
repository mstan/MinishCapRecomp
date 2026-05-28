#!/usr/bin/env python3
"""repro_hang.py — reproduce the MC-HP-002 transition hang from RESET
over TCP, on the user's canonical path (title -> Start -> A -> A -> idle).

Single-threaded server: a `step` that enters the M4A null-song spin
never returns, so we detect the spin two ways:
  (1) arm break_pc at the sequence walker (0x08004286); the dispatch
      unwinds when PC hits it, so `step` returns with PC parked there.
  (2) a per-step wall-clock timeout as a backstop.

On a catch we dump the M4A track struct (0x030018D0) fields the spin
depends on, so we can confirm songidx==0 / cmdptr@5c==garbage and prove
this is the same root as the documented MC-HP-002, on THIS transition.

Screenshots are written as raw RGB .ppm at each scenario checkpoint so
the menu navigation can be eyeballed and the input timing adjusted.

Usage:
    python tools/repro_hang.py [--no-spawn] [--port 19842]
"""
from __future__ import annotations
import argparse, json, pathlib, socket, struct, subprocess, sys, time
from typing import Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent          # MinishCapRecomp/
NATIVE_EXE = ROOT / "build" / "MinishCapRecomp.exe"
OUT = ROOT / "tools" / "repro_out"

SPIN_PC = 0x08004286
M4A_STRUCT = 0x030018D0
M4A_LEN = 0x80

# GBA KEYINPUT (active-low): A0 B1 Sel2 Start3 R4 L5 Up6 Dn7 R8 L9
A     = 0x3FF & ~0x001
START = 0x3FF & ~0x008
NONE  = 0x3FF


class JsonClient:
    def __init__(self, host, port):
        deadline = time.time() + 10.0
        self.sock = None
        last = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:
                last = e; time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}: {last}")
        self.buf = b""

    def call(self, timeout=None, **kw):
        self.sock.sendall(json.dumps(kw).encode() + b"\n")
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                ch = self.sock.recv(65536)
                if not ch:
                    raise RuntimeError("peer closed")
                self.buf += ch
        finally:
            self.sock.settimeout(None)
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def read_region(self, cmd, base, size):
        out = bytearray()
        for off in range(0, size, 1024):
            n = min(1024, size - off)
            r = self.call(cmd=cmd, addr=base + off, len=n)
            if not r.get("ok"):
                raise RuntimeError(f"{cmd}@{base+off:#x}: {r}")
            out += bytes.fromhex(r["data"])
        return bytes(out)


def m4a_fields(b):
    u8 = lambda o: b[o]
    u16 = lambda o: b[o] | (b[o+1] << 8)
    u32 = lambda o: b[o] | (b[o+1]<<8) | (b[o+2]<<16) | (b[o+3]<<24)
    return {"sel@12": u16(0x12), "f58": u8(0x58), "f59": u8(0x59),
            "cmdptr@5c": f"0x{u32(0x5c):08x}", "flags@0": u8(0x00)}


def save_ppm(cl, tag):
    from PIL import Image
    OUT.mkdir(parents=True, exist_ok=True)
    r = cl.call(cmd="screenshot")
    if not r.get("ok"):
        print(f"   [screenshot {tag}: {r}]"); return
    raw = bytes.fromhex(r["data"])
    w, h = r.get("w", 240), r.get("h", 160)
    if len(raw) == w * h * 4:
        img = Image.frombytes("RGBA", (w, h), raw).convert("RGB")
    elif len(raw) == w * h * 3:
        img = Image.frombytes("RGB", (w, h), raw)
    else:
        print(f"   [shot {tag}: unexpected len {len(raw)} for {w}x{h}]")
        return
    p = OUT / f"{tag}.png"
    img.save(p)
    ex = img.getextrema()
    print(f"   [shot {tag}: {p.name} extrema={ex}]", flush=True)


def step_n(cl, n, keymask, timeout=8.0):
    """Hold keymask for n frames. Returns ('ok'|'notok'|'timeout', frame)."""
    cl.call(cmd="set_keyinput", value=keymask)
    last = None
    for i in range(n):
        try:
            last = cl.call(timeout=timeout, cmd="step")
        except socket.timeout:
            return ("timeout", None)
        if not last.get("ok"):
            return ("notok", last.get("frame"))
    return ("ok", last.get("frame") if last else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--no-spawn", action="store_true")
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--boot", type=int, default=640,
                    help="frames to idle before pressing Start (title up)")
    ap.add_argument("--state", default=None,
                    help="load this savestate and skip menu navigation")
    ap.add_argument("--hold", default="up",
                    help="key to hold during the post-state idle")
    ap.add_argument("--inspect", action="store_true",
                    help="arm break_pc at the walker and dump every song-start")
    args = ap.parse_args()

    HOLD_MASK = {"up": 0x3FF & ~0x040, "down": 0x3FF & ~0x080,
                 "left": 0x3FF & ~0x020, "right": 0x3FF & ~0x010,
                 "a": A, "none": NONE}.get(args.hold.lower(), NONE)

    proc = None
    if not args.no_spawn:
        if not NATIVE_EXE.exists():
            print(f"missing {NATIVE_EXE}", file=sys.stderr); return 1
        print(f"==> spawning native --tcp {args.port}", flush=True)
        OUT.mkdir(parents=True, exist_ok=True)
        errf = open(OUT / "native_stderr.txt", "wb")
        proc = subprocess.Popen([str(NATIVE_EXE), "--tcp", str(args.port)],
                                cwd=str(ROOT),
                                stdout=subprocess.DEVNULL,
                                stderr=errf)
    try:
        cl = JsonClient("127.0.0.1", args.port)
        f = cl.call(cmd="frame")
        print(f"==> connected, frame={f.get('frame')}", flush=True)

        if args.state:
            r = cl.call(cmd="savestate_load", path=args.state)
            print(f"==> savestate_load {args.state}: {r}", flush=True)
            if not r.get("ok"):
                return 1
            save_ppm(cl, "state_loaded")
            iw = cl.read_region("read_iwram", M4A_STRUCT, M4A_LEN)
            print(f"==> M4A @load: {m4a_fields(iw)}", flush=True)
            if args.inspect:
                # Arm the walker break; step one frame at a time. On the
                # frame a song-start runs, the dispatch unwinds at the
                # walker and `step` returns with R15 parked there.
                cl.call(cmd="set_break_pc", value=SPIN_PC)
                cl.call(cmd="set_keyinput", value=HOLD_MASK)
                for i in range(200):
                    try:
                        r = cl.call(timeout=args.timeout, cmd="step")
                    except socket.timeout:
                        print(f"==> TIMEOUT at frame {i} (spin reached the "
                              f"walker before any park?)", flush=True)
                        break
                    pc = cl.call(cmd="registers").get("r15")
                    if pc == SPIN_PC:
                        iw = cl.read_region("read_iwram", M4A_STRUCT, M4A_LEN)
                        regs = cl.call(cmd="registers")
                        print(f"==> ANIM-WALKER PARK at frame {i}: "
                              f"R15=0x{pc:08x}  fields={m4a_fields(iw)}",
                              flush=True)
                        # Full entity struct dump (gEntities slot 0x030018D0).
                        print(f"    entity @0x{M4A_STRUCT:08x} dump:")
                        for row in range(0, 0x40, 16):
                            hexs = ' '.join(f'{iw[row+b]:02x}' for b in range(16))
                            print(f"      +0x{row:02x}: {hexs}")
                        anim = iw[0x12] | (iw[0x13] << 8)
                        print(f"    KEY: +0x00(type/flags)=0x{iw[0]:02x} "
                              f"+0x10(deleted?)=0x{iw[0x10]:02x} "
                              f"+0x11=0x{iw[0x11]:02x} +0x12(animIdx)={anim} "
                              f"+0x5c(framePtr)=0x{iw[0x5c]|(iw[0x5d]<<8)|(iw[0x5e]<<16)|(iw[0x5f]<<24):08x}",
                              flush=True)
                        print("    regs:", {k: hex(v) for k, v in regs.items()
                              if k.startswith('r') and isinstance(v, int)},
                              flush=True)
                        # The break unwound the spinning dispatch, so the
                        # server is responsive: pull the trace ring and show
                        # the dispatch/call chain (named) that led here, to
                        # find the main-loop frame that dispatched this entity.
                        tr = cl.call(cmd="runtime_trace", count=512)
                        ents = tr.get("entries") or tr.get("trace") or []
                        symcache = {}
                        def nm(pcv):
                            if pcv not in symcache:
                                s = cl.call(cmd="symbol", addr=pcv)
                                symcache[pcv] = (s.get("name"), s.get("offset", 0))
                            n, o = symcache[pcv]
                            return f"{n}+0x{o:x}" if n else f"0x{pcv:08x}"
                        print(f"    --- trace ring ({len(ents)} entries), "
                              f"dispatch/call events (named) ---")
                        seen_fns = []
                        for e in ents:
                            kind = e.get("kind")
                            if kind not in (1, 7, 2):  # dispatch, call, exchange
                                continue
                            pcv = e.get("pc", 0)
                            label = nm(pcv)
                            fn = label.split("+")[0]
                            if not seen_fns or seen_fns[-1] != fn:
                                seen_fns.append(fn)
                                print(f"      {kind:>8} {label}")
                        break
                else:
                    print("==> no song-start park in 200 frames", flush=True)
                try: cl.call(cmd="quit")
                except Exception: pass
                return 0

            print(f"==> idling holding '{args.hold}' "
                  f"(timeout={args.timeout}s), watching for spin", flush=True)
            spin = None
            for i in range(2000):
                try:
                    cl.call(timeout=args.timeout, cmd="set_keyinput",
                            value=HOLD_MASK)
                    r = cl.call(timeout=args.timeout, cmd="step")
                except socket.timeout:
                    print(f"==> TIMEOUT after {i} frames holding "
                          f"'{args.hold}' — HANG (the spin).", flush=True)
                    spin = i
                    break
                if i and i % 60 == 0:
                    save_ppm(cl, f"hold_{i}")
            if spin is None:
                print(f"==> no hang in 2000 frames holding '{args.hold}'",
                      flush=True)
                try: cl.call(cmd="quit")
                except Exception: pass
            return 0

        # Scenario as absolute frame targets: idle to `at`, then hold `mask`
        # for `hold` frames, then screenshot. Title "PRESS START" ~f762;
        # file-select up ~f920; pick + start from there.
        cur = 0
        def idle_to(target):
            nonlocal cur
            if target > cur:
                step_n(cl, target - cur, NONE, args.timeout)
                cur = target
        def press(label, at, mask, hold=12):
            nonlocal cur
            idle_to(at)
            st, fr = step_n(cl, hold, mask, args.timeout)
            cur += hold
            print(f"== {label} @f{at}: {st} frame={fr}", flush=True)
            save_ppm(cl, label)
            return st

        press("start", 800, START)        # title PRESS START -> file select
        press("pick",  1000, A)           # CHOOSE A FILE: pick file 1 (LINK)
        press("begin", 1080, A)           # start file 1
        idle_to(1140)
        save_ppm(cl, "after_begin")

        # Idle and watch for the hang. Detect by TIMEOUT: benign song-starts
        # complete in ms, the null-song spin blocks one `step` for seconds.
        # (No break_pc here — a break would catch the first benign song-start
        #  and we can't cleanly continue past a mid-function park.)
        print(f"==> idling (timeout={args.timeout}s), watching for spin",
              flush=True)
        cl.call(cmd="set_keyinput", value=NONE)
        spin_frame = None
        last_shot = 0
        for i in range(3000):
            try:
                r = cl.call(timeout=args.timeout, cmd="step")
            except socket.timeout:
                fr = cl.call(timeout=2.0, cmd="frame").get("frame") \
                    if False else None
                print(f"==> TIMEOUT after {i} idle frames — HANG (one step "
                      f"blocked > {args.timeout}s). This is the spin.",
                      flush=True)
                spin_frame = i
                break
            fr = r.get("frame")
            if fr is not None and fr - last_shot >= 120:
                last_shot = fr
                save_ppm(cl, f"idle_f{fr}")
        if spin_frame is None:
            print("==> no hang in 3000 idle frames", flush=True)
            try:
                cl.call(cmd="quit")
            except Exception:
                pass
        else:
            # The server thread is stuck in the spin; it cannot answer TCP.
            # Kill the process; the hang is reproduced + frame located.
            print("==> server thread stuck in spin; killing native.",
                  flush=True)
        return 0
    finally:
        if proc:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
