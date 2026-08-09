# Zelda 1 OW77 start-cave control

`Zelda1StartCaveControl` is intentionally separate from the live Minish
portal/session. It is a source-coordinate, pure control component for the
single normal cave entered from OW77; it neither reads host state nor mutates
inventory.

Its immutable geometry comes from `Z_05.asm:RoomLayoutOWCave0` (headerless PRG
`[89000,89016)`) expanded through `ColumnDirectoryOW` (`[105743,105775)`),
`PrimarySquaresOW` (`[92540,92596)`), and `SecondarySquaresOW`
(`[92596,92660)`). Collision follows `Z_07.asm:GetCollidingTileMoving` and
`Walker_CheckTileCollision`: `$89` is the normal first solid final tile, with
the source exceptional walkables `$8d,$91,$9c,$ac,$ad,$cc,$d2,$d5,$df`.

Entry begins at `($70,$dd)` facing up and source-walks `$30` pixels to
`($70,$ad)` (`Z_01.asm:InitModeB_EnterCave_Bank5`,
`Z_05.asm:InitMode_WalkCave`). The component represents that animation as an
explicit atomic `settle_entry()` operation. `acknowledge_dialogue()` is also an
explicit host policy seam for the source textbox; Zelda halts Link until
`UpdatePersonState_Textbox` completes.

`Z_01.asm:CheckPersonBlocking` rejects upward movement while the pre-move Y is
below `$8e`; this is a global cave barrier, not an Old Man collision rectangle.
Leaving is the source's `CheckCaveEdge -> CheckScreenEdge` condition: downward
input at exact Y `$dd`. It has no X condition, while the final-tile geometry
confines reachable positions to the center mouth.

The wooden-sword predicate is exact: `Z_01.asm:UpdateCavePersonState_TalkOrShopOrDoorCharge`
requires `ObjX==$78` and `abs(ObjY-$98)<$06`, i.e. `($78,$93..$9d)`. It is
backed by OW77 cave item bytes `[99840,99843) = {$3f,$01,$7f}`. The component
only records the completed cave fact; inventory acquisition remains the owning
session's responsibility.

`get_first_quest_start_cave_old_man_sprite()` exposes the non-opaque static
sprite proof: object `$6a`, mirrored tiles `$98/$98` at `($78,$80)/($80,$80)`,
attributes `$02/$42`, overworld-sprite pattern span `[53755,53771)`, and sprite
palette row 2 at `[103195,103199)`. These facts derive from
`Z_01.asm:InitCave`, `DrawCavePerson`, `ObjAnimations`, `ObjAnimFrameHeap`,
and `ObjAnimAttrHeap`, plus `Z_03.asm:PatternBlockOWSP`.

The static cave renderer composes that mirrored Old Man directly from those
source tiles. `InitCave` calls `SetUpCommonCaveObjects` before the initial
textbox completes, so `intro_complete` deliberately does not hide either the
Old Man or fires. Once the sword's persistent cave-item flag is set, the
source removes object type `$6a` but leaves both fires. `UpdateFire` proves
the initial non-flipped fire pair at `($48,$80)` and `($a8,$80)`: tiles
`$5c/$5e`, attribute `$02`, using the same palette row 2. Their raw common
sprite-pattern spans are `[34367,34383)` and `[34399,34415)`. Its advancing
animation timing is not modeled; the renderer intentionally draws this one
verified static frame. The fire provenance is
`Z_01.asm:SetUpCommonCaveObjects`, `Z_07.asm:UpdateFire`, and the `$40 -> $41
-> $08` `ObjAnimations`/`ObjAnimFrameHeap` descriptor chain. Actual-ROM
cave-control tests emit
`build/zelda1_start_cave_{before_dialogue,dialogue_complete,sword_taken}.ppm`
and assert the first two images are equal, the fires survive sword pickup, and
the Old Man region changes after it.
