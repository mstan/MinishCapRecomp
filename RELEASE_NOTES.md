# MinishCapRecomp v0.0.1

First cut. The Legend of Zelda: The Minish Cap (GBA, USA) reaches its
title screen via the static gbarecomp pipeline — recompiled BIOS,
recompiled cart, `runtime_dispatch` only, no interpreter on the hot
path.

## What's in this release

- `MinishCapRecomp.exe` — game runner. Loads your GBA BIOS + your
  Minish Cap ROM, hash-verifies both, and runs the recompiled output.
- `game.toml` + `config/minishcap_usa.toml` — runtime config.
- `bios.cfg` + `rom.cfg` are written next to the .exe after a
  successful pick.
- `SDL2.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
  `libwinpthread-1.dll` — runtime dependencies (MSYS2 mingw64 build).
- `LICENSE` — PolyForm Noncommercial 1.0.0.
- `START_HERE.txt` — first-launch instructions.

## What's NOT in this release

- **No GBA BIOS.** Provide your own `gba_bios.bin` dump.
  SHA-1 `300c20df6731a33952ded8c436f7f186d25d3492`,
  CRC32 `0x21A2AE0A`.
- **No game ROM.** Provide your own Minish Cap (USA) cartridge dump.
  SHA-1 `b4bd50e4131b027c334547b4524e2dbbd4227130`,
  CRC32 `0x32D19810`.
- **No game source.** The recompiled cart C is regenerated locally at
  build time from the seed tables in `symbols/`. It does not ship.

## First launch

1. Run `MinishCapRecomp.exe`.
2. A Windows file picker appears for your `gba_bios.bin`. Pick it.
   The runtime hash-verifies — match → ok, mismatch → warning dialog
   then proceeds anyway.
3. A second picker appears for your Minish Cap (USA) ROM.
4. The recompiled boot path runs: BIOS intro (GAME BOY logo + chime),
   then cart code, then the Minish Cap title screen.

The validated paths are remembered in `bios.cfg` and `rom.cfg`. Delete
them to pick again.

Default keymap (matches the gbarecomp framework):
- Z = A, X = B, Return = Start, Right Shift = Select
- Arrow keys = D-pad
- S = R, A = L
- Esc = quit

## Status

What works:
- BIOS intro is byte-identical to mGBA on framebuffer / PAL / VRAM /
  OAM (per Phase 2.7 acceptance).
- The recompiled BIOS hands off into Minish Cap's cart entry.
- Title screen renders with the recompiled-only runtime (no
  interpreter on hot path; HP-001 gate stays closed).

What does not yet work:
- Pressing Start from the title currently lands on a function the
  recompiler hasn't reached. The runtime aborts with a clear
  `dispatch_miss` / `unimplemented_op` message naming the gap; that
  abort is the next gate, not the previous one.
- Save / load round-trip (EEPROM 8 KB), input through to player
  movement, audio equality through to perceptual-tolerance level —
  all pending.

This is a snapshot of the Phase 2.8.D milestone: BIOS + cart boot to
title via recompiled execution, validated end-to-end. Subsequent
releases will widen the recompiler until the game is playable.

## Compatibility

ROM tested against: USA (BZME) SHA-1 `b4bd5...7130`. Other regions
(EU, JP) will warn-and-try; the recompiler hasn't been validated
against them and behavior is undefined.

BIOS tested against: canonical Nintendo dump SHA-1 `300c2...3492`.
