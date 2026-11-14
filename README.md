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

## Definitive Edition 1.4 mod

**Definitive Edition 1.4 by Zeldafangirl** is an optional package in Mods,
disabled by default. Supply the original USA ROM above and select the exact
`MinishCapDefinitiveEdition_USA1.4.ips` from the author's
[patch release](https://www.romhacking.net/hacks/9976/). The patch is not bundled.
Its SHA-256 must be
`c96b61dff8563d4b9e1fe5730c985f1523aca57f89a6f0a4655b11313c6c24b2`.
This integration supports that USA version; it does not accept arbitrary patches
or prepatched cartridges. Turn the package off to play the original game.

The complete patch includes quick-item wheels, difficulty options, expanded
wallets and the four-digit rupee display, and its other gameplay adjustments.
Hold L or R for the corresponding item wheel. L+R changes which equipped slot
future wheel selections target. In file-selection Options, A confirms and saves
a difficulty change; B cancels it. SELECT opens the map after Link has acquired
the normal map item, and B exits the map or inventory.

Ordinary native progress uses `<rom>.sav`. With the package enabled, its default
is `<rom>.definitive-1-4.sav`; the launcher Save row and import/clear actions
follow that selection. An explicitly configured save path remains an intentional
override. Adaptive Widescreen can be enabled independently.

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

## Adaptive Widescreen mod

The faithful default remains 240x160. Open **Mods** in the pre-boot launcher
and enable **Adaptive Widescreen** to opt into the game-specific extended view.
The feature is disabled by default and persists independently in
`mods/state.toml`; old `--resize-view`, `game.toml`, and environment settings
do not bypass the mod's ROM and plugin validation.

This is intentionally different from Mega Man Zero's fixed-width modes. The
window still opens at the native 3:2 size. As its aspect ratio becomes wider,
the logical framebuffer widens from 240 up to 480 pixels while remaining 160
pixels tall, revealing additional world on both sides of the original camera.
At 3:2 the literal native renderer is used; a typical 21:9 fullscreen display
requests roughly 373x160. Resizing to a larger window with the same 3:2 aspect
only scales the native image and does not reveal more world.

The side margins read Minish Cap's complete rendered room-layer buffers rather
than the GBA's wrapping
32x32 hardware tilemap, so authored scenery continues without repeating. An
actual room edge clears naturally where no adjacent room pixels exist.
The gameplay HUD follows the physical corners as the view changes width, while
dialogue remains centered over the native play area. Entities, scripted
triggers, other screen-space effects, and the hardware OAM limit retain original
behavior, so they can pop in or assume the 240-pixel viewport. Disable the mod
to return to faithful presentation.

## Zelda 1 foreign-world hidden checkpoint

This is a **developer-only hidden checkpoint**, not a player-facing feature.
Release builds and release catalogs contain neither the Zelda manifest nor its
plugin/foreign-world runtime, so recreating or installing a manifest cannot
activate it. Developers must explicitly configure
`-DMINISH_ENABLE_ZELDA1_FOREIGN_WORLD=ON` to stage the separate checkpoint.

That build requires the user's exact 131,088-byte **The Legend of Zelda (USA,
PRG0)** iNES image, SHA-1 `dab79c84934f9aa5db4e7dad390e5d0c12443fa2`. The
runtime fails closed before direct, bounded in-memory extraction—like the
Samus approach—and no ROM or derived Zelda content is shipped. A persistent
Falcon-style extracted-asset cache remains future work and is not required for
this hidden checkpoint.

The disabled-by-default developer **Zelda 1 Foreign World** package requires and
hash-validates that user-owned Zelda 1 USA PRG0 iNES image before its trusted
plugin can run. The current entrance is the visible rift in the white-flower
patch left of Link's house in South Hyrule Field (area 3, room 1). Walk onto it
to enter; no action button is required. It arms only after a safe outside
sample, so loading or returning with Link already overlapping it cannot enter
unexpectedly. The temporary explicit exit remains **Up + Down + Left + Right**
after releasing the D-pad once in Zelda. The complete source-mapped readiness
gate rejects transitions, scripts, dialogue, unavailable normal control, and
hidden or dead Link states.

Inside the foreign world, ordinary D-pad input advances the host-owned Zelda
position once per Minish source frame through decoded Zelda terrain collision
and screen edges. The existing Player-only movement hook suppresses native-map
coordinate movement while active; it never writes guest coordinates. The PPU
instead applies a bounded data-only focus transform to Link's existing Minish
OBJ, preserving his native animation, items, sword behavior, OBJ ordering,
windows, and color effects over the decoded 240x160 BGR555 terrain.

