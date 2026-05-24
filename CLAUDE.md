# CLAUDE.md — MinishCapRecomp

This game repo defers all rules to the platform core:

- `../gbarecomp/CLAUDE.md`     — execution contract
- `../gbarecomp/PRINCIPLES.md` — recomp principles
- `../gbarecomp/DEBUG.md`      — debug loop (RULE 0, RULE 0a, RULE 0b)
- `../gbarecomp/TCP.md`        — TCP debug protocol

## Showstopper: no interpreter, no game

**This game does not begin until the runtime is fully recompiler-
driven, including the BIOS.** See:
- `../gbarecomp/PRINCIPLES.md` "BIOS is sacred — and recompiled, not
  interpreted (SHOWSTOPPER)"
- `../gbarecomp/PRINCIPLES.md` "Interpreter is informative, never
  load-bearing (SHOWSTOPPER)"

If `MinishCapRecomp.exe`'s runtime exec loop calls into
`armv4t::Interpreter` for any PC — BIOS or cart — title-screen work
is suspended until the recompiler-only path is restored. Phase 2.7
(BIOS intro flawless) must be re-passed with **recompiled BIOS
execution** before Phase 5 milestones (first cart instruction,
first game function, title screen) resume.

## Game-specific rules

1. **The decomp is a reference, not an oracle.** `zeldaret/tmc` is
   imported through `tools/import_tmc_symbols/` and only its symbol
   names, function boundaries, asset labels, and ROM-layout
   annotations enter this project. The decomp's C source code, its
   PC-port runner, its renderer/audio/input shims — none of these
   may be linked, copied, or used as ground truth.
2. **mGBA is the oracle.** When the recompiled build differs from
   mGBA, mGBA is right unless we can show otherwise with a hardware
   test ROM. NanoBoyAdvance is a tiebreaker; never the primary.
3. **ROM hash gates everything.** The runner refuses to launch with
   an unknown ROM. Versions live in `config/<region>.toml`. New
   versions are added by checksum, not by guessing.
4. **No Minish Cap special cases in the GBA core.** If we discover
   that Minish Cap exercises an obscure hardware corner, the fix
   lives in `gbarecomp/src/gba/` with a hardware-test citation,
   not behind `if (game == "minish_cap")`.
5. **Generated C is never edited.** `generated/*` is regenerated
   by `gba_recompile`. If output is wrong, fix the recompiler.

## Validation milestones

In strict order, each measured (no eyeballing):

1. ROM hash verified.
2. Header / entrypoint decoded.
3. Reset / BIOS handoff matches oracle.
4. First executed game function identified.
5. First VBlank / IRQ path matches oracle.
6. First meaningful VRAM / OAM / PAL writes match oracle.
7. Title screen renders.
8. Input reaches game code; menu navigation works.
9. New-game path starts.
10. Save type detected, save / load round-trips against oracle.
