# Heightmap apron — design (prerequisite for the world-gen set)

Every column the generator fills asks `Noise.height` five times: once for
itself (from the F32x4 path) and four more inside `Noise.slope` for its
neighbours. With the layered generator that is roughly 15 value-noise samples
per height, 75 per column, and it is the whole of the 240-370 ms world build.
Four of the specs that follow (talus, sediment, lakes, climate ground) need
more neighbours still, and would multiply that again.

## The apron

`Chunk.generate` computes one **heightmap for the chunk plus an apron** of
`apron()` columns on every side — a `(16 + 2a)^2` `NativeIntArr`, filled with
`height_x4` four columns at a time — and every per-column rule reads heights
from it: slope, talus, sediment, the shore test. `Noise.slope` stays as the
pure scalar reference and is what the tests and `find_spawn` use.

| apron | array | heights per column | today |
|---|---|---|---|
| 1 | 18 x 18 | 1.27 | 5 |
| 2 | 20 x 20 | 1.56 | 5 |
| 3 | 22 x 22 | 1.89 | 5 |

The apron is a chunk-local cache, never shared: the array is threaded through
the fill and dropped, so it does not meet the shared-reference copy (GAPS G68).

## Agreement

The F32x4 and scalar heights disagree on 0-5 columns in 16,384 (see
`CF_TERRAIN_STATS`). Today the slope of a column is computed from *scalar*
neighbours against an *F32x4* own height, so a disagreement can shift a
material by one block. With the apron every height in a chunk comes from one
path, and the only remaining seam is the apron's overlap with the next chunk,
where both chunks read the same `height_x4` lanes for the same columns (the
lane index is `wx % 4`, fixed by the world x, not by the chunk). So the seam
agrees by construction. `terrain_test` gains: the apron of chunk (cx, cz)
equals the interior of its neighbours where they overlap.

## Measured

- World build time before and after, `CF_HEADLESS=1`.
- `CF_TERRAIN_STATS` surface percentages unchanged to within 1% (materials
  move only on the 0-5 disagreeing columns).

## Landing

One change, no behaviour change beyond those columns. It lands first; the
specs below assume `heights(c, lx, lz)` with `lx, lz` in `[-a, 16 + a)`.