The current playable vertical slice includes source-backed overworld rooms, the
OW77 Old Man/sword cave, four source-rostered red Octoroks in OW66, and a
source-room route through First Quest Level 1 to six-HP Aquamentus. The start
cave dialogue reveals and completes on its source six-frame cadence. After it
finishes, walk onto the sword to acquire the exact `Zelda1/01` record; no action
button is required. A compatible sword already present in durable inventory is
reconciled idempotently with the cave source instead of leaving an
uncollectible respawn. Native and Zelda-origin items remain distinct, and
either origin's selected sword can be used by the implemented foreign-world
combat adapters. Native and Zelda resource pools are also kept separate.
Injecting Zelda-origin item behavior back into ordinary Minish Cap rooms still
awaits a source-proven guest equipment boundary.

Deterministic QA routes from the initial OW `$77` position are expressed as
held D-pad source frames: the start cave is `Left 64, Up 64`; OW `$66` is
`Up 232, Left 128`; and the Level 1 portal is
`Up 231, Left 1, Up 24, Left 7, Up 8, Left 1, Up 152, Left 7, Up 8, Left 1,
Up 152, Right 1, Up 8, Left 1, Up 73, Right 1`. In the start cave, wait for the
dialogue to finish, then move Up 21 and Right 8 onto the source sword hotspot.
Move down to source Y `$DD` and take one more Down step to return to OW `$77`.

This is still an engineering QA slice, not a finished port. Octoroks, drops,
Aquamentus, and dungeon items use conspicuous host QA markers where NES
metasprites or drop semantics have not yet been decoded. Level 1 currently uses
a documented bounded intra-room movement policy, and only the implemented
rooms, cave, enemy roster, and dungeon route should be treated as playable.
The compositor deliberately preserves Minish OBJ/window/color-effect rules;
those guest effects can occlude tiny source cave sprites in a live frame even
though the direct source renderer and pickup collision retain them. The exact
QA movement counts above remain the reliable temporary interaction guide.
Trusted snapshot state covers the cross-world inventory and live overworld,
cave, and OW66 combat record; ordinary Minish save-file sidecar attachment and
the final return-portal/NPC UI remain later integration work.

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
checkout with the native-program profile support used by this title):

```
git clone https://github.com/mstan/gbarecomp.git
git clone https://github.com/mstan/MinishCapRecomp.git
cd MinishCapRecomp
```

**2. Supply your ROM** at `roms/minishcap_usa.gba` (SHA-1 above), the normal
GBA BIOS, and the exact author USA1.4 IPS described above. Inputs and generated
code are local build data and must not be committed.

**3. Recompile + build.** The committed `symbols/` map is the importer output, so
you can regenerate the C and build directly:

```
# from PowerShell, mingw64 on PATH
python tools/generate_definitive14.py --compiler ../gbarecomp/build/gba_recompile.exe --rom roms/minishcap_usa.gba --ips patches/MinishCapDefinitiveEdition_USA1.4.ips
cmake -S . -B build -G Ninja -DGBARECOMP_ROOT=../gbarecomp
cmake --build build --target MinishCapRecomp
```

(`gba_recompile` is built from the `gbarecomp` checkout; see that repo's README.)
The helper verifies both exact inputs, generates the original program plus a
separately prefixed complete Definitive Edition program, and includes the
reviewed copied-code metadata. It leaves the original ROM unchanged. The
recompiler emits deterministic parallel translation units; current gbarecomp
also rejects the retired monolithic `recompiled.cpp` output. Compilation links
both programs so the mod can be selected without rebuilding.

## License

For this project's code covered by the [PolyForm Noncommercial License](LICENSE),
[PERMISSIONS.md](PERMISSIONS.md) grants standing permission for voluntary
donations, sponsorships, and advertising revenue, including support for broader
development initiatives. Payment must not buy access to the software, games
built with it, or their updates, features, or game content, including early access.

Matthew Stanley has authority from the relevant copyright holders to grant
these permissions for the project's PolyForm-covered code. Third-party
components and code explicitly licensed under other terms retain those terms.

PolyForm Noncommercial 1.0.0 — see [`LICENSE`](LICENSE). Third-party
components retain their own licenses.

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
