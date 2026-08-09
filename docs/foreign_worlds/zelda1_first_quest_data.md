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
`y=0..7`. `Z_05.asm:LayoutRoomOW` performs its 16-bit `$10` address add on the
complete `LevelBlockAttrsD[room_id]` byte; despite the nearby
`GetUniqueRoomId` helper masking `$3F`, that helper is not called here. This
is observable on OW `$77`, whose actual descriptor record is `$72`, rather
than `$32`. The typed view retains that raw layout reference and its 16 column
descriptors by the exact 16-bit PRG address the routine computes. This matters
for OW `$66`: reference `$E1` continues past the extracted `RoomLayoutsOW`
blob into following source-mapped PRG data. The typed layer validates that
computed address against the verified whole PRG; it does not substitute a
guessed six-bit reference.

`overworld_square_grid` now executes the source's column-directory and
column-heap walk against the verified PRG: all 16 descriptors expand to an
exact 16 x 11 grid of source primary-square selections, including its
one-extra-square repeat bit. The directory and primary-square lookup tables
remain in the verified PRG; no game table bytes are compiled into this project.
Each result keeps its raw descriptor plus primary square for a renderer. Its
collision remains `kOpaqueFinalTile` at the square-view layer: the original
checks collision only after expansion and tile-object substitutions.

For the playable First Quest route, the layer also decodes source-backed room
semantics:

* Overworld `AttrsB & $FC` is the entrance index used by `HandleWarpOW`.
  Values 1--9 are named levels, 16+ are caves, and 20 is a shortcut. The
  return X (`AttrsA & $F0`) and return square row (`AttrsF & 7`) are retained.
* `AttrsF` bits 4--5 select a `LevelInfo_ShortcutOrItemPosArray` entry. This
  yields an overworld shortcut/secret placement coordinate, but not a claim
  that the secret has been revealed: that is runtime world-flag state.
* `AttrsC` plus `AttrsD` bit 7 form the source 7-bit object-list/template ID;
  `AttrsC` bits 6--7 index the source foe-count table; `AttrsF` bit 3 is the
  edge-spawn flag. The IDs are intentionally not all renamed as enemies.
* Underworld rooms expose all four door types in E/W/S/N order. These are the
  documented encodings 0=open, 1=wall, 2/3=false walls, 4=bombable, 5/6=key,
  and 7=shutter. They also expose item ID/position, secret trigger, push-block
  and dark-room flags, and the same object placement request.

The optional real-cache test asserts the vertical-slice anchors without
embedding bytes: First Quest overworld start `$77`, Level 1 entrance at `$37`,
the start-screen sword cave as a normal cave, Level 1 start `$73`, and boss
room `$35`. The latter is source-confirmed as Aquamentus: its template `$3D`
selects `InitAquamentus`, which places it at its fixed boss coordinates. The
test also confirms its shutter/key door structure and deferred heart-container
item trigger.

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

All byte sequences called opaque remain unparsed deliberately. Remaining work
is host behavior, not a hidden data claim: render primary squares through a
licensed/verified tile pipeline; apply dynamic secret and tile-object changes;
implement tile-level collision; resolve object-list entries to host enemies;
track world/door/item flags and keys; and turn the decoded Level 1 graph into
interactive movement, combat, pickup, boss, and return-portal behavior.

`Zelda1WorldModel` persistence is a fixed 340-byte, version-2 provider blob.
It includes room flags, inventory facts, tick counter, actors (HP, position,
defeat/known-value flags), and pending opaque drops. Restore validates counts,
enums, flags, IDs, and bounded values before mutating model state.

The OW renderer binds `patterns/overworld_background.bin` directly to the
verified PRG slice named by the importer (offset 51515, length `$820`). It
fails closed unless the named ignored-cache file is byte-for-byte equal to that
slice. This is sufficient for the current single-asset renderer even without a
second JSON SHA parser: the cache bytes are proven equal to the already loaded
verified source bytes, while the manifest remains the importer provenance.

For foreign-world output, the viewport policy is a 1:1 centered crop of the
NES 256x176 playfield: source playfield `(x,y) = (8..247, 8..167)` (absolute
NES `(8..247, 72..231)`) maps to the full 240x160 GBA BGR555 framebuffer
(R bits 0..4, G 5..9, B 10..14). The
64-pixel NES status bar is deliberately outside this terrain framebuffer.
The renderer follows `LayoutRoomOW`, including its `CheckTileObject` static
primary-square substitution before `WriteSquareOW`, and uses the canonical NES
palette values in `nesrecomp/runner/src/ppu_renderer.c`. The actual-cache test
pins OW `$77` to FNV-1a `10743561982003409067` under GBA BGR555 packing and writes a build-only PPM; PNG
conversion and a stock-crop side-by-side are manual screenshot QA artifacts.

