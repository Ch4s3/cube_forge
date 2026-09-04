# Biomes — a derived, mutable climate field — design

The terrain work that shipped under `2026-09-03-terrain-biomes-design.md` is
altitude banding, not biomes: `surface_id` picks a block from height and slope,
`sea_level` is 62, `snow_line` is 95, and granite appears above a slope
threshold. Nothing anywhere answers "what kind of place is this?", which is why
weather is global and why the accumulation work in `todos.md` is blocked.

This adds that answer, and makes it **mutable**: a player who digs a canal
through desert wets its banks, and a player who levels a mountain warms it.

Scope: the climate field, weather driven by it, three new surface blocks with
gradual retexturing, and vegetation that grows and decays. Landing order in §9;
phase 1 is the only phase the others cannot be built without.

## 1. Two axes, three inputs

Temperature and moisture, the Whittaker pair. The three things a biome is
supposed to depend on map onto them:

```
temperature = temp_base(wx, wz, seed) - LAPSE * (height - sea_level)
moisture    = clamp01(1 - dist_to_water / MOISTURE_REACH)
elevation   = the column's actual surface height
```

`dist_to_water` is a Chebyshev distance in columns from the multi-source BFS in
§2, so moisture falls off linearly and reaches zero `MOISTURE_REACH` columns from
any water. `temp_base` is a single low-frequency `value2` octave, the same shape as
`Noise.ruggedness` and for the same reason: summing octaves clusters values
around 0.5 and leaves nothing above a threshold. It forms regions rather than
noise, and it is seeded, so a world's climate is stable across runs.

Elevation enters twice: through the lapse rate, and directly as a gate for the
alpine biomes. That is what makes levelling a mountain warm it.

Classification is a lookup on the two axes plus the elevation gate:

| | cold | temperate | hot |
|---|---|---|---|
| **dry** | tundra | grassland | desert |
| **damp** | taiga | forest | wetland |

plus **beach** (any temperature, immediately adjacent to water, low slope) and
**alpine** (above the elevation gate, whatever the axes say). Eight in total.

Granite is deliberately absent from the table: it stays on the existing
slope rule in `surface_id`, so a cliff face reads as rock in every biome. Alpine
is scree above the treeline, not cliff.

### No feedback loops

Retexturing never places water, and vegetation never changes terrain height. So
neither output can feed back into an input, and the field cannot oscillate.
This is a property worth stating rather than discovering: a mutable climate that
influenced its own inputs would need damping, hysteresis and a stability
argument, and this one needs none of that.

## 2. The field is recomputed whole, not incrementally

The world is 128 x 128 columns: **16,384 entries**. For scale, the skylight
flood is 4.2 million cells at 271 ms; a 2D multi-source BFS over 16k columns is
about 256 times smaller.

So the biome field is recomputed **in full on the existing `tick_period()`
boundary**, and player edits need no special handling whatsoever. This is the
opposite of the decision the light field made, and deliberately so: the light
field is 4 MB and must be relit incrementally, and that incremental path is the
most delicate code in the project. At 64 KB the same care would be waste.

Three arrays, all `NativeU8Arr(16384)`:

- **`height`** — the column's surface y. Built once at startup by scanning down
  from `Light.sky_floor`, then updated **O(1) per block edit**: an edit only
  changes a column's surface if it is at or above the current one.
- **`water`** — whether the column's surface block is water, or the block
  directly above it is. *As built:* rescanned whole every tick (two reads per
  column off the heightmap) rather than hooked per edit, because the water
  actors move water every tick through a path no edit hook sees.
- **`biome`** — the classification, rewritten each tick.

Plus two more for the eased axes (§3). Roughly 80 KB in total.

### Rejected

- **Baking the biome at world generation**, as a function of `(wx, wz, seed)`.
  Cheapest by far, and it is what the current terrain does — but a baked field
  cannot change, which is the entire point of this work.
- **Incremental biome updates on edit**, mirroring `Light.relight_at`. Correct
  and much more code. The measurement above says it buys nothing.

## 3. Easing happens on the axes

A discrete biome id cannot be eased, so the easing lives one level down:
`moisture_actual` and `temperature_actual` move toward their targets by at most
`ease_rate` per tick, and classification runs on the eased values. This is the
same mechanism `Weather.advance` uses for storm intensity, for the same reason —
nothing should snap.

**A dead band is required.** Without one, a column whose eased value sits exactly
on a threshold flips between two biomes every tick, and with retexturing (§5)
that means it also rewrites its surface block every tick forever. The band is
asymmetric: a biome must be exceeded by the band's width to be entered, and
fallen below it to be left.

Rate is set so a canal crosses a threshold in **one to two in-game days** — at
`CF_DAY=1800` that is roughly 30-60 minutes of play for the biome to turn, then
a few more minutes for §5 to finish the surface. Long enough to read as
consequence, short enough that the player who dug the canal sees it happen.

`CF_BIOME_RATE` scales it, so the whole effect can be inspected without waiting.

*As built:* the anti-flap is a **hold counter** rather than a value dead band —
a column flips only after `hold_ticks()` (30) consecutive ticks of disagreement.
It has the same effect on every axis at once and needs no per-threshold tuning.

