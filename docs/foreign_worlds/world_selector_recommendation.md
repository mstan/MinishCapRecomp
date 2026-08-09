# Zelda 1 world access recommendation

## Chosen path

Place the first portal outdoors in area 3, room 1 (South Hyrule Field, the
Smith's-yard/opening field). It is available at the beginning, has ample room,
and avoids changing the small indoor house. The first successful use persists
an unlock; after that, a game-owned **Worlds** action provides discoverable
travel and an emergency return. The Zelda 1 starting sword cave contains the
normal return portal.

For early iterations, activation may intentionally hard-code entry into Zelda
1, but it must still use the registry and return-anchor path so the temporary
shortcut cannot create a second transition system.

## Activation safety

The host-owned portal becomes interactive only when all of these are true:

- area/room is 3/1;
- `gRoomTransition.transitioningOut` is zero;
- Link has normal player control;
- the opening scripts have reached a stable update state;
- the foreign-world mod is enabled and its verified asset cache is present.

The final collision-safe coordinate must come from a visual runtime pass; it
must not be guessed from symbols. `UpdateEntities` is the one low-frequency
guest entry used to poll the conditions. Rendering and interaction stay in the
trusted game-owned plugin; no raw guest entity is fabricated.

## Alternatives considered

| Option | Result |
| --- | --- |
| Guest pause-menu item | Familiar, but invasive before the hook/call bridge and save ownership are proven. Use the runner's game-owned Worlds action after unlock instead. |
| Early NPC dialogue | Natural fiction, but entangles opening scripts and NPC dialogue progression. It is less reusable for future world packs. |
| Launcher-only selection | Easy to implement and useful for development, but not an in-world entrance and awkward for returning. Keep only as a developer override. |

## Return behavior

- Smith-yard portal: Minish Cap → Zelda 1 starting screen, with a saved Minish
  return anchor.
- Starting sword cave portal: Zelda 1 → saved Minish anchor.
- Worlds emergency action: foreign world → saved Minish anchor.
- Saving in Zelda 1 restores Zelda room, coordinates, world flags, distinct
  inventory origins, and the Minish return anchor.
- If foreign state is missing or invalid, recover to the Minish anchor (or the
  known outdoor entry fallback) without setting Minish story flags.
