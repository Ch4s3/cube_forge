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

## 4. Ice — cold water surfaces

Water columns are deliberately excluded from the existing surface migration
(`migrations_go` skips any column where `wet` is true — water is never a
"palette block"). Ice needs a parallel, small migration pass rather than a
change to that one, so the finite-water flow simulation stays untouched.

A column's **source** water block (id 4, `is_source`) becomes ice when the
column is wet **and either**:

- its biome is tundra (`Biome.b_tundra()`), or
- its surface height is at/above the snow line (`Noise.snow_line()`, 95) —
  this is what catches a mountain spring that's cold regardless of the
  biome classifier's read on the column.

Only the exposed source cell freezes, not flow cells beneath/around it — a
frozen lake is still "water" a few cells down in this pass, which is fine
since the visible top is what a player interacts with. No melting, no
slipperiness, no interaction with the evaporation/flow tick: ice behaves
exactly like a normal opaque solid block once placed. If a source migrates
back to a flowing/unfrozen state later (biome drift, or the column drops
below the snow line), the same pass can migrate it back to water — mirroring
how `migrations_go` re-checks every settled column already.

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
- A volcanic/badlands biome for basalt — it's depth-only, not biome-driven.
- Alternating clay/mud subsurface layers — both are surface-only for now.

## 7. Landing order

1. Block ids + `is_palette_block` entry (`chunk.march`).
2. Textures + `layer_for` (`texture.march`) — needed before generation is
   testable by eye.
3. Basalt depth band (`chunk.march` `fill_column_mat`).
4. Mud/clay wetland hash (`biome.march` `palette`).
5. Ice migration pass (`biome.march`), alongside the existing `migrations`/
   `migrations_go`.
