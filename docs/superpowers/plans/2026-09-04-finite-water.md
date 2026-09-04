# Finite Water Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Water above sea level conserves volume — a placed block is seven units that spread into a puddle and stop — per `docs/superpowers/specs/2026-09-04-finite-water-design.md`.

**Architecture:** `Water.process` becomes a push rule (fall first, then equalise across a difference of two); id 4 stays the infinite source and tops itself up; `accept_spill` adds the units a neighbour pushed instead of merely marking; the `place_water` call site places id 5 (seven units) instead of id 4. Actor, queue, mirrors, budget and reply format are unchanged. The `kind 2` forwarding path already exists end to end.

**Tech Stack:** March (forge).

## Global Constraints

- Encoding unchanged: `level_of(id) = 12 - id`, `id_of_level(l) = 12 - l`, air 0, source 4, volume 5..11 = levels 7..1.
- Fall first; then horizontal neighbours in fixed order `+x, -x, +z, -z`; horizontal move is `(L - n) / 2` units only when `n < L - 1`.
- A source gives as `L = 8` and is always written back as id 4. Units moving *into* a source are absorbed.
- `source_neighbour` (lake promotion at or below sea level) runs first, unchanged.
- Cross-edge push: giver decrements and emits `kind_spill() + dir * 1048576 + slot * 256 + u`; receiver adds `u` capped at 7 and bounces the remainder as a spill in `opposite(dir)`.
- Every `NativeArray`/`Chunk` write is threaded, never discarded (GAPS G69).
- Build cycle per task: `forge check && forge build && forge lint --strict && forge test`.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `lib/cube_forge/water.march` | `volume`, the push rule in `process`, `accept_spill` adding units | 1-3 |
| `test/flow_test.march` | Conservation tests; derived-model cases rewritten | 1-3 |
| `lib/cube_forge.march` | `place_water` places id 5 | 4 |
| `RESULTS.md`, `todos.md` | Canal re-run, tick entry | 4 |

---

## Task 1: `Water.volume` and the closed-basin invariant (red)

**Interfaces:** Produces `Water.volume(s : Sim) : Int` — sum of `level_of` over non-source water cells.

- [ ] Add to `test/flow_test.march`: a walled basin (stone floor at y=70, stone walls around a 4x4 interior), seven placed units (id 5) at the centre; assert `volume == 7` after 0, 1, 5, 40 ticks and that no cell exceeds level 7. Run: expect FAIL (`volume` undefined).
- [ ] Implement `volume` (scan 65536 cells, `C.get_idx`, sum `level_of` when `is_water && !is_source`). Run: the invariant test FAILS on the *tick* assertions — the derived rule drains an unsourced level-7 cell to nothing. That failure is the point of Task 2.
- [ ] Commit: `water: volume helper and the conservation test that the derived model fails`.

## Task 2: The push rule

**Files:** `lib/cube_forge/water.march` — replace `contrib`, `from_above` and the body of `process`.

- [ ] Write `give(st, sx, sy, sz, dx, dy, dz, u)`: subtract `u` from the giver (source: no-op on the id), add to the receiver (`air -> id_of_level(u)`, water `-> id_of_level(level + u)`, source -> absorbed), via `set_cell` for both, so marks and reply entries happen. If the receiver is outside the chunk (`cell` reads a mirror), instead subtract from the giver and append `kind_spill() + dir*1048576 + edge_slot(dir, ...)*256 + u` to the reply, where `dir` is the crossed edge.
- [ ] Rewrite `process`: keep the `source_neighbour` promotion; then `L = if source then 8 else level_of(id)`; **fall**: below air -> give all; below water `w < 7` -> give `min(L, 7-w)`; below source -> give all (absorbed); **spread**: for each of the four neighbours in order, `n = level of neighbour (0 for air, skip solids and sources... sources absorb only when falling into them, never sideways)`, if `n < L - 1` give `(L - n) / 2` and reduce `L`.
- [ ] Run tests: the basin invariant now PASSES. The old cases "a source on a floor spreads rings 7..1" and "removing the source drains everything" FAIL — expected; rewrite them in Task 3.
- [ ] Commit: `water: push rule, volume conserved above sea level`.

## Task 3: Edges, bounce, and the rewritten cases

- [ ] `accept_spill(s, dir, slot, u)`: read the edge cell; `room = 7 - level`; add `min(u, room)`; if `u > room`, append a bounce entry to a **pending spill list** on the Sim (new 7th slot, `List(Int)`) drained into the next tick's reply. Mark the cell.
- [ ] Tests: two `Sim`s with mirrored edges (`load_edge`), seven units at `x = 15` of A; drive both, forwarding every kind-2 entry to the other's `accept_spill` and every kind-1 entry to `edge_changed`; assert `volume(A) + volume(B) == 7` after each tick and that B ends with water at `x = 0`. Overflow: set B's `x = 0` cell to level 6 but leave A's mirror showing air; push 3; assert the sum stays 7 and a bounce entry appears.
- [ ] Rewrite the two derived-model cases: "a placed block on a floor settles as seven cells of level 1 and the queue empties" and "water falls down a shaft" (kept, re-asserted on volume). Keep the sea-level pit case unchanged.
- [ ] Commit: `water: volume crosses chunk edges; overflow bounces`.

## Task 4: Placing water, and the canal again

- [ ] `lib/cube_forge.march`: `if place_water do Chunk.water() else -1 end` -> `Chunk.id_of_level(7)`. `CF_AUTOCANAL` (`dig_canal`) likewise places `Chunk.id_of_level(7)`.
- [ ] Full cycle. Then: `MARCH_NUM_SCHEDULERS=1 CF_NOMOUSE=1 CF_AUTOMAP=0 CF_AUTOCANAL=10 CF_FRAMES=900 CF_DUMP_FRAME=890 CF_DUMP=/tmp/canal2.bmp CF_SEED=7` — the terrain map shows a short line, not a flood. And with `CF_BIOME_MAP=1 CF_BIOME_RATE=20000`, the biome map changes along the line only. Save `docs/biome-map-canal.png` from this run (replacing the flood image).
- [ ] `RESULTS.md`: replace the flood paragraph; `todos.md`: tick finite water. Commit: `water: placing water places seven units, not a spring`.

## Self-Review

Spec §1 -> Tasks 2, 4. §2 rule -> Task 2. §3 edges/bounce -> Task 3. §4 tests: basin (T1), puddle extent + fall + two chunks + bounce (T3), sea (kept), canal (T4). `volume` excludes sources everywhere it is asserted.
