# Points of interest — rare, hand-shaped features in generated terrain — design

Everything the generator makes today is a **field**: height, temperature, the
river mask, strata, dunes, glaciers, caves. A field is the same rule everywhere,
so the world is varied but never *particular* — there is no place a player walks
to and remembers. This adds a second layer on top: a small number of **sites**,
placed sparsely and deliberately, each carrying a shape that is nothing like the
field around it. A canyon. An arch. A tree the size of a hill.

The seven asked for:

| feature | what it is |
|---|---|
| canyon system with a river | a terraced slot 40+ blocks deep along a river, hundreds long |
| natural stone arch | a rock span over a river channel |
| giant tree | a trunk 40-60 tall with twisting branches |
| crystal cavern | linked chambers underground, lit by glowing crystal |
| atoll with a deep lagoon | a reef ring in the ocean around deep water |
| desert buttes | a cluster of flat-topped mesas with vertical walls |
| giant river with a delta | a wide channel fanning into distributaries at the sea |

## 1. Two kinds of feature, two hooks

The split that drives everything: **does the feature change the heightmap?**

**Landform POIs** — canyon, atoll, buttes, delta — do. They must be a term in
`Noise.height` itself, because the heightmap is what every downstream pass reads:
the lake pour floods it, the talus pass piles against its slopes, sediment
flattens its valley floors, the biome takes its temperature from the elevation,
trees refuse to stand on its cliffs, springs sit on its centrelines. A canyon
added as a term in `height` gets a brook down its floor, scree at the foot of its
walls, and strata banding on its walls, all without asking. A canyon carved after
the fact would get none of it.

**Structure POIs** — arch, giant tree, crystal cavern — do not. They are blocks
placed in or removed from a chunk after it is filled, exactly as trees are
stamped and cave worms are carved today, and they take the same rule: **write
only into air** (a stamp) or **remove only rock** (a carve), so a structure can
never eat the terrain it stands on.

## 2. Two placements, matching the two hooks

The first draft of this design gave every kind a hashed **site**, resolved once
per lake tile into a `Poi.Field` threaded beside `Lakes.Tile`. Writing the plan
killed it. A site is resolved by probing the terrain at its centre, and for a
landform that probe has to run inside `Noise.height` — the hottest function in
generation, called for every column of every chunk heightmap, every apron column,
every column of a 128x128 lake pour, and once per tree and worm placement. A
probe there either calls `height` recursively or pays a second full noise stack
per candidate cell per column. Neither is affordable, and the field that would
have hidden the cost needed threading through twenty call sites, a seam apron of
`max_reach() + Chunk.apron()`, and its own agreement test.

The two layers want two different mechanisms, and each is already in the codebase.

**Landforms are masked fields.** A landform is placed the way dunes and glaciers
are placed: a **rarity mask** times a **shape**. One broad octave, thresholded
hard, says where the feature's country is — a few percent of the world. The
climate and river fields already computed inside `height` say whether that ground
suits it. A fine cell grid inside the country places the individual shapes.

Rarity comes from the threshold, not from a site list. There is nothing to
resolve, nothing to thread, and no seam to reconcile: `height` stays a pure
function of the world column, exactly as it is today. The cost is one `value2`
for the country mask plus a 3x3 of cell hashes for the shape grid, and the mask
is near zero almost everywhere, so the shape grid is skipped outright for most of
the world.

It also blends better than a site would. A butte field that straddles the edge of
the desert fades out across it instead of stopping at a circle.

**Structures are resolved sites** — `Trees.tree_at_cell` exactly. A hashed site
per cell, a suitability test that is free to call `Noise.height` (height carries
no structure term, so there is no recursion), stamped once per chunk over the
cells its footprint can reach. `plant_trees_go` already runs this loop per chunk
for every tree; a structure is the same loop with a bigger cell and a smaller
density.

