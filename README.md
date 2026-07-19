# MinishCapRecomp — The Legend of Zelda: The Minish Cap, Recompiled

> _This recompilation is a **byproduct of developing
> [gbarecomp](https://github.com/mstan/gbarecomp)** — the games are the proving ground, the framework is the goal.
> **These are in-development previews, not finished ports — expect rough
> edges**, and depth will keep landing over months, not days. My time for any
> one title is limited, so I ask for your patience. Contributions are welcome —
> testing, issues, and PRs to the game or framework all help and will
> accelerate this game's polish. More on the why at:
> [Recomp + AI: 5 Months Later »](https://1379.tech/recomp-ai-5-months-later/)_

Static recompilation of **The Legend of Zelda: The Minish Cap** (Game Boy Advance)
to native PC, built on the [`gbarecomp`](https://github.com/mstan/gbarecomp)
framework. Minish Cap is `gbarecomp`'s original target and the most mature of the
GBA recomps here.

> ### Status — playable into gameplay (v0.0.1), and self-improving
>
> This is a **static-recompilation base + runner**, not a finished port. It
> **boots through the BIOS intro to the title screen and into gameplay** —
> overworld, dialogue, and save states round-trip. It is the most-complete GBA
> title in this collection, but still **early**: not every code path is statically
> recompiled, and content is not exhaustively tested.
>
> **It gets better the more you play.** Any code path the static recompiler hasn't
> covered runs through a built-in **interpreter the first time it's hit**, then is
> **JIT-compiled to native** (in-process, no toolchain needed) and **remembered on
> disk** — so the next launch runs it natively from the start. Interpreted once,
> native ever after; coverage grows toward fully-native as the game is played. See
> [How it self-improves](#how-it-self-improves).

---

## What "static recompilation" means here

The ROM's **ARM7TDMI machine code is statically translated to native C** — every
function the game runs becomes a real generated C function. Unlike most recomp
projects, **the GBA BIOS is recompiled and executed too** (not HLE'd or stubbed),
so the boot sequence and interrupt/SWI handlers run as real recompiled code. The
rest of the console — the PPU (graphics), APU + M4A sound engine, DMA, timers, the
cartridge EEPROM save chip, and hardware I/O — is modeled by the `gbarecomp`
runtime.

Only **symbol metadata** (function names, addresses, sizes) from the
[`zeldaret/tmc`](https://github.com/zeldaret/tmc) decompilation enters this repo —
never its C source, PC-port runner, or toolchain. **The ROM is never
redistributed**; you supply your own legally-dumped copy.

## ROM

| Target            | Game                                  | ROM (USA) | SHA-1                                      | Debug port |
|-------------------|---------------------------------------|-----------|-------------------------------------------|------------|
| `MinishCapRecomp` | The Legend of Zelda: The Minish Cap   | USA       | `b4bd50e4131b027c334547b4524e2dbbd4227130` | 19842      |

The runtime **refuses to launch on an unrecognized ROM** — the SHA-1 must match.
Save chip: EEPROM. (Other regions can be added by checksum in `config/<region>.toml`.)

## Quick start

1. Download the latest `MinishCapRecomp-windows-x64` zip from
   [Releases](../../releases) and extract it (or build from source — see below).
2. Run `MinishCapRecomp`.
3. Supply your own **legally-obtained** Minish Cap (USA) ROM when prompted. The
   path is cached next to the exe for future launches.
4. Play. Early on you may briefly see the interpreter warm up new code paths; once
   warmed (and cached), they run native.

## Controls

| GBA button | Keyboard      |
|------------|---------------|
| D-Pad      | Arrow keys    |
| A          | Z             |
| B          | X             |
| L / R      | A / S         |
| Start      | Enter         |
| Select     | Backspace     |

Save states: **Shift+F1–F9** save to a slot, **F1–F9** load it.

## Experimental resize-driven extended view

The faithful default remains 240x160. The experiment is intentionally hidden
from the pre-boot launcher until its performance has been audited. Developers
can opt in by passing `--resize-view` or setting the following in `game.toml`:

```toml
[video]
resize_view = true
```

This is intentionally different from Mega Man Zero's fixed-width modes. The
window still opens at the native 3:2 size. As its aspect ratio becomes wider,
the logical framebuffer widens from 240 up to 480 pixels while remaining 160
pixels tall, revealing additional world on both sides of the original camera.
At 3:2 the literal native renderer is used; a typical 21:9 fullscreen display
requests roughly 373x160. Resizing to a larger window with the same 3:2 aspect
only scales the native image and does not reveal more world.

This first implementation is a feasibility prototype. The side margins read
Minish Cap's complete rendered room-layer buffers rather than the GBA's wrapping
32x32 hardware tilemap, so authored scenery continues without repeating. An
actual room edge clears naturally where no adjacent room pixels exist.
The gameplay HUD follows the physical corners as the view changes width, while
dialogue remains centered over the native play area. Entities, scripted
triggers, other screen-space effects, and the hardware OAM limit retain original
behavior, so they can pop in or assume the 240-pixel viewport. Remove the
explicit opt-in for faithful presentation.

## How it self-improves

`gbarecomp`'s coverage is honest: a path that wasn't statically recompiled is
**bridged through the interpreter** the first time, *loudly*, then healed:

- **First hit:** the interpreter runs the missed function (correct, just not
  native) and the runtime records it.
- **Heal:** the function is **JIT-compiled to native in-process** via a
  toolchain-less backend (sljit) — no compiler required on your machine.
- **Persist:** the healed path is written to a per-ROM cache
  (`recomp_cache/<rom-sha1>/`), so **the next launch re-JITs it up front** and it
  runs native from the start.

The result is a game that converges toward fully-native execution the more it's
played, and **stays** improved across launches. A handful of instruction patterns
the JIT can't lower yet stay on the interpreter (precision over recall); those are
emitter gaps that close over time. Self-improvement is on by default; set
`GBARECOMP_SELFHEAL_RECOMPILE=0` for a pure-interpreter run.

## Building from source

**Prerequisites (Windows):** [MSYS2](https://www.msys2.org/) with the mingw64
toolchain (`gcc`/`g++`), CMake 3.16+, Ninja, and SDL2 (mingw64 package). Builds
are invoked from PowerShell with the mingw64 toolchain on `PATH`.

**1. Clone this repo next to `gbarecomp`** (it builds against the sibling engine
checkout on `main`):

```
git clone https://github.com/mstan/gbarecomp.git
git clone https://github.com/mstan/MinishCapRecomp.git
cd MinishCapRecomp
```

**2. Supply your ROM** at `roms/minishcap_usa.gba` (SHA-1 above). ROMs are
gitignored and never committed.

**3. Recompile + build.** The committed `symbols/` map is the importer output, so
you can regenerate the C and build directly:

```
# from PowerShell, mingw64 on PATH
gba_recompile --rom roms/minishcap_usa.gba --config symbols/minishcap.toml --out generated
cmake -S . -B build -G Ninja -DGBARECOMP_ROOT=../gbarecomp
cmake --build build --target MinishCapRecomp
```

(`gba_recompile` is built from the `gbarecomp` checkout; see that repo's README.)
The recompiler emits deterministic parallel translation units; current
gbarecomp also rejects the retired monolithic `recompiled.cpp` output.

## Legal

This project contains **no copyrighted ROM data, no Nintendo BIOS, and no decomp
source** — only original recompiler/runtime code and symbol metadata. **You must
supply your own legally-dumped ROM** (and BIOS, where the runtime requires one).
The Legend of Zelda and The Minish Cap are trademarks of Nintendo; this project is
an unaffiliated, non-commercial preservation and research effort.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
