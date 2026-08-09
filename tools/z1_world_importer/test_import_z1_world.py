from __future__ import annotations

import hashlib
import json
import pathlib
import tempfile
import unittest

from import_z1_world import (
    ASSETS,
    ImportFailure,
    extract_assets,
    parse_ines,
    verify_rom,
    write_cache,
)


def synthetic_rom() -> bytes:
    header = bytes((0x4E, 0x45, 0x53, 0x1A, 8, 0, 0x12, 0)) + bytes(8)
    prg = bytes(((index * 37 + 11) & 0xFF) for index in range(128 * 1024))
    return header + prg


class ImportZ1WorldTests(unittest.TestCase):
    def test_parse_expected_ines_layout(self) -> None:
        image = parse_ines(synthetic_rom())
        self.assertEqual(image.prg_size, 128 * 1024)
        self.assertEqual(image.chr_size, 0)
        self.assertEqual(image.mapper, 1)
        self.assertTrue(image.battery)

    def test_revision_mismatch_is_loud(self) -> None:
        with self.assertRaisesRegex(ImportFailure, "unsupported Zelda 1 revision"):
            verify_rom(synthetic_rom())

    def test_deterministic_extraction_and_cache_reuse(self) -> None:
        rom = synthetic_rom()
        accepted = hashlib.sha1(rom).hexdigest()
        image = verify_rom(rom, accepted_sha1=accepted)
        manifest, files = extract_assets(rom, image)
        self.assertEqual(len(files), len(ASSETS))
        self.assertEqual(manifest["world_pack"], "zelda1-first-quest")
        first = ASSETS[0]
        self.assertEqual(
            files[first.name + ".bin"],
            rom[16 + first.offset:16 + first.offset + first.length],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            cache = write_cache(root, manifest, files)
            self.assertEqual(cache, write_cache(root, manifest, files))
            on_disk = json.loads((cache / "manifest.json").read_text())
            self.assertEqual(on_disk, manifest)

    def test_refuses_to_overwrite_modified_cache(self) -> None:
        rom = synthetic_rom()
        image = verify_rom(rom, accepted_sha1=hashlib.sha1(rom).hexdigest())
        manifest, files = extract_assets(rom, image)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            cache = write_cache(root, manifest, files)
            (cache / "manifest.json").write_text("{}")
            with self.assertRaisesRegex(ImportFailure, "refusing to overwrite"):
                write_cache(root, manifest, files)


if __name__ == "__main__":
    unittest.main()

