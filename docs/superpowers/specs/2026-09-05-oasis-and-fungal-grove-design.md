# Oasis and fungal grove — two biomes the field earns, not the seed — design

`2026-09-04-biomes-design.md` gave the world eight biomes: a 3x2 Whittaker
table plus two elevation gates. Both are functions of temperature, moisture and
height, and every one of them exists in a fresh world before the player touches
it. This adds two that a fresh world rarely has, because both are answers to
something that happened:

- **oasis** — a small body of water in a hot region, ringed by grass and palms.
  A well dug in the desert makes one.
- **fungal grove** — ground the mycelium network has taken outright. A patch
  that fruits is a grove; a forest planted with fungus loses its oaks to it.

Ten biomes in total. Scope: water bodies and their moisture reach, the two
classifications, palms as a third tree species, and the table rows every biome
carries (palette, precipitation, vegetation, map colour, audio). Landing order
in §7.

## 1. Why oasis cannot be a cell in the table

Moisture is `1 - dist / 24`, so every column within 12 of any water is damp,
and a hot damp column is wetland. Dig a pool in the desert today and you get a
clay marsh twelve columns wide on every side. There is no room in that picture
for a grass ring: the pool wets as far as the sea does.

The fix is upstream of classification. **A small body of water moistens a short
way.** Once that is true, the oasis falls out of the existing table as "hot,
damp, and the water is small", and the canal the biome spec was built around
still works, because a canal is long.

## 2. Water bodies

The field already keeps a `water` flag per column, rescanned every tick. Each
tick, the flagged columns are grouped into **connected bodies** by 8-neighbour
adjacency, and each body's column count is compared against a threshold:

| knob | default | meaning |
|---|---|---|
| `small_body()` | 48 columns | a body at or under this is small |
| `small_reach()` | 4 columns | moisture from a small body reaches this far |

The labelling is a level sweep in the light-field idiom, like `distances`: one
label array threaded through every write, never wrapped (GAPS G68). Sixteen
thousand columns, a few passes, well under the biome tick's budget. Counts are
taken in one pass over the labels; a second pass turns each water column's
label into a **small flag**.

`distances` then carries that flag with the distance. Level `lv` marks each
unvisited neighbour with `lv + 1` **and copies the marker's small flag**. Where
two bodies reach a column on the same level the first marker wins; the tie is
arbitrary and harmless, since either answer is one column off a real boundary.
The flag lives in the distance byte's top bit (distances are at most 24, so the
low five bits are plenty), which keeps the Field's shape and the save format
unchanged.

```
moisture_target(dist, small) = clamp01(1 - dist / (small ? small_reach() : reach()))
```

A column farther than `small_reach()` from a small body, and farther than
`reach()` from any large one, is dry.

**What a body's size means in practice.** A placed finite-water block spreads
into a puddle of a handful of columns: small. A spring's brook on a hillside
is a line of a dozen or two: small, unless it runs to the sea, in which case it
is part of the sea. The canal test's trench is over 48 columns long: large. The
sea and every lake at sea level: large. The threshold is a knob because the
right value is the one that makes a brook read as an oasis and a canal read as
a marsh, and that is judged on the map, not derived.

**This changes existing worlds.** Every small pond in every climate gets a
four-wide damp halo instead of a twelve-wide one: a temperate pond grows a
tight forest ring, a cold one a tight taiga ring. Accepted as an improvement;
the biome map before and after is the check that it reads as one.

## 3. Classification

`classify` gains two inputs: the small flag, and whether the column's mycelium
is established (§4). The gate order becomes:

1. **grove** — the network is established here, whatever else is true.
2. **alpine** — as today.
3. **oasis** — hot, damp, and the water is small.
4. **beach** — as today. Oasis precedes it so a low pool rim in the desert is
   palms, not sand.
5. the Whittaker table, as today. Hot damp large water is still wetland.

The pure function stays testable across the whole input space, and the two new
inputs are booleans, so the existing threshold tests do not move.

