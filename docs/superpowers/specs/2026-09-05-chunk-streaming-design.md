# Chunk streaming — design

The world is a fixed grid of at most 8x8 chunks, and every field that
describes it -- skylight, block light, occupancy, the biome and mycelium
fields, the shown species, the meshes, the water actors -- is an array of
that size written against the literal 128. Going bigger means either raising
the literal (a 256-block world opens in five to eight seconds and is still a
box) or letting the world be unbounded and keeping every cost proportional
to what is near the player. This is the second: a **window** of 8x8 chunks
that slides with the player over an unbounded world of chunks.

## 1. The window

Nothing about the fixed arrays changes. 128 stops meaning "the world" and
means "the window": the 128x256x128 light and occupancy arrays, the
16,384-column biome, mycelium and shown arrays, the 64 meshes, the 64 water
actors, the shader's `WORLD`, the shim's skylight lookup and coarse
occupancy are all window-sized, and their proofs and literals stand.

The `World` gains an **origin** `(ox, oz)`: the world chunk coordinates of
the window's corner. A world chunk `(cx, cz)` is in the window when
`0 <= cx - ox < 8` and likewise for z, and its local index is
`(cx - ox) + 8 * (cz - oz)` -- `World.ci_of`. Two coordinate spaces, and
which one a value is in is a property of what it describes:

| world coordinates (unbounded) | window coordinates (0..127) |
|---|---|
| the player, the camera, the view matrix | the light and occupancy arrays' indices |
| mesh vertices (floats; fine to +-100k) | the biome, mycelium and shown arrays' columns |
| block edits, the raycast, `World.block_at` | the mesh, count, flag, pending and actor arrays' slots |
| a water actor's identity `(cx, cz)` | the shim's occupancy texture and skylight lookup |
| springs and spray in the shim | |

`World.block_at`, `chunk_at`, `set_block`, `set_cells`, `shown_at` take
world coordinates and translate. The light module works in window
coordinates internally (its `index` proofs are about the array) and its
entry points -- `relight_marked`, `seed_column`, `flood`, `occupancy`,
`get` for the mesher -- take world coordinates and subtract the origin once.
The mesher gets two origins per chunk: the world one for vertex positions,
the window one for its light, occupancy and shown lookups. The shim gets
`cf_gfx_set_origin(x, z)`: a `u_origin` the shadow ray-march subtracts
before sampling, the origin the precipitation's skylight lookup subtracts,
and the one `cf_gfx_set_voxel` subtracts. Springs and spray are drawn in
world coordinates and need nothing.

## 2. Generation is already unbounded

`Chunk.generate_lakes(wx0, wz0, seed, lk, n)` is a pure function of the
world column, for any `wx0`: the noise floors, the tree and worm cells
floor-divide, the apron reads whatever columns it needs. The one global
thing is the **lake pour**, which floods the whole world's heightmap from a
draining border. It becomes a pour per **tile**: a tile is 8x8 chunks on a
fixed grid (tile `(tx, tz)` covers world chunks `8tx .. 8tx+7`), poured
with its own border draining, so a basin that crosses a tile seam is cut
there and never fills. A chunk generates against its tile's table; a
window covers at most four tiles; the water actors compute the tile their
chunk is in (30 ms, once). A tree whose canopy crosses a tile seam into a
lake column of the next tile is planted by one chunk and not the other --
the canopy stamps only into air, so at worst a partial canopy stands over
that lake; accepted.

## 3. The shift

When the player's chunk leaves the window's central 2x2 -- local chunk x
or z under 3 or over 4 -- the window shifts by one chunk toward the player,
and again next frame if still off-centre. Every shift is the same eight
steps, in `World.shift`, `Biome.shift`, `Myc.shift` and the scene:

1. **Chunks.** The new origin; the chunk array remapped; the eight chunks
   entering are taken from the evicted cache (§4) if they are there, else
   generated (a `pmap` of eight, ~3 ms); the eight leaving go to the cache
   if dirty and are dropped otherwise.
2. **Light.** The skylight, block-light and occupancy arrays are shifted
   in place by one chunk, a blit per row (the layouts make a z shift one
   contiguous copy per layer and an x shift a copy per row). The new
   column of chunks is seeded (sky columns, emitters, occupancy) and lit
   by the existing box sweep over that column widened by
   `relight_radius()`, so light from the chunks that stayed crosses the
   seam. The whole occupancy texture is re-uploaded (4 MB, ~1 ms).
3. **Biome.** The nine per-column arrays shift; the new columns are
   scanned (surface, water flag), classified, set active, and the
   distances marked stale so the sliced tick recomputes them.
4. **Mycelium and shown.** The arrays shift; new columns are empty, or
   restored from the cache with their chunk.
5. **Meshes.** The mesh, count, flag, pending and mark arrays shift; the
   eight new chunks are queued as wholly pending, so the drain meshes
   them over the following frames on its existing budget rather than in
   the shift; the chunks that stayed re-upload to their new slots (Phase 4
   gives slots a stable identity so they do not).
6. **Water.** The actor array shifts with the chunks, so a chunk keeps its
   actor; the eight freed actors are sent `WLoad` for the new chunks.
7. **Springs.** The shim's spring table is rebuilt from the window.
8. **The shim.** `cf_gfx_set_origin`.

Nothing the player holds moves: position, camera and meshes are in world
coordinates, so a shift is invisible except for the far edge of the world
coming into being. The budget is under 60 ms for a shift with the meshing
deferred; measured in Phase 4, and the trigger sits two chunks inside the
edge so the deferred meshing lands before the edge is seen.

