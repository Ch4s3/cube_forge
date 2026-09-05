# Micro-voxel models — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Small mushrooms, palm fronds and bush leaves drawn as 8x8x8 coloured sub-cube sculptures inside their cell. Design: `docs/superpowers/specs/2026-09-05-micro-voxel-models-design.md`.

**Architecture:** `CubeForge.Model` builds shapes as 512-byte palette-index grids, greedy-meshes each once into a template (ordinary 9-float vertices, cell-local positions, shade 1), and packs every template into one `Set` (a float blob plus offsets). The `Set` lives in the `World` beside `shown`, so the mesher reaches it the way it reaches the species array and no remesh call site changes. `Mesher.mesh_section_cat` for the foliage category excludes model blocks from the greedy face pass and stamps their templates afterwards, translated to the cell and shaded from the cell's own light. Colours come from a palette texture layer (89): a model quad's uv points at one texel.

**Tech Stack:** March. No C change.

## Constants

| item | value |
|---|---|
| `Model.size()` | 8 |
| palette layer | 89; `Texture.layers()` 89 -> 90 |
| template ids | mushroom sp 1..6 -> 0..5; frond dir 0..7 -> 6..13; frond umbrella 14; frond tuft 15; bush variant 0..3 -> 16..19; `Model.count()` 20 |
| face caps | mushroom 160, frond 120, bush 100 (tests) |
| palette | 0 unused; 1..6 species cap (`Species.colour_*`); 7..12 species gill (cap x 0.6); 13 stem cream (235,225,200); 14 stem shade (200,190,165); 15 soil dark (70,48,38); 16 soil light (100,72,50); 17 frond (84,150,62); 18 frond dark (60,115,45); 19 midrib (130,190,90); 20 bush (62,128,54); 21 bush light (90,160,70); 22 bush stem (90,64,40) |

Frond directions: 0..3 the axis directions +x, +z, -x, -z; 4..7 the diagonals +x+z, -x+z, -x-z, +x-z. A frond's direction is the way its blade points: away from the trunk.

## Tasks

### Task 1: `Model` — grids, palette, templates, the set (`lib/cube_forge/model.march`, `test/model_test.march`)

- `fn size() : Int do 8 end`, `fn idx(x, y, z) : Int do x + 8 * (z + 8 * y) end`, `fn count() : Int do 20 end`.
- Palette: `colour_r/g/b(k : Int) : Int` from the table; `palette_layer() : Float = 89.0`; `palette_u(k) = (k % 16 + 0.5) / 16`, `palette_v(k) = (k / 16 + 0.5) / 16`.
- Shapes, each `fn ... : NativeU8Arr` of 512: `mushroom(sp)`, `frond(dir)`, `frond_umbrella()`, `frond_tuft()`, `bush(variant)`. Geometry per the spec §3; the frond blade follows `y(t) = 7 - t * t / 12` for `t` 0..7 along its direction, three wide, one thick, midrib colour on the centre line. The umbrella is four axis blades of length 4 from the centre; the tuft a plus at y 7, a ring at distance 2 at y 6 and distance 3 at y 5.
- `shape(id : Int) : NativeU8Arr` picks by template id.
- Template builder: `template(grid) : F32Buf`: for each direction d (0..5, the mesher's order), each slice a (0..7), an 8x8 mask of the colour index where the cell is filled and its neighbour in d is empty or outside; greedy rectangles of equal index (the mesher's run_width/run_height shape at width 8); each rectangle six vertices at positions scaled by 1/8 with the palette uv, `palette_layer()`, shade 1.0, the direction's face float, `Vx.plain()`.
- `Set = Set(NativeF32Arr, NativeIntArr)`: blob of every template's floats in id order, offsets array of `count() + 1` (offset k .. offset k+1 is template k). `fn build() : Set`; `fn floats(s, k) : Int`; `fn at(s, i) : Float`; `fn offset(s, k) : Int`.
- `fn faces(s, k) : Int` = floats / (6 * 9).
- Tests: every template's vertex positions are within [0, 1]; face counts under the caps for all twenty; no template has zero faces; a mushroom of species 5 uses palette index 5 on its top face and 11 below the cap (find a vertex with face_top whose y is the cap top; find one with face_bottom at the cap underside); the frond template for dir 0 has its highest vertices at x <= 1/8 and lowest at x >= 6/8 (the blade rises at the trunk side and droops outward); dir 2 is the mirror; the tuft and umbrella are non-empty; bush variants differ from each other (different float counts or a differing vertex); `palette_u(17)` is 17.5 / 16 - 1 = 0.09375.

