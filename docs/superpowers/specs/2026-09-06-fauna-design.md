# Birds and fish — design (2026-09-06)

## The problem

The terraforming loop closed last session: survey a place, learn what it wants,
dig, watch the climate ease and the ground reclassify. It works, and it is
still a loop about *dirt*. The evidence that you changed something is a
retextured surface and a number in a panel.

Animals are the missing half of that. A lake you dug is an abstraction until
something lives in it. The design goal here is not "add wildlife" — it is to
make fauna the **readable consequence of terraforming**, so the loop pays out in
something that moves.

Which fixes the scope. Animals are not a resource: you cannot hunt, catch or
eat them. They are evidence, and the survey is the instrument that reads them.

## The loop this joins

    explore -> scan a species -> survey a place -> reshape it -> it is colonised

The last arrow is new. Everything before it exists.

## What the world already gives us for free

This design is deliberately parasitic on machinery that is built, tested and
already paid for in the frame budget:

| need | what already answers it |
|---|---|
| small voxel bodies | `Model.march` — 8x8x8 grids, greedy-meshed once into templates |
| drawing them anywhere | `cf_f32_stamp` — the shim copies a template with an offset and a shade |
| a thing drawn per frame outside the chunk mesh | `Marker` (slot 250), `Precip` (slot 249) |
| habitat inputs | `Biome.temp_of` / `moist_of` / `water_dist_at` / `water_reach_at`, and the heightmap |
| day and night | `sun_level_at`, `moon_level`, `CF_DAY` |
| a group that owns its own state and replies in packed Ints | the `WaterChunk` actor pool |
| a catalogue gated on discovery | the survey's `known` bitmask and panel |
| sparse lists in a save | the header's `cached` and `hashes` fields |

Nothing here needs a new subsystem. It needs one new module, one new actor, one
new mesh slot, and about ten lines of C.

## Rendering

### Bodies

Animals are `Model` templates, built the same procedural way mushrooms and
bushes are. A body is one grid per **pose**; a species has three or four poses
(wings up / mid / down; tail left / centre / right). No skeletons, no skinning —
at this resolution a flip-book reads correctly and costs one template lookup.

**Animals are 16 cells a side, not the 8 a block model uses.** Eight was built
first and every animal read as a stack of slabs. The reason is not subtlety: at
8, an eighth of a block is the *thinnest thing that exists*, so a wing, a fin, a
beak and a leg all weigh the same as the body they hang off and nothing can be
slender. At 16 a wing is one cell against a body of four, which is the
difference between a bird and a brick with flaps.

`Model`'s greedy pass is therefore parameterised on the grid side, with the
8-cube case kept as the entry point the block models use — verified by the mesh
hash, which is byte-identical across the change (`413448066`).

**A species does not get a hand-built grid.** It gets a row — how long, how
wide, how far the wings reach, whether it flies — and one of two generators
(`bird`, `fish`) builds every pose from it. This is not only shorter than
sixteen hand-placed skeletons. Every part is positioned against the *body's own
extent*, so attachment is a property of the generator rather than of each
species being typed in correctly.

That distinction was earned rather than assumed. The first hand-built bird put
its wings at z 1..2 beside a body at z 3..4 — two cells clear of it, touching
nothing — and rendered as a bird with two slabs floating near it. Sixteen
species times three poses is forty-eight chances to make that mistake once.

The invariant is tested rather than trusted: a flood fill from one cell must
reach every filled cell of every pose. Note what that is *not* — checking that
each filled cell touches another would have **passed** the broken bird, because
the detached wing was itself a solid slab whose cells touched each other
perfectly well. Only reaching every cell from a single seed catches a part that
is whole and in the wrong place. It has already earned its keep a second time,
catching a wing tip that stepped in both z and y at once and so met the wing
along an edge with no shared face.

### Facing

`cf_f32_stamp` translates and shades; it does not rotate. Palm fronds solved
this by baking eight directions, and that is the wrong answer here: a bird
banking through eight yaw steps snaps, and `species x poses x 8` multiplies the
table by an order of magnitude.

**Add `cf_f32_stamp_xf` to the shim** — the same copy loop, with a yaw rotation
about the model centre and a uniform scale:

    x' = cx + (x - 0.5) * c * s - (z - 0.5) * sn * s + dx

