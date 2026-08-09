# Zelda 1 foreign-world importer

This tool validates a user-owned **The Legend of Zelda (USA, PRG0)** iNES ROM
and builds a deterministic local cache for the foreign-world mod. ROM bytes and
extracted assets are deliberately excluded from source control.

```powershell
python tools/z1_world_importer/import_z1_world.py `
  "C:\path\to\Legend of Zelda.nes" `
  --output build/foreign-world-cache
```

The accepted whole-ROM SHA-1 is
`dab79c84934f9aa5db4e7dad390e5d0c12443fa2`. The importer also checks the iNES
layout, mapper, SHA-256, and PRG payload hash before extracting anything. It
creates a content-addressed directory with a versioned `manifest.json` and
per-asset hashes. The verified PRG payload is retained inside this ignored
cache so future world-pack parser revisions do not depend on the original ROM
path. Re-running is safe and reuses an intact cache; it will not overwrite a
cache that has been modified.

Run its self-contained tests with:

```powershell
python -m unittest discover tools/z1_world_importer -p "test_*.py"
```
