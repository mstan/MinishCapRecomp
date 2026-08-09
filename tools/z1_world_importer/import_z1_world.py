#!/usr/bin/env python3
"""Validate Zelda 1 PRG0 and build a local foreign-world asset cache.

No Nintendo data is part of this source tree. The offsets below describe data
in a user-provided ROM and are derived from the pinned Zelda 1 disassembly.
They are headerless PRG offsets; the importer accounts for the iNES header.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
import tempfile
from dataclasses import dataclass


SCHEMA_VERSION = 1
SOURCE_REVISION = "aldonunez/zelda1-disassembly@50a1c869a8d8e2eb8b5b60acea325f44b4341762"
EXPECTED_ROM_SHA1 = "dab79c84934f9aa5db4e7dad390e5d0c12443fa2"
EXPECTED_ROM_SHA256 = "8f72dc2e98572eb4ba7c3a902bca5f69c448fc4391837e5f8f0d4556280440ac"
EXPECTED_PRG_SHA1 = "a12d74c73a0481599a5d832361d168f4737bbcf6"
INES_HEADER_SIZE = 16


class ImportFailure(ValueError):
    """A ROM or cache failed deterministic validation."""


@dataclass(frozen=True)
class InesImage:
    prg_offset: int
    prg_size: int
    chr_size: int
    mapper: int
    battery: bool
    mirroring: str


@dataclass(frozen=True)
class AssetSpan:
    name: str
    offset: int
    length: int


# Keep names aligned with bins.xml in the pinned disassembly. This list spans
# the visual, map, object, text, music, dungeon, and ending data needed by the
# eventual complete First Quest implementation.
ASSETS = (
    # Preserve the verified PRG payload in the ignored cache so later parser
    # revisions can consume tables that are still embedded in assembly code,
    # without asking the launcher to retain access to the user's ROM path.
    AssetSpan("source/prg", 0, 128 * 1024),
    AssetSpan("audio/song_item_taken", 3677, 19),
    AssetSpan("audio/song_overworld", 3696, 621),
    AssetSpan("audio/song_underworld", 4317, 199),
    AssetSpan("audio/song_end_level", 4516, 89),
    AssetSpan("audio/song_last_level", 4605, 207),
    AssetSpan("audio/song_ganon", 4812, 43),
    AssetSpan("audio/song_ending", 4855, 404),
    AssetSpan("audio/song_demo", 5259, 825),
    AssetSpan("audio/song_zelda", 6084, 97),
    AssetSpan("text/person", 16460, 1366),
    AssetSpan("patterns/demo_sprites", 19892, 2304),
    AssetSpan("patterns/demo_background", 22196, 2080),
    AssetSpan("patterns/common_sprites", 32895, 1792),
    AssetSpan("patterns/common_background", 34687, 1792),
    AssetSpan("patterns/common_misc", 36479, 224),
    AssetSpan("text/demo_fields", 37530, 531),
    AssetSpan("text/credits_lines", 44124, 414),
    AssetSpan("patterns/underworld_background", 49435, 2080),
    AssetSpan("patterns/overworld_background", 51515, 2080),
    AssetSpan("patterns/overworld_sprites", 53595, 1824),
    AssetSpan("patterns/underworld_sprites_358", 55419, 544),
    AssetSpan("patterns/underworld_sprites_469", 55963, 544),
    AssetSpan("patterns/underworld_sprites_common", 56507, 256),
    AssetSpan("patterns/underworld_sprites_127", 56763, 544),
    AssetSpan("patterns/underworld_boss_1257", 57307, 1024),
    AssetSpan("patterns/underworld_boss_3468", 58331, 1024),
    AssetSpan("patterns/underworld_boss_9", 59355, 1024),
    AssetSpan("world/object_lists", 83574, 201),
    AssetSpan("world/room_layouts_overworld", 87064, 1936),
    AssetSpan("world/room_layouts_underworld", 90334, 504),
    AssetSpan("world/level_block_overworld", 99328, 768),
    AssetSpan("world/level_block_underworld_1_first_quest", 100096, 768),
    AssetSpan("world/level_block_underworld_2_first_quest", 100864, 768),
    AssetSpan("world/level_block_underworld_1_second_quest", 101632, 768),
    AssetSpan("world/level_block_underworld_2_second_quest", 102400, 768),
    AssetSpan("world/level_info_overworld", 103168, 252),
    AssetSpan("world/level_info_underworld_1", 103420, 252),
    AssetSpan("world/level_info_underworld_2", 103672, 252),
    AssetSpan("world/level_info_underworld_3", 103924, 252),
    AssetSpan("world/level_info_underworld_4", 104176, 252),
    AssetSpan("world/level_info_underworld_5", 104428, 252),
    AssetSpan("world/level_info_underworld_6", 104680, 252),
    AssetSpan("world/level_info_underworld_7", 104932, 252),
    AssetSpan("world/level_info_underworld_8", 105184, 252),
    AssetSpan("world/level_info_underworld_9", 105436, 252),
    AssetSpan("story/tile_attribute_transfer", 107518, 1131),
    AssetSpan("story/game_title_transfer", 108649, 1121),
    AssetSpan("audio/pcm_samples", 114688, 9216),
)


def digest(data: bytes, algorithm: str) -> str:
    return hashlib.new(algorithm, data).hexdigest()


def parse_ines(data: bytes) -> InesImage:
    if len(data) < INES_HEADER_SIZE or data[:4] != b"NES\x1a":
        raise ImportFailure("not an iNES ROM (missing NES\\x1a header)")
    flags6, flags7 = data[6], data[7]
    if flags7 & 0x0C == 0x08:
        raise ImportFailure("NES 2.0 images are not supported for this world pack")
    if flags6 & 0x04:
        raise ImportFailure("trainer-bearing images are not the supported PRG0 dump")
    prg_size = data[4] * 16 * 1024
    chr_size = data[5] * 8 * 1024
    mapper = (flags6 >> 4) | (flags7 & 0xF0)
    expected_size = INES_HEADER_SIZE + prg_size + chr_size
    if len(data) != expected_size:
        raise ImportFailure(
            f"ROM size is {len(data)} bytes; header describes {expected_size} bytes"
        )
    return InesImage(
        prg_offset=INES_HEADER_SIZE,
        prg_size=prg_size,
        chr_size=chr_size,
        mapper=mapper,
        battery=bool(flags6 & 0x02),
        mirroring="vertical" if flags6 & 0x01 else "horizontal",
    )


def verify_rom(data: bytes, accepted_sha1: str = EXPECTED_ROM_SHA1) -> InesImage:
    image = parse_ines(data)
    problems: list[str] = []
    if image.prg_size != 128 * 1024:
        problems.append(f"expected 128 KiB PRG, got {image.prg_size} bytes")
    if image.chr_size != 0:
        problems.append(f"expected CHR RAM image, got {image.chr_size} CHR bytes")
    if image.mapper != 1:
        problems.append(f"expected MMC1 mapper 1, got mapper {image.mapper}")
    if not image.battery:
        problems.append("expected battery-backed save RAM flag")
    if problems:
        raise ImportFailure("unsupported Zelda 1 layout: " + "; ".join(problems))

    actual_sha1 = digest(data, "sha1")
    if actual_sha1 != accepted_sha1:
        raise ImportFailure(
            "unsupported Zelda 1 revision: SHA-1 " + actual_sha1
            + "; this pack currently requires USA PRG0 " + accepted_sha1
        )
    if accepted_sha1 == EXPECTED_ROM_SHA1:
        if digest(data, "sha256") != EXPECTED_ROM_SHA256:
            raise ImportFailure("ROM SHA-256 disagrees with the pinned PRG0 identity")
        prg = data[image.prg_offset:image.prg_offset + image.prg_size]
        if digest(prg, "sha1") != EXPECTED_PRG_SHA1:
            raise ImportFailure("PRG payload disagrees with the pinned PRG0 identity")
    return image


def extract_assets(data: bytes, image: InesImage) -> tuple[dict, dict[str, bytes]]:
    prg = data[image.prg_offset:image.prg_offset + image.prg_size]
    files: dict[str, bytes] = {}
    records: list[dict] = []
    for span in ASSETS:
        end = span.offset + span.length
        if end > len(prg):
            raise ImportFailure(
                f"asset {span.name} ends at PRG offset {end}, beyond {len(prg)}"
            )
        payload = prg[span.offset:end]
        relative_path = span.name + ".bin"
        files[relative_path] = payload
        records.append({
            "name": span.name,
            "path": relative_path,
            "prg_offset": span.offset,
            "length": span.length,
            "sha256": digest(payload, "sha256"),
        })
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "world_pack": "zelda1-first-quest",
        "source_revision": SOURCE_REVISION,
        "rom": {
            "sha1": digest(data, "sha1"),
            "sha256": digest(data, "sha256"),
            "prg_sha1": digest(prg, "sha1"),
            "mapper": image.mapper,
            "prg_size": image.prg_size,
            "mirroring": image.mirroring,
        },
        "assets": records,
    }
    return manifest, files


def write_cache(root: pathlib.Path, manifest: dict, files: dict[str, bytes]) -> pathlib.Path:
    cache = root / ("zelda1-prg0-" + manifest["rom"]["sha1"][:12])
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    if cache.exists():
        existing = cache / "manifest.json"
        if existing.is_file() and existing.read_bytes() == manifest_bytes:
            for relative_path, payload in files.items():
                candidate = cache / relative_path
                if not candidate.is_file() or candidate.read_bytes() != payload:
                    raise ImportFailure(
                        f"existing cache is incomplete or modified: {candidate}"
                    )
            return cache
        raise ImportFailure(
            f"refusing to overwrite non-matching cache directory: {cache}"
        )

    root.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix=".z1-import-", dir=root))
    try:
        for relative_path, payload in files.items():
            output = staging / relative_path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)
        (staging / "manifest.json").write_bytes(manifest_bytes)
        os.replace(staging, cache)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return cache


def import_rom(rom_path: pathlib.Path, output_root: pathlib.Path) -> pathlib.Path:
    try:
        data = rom_path.read_bytes()
    except OSError as exc:
        raise ImportFailure(f"could not read {rom_path}: {exc}") from exc
    image = verify_rom(data)
    manifest, files = extract_assets(data, image)
    return write_cache(output_root, manifest, files)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=pathlib.Path, help="user-owned Zelda 1 USA PRG0 .nes")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("foreign-world-cache"),
        help="local cache root (default: ./foreign-world-cache)",
    )
    args = parser.parse_args(argv)
    try:
        cache = import_rom(args.rom, args.output)
    except ImportFailure as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(cache)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
