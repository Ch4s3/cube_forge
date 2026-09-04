# Vegetation: trees, bushes, cutout leaves

Adds oak and pine trees plus bushes, a third mesh category for alpha-cutout
foliage, biome-driven placement that survives independent chunk generation, and
two more hotbar slots.

## 1. Blocks and textures

| id | block | texture layers |
|----|-------|----------------|
| 16 | oak log | 8 side, 9 cut end |
| 17 | oak leaves | 10 |
| 18 | pine log | 8 side, 9 cut end (tinted) |
| 19 | pine leaves | 11 |

Bushes are made of oak leaves (17), so they cost no new block or texture.
Logs use a **per-face** layer so the cut end shows rings: `emit_rect` already
knows the face direction, so `layer_of` becomes `layer_of(id, dir)` rather than
new machinery. All textures stay procedural, and every channel goes through
`Texture.byte` (a `NativeU8Arr` store wraps rather than saturating, GAPS.md G61).

## 2. Rendering: a third mesh category

`ChunkMesh` gains `foliage_sections` beside `opaque_sections` and
`water_sections`; chunk slot `i` draws opaque from `i`, water from `64 + i`,
foliage from `128 + i`.

Foliage is greedy-meshed exactly like opaque geometry, then drawn through a new
`cf_gfx_draw_cutout(slot, nverts)`: depth test and writes on, culling on, **no
blending**. GL 3.3 core has no fixed-function alpha test, so the fragment shader
gains a `u_cutout` uniform and does `discard` when the sampled alpha < 0.5.
Because nothing is blended, there is no sort-order problem and foliage may be
drawn before water.

**The culling rule is the subtle part.** `see_through(id)` must be true for
leaves, or a stone face behind a canopy is wrongly culled. But leaf-against-leaf
faces must still be culled, or a canopy becomes a solid mass of interior quads.
So face emission becomes "the neighbour is see-through **and** is not in the
same foliage group", the same shape as the existing water-against-water rule.

## 3. Lighting

`Light.opacity` is graded, not binary. Leaves take roughly half of
`max_level()` so a canopy casts dappled shade and thin foliage still passes
light; logs are fully opaque. Trees therefore participate in the existing
skylight flood with no changes to the light module itself.

## 4. Generation: placement independent of chunk order

Chunks generate independently (stage 1) but a tree at a chunk edge spills its
canopy into the neighbour, so placement must be a **pure function of world
coordinates**.

- The world is divided into 8x8-block **tree cells**. A hash of (cell, seed)
  decides whether the cell has a tree and its position within the cell, giving
  guaranteed spacing without any global list.
- Generating chunk (cx, cz) iterates every cell overlapping the chunk
  **expanded by the maximum canopy radius**, and writes whichever blocks of each
  tree fall inside this chunk. Neighbours derive identical trees with no
  communication — the same trick that makes the terrain chunk-independent.
- Bushes use a finer 4x4 grid and a single-block or two-block leaf clump.

Species and density come from the existing height/slope data: oak on grass
below the treeline, pine above it, thinning toward the snow line, and never on
sand, snow, granite, water, or slope > 1. A tree is only placed where its trunk
column is grass and above sea level.

## 5. Interaction

- Hotbar extends to 9 slots (keys 1-9): 8 oak log, 9 pine log.
- Leaves and bushes break but yield nothing (Minecraft without shears); logs
  are collectable and placeable.
- **Leaf decay:** breaking a log removes leaves within a small radius that have
  no other log within range — a local check at edit time, no actor needed — so
  chopping a trunk does not leave a canopy floating.

## 6. Verification

- Placement is deterministic for a seed, and differs across seeds.
- **A tree straddling a chunk boundary produces identical blocks whichever
  chunk generates it** — the property that would silently break and leave
  half-trees at chunk seams.
- Density and species land in expected ranges per biome; no tree or bush on
  sand, snow, granite, water or steep ground.
- Leaf-against-leaf faces are culled while a solid face behind leaves is not.
- Breaking a log decays its orphaned leaves and leaves neighbouring trees alone.
- `forge test`, `forge lint --strict`, and the per-frame allocation gauge
  unchanged (generation is startup work).
- Frame dumps: an oak lowland, a pine treeline, and a canopy seen from below.

## Out of scope

Cross-quad tufts and flowers (ruled out in favour of cutout cubes), saplings and
growth, tree variety beyond two species, seasons, leaf colour by biome.