Ids: `b_oasis()` is 8, `b_grove()` is 9, appended after alpine so no existing
id or map colour shifts. The map upload in `cf_shim.c` masks the id with `& 7`
against an eight-row colour table; that becomes a ten-row table and a bounds
clamp. Every per-biome table (`name`, `precip_scale`, `palette`, the five in
`audio.march`, `Veg.wants_trees` and friends) gets two rows.

## 4. Fungal grove

**Rule.** A column is a grove when

- its mycelium species is set, and its vigour is at or above
  `Myc.fruit_vigour()` (0.8), and
- at least five of its nine columns (itself and its eight neighbours) meet the
  same test.

The neighbour count puts the grove's edge one column inside the patch's edge,
so a single frontier column whose vigour is hovering never flickers a grove in
and out on its own. The biome's hold counter applies on top, as for every flip.

A fruiting patch and a grove are therefore the same thing seen from two
layers: "fruits here" is the network's statement, "grove" is the ground's.

**Where it is computed.** `Biome.tick` already takes the network's climate
pulls. It also takes the network's species and vigour arrays, and it treats the
mycelium field's **dirty flags as a wake source**, the way it treats pull rows
today: a column the network changed last tick is evaluated even if the biome
considered it settled. Without that, a settled column would never notice its
vigour crossing the line. The biome reads the network as it stood at the end of
the previous tick, one tick behind, which is the order the field slot already
runs in.

**What a grove does.**

| table | grove |
|---|---|
| palette | none: the surface is the species' mycelium block, which the biome migration already skips (`is_palette_block` is false for mycelium ids), so the two migrations never fight |
| trees, bushes | not wanted: existing trees and bushes decay through `Veg` exactly as they do in a biome that has none. This is the grove's visible act: plant Lanterncap in a forest and the oaks fall as it establishes |
| fruit | `Fruit.rolls` gains a biome multiplier; grove is 3x. The fruit scan already receives the biome field |
| precipitation | 1.1 |
| map | violet |
| audio | its own row: the wetland's intervals, an octave down, slowest rate |

**Stability.** The grove is derived from mycelium state. The mycelium reads the
eased temperature and moisture and never the biome id. The grove removes trees
and raises fruit, and neither of those is an input to either field. So the
grove cannot influence what creates it, in either direction. This is the same
kind of property the biome spec stated for retexturing and the fungus spec
stated for climate feedback, and it is why the grove needs no damping beyond
the hold counter it inherits.

**Not in scope.** A distinct grove ground block (humus under the mycelium)
would be an eighth mycelium base; the base-times-glow id scheme ends at 46 with
fruit starting at 47, so it is a renumbering, and it buys a colour. The save
format still rebuilds the mycelium field from the seed, so groves vanish on
load until the save-the-fields entry in `todos.md` lands; that gap is already
recorded there and this adds nothing new to it.

## 5. Oasis

| table | oasis |
|---|---|
| palette | grass |
| trees | palms, density 0.5 so the ring fills |
| bushes | wanted |
| precipitation | 0.3: it is still in the desert |
| map | bright green |
| audio | desert's form and transposition, forest's octave and rate |

