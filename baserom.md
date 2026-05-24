# Base ROM (and BIOS)

We do **not** ship the ROM or the BIOS. You must provide your own
dumps of both:

1. **BIOS** — see [`../gbarecomp/bios/README.md`](../gbarecomp/bios/README.md).
   Expected at `../gbarecomp/bios/gba_bios.bin`. SHA-1
   `300c20df6731a33952ded8c436f7f186d25d3492`.
2. **Cartridge ROM** — your own dump of a known-good Minish Cap
   cartridge. See below.

The runner refuses to launch unless **both** verify. The BIOS is
not optional: every game in `gbarecomp` boots through the real
BIOS (see `../gbarecomp/PRINCIPLES.md` "BIOS is sacred").

## How to point the build at it

Edit `game.toml` and set `rom = "..."` (or pass
`-DMINISHCAP_ROM=...` on the cmake configure line). The runner
verifies the SHA-1 on startup against the table below and refuses
to launch with an unrecognized hash.

## Known-good ROMs

The Minish Cap shipped in multiple regions. Each region has its own
SHA-1 and its own `config/<region>.toml` because internal symbol
addresses and IO timing nuances can differ.

| Region | SHA-1                                      | Game code | Internal name        |
|--------|--------------------------------------------|-----------|----------------------|
| USA    | `b4bd50e4131b027c334547b4524e2dbbd4227130` | `BZME`    | The Minish Cap (USA) |
| EUR    | TBD                                        | `BZMP`    | TBD                  |
| JPN    | TBD                                        | `BZMJ`    | TBD                  |

The USA SHA-1 is cross-verified against `zeldaret/tmc/tmc.sha1`.

We deliberately leave the hashes blank in this scaffold — they go in
once verified, not from secondhand sources. The verifier will reject
any unverified hash so accidental wrong-ROM use is loud.

## What we don't accept

- Trimmed ROMs (header pad removed). The original cartridge image is
  what hardware sees, including pad bytes.
- IPS/UPS-patched ROMs (translation patches, randomizers, etc.).
  Recompiler output is keyed to specific opcodes at specific
  addresses; a patched ROM is a different game and needs its own
  hash entry.
- Decomp-built ROMs. The decomp produces a byte-different artifact;
  even if it boots, it isn't the original cartridge.