## 4. Persistence

Chunks leaving the window are kept when they are **dirty** -- the world
marks a chunk dirty in `set_chunk`, its single writer -- in an in-memory
**evicted cache** keyed by world chunk coordinates, with the chunk and its
columns' mycelium and biome state (a 16x16 blob per chunk). A clean chunk
regenerates identically from the seed, so it is dropped. A chunk
re-entering the window is restored from the cache first.

A save slot is the header (seed, origin, player, inventory, day, weather),
the window's chunks and the cached chunks, each as a file named by its
world chunk coordinates, plus the window's field blob as today and a field
blob per cached chunk; the header lists the cached coordinates so a load
needs no directory listing. A load reads the header, fills the window from
the files it lists (generating any missing chunk), and puts the rest in
the cache. Old slots (files by window index, origin 0) still read.

## 5. What else moves

- **Spawn** at the window centre; the window starts centred on the spawn.
- **The map view** and biome overlay draw the window: already local.
- **`find_water`, `find_spawn`, the stats and the map dump** scan the
  window in world coordinates.
- **Lakes' notches, valley springs, caves** are per column or per cell:
  unbounded already.
- **The size menu row** goes away: a world has no size. The seed's size
  byte is ignored (and stays reproducible: the terrain never read it).

## 6. Phases

1. **Coordinates.** The origin exists and is `(0, 0)`: `World.ci_of`,
   the translations in `World`, the light entry points, the mesher's two
   origins, the shim's origin. Behaviour identical; every test passes.
2. **The shift.** `World.shift`, `Biome.shift`, `Myc.shift`, the scene's
   shift, the trigger, tile pours. Walked with `CF_AUTOWALK` across an
   edge; the world continues.
3. **Persistence.** Dirty flags, the cache, the save format, restore.
   Edit, walk away, walk back, save, load.
4. **Budget.** Measure the shift; stable mesh slots; the light band's
   width; a shift every frame while far off-centre.

## 7. Measured

*Built 2026-09-05*, in the four phases above, with these departures:

- **Coordinates.** The design's Phase 1 (world coordinates for the player,
  meshes and edits, a `u_origin` in the shader) was not built. Everything
  the engine holds stays window-local, and a shift translates the few
  things that float: the player by 16 blocks, and in the shim the mesh
  slots (a moved slot draws with a -16 offset, `u_off`, reset when March
  uploads a mesh baked at the current origin), the precipitation and spray
  particles, the spray emitters and the springs. Lighting, biome,
  vegetation, fungus, the mesher and the water sim were not touched for
  coordinates at all. The one thing that had to learn the world's
  coordinates is the biome's temperature octave, read at the world column
  so the climate is the ground's and not the window's.
- **A moved chunk is rebaked whole.** A mesh is baked at the local origin
  its chunk had; after a shift a moved slot draws with the shim's -16
  offset, but the drain and the edit paths rebuild single sections and
  upload the whole pass buffer, which mixed two origins in one buffer
  (seen as slabs floating in the sky). So a moved chunk is flagged (the
  counts array's fourth block) and, while flagged, any rebuild of it
  rebuilds all sixteen sections and all passes; the shift also owes every
  moved chunk to the drain, which rebakes them at the new origin over the
  following seconds.
- **The band is meshed inline**, in one `pmap` over its eight chunks (12
  ms), rather than deferred to the drain: it is at the window's far edge
  and its vertices are needed by the time it is seen.
- **Springs** are scanned over the band only (3 ms; the whole window was
  35).
- **Lake tiles** are kept in the world, at most four, so a shift inside a
  tile does not pour it again (26 ms when it does).
- **The cache** keys on the dirty flag `set_chunk` sets, so a chunk the
  water sim or the fungus touched is flagged too -- but a flagged chunk
  whose blocks hash to what it generated to (the world keeps each chunk's
  generation hash; a walk of the 65,536 voxels is 0.15 ms) is dropped at
  eviction: water that came and went, a fungus that retreated. The hashes
  travel with the cache entries and the save header (`hashes`, and the
  `cached` list is cx, cz, hash triples). Open: a bound on the cache with
  spill to disk for a long walk through edited ground.
- **The size row and `CF_SIZE`** went; the world has no size. A seed's size
  byte is ignored and still reproduces.

A shift, seed 1234 at age 50, walking south (`CF_STREAM_LOG=1`), after
the cuts of the same day (the biome shift blits its rows and scans the band
from the band's own top through the chunk directly, 17 ms -> 2; the sky
sweep's box stops at the band's top; the fungus shift blits, 0.2 ms):

| stage | ms |
|---|---|
| world: generate 8 chunks (pmap) | 12 |
| world: slide three voxel fields (blits) | 1-2 |
| world: light the band (sky) | 14 |
| world: light the band (block light) | 6 |
| world: occupancy of the band | 5 |
| world: hash the chunks leaving | 1 |
| biome and mycelium field shifts | 2 |
| shim shift, occupancy upload, array remaps | 4 |
| mesh the band (pmap) and upload | 11 |
| actors and springs | 3 |
| **a shift within a tile** | **63-70** |
| a shift that pours a new tile | +26 |

Frame rate on the walk 102-105 fps against 110 standing; the worst frame
is the shift. What is left is generation, the sky sweep and the meshing,
each already parallel or bounded by the band.

Persistence: a walk across two shifts saved to a scratch slot (origin
`0 -2`, twelve cached chunks, 90 files) and loaded back with the same
world hash at 108 fps. `stream_test` covers the shift of every field, the
shift back regenerating the original, the cache round trip of an edited
chunk, and a fungus column blob.
