# Micro-voxel models — small things drawn as sculptures inside their cell — design

Every block is a metre cube, and the things that are not cubes -- small
mushrooms, palm fronds, bush leaves -- are drawn as cubes wearing an
alpha-cutout texture. That was the cheapest thing that read as "not a block".
This replaces it, for those three, with a **model**: an 8 x 8 x 8 grid of
coloured sub-cubes drawn inside the cell. A small mushroom gets a stem, a round
cap and gills; a frond becomes a curved blade; a bush a low tangle.

World state does not change. Block ids, items, icons, the save format,
collision, light emission, growth and decay are all untouched: this is skin. A
player-built detail layer and a finer world grid were considered and set aside
(see the brainstorm in the session that produced this); this is the cheap one.

## 1. A model is a grid of colour indices

`CubeForge.Model`: a shape is an `8 x 8 x 8` `NativeU8Arr` of **palette
indices**, 0 for empty, indexed `x + 8 * (z + 8 * y)` like a chunk. Shapes are
built **procedurally in code**, the way `texture.march` builds every texture, so
a mushroom is one function taking a species colour and six species are six
arguments, not six sculptures.

Colours live in one new texture layer, **the palette**: layer 89, 16 x 16
texels, texel `k` is colour `k`. A model face is an ordinary quad whose `uv`
points at the centre of texel `k` and whose `layer` is the palette's. No shader
change, no new vertex format, no new draw pass: the quads go in the foliage
buffer under the same `u_cutout` draw, with alpha 1 everywhere.

`Texture.layers()` goes 89 -> 90. The palette holds the six species cap colours
(`Species.colour_*`), their darker gill shades, a stem cream, two soils, the
frond greens and midrib, the bush greens and a stem brown: about thirty entries,
with room to 256.

## 2. Templates: greedy once, stamp many

At startup every (shape, orientation, variant) is meshed **once** into a
template: quads in cell-local coordinates (0..1), produced by a greedy pass over
the 8 x 8 x 8 grid with faces between two filled sub-cubes dropped. A template
is an `F32Buf` in the ordinary 9-float vertex layout with position relative to
the cell origin and shade left at 1.

When `Mesher.mesh_section_cat` meets a model block in the foliage category it
**stamps** the template: appends its vertices with the cell's origin added to
each position and the cell's shade written in. One shade for the whole model,
from the cell's own skylight and block light (`la`, `lb` at the cell), packed as
every vertex packs it today. No ambient occlusion on a model; its own
self-shadowing is drawn into the shape as darker gill and underside colours.

The greedy pass over the 16 x 16 x 16 section is unchanged for every block
that is not a model. Model blocks stay **see-through** for their neighbours, as
cutout blocks are today, and never hide a face; they emit no cube faces of their
own.

Stamping cost is proportional to the template's vertex count, a few hundred
floats per placement, appended into the section buffer the mesher already
threads (GAPS G21/G68 shapes: the buffer is uniquely owned during the mesh).

## 3. The three shapes

**Small mushroom** (`Chunk.is_fruit_small`, ids 47..52). A stem two sub-cubes
wide tapering to the cap, a round cap in the species colour five to six wide
and two tall with a darker rim and gills underneath, standing on a two-by-two
patch of soil. The species is in the id; the colour is the species' cap colour
from `Species`. Glow is untouched: `Light.emission` reads the id.

**Palm frond** (`Chunk.palm_frond`, id 66). A blade one sub-cube thick, three
wide, rising from the trunk side and arching down toward the tip, with a lighter
midrib. **Orientation comes from the neighbours**, which the mesher already
reads for face culling:

1. a palm log in one of the four horizontal neighbours: the blade points away
   from it;
2. else fronds among the horizontal neighbours: the blade points away from the
   sum of their directions (so the ring's outer fronds point outward, the
   diagonal ones diagonally);
3. else a palm log directly below: the crown centre, an umbrella of four short
   blades;
4. else (a frond placed alone by the player) a drooping tuft.

Eight directions plus the umbrella and the tuft: ten templates.

**Bush leaf** (`Chunk.oak_leaves`, id 17, **standing on solid non-foliage,
non-log ground** -- the same test as `Veg.has_bush`). A low tangle: three or
four leaf plates one sub-cube thick at different heights and angles on short
stems, in two greens. Four variants chosen by a position hash so a 3 x 3 clump
does not repeat. Oak leaves anywhere else -- the canopy -- stay cubes and keep
the greedy mesher, which is what makes a forest affordable.

## 4. Cost, and the one risk

A cube is at most 36 vertices. A model is a few hundred. Mushrooms and fronds
are few per world. **Bushes are not**: grassland and forest hold about fifteen
bush leaf blocks per chunk, so a bush template must stay lean -- on the order
of a hundred faces, six hundred vertices -- or the foliage pass grows several
times over and every section remesh with it.

So the plan lands the machinery on mushrooms and fronds, **measures** foliage
vertices per chunk and the section remesh time at the seed 7 spawn before and
after, sets the bush template's face cap from that, and records the numbers in
`RESULTS.md` before bushes ship. If bushes cannot fit, they stay cubes and the
spec says so in an *As built* note.

Per-face caps, enforced by tests: mushroom 160, frond 120, bush 100.

## 5. What does not change

Block ids and items. HUD icons (`Texture.icon_layer`). The save format. Collision
(`Chunk.is_collidable`). Light emission and opacity. The raycast target, which
is still the whole cell -- a mushroom occupies part of the cell, and aiming at
the empty corner still hits it, as today. Growth, decay, felling. Wind sway
through the fx word is a natural follow-up and is out of scope.

## 6. Verification

- Every template's vertices lie inside the unit cell; every template's face
  count is under its shape's cap; a template has no two faces at the same place
  facing opposite ways (the greedy dropped the interior).
- The palette layer's texel `k` is the colour `Model.colour(k)`.
- The mesher: a section with one small mushroom emits exactly the mushroom
  template's vertex count, translated to the cell, shaded by the cell's light;
  the same cell as air emits nothing; a canopy leaf emits cube faces.
- Frond orientation: on a palm stamped by `Veg.plant` the ring's fronds face
  away from the trunk, the crown centre is the umbrella, a lone frond is the
  tuft.
- A bush leaf on grass models; the same leaf with air below is a cube.
- Foliage vertex count and section remesh time before and after, seed 7 spawn,
  in `RESULTS.md`, with the bush decision.

## 7. Landing order

1. `Model`: the grid, the palette layer, the greedy template builder, the
   template cache, and the mushroom shape. Tests on the templates alone.
2. Stamping in the mesher for small mushrooms. Measure. Frame dump.
3. Fronds with neighbour orientation.
4. Bushes behind the measurement.

## 8. As built (2026-09-05)

- The template set is a `Model.Templates` value (a float blob plus offsets),
  held in the `World` as a seventh field beside the shown species; the mesher
  takes it as a parameter from `ChunkMesh`. `Set` collided with a stdlib type.
- Bushes ship: foliage vertices at the seed 7 spawn went 3.0x (47k -> 142k),
  the frame budget did not move (10.6 ms worst against 11.6 before), and the
  bush is a four-layer mound rather than the spec's three plates, which read
  as tables on sticks. See RESULTS.
- Palms were not confirmed visually; the frond rule and the crown stamping are
  covered by the mesher tests.