| feature | layer | placement |
|---|---|---|
| canyon | landform | canyon country x the river mask, re-cut into a slot |
| delta | landform | coastal river mask, amplified and splayed |
| atoll | landform | reef mask over ocean, one ring per 256-block cell |
| buttes | landform | butte country x a 24-block mesa grid |
| cavern | structure | cell 128, reach 60, density 0.20 |
| giant tree | structure | cell 160, reach 26, density 0.12 |
| arch | structure | cell 96, reach 24, density 0.15 |

Nothing is threaded. No signature changes. `Lakes.Tile` is untouched, and so is
every water actor, test and call site that carries one.

## 3. What each layer may read

A landform's mask reads only what `height` has already computed at that column by
the time the mask runs — the warped point, the land height before POIs, the river
mask, the temperature — so the mask is a few multiplies on values in hand. It may
not sample terrain at another column, and it may not call `height`.

A structure's suitability probes `Noise.height`, `river_at` and `temperature` at
whatever columns it likes, because it runs once per site per chunk rather than
once per column, and because height cannot see structures.

`Noise.height_x4` is the SIMD benchmark and the lanes test's reference. It does
**not** carry the landform terms, and the lanes test is restated against a new
`Noise.height_base` — the height without them — so it cannot rot silently.

## 4. Blending and the off switch

Every landform term is multiplied by a mask that eases to zero, so terrain
outside is bit-identical to today and no seam is visible. Nothing snaps — the
same discipline as the biome's eased axes.

`CF_POI=0` zeroes every mask and every density, and the world must then be
**bit-identical to the world before this feature**: same frame-30 mesh hash.
`CF_POI_DENSITY=<pct>` scales every density so a shape can be found without
hunting, and `CF_POI_AT=<kind>` forces the kind at the spawn column.

## 5. The seven shapes

**Canyon system.** Mask: canyon country — one broad octave over 0.74 — where the
land is at least 20 above sea level and ruggedness at least 0.3. Where that mask
is up, the valley is re-cut as a slot: the river mask is sharpened through a hard
smoothstep so the walls are near-vertical rather than a V, then dropped by
`canyon_depth(a)` (about 45, deeper with age), and the wall is run through the
existing `terrace()` at a 6-block step so it steps down in benches. The floor
stays above the channel, so the existing valley-spring rule puts a spring at the
head and a brook runs the length of the slot. The strata pass gives the walls
their banding for nothing.

**Stone arch.** Site suitability: a river centreline column whose banks, probed at
±10 across the channel axis, both stand at least eight above the channel. The
stamp is a swept half-ellipse across the channel — 10-20 wide, 3-5 thick — in the
bank's own rock, read from the strata rule so an arch matches the cliff it grows
out of. Written into air only.

**Giant tree.** Site suitability: damp, temperate, above sea level, on ground flat to
within one block over the trunk's footprint. A trunk 4-6 across and 40-60 tall,
then six to ten branches leaving at hashed heights and angles. A branch is a
polyline of spheres — the same walk the cave worms take, writing logs instead of
clearing rock — so the twist comes free from the worm's hashed turn per step.
Leaf ellipsoids at the tips. This is the most expensive stamp; it gets a cap on
blocks written, enforced by a test.

**Crystal cavern.** Site suitability: at least ten of rock under the surface and
outside a lake basin (`Caves.basin_wall`, which exists because a carved basin
wall leaks a lake into a tunnel). Three to five ellipsoidal chambers of radius
10-16 at hashed offsets, joined by worm tunnels at a wider radius than the
ordinary ones. Crystals are a new block (id 69, plus a darker matrix at 70) that
`Light.emission` gives a level to, growing as spikes from chamber floors and
ceilings at hashed points on their surfaces. The block-light channel and the
glow shading already exist for the fungus; this is the first thing to make them
worth having underground. The worm roof rule holds: no chamber breaks the surface
before `sinkhole_age()`.