Two multiply-adds per vertex on a loop that is already memory-bound. This buys
continuous facing *and* free size variation, which is what makes "fish of
multiple sizes" a table column rather than a new set of grids. It is the same
reasoning that put the precipitation and spray loops in C: the per-vertex work
belongs there, the policy belongs in March.

### The draw

One mesh slot, rebuilt each frame from the live transforms, drawn in the cutout
pass. Cost is `visible animals x template floats`; cull by distance and cap the
visible count, exactly as `Precip` turns intensity down by simulating fewer
particles rather than cheaper ones.

## The species table

`Fauna.march`, shaped like `Species.march`: one row per species, every property
a function of the id. A species is a row here plus its pose grids.

Niches are keyed on **depth and body size** for fish, and on **biome and
temperature** for birds — every one of them a number the world already
maintains, so habitat responds to digging without a single new field.

### Fish — depth is the axis

Depth is `sea_level() - height`, so it is the heightmap, so a shovel changes it.

| species | size | niche | depth | active |
|---|---|---|---|---|
| Sunfin | 0.35 | ponds and lakes | 1-4 | day |
| Frostshiner | 0.4 | cold lakes (tundra, taiga) | 1-5 | day |
| Reedcarp | 0.8 | lake and wetland margins | 2-6 | dawn/dusk |
| Shoalback | 0.45 | shallow sea | 3-10 | day |
| Kelpjaw | 0.9 | shallow sea off a beach | 4-12 | day |
| Glasseel | 1.1 | deep water | 14+ | night |
| Deepmaw | 1.6 | deep water, near-solitary | 18+ | night |

Small bodies matter: `Biome.water_reach_at` already distinguishes a pond from a
sea, so "lake fish" and "sea fish" is a distinction the field can make.

### Birds — biome is the axis

| species | size | biome | flight | active |
|---|---|---|---|---|
| Grasspipit | 0.4 | grassland | flocking, low | day |
| Cinderfinch | 0.35 | desert, oasis | flocking, low | dawn/dusk |
| Pinecrest | 0.6 | taiga, forest | canopy | day |
| Frostgull | 0.7 | beach, tundra coast | coastal, soars | day |
| Marshheron | 1.2 | wetland | wades the shallows | dawn/dusk |
| Ridgehawk | 1.1 | alpine | high, solitary soarer | day |
| Duskowl | 0.8 | forest | silent, low | **night** |
| Stoneratite | 2.2 | grassland, desert | **flightless** | day |
| Mossmoa | 2.6 | forest, taiga | **flightless** | dawn/dusk |

Two deliberate bridges: the **Marshheron** stands in shallow water, so it reads
as a bird that a *fish* habitat produced; the **Duskowl** shares the forest with
the Pinecrest and takes over at dusk, so the day/night cycle is legible as a
shift change rather than a dimmer.

The flightless pair are the reward for a mature biome — big, slow, ground-bound
silhouettes you notice from a distance.

## Actors: one per flock

A `Flock` actor owns 5-20 animals of one species: their positions, velocities,
pose phase, and one shared mode (feeding, roosting, fleeing, drifting). Group
behaviour is then free, because the group is one unit of state.

### The G44 problem, and the terraforming trap in it

Messages cannot carry native arrays. `WaterChunk` solves this by regenerating
its chunk from `(cx, cz, seed)` — and **that answer is wrong here**. Regenerated
terrain is terrain as it was *generated*, and the whole point of this feature is
that the player has changed it. A flock navigating generated ground would fly
into a hill the player built and ignore the lake they dug.

So the flock is *told*, each tick, in scalars: a small height sample around its
centre (a 5x5 patch, 25 Ints), the water surface level, the biome id. That is
enough for every behaviour below, it costs one small list per flock per tick,
and it is correct against edits by construction.

### Cadence

Flocks tick on their own phase slot in `tick_period()`, alongside water, myc and
veg — so AI runs at roughly 6Hz and the renderer **interpolates** between the
last two tick transforms. Motion stays smooth at a tenth of the cost, which is
the same trade the biome sweep already makes.

### Behaviour

Rudimentary on purpose: a target point, a steering force toward it, separation
from flockmates, and avoidance of solid ground. The mode picks the target.

- **birds** — feed over their biome by day at low altitude; soarers circle on a
  wide slow arc; at dusk diurnal flocks descend to the nearest canopy or ridge
  and roost (perched pose, no flapping) until dawn. Nocturnal flocks invert it.
- **flightless birds** — the same steering with the altitude term removed and
  the ground clamp made hard: they walk.
