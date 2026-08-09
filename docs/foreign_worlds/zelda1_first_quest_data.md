# Zelda 1 First Quest PRG typed-data view

This slice reads no guest memory and invokes no game hook or renderer. It
consumes only a previously verified, headerless 128 KiB PRG payload—normally
the ignored `source/prg.bin` written by `tools/z1_world_importer`. `VerifiedPrg`
checks that fixed size and every named-span bound; it does not hash bytes. The
importer's required ROM/PRG hash gate establishes content identity before this
layer is constructed. The class gives out only immutable byte spans and does
not search for a ROM or cache path. The optional test argument is the sole path
that causes a real PRG/cache file to be opened.

The named-span schema exactly uses the importer names and PRG offsets for the
world data: `source/prg`, both room-layout spans, all five level-block spans,
and `level_info_overworld` plus `level_info_underworld_1` through `_9`.
Unknown names, null outputs, and spans outside the verified PRG are errors.
The source basis is `aldonunez/zelda1-disassembly` commit
`50a1c869a8d8e2eb8b5b60acea325f44b4341762`, `src/bins.xml`, `Z_05.asm`,
`Z_06.asm`, and `Variables.inc`.

## Decoded coverage

`Z_06.asm:LevelBlockAddrsQ1` establishes the First Quest table selection:
levels 1–6 use `LevelBlockUW1Q1`; levels 7–9 use `LevelBlockUW2Q1`. The level
info table is `LevelInfoAddrs`, one `LevelInfoUWn` block per level. Every level
block remains six label-preserving 128-byte planes, `LevelBlockAttrsA` through
`F`, as copied by `FetchLevelBlockDestInfo` to `$687E..$6B7D`. Their byte-level
meaning is not inferred here.

The overworld is a room ID grid with `room_id = y * 16 + x`, for `x=0..15` and
`y=0..7`. `Z_05.asm:LayoutRoomOW` selects `LevelBlockAttrsD[room_id] & $3F` and
multiplies it by `$10`, so this slice exposes that exact six-bit layout
reference and its 16 raw column-descriptor bytes from `RoomLayoutsOW`. It does
not assign terrain, collision, or tile semantics to those descriptors. Bytes
in the remainder of the 1936-byte importer span that are not selected by this
formula remain opaque.

`LevelInfoView` uses the exact RAM label offsets in `Variables.inc`, measured
from `LevelInfo_PalettesTransferBuf = $6B7E`:

| Offset | Field/label | Decoding |
| --- | --- | --- |
| `$00..23` | `LevelInfo_PalettesTransferBuf` | Opaque 36-byte span |
| `$24..27` | `LevelInfo_FoeCounts` | Opaque four-byte span |
| `$28` | `LevelInfo_StartY` | Byte |
| `$29..2C` | `LevelInfo_ShortcutOrItemPosArray` | Opaque four-byte span |
| `$2D` | `LevelInfo_SubmenuMapRotation` | Byte |
| `$2E` | `LevelInfo_StatusBarMapXOffset` | Byte |
| `$2F` | `LevelInfo_StartRoomId` | Byte |
| `$30` | `LevelInfo_TriforceRoomId` | Byte |
| `$31..32` | `LevelInfo_WorldFlagsAddr` | Little-endian 16-bit address |
| `$33` | `LevelInfo_LevelNumber` | Byte |
| `$34..3D` | `LevelInfo_CellarRoomIdArray` | Opaque ten-byte span |
| `$3E` | `LevelInfo_BossRoomId` | Byte |
| `$3F..4E` | `LevelInfo_SubmenuMapMask` | Opaque 16-byte span |
| `$4F..7B` | `LevelInfo_StatusBarMapTransferBuf` | Opaque 45-byte span |
| `$7C..DB` | `LevelInfo_PaletteCycles` | Opaque 96-byte span |
| `$DC..FB` | `LevelInfo_DeathPaletteSeries` | Opaque 32-byte span |

All byte sequences called opaque remain unparsed deliberately. First/Second
Quest replacement mechanics, room-layout column heaps, tile expansion,
collision, room flags, and every gameplay interpretation are outside this
slice until independently source-justified and tested.
