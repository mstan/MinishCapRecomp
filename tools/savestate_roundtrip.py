#!/usr/bin/env python3
"""savestate_roundtrip.py — exercise the gbarecomp save-state feature
end-to-end against the recompiled runtime over TCP.

It validates three properties, all without an emulator oracle (the
runtime is compared against *itself*):

  1. Restore fidelity   — save at frame A, run ahead, load A back:
                          the full machine state must match the
                          state originally captured at A.
  2. Deterministic replay — from the restored A, running the same
                          number of frames must reproduce the same
                          state the first run reached.
  3. Byte round-trip    — save A -> load A -> save again: the two
                          .state files must be byte-identical.

Usage:
    python tools/savestate_roundtrip.py
    python tools/savestate_roundtrip.py --warmup 40 --advance 20

Spawns build/MinishCapRecomp.exe --tcp <port> in the repo root (so the
cached BIOS/ROM sidecars + game.toml resolve), drives it, and exits
non-zero on any mismatch.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
from typing import Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "MinishCapRecomp.exe"

# (region command, byte size) — the full guest memory surface the
# always-on TCP server exposes.
REGIONS = [
    ("read_iwram", 32 * 1024),
    ("read_ewram", 256 * 1024),
    ("read_vram", 96 * 1024),
    ("read_pal", 1024),
    ("read_oam", 1024),
]


class JsonClient:
    """Line-delimited JSON request/response over TCP."""

    def __init__(self, host: str, port: int):
        deadline = time.time() + 15.0
        last_err: Optional[Exception] = None
        self.sock = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=2.0)
                break
            except OSError as e:
                last_err = e
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"can't reach {host}:{port}: {last_err}")
        self.sock.settimeout(None)
        self.buf = b""

    def call(self, **kwargs) -> dict:
        line = json.dumps(kwargs).encode("utf-8") + b"\n"
        self.sock.sendall(line)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("peer closed the connection")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode("utf-8"))

    def close(self) -> None:
        try:
            self.call(cmd="quit")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    def read_region(self, cmd: str, size: int) -> bytes:
        chunk = 4096
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            resp = self.call(cmd=cmd, addr=off, len=n)
            if not resp.get("ok"):
                raise RuntimeError(f"{cmd} @{off}: {resp}")
            out += bytes.fromhex(resp["data"])
        return bytes(out)

    def step_to(self, frame: int) -> int:
        cur = self.call(cmd="frame")["frame"]
        while cur < frame:
            cur = self.call(cmd="step")["frame"]
        return cur

    def state_hash(self) -> str:
        """SHA-256 over CPU registers + all guest memory regions."""
        h = hashlib.sha256()
        regs = self.call(cmd="registers")
        if not regs.get("ok"):
            raise RuntimeError(f"registers failed: {regs}")
        for i in range(16):
            h.update(str(regs[f"r{i}"]).encode())
        h.update(str(regs["cpsr"]).encode())
        for cmd, size in REGIONS:
            h.update(self.read_region(cmd, size))
        return h.hexdigest()


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--warmup", type=int, default=40,
                    help="frame to reach before taking the first snapshot")
    ap.add_argument("--advance", type=int, default=20,
                    help="frames to run past the snapshot before reloading")
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--no-spawn", action="store_true")
    args = ap.parse_args()

    proc = None
    if not args.no_spawn:
        cmd = [args.exe, "--tcp", str(args.port)]
        print(f"==> spawning: {' '.join(cmd)} (cwd={ROOT})")
        proc = subprocess.Popen(cmd, cwd=str(ROOT),
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)

    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="gbastate_"))
    file1 = tmpdir / "a.state"
    file3 = tmpdir / "a_again.state"

    cli = JsonClient("127.0.0.1", args.port)
    try:
        a = args.warmup
        b = args.warmup + args.advance

        print(f"==> stepping to frame {a}")
        cli.step_to(a)
        h0 = cli.state_hash()

        r = cli.call(cmd="savestate_save", path=str(file1))
        if not r.get("ok"):
            fail(f"savestate_save: {r}")
        print(f"==> saved state at frame {a} -> {file1.name}")

        print(f"==> running ahead to frame {b}")
        cli.step_to(b)
        h1 = cli.state_hash()

        # (1) Restore fidelity.
        r = cli.call(cmd="savestate_load", path=str(file1))
        if not r.get("ok"):
            fail(f"savestate_load: {r}")
        print(f"==> loaded state; runtime reports pc=0x{r.get('pc', 0):08x} "
              f"frame={r.get('frame')}")
        if cli.call(cmd="frame")["frame"] != a:
            fail(f"frame after load is {cli.call(cmd='frame')['frame']}, "
                 f"expected {a}")
        h0b = cli.state_hash()
        if h0b != h0:
            fail("restored state at A does not match the originally "
                 f"captured state\n  original={h0}\n  restored={h0b}")
        print("PASS (1/3): restore fidelity — state at A matches")

        # (2) Deterministic replay.
        cli.step_to(b)
        h1b = cli.state_hash()
        if h1b != h1:
            fail("replay from restored A diverged by frame B\n"
                 f"  first run ={h1}\n  replay    ={h1b}")
        print("PASS (2/3): deterministic replay — state at B reproduced")

        # (3) Byte round-trip.
        cli.call(cmd="savestate_load", path=str(file1))
        r = cli.call(cmd="savestate_save", path=str(file3))
        if not r.get("ok"):
            fail(f"second savestate_save: {r}")
        b1 = file1.read_bytes()
        b3 = file3.read_bytes()
        if b1 != b3:
            fail(f"save->load->save not byte-identical "
                 f"({len(b1)} vs {len(b3)} bytes)")
        print(f"PASS (3/3): byte round-trip — {len(b1)} bytes identical")

        # (4) Gate rejections. A tampered blob must fail load cleanly
        # (ok:false) rather than corrupt the running machine.
        good = file1.read_bytes()

        def expect_reject(label: str, blob: bytes) -> None:
            bad = tmpdir / "bad.state"
            bad.write_bytes(blob)
            r = cli.call(cmd="savestate_load", path=str(bad))
            if r.get("ok"):
                fail(f"gate accepted a {label} blob (expected rejection)")
            print(f"    rejected {label}: {r.get('error')}")

        ba = bytearray(good); ba[0] ^= 0xFF              # corrupt magic
        expect_reject("bad-magic", bytes(ba))
        ba = bytearray(good); ba[4] = 0x7F               # bump version
        expect_reject("bad-version", bytes(ba))
        ba = bytearray(good); ba[8] ^= 0xFF              # flip a SHA-1 char
        expect_reject("rom-mismatch", bytes(ba))
        expect_reject("truncated", good[:128])
        # The machine must still be usable after the rejected loads.
        cli.call(cmd="savestate_load", path=str(file1))
        if cli.call(cmd="frame")["frame"] != a:
            fail("machine not restorable after rejected loads")
        print("PASS (4/4): gate rejects tampered/mismatched/truncated blobs")

        print("\nALL CHECKS PASSED")
        return 0
    finally:
        cli.close()
        if proc is not None:
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
