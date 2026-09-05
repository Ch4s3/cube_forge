# Lakes above sea level — design

Water at generation reaches exactly `sea_level()` (62): `fill_column_mat`
fills every column below it and no column above it. A basin on a plateau, a
cirque under a ridge, or a valley dammed by a terrace rim is a dry bowl. This
fills every closed basin to its rim, so the terrain's own shapes hold water
where they would.

## 1. The pour

A basin is found by the priority-flood: start from every border column at
its own height (the map edge drains off-world), repeatedly take the lowest
frontier column and give each unvisited neighbour a **spill level** of
`max(neighbour height, current level)`. A column whose spill level is above
its height is under a lake whose surface is that level; the level is the
same for every column of one basin, by construction.

Two caps keep it honest:

- a lake deeper than `lake_max_depth()` (16) is cut to that depth — a
  ridged-noise pit can be forty blocks and the priority flood would fill it
  to the brim;
- a basin under `lake_min_cols()` (6) columns is left dry: a one-column dip
  is a puddle the water sim would evaporate anyway.

The result is one `NativeU8Arr` per world, `Lakes.levels(seed)`, 0 where
there is no lake and the surface height otherwise. It is a pure function of
the seed (through `Noise.height`), 16,384 columns, and takes single-digit
milliseconds — it is essentially `Biome.distances` with a priority queue in
place of the level sweep, which the same G68 rule shapes: one array threaded
through every write, never wrapped.

## 2. Filling

`Chunk.generate` takes the level table and fills each column from its
surface to its lake level with **source** blocks (id 4), the same block the
sea is made of. Above the snow line the surface block is ice, as a spring is.

Then the surface rules: the sand shore test compares against the *local*
water level (`sea_level()` where there is no lake), so a lake has a beach;
the lake bed under `level - 5` is dirt, as the sea bed is.

## 3. The water sim

`Water.springs_in` calls every source above sea level a spring, marks it
every tick, and gives it `spring_rate()` — a lake of 400 sources would eat
the tick budget (`max_cells_per_tick` 250) doing nothing. So the spring list
takes only sources with somewhere to give: a horizontal or downward
neighbour that is not water and not solid. In a full lake that is nobody —
*including the pour point*: the rim column's block at the lake's level is
its surface, solid, so a full lake has no outlet at all (the design above
first claimed it overflowed there; it did not). So the pour finds the
**notch**: the dry rim column that first relaxes a lake column to the
lake's level is recorded (`Lakes.notch_at`), one per basin of
`lake_min_cols()` (6) or more, and the generator digs it one block down.
The lake cell beside the notch is then a spring, and a brook leaves every
real lake over its notch at one unit a tick. Digging the rim elsewhere
makes the exposed sources springs (`springs_after` on the edited cell's
neighbours), and the lake gives there at the spring rate. Sources never
evaporate, so a lake keeps its level.

The actor rebuilds its chunk from `(cx, cz, seed)` on `WLoad` and messages
may not carry arrays (GAPS G44), so each actor computes `Lakes.levels(seed)`
itself: 64 actors, single-digit milliseconds each, once. If that measures
badly, the alternative is a run-length list of (column, level) in the load
message.

## 4. Biome

Lake columns are water flags. Bodies are grouped and sized already: a small
cirque is a small body (a green ring, or an oasis where hot), a plateau lake
is a large one with `reach()`. Distance-to-water is what moisture is, so the
climate around every lake follows without a rule.

## 5. Age

Nothing in the pour is age-specific. The terrain makes the basins, and age
makes the terrain: young worlds have many small sharp basins between ridges,
ancient ones a few wide shallow lakes on their floodplains and terraces.
Measure it (§6) rather than adding a knob.

## 6. Measured

*As built, 2026-09-05.* Three things moved from the design above:

- **No caps.** Neither the depth cap nor the minimum size was built: both
  need basins labelled, and the measurement did not ask for them. The
  deepest lakes are mountain cirques (a surface at 100-125 on seeds 7, 99
  and 2024); a one-column dip is a single inert source.
- **The actors compute the table themselves**, `Lakes.levels(seed, n)`, 30
  ms each on this machine (16,384 scalar heights plus the sweep), once at
  load; the frame loop computes it once for `World.generate`.
- **The spring list** (`Water.is_spring_at`) reads its neighbours through the
  edge mirrors, so a lake crossing a chunk edge is all interior; and the
  list is refreshed for a cell and its six neighbours only when a source
  came or went or a cell's openness flipped (`springs_after`), so the sim's
  hot path does not pay for it. The render side (`is_world_spring`) applies
  the same rule for spray, so a lake is not a thousand emitters.

Lake columns by seed and age, `CF_TERRAIN_STATS` (of 16,384). *Corrected
2026-09-05:* the first counts (729 / 2,321 / 2,283 on seeds 7, 99 and 2024)
were mostly dips in the sea bed that drained to the border below sea level
and were poured as "lakes" at that lower level -- a column filled to 55 in
a sea at 62, a hole in the ocean with walls of water, seen in play. The
pour's heightmap is floored at sea level now, so the sea is the base every
basin drains into; `lakes_test` asserts no lake surface sits at or under
sea level over eight seeds (4,624 such columns before).

| seed | age 50 | age 100 |
|---|---|---|
| 7 | 105 | 81 |
| 1234 | 972 | 970 |
| 99 | 300 | 419 |

Seed 99 at age 50, windowed, 900 frames: 4 spray springs for 2,321 lake
columns, worst frame 14.6 ms, flowing water 85 cells at frame 800 (the
outlets' brooks). The map draws lakes as `~`: a cirque under the snowfields,
a plateau lake, lowland ponds strung along the valleys.

**The leak, and the notch (dug into 2026-09-05).** The "outlets" first
measured here were leaks: a cave worm carving rock beside a lake's basin
below its surface opened the wall, and every lake cell along it was an
infinite spring into the tunnel. Seed 1234 at age 100 with valley springs
off: 26 springs and 739 flowing cells with caves, 1 and 11 with `CF_KARST=0`.
Worms now leave basin walls alone (`Caves.basin_wall`: any voxel at or
below a lake's surface in, or beside, a lake column), and `caves_test`
asserts an ancient world has no lake cell with an open neighbour that is
not its notch. With that fixed a full lake had no outlet at all (§3), so
the notch was built; the first cut marked every column of a flat rim a
pour point (90 outlets a world), and one per basin of six or more is kept.

| seed | age | lake columns | outlets |
|---|---|---|---|
| 7 | 50 / 100 | 105 / 81 | 5 / 1 |
| 1234 | 50 / 100 | 972 / 970 | 10 / 8 |
| 99 | 50 / 100 | 300 / 419 | 4 / 4 |

(after the sea-level floor above; the first pass counted sea-bed pits).

Seed 1234 windowed, 1,200 frames, everything on: flowing water plateaus at
~410 cells at age 50 and ~370 at age 100 (it was 1,544 and climbing), 109
and 106 fps, worst frames 14.9 and 14.7 ms. The table takes ~20 ms with
the labelling.

## 7. Tests

- Priority flood on a synthetic 8x8 heightmap with one bowl: the bowl's
  columns get the rim's height, the rim and outside get 0.
- Depth and size caps.
- A generated chunk over a lake column holds sources from the surface to the
  level and air above; the lane heights are untouched.
- A sim loaded on a lake chunk lists only the outlet as a spring, and its
  volume is flat over 200 ticks (sources do not count in `volume`).
