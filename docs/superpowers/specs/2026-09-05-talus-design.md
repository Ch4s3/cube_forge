# Talus — design

A cliff in the game meets the ground at a right angle. A worn mountain does
not: loose rock piles at the foot of every face at the angle it comes to
rest, and the older the face the deeper the pile. Gravel (20) exists as the
alpine surface; this puts it where cliffs shed it, in an amount that grows
with the world's age.

## 1. The pile

For a column with height `h`, look at the heightmap within `talus_reach(a)`
columns (1 young .. 3 ancient) and take the largest drop onto it:

    drop = max over neighbours n within reach of (h_n - h - dist(n) * angle)

where `angle` (2) is the slope at which rock rests: a neighbour two columns
away and four blocks up contributes nothing. When `drop >= cliff_slope()`
(3) the column carries a pile

    pile = min(talus_depth(a), (drop - 2) / 2)      -- 0..3 blocks

of gravel on top of its surface, with gravel as its subsurface. `talus_depth`
is 0 at age 0 (a young cliff is clean), 1 at mid-age, 3 ancient.

Heights come from the chunk's apron (heightmap apron spec), which is why
that lands first: reach 3 needs a 3-column apron, and asking `Noise.height`
for 48 neighbours a column would multiply the world build several times.

## 2. What it does to the terrain

The pile raises the column, so the cliff's foot becomes a short gravel ramp:
the column's slope against its uphill neighbour drops by `pile`, and a
one-block step onto the ramp is walkable where a three-block face was not.
Materials are decided after the pile (the pile *is* the surface), so
`surface_id` sees gravel with slope under `cliff_slope()` — gravel, not
granite — and the cliff proper above it keeps its granite.

Because the pile reads the apron, it is a pure function of the world column
and agrees across chunk edges like every other rule.

## 3. Biome

Gravel is a palette block. Where the pile lies in grassland or forest the
retexture will grass it over within the migration budget, which is what an
old talus slope looks like; alpine keeps gravel; the subsurface gravel stays
under the grass and shows when dug. Nothing to add.

## 4. Age

`talus_reach` and `talus_depth` above. The visible reading is: young worlds
have sharp cliffs to the ground; ancient worlds have every face standing in
its own skirt of scree, and the mountain valleys of the erosion spec (smooth
floors, gated octaves) are floored with it.

## 5. Cost

Reach 1 is 8 apron reads a column, reach 3 is 48; both are array reads, not
noise. Expected under 5 ms over the world at reach 3.

## 6. Measured

- `CF_TERRAIN_STATS` gains a gravel percentage; expected 0% at age 0, a few
  percent at 100 on a mountain seed (7, 1234).
- Granite percentage at age 100, expected to fall by the columns that turned
  to ramp.

## 7. Tests

- A synthetic apron with one three-block step: the low column gets a pile of
  1 at mid-age, 0 at age 0; a step of one gets none at any age.
- The pile never exceeds `talus_depth(a)` and never exceeds the drop.
- Materials: the pile column is gravel over gravel; the column above the
  face is still granite.