**Atoll.** Mask: reef country — a broad octave over its threshold, where the land
before POIs is at least eight below sea level. One ring per 256-block cell, its
centre hashed in the cell. The term raises a ring between `r0` and
`r0 + w` to two or three above sea level and lowers the inside to about
`sea - 24`, with the ring's radius wobbled by a noise term so it is not a
compass circle. **One gap in the ring is forced below sea level**, so the lagoon
is always connected to the sea and fills by the sea rule rather than depending on
the lake pour — which would cut it at a tile seam. The shore rule already gives
the reef sand. The deep blue is depth, not pigment — see the shader note in the
phase plan, which carries the water depth tint that makes 24 blocks of water look
like 24 blocks of water.

**Desert buttes.** Mask: butte country — one broad octave over 0.72 — times the
desert test the dunes already use (temperature at or above `dune_temp()`, well
clear of sea level, outside a river valley). Inside that country, one mesa per
24-block cell at a hashed density, each with a hashed radius 5-11 and a rise of
10-30, with the wall shaped over a single block so it is a cliff rather than a
slope, and the rise quantised to whole blocks so the top is flat. Overlapping
mesas take the **max**, not the sum, so a pair merges into one bench instead of
stacking to double height. Cliffs mean the surface rule shows rock faces and the
talus pass piles scree at the foot, both already true; the strata give the walls
their layers.

**Giant river and delta.** The hardest, and the one with an honest limit. Two
parts. The trunk: where a broad "great river" octave is up and the land is near
the coast, the river mask is amplified — widened by lowering `river_edge` locally
and deepened to cut to sea level — so it is 20-40 across and holds water its
whole length. The fan: at the
mouth the channel stops being one channel; the lobe is flattened to within two
blocks of sea level and a radially-splayed ridged noise cuts a handful of
distributaries across it. The sediment pass already puts mud and clay under a
floodplain, so the land between the channels reads as delta without new rules.
**The limit:** with no flow routing, the giant river is giant only where its mask
is up and fades to an ordinary river outside it. The mask's octave is far wider
than the 128-block window, so in play it reads as a big river; on a map of a
whole region it would not.

## 6. Verification

- `CF_POI=0` gives a bit-identical world: the frame-30 mesh hash does not move.
- `CF_POI_AT=<kind>` forces that kind at the spawn column, and
  `CF_POI_DENSITY=<pct>` scales every density, so each shape can be walked
  without hunting for one.
- A `poi` line at startup lists the sites in the window with kind and centre.
- Two chunks sharing a POI's footprint write identical blocks in the overlap —
  the `trees_test` shape, run for every kind.
- `height` is unchanged with `CF_POI=0`, over a grid of columns spanning a tile
  seam, and a landform column's height is the same whichever chunk computes it.
- No landform mask reaches another column: checked by reading, and pinned by the
  cross-chunk agreement test, which a remote probe would break.
- Chunk generation time with POIs off (must be unchanged) and on, and the
  eight-chunk window shift, which is ~3 ms today. A landform costs one octave
  plus a 3x3 of hashes per column; a structure costs a per-chunk cell scan and
  its stamp.
- Screenshots of each shape at a forced seed, in `docs/`.

## 7. Landing order

Seven features is three pieces of work, not one.

1. **The system, plus buttes and the arch.** The grids, sites, suitability, the
   field and its threading, both hooks, the `CF_POI*` knobs, and the
   off-is-identical proof — landed against the simplest landform and the
   simplest structure. Measure here; every later kind is then only a shape.
2. **Crystal cavern and giant tree.** Two new blocks and an emission level; the
   worm walk reused additively for branches.
3. **Canyon, atoll, delta.** The three that argue with rivers, springs, lakes and
   sediment. One at a time, each with its own measurement, canyon first.

## 8. Not in scope

Ruins, dungeons, loot or anything built rather than grown. Markers on the map or
compass beyond the debug line. Real flow routing for river networks. Any change
to the biome table — a canyon is a landform, not a biome. POIs that need a basin
to fill across a lake-tile seam.

## 9. As built, phase 1 (2026-09-06)

The system, **desert buttes** and the **stone arch**. See RESULTS for the
measurements; the parts that change what this document says:

