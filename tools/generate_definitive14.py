#!/usr/bin/env python3
"""Generate stock + the exact separately supplied USA Definitive Edition 1.4.

This developer build step accepts only the supported original ROM and author
patch. Runtime players select only the original cartridge and tick the mod.
Neither original input is changed; generated ROM/code remain local build data.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

STOCK = "b4bd50e4131b027c334547b4524e2dbbd4227130"
PATCH = "c96b61dff8563d4b9e1fe5730c985f1523aca57f89a6f0a4655b11313c6c24b2"
PROGRAM = "5de00d88ecc367c9d374c6b2601caa51a17ca743"

def write_runtime_data(rom, image, out):
    """Emit changed ROM bytes beside the ignored native program.

    The original cartridge supplies unchanged bytes at runtime. Neither an IPS
    container nor a complete cartridge image is distributed as an asset.
    """
    edits, data = [], bytearray()
    pos = 0
    while pos < len(rom):
        if rom[pos] == image[pos]:
            pos += 1
            continue
        start = pos
        while pos < len(rom) and rom[pos] != image[pos]:
            data.append(image[pos])
            pos += 1
        edits.append((start, pos-start))
    lines = ["// Generated from the verified original and Zeldafangirl's DE1.4.",
             "// Local derivative build data: do not commit.", "#pragma once",
             "#include <cstdint>", "namespace minish::definitive14_data {",
             "struct Edit { std::uint32_t offset, size; };",
             "inline constexpr std::uint8_t bytes[] = {"]
    lines += ["    " + ",".join(f"0x{x:02x}" for x in data[i:i+16]) + ","
              for i in range(0, len(data), 16)]
    lines += ["};", "inline constexpr Edit edits[] = {"]
    lines += [f"    {{0x{offset:x}, {size}}}," for offset, size in edits]
    lines += ["};", "}", ""]
    out.mkdir(parents=True, exist_ok=True)
    (out / "de14_rom_data.h").write_text("\n".join(lines), encoding="utf-8")

def exact_image(rom, patch):
    if len(rom) != 0x1000000 or hashlib.sha1(rom).hexdigest() != STOCK:
        raise ValueError("Expected the supported original USA cartridge")
    if len(patch) != 26644 or hashlib.sha256(patch).hexdigest() != PATCH:
        raise ValueError("Expected the exact Definitive Edition USA 1.4 IPS")
    image = bytearray(rom)
    pos = 5
    assert patch[:5] == b"PATCH"
    while patch[pos:pos+3] != b"EOF":
        offset = int.from_bytes(patch[pos:pos+3], "big")
        count = int.from_bytes(patch[pos+3:pos+5], "big")
        pos += 5
        if count:
            data = patch[pos:pos+count]
            pos += count
        else:
            count = int.from_bytes(patch[pos:pos+2], "big")
            data = bytes([patch[pos+2]]) * count
            pos += 3
        assert len(data) == count and offset + count <= len(image)
        image[offset:offset+count] = data
    assert pos + 3 == len(patch) and image[:0xC0] == rom[:0xC0]
    assert hashlib.sha1(image).hexdigest() == PROGRAM
    return image

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--ips", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--runtime-data-only", action="store_true",
                        help="refresh built-in data beside an existing native program")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    out = (args.out or root / "generated").resolve()
    original = args.rom.read_bytes()
    image = exact_image(original, args.ips.read_bytes())
    write_runtime_data(original, image, out / "definitive14")
    if args.runtime_data_only:
        return
    if not args.compiler:
        parser.error("--compiler is required unless --runtime-data-only is used")
    source_configs = [root / "symbols/minishcap.toml", root / "symbols/minishcap_reviewed.toml"]
    subprocess.run([str(args.compiler.resolve()), "--rom", str(args.rom.resolve()),
        *[arg for config in source_configs for arg in ("--config", str(config))],
        "--out", str(out)], check=True)
    with tempfile.TemporaryDirectory(prefix="minish-de14-") as temp:
        temp = Path(temp)
        rom = temp / "definitive14-usa.gba"
        rom.write_bytes(image)
        configs = []
        for source in source_configs:
            config = temp / source.name
            config.write_text(source.read_text(encoding="utf-8").replace(STOCK, PROGRAM), encoding="utf-8")
            configs.extend(["--config", str(config)])
        subprocess.run([str(args.compiler.resolve()), "--rom", str(rom), *configs,
            "--output-prefix", "de14_", "--no-symbol-map", "--out", str(out / "definitive14")], check=True)

if __name__ == "__main__":
    main()
