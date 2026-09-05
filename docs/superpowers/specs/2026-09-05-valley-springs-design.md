# Springs at valley heads — design

The terrain now carves river valleys (`Noise.river_mask`, the crease of a
low-frequency ridged octave), and a valley whose floor dips under sea level
fills. Above that it is a dry wash. Springs exist (`Trees.spring_at_cell`:
one per 8-block cell at 100 per mille, on ground at 72 or higher with a slope
of at least two) but are scattered over mountainsides with no regard for
where the valleys are. This puts them in the valleys, so the channels the
terrain draws carry the water the sim already knows how to move.

## 1. Where a valley spring goes

A cell holds a valley spring when its trunk column (`Trees.cell_trunk`, the
same deterministic pick trees use) is

- **on the centreline**: `river_mask(xw, zw, seed) >= spring_mask()` (0.85),
  sampled at the warped point exactly as `Noise.height` does — a helper
  `Noise.river_at(wx, wz, seed)` exposes the mask for a world column;
- **upland**: height above `sea_level() + spring_rise()` (12), so the sea's
  own inlets are not seeded; the channel below that already holds water;
- **not a plain**: slope at least one along the channel, so a spring on a
  perfectly flat floor (a terraced plateau) makes a pool, which is allowed
  but rare.

The density draw stays (`cell_hash(cx, cz, seed, 6)`), against a separate
knob `CF_VALLEY_SPRING_DENSITY`, per mille of *valley* cells, default found by
measurement (§4); the mountainside rule keeps its own knob. Above the snow
line the spring is ice, as today.

A valley of width ~20 (mid-age) crosses roughly 2-3 cells per 8 blocks of
length; at 500 per mille that is a spring every ~10-16 blocks along the
channel. Each gives `spring_rate()` (1 unit a tick) and thin sky-exposed
water evaporates at `1/evap()`, so a single spring runs a brook a dozen cells
long before it dies (RESULTS.md, springs). A chain of them is a brook that
reaches the next spring: a continuous stream down the valley, its depth set
by the spacing, not by any new mechanism.

## 2. Age

`river_edge` already widens the mask with age, so more cells qualify in an
old world; and `spring_rise()` falls with age (12 young, 4 ancient), so old
valleys run wet almost to the sea. Young worlds: a few springs high up,
mostly dry washes. No new knob is age-specific.

## 3. What it touches

- `Trees.spring_at_cell` gains the valley test as a second way to say yes.
  `plant_springs_go` in `Chunk` is unchanged: a spring is still one source at
  the surface of its column, never straddling an edge.
- `Water`: nothing. Sources above sea level are springs (`springs_in`), rate
  capped, marked once a tick. The count per chunk rises; `max_cells_per_tick`
  (250) is the thing to watch.
- `Biome`: nothing, and this is the point. The brook's columns are water
  flags; a brook under `small_body()` (48 columns) wets `small_reach()` (4)
  either side — a green ribbon down the valley, wetland where it is damp
  enough — and once brooks join into a body over 48 columns the ribbon widens
  to `reach()`. Oasis needs hot and small water, so a desert valley brook
  rings itself with palms without any rule saying so.

## 4. Measured

*As built, 2026-09-05.* The centreline test at 0.85 was a block and a half
wide and a cell's trunk column almost never landed on it (0-6 valley springs
a world at any density); `spring_mask()` is 0.6, the valley floor, and the
density default is 500. Springs by kind (`CF_TERRAIN_STATS`), seed 1234
being the valley-rich one:

| seed | age 0 | age 50 | age 100 |
|---|---|---|---|
| 7 | 6 + 5 | 7 + 3 | 3 + 7 |
| 1234 | 10 + 13 | 7 + 16 | 2 + 25 |
| 99 | 5 + 2 | 4 + 3 | 2 + 5 |
| 2024 | 11 + 1 | 9 + 4 | 9 + 10 |

(mountainside + valley). `CF_WATER_LOG=1` prints the world's flowing
(non-source) water cells every 100 frames, `Chunk.count_flow`. Seed 1234,
1200 frames, windowed:

| | frame 100 | 500 | 1100 |
|---|---|---|---|
| age 0 | 90 | 270 | 413 |
| age 50 | 144 | 360 | 491 |
| age 100 | 155 | 317 | 412 |
| age 100, valley density 0 | 12 | 37 | 38 |

About twenty cells a spring at the plateau, and a tenfold rise over the
mountainside springs alone. Frame time did not move outside its noise
(worst frame 18-23 ms with or without them, one 85 ms outlier with them off).
`CF_TERRAIN_MAP` draws `s` on a 2x2 holding a spring column.

## 5. Tests

- Every valley spring column has `river_at >= spring_mask()` and is above
  `sea_level() + spring_rise(age)`.
- A chunk crossed by an upland valley (seed found by the map) holds at least
  one spring at density 500; a chunk with no valley holds none from this rule.
- Determinism across the chunk edge: the cell test is a pure function of the
  cell, as for trees.
- Water: a sim loaded on a valley chunk reaches a volume plateau (existing
  spring test shape) rather than growing.

## 6. Later

Aquifers (todos.md) bound a spring's reservoir and recharge it from rain;
valley springs would be the first to draw on it, since they are the ones that
make lakes fill in sealed terrace basins.
