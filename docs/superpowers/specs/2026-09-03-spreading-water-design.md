# Spreading water — design

Sub-project 2 of 2 for water; builds on static water
(`2026-09-03-static-water-design.md`). Goal: Minecraft-style finite spread
with levels, simulated per chunk by actors, with cross-chunk flow via
messages. Dogfooding target: March's actor model.

## 1. Storage and flow rules

- Block ids 4–11 are water. 4 is a **source** (level 8, never recomputed;
  placed by the player or generated at sea level). 5–11 are **flow** cells,
  level = 12 − id (7..1). `Chunk.is_water(id) = 4 <= id <= 11`; physics,
  raycast and the mesher use it. `Chunk.level_of(id)`, `Chunk.id_of_level(l)`.
- Generated lakes are sources, so the initial world is stable.
- Recompute (flow cells only, when dirty): level = max(7 if the cell above
  is water, highest horizontal water neighbour level − 1). Below 1 → air.
- Spread from any water cell of level L: if the cell below is air it becomes
  level 7 and nothing spreads sideways; else each horizontal air neighbour
  becomes level L − 1 if L − 1 ≥ 1. Spreading only overwrites air or a flow
  cell with a lower level.
- Any cell change marks its 6 neighbours dirty. Terrain edits mark the edited
  cell and its 6 neighbours dirty.
- Dirty tracking per chunk: a queue of packed indices (`NativeIntArr` ring,
  capacity 65536) plus a 65536-byte `NativeU8Arr` bitmap for dedupe. At most
  250 cells are processed per chunk per tick; the rest stay queued.

## 2. Actors and frame integration

- `actor WaterChunk`, one per chunk, spawned after generation. State:
  chunk, queue, bitmap, cx, cz.
- Messages:
  - `Tick(n, s, e, w, reply_to)`: neighbour chunks passed by value (shared
    refs). Step against own chunk + snapshots. Reply `Changed(chunk',
    spills)` or `Unchanged`. `spills` is a list of `Spill(dir, lx, y, lz,
    level)` for flow crossing an edge (dir 0 = -z, 1 = +z, 2 = +x, 3 = -x).
  - `Spill(lx, y, lz, level)`: write the cell if air or a lower flow level
    (level 0 = "just mark dirty"); mark dirty.
  - `Edited(lx, y, lz, id)`: set the cell, mark it and its neighbours dirty;
    cross-edge neighbours are returned to the loop as level-0 spills via the
    next Tick reply (the loop also marks the neighbour chunk dirty itself).
- Frame loop: every 10 frames, for chunks flagged dirty in a 64-entry
  `NativeIntArr`, `Actor.call(Tick)` inside `List.pmap_n`; then serially:
  swap changed chunks into the world, route spills to neighbour actors and
  flag them, remesh changed chunks. Edge flow is one tick late (accepted).
- The world in `Scene` stays the rendering/physics truth; player edits go to
  the world first (immediate remesh) and to the actor as `Edited`.
- `needs IO.Spawn` where actors are spawned or called.

## 3. Meshing with levels

- Surface height = `level * 0.875 / 8`.
- Top face when the cell above is not water, at that height. Side faces
  against air at the cell's height; against a lower water neighbour, the
  strip between the two heights; against equal/higher water, culled. Bottom
  face against air only.

## 4. Verification

- Pure `step` unit tests: rings 7..1 from a source on a flat floor; draining
  after source removal within 8 steps; falling water does not spread on the
  rim; the 250 cap leaves work queued.
- Actor test: spawn one `WaterChunk`, `Edited` a source, `Tick` via
  `Actor.call`, assert `Changed` with the expected cells.
- Scripted `CF_AUTOFLOW=1`: place a source on a plateau, print the chunk's
  water-cell count every 10 ticks (grows then stabilises), break the source
  (count returns to the pre-edit value); a source at local x = 15 produces
  water in the +x neighbour within 2 ticks. Frame dumps before/after.
- `RESULTS.md`: tick cost for one dirty chunk, worst case 64 dirty chunks,
  remeshes per tick, per-frame allocation during flow (honestly, messages
  allocate).
- Actor-model friction → `GAPS.md`.

## Out of scope

Lava/other fluids, flow animation, sound, source creation from two sources,
infinite ocean refill.

## Amendments made during implementation

- **Actors cannot receive chunks** (native arrays are non-sendable; `Actor.call`
  takes a zero-arg sentinel). `WaterChunk` regenerates its chunk from
  `(cx, cz, seed)` on `WLoad`, mirrors neighbour edge columns, and replies with
  a packed `List(Int)`. See GAPS.md G44.
- **Pull-based rule.** A dirty cell (air included) computes the level it should
  have from its neighbours: 7 if water is above, else the best supported
  horizontal neighbour's level − 1. "Supported" = the cell below is neither
  air nor flow water. Spills are therefore just "mark that cell dirty"; the
  mirror update (kind 1) carries the level.
- **Water top under a solid block is kept** (the 1/8 gap is visible).
- `Actor.call` is unusable under `forge test` (G49): the actor is smoke-tested
  with `send`; the round-trip is verified by `CF_AUTOFLOW`.
- **Lakes are infinite (2026-09-03 evening).** At or below sea level, a
  dirty non-source cell that touches a source (beside or below one) becomes a
  source, so anything connected to a lake fills to sea level. Above sea level
  only the finite-spread rule applies. Unit tests moved their scenarios above
  sea level accordingly, and a lake+pit test pins the fill.
- **Targeting.** The raycast ignores water unless the water hotbar slot is
  selected; swimming follows the look direction, Space swims up, Shift sinks.
