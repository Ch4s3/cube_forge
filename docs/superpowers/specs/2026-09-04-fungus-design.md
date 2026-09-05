# Fungus — mycelium networks, glowing fruit, spores — design

The world has systems but no loop: nothing the player does asks them to come
back tomorrow. This adds the first one. Fungus lives as a per-column
**mycelium field** beside the biome field, shows as combined surface blocks,
glows at night through a new block-light channel, fruits mushrooms of three
sizes, and is spread by the player planting **spores** harvested from fruit.

The frame is territory with a resource engine underneath: where the network
reaches, night is lit; what it fruits funds spreading it further. Spread is
bounded per planting and species compete for ground by climate fitness, so
both planting and terraforming are decisions.

Depends on: the biome field and its retexture queue
(`2026-09-04-biomes-design.md`), the tree placement and felling pattern
(`2026-09-03-vegetation-design.md`), and the lighting flood
(`2026-09-03-lighting-design.md`). Landing order in §9.

## 1. The loop

Nightly: the world is dark and glowing patches show where fungus lives. The
player walks to one, harvests mushrooms for spores, plants them on ground
that suits the species, and returns over the next in-game days to watch the
patch creep, glow, and fruit.

Long term: the player joins patches into a network, pushes it into contested
ground by planting and by changing the climate (a canal wets a desert edge),
and collects giant fruit as landmarks and as the game's only portable light
block.

Spores are the currency. A planting seeds a bounded patch, so covering ground
costs spores per area. Fruiting returns spores, so a healthy network funds its
own expansion and one planted outside its band withers before it fruits.

## 2. The mycelium field

Two per-column arrays beside the biome field, recomputed whole on the same
`tick_period()` boundary, for the same reason: 16,384 columns is small.

- **`species`** — `NativeU8Arr`, 0 for none, otherwise a species id (§3).
- **`vigour`** — `NativeU8Arr`, 0..255, how established the column is.

Per tick, for every column holding a species:

