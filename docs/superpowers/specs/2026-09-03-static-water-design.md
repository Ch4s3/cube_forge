# Static water — design

Sub-project 1 of 2 for water (sub-project 2, spreading water, is in `todos.md`
and depends on this one). Goal: a sea level with translucent water, placeable
water blocks, and swimming physics, while keeping the frame loop
allocation-free and the C shim minimal. Dogfooding March remains the point:
every friction point goes into `GAPS.md`.

## 1. World data and generation

- Block ids: 0 air, 1 grass, 2 dirt, 3 stone, **4 water**.
- `Chunk.is_solid(id) = id != 0` is unchanged (used by meshing).
  New `Chunk.is_collidable(id) = id != 0 && id != 4` is used by physics.
  Raycast selection keeps hitting water so it can be broken.
- `World.sea_level() = 62`. `Chunk.fill_column` fills air from the terrain
  top up to sea level with water. If the current heightmap (58..84) yields
  too few lakes, lower the terrain base in `Noise.height` by 4; do not move
  sea level.
- Placement: keys 1–4 select the block to place (grass, dirt, stone, water);
  right-click places the selected block, left-click breaks. Water is a plain
  block; nothing spreads.

## 2. Meshing

- `Mesher.mesh_with_neighbors` returns `Meshes(F32Buf, F32Buf)` — opaque and
  water — a two-field variant, never a tuple (GAPS.md G29). Both buffers are
  threaded through the face loop so the loop stays allocation-free apart
  from growth.
- Face rules, per block against each of its six neighbours:
  - opaque block vs air or water → opaque buffer (lake beds show through);
  - water vs air → water buffer; the water top face is lowered by 1/8 block;
  - water vs water, and anything vs opaque → culled.
- Water uses texture-array layer 1: a 16x16 procedural blue, alpha 150,
  generated in `Texture` beside the checkerboard. The array has 2 layers.
- Slots: opaque mesh of chunk i in slot `i`, water mesh in slot `64 + i`.
  One `NativeIntArr` of 128 vertex counts, indexed by slot.
- `remesh_at` uploads both slots and updates both counts; the neighbour
  remesh rule on chunk boundaries is unchanged.

## 3. Rendering (all shim changes)

- Fragment shader: `o_color = vec4(base * shade, tex.a)` when textured,
  alpha 1 otherwise. Opaque layers carry alpha 255.
- New `cf_gfx_draw_translucent(slot, nverts)`: `GL_BLEND` with
  `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, `glDepthMask(GL_FALSE)`, culling off,
  depth test on; draws, then restores state.
- Frame order: 64 opaque slots, 64 water slots (unsorted), outline, HUD.
- Underwater tint: when the eye cell is water, draw a full-screen quad via
  the HUD path with layer 1 and shade 0.6. `cf_gfx_draw_hud` gains a
  `textured` flag (samples the array, blending on). The quad is built once
  into a persistent `F32Buf` in slot 253.
- Everything stays inside the GL 3.3 core / GLES 3.0 intersection.

## 4. Physics

`Player.update` takes an extra `in_water : Bool`, computed by the caller as
"block at the feet or at the eye is water" (two `World.block_at` reads).
When in water: gravity −4 (not −24); vertical speed clamped to [−3, +3];
Space adds +4 m/s upward every frame (swim), not the ground-gated jump;
horizontal wish speed halved. `box_hits`/`hits_go` use `collidable_at`, so
water never blocks movement and walking out of a lake works unchanged.

## 5. Verification

- `forge test`: `is_collidable`; a single water cell meshes to 6 water faces
  with the top at y + 7/8; a one-opaque-block-over-water fixture gives the
  expected opaque/water face counts.
- Scripted (headless-verifiable, like M1–M5): `CF_AUTOEDIT` gains a
  place-water step; `CF_AUTOSWIM=1` spawns over a lake, falls in, holds
  Space for 60 frames, prints y per 20 frames (must rise) and `in_water`
  (must be true). Frame dumps at the surface and underwater; the underwater
  dump's centre pixel must be blue-tinted.
- `CF_ALLOC_PROBE`: frame-loop live-object delta stays ≤ 1 per frame.
- New friction goes into `GAPS.md` as G42+.

## Out of scope

Spreading/flow, waves or animation, water lighting, sorting of water faces,
buoyancy beyond the velocity clamp.