## OW `$77` sword-cave mouth and return

The initial `$77` layout is not missing a runtime-only mouth. Its source column
descriptor selects `SecondarySquaresOW[$0C]`, whose final tiles are
`$F3,$24,$F3,$24`; after the centered crop, the two `$24` trigger tiles occupy
viewport `x=56..71`, `y=16..23`. The actual stock crop matches this static
terrain (including the black mouth) pixel-for-pixel apart from Link's sprite.
There is therefore no world-flag substitution to invent for this screen.

`HandleWarpOW` accepts only final tiles `$24`, `$88`, or `$70..73`, then uses
`AttrsB & $FC` to select a cave or level. `Zelda1OverworldSession` exposes that
fact independently from collision: it checks the original stationary hotspot
alignment after converting its viewport point back to NES coordinates. For
this normal cave `$77` has source cave index `$10`. On exit, `InitMode2` derives
`ObjX=$40`, target `ObjY=$4D`, and initial walk-out `ObjY=$5D`; the session
returns at the last value, viewport `(56,21)`, where the original starts its
walk-down animation.

The session record is fixed 64-byte `Z1OS` version 8. It persists signed source
coordinates, source grid phase/direction, overworld/cave area, source cave
index, one-way starting-sword state, and (for the active start-cave textbox)
the visible-glyph cursor plus its source `ObjTimer+1` delay. Strict v5/v6/v7
migration produces a canonical settled source-grid record; v7's former
unacknowledged A gate resumes at the first automatic glyph. Restore rejects an
area/index combination that does not agree with the currently loaded room's
source entrance record.

## First Quest starting sword cave

The bounded normal-cave renderer is source-backed for the start cave selected
by OW `$77` (normal cave slot zero). It expands `Z_05:RoomLayoutOWCave0` with
the same column heap, primary-square, and final-tile code used by OW terrain;
uses `Z_06:CaveBgPaletteRowsTransferBuf` from verified PRG offset `107101`;
and applies `InitModeB_Sub5`'s room `$44` AttrsA palette selector. It remains
the same centered 240x160 BGR555 playfield crop.

`Z_01:InitCave` proves the cave person is source object `$6A` at `($78,$80)`.
The pinned reset in `Z_07` writes `PPUCTRL=$30`, selecting 8x16 OAM sprites:
each OAM tile is its upper 8x8 CHR tile followed by the next lower tile. The
renderer consequently composes the Old Man's mirrored `$98/$99` pair, both
standing fires' `$5C/$5D` and `$5E/$5F` pairs, and the wooden sword's narrow
`$20/$21` pair at their source positions, with the source descriptor palette
and horizontal flip. This is static frame-zero composition only; fire animation
remains intentionally outside the renderer. `Z_01:InitCaveContinue`
copies source cave-item bytes `$3F,$01,$7F`; the middle `$01` is the wooden
sword. `DrawCaveItems` locates it at `($78,$98)`. The pre-pickup actual-ROM
frame hashes to `1204060643028526704`; the acquired state removes the Old Man
and sword while retaining both fires, hashing to `863818059061532955`.

The initial cave text is not an A prompt. `PersonText.dat` begins at verified
PRG offset `16460`, and selector zero occupies its first 43 bytes (the next
selector begins at `+43`). `Z_01:UpdatePersonState_Textbox` starts at
`TextboxLineAddrsLo+2 = $A4`, writes a low-six-bit character tile immediately,
and the global `Z_07` object-timer pass decrements its `$06` delay before the
next person update. Six `$25` special spaces consume a character cell without
a delay, leaving 37 timed transfers. The actual record's `$98` after the first
row selects `$C4`; its final `$EC` selects `$A4` through the `$C0` path and
calls `UnhaltLink` in that same final-glyph frame. `$E4` is the generic third
row but is unused by this particular record. The game-owned renderer consumes
those source tile codes from common background CHR and displays the two
decoded rows at crop-space Y=32 and Y=40; characters remain visible after
unhalt just as the source nametable does. `qa-artifacts/cave-dialogue/` holds
actual-ROM before, in-progress, complete, and post-sword PNG captures.

`Zelda1OverworldSession::try_take_start_sword_at` preserves the source pickup
gate (X exactly `$78`; `abs(Y-$98) < 6`) while cave movement itself remains an
adapter omission. It acquires only `CrossWorldItemId{Zelda1, $01}`, with
`Capability::Sword`, `ItemUseKind::Equip`, and
`ResourcePoolProvenance::None`. Thus a loadout resolver can use it in either
world without conflating it with a Native sword or inventing a Zelda resource
pool. Other cave slots are deliberately unsupported by this bounded renderer.