- **Fitness** ∈ [0, 1] is how well the column's *eased* temperature and
  moisture (the biome field's `temperature_actual` / `moisture_actual`) sit
  inside the species' bands: 1 in the core, falling linearly to 0 at the
  band edge. Reading the eased axes means a canal changes fitness with the
  same one-to-two-day lag it gives biomes.
- **Vigour** eases toward `255 * fitness` by at most `CF_MYC_RATE` per tick.
  At fitness 0 it decays to 0 and the column empties, so a planting in the
  wrong place fades in a couple of days.
- **Spread.** A column with vigour ≥ `spread_vigour()` tries to claim each of
  its eight neighbours:
  - *empty neighbour*: claimed if this species' fitness there ≥
    `claim_floor()`. It starts at `seed_vigour()`.
  - *occupied neighbour*: **contested**. The claim succeeds only if this
    species' fitness there exceeds the incumbent's by `contest_margin()`,
    and then the incumbent's vigour drops one step per tick; the column
    flips species only when it reaches 0. Borders drift, never flip.
- **Reach.** A third array, **`reach`** (`NativeU8Arr`), decrements by one
  per hop from the planting. A column at reach 0 never spreads. This bounds
  a planting to roughly `plant_reach()` = 8 columns of radius. A patch grows
  further only when a new planting lands inside it or fruit reseeds it (§5).

A **hold counter** gates every species change, as the biome field's does:
a column flips only after `hold_ticks()` consecutive ticks of the same
verdict. There is **no feedback** from the field into the climate axes in
this version, so the biome spec's no-oscillation argument holds unchanged.

Cost: 16k columns × 8 neighbour reads, an order of magnitude below the
biome distance BFS.

## 3. Species

A species is a **row in a table**: name, temperature band, moisture band,
fruit tier, glow level, cap colour, spore yield. Adding one is data.

| id | species | temp | moisture | tier | glow | home ground |
|---|---|---|---|---|---|---|
| 1 | Frostcap | cold | any | small | dim | tundra, alpine |
| 2 | Pinewart | cold–temperate | damp | medium | none | taiga |
| 3 | Meadowbell | temperate | dry–mid | small | none | grassland |
| 4 | Lanterncap | temperate | damp | medium | bright | forest |
| 5 | Marshlight | temperate–hot | damp | giant | bright | wetland |
| 6 | Sunshelf | hot | dry | giant | none | desert |

Bands overlap at their edges on purpose: Lanterncap and Marshlight contest
warm forest and cool wetland, and a canal through the desert edge lets
Marshlight walk into Sunshelf's ground. Two bright species, two dim-or-none,
so night reads as "where the good stuff is" rather than everything glowing.

Glow attaches to the **species**, not the tier: glowing mycelium emits a low
level from the surface and its fruit emits more (§6). Band edges are the
same thresholds `Biome.classify` uses, so a species' home ground is exactly
the biomes named, with the overlap falling on the eased transitional columns.

## 4. Blocks and textures

**Seven combined block ids**, one per surface base: mycelium over grass,
dirt, sand, snow, gravel, clay, and stone. The id records the base so that
digging the block, or the network dying, restores the right surface. The
**species is not in the id** — it comes from the field at mesh time.

`layer_of(id, dir)` already picks a texture layer per block and face; for a
mycelium id it also reads the column's species and picks the layer for that
(base, species) pair. Layers are generated at startup: seven bases × six
species = 42, each the base's procedural generator with a threading pass in
the species' colour laid over it. A new species costs seven layers and zero
block ids; the vertex layout is untouched. When a column's species changes,
its chunk goes dirty through the path retexturing already uses.

Surface migration runs through the **existing retexture queue and budget**,
both directions: a claimed column swaps to its combined block, an emptied one
swaps back to the base. Mycelium blocks are diggable and drop the base block
plus a `dig_spore_chance()` of one spore.

**Fruit blocks**, per species via layers: `mushroom_small` (one cutout block,
foliage mesh category), `mushroom_stem`, `mushroom_cap` (both opaque).
Stem and cap are harvestable and placeable building material. A placed cap of
a glowing species is a **light block** — the only portable light in the game,
which is the resource half of the design. Placed fruit blocks are inert: they
neither fruit nor spread.

## 5. Fruiting

Fruit follows the tree pattern: a **fruit cell** grid and a pure function of
(cell, species, seed) deciding where in the cell a body stands and its exact
shape, so a giant straddling a chunk seam is the same giant from either side.
Cell size by tier: small 3×3, medium 6×6, giant 16×16.

Each tick, for each cell whose canonical column has vigour ≥
`fruit_vigour()`, roll against the species' rate. Growth places the body
through the biome queue, which already bounds edits per tick. Shapes:

- **small** — one cutout block on the surface.
- **medium** — stem 2–3 high, cap a 3×3 slab one block above the stem top.
- **giant** — stem 5–9 high; cap a flat 7×7 to 9×9 disc, or a two-layer dome
  for some species. Walkable on top.

Bodies **decay** when their canonical column loses the species, using the
shape function for extent, exactly as tree felling uses
`Trees.block_of_tree`. Breaking the small block or any stem block fells the
whole body. Yield by tier: small 1 spore, medium 3, giant 8, plus the stem
and cap blocks for medium and giant. A felled body also **reseeds**: its
canonical column's reach resets to `plant_reach()`, which is how a mature
patch keeps growing without the player replanting.

Rates: a mature patch fruits a small body about once a day and a giant every
several days. `CF_FRUIT_RATE` scales them for inspection.

## 6. Block light

A **second light byte per voxel** in the chunk beside skylight, so
`Chunk` gains a third `NativeU8Arr`. The flood is the skylight BFS with the
same add/remove pair, seeded from emissive blocks rather than the sky:

| emitter | level |
|---|---|
| glowing mycelium surface block | 4 |
| small glowing fruit | 6 |
| glowing cap block (medium) | 10 |
| glowing cap block (giant) | 12 |

Levels drop by `1 + opacity(n)` per voxel, so a cap at 10 lights a radius of
about nine and mycelium at 4 gives a soft floor glow. Placing and removing an
emitter goes through the existing edit hook, so incremental relight is
shared and tested once. Emissive levels are a function of block id plus, for
mycelium, the column species.

The mesher packs **sky and block light as two nibbles** into the existing
per-vertex light float; the layout stays at nine floats. The shader unpacks,
scales sky by daylight, and takes `max(sky * daylight, block)`, so glow is
invisible at noon and full at midnight. Glow does not cast in the DDA shadow
trace. Block light varies per vertex and so limits greedy merge runs the way
skylight already does — the accepted cost of the lighting design.

*As built (phase 1, 2026-09-05):* the field is a world-flat `NativeU8Arr` on
`World` beside skylight, not a third array in `Chunk` -- the skylight field
already lives there for the reason `light.march` gives (a per-chunk array is
shared and cannot be written in place). Propagation is the level-synchronous
sweep, not a BFS (GAPS G63), with a scan bound `max_level()` layers above the
sky floor so block light can climb into open air. The vertex packing is
`2 * round(blk * 255) + sky` in the existing shade float rather than two
nibbles, so overlay vertices needed no change. Emission is a function of the
block id alone for now; the glow cap (id 22) is a placeholder emitter at 10.
Measurements in `RESULTS.md`.

## 7. Items and interaction

- **Spores** — one item per species, stacking. Sources: harvesting fruit,
  and a chance from digging mycelium. Wild patches exist at generation
  (one seeded planting per suitable fruit cell at low density), so the first
  spores come from exploring at night.
- **Planting** — select spores in the hotbar and use them on a surface block.
  The column gets the species at `seed_vigour()` with full reach; the
  retexture queue paints it over the next few ticks. Planting on ground held
  by another species is allowed, starts a contest, and costs the same one
  spore.
- **Fitness readout** — the existing target readout gains the species and
  fitness of the looked-at column. Without it a player cannot tell why a
  planting withered.
- **Fruit blocks** in the hotbar for building, as in §4.

## 8. Map view

The map gains a mycelium overlay under `CF_MYC_MAP=1`: species colour,
brightness by vigour. As with biomes, this is the primary verification
surface for the field phase, and it is also the player's territory map once
exposed in the menu.

## 9. Landing order

1. **Block light channel** — storage, flood, incremental relight, vertex
   packing, shader; verified with a temporary emissive block before any
   fungus exists. Highest risk, most delicate code: first and alone.
2. **The field** — species table, fitness, vigour, spread, contest, reach,
   hold, map overlay. Mutates nothing in the world; observable via the map.
3. **Mycelium blocks** — seven ids, layers, migration both ways, glowing
   mycelium as a light seed.
4. **Spores and planting** — items, the use action, wild patches at
   generation, fitness readout. The loop closes here even with no fruit.
5. **Small and medium fruit** — shapes, growth, decay, harvest, reseed.
6. **Giant fruit** — its own phase, as tree decay was.

Phases 1 and 2 are independent and can be built in parallel.

## 10. Verification

- **Fitness, spread and contest are pure functions** of the axes and are
  unit-tested across bands, including both sides of every overlap and the
  hold counter.
- **Reach**: a single planting on ideal ground covers at most the expected
  radius, and no more.
- **Contest**: two species planted on ground inside both bands settle to the
  fitter one, and each column's species changes at most once.
- **Wither**: a planting outside its band empties within N ticks and its
  surface blocks return to base.
- **Canal**: extend the biome canal test — a species that cannot live beside
  the desert trench before it is dug can afterwards.
- **Block light**: placing then removing an emitter leaves the light field
  equal to a full reflood, the property skylight already tests. A night
  frame dump of a glowing patch (with `CF_NOMOUSE=1`, `CF_TIME` pinned).
- **Seams**: a fruit body straddling a chunk boundary is identical from
  both chunks.
- **Budget**: no tick migrates more than the retexture budget; the frame
  stays under 12 ms with a fully grown world; recorded in `RESULTS.md`.

## 11. Risks

- **Block light cost**, memory (4 MB) and relight time. Low glow levels keep
  floods small; phase 1 is measured before anything depends on it.
- **Layer count** grows as species × surfaces. 42 is fine; a roster of
  twenty is 140, at which point the threading pass should become a separate
  overlay layer blended in the shader rather than baked per pair.
- **Tuning** of spread rate, reach and fruit rate is guesswork until the map
  overlay exists, which is why phase 2 ships with it.
- **Save/load**: species, vigour and reach are world state that cannot be
  rebuilt from the voxels. Same note the biome spec carries.

## Out of scope

Climate feedback from the network (a later species trait, needs a damping
argument), player buffs on network, signalling or travel along the network,
mushrooms as food, underground mycelium (the field is surface-only).