- Landform terms live in `CubeForge.Noise` beside the dune and glacier masks,
  because that is what they are. `CubeForge.Poi` holds the structures only.
  `Noise.height_base` is the field terrain; `Noise.height` adds the landforms.
- A butte's rise is an **absolute top level** hashed per cell (74..100), not a
  rise added to the ground: a constant rise tilts the top with the slope under
  it. A cell whose ground is within `butte_min_rise()` of its top holds nothing.
- Butte country's threshold is 0.62, not 0.70, and the density is
  `clamp01(2 * country * desert)` -- doubled before clamping, so the inside of a
  field is at full density and only its fringe thins. At 0.70 with an unscaled
  density a field was one or two mesas.
- **The arch's span is read off the terrain, not hashed**, and each cell tries
  sixteen candidate columns rather than one. With a hashed span and one
  candidate there were no arches at all in 160 cells.
- `CF_POI_AT` was not built: `CF_POI` and `CF_POI_DENSITY` are, and the `poi:`
  line under `CF_TERRAIN_STATS` reports butte columns and arch sites with the
  first of each, which is what actually got used.
- Neither shape was confirmed visually; both are confirmed by cross-section,
  by the tests, and by the world hash. See RESULTS.

## 10. The template (2026-09-06, after the arch was written twice)

The arch shipped twice and was wrong both times, because every kind was its own
algorithm arrived at by trying it, taking a screenshot, and trying again. The
screenshots were the worst part: a rare feature in a 128-block window, a camera
whose yaw is fixed by the window manager, and four rounds of blind sweeps that
landed on a peak, in a lake and under water. `CubeForge.Poi` is now a template
instead, and the iteration happens somewhere it can be seen.

**One distinction decides almost everything: scatter or landmark.**

*Scatter* kinds are many and small, on a small cell grid. Their rules must be
read at the **column** being evaluated, because a site probe would run for nine
cells on every column and no small-cell kind can afford that. So a scatter fades
at the edge of its country rather than stopping, and its shapes may be clipped.
Buttes are a scatter.

*Landmark* kinds are few and large. Their rules are read once at the **site**,
so every column agrees about whether one exists and how big it is -- which is
exactly what lets a landmark be **carved** as well as raised. It is whole or
absent, never clipped. The arch is a landmark; so are the crystal cavern, the
giant tree and the atoll.

**Shape is separated from placement.** `shape_at` is the shape alone; `rise_at`
is the shape once placement has agreed there is one. The preview draws the
shape, which is why it works at all -- the first run of it drew an empty sky,
because it had asked whether an instance happened to be placed at the sample
cell.

**The rules, checked for every kind by one test** (`Poi.rule_ok`):

1. `reach <= cell`, so two of a kind never overlap and a column looks only at
   the 3x3 around it.
2. An opening is strictly inside the mass carrying it: twice its half-width
   under the flat top, and a band of at least `roof()` above it. **This is the
   rule the first arch broke** -- an opening as tall as the rise is a lintel.
3. A level top comes from an absolute top level, never a rise added to the
   ground, which tilts with the slope under it.
4. A vertical wall uses `prof_hard`; anything meant to blend uses `prof_flat`
   or `prof_round`. There is no fifth, ad-hoc option, and a one-block ramp --
   which is what the first butte rim was -- is none of them.
5. A landmark's site rules read `height_base`, never `height`.

Derived parameters exist so rule 2 holds by construction: an arch's opening is
45% of its half-length and its rise less a roof, so no knob can make the opening
wider than the mass over it.

**`CF_POI_PREVIEW=<kind>`** draws the kind's table and two orthographic sections
of it standing on flat ground. Every iteration on a shape belongs there.

```
poi preview: arch  (landmark)
  cell 160  density 50%  country over 56% at 1/285 blocks  reach 26
  len 31  wide 6  rise 30  top 0  opening 26x20  rules ok
   30              ###############
   25           #####################
   20           ##########.##########
   15         ########.........########
   10         #######...........#######
    5        #######.............#######
    0 ##############.............##############
```