## 4. Weather stays one system, felt locally

The weather actor is **unchanged**. It still runs one global storm with one
phase machine, because a world 128 blocks across does not have room for two
weather systems and pretending otherwise would be theatre.

What becomes biome-aware is the local effect:

- `Precip.snow_mix` keys on the column's **temperature** rather than
  `Noise.snow_line`. Snow falls where it is cold, not merely where it is high.
- Precipitation intensity gains a per-biome multiplier, so a desert stays drier
  than a wetland under the same storm.

Both are small changes to existing functions, and together they are what
unblocks the accumulation entry in `todos.md`.

## 5. Retexturing: a bounded migration queue

Surface blocks migrate toward the biome's palette over time. A column whose
surface disagrees goes in a queue; each tick at most **`CF_BIOME_BUDGET`**
columns migrate, each one a `set_block` -> `relight_at` -> mark the chunk dirty,
reusing the existing remesh path with no changes to it.

**The budget is the whole cost story.** Without it a single canal block can flip
thousands of columns in one tick and remesh a large fraction of the world in one
frame. This is the same lesson `CF_PRECIP` learned: bound the work, measure it,
and expose the bound.

Migration is one block per column per step — the surface only. Subsurface stays
as generated; a biome change is a change of skin, not of geology.

## 6. Vegetation grows and decays

`trees.march` places trees as a pure function of `(wx, wz, seed)`. If that
function read the biome, every tree in a drifting region would **pop in and out
of existence**, because the function is evaluated fresh every time the chunk is
meshed.

So the tree function is left exactly as it is — it remains the generator for new
worlds — and ongoing change goes through the same queue as §5:

- **growth**: a column whose biome wants vegetation and has none gets a tree,
  placed by the existing `Trees` code at that column.
- **decay**: a column whose biome no longer supports its vegetation loses it,
  one block per step, leaves before logs.

Decay is the harder half: a tree is a 3D structure spanning several columns, not
a single column, so removal needs the tree's extent rather than just its base.
It lands last (§9) and may be reduced to leaves-only if the cost is bad.

## 7. Eight biomes, three new blocks

`dirt(20)`, `gravel(21)`, `clay(22)`, each a procedural generator in
`texture.march` alongside the existing ones, with `layers()` going 13 -> 16.

| biome | surface | vegetation |
|-------|---------|------------|
| tundra | snow | none |
| taiga | dirt | pine, sparse |
| grassland | grass | bushes only |
| forest | grass | oak, dense |
| desert | sand | none |
| wetland | clay | oak, sparse |
| beach | sand | none |
| alpine | gravel | none |

## 8. The map view shows it

The top-down map view already exists (M, `CF_AUTOMAP`) and is already used for
headless verification. It gains a biome colouring toggled by `CF_BIOME_MAP=1`.

This is not decoration: without it the field is observable only through its
second-order effects, and a classification bug would be indistinguishable from a
weather bug or a retexturing bug. It is the primary verification surface for
phase 1, which otherwise mutates nothing and would be invisible.

## 9. Landing order

1. **The field** — axes, heightmap, water map, classification, easing, map-view
   colouring, knobs. Mutates nothing; fully observable through the map.
2. **Weather per biome** — snow by temperature, intensity by biome.
3. **New blocks and surface retexturing** — the migration queue and its budget.
4. **Vegetation** — growth first, then decay.

Phase 1 is the one that has to be right. Two, three and four are consumers of
it and can be judged independently once it is.

## 10. Verification

- **Classification is a pure function** of (temperature, moisture, elevation) and
  is unit-tested directly across the axis space, including every threshold and
  both sides of the dead band.
- **Heightmap agreement**: after a generated world plus a scripted sequence of
  edits, the incrementally maintained heightmap equals a full rescan. This is
  the property most likely to rot, because it is the only O(1) incremental path
  in the design.
- **A canal changes a biome**: script a trench of water blocks through desert
  with `CF_AUTOEDIT`, run with `CF_BIOME_RATE` high, and assert the columns
  beside it classify as wetland — and that they do not before the trench.
- **Levelling warms**: remove the top of a mountain and assert the column's
  temperature rises and its biome leaves alpine.
- **No flapping**: run a long simulation with a column parked on a threshold and
  assert its biome id changes at most once.
- **Budget is respected**: assert no tick migrates more than `CF_BIOME_BUDGET`
  columns, and record the frame cost in `RESULTS.md`.
- **Frame dumps require `CF_NOMOUSE=1`** and `scratch/cmpframe.py`; see
  `RESULTS.md`.

## 11. Risks

- **Migration cost** is the main one, and it belongs to phases 3-4 rather than
  to the field. Bounded by the budget, measured before and after.
- **The eased axes are world state.** The field is derived and can be rebuilt
  from the world, but its current eased *position* cannot. Save/load must
  persist the two axis arrays or biomes will snap on load.
- **Threshold tuning is guesswork** until the map view exists. That is the
  argument for landing phase 1 first and looking at it.
- **Decay may be too expensive** and is explicitly allowed to shrink to
  leaves-only rather than blocking the rest.
