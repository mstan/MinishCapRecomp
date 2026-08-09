# Minish Cap foreign-world host map (USA)

This map pins the first Zelda 1 world pack to the USA Minish Cap ROM and to
named source revisions. Addresses are ROM-aligned Thumb entry addresses from
the imported USA symbols, checked against the local `zeldaret/tmc` source.
They are integration candidates, not permission to call guest functions from
host C++: the function-entry hook observes or replaces an arriving guest call
and is not a host-to-guest call bridge.

## Pinned inputs

| Input | Identity |
| --- | --- |
| Minish Cap USA ROM | SHA-1 `b4bd50e4131b027c334547b4524e2dbbd4227130` |
| `zeldaret/tmc` | `5ab63f00e522ff69c5bc987e379f0163943f2833` |
| Zelda 1 USA PRG0 ROM | SHA-1 `dab79c84934f9aa5db4e7dad390e5d0c12443fa2` |
| Zelda 1 PRG payload | SHA-1 `a12d74c73a0481599a5d832361d168f4737bbcf6` |
| `aldonunez/zelda1-disassembly` | `50a1c869a8d8e2eb8b5b60acea325f44b4341762` |

The Ghidra registry program is `gba/gameboyadvance:minishcap_usa.gba`. If it is
locked by another writer, do not force it or kill that process; the ROM-aligned
symbols and pinned decomp remain the source of the entries below.

## Guest routines

All entries use normal AAPCS (R0-R3 arguments, R0 result). Guest pointers are
GBA addresses and must be read through emulated memory, never dereferenced as
host pointers.

| System | Address | Routine / role |
| --- | ---: | --- |
| Rooms | `0804B01C` | `LoadRoom()` |
| | `0804ADDC` | `LoadRoomEntityList(const EntityData*)` |
| | `0804ADF8` | `LoadRoomEntity(const EntityData*) -> Entity*` |
| | `0804B0C0` | `SetCurrentRoomPropertyList(area, room)` |
| | `0804B0FC` | `GetRoomProperty(area, room, property)` |
| | `0804B128` | `GetCurrentRoomProperty(property)` |
| Maps/collision | `080197D4` | `LoadMapData()` |
| | `0807BFD0` | `LoadRoomTileSet()` |
| | `0807C0DC` | `LoadRoomGfx()` |
| | `0807BC84` | `CreateCollisionDataBorderAroundRoom()` |
| | `08019840` | `UpdatePlayerCollision()` |
| Camera/flow | `0807C740` | `InitializeCamera()` |
| | `08052CFC` | `InitRoom()` |
| | `08052A94` | `InitRoomTransition()` |
| | `08049DCC` | `UpdateCurrentRoom(RoomMemory*)` |
| Exits | `08080840` | `DoExitTransition(const Transition*)` |
| | `0808091C` | `DoExitTransitionWithType(const Transition*, u32)` |
| Player | `080524A8` | `InitializePlayer()` |
| | `08016F28` | `PlayerUpdate(PlayerEntity*)` |
| | `08078EE4` | `ResetPlayerPosition()` |
| | `08079458` | `RespawnPlayer()` |
| | `0807B0FC` | `PlayerWarp(PlayerEntity*)` |
| | `08078A90` | `SetPlayerControl(PlayerControlMode)` |
| Entities | `0805E678` | `GetEmptyEntity() -> Entity*` |
| | `0804AA60` | `CreateEnemy(id, type) -> Entity*` |
| | `0806ED50` | `CreateNPC(id, type, type2) -> Entity*` |
| | `0805E5C0` | `UpdateEntities()` |
| | `080011C4` | `EnemyUpdate()` |
| | `0800129E` | `EnemyFunctionHandler()` |
| | `0800404C` | `DrawEntity(Entity*)` |
| | `080175F4` | `CollisionMain()` |
| Items/menu | `080705AC` | `CheckInitPauseMenu()` |
| | `080A4D88` | `InitPauseMenu()` |
| | `080A4EA0` | `Subtask_PauseMenu()` |
| | `080A5218` | `PauseMenu_ItemMenu()` |
| | `0807CA84` | `GetInventoryValue(item)` |
| | `0807CAA0` | `SetInventoryValue(item, value)` |
| | `08053FF0` | `GiveItem(item, param)` |
| | `08054398` | `PutItemOnSlot(item)` |
| | `08054414` | `ForceEquipItem(item, slot)` (0=A, 1=B) |
| | `08077698` | `UpdateActiveItems(PlayerEntity*)` |
| Save | `0807CA18` | `FinalizeSave() -> u32` |
| | `0807CDA4` | `HandleSave(action) -> SaveResult` |
| | `0807CF08` | `WriteSaveFile(index, SaveFile*)` |
| | `0807CF28` | `ReadSaveFile(index, SaveFile*)` |
| Audio | `0805283C` | `LoadRoomBgm()` |
| | `080A3204` | `InitSound()` |
| | `080A3248` | `SetBgmVolume(u32)` |
| | `080A3268` | `SoundReq(u32)` |
| | `080A3480` | `AudioMain()` |

