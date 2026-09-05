# Rock strata — design

A column under its four-block subsurface is stone (3) down to
`deep_stone_depth()` (24), basalt (22) below that, bedrock at the floor
(`fill_column_mat`). Every cliff face, canyon wall and terrace rim the
terrain now cuts shows the same flat stone. Bedding the rock in bands makes
each exposed face read as geology, and the deeper cuts of an old world expose
more of it. It is a material rule only: no height changes, nothing off the
heightmap path.

## 1. Bands

`Chunk.rock_id(wx, y, wz, seed)` replaces the literal `3` in the fill:

    o  = tilt_x * wx + tilt_z * wz + 6 * (value2(wx * 0.02, wz * 0.02, seed + 1200) - 0.5)
    k  = floor((y + o) / band_thickness)
    id = band_material(hash2(k, region(wx, wz), seed + 1201))

- **tilt**: a fixed dip per world, `tilt_x, tilt_z` in ±0.08 from the seed
  (a few degrees), so bands cross a cliff at an angle rather than ruling it.
- **warp**: the low-frequency term folds the bands gently; without it the
  bands are planes and every cliff in the world shows the same stripes.
- **thickness**: `band_thickness` 4, with the hash nudging each band ±1 so
  they are not a ruler.
- **region**: `floor(wx / 64), floor(wz / 64)`, so the band sequence changes
  across the map and two distant cliffs do not match.

`band_material` draws from stone (3) at 55%, basalt (22) at 20%, granite (14)
at 10%, and two **new blocks** at 15% between them:

| id | block | look |
|---|---|---|
| 47 | sandstone | pale, faintly layered |
| 48 | shale | grey-blue, thin lines |

Ids 47 and 48 are the first free above the mycelium range (26..46); the fruit
and palm ids sit higher. Two texture layers (`Texture.layers()` 89 -> 91),
generated procedurally like the rest. Both harvest as **stone** (one inventory
slot, no new items) in this phase; a later phase can give them slots.

The four-block subsurface keeps its rule (dirt, sand or granite by surface),
so the bands start below it; the deep basalt band (`deep_stone_depth`) and
bedrock are unchanged, and `band_material` never returns bedrock.

## 2. What sees it

- Cliff faces (slope >= `cliff_slope()`), canyon walls in the river valleys,
  terrace rims on the plateaus, and everything the player digs.
- The mesher keys faces by block id, so a band boundary is a real edge and
  greedy quads stop at it; vertex counts rise on exposed faces. Measured.
- `Biome.is_palette_block` excludes stone and granite and must exclude the
  two new ids, so the retexture never rewrites a band.
- Mycelium bases (grass, dirt, sand, snow, gravel, clay, stone) do not
  include the new blocks; the network cannot claim a sandstone face. Fine.

## 3. Age

None directly. Erosion is what exposes strata, and the age already decides
how deep the cuts are; the bands themselves are as old as the world.

## 4. Cost

One `value2` per column (the warp) and one `hash2` per band crossed, not per
block: `fill_column_mat` carries the current band and its material down the
column and rehashes only when `k` changes. Well under a millisecond a chunk.

## 5. Measured

- World build time, unchanged to within noise.
- Vertex count from `mesh all` before and after (seed 7, age 50), expected
  up a few percent on cliff-heavy seeds.

## 6. Tests

- `rock_id` is a function of (wx, y, wz, seed): the same across chunks.
- Bands are contiguous in y with thickness 3..5, and only stone, basalt,
  granite, sandstone or shale come back.
- The four-block subsurface and the bedrock floor are exactly as before
  (`terrain_test` deep stone band tests extended).
- `layer_for` maps the two ids to two distinct new layers.
