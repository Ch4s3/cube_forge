# Extracting the voxel engine

A plan for separating the reusable voxel engine from cube_forge's game
content. Written after a read of the module graph, the FFI surface and the
places where the low-level modules reach upward. Nothing here has been
implemented.

## The constraint that shapes everything

`forge` resolves `[deps]` transitively — `path = "../foo"` works, and
`MARCH_LIB_PATH` walks a dependency's own dependencies. But `forge build`
reads `[ffi] sources` from **the current project only**. Nothing walks a
dependency's FFI section. So a March library package cannot ship its own C
shim: the consuming app must declare the engine's `.c` files and its
`-lglfw` link flags in its own `forge.toml`.

That is survivable — the engine ships `native/` in its tree and documents a
four-line `[ffi]` block consumers paste — but it means "packaged engine" can
never be a pure `forge deps` install today. Worth filing in `GAPS.md`
alongside the others, because it is the single thing standing between this
plan and a clean package.

Everything below assumes that workaround.

## Target shape

Three layers, in dependency order:

**`march_voxel` (the engine)** — chunk storage and the streaming window, the
light sweep, the greedy mesher, raycast, swept-AABB collision, the GL
renderer and windowing, the save container format, math, noise, the
persistent tree, the vertex/buffer plumbing.

**`cube_forge` (the game)** — the block palette and its material properties,
terrain generation (biome, caves, lakes, trees, veg), the simulation systems
(water, myc, fruit, weather, wind), audio and tone, the UI (hud, menu,
inventory, settings), and the frame loop that drives the engine.

**The seam** — a block registry the game populates and the engine reads, a
terrain-generation interface the game implements and the engine calls, and a
renderer effect-id protocol the game uses to tag geometry.

The engine is the layer that should be able to render a world of coloured
cubes with no knowledge of grass, palm fronds or mycelium. Today it cannot,
and that is the whole job.

## Phase 0 — Lock in the safety net

Before touching anything, make the existing verification reproducible as a
single command, because it will run thirty times.

The harness already exists and is unusually good for a refactor of this
size: 456 tests, scripted headless runs (`CF_AUTOWALK` for 2400 frames
crossing a window shift), a save/load round trip that compares a world hash,
and deterministic frame dumps via `CF_DUMP`/`CF_DUMP_FRAME`.

Add to that a **rendering regression check**: capture a fixed set of
screenshot parameter sets as a golden set and diff new dumps against them.
Most of the risk in phases 2 and 3 is visual — a mesher or lighting
regression that no unit test catches but that shows up instantly as a
changed pixel. A byte-compare of BMP dumps at fixed seed, sun angle and
frame number is a strong signal.

Two trap doors to build into the script:

- Always run `forge build --release`, not just `forge check`. Release treats
  unverifiable refinement obligations as errors; check does not. Several
  phases move code carrying `{Int | 0 <= _ && _ < 65536}`-style refinements,
  and a lost Z3 discharge should surface immediately.
- Always run `forge lint --strict`. Moving functions between modules orphans
  private ones, and `dead-code/unused-private-fn` will catch it. More
  importantly, once engine code lives in a library, `safety/no-panic-in-lib`
  applies to all of it — the same rule that forced `Tree` to become total
  with a fill value. Every registry lookup must have a total answer for an
  out-of-range id.

**Done when:** one command runs check, release build, strict lint, the test
suite, both autowalks, the save round trip and the golden-image diff, and
reports pass/fail.

## Phase 1 — Split the FFI module by capability

The cheapest phase with the highest immediate payoff, and it goes first
because it makes the rest legible.

`CubeForge.Ffi.Window` is currently four unrelated things behind one name:
windowing, the GL renderer, generic native memory primitives, and two
game-specific map overlays. The consequence is severe and easy to miss:
`World`, `Light`, `Biome`, `Myc` and `F32Buf` all alias it — and `F32Buf`
declares `needs Window` — purely to reach a fast `memcpy`. Terrain
generation transitively depends on GLFW.

Split into four modules, each with its own extern block. The project already
establishes this pattern; the manifest comment notes that two extern blocks
split the shim into the Window and Input capability domains, so a four-way
split is the same move.

- **`Ffi.Mem`** — the blits (`u8_blit`, `f32_blit`, `f64_blit`), the box
  operations (`u8_zero_box`, `mark_box`, `f32_stamp`), the vertex packers
  (`mesh_quad`, `mesh_slice`), and the diagnostics (`arr_rc`, `rss_bytes`,
  `live_allocs`). Capability: `IO.Foreign` only. **No `needs Window`.**
- **`Ffi.Window`** — open, close, swap, time, framebuffer size, vsync,
  fullscreen, should-close, request-close.
- **`Ffi.Gfx`** — everything `gfx_*`: init, the frame begin, the draw calls
  per pass, uploads, the uniform setters, the slot shift, the pixel read and
  the BMP dump.
- **`Ffi.Particles`** — precipitation, spray and springs. These stay
  engine-side for now (a generic GPU particle pool) but the *semantics* of a
  spring belong to the game; see phase 3.