## Live state required by the adapter

- `gRoomControls` at `03000BF0`: area `+4`, room `+5`, room origin `+6/+8`,
  scroll `+A/+C`, pixel width/height `+1E/+20`, camera target `+30`.
- `gRoomTransition` at `030010A0`: transitioning-out `+8`, transition type
  `+9`; embedded target area/room/spawn `+C/+D/+F`, target x/y/layer
  `+10/+12/+14`.
- `gPlayerEntity` at `03001160`: a `PlayerEntity` is `0x88` bytes. Entity
  world x/y/z are signed Q16.16 at `+2C/+30/+34`.
- `gEntities` at `030015A0`: 72 entries with stride `0x88`. Base `Entity` is
  `0x68`; kind/id/action are `+8/+9/+C`, collision layer/flags `+38/+3C`,
  hitbox `+48`, parent/child `+50/+54`.
- `gPauseMenuOptions` at `02034490` is `0x18` bytes.
- `gSave` at `02002A40`: `SaveFile` is `0x4B4`; its native packed inventory
  starts at `+F2` and is 34 bytes.
- Map memory supports 128×128 8-pixel tiles (top `0200B650`, bottom
  `02025EB0`) and therefore comfortably supports a 256×176 foreign room.

## Safe first hook allowlist

Start with exact PC equality and only the entries actually enabled by the mod:

1. `0805E5C0 UpdateEntities`: once-per-update portal/world state machine.
2. `0807CDA4 HandleSave`: observe save action in R0 and attach sidecar state.
3. `080524A8 InitializePlayer`: optional new-game/load session reset.
4. `08080840 DoExitTransition`: optional observation, never manufacture a
   guessed `Transition` record.
5. `080A4EA0 Subtask_PauseMenu`: optional only if guest pause UI is extended.
6. `080A3268 SoundReq`: optional sound observation/replacement.

Do not hook the high-frequency player, enemy, draw, collision, inventory-set,
or room-graphics routines for the MVP. Do not allocate a raw guest `Entity`
for the portal: allocation, linked lists, scripts, animation, collision, and
reclamation are coupled. The first portal is host-owned presentation plus a
guest-memory proximity check.

## Presentation and persistence constraints

The outdoor starting field (area 3, room 1,
`HyruleField_SouthHyruleField`) is 1008×688 pixels and is the early portal
host. The indoor Smith/Link house (area `0x22`, room `0x11`) is approximately
240×160 and is not suitable for a 256×176 portal presentation.

Map and camera state can represent a 256×176 Zelda room. The current extended
view is horizontal-only, however, and exposes only native Y=0..159. Showing
all 176 rows requires a separate vertical presentation change; until then the
world adapter must pan/crop vertically.

Foreign state belongs in a versioned runner/plugin sidecar keyed to the active
Minish save slot. Do not consume unknown `SaveFile` padding: native sectors are
duplicated, checksummed, and written asynchronously.