- **fish** — hold their depth band, school tightly, and drift; crepuscular
  species rise toward the shallows at dawn and dusk and sink at midday. A fish
  whose water is removed is a fish whose flock dies — which is the terraforming
  loop running backwards, and should be visible.
- **all** — flee the player at close range, which is what makes them feel alive
  and is also what makes scanning a skill rather than a formality.

## Populations: persistent, and driven by habitat

### The grain

One record per **chunk** per species: a count 0-15. Sparse — only non-empty
cells exist.

### The tick

Each cell has a **carrying capacity** from the species row read against the
world: depth and water reach for fish, biome and temperature for birds. The
count eases toward capacity slowly, the way the climate axes ease.

That single rule is the whole feature:

- dig a pond in grassland -> depth 1-4 appears -> Sunfin capacity rises from 0
  -> over minutes, a school exists that did not
- flood a basin to depth 20 -> Glasseel become possible, and only at night
- grow a forest (which the mycelium and vegetation systems already do) ->
  Pinecrest capacity rises, then Mossmoa
- drain the pond -> capacity falls to zero -> the school thins and goes

### Off-window cells

The world streams, so most cells are not loaded. A cell stores `last_ticked`
and integrates lazily on revisit — capacity is recomputed once and the count
walks toward it by the elapsed tick count, capped. Populations therefore change
while you are away without anything simulating while you are away.

### Saving

A header field, sparsely packed, in the same shape as `cached` and `hashes`:
`(cell x, cell z, species, count)` per entry, non-empty cells only.

## The survey, extended

### A second bitmask

`known` is six fungus species in one Int. Fauna gets its **own** header field
rather than crowding that one: sixteen species and room to grow, and the two
catalogues stay independently readable.

### Scanning an animal

Aim and press `X` — the key already means scan. The ray test is a sphere check
along the aim ray over the animals within ~40 blocks, run only while the survey
is open or `X` is pressed, so it costs nothing the rest of the time.

Unknown, it reads `UNKNOWN SPECIES  SCAN`. Known, it names the animal and its
niche. Fleeing animals mean you have to approach carefully — the scan is the
gameplay.

### The habitat report

This is the line that closes the loop. When the survey is open and you are not
aiming at an animal, it reports, for the column under the reticle, which
**known** species could live here and what the nearest miss needs — the exact
shape of the existing `shortfall_go`:

    WATER 0  DEPTH 3   SUNFIN VIABLE
    DEPTH 3           GLASSEEL NEEDS DEEPER WATER

So the instrument that told you what a place *is* now also tells you what could
*live* there, and what to dig to get it. The player learns the table by
surveying, not by being told it.

## Build order

Each slice is independently verifiable and independently useful.

1. **Bodies on screen.** `cf_f32_stamp_xf`, the pose templates for two species,
   the mesh slot, and a hardcoded circling flock. Verifies rendering and yaw
   alone. Oracle: a vertex-count and position hash for a pinned transform.
2. **The flock actor.** *(done)* Steering, the height-sample message, ground
   avoidance, interpolation between ticks. Two species, spawned by hand.
   Oracle: a deterministic position hash after N ticks from a fixed seed, plus
   behaviour tests (cruise height, home, separation, flee, roost, fish stay
   wet, nothing ends a tick inside terrain) — each shown to fail when the
   behaviour it names is disabled.

   Two things the plan got wrong and the build corrected. The flock's inputs
   cannot ride on the `Actor.call` request: arguments there arrive as zeros
   (GAPS G84), so they go by `send` and the call is nullary, as every other
   actor here already does. And "ground" is two different questions —
   `Biome.height_of` is the surface *including water*, right for a bird and
   exactly wrong for a fish, whose floor would sit above its own ceiling and
   push it out of the lake. Fish get the bed from a bounded `surface_y` walk
   instead.
3. **The species table and the full roster.** *(done)* Eighteen species, poses
   and colours, generated from a data table rather than hand-written: a species
   is a row, and the March if-chains are emitted from it, so eighteen entries
   cannot drift out of step across a dozen functions.

   Two species were added on top of the plan — a **Brinewaddle** (upright,
   flippers, legs: a penguin is one branch in the head placement plus rows it
   already had) and a **Bladefin** (a swept dorsal fin eight cells tall). The
   **Mossmoa** grew a crest and a hooked beak, and the **Glasseel** got long.

   The grid went 16 -> 32 -> 64 cells a side, which is what lets a wing be a
   sixtieth of the body it hangs off rather than a fifteenth. That cost 2.4
   seconds of startup until the greedy pass was restricted to each shape's
   bounding box; it is now ~0.3 s, and the world mesh hash is byte-identical
   across the change.