The two map uploads — biome and mycelium — move out of the FFI layer
entirely and become game code in phase 2.

The measurable result: `World`, `Light`, `Biome` and `Myc` no longer name a
window module, and `F32Buf` drops `needs Window`. That is the first real
evidence the seam is where this plan says it is.

**Risk:** low. Pure renaming plus extern-block redistribution. The failure
mode is a missed call site, which `forge check` catches instantly.

## Phase 2 — Split the C shim along the same seams

`cf_shim.c` is one 1738-line translation unit holding GLFW, the GL context,
the GLSL source, the mesh slot tables, particle physics, water spray,
springs, and two game map overlays that squat on reserved slot indices near
the top of the mesh array.

Mirror the March split:

- **`cf_mem.c`** — the blits, boxes and vertex packers. No GL include. This
  file depends on nothing else and can move first as a standalone commit.
- **`cf_window.c`** — GLFW lifecycle and input polling.
- **`cf_gfx.c`** — the context, the shader, the slot tables, the VBO pairs,
  the staged upload machinery, the draw passes, the window shift. Keeps the
  bulk of the shared static state and gains an internal `cf_gfx.h` exposing
  the slot accessors that particles and the game overlays need.
- **`cf_particles.c`** — the precipitation pool, spray and springs, talking
  to `cf_gfx.h` rather than to raw statics.
- **`cf_game_maps.c`** — the biome and mycelium overlays, which move to the
  *game's* native directory, not the engine's.

Two design questions to settle here rather than defer:

**Reserved slots.** The overlays currently claim fixed indices in the mesh
slot array. Once the overlays are game code, the engine must hand out slots
rather than have callers pick constants. Add a small reservation call: the
game asks for a slot, the engine returns an index, and the constants
disappear. This also unblocks phase 4.

**The shader.** The hardest thing to make generic and the place to accept
debt deliberately. The GLSL has water bobbing, flow ripple, foam blending
and warm block-light tinting compiled into it. But there is already a
generalization hook: vertices carry an **effect id**, with one value meaning
precipitation and a range meaning water. Formalise that. The engine owns the
shader and documents the effect-id contract — which ids are reserved, what
each does, what a game may pass through. Version one ships the existing
effects as a fixed set; that is honest and reusable enough. A later version
can splice a game-supplied GLSL fragment at a marked point, which is how
most engines solve this and is not worth building until a second game
exists.

**Risk:** medium. Static state that was file-local becomes header-exposed,
and it is easy to duplicate a definition or lose a `static`. The
golden-image diff is the check that matters — a shader or draw-order
regression is invisible to unit tests.

## Phase 3 — The block registry

The expensive phase, the one that actually makes the engine reusable, and
the one to budget a week for rather than a day.

**The problem.** `Chunk` is the type every layer depends on, and it is
simultaneously a voxel container and cube_forge's block table. It declares
grass, water, sand, snow, granite, bedrock, the log and leaf pairs for three
tree species, gravel, clay, basalt, ice, mud, glow cap, sandstone, shale,
the mycelium id range — and, stranger still, terrain-generation parameters:
sea level, talus angle, minimum talus drop, band thickness, apron width, bog
pit depth, the piled and heights side lengths.

That contamination propagates. `Light` special-cases the glow cap and the
mycelium ids to compute emission. `Mesher` carries bespoke geometry for palm
fronds and oak leaves, including the logic that orients a frond by
inspecting its neighbours for palm logs — and it reaches *upward* into
terrain generation to get a stride from the biome module.

**The design.** A `Blocks` registry the game builds once at startup and the
engine reads, backed by flat arrays indexed by block id, in the same spirit
as the existing texture layer table — which is already exactly this pattern,
built once and read by the shim per rectangle instead of calling back into
March per face. Generalize it.

The registry holds, per id: **opacity** (the value already stored in the
occupancy field — 0 for air, 2 for water, 6 for leaves, 255 for opaque),
**emission** (what the glow cap and mycelium special cases become),
**render pass** (opaque, cutout, translucent), **mesh style** (cube, cross,
frond, umbrella — what the palm and oak special cases become), and
**texture layers per face**.

Every lookup must be total, returning a documented default for an
unregistered id. The `Tree` fill-value pattern is the precedent.

**The migration, incrementally.** Not one commit. The safe order:

1. Introduce the registry type and build it from the existing constants, so
   the game produces the same table the hardcoded logic implies.
2. Convert `Light` first — smallest surface, two special cases, a dedicated
   test module.
3. Convert `Mesher` next. Keep the block-id constants as thin wrappers that
   read the registry, so call sites do not all change at once. The
   frond-orientation logic becomes a mesh style whose neighbour test is
   "same style" rather than "is a palm log".
4. Break the mesher's upward call into terrain generation by passing the
   stride in as a parameter rather than deriving it from the biome field's
   length.
5. Move the terrain-generation parameters out of `Chunk` into the game's
   terrain modules. Sea level is the interesting one — the engine may
   legitimately need a water level for the light sweep, in which case it
   becomes a world parameter rather than a chunk constant.