## Static overworld geometry

`zelda1_ow_renderer.h` exposes the renderer's exact static result as a 32 x 22
`OwFinalTileMap`: `LayoutRoomOW` column expansion, source `CheckTileObject`
primary-square substitution, then source `WriteSquareOW`. The pixel renderer
and `zelda1_ow_geometry.h` both consume this one plane, so geometry cannot
silently use a differently oriented descriptor expansion. The visible
240 x 160 geometry maps pixel `(x,y)` to source final tile `(x/8+1,y/8+1)`,
the same centered terrain crop used by the framebuffer.

The only emitted classes are `kWalkable` and `kSolid`; they are collision
facts, not visual terrain labels. `Z_05.asm:ObjectRoomBoundsOW` gives OW a
first-unwalkable final tile of `$89`. `Z_07.asm:GetCollidableTile` first maps
the exact exception list `$8D,$91,$9C,$AC,$AD,$CC,$D2,$D5,$DF` to `$26`, then
uses that threshold. Water, entrances, and secret tiles are therefore not
invented as geometry classes. The actual-ROM `$77` test checks the tree mass,
clearing, visible edge openings, crop/final-tile alignment, and the source
room-ID 2x2 lattice `$66/$67/$76/$77`.

This is the initial static state only. Source code can mutate the playfield
afterward (revealed secrets, bombs, ladder, and the pond cycle's temporary
`$99` threshold); those runtime changes remain explicit unsupported behavior.
`CheckScreenEdge` also requires Link at its source coordinates (`$3D,$DD,$00,
$F0`) before a lattice transition. A future Minish collision adapter must keep
that coordinate/actor policy separate from this source-derived tile plane.

`Zelda1LiveFrameLoader` is the game-owned activation bridge for that same
renderer. Its caller supplies an already whole-ROM-hash-validated iNES path;
the loader independently rejects anything other than the exact PRG0 header
shape (16-byte `NES` header, eight PRG banks, CHR RAM, MMC1/battery, horizontal
mirroring, no trainer) and owns only the extracted 128 KiB PRG. It derives the
OW background transfer block from those owned PRG bytes, never from an importer
cache. `load_and_render_hash_validated_ines(path, 0x77, &error)` is
failure-atomic: bad input or an unrenderable room leaves the prior data/frame
pair unchanged; `framebuffer()` then exposes the stable 240x160 BGR555 array.

## Overworld session host policy

`Zelda1OverworldSession` is a reusable pure layer around the live loader and
shared static geometry. It owns the active OW room plus signed source
`ObjX/ObjY`; its presentation position is the fixed crop inverse
`(ObjX-8, ObjY-72)`. World flags, inventory, and actor records remain owned by
their respective host controllers. Its fixed 64-byte `Z1OS` v8 record restores
room/frame/geometry failure-atomically from the already loaded,
caller-identity-validated PRG.

That crop position is Link's source sprite origin, not the Minish focus
destination. `Z_01:Anim_WriteSpritePair` draws the 16x16 Zelda Link from
`ObjX/ObjY`, so the data-only Minish OBJ-focus destination uses its
bottom-center feet `(ObjX, ObjY-56)` after the usual crop: source `+$08,+$10`
followed by crop `-$08,-$48`. This presentation-only conversion applies in the
overworld, caves, and Level 1. `Z_07` collision's `ObjY+$0B` remains a distinct
inset sample point five pixels above the visual feet; it does not change
collision, warps, source coordinates, or persistence.

Input is a proposed one-pixel cardinal displacement. The session reproduces
the source grid-phase/direction cadence, uses `Z_07` directional probes at
source grid points, and crosses the `x + 16*y` lattice only at the original
`PlayerScreenEdgeBounds`; it does not choose a crop-local fallback position.

## Level 1 static room renderer

`zelda1_uw_renderer.h` renders First Quest underworld rooms into the same
240x160 BGR555 crop and canonical NES palette pipeline as OW. It follows
`Z_05:LayOutRoom`'s static tile order: `$F6` margin fill, `FillWalls`, then
`LayoutUWFloor`'s 12x7 source column expansion and `WriteSquareUW` type-1/type-2
tile rules. The UW background transfer is derived directly from verified PRG
offset 49435 (the source `$1700` block); compiled wall/square/column tables
also come from that PRG at runtime, so no Zelda bytes are stored in the repo.