4. **Populations.** *(done)* Capacity from habitat, the ease, the registry,
   save/load, and flocks placed and retired from the counts. The registry is
   dense over the window rather than sparse over the world -- a chunk that
   leaves the window is ARCHIVED under its world key with the tick it left at,
   and restored on return caught up by the visits it missed -- so a school you
   watched fill in twenty chunks away is there when you come back, grown or
   thinned by the time between. The save carries every record keyed by world
   chunk, so a load does not care where the window was. Oracle: a
   flat world with a 6x6 pond dug into it holds two Sunfin and no Deepmaw;
   fill the pond and the count goes to nothing, one a tick. Flocks are given
   only to chunks within two of the player: populations exist everywhere in
   the window, flocks are the visible sample.
5. **Survey integration.** *(done)* The fauna bitmask (its own header field,
   `fauna`), the animal ray test (a sphere test over every drawn animal, run
   only while the survey is open or the scan key is down), the habitat report,
   and the save round-trip. The scan key goes to the animal under the reticle
   when there is one and to the ground otherwise. Aimed at a known animal the
   panel names it and its niche -- and its season, if it has one; aimed at the
   ground it says which known species this chunk would hold and what the
   nearest miss needs, in terms a shovel can act on: a depth, a pond, open
   water, a biome, or a season to wait for.
6. **Day and night.** *(done)* Roosting and the nocturnal shift were live
   from slice 2; what this added is the roosting POSE -- a fourth template per
   bird with the wings folded as a one-cell shell against each flank, worn by
   a still animal whenever its flock's mode is roost (a bird put up by the
   player flaps like any other). A fish's fourth pose is its first. With it,
   feathered trailing edges on the inner wing and a hock on the ratites.
   Crepuscular depth changes for fish are not built.

## Seasons, and the migrants to come

A migrant's capacity is zero outside its season, so its counts drain when the
season ends and refill when it returns -- and because the registry persists,
it returns to the same water. A year is eight days; the Frostgull holds the
coast for the second half of it and the Marshheron the wetland for the first,
so the two are never here together. That is migration as the population sees
it. The flocks crossing the sky do it too: a flock whose season is over enters
a `depart` mode -- climb thirty above its cruise, push the way its species
leaves at fleeing speed, no pull home -- and is retired once its lead is
forty-four blocks out. An arriving flock needs no mode at all: it is placed
forty-four blocks out and twenty-five up, from the way it leaves, and the pull
home and the altitude spring it already has fly it in. The gull leaves north
up the coast, the heron south, and each arrives from where it went.

## Risks worth naming now

- **Frame budget.** The standing constraint, and the first system that adds
  per-frame *rendering* work rather than per-tick simulation work — none of the
  phase-slot tricks from the performance work apply to drawing.
  **Measured in slice 1 rather than deferred**, interleaved A/B, three runs each,
  150 bodies stamped and uploaded every frame:

  | bodies | worst frame | frame rate |
  |---|---|---|
  | none (baseline) | 5.4-6.6 ms | 214-225 fps |
  | 150 at 8 cells a side | 6.7-7.0 ms | 207-218 fps |
  | 150 at 16 cells a side | 6.6-8.0 ms | 189-208 fps |

  The resolution is not free: at 8 the bodies cost nothing distinguishable from
  noise, at 16 they cost roughly a millisecond and a tenth of the frame rate.
  That is affordable and it is the right trade — an animal that reads as an
  animal is the entire point — but it means the visible cap is now a real
  budget rather than a formality, and 150 is close to it rather than
  comfortably inside it.

  Re-measure when the flocks are real: the AI, the per-flock messages and the
  population tick are not in these numbers.
- **Actor count.** One actor per flock is 20-60 world-wide, comfortably inside
  the water pool's precedent — but flock *spawning* has to be rate-limited, or a
  newly streamed region creates a dozen actors in a frame.
- **The registry over an unbounded world.** Sparse and lazily ticked keeps it
  small, but "the player has visited a great many chunks" is a save that grows.
  A cap, or eviction of empty cells, should land in slice 4 rather than later.