6. Delete the wrappers, leaving `Chunk` as dimensions plus bytes plus
   indexing.
7. `Texture` stays entirely in the game. The engine needs only the layer
   count and the table's shape to drive the texture-array upload.

**Risk:** high, concentrated in the mesher. Every step lands as its own
commit with a full harness run, because a subtle mesh-style regression on
one block type may only show in one screenshot.

## Phase 4 — Parameterise what can be, document what cannot

The window is a fixed 8x8 chunks; the mesh slot array is three passes of 64
slots each; the chunk is 16x256x16.

**Parameterise:** the window side and the derived slot budget. Both are
runtime quantities on the March side already; they are constants on the C
side. Make the shim take its slot budget at init and size its tables
accordingly, and let the world's window side be a construction parameter.
This turns view distance into a setting, which the game wants anyway.

**Do not parameterise the chunk dimensions.** The chunk's flat index carries
refinement types that let Z3 discharge the bounds statically, and the
release build treats an undischarged obligation as an error. Making the
dimensions runtime values would forfeit those proofs across the hottest code
in the engine. Keep them a compile-time constant of the engine package,
document them as such, and note that a consumer needing different dimensions
edits one module and rebuilds. A legitimate engineering position, worth
writing down rather than discovering later.

## Phase 5 — Extract an API from the frame loop

The main module is 3062 lines and 259 functions, aliases thirty modules, and
holds the frame loop. There is no engine API — the surface is implicit in
whatever this file calls.

Two interfaces need real design work, and both are callbacks from engine
into game, which is the direction March makes you think about:

**Terrain generation.** The world spawns workers to generate chunks, which
is why it declares `needs IO.Spawn`. Those workers must call into game code.
The choice is a function value passed at world construction versus an actor
protocol the game implements. The actor protocol likely fits better given
the existing worker structure and the water simulation's per-chunk actors,
but this is a decision to make with the code open rather than in a plan.

**Block modification hooks.** Every write goes through the world's set-block
and set-cells paths, which maintain the occupancy field. The game needs to
observe those writes (water spreading, mycelium growth, fruit) without the
engine knowing why.

`Player` splits: the swept-AABB collision against the world is engine;
jumping, swimming, the status effects and the inventory interaction are
game. `Save` splits similarly — the container format, chunk serialization
and the slot layout are engine; what the game stores in its per-column bytes
is not.

The frame loop itself stays in the game. That is correct — the host owns the
loop and calls the engine.

**Done when:** the engine modules can be listed and none of them names a
game module.

## Phase 6 — Make it a package

Mechanically the smallest phase, once the seams hold.

- `forge new --lib march_voxel`, placed as a sibling or vendored in-tree.
- The game's manifest gets `march_voxel = { path = "..." }`.
- The engine's manifest declares its capability ceiling: foreign calls, the
  window and input domains, and process spawning for the generation workers.
- **The FFI workaround:** the game's manifest keeps an `[ffi]` block listing
  the engine's C sources by path plus the GLFW link flags, because forge
  does not walk a dependency's FFI section. Document this prominently in the
  engine's README as the required consumer step, and file the gap.
- **Split the tests.** They already divide cleanly: the tree, math, vertex,
  light, world-size, streaming, greedy-mesher and save suites go with the
  engine; biome, caves, lakes, trees, veg, myc, fruit, water, weather, wind,
  tone, species, effects, menu, hud, inventory, settings, model, font,
  map-view and terrain stay with the game. Watch for the shared test-module
  alias namespace when files move between packages — a documented gap that
  will bite.
- Prove it: build a minimal second consumer. A few hundred lines that
  registers three block types, generates flat terrain, and renders a
  walkable world. If that program compiles against the engine with no
  cube_forge module in sight, the extraction is real. If it needs one, the
  seam is in the wrong place and that was found out cheaply.

## Sequencing and where to stop

Phases 1, 2 and 4 are mechanical, independently valuable, and land in
roughly a day each. They give a clean internal boundary and a `Biome` that
does not depend on GLFW, whether or not a package ever ships.

Phase 3 costs real time and carries real risk, and it is also the only phase
that makes the engine genuinely reusable. Everything before it is
preparation; everything after it is packaging.

Phases 5 and 6 are worth doing only if a second consumer is actually wanted.
Stopping after phase 3 leaves a well-layered app, which may be the right
answer.

## Why this is feasible

The hard architectural work is already done and is genuinely game-agnostic:
the streaming window with its eviction cache and generation hashes, the
persistent tree chunk store, the occupancy-byte light sweep, the
double-buffered staged uploads, and the per-slot GPU offset. None of that
needs redesign. What remains is decoupling, backed by 456 tests,
deterministic scripted runs and reproducible frame dumps — a stronger net
than most refactors of this size get.

The main risk is not that a phase fails. It is phase 3 sprawling: the block
registry touches the mesher, the light sweep, the textures and the save
format, and it is tempting to do it in one motion. Doing it as seven small
commits behind compatibility wrappers is what keeps it bounded.
