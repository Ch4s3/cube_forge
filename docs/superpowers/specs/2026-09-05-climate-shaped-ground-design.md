# Climate-shaped ground — design

The biome field colours the surface after the fact: a desert is grass that
the retexture turns to sand. The ground under it was shaped with no idea of
the climate — a desert has the same rolling hills as a forest, the alpine
zone is bare terrain under snow, a wet lowland is a lawn. This gives three
climates a landform of their own: dunes, glaciers and bog. All three are
generation-time and read the climate the way the trees do — from what can be
computed per column without the field.

## 1. Climate at generation

`Biome.temp_base` is one seeded octave (`seed + 1700`) and the lapse is
`lapse()` per block above sea level: `temp_target(wx, wz, seed, h)` is a
pure function and moves into `Noise` (`Noise.temperature`), with `Biome`
calling it, so the height paths can read it without a cycle. Moisture is
distance to water, which does not exist yet; the proxies are height above
sea level and the river mask, and every rule below says which it uses.

## 2. Dunes — hot and dry

Where `temperature >= 0.66` and the column is at least `dune_rise()` (6)
above sea level and outside a river valley (`river < 0.2`), a dune term is
added in `Noise.height`:

    wind  = a fixed direction per world from the seed
    dune  = ridged(along-wind * 0.08, across-wind * 0.025, seed + 1300)
    h    += dune * dune_amp() * dune_mask

`dune_mask` is `smooth(clamp((temperature - 0.66) / 0.15))` times the
dryness proxy, so the dune field fades in over the hot edge and never sits
on a wet floor. Elongated crests across the wind at ~12-block spacing,
`dune_amp()` 6 blocks. Both height paths; the ridged helper exists in both.

Materials: a dune column's surface and subsurface are sand to 6 deep
(`subsurface_id` with the mask), so the biome finds sand and keeps it, and a
dug dune is sand through. Without the sand rule the retexture would sand
the surface and leave dirt an inch down.

## 3. Glaciers — cold valleys above the snow line

Where `temperature < glacier_temp()` (0.2, i.e. cold country with the lapse
already taken) and `h >= snow_line()` and the column is in a river valley
(`river >= 0.5`), the valley is a glacier:

- the column is filled with **ice** (23) from the carved floor up to the
  land height *before* the carve — the ice is the valley's fill, a tongue
  flowing down the channel with a flat top;
- the tongue ends where the temperature crosses `glacier_temp()` going down
  the valley; the last `moraine_len()` (6) blocks of it lay gravel instead
  of ice (a moraine, using the talus spec's subsurface rule);
- the U-shape: in a glacier column the river's carve uses `river_edge - 0.1`
  (wider) and `plain_strength` 1.0 (flat floor) from the sediment spec, so
  what is under the ice, and what an ancient world's dry glacial valley
  shows, is the wide flat trough.

Ice is generation-time only today (a spring's frozen form); it is not a
palette block, so the biome leaves it, and it is solid, so the glacier is
walkable ground. Alpine's gravel shows around it.

## 4. Bog — wet flat lowland

Where `h` is in `[sea_level(), sea_level() + 3]`, slope <= 1, and the column
is within a river valley's plain (`river >= 0.5`) or under `bog_reach()` (8)
blocks of the sea in height and mask terms — the wet proxy — one column in
`bog_pit()` (7) by `hash2` is dug one block to `sea_level() - 1` and fills
with water under the existing sea rule, and the surrounding surface is mud
or clay by `Biome.wetland_surface`. The result is flat ground pocked with
still pools: bog. The pools are sources at or under sea level, so they are
the lake rule's (no spring cost) and never evaporate. Distance-to-water then
makes the biome wetland here, which picks mud and clay — the same blocks —
so the retexture and the generation agree.

## 5. Age

- Dunes: none. Sand moves every year; a dune field is as young as the last
  wind.
- Glaciers: the moraine and the trough are the age's marks. At age 0 the
  tongue fills the channel to the brim; at 100 the U-valley is there and the
  ice is gone from all but the head (`glacier_temp` falls with age, 0.2 to
  0.08).
- Bog: the floodplain widens with age, so the bog does.

## 6. Cost

One temperature octave per column in each height path (a `value2`), the
dune ridged sample where hot, one hash for the bog pit. Glacier fill is a
column fill like water's. Generation time up by a few percent; measured.

## 7. Measured

- `CF_TERRAIN_STATS`: sand, ice and mud percentages by age on a hot seed
  and a cold seed (found by `Biome.debug_axes` at the player column).
- `CF_TERRAIN_MAP`: `d` dune crest, `g` glacier, `b` bog pool, so the three
  fields are seen as regions.
- Biome agreement after the first tick: the fraction of dune columns the
  retexture leaves alone (expected ~100%), of bog columns classified
  wetland.

## 8. Tests

- `Noise.temperature` equals `Biome.temp_target` for sampled columns (the
  move is a rename).
- A dune column is sand six deep; a cold valley column above the snow line
  is ice from floor to the pre-carve height; a bog pit column holds water at
  `sea_level()` with mud or clay around it.
- The dune term is zero where the temperature is below 0.66, and the SIMD/
  scalar mismatch count stays under 10.
