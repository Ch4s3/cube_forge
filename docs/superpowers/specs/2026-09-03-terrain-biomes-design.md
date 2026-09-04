# Procedural terrain: ruggedness, ridged mountains, surface materials

Replaces the single-fBm heightmap (heights 50..76) with a layered generator and
material rules, adds three block types, extends the hotbar to 7 slots, and makes
the world seed random by default.

## 1. Block palette

Water occupies ids 4..11 (source + 7 flow levels), so new solids start at 12:

| id | block | texture layer |
|----|-------|---------------|
| 1 | grass | 0 |
| 2 | dirt | 2 |
| 3 | stone (underground) | 3 |
| 4..11 | water | 1 |
| 12 | sand | 4 |
| 13 | snow | 5 |
| 14 | granite (cliff faces) | 6 |

`is_collidable` / `is_solid` / `see_through` already classify these correctly;
only `Texture.layer_for` gains cases. The greedy mesher keys faces by block id,
so materials never merge across a boundary.

## 2. Height: four layers

- **base** — fBm at ~0.010, the broad landmass: roughly 46..68.
- **hills** — fBm at ~0.045, amplitude ~6, everywhere, so plains still roll.
- **ruggedness** — a separate fBm at ~0.006 (offset seed), clamped and
  smoothed into [0, 1]: near 0 over plains, near 1 over mountain country.
- **mountains** — ridged noise (`1 - |2n - 1|`, 3 octaves) at ~0.020, scaled by
  `ruggedness²` and a large amplitude (~70), so sharp ridgelines appear only in
  rugged regions rather than as noise everywhere.

`height = base + hills + mountains`, roughly 46..144 against sea level 62.

## 3. Surface materials

Slope is the maximum height difference to the four neighbouring columns.
Applied in order, so a rock face beats altitude and a cliff beats a beach:

1. slope >= 5 → **granite** (cliff faces, including through the snow line)
2. height >= 95 (snow line) → **snow**
3. height <= sea level + 2 and slope <= 1 → **sand** (beaches and lake shallows)
4. otherwise → **grass**

Sub-surface: sand under sand, granite under granite, dirt under grass and snow,
for 4 blocks; stone below that.

## 4. Seed

`CF_SEED` unset seeds from the clock; the seed in use is always printed at
startup so any world is reproducible with `CF_SEED=<n>`.

## 5. Hotbar: 7 slots

Slots map 1:1 to placeable blocks: 1 grass, 2 dirt, 3 stone, 4 water, 5 sand,
6 snow, 7 granite; keys 1-7. `Inv` carries a count per slot. Harvest maps a
broken block id to its slot (water only from a source, as today). The hotbar
is re-laid out for 7 slots, still centred, with an icon per slot.

## 6. SIMD path

`height_x4` keeps computing the full height four columns at a time: the lattice
and hashing stay scalar (GAPS.md G32: no per-lane floor or float/int convert),
while base, hills, ruggedness and the ridged mountain term are vector ops
(`1 - |2n-1|` uses `max(2n-1, 1-2n)`). Material rules are scalar, since slope is
an integer comparison over neighbouring columns.

## 7. Verification

- Tests: ridged noise stays in [0, 1]; height is deterministic for a seed and
  differs across seeds; surface material rules pick the expected block for
  constructed height/slope combinations; SIMD and scalar heights agree.
- Frame dumps: a coastline (sand), a mountain range (granite faces, snow caps)
  and the top-down map view showing biome variety.
- `forge test`, `forge lint --strict`, and the per-frame allocation gauge
  unchanged (terrain generation is startup work, not the frame loop).

## Out of scope

Caves and overhangs (3D noise), rivers, trees, biome-specific water colour.