The actual-ROM test pins static framebuffer hashes for Level 1 start room
`$73` (`11042205061152487424`) and boss room `$35`
(`12253611002618560876`) under GBA BGR555 packing and emits build-only PPM/PNG QA artifacts. This view
does not claim the full live room: `LayOutDoors` depends on runtime opened-door
flags, while items, Link, enemies, and Aquamentus are sprite-layer objects.
Those layers are deliberately omitted rather than represented with fabricated
pixels.

## First Quest Level 1 host session

`Zelda1Level1Session` is a pure, persistent Level 1 host state machine. It
admits an overworld room only when its actual `AttrB` entrance index names
First Quest Level 1; current First Quest data therefore enters from OW `$37`
and starts in `LevelInfo_StartRoomId`, `$73`. Its explicit return API preserves
that owning OW portal room for a future live plugin to hand back to the
overworld session.

Door movement uses the decoded E/W/S/N `DoorType` bits and the original
underworld `$xx + {1,-1,$10,-$10}` room arithmetic. Walls stay blocked;
false walls and shutters return `kSecretUnresolved` rather than silently
turning into a generic passage. Key and key2 doors atomically consume one
key and record both sides of the now-open connection. Bombable doors require
an explicit `Level1DoorTool::kBomb`. Production adapters use the tested
`try_transition(edge, InventoryCore, LoadoutId)` overload instead: it resolves
`Capability::Bomb` from an origin-qualified loadout provider, so a Native or
Zelda 1 bomb is usable across worlds without conflating their owned records or
resource pools. The raw proof token remains a narrow adapter/test boundary;
this session does not decrement a pool because only the game-specific live
item adapter can authorize and persist that consumption.

The current source-backed QA route is `$73 → $74 → $73 → $63 → $53 → $54 →
$44 → $45 → $35`. Room `$74` has a static `$19` item (a key, proven by
`ItemIdToSlot[$19] = $17` and descriptor `$11`), unlocking `$73` north;
the north door from `$54` is bombable; `$45` contributes the second static
source key for its own north key door. The actual-ROM test executes this
route, not a boss-room teleport.

`CreateRoomObjects` treats item ID `$03` as its packed no-item sentinel, and
suppresses secret action 3 and 7 room items. The session faithfully exposes
the raw source item ID and position for each static item; it increments only
the source-confirmed key `$19`, leaving cross-world inventory acquisition to
the inventory core. Room `$35` is template `$3D`; `ObjectTypeToHpPairs` gives
it six HP and `InitAquamentus` gives initial position `($B0,$80)`. Six
adapter-supplied sword damage points mark it defeated and reveal its
source `$1A` foes-for-item reward. Contact is reported but does not invent a
Minish-health conversion.

The fixed 204-byte `Z1L1` v1 record persists active/entry/current rooms,
keys, boss HP/defeat, taken room items, and opened keyed/bombed door pairs.
Restore validates magic, version, reserved fields, source entrance/room
membership, legal flags, actual non-sentinel source items, boss-reward ordering,
legal source-door openings, and reciprocal opened edges before changing state.
All mutating APIs construct a candidate session, render it, then commit it;
failed entry, key-door, item, or boss-defeat rendering leaves every byte of
the prior serialization unchanged. The return portal is deliberately narrow:
`exit_to_overworld` succeeds only from source start room `$73`, representing
its south exit rather than a host escape from arbitrary dungeon rooms.

Each rendered room begins with the source static UW framebuffer. Until the
Metasprite CHR path is decoded, a magenta outlined Aquamentus rectangle and
a gold outlined currently collectible room-item rectangle are deliberately
conspicuous QA markers, not sprite reproductions. The actual-ROM test emits
ignored build artifacts `zelda1_l1_entry.ppm`, `zelda1_l1_intermediate.ppm`,
`zelda1_l1_boss_before.ppm`, and `zelda1_l1_boss_defeated.ppm` for screenshot
review.

## Level 1 live adapter seam

`Zelda1Level1LiveAdapter` is the pure controller intended for the later
Minish plugin integration. It neither reads or writes guest memory nor moves a
guest entity. Its `try_enter_from_overworld` requires the loaded
`Zelda1OverworldSession` to be in OW `$37`, then repeats `HandleWarpOW`'s
source gate at a source-warp final tile with X low nibble zero and Y low nibble
`$D`. Only then does it create the Level 1 host session. The actual-ROM test
uses a collision-respecting BFS through the source geometry to reach that
specific hot spot; this is not a room-number-only entrance.

### Source-space overworld movement

