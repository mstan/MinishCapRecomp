#!/usr/bin/env python3
"""Exercise copied ARM collision through an existing native test session.

Use a current-build USA checkpoint immediately after confirming the opening
South Hyrule Field dialogue, before the actors begin colliding. The supplied
checkpoint is a prerequisite; it is never patched. Attach only to a test-owned
headless --tcp session. No game/window is launched and no input picker opens.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from savestate_roundtrip import JsonClient

ROM_SHA1 = "b4bd50e4131b027c334547b4524e2dbbd4227130"
COPY_SHA256 = "5801a8dc1163ce66d724af1c2ae02c08afbb1636f706819dafd8bf49f81dceb1"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--state", required=True, help="Current-build checkpoint path on the test server")
    parser.add_argument("--scratch-state", required=True, help="Disposable save path on the test server")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.state == args.scratch_state:
        parser.error("The disposable checkpoint must differ from the input")
    report: dict = {"scope": "Native copied-collision trigger plus own-build continuation"}
    client = None
    try:
        client = JsonClient("127.0.0.1", args.port)
        client.sock.settimeout(30)

        def call(cmd: str, **kwargs):
            result = client.call(cmd=cmd, **kwargs)
            if not result.get("ok"):
                raise RuntimeError(result)
            return result

        def native_only():
            result = call("misses")
            assert result["sha1"] == ROM_SHA1 and result["coverage"] == "FULLY_STATIC"
            assert result["distinct_misses"] == result["interpreted_insns"] == result["healed_native"] == 0
            return result

        def observe():
            hashes = call("state_hash")
            shot = call("screenshot")
            return {"status": call("run_status"), "registers": call("registers"),
                    "memory": {k: hashes[k] for k in ("iwram", "ewram", "vram", "pal", "oam")},
                    "native_rgb_sha256": hashlib.sha256(bytes.fromhex(shot["data"])).hexdigest()}

        report["load"] = call("savestate_load", path=args.state)
        report["native_before"] = native_only()
        code = bytes.fromhex(call("read_iwram", addr=0x5B0C, len=0x198)["data"])
        report["copied_body_sha256"] = hashlib.sha256(code).hexdigest()
        assert report["copied_body_sha256"] == COPY_SHA256
        room = bytes.fromhex(call("read_iwram", addr=0xBF0, len=6)["data"])
        assert room[4:6] == bytes((3, 1)), "Expected the authored South Hyrule Field trigger"
        report["trigger"] = call("run_frames", n=180, keyinput=0x3FF)
        report["native_after_trigger"] = native_only()
        report["checkpoint"] = call("savestate_save", path=args.scratch_state)
        report["continuous_step"] = call("run_frames", n=20, keyinput=0x3FF)
        report["continuous"] = observe()
        report["restore"] = call("savestate_load", path=args.scratch_state)
        report["restored_step"] = call("run_frames", n=20, keyinput=0x3FF)
        report["restored"] = observe()
        assert report["continuous"] == report["restored"]
        report["native_after_restore"] = native_only()
        report["result"] = "PASS"
        return 0
    except Exception as error:
        report["result"] = "FAIL"
        report["error"] = repr(error)
        return 1
    finally:
        if client is not None:
            client.sock.close()  # Keep the attached test session running.
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    raise SystemExit(main())