At generation `trees.march` places oaks and pines by height and knows nothing
of the field, and that stays so (the biome spec's §6 argument). Oases fill in
through `Veg` growth like every other biome-driven tree. A world with a hot
hillside spring therefore starts as bare grass around the brook and grows its
palms over the first in-game day, which is the same way a canal's banks grow
their oaks today.

## 6. Palms: a third tree species

`Trees.block_of_tree` and everything that calls it take a `pine : Bool`. That
becomes a species number: **0 oak, 1 pine, 2 palm**. Callers are the chunk
generator's `stamp_tree`, `Veg.plant`, `Veg.fell` and `Veg.unleaf_go`;
`Trees.is_pine(h)` becomes `Trees.species_at(h)` returning 0 or 1, so
generation is unchanged in effect.

Two new blocks, two new textures:

| block | id | layer | look |
|---|---|---|---|
| palm log | 65 | 87 | pale, ringed bark; cut ends reuse the oak rings layer |
| palm frond | 66 | 88 | alpha-cutout, long green with a lighter midrib |

`Texture.layers()` goes 87 -> 89. `Chunk.is_log`, `is_foliage`, `is_cutout`,
`Veg.is_log` and `Light.occludes` learn the two ids. Fronds are foliage in every
sense leaves are: cutout pass, see-through for culling and light, decay when
orphaned.

**Shape.** Trunk 5..7 logs, bare. Crown at the top: fronds at `dy == trunk`
where `|dx| + |dz|` is 1 or 2, and at `dy == trunk + 1` where it is 0 or 1.
A tall stem with a small spread head, readable as a palm at cube scale, and it
fits inside the 9x9 footprint every tree operation already walks. Felling
finds the species from the log id, as it does for pine.

## 7. Landing order

1. **Water bodies and the small-body reach.** Labelling, the flag in the
   distance sweep, `moisture_target` with the flag. Tests: a placed puddle
   gives a four-column halo; a 60-column trench gives the full one; a puddle
   touching the sea is large. Existing canal test unchanged. Look at the biome
   map on a seeded world before and after.
2. **Oasis.** The classification input, the two ids, every table row, the shim
   colour table. Test: hot column, small water within four, classifies oasis;
   same column with a large body classifies wetland; oasis beats beach.
3. **Palms.** Blocks, textures, the species refactor, oasis growth. Test:
   `block_of_tree` for palm places logs and fronds inside the footprint and
   nothing outside; a felled palm leaves nothing; oaks and pines are
   byte-identical to before the refactor (state hash of a generated world).
4. **Grove.** Species and vigour into the biome tick, the dirty-flag wake, the
   neighbour rule, table rows, the fruit multiplier. Tests: a mature patch
   (`Myc.plant_patch`) becomes grove one column inside its edge after
   `hold_ticks()`; a lone established column does not; a forest column that
   becomes grove is felled by `Veg`; a settled biome column wakes when its
   vigour crosses the line.

Step 1 is the one that has to be right; 2 and 3 consume it. Step 4 is
independent of 1 to 3 and could land first if the mycelium side is fresher in
hand.

## 8. Knobs

| knob | default |
|---|---|
| `Biome.small_body()` | 48 |
| `Biome.small_reach()` | 4 |
| `Biome.grove_vigour()` | `Myc.fruit_vigour()`, 0.8 |
| `Biome.grove_neighbours()` | 5 of 9 |
| `Fruit.biome_rate(b)` | grove 3.0, else 1.0 |
| `Veg.density(oasis)` | 0.5 |

No new environment variables. `CF_BIOME_MAP` shows both; `CF_BIOME_RATE`,
`CF_MYC_RATE` and `CF_FRUIT_RATE` speed them up as they do everything else.

## 9. Risks

- **The body threshold is guesswork until the map shows it.** Same argument as
  the biome spec's §11; it is why step 1 lands alone and is looked at.
- **Labelling cost.** New per-tick work over 16k columns. Bounded and small by
  construction, but measured and recorded in `RESULTS.md` like the rest of the
  tick.
- **The small-body halo alters existing worlds.** Accepted above; the risk is
  that a brook's halo at four columns reads as too thin for palms to register.
  `small_reach()` is the knob.
- **Grove flicker at a contested edge.** Two species fighting over a column
  move its vigour down and up; the neighbour rule and the hold counter are the
  two guards. If a grove edge still flaps under a scripted contest, widen the
  neighbour requirement before touching the hold.

## 10. As built (2026-09-05)

- The distance sweep is skipped when the water flags did not change since the
  last sweep; an edit that moves a flag marks the column's distance byte
  `stale()` (255) so the next tick sweeps. §2's "each tick" is the worst case.
- The grove wake is narrower than §4 says: a dirty row's columns are evaluated
  only where the grove test disagrees with the stored biome. Evaluating the
  whole row tripled the tick while wild patches eased.
- A palm's crown is twelve fronds round the trunk top (|dx| + |dz| in 1..2),
  not eight, plus the five above: seventeen.
- Ten biomes; the shim's colour table is indexed with a bounds check.
- `Biome.tick` keeps its signature for callers without a network; `tick_net`
  is the full one.
