# Validation Plan

This file tracks how we'll prove each milestone in
`../../gbarecomp/docs/ROADMAP.md` Phase 5. None of these is
"looks right" — each is a measured comparison.

The pattern is the same for every milestone:

1. **Sync point** — a hardware-event count we can land both native
   and oracle on (VBlank IRQ count, BIOS handoff return PC, etc.).
2. **Observable** — what we read at the sync point (registers, IO
   regs, VRAM bytes, framebuffer pixels).
3. **Pass criterion** — exact equality with the oracle, or an
   explicit and documented tolerance.
4. **Re-test command** — a script under `tools/` that anyone can
   re-run without manual setup.

## Milestone 1 — ROM hash verified

- Sync point: pre-boot.
- Observable: SHA-1 of the loaded ROM bytes.
- Pass: matches `config/<region>.toml [rom] sha1`.
- Re-test: `verify_rom_hash <path> [region]`.

## Milestone 2 — Header / entrypoint decoded

- Sync point: pre-boot.
- Observable: parsed header fields (entry-point branch target,
  game code, save chip string detected in ROM).
- Pass: header fields match those derived independently from the
  cartridge image with a hex dump.
- Re-test: `gba_scan` on the ROM.

## Milestone 3 — Reset / BIOS handoff matches oracle

- Sync point: first instruction after BIOS hands control to the
  cartridge entry point.
- Observable: R0..R15, CPSR, banked SP/LR for SVC mode.
- Pass: every value matches mGBA at the same handoff point.
- Re-test: scripted launch + `get_registers` on both sides.

## Milestone 4 — First executed game function identified

- Sync point: first call from the entry-point thunk into game code.
- Observable: PC + ARM/THUMB mode + caller PC.
- Pass: matches the symbol map landmark we expected.
- Re-test: scripted launch + `trace_calls` first entry.

## Milestone 5 — First VBlank / IRQ path matches oracle

- Sync point: first VBlank IRQ taken.
- Observable: SPSR_irq, LR_irq, PC at IRQ vector entry, IE/IF/IME
  at entry.
- Pass: identical to mGBA's same VBlank.
- Re-test: scripted launch with VBlank breakpoint.

## Milestone 6 — First meaningful VRAM / OAM / PAL writes match oracle

- Sync point: 10 frames after first VBlank, or first frame after
  game's title screen draws.
- Observable: full VRAM, OAM, PAL byte dumps.
- Pass: byte-equal to mGBA.
- Re-test: scripted launch + `memory_diff`.

## Milestone 7 — Title screen renders

- Sync point: first frame where the title screen's expected
  framebuffer is present (oracle-defined).
- Observable: framebuffer hash.
- Pass: hash matches mGBA's at the same frame.
- Re-test: scripted launch + `framebuf_diff`.

## Milestone 8 — Input works

- Sync point: 30 frames after pressing Start at title screen.
- Observable: PC / R0..R15 in the menu-handler function.
- Pass: match mGBA at the same input sequence.
- Re-test: scripted input + `get_registers`.

## Milestone 9 — New game path starts

- Sync point: file-select → confirm → first overworld frame.
- Observable: framebuffer hash + VRAM hash.
- Pass: match oracle.
- Re-test: scripted input + `framebuf_diff`.

## Milestone 10 — Save type detected, save / load round-trip

- Sync point: after first in-game save.
- Observable: save chip state machine log + persisted EEPROM bytes.
- Pass: persisted bytes match what mGBA would persist for the same
  in-game action.
- Re-test: scripted save + diff against oracle's persisted save.

## What this plan rules out

- Eyeball validation. Every milestone has a programmatic re-test.
- Cherry-picked frames. We compare on a sync-event, not a
  hand-chosen frame number.
- "Probably good." Either the bytes / pixels / hash match the
  oracle, or the milestone isn't done.
