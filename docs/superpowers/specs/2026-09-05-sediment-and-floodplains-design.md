# Sediment and floodplains — design

Rivers that erode should deposit. The valley carve (`Noise.river_mask` times
`river_depth(a)`) takes material out and puts nothing down: the floor of a
valley is the same dirt and grass as the slope above it, and a valley floor
follows every bump of the land it was cut into. This lays what the river
moved: a flat floodplain along the channel and clay and mud beneath it.

## 1. The floodplain is a height rule

In `Noise.height`, after the carve, the land inside the valley is pulled
toward the channel's floor:

    floor = land2 + mountains - river_depth(a) * (1 - 0.5 rug)   -- the centreline height
    plain = smooth(clamp((river - plain_edge(a)) / (1 - plain_edge(a))))
    h     = lerp(h, floor, plain * plain_strength(a))

`plain_edge` sits inside the river mask (0.55 young .. 0.35 ancient) so the
plain is the inner part of the valley; `plain_strength` 0.5 .. 0.9. A young
valley is a V with a narrow floor; an ancient one is a wide flat floor with
the slopes pushed out to the edges of the mask. Both paths (scalar and
F32x4) as ever, and the mismatch count stays the check.

On the plains the floor dips under sea level by mid-age, so the floodplain is
the flat sandy margin of a river. On the uplands it is a dry flat-floored
valley, which is where a valley spring (its own spec) makes a brook with room
to meander.

## 2. Sediment is a subsurface rule

The surface belongs to the biome: `Biome.palette` rewrites every palette
block to what the climate wants, and the floodplain will be wetland or
grassland by moisture (the brook decides). So sediment goes **under** the
surface, where it lasts, and shows on riverbank faces and under the spade:

| where | subsurface (4 blocks) |
|---|---|
| valley floor (`river >= plain_edge`), under water or within 1 of it | mud (24) |
| valley floor, dry | clay (21) |
| lake and sea margin: `h` in `[sea - 2, sea + 1]`, slope <= 1, outside a valley | sand, as today |

`subsurface_id` takes the river mask and height; `fill_terrain_column` passes
them. The surface rule (`surface_id`) is unchanged, so the biome field sees
what it saw before, and the sand shore keeps its rule.

`Biome.wetland_surface` already mixes clay and mud on a fixed hash; the
floodplain's subsurface uses the same split so a dug wetland matches its
bank.

## 3. Age

All of it: `plain_edge` and `plain_strength` above, and `river_edge` and
`river_depth` from the erosion spec. Young worlds have almost no plain; an
ancient world's lowland rivers run through flats a dozen blocks wide, which
is where the sediment (and, with valley springs, the wetland) is.

## 4. Cost

One `smooth` and one `lerp` per column in each height path. Nothing else.

## 5. Measured

- `CF_TERRAIN_STATS`: flat columns (slope <= 1) as a percentage, ages 0/50/
  100 — expected to rise with age; granite unchanged or lower.
- `CF_TERRAIN_MAP` at age 100, a river seed: the floor reads as a band of
  one character, not a gradient.

## 6. Tests

- Along a sampled centreline (columns with `river_at >= 0.9`), the height
  difference between neighbouring columns is at most 1 at age 100 for 95% of
  pairs.
- `subsurface_id` returns mud on a wet floor, clay on a dry floor, dirt
  outside the valley, sand on the shore: the four cases by argument.
- SIMD/scalar agreement over the whole map stays under 10 of 16,384.