Adding a kind is now: a row in the placement table, a row in the parameter
table, a case in `shape_at` built from the shared profiles, and -- if it needs
one -- a case in the carve. The invariants and the preview come with it.

## 11. As built, phase 2 (2026-09-06)

The crystal cavern and the giant tree. Both are landmarks with **no height
term**; the template gained a volume op (`vol_carve` / `vol_stamp` / `vol_none`)
orthogonal to scatter/landmark, and `raises(k)` keeps volume-only kinds out of
the per-column scan.

- One function, `vol_block`, says what a kind does to a voxel, and the generator
  and the preview both call it. This is the rule that keeps the preview
  believable.
- **A carve owns the volume it opens**; a stamp owns nothing. The crystals
  lining a chamber replace the rock the carve just took; a tree writes only into
  air. Treating both the same left every cavern unlit.
- The giant tree is a live oak, not a conifer: wider than it is tall, short
  thick trunk, boughs that dip at the elbow before lifting. The first version
  was a 50-block pole with blobs around it.
- Crystals are **micro-voxel models** (`Model.t_crystal`), four blade variants
  hashed on a coarse grid so a patch leans together. A cube per crystal read as
  a stack of boxes.
- The cavern's `p_depth` is 22..34 against a `min_ground` of sea + 24, so the
  roof rule holds by construction rather than by luck.

The crystals are two things, because one mechanism cannot be both: the fine
growth on the shell is **micro-voxel blades** (a cube read as a box), and the
long shafts are **geometry** — `crystal_beam`, id 70, laid down as segments
through the chamber, because a model can never be longer than the cell it is
drawn in. Two beams per chamber, three to five blocks across, driven into the
rock at both ends.

## 12. As built, phase 3a: canyon and atoll (2026-09-07)

- A third class, **field**: no cells, no site. Evaluated per column from the
  river mask, its raw crease and the ruggedness, which `height` now hands to
  `height_at`. The canyon is a field; so will the delta be.
- `kind_go` and `height_at` combined kinds with `max`, which silently discards
  any cut. `pick` lets a nonzero beat zero. The atoll was the first kind to go
  negative and the first to find this.
- The atoll is the first kind with a ground **ceiling**: `max_ground` and
  `cover_hi`. It only has to start under water; its levels are absolute. An
  apron cut to sea - 6 gives the ring water on a shelf. In this terrain it is
  rare (two seeds in 140) and reads as a crater lake as often as a reef,
  because the world has shelf rather than ocean.
- `CF_POI_TRANSECT=<z>` reads a landform across the world in one line, and
  `CF_POI_GATE=1` says which placement rule refuses a kind. Both found bugs
  that screenshots had not.

## 13. As built, phase 3b: the delta (2026-09-07)

- The delta is a field like the canyon, split by height into a trunk (the
  crease against edge 0.72, bed cut below the sea) and a fan (flattened to
  sea + 1 and braided), the fan gated on the trunk's own width so it is the
  mouth of that channel and not the whole coast.
- The crease is a band, not a line: the delta's edge was set by measuring its
  width by transect (45 -> 78 -> 20-40 columns at 0.55 / 0.60 / 0.72), not by
  guessing.
- All seven kinds from the request are in. The census over forty seeds: buttes
  and canyons regional, arches and giant trees one in eight, caverns one in
  five, deltas nine in forty, atolls two in a hundred and forty.

## 14. Open ocean (2026-09-07)

The atoll was right and the world was wrong: it had shelf, not sea. `Noise`
gained an ocean regime -- a broad octave under which the land is pulled to an
abyssal floor, full up to sea + 12 and fading above so high ground is an island
-- and the atoll's country became the ocean's own mask. Five seeds in 140 have
an atoll, in open water, ring and lagoon and gap all readable from above. Three
pinned terrain tests broke on the way, each a hunt asking a simpler question
than the generator; RESULTS has them.
