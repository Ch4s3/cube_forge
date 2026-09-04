# Finite water — conserved volume above sea level — design

The water simulation is pull-based and derived: a source (id 4) is a permanent
level-8 emitter that `process` never re-evaluates, and every flow cell (ids
5-11, levels 7..1) recomputes its level from its neighbours each tick and holds
no volume of its own. That is the right model for the sea. It is the wrong
model for a bucket: the biome canal test placed nine water blocks on a plain
and, because every placed block is id 4, flooded the eastern half of the map
to the lake (`docs/biome-map-canal.png`, `RESULTS.md`).

This makes water above sea level **conserve volume**. A placed block is a
finite quantity that spreads into a puddle and stops. The sea stays infinite,
dug pits still fill to lake level, and a spring is already representable.

## 1. What changes and what does not

| | today | after |
|---|---|---|
| id 4 | infinite source, never processed | **unchanged**: an infinite source, topped back up to 8 whenever it gives |
| ids 5-11 | derived levels 7..1, recomputed from neighbours | **volume**: levels 7..1 in units, conserved, moved by push |
| placing water | places id 4 | places **7 units** (id 5), one line at the `place_water` call site |
| lakes at or below sea level | `source_neighbour` promotes an adjacent cell to a source | **unchanged** |
| a spring above sea level | not expressible without flooding | "allow placing id 4" — nothing to build |

Encoding is untouched: `level_of(id) = 12 - id`, `id_of_level(l) = 12 - l`,
air is 0. The actor, the dirty queue, the edge mirrors, the per-tick budget and
the packed reply format all stay. Only the rule inside `process` changes, plus
one new message payload.

## 2. The rule: push, not pull

`process(x, y, z)` for a **volume** cell with `L` units (1..7):

1. **Fall first.** If the cell below is air, or water with level `w < 7`, move
   `min(L, 7 - w)` units down (all of it into air). If the cell below is a
   source, the units are absorbed — the sea is an infinite sink as well as an
   infinite spring. Mark below and self dirty.
2. **Then spread.** For each of the four horizontal neighbours in fixed order
   (`+x, -x, +z, -z`), if it is air or water with level `n < L - 1`, move
   `(L - n) / 2` units (integer division) into it and continue with the reduced
   `L`. A difference of 1 never moves, so a level pattern cannot oscillate.
   Mark every neighbour that received units, and self.
3. A neighbour that is a solid block, or water at `>= L - 1`, takes nothing.

For a **source** cell (id 4): give as if `L = 8` by the same two steps, but
never decrement — it is written back as id 4. This is the only place the sea's
infinity lives, and it is exactly the spring.

`source_neighbour` runs before either, unchanged: at or below sea level, a
cell touching a source becomes one.

**Termination.** Every horizontal move strictly reduces the giver and needs a
difference of at least 2; every fall moves units down. With a fixed volume and
a bounded world, the number of possible moves is finite. Seven units on flat
ground settle as 7 cells of level 1 — a puddle — in a handful of ticks.

## 3. Crossing a chunk edge

Push needs to write into a neighbour chunk, and an actor cannot touch another
actor's chunk. Today's spills are pull-based through the edge mirrors, which
can tell a cell its neighbour's level but cannot carry volume across.

`kind 2` in the packed reply format is reserved for spills and is unused. It
becomes the volume carrier:

- The giver decrements its own cell by `u` units and emits
  `kind_spill + dir * 2^20 + slot * 256 + u`.
- The frame loop forwards it as `WSpill(dir, slot, u)` to the neighbour, whose
  `accept_spill` **adds** `u` to its edge cell (creating id `12 - u` in air) and
  marks it dirty. `_level` in the current signature was informational; it is now
  the payload.
- The giver decides `u` from the neighbour's **mirrored** level, which may be a
  tick stale. If the sum would exceed 7, the receiver keeps 7 and bounces the
  remainder back with a spill in the opposite direction. Bounces terminate: a
  giver only ever pushes to a strictly lower cell, so a bounced unit lands in a
  cell that is at least as full as the one it left, and the difference-of-two
  rule stops it going back out.

Conservation is therefore exact: units removed from one chunk equal units
added to another, in the same tick's reply, with overflow returned rather than
dropped.

## 4. Verification

The invariant is the test. `Water.volume(sim)` sums `level_of` over every
non-source water cell in a chunk.

- **Closed basin.** Walls, floor, a column of seven placed blocks. After any
  number of ticks, `volume` is exactly `7 * 7`, and no cell exceeds 7.
- **Puddle extent.** One block placed on a flat plain settles to exactly seven
  cells of level 1 and then stops changing — the dirty queue empties.
- **Fall first.** A block placed over a pit ends up at the bottom of the pit,
  and the cell it was placed in is air.
- **Two chunks.** Two `Sim`s with mirrored edges; place seven units at `x = 15`
  of the first. Drive both, forwarding every `kind_spill` reply to the other's
  `accept_spill`. The sum of the two volumes stays `7` throughout; some units
  end up at `x = 0` of the second.
- **The overflow bounce.** Force a stale mirror (edge shows air, neighbour cell
  is actually at 6), push 3 units, assert 1 is bounced and the total is `7`.
- **The sea is still the sea.** Below sea level, a dug pit beside a source
  fills to lake level — the existing `flow_test` cases, kept.
- **Biome canal, end to end.** `CF_AUTOCANAL=10`: the terrain map at frame 890
  shows a short line of water beside the player and nothing else; the biome
  map shows a few columns of change along it, not half the world.

`flow_test.march` currently asserts the derived model (a flow cell beside a
source reads level 7, etc.). Those cases are rewritten against the conserving
semantics rather than kept alongside it; the sea-level cases survive as they
are.

## 5. Risks

- **Water that used to be free now costs something.** Anything that relied on a
  placed block behaving as a spring — nothing in the codebase does, but the
  behaviour was visible — changes. Springs are one `place id 4` away.
- **Stale mirrors.** The bounce handles the overfill case; the underfill case
  (mirror says 6, cell is actually air) just pushes less than it could this
  tick and catches up next tick. Neither loses a unit.
- **Budget.** `max_cells_per_tick` is 250 per chunk. Pushing marks up to five
  cells dirty per processed cell; a big reservoir settles over several ticks
  rather than one. That is the intended behaviour, not a regression.