### Task 2: the palette layer and the set in the world (`texture.march`, `world.march`, tests)

- `Texture`: `palette_go(a, i)` writes layer 89 from `Model.colour_*`, alpha 255; `layers()` 90; array sized 90; the doc comment lists 89 palette. `layer_for` unchanged (no block id maps to it).
- `World`: a seventh field `CubeForge.Model.Set`; `fn models(w) : Model.Set`; `from_chunks` builds `Model.build()`; every `World(...)` match in world.march gains a `_`; test constructors (`fruit_test`, `effects_test`, `veg_test`, `biome_test`, `flow_test`, `light_test`) gain `CubeForge.Model.build()` — put the models argument **last**, after `side`.
- The four `layers() == 89` assertions become 90.
- Test (`myc_block_test` or `model_test`): texel 17 of layer 89 in `Texture.checkerboard()` is (84, 150, 62, 255).

### Task 3: stamping in the mesher (`mesher.march`, `chunk_mesh.march`, `greedy_test.march`)

- `Mesher.is_model(c, n, s, e, w, x, y, z, id) : Bool`: `C.is_fruit_small(id)`, or `id == C.palm_frond()`, or `id == C.oak_leaves()` standing on ground (the block below is not air, not foliage, not a log, not water). Nested ifs (G33).
- `face_key` returns 0 for a model block (so the greedy pass emits no cube for it). `face_visible` unchanged: neighbours still see through it.
- `template_of(c, n, s, e, w, x, y, z, id, oxi, ozi) : Int`: mushroom -> `C.fruit_species(id) - 1`; frond -> orientation from the four horizontal neighbours: a palm log among them gives the direction away from it (6 + dir); else fronds among them give away from the sum of their offsets (axis when one component is zero, diagonal otherwise); else a palm log below gives 14; else 15. Bush -> 16 + `N.hash2(oxi + x, ozi + z, 4242) * 4`.
- `stamp(b, set, k, fx, fy, fz, shade) : F32Buf`: for each vertex of template k, push position + (fx, fy, fz), uv, layer, `shade`, face, fx as stored. `shade = Vx.pack_shade(sky / 15, blk / 15)` from `Light.get(la, oxi + x, y, ozi + z)` and `lb` likewise.
- `mesh_section_cat` gains `models : Model.Set`; for the foliage category it runs `stamp_go` over the section's 4096 cells after the greedy pass. `mesh_section_foliage` and `mesh_section_opaque` gain the parameter; `chunk_mesh` passes `World.models(world)`.
- Tests (`greedy_test`): a section with one small mushroom of species 3 at (5, 9, 5) emits exactly `Model.floats(set, 2) / 9` vertices, every x in [5, 6], y in [9, 10], z in [5, 6]; the same section with a stone slab under the mushroom still emits the slab's top quad in the opaque pass (the model does not hide it); an oak leaf at (5, 10, 5) on stone at (5, 9, 5) stamps a bush template (vertex count equals one of the four bush templates') and the same leaf with air below emits 36 or fewer vertices of cube faces (a lone cube); a frond at (6, 9, 5) beside a palm log at (5, 9, 5) stamps template 6 (dir +x), a frond at (4, 9, 5) stamps 8 (dir -x), a frond at (5, 10, 5) above the log stamps 14, a lone frond 15.

### Task 4: run, measure, decide on bushes

- `forge test`; release build.
- Seed 7 spawn: `CF_FRAMES=30 CF_DUMP_FRAME=20 CF_PITCH=-140` over a Lanterncap patch (`CF_WILD=0 CF_AUTOPLANT=100 CF_AUTOPLANT_SPECIES=4 CF_AUTOPLANT_MATURE=1 CF_FRUIT_RATE=2000`, frame 800): frame dump of the mushrooms; `docs/models-mushrooms.png`.
- Foliage vertex totals: the `mesh all` line prints vertices; add foliage to the startup line if it is not there, before and after (base worktree at 2c543ad).
- Section remesh time: `CF_AUTOBREAK` under a bush, `biome veg` timings; before and after.
- Bushes: if foliage vertices at the seed 7 spawn grow by more than 3x or a bush section remesh exceeds the frame budget's slice, bushes stay cubes (`is_model` drops the leaf case) and the spec's *As built* says so. Otherwise they ship.
- `RESULTS.md`, spec *As built*, `todos.md` ticked.