`Zelda1OverworldSession` v7 retains signed source `ObjX/ObjY` as its
authoritative position and derives crop presentation as `(ObjX-8, ObjY-72)`.
That permits the real `PlayerScreenEdgeBounds` collar: north `$3D` (crop
`-11`), south `$DD` (149), west `$00` (-8), and east `$F0` (232). Directional
collision is the pinned `Z_07:GetCollidingTileMoving` Link probe: the Link
hotspot uses `-$08` for up/left, `+$08` down, `+$10` right, including the
source's `$10/$F0/$DD` boundary suppressions and vertical second-column tile
comparison. The host does not emulate NES fractional velocity; instead it
samples those exact probes only at documented `ObjGridOffset==0` grid points
(`ObjX%8==0`, `ObjY%8==5`). The retained OW77 start is correspondingly raw
`($80,$9D)` / crop `(120,85)`, so this policy cannot bypass source solids
from the former unaligned presentation-only start.

`Z_05:Link_ModifyDirOnGridLine` is retained as well: release leaves an
in-flight segment unchanged; opposite input walks it back to the preceding
grid point; and an early perpendicular request reverses to that point before
the requested axis can begin. This prevents the host from entering a
two-axis, collision-unsampled state. v7 serializes the signed
`ObjGridOffset` and active `ObjDir` representation; strict v5/v6 migration
creates the canonical settled (`0`/none) v7 phase.

At an exact edge, `CheckScreenEdge` uses the source room lattice without an
edge-opening search or room-specific bridge rule. Mode 6/7 lands at the
opposite raw edge while preserving the perpendicular coordinate. The v7
record remains 64 bytes, migrates strict v5 crop coordinates via `(+8,+72)`,
and preserves an initialized OW66 actor record across non-OW66 overworld
screens so a defeated actor does not respawn on return. The actual-ROM route
validator explores and replays only these source grid/edge states, then emits
a compressed D-pad trace on success; it does not treat a room-number chain as
proof that a visible terrain route exists.

The live-frame test retains screenshot-review overlays
`build/zelda1_ow77_collision_probes.ppm` and
`build/zelda1_ow76_collision_probes.ppm`. Green crosses mark PRG-derived
Z_07 walkable samples and red crosses mark PRG-derived blockers; the top-left
legend reserves blue/magenta for a source/session disagreement. The test
exhaustively compares each expected source probe with the actual session
response, then drives ordinary held-Right input from each real loaded start
to a visible tree wall: OW77 stops at crop `(200,85)` after 80 pixels and
OW76 at `(136,85)` after 16. This collision audit does not alter PPU
presentation, focus, or scaling.

The adapter publishes only a stable framebuffer pointer, room ID, and a
host presentation coordinate. Its source sprite origin uses source `$80`
through the 8-pixel crop and LevelInfo's source `StartY` through the same
72-pixel status-bar crop used by OW/cave positions; publication converts that
16x16 sprite origin to bottom-center feet before using the Minish focus ABI.
Until UW collision is decoded,
inside-room movement is expressly host policy: one-pixel bounded movement with
X then Y ordering. A room-edge transition is offered only when that
presentation point reaches the crop edge. It delegates the exact E/W/S/N door
semantics to `Zelda1Level1Session`; `$73` south is the sole return to OW `$37`.

For bomb doors, the controller uses the `InventoryCore` capability overload,
not a raw gameplay flag. The actual-ROM path proves a Native-origin Bomb in a
selected loadout opens the `$54` north source door while keeping its record
origin separate. Static `$19` room keys are acquired as
`CrossWorldItemId{Zelda1,$19}` and increment only the Zelda1 resource-key
pool; the `$1A` boss reward likewise remains Zelda1-origin. Collection clones
both the inventory and the host session, renders/collects both candidates, and
commits them together only on success. The temporary host touch policy accepts
only an 8-pixel square around the source item feet coordinate
`(item_x-8,item_y-72)`; a distant room-wide collection call is rejected. Key
door transitions stage the child and inventory candidates together and reduce
the Zelda1 resource pool exactly when the source child consumes a key. Native
key resources and selected loadout slots remain unchanged.

`observe_sword` accepts the existing source-observed Minish sword sample and
its rising edge. It resolves an equipped sword through `InventoryCore`, uses
the same bounded host hitbox policy as the OW Octorok seam against
Aquamentus's source body at `($B0,$80)`, and supplies one damage point per
edge to the six-HP host session. It never teleports Link or changes Minish
readiness. A source start-room south return leaves the completed/in-progress
child resident but inactive; re-entering the same exact OW37 hotspot resumes
its flags, doors and keys. Explicit plugin lifecycle reset discards that
resident child. `Z1LA` v1 additionally persists resident/player/sword-edge
state plus the strict `Z1L1` blob, staging Level 1 restore before exposing it.
