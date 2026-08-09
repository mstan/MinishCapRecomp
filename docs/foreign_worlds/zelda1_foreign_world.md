# Zelda 1 foreign world: implemented QA-slice contracts

## Scope and boundary

The first sections define the durable, game-owned foreign-world seam for
MinishCapRecomp. That core remains independent of the GBA runtime, guest
memory, renderer, audio system, ROM contents, and Minish Cap symbols. The later
sections record the live Zelda 1 loader, source-coordinate session, trusted
Minish adapters, and current QA limitations built on top of the seam. Game
specific behavior remains game-owned; `gbarecomp` only supplies generic trusted
hook, state, audio, and data-only presentation facilities.

`WorldId` is persisted as an 8-bit stable ID:

| ID | Meaning |
| --- | --- |
| `Native` (0) | Minish Cap's normal world |
| `Test` (1) | ROM-free synthetic test world |
| `Zelda1` (2) | The future Zelda 1 guest world |

Changing an assigned value requires a save-format version bump and migration.

## World lifecycle and location

`WorldPosition` is `{ room, x, y }`; the values are opaque to the registry and
are interpreted only by that world's adapter. A `ReturnAnchor` contains a
world ID and `WorldPosition`. The registry owns the current world, its entry
position, and at most one anchor. It calls `leave(anchor)` on the old world,
then `enter(position)` on the destination. Returning consumes the anchor.
If a destination `enter` fails after the prior world's `leave`, the registry
compensates by re-entering the prior lifecycle at its saved current position.
The registry state and return anchor remain unchanged. If that rollback also
fails, the error reports both failures; an adapter may then surface that
terminal lifecycle failure rather than silently continuing with stale state.

`WorldLifecycle` contains only `enter`, `leave`, and `tick`. A world adapter
publishes movement with `set_current_position`; the registry never inspects
guest memory to infer it. This makes the seam unit-testable and prevents the
shared engine from acquiring Minish Cap or Zelda-specific types.

The proof world establishes the deterministic contract without ROM assets:

| State | Allowed input | Destination |
| --- | --- | --- |
| room A `{1,2,3}` | east door | room B `{2,40,3}` |
| room B `{2,40,3}` | west door | room A `{1,2,3}` |

Every other door is rejected. It is registered as `WorldId::Test` and proves
both an enter/return-anchor cycle and deterministic intra-world transitions.

## Dual-origin inventory

An item identity is always `CrossWorldItemId { origin: WorldId, item: u32 }`.
There is deliberately no item-number-only ownership or capability query:
`Native/7` and `Zelda1/7` are distinct keys even when both confer `Sword`.

Each key has distinct `OwnershipFlags` (`Owned`, `Equipped`, `Quest`), a
capability bitset, and stable `ItemTraits`: a numeric tier ordinal (`0` through
`65534`, not a category), an explicit `CrossWorldItemId` behavior key, a use
kind (`Passive`, `Equip`, `Consume`, `Quest`), and resource-pool provenance
(`None`, `Native`, or `Zelda1`). A behavior key contains its own origin, so two
same-origin equip items need not share behavior just because their use kind is
the same. Neither behavior keys nor tiers are inferred from the owned item;
they are persisted data identities and never guest addresses. `None` records
that an item spends no resource pool. Query accessors return traits and
capabilities by the exact cross-world key. Two independent four-slot loadouts,
A and B, store full cross-world identities and may select items from either
origin. A loadout can reference only an owned exact key. Capability queries
are made against a chosen loadout and return the full provider IDs, preserving
provenance rather than merging same-number items. Public loadout APIs reject
values other than A or B.

The core also preserves two separate resource bundles: `native_resources` and
`zelda1_resources`. Each bundle includes health/max health, rupees, bombs,
arrows, and keys. They are separate state and are never summed, transferred,
or exposed as a generic combined pool.

Runtime acquisition uses `InventoryCore::acquire_item`, which is idempotent
only for the exact `{origin,item}` key. Reacquiring `Native/42` can add its
own ownership flags but cannot alter that record's traits or overwrite
`Zelda1/42`. `resolve_loadout_use` returns the acquired exact key, explicit
behavior key, use kind, and resource-pool provenance; it intentionally has no
active-world parameter. Thus either origin can be selected in the same
loadout and used while either world is active. The adapter obtains a pool only
through `resource_pool_for(Native|Zelda1)`; `None` returns no pool and the API
never provides a combined fallback. This is the only game-owned cross-world
use decision surface; guest inventory and foreign renderer/session code must
translate through it rather than compare numeric item IDs.

## Save fragment format

`InventoryCore` serializes a self-contained little-endian fragment:

```
FWIV | u16 version (=3) | u16 item_count |
item_count * (u8 origin | u32 item | u8 ownership | u32 capabilities |
              u16 tier | u8 behavior_origin | u32 behavior_item |
              u8 use_kind | u8 resource_pool) |
2 * (u8 loadout_id | 4 * (u8 present | [u8 origin | u32 item])) |
native ZeldaResourcePools | Zelda1 ZeldaResourcePools
```

