# MinishCapRecomp

Static recompilation of *The Legend of Zelda: The Minish Cap* (GBA),
built on top of [`gbarecomp`](../gbarecomp).

This is a **recomp**, not a port and not a decomp. We take the
original ROM's ARM/THUMB machine code and lift it to native C/C++
that runs against a principled GBA hardware/runtime model.

The decomp at [`zeldaret/tmc`](https://github.com/zeldaret/tmc) is a
valuable reference for symbols, function boundaries, ROM layout, and
asset labels. It is **not** an execution oracle and we do not lift
its PC-port runtime. See `../gbarecomp/docs/GBA_REFERENCE_NOTES.md`
for what we may and may not borrow.

---

## What this repo contains

| Path                | Purpose                                                       |
|---------------------|---------------------------------------------------------------|
| `game.toml`         | ROM identity, entry point, save chip, recompiler config.      |
| `baserom.md`        | Documented ROM hashes and where the user puts their ROM.      |
| `config/`           | Per-region configs (USA/EUR/JPN) layered on `game.toml`.      |
| `symbols/`          | Imported symbol map + function boundaries (TSV).              |
| `generated/`        | Output of `gba_recompile`. **Never** hand-edited.             |
| `src/main.cpp`      | Game runner entry point (links against `gbarecomp_runtime`).  |
| `src/game_config.*` | Game-specific config wiring.                                  |
| `tools/`            | Symbol importer + ROM hash verifier.                          |
| `docs/`             | Project notes, borrowed-references map, validation plan.      |

---

## Build

You need a built `gbarecomp` at `../gbarecomp`. Then:

```
cmake -B build -S . -DGBARECOMP_ROOT=../gbarecomp
cmake --build build
```

This produces:
- `verify_rom_hash` — refuses to launch the game with an unknown ROM.
- `import_tmc_symbols` — pulls symbol + boundary data from a local
  checkout of `zeldaret/tmc` into `symbols/`. The decomp source
  itself never enters this repo.
- `MinishCapRecomp` — the game binary, linked against the recompiled
  generated C and `gbarecomp_runtime`.

---

## Status

Phase 0. ROM hash is not yet verified, no functions are yet
recompiled, and no boot path is yet validated. See
`../gbarecomp/docs/ROADMAP.md` Phase 5 for the milestone list. Do
**not** claim boot progress before each milestone is measured.

---

## Provide your own ROM

We don't distribute the ROM. The build expects a local path the user
provides in `baserom.md`. The runner verifies the SHA-1 before doing
anything and refuses to start if the hash isn't recognized.
