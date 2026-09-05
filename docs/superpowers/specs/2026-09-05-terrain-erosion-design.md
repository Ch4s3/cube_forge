# Terrain: plateaus, rolling hills, river valleys, and an eroded look

Reworks `CubeForge.Noise.height` (and its F32x4 twin) from three summed noise
layers into a small stack of landform layers that share one distortion, so the
result reads as a landscape that water has worked on rather than as noise.

## Layers

All frequencies are per block. A feature is a fixed number of blocks across,
so a larger world holds more plateaus, hills and valleys instead of stretching
the same few; the 128-block world already shows several of each.

1. **Domain warp.** Two low-frequency value-noise fields (wavelength ~90
   blocks) move the sample point by up to `warp_amp()` = 14 blocks in x and z.
   Every layer below samples at the warped point, so landmass edges, hill
   crests, plateau rims, ridgelines and river valleys all bend along the same
   flow lines. Ruggedness stays unwarped: it is a region mask, not a shape.
2. **Base.** The broad landmass: one octave at 0.022 plus 3-octave detail at
   0.050, `37 + (0.82 cont + 0.18 detail) * 64`. The detail's share dropped from
   0.24 so the plains are gentler; the offset rose from 30 to keep the mean
   height where it was once the hills became zero-mean.
3. **Rolling hills.** 3-octave fBm at 0.030 (~33-block wavelength), zero-mean,
   `hill_amp()` = 20 peak to trough, scaled by `1 - 0.6 * ruggedness` so
   mountain country is not double-bumped.
4. **Plateaus.** `terrace(land)` snaps a height onto `plateau_step()` = 9-block
   terraces: flat for the middle 60% of each step, a ramp steep enough to be a
   granite rim between. Blended in by `plateau_mask`, a single low-frequency
   octave at 0.016, so mesas form in plateau country and nowhere else.
5. **Mountains.** Ridged fBm at 0.028, gated by ruggedness, amplitude 45. Each
   octave is now multiplied by the ones below it (`r0 * (0.45 + 0.33 r1 +
   0.22 r1 r2)`), so valley floors are smooth and only the ridges carry fine
   creases.
6. **River valleys.** `river_mask` is the crease of a single ridged octave at
   0.012: the level set of a noise field, a meandering line that never
   dead-ends. It carves `river_depth()` = 13 blocks (half that in mountains, a
   canyon rather than a plain) over a ~20-block-wide valley. On the plains the
   floor dips under sea level and fills, so rivers and long lakes appear; on
   uplands they are dry, sand-floored valleys.

## Measured

`CF_TERRAIN_STATS=1` now also prints how many columns the F32x4 and scalar
generators disagree on (they are f32 and f64 implementations, and the
truncation to Int can straddle an integer: 0-5 of 16384 across five seeds),
and `CF_TERRAIN_MAP=1` prints the world as a 64x64 character map so plateaus
and valleys are seen rather than inferred.

Five seeds (7, 42, 1234, 99, 2024), before and after:

| | underwater | granite | snow | mean height |
|---|---|---|---|---|
| before | 17-42% | 1-15% | 0-7% | 64-78 |
| after | 7-55% | 5-18% | 0-11% | 61-82 |

World generation time rose from roughly 220-290 ms to 240-370 ms: the scalar
`slope` still calls `height` four extra times per column, and there are more
octaves per call.

## World age

A world is a seed and an age, 0 (young) to 100 (ancient). The age rides in the
seed's high bits (`Noise.with_age`, `age_of`, `base_seed`; one age unit is
2^32, clear of the 30-bit seed and the offsets the layers add), so it travels
everywhere the seed already goes: saves, the startup print, the menu's seed
field, every generator. `Noise.hash2` masks the age off, so the climate,
vegetation and every other consumer of the seed see the same world at any age;
only erosion reads it.

Older means more erosion. Each knob is linear in the age fraction `a`, and
`a = 0.5` is the tuning above:

| knob | young (0) | ancient (100) |
|---|---|---|
| domain warp | 6 blocks | 22 |
| fBm detail share of the landmass | 0.24 | 0.12 |
| mountain amplitude | 58 | 32 |
| terrace sharpness (ramp = 1/sharpness of a step) | 4 | 1 (a slope) |
| river depth | 4 | 22 |
| river edge on the crease (lower = wider) | 0.86 | 0.66 |

Setting it: `CF_AGE=<0..100>` on the command line sets or overrides the age
(a plain `CF_SEED` keeps whatever age it carries, 0 for a bare number, so a
printed seed reproduces alone; an unset seed gets a random age). In the menu,
`-` and `=` move the age by 5 and `R` rerolls the seed and the age together;
the digits the player types are the base seed. Slot rows show `SEED n AGE a`.