Each resource bundle is six `u16` values in the member order above. The
decoder accepts only the exact magic and version, known world IDs, capability
bits, ownership bits, tier ordinals through `65534`, all use-kind and
resource-pool values, the two in-order loadout IDs, no duplicate item keys,
loadout entries that reference owned items, a bounded item count (1024), and
exact input exhaustion. It rejects malformed, truncated, future-version, and
trailing-byte data without changing the output object.

`ForeignWorldNativeState` is the game-owned trusted-native provider attached to
engine savestates only when the committed Zelda feature activates. Keeping the
provider catalog empty while the feature is disabled preserves legacy Minish
savestate compatibility; enabling it after the engine has frozen a snapshot
catalog requires a process restart. Its current `FWNS` v3 payload contains the
exact `FWIV` v3 fragment, the optional fixed 340-byte `Z1WM` v2 proof record,
the optional fixed 64-byte live `Z1OS` v5/v6/v7/v8/v9 overworld/cave/OW66 record, and a
strict presentation-active byte. `FWNS` v1/v2 inputs migrate as inactive. The
provider validates into temporary state before restore; its nonserialized
generation counter lets the trusted plugin deterministically rebuild or clear
the PPU presentation on the next hook without guest writes.

Level 1 child state is not yet attached to `FWNS`, so loading a snapshot resets
an active dungeon adapter instead of carrying a newer host dungeon across the
restore boundary. That limitation is explicit until the child record joins a
new provider schema.

This does not yet attach to a Minish Cap cartridge save. Durable sidecar writes
remain deliberately outside the `HandleSave` hook until the pinned TMC source
proves the save-slot index and a successful write completion boundary. No guest
save padding, guessed save action, or `state.toml` fallback is permitted.

## First combat bridge (OW $66)

The first live combat slice is deliberately host-owned and read-only with
respect to Minish Cap. `UpdateEntities` samples the source-defined active
`gActiveItems[0]` fields (`behaviorId`, `priority`, and the facing snapshot
written by `sub_08077D38`) plus `PlayerState.sword_state` and
`attack_status`. IDs 1 through 6 are the exact TMC `ItemSword` definitions.
A nonzero live sword state has one rising edge per swing/charge sequence; a
held value cannot repeatedly damage a Zelda actor. The active behavior retains
normal Minish animation and item execution.

The adapter resolves a sword through a selected `InventoryCore` loadout first.
It returns the full acquired and behavior IDs, and checks the exact acquired
record's `Sword` capability; it does not compare a raw Native or Zelda item
number. Therefore an owned Native sword and the Zelda1 wooden sword are both
valid providers when explicitly placed in the selected loadout. On a proven
active native `ItemSword` edge only, the bridge idempotently mirrors exact
`Native/{behaviorId}` ownership (tier `1..6`, same native behavior key,
`Equip`, no resource pool) and selects its first empty slot only when no
compatible sword is already selected. A selected Zelda1 sword stays selected,
while the independent observed Native record is still acquired. This observes
an already-equipped guest sword without setting guest inventory/story flags or
overwriting a selected Zelda1 sword. It deliberately does not attempt a
broader live A/B equipment mapping until that mapping has source support.

The start-cave `Zelda1/01` pickup stages both the exact acquisition and A
loadout selection before it commits source cave state: if no compatible sword
is selected, it occupies the first empty A slot; an already selected compatible
Native or Zelda sword is preserved. In host-owned OW `$66` and Level 1 boss
combat, a read-only active-low A rising edge may use that selected record even
with no active TMC `ItemSword`.
For that host-only request the adapter reads `gPlayerEntity.animationState`
(`Entity + $14`) for its documented cardinal facing. An active source
`ItemSword` suppresses the simultaneous host request, so one physical swing
cannot double-hit. This does **not** inject Zelda-origin sword behavior into
normal Minish Cap gameplay; native-world Zelda-item behavior mapping remains
deferred until a source-proven guest equipment boundary exists.

For room `$66`, a source-coordinate, 17x24 forward hit rectangle is explicit
host policy (not a claim that the two games share hitbox data). It uses the
source animation-state snapshot (`north/east/south/west = 0/2/4/6`) and applies
one Zelda damage through the host session. Octoroks tick once per TMC
`gRoomTransition.frameCount`; the duplicate ROM/IWRAM observation addresses
are de-duplicated by that source frame counter. Contact is recorded in a
host-only six-point QA health counter with a 30-frame host cooldown. It never
writes Minish health. A defeated Octorok's currently-unclassified drop marker
is collected by source-coordinate proximity and recorded as a QA event, but
does not invent a rupee/item reward before Zelda RNG/drop-table behavior is
implemented.
