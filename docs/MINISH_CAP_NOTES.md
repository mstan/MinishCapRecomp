# Minish Cap notes

Game-specific facts that the platform core has no business knowing.
None of this is execution truth — facts here are pulled from public
references and must be verified against the actual cartridge before
they're load-bearing.

## Hardware quirks we've actually observed

(empty in the Phase 0 scaffold)

When something gets added here, the entry should cite:

1. Where it was observed (frame ring index, oracle command).
2. Which hardware reference confirms it's a real GBA behavior, not
   a Minish-Cap-specific artifact of our recompile.
3. Where in `gbarecomp/src/gba/` the matching support lives.

If a quirk doesn't fit (3), it's *probably* a recompiler bug
masquerading as a hardware quirk — keep digging.

## Symbol map landmarks

(filled in once `import_tmc_symbols` is wired)

Known landmarks we'll want to cross-reference during boot:

- ROM header / crt0 entry.
- IRQ handler entry.
- BIOS handoff return point.
- Main game loop function.
- Title screen state functions.

## What we won't borrow from the decomp

Per `../../gbarecomp/docs/GBA_REFERENCE_NOTES.md`:

- Decompiled C as execution truth.
- PC-port renderer / audio / input shims.
- Any HLE helper.

If you find yourself reaching for one of these, you have a recomp
bug; fix the recomp.
