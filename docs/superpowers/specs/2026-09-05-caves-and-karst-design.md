# Caves and karst — design

The world is a heightmap: every column is solid from bedrock to its surface,
with nothing under an overhang because there are no overhangs. Caves change
what is under the player's feet, and in a world whose age is erosion they
are its underground half: water dissolving rock, so an ancient world is
hollowed where a young one is solid. This carves tunnels and, past a
certain age, the sinkholes that open into them.

## 1. Worms, not noise

Three-dimensional noise over a chunk is 65,536 samples of eight hashes each;
over the world, 33 million hashes, most of them in rock nobody sees. A cave
is instead a **worm**: a polyline carved as a run of spheres, placed per
32-block cell exactly as trees are placed per 8-block cell, so every chunk
carves the same worm the same way (`Chunk.plant`'s apron of cells, widened
to the worm's reach).

A cell holds a worm when `cell_hash(cx, cz, seed, 8) < karst_density(a)`;
`karst_density` is 0 at age 0, rising to 0.6 at 100. The worm starts at a
point in the cell at depth `y0` in `[bedrock_depth() + 6, surface - 8]`,
heads in a hashed direction with a hashed pitch, and takes `worm_len` (24 ..
64 blocks, by age) steps of 2 blocks, turning by a smooth hashed yaw and
pitch each step. Radius `worm_r` is 1.5 young .. 3 ancient, with a per-step
wobble, and the sphere at each step clears every voxel within it that is
rock (stone, basalt, granite, the strata blocks) — never the surface block,
never a subsurface block, never bedrock, never water. A worm is clamped to
`y >= sea_level() + 2` and rises where it must: one that dipped under the
sea level would open a dry pocket below the water table, and the water sim
would spend a thousand ticks filling it from the sea rule. The water table
is where karst stops, as it does.

Two worms whose cells are adjacent are joined when their ends come within 6
blocks (the join is a straight run), so an ancient world is a network, not
a scatter of tubes. Chambers — a sphere of radius 5-7 at a worm's midpoint
in one worm in four — give the network rooms.

## 2. Sinkholes

Past `sinkhole_age()` (60), a worm that passes within `sinkhole_reach()` (6)
blocks of the surface at any step collapses the column above that step: the
column's blocks from the surface down to the tunnel roof are removed, with
a radius-2 funnel at the surface. The surface ends up at the tunnel floor
where the funnel is; that column's height in the biome's scan drops, its
material becomes gravel (talus) at the funnel's rim and stone at the floor,
and rain and light reach the tunnel there. On the map it is a dark pit.

## 3. What assumes solid columns

Checked, and what changes:

- **Chunk.fill** — worms are carved after the fill and after the trees, in
  `Chunk.plant`'s pass; the water fill happens first and worms never touch
  water or go below its table.
- **Light** — `flood` seeds every column from the top and the sweep is a 3D
  BFS; caves are dark, sinkholes let light in, `sky_floor` is unaffected.
  Block light is what lights a cave (glowing mycelium, glow caps, the
  flashlight); stone is a mycelium base, so the network can move into a
  cave and light it, which is the best reason to build this.
- **Occupancy and shadows** — per voxel; nothing assumes a column.
- **Biome.surface_go** scans down from the sky floor to the first solid:
  a sinkhole column's height is the tunnel floor, which is right. A cave
  under intact surface is invisible to it, also right.
- **Physics, raycast, digging** — per voxel.
- **Trees** — planted before the carve; a worm under a tree leaves the
  subsurface (never carved), so roots are safe. A sinkhole under a tree
  takes the tree's column with it; `Veg` decay handles a trunk over air as
  it handles a chopped one.
- **Water actors** rebuild the chunk from the seed, so they carve too.
- **find_spawn** uses `Noise.height` and would happily pick a sinkhole's
  rim; it reads the world's block surface instead (one scan of a 32x32
  box).
- **Save** — blocks are saved; nothing to add.

## 4. Age

`karst_density`, `worm_len`, `worm_r` and the sinkhole gate above. At age 0
there are no caves at all. At 30, a few short tubes. At 100, a network with
chambers and a pitted surface. Age is the whole mechanic.

## 5. Cost

A worm is 12-32 spheres of radius up to 3: at most ~4,000 voxel writes,
most of them air already. Per chunk the apron of 32-block cells is 3x3, so
each chunk visits up to nine worms and writes only the voxels inside it. At
density 0.6 that is ~5 worms a chunk, well under a millisecond. Meshing is
where the cost lands: cave walls are new faces, and a pitted ancient world
may mesh 20-40% more vertices. Measured; `CF_MESH_REPS` exists for it.

## 6. Measured

*As built, 2026-09-05.* What moved from the design:

- **No joins.** Worms are not joined end to end; on a 128-block world the
  4x4 grid of 32-block cells holds at most sixteen, and at density 0.6 a
  few of them cross by chance. Chambers are in (one worm in four).
- **A worm is boxed** to its cell plus one cell either way by reflecting
  its heading, so a chunk's 3x3 cell apron is exact.
- **The floor** is `sea_level() + 3 + r`, not + 2: the centre is floored
  and the radius wobbles, and the lowest carved voxel must still sit at
  + 2 or above (tested).
- **Lowland worms carve nothing.** Between the water table and `roof()`
  under a surface below ~74 there is no room; a worm placed there is a
  no-op. Caves are an upland thing, which is where karst is.
- **Sinkholes** clear the columns within two of a head that comes within
  `sinkhole_reach()` of the surface, from the head to the surface, unless
  water stands in the column. `find_spawn` reads the world's surface now,
  and its flatness from the world too, so it does not pick a rim.
- **The stat** counts air at or below the *piled terrain height*, not
  below the top block: a canopy has air under it and is not a cave.

`CF_TERRAIN_STATS`, air voxels at or below the terrain surface, and worm
cells of sixteen:

| seed | age 0 | age 60 | age 100 |
|---|---|---|---|
| 7 | 0 / 0 | 2,199 / 3 | 7,210 / 7 |
| 1234 | 6 / 0 | 4,961 / 7 | 12,734 / 11 |

(the six at age 0 are bog pools' air over their water). Mesh vertices on
seed 7: 225,408 at age 0, 246,870 at 60, 277,824 at 100 (+23%), with lakes
and talus also moving with age; `mesh all` time unchanged within noise;
world build unchanged within noise.

## 7. Tests

- A worm never removes the surface, subsurface, bedrock, water or a voxel
  under `sea_level() + 2`.
- Determinism: two chunks sharing a worm's cell carve identical voxels in
  their overlap.
- Age 0 carves nothing: a generated chunk is column-solid, checked
  voxel-by-voxel against the fill.
- A sinkhole column's surface, as `Biome.surface_go` sees it, is the tunnel
  floor, and the column above is air to the sky.
