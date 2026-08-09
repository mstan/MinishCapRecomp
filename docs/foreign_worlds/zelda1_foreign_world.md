# Zelda 1 foreign world: approved first-slice specification

## Scope and boundary

This document defines the durable, game-owned foreign-world seam for
MinishCapRecomp. It is intentionally independent of the GBA runtime, guest
memory, renderer, audio system, ROM contents, and Minish Cap symbols. No live
Minish Cap hook, Zelda 1 ROM loader, or guest address is part of this slice.
Future adapters translate their game-specific state at this seam; they do not
push foreign-world behavior into `gbarecomp`.

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

This fragment is not yet attached to a Minish Cap save. Save placement,
migration, a Zelda 1 loader, and concrete guest-memory adapters are explicitly
deferred until their validated game boundaries are known.
