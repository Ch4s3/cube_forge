# New terrain blocks: basalt, ice, mud — design

Three new full-cube terrain materials, all following the existing pattern of
plain opaque solid blocks (like granite, gravel, clay): a new id, a texture
layer, and a generation rule. No new mesh geometry, no new interactions —
they mine, stack, and place exactly like every other solid block.

## 1. Block ids

Next free ids after `clay()` (21), in [chunk.march](../../../lib/cube_forge/chunk.march):

| id | name | fn |
|----|------|-----|
| 22 | basalt | `basalt()` |
| 23 | ice | `ice()` |
| 24 | mud | `mud()` |

`is_solid`, `is_collidable`, and `Inventory.yield_of` already cover any
nonzero, non-water id generically — none of the three need special cases
there. They are breakable like stone/dirt (no `is_unbreakable` entry).

## 2. Basalt — deep stone band

`fill_column_mat` currently fills plain stone (id `3`) for every cell between
the subsurface layer and the bedrock floor. That branch gets a depth check:

```
fn deep_stone_depth() : Int do 24 end
```

Below `y < deep_stone_depth()`, use `basalt()` instead of `3`. This is a pure
function of `y` — no biome or slope dependency, the same way granite already
overrides the surface rule on steep slopes regardless of biome. Basalt starts
appearing well before bedrock (`bedrock_depth()` is 3), so it's a real
mid-depth material a player digs through, not a rare bedrock-adjacent flourish.

## 3. Mud — wetland surface variant, alongside clay

[biome.march](../../../lib/cube_forge/biome.march)'s `palette()` currently
maps the wetland biome unconditionally to `C.clay()`. It becomes a per-column
choice between clay and mud, hashed from the column coordinates the same way
tree/bush placement already hashes cells (cheap, deterministic, no new noise
octave):

```
fn wetland_surface(x : Int, z : Int) : Int do
  if (x * 73856093) ^^ (z * 19349663) % 3 == 0 do C.mud() else C.clay() end
end
```

(exact hash mixing left to implementation — the requirement is just "looks
mixed, not striped or checkerboarded"). `palette()` calls this for the
wetland case instead of returning `C.clay()` directly. `is_palette_block`
gains `mud()` alongside the existing list, so mud migrates and re-migrates
the same way clay does when a column's biome classification changes.

Clay and mud are both surface-only in this pass — no change to what's
beneath them (still plain dirt/stone via `subsurface_id`).

## 4. Ice — cold water surfaces, altitude only

**Scope note:** the original draft of this design also froze tundra-biome
lakes. That turned out to need a second migration/apply pipeline parallel to
the existing surface-palette one — biome classification isn't available
until after generation (it depends on a live, world-wide `Biome.Field`, not
noise), so it would mean new tick wiring in `cube_forge.march`'s frame loop
and a new `World.set_block` path for water cells. That's a much bigger,
riskier change than the rest of this feature, so it's dropped from this pass
in favor of the altitude rule alone. Tundra-lake freezing is a reasonable
follow-up once it's the only thing being built.

Ice is generation-time only, exactly like basalt and mud: a water **source**
block (id 4, `is_source`) placed at or above the snow line
(`Noise.snow_line()`, 95) becomes `ice()` instead. `fill_water` (in
`fill_column_mat`) only ever fills from just above a column's terrain top up
to sea level (62) — always below the snow line, so it is never a candidate
and is left alone. `plant_springs_go`, which places a single source block at
a spring cell's surface height, is the one path that reaches high altitude
(mountain springs) — that's where the check goes.

Only the source cell placed by generation is ever ice; nothing about the
flow simulation, evaporation, or `is_water`/`is_source` changes — `ice()` is
a distinct, non-water id, so mined or placed ice behaves exactly like any
other opaque solid block (no melting, no slipperiness, no interaction with
the water tick).

## 5. Textures

[texture.march](../../../lib/cube_forge/texture.march) gets three new 16×16
layers appended after the existing 17 (`layers()` becomes 20), each built with
the existing `speckle_go` generator:

| block | layer | base RGB (speckled) |
|-------|-------|----------------------|
| basalt | 17 | dark charcoal-grey, near-black (e.g. `40, 38, 42`) |
| ice | 18 | pale blue-white (e.g. `205, 225, 240`) |
| mud | 19 | dark reddish-brown (e.g. `70, 48, 38`) — distinct from clay's tan |

`layer_for` gets three more branches mapping the new ids to these layers. No
change to `layer_for_face` (none of the three have direction-dependent faces
like logs do).

## 6. Out of scope

- Obsidian (a rarer/deeper basalt variant) — noted as a natural follow-up,
  not built here.
- Ice melting, slipperiness, or any gameplay behavior beyond "solid opaque
  block."
- Tundra-biome lake freezing — see the scope note in §4; needs a live-tick
  migration pipeline, follow-up work.
- A volcanic/badlands biome for basalt — it's depth-only, not biome-driven.
- Alternating clay/mud subsurface layers — both are surface-only for now.

## 7. Landing order

1. Block ids + `is_palette_block` entry (`chunk.march`).
2. Textures + `layer_for` (`texture.march`) — needed before generation is
   testable by eye.
3. Basalt depth band (`chunk.march` `fill_column_mat`).
4. Mud/clay wetland hash (`biome.march` `palette`) — signature changes to
   `palette(id, x, z)`; update its six call sites (`biome.march`'s
   `migrations_go`, `cube_forge.march`'s `migrate_go`, `veg.march`'s
   `can_grow` and `can_grow_bush`, and `biome_test.march`'s `apply_palette`
   and `all_mismatched`).
5. Ice altitude rule (`chunk.march`, generation-time: `plant_springs_go`).
