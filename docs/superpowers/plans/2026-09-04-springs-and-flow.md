# Springs and Flowing Water Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finite water and infinite springs as distinct items, springs as a rate-capped trickle balanced against evaporation, flow direction in the vertex, an animated water surface, and spray — per `docs/superpowers/specs/2026-09-04-springs-and-flow-design.md`.

**Architecture:** Items are block ids, so "water" is id 5 and "spring" is id 4 — the split is `yield_of` plus an icon mapping. The trickle and evaporation are two branches in `Water.process`. Flow rides in the vertex `fx` word (effects 2-11, speed in the alpha byte) and a `u_time` uniform animates it. The shim owns the spring list and the spray emitters, next to the particle pool it already owns.

**Tech Stack:** March (forge), OpenGL 3.3 via `native/cf_shim.c`.

## Global Constraints

- `spring_rate` (knob `CF_SPRING_RATE`, default 1): a source gives at most this many units per processing, summed over neighbours. Below sea level unchanged.
- Evaporation: a level-1 volume cell with air directly above loses its unit with probability `1/evap` per processing (`CF_EVAP`, default 16), hash `Noise.hash2(idx, qh, 77)` so it is deterministic. Sources never evaporate.
- `fx` effects: 2-9 = compass directions in the order `+x, +x+z, +z, -x+z, -x, -x-z, -z, +x-z`; 10 = still; 11 = falling. Alpha byte = `speed * 255 / 7`. Shader forces alpha 1 for effect ≥ 2.
- `CF_TIME` pins `u_time`; every dump comparison in this plan uses `CF_NOMOUSE=1 CF_TIME=0`.
- Texture layers: 15 = spring icon, 16 = foam. `layers()` 15 -> 17.
- Every `NativeArray` write is threaded (GAPS G70); C owns anything written per element per frame (G68).
- Cycle per task: `forge check && forge build && forge lint --strict && forge test`.

---

## Task 1: Water items and the trickle

**Files:** `lib/cube_forge/inventory.march` (`yield_of`), `lib/cube_forge/texture.march` (layer 15, `icon_layer`), `lib/cube_forge/hud.march` (two `layer_for` -> `icon_layer`), `lib/cube_forge/water.march` (`process` source cap), `lib/cube_forge.march` (`CF_AUTOCANAL`/`place_water` unchanged — already id 5), `test/flow_test.march`, `test/inventory_test.march`.

- [ ] Test (inventory): `yield_of(id_of_level(3)) == id_of_level(7)`; `yield_of(water()) == water()`. Run: fails (both yield 4).
- [ ] `yield_of`: `if C.is_source(id) do C.water() else if C.is_water(id) do C.id_of_level(7)`. Pass.
- [ ] `Texture.icon_layer(id)`: `if id == 4 do 15.0 else layer_for(id)`; layer 15 = `water_go`-style blue with a pale ring (`ring = abs(dist from centre - 5) <= 1 -> +60`); `layers()` 17 (16 reserved for foam, filled in Task 3 with `speckle_go(..., 16, 0, 226, 236, 246)` now so the array is complete). Both hud sites -> `icon_layer`.
- [ ] Test (flow): a source at (8,71,8) on a slope (floor stepping down in +x); after 1 tick `volume <= spring_rate()`; after 10 ticks `volume <= 10 * spring_rate()`. Run: fails (gives ~16/tick).
- [ ] `process`: for `src`, `let cap = spring_rate()`; falling gives `min(fell, cap)`, spreading continues with `left = cap - given` and stops at 0 — thread the remaining allowance through `spread` for sources (`l` for a source becomes the allowance, still presenting level 8 to neighbours). `spring_rate()` reads `Env.get_int("CF_SPRING_RATE", 1)` — once per source processed, a handful per tick. Pass.
- [ ] Cycle; commit `water: finite water item, spring item, and the trickle`.

## Task 2: Evaporation

- [ ] Test: closed basin from `flow_test` gains a stone roof at y = 72 over the interior -> volume still 7 after 400 ticks. Open basin (no roof), seven units -> `volume == 0` within 800 ticks. Run: second fails.
- [ ] `process`: before spreading, if `!src && l == 1 && cell(above) == 0 && Noise.hash2(idx(x,y,z), qh, 77) < 1 / evap()` -> `set_cell(..., 0)` and return. `qh` comes from the Sim (thread it into `process` from `run`). `evap()` = `Env.get_int("CF_EVAP", 16)`. Pass.
- [ ] Cycle; commit `water: thin sky-exposed water evaporates`.

## Task 3: Flow in the vertex, and the shader

**Files:** `lib/cube_forge/vertex.march` (effect ids + `pack_flow`), `lib/cube_forge/mesher.march` (`vert_fx`, `flow_of`, `water_faces`), `native/cf_shim.c` (`u_time`, VS bob, FS scroll/drift/foam/alpha), `lib/cube_forge/ffi/window.march`, `lib/cube_forge.march` (set time; `CF_TIME`), `test/vertex_test.march`, `test/greedy_test.march` or new `test/flow_vertex_test.march`.

- [ ] Test: `Mesher.flow_of(here, px, mx, pz, mz, below_air)` over levels: all equal -> `(10, 0)`; `+x` lowest by 3 -> `(2, 3)`; `-z` lowest -> `(8, d)`; `below_air` -> `(11, 7)`. `Vertex.pack_flow(dir, speed)` round-trips through `effect_of`/`alpha_of`. Run: fails.
- [ ] Implement: `Vertex.fx_still() = 10`, `fx_fall() = 11`, `fx_dir(k) = 2 + k`, `pack_flow(e, speed) = pack(e, speed / 7)`. `Mesher.flow_of` returns the pair packed `e * 16 + speed`. `vert_fx` = `vert` with an fx parameter; `water_faces` computes `flow_of` once per cell and passes `pack_flow` to every water vertex it emits. Pass.
- [ ] Shader: `uniform float u_time`; VS: `int fe = int(a_fx + 0.5) >> 8; if (fe >= 2) pos.y += 0.03 * sin(u_time * 1.7 + a_pos.x * 1.3 + a_pos.z * 0.9);`. FS: `DIRS[8]` table; for `fe` 2..9 `uv += DIRS[fe-2] * u_time * (0.05 + speed * 0.04)`; `fe == 10` `uv += vec2(0.02, 0.013) * u_time`; foam: `float foam = fe == 11 ? 1.0 : (fe >= 2 && fe <= 9 ? speed / 7.0 : 0.0); t = mix(t, texture(u_tex, vec3(uv, 16.0)), foam * 0.7);` where `speed = float(fx & 255) / 255.0 * 7.0`; `a = (fe >= 2) ? 1.0 : a`. `cf_gfx_set_time(t)`; March sets it each frame from `Win.time()` unless `CF_TIME >= 0` pins it (`Knobs` gains an `Int`, seconds; `-1` = live).
- [ ] Verify: `CF_TIME=0` twice -> identical; `CF_TIME=0` vs `CF_TIME=3` -> water pixels differ, land does not (count differing pixels under the lake vs a land crop). Save `docs/water-flow.png`.
- [ ] Cycle; commit `water: flow direction in the vertex, animated surface`.

## Task 4: Generated springs, and the balance

**Files:** `lib/cube_forge/chunk.march` (`plant_springs_go` in `plant`), `lib/cube_forge/trees.march` (salt-6 draw exposed as `spring_at_cell`), `native/cf_shim.c` (`cf_spring_set`), `lib/cube_forge.march` (startup scan; `edit_block` hook on id 4 above sea level), `test/terrain_test.march`.

- [ ] Test: for seed 7, `Trees.spring_at_cell` returns a column only where `Noise.slope >= 2 && Noise.height >= 72`; a generated chunk holding one has id 4 at that column's surface. Run: fails.
- [ ] Implement: `spring_at_cell(cx, cz, seed, density_per_mille)`; `plant` composes `plant_springs_go` after bushes. Density from `Env.get_int("CF_SPRING_DENSITY", 40)` read once in `World.generate` and passed down (generation already threads `seed`; add a parameter). Pass.
- [ ] Shim: `static int32_t g_springs[512][3]; g_nsprings`; `cf_spring_set(x, y, z, on)` adds/removes. March: after `Biome.build` at startup, scan columns: `block_at(x, h, z) == 4 && h > sea_level` -> `Win.spring_set(...)`; `edit_block`: when `id == 4` or the replaced block was 4, above sea level, call it.
- [ ] **Balance (§8).** Add a verbose print on the water tick: `active cells` = sum of `pending` over chunks after the tick. Run `CF_NOMOUSE=1 CF_TIME=0 CF_AUTOFLOW=9999 CF_FRAMES=900` for each of: rate 1/2, evap 8/16/32, density 20/40/80. Record active cells at frame 890 and the water tick ms in `RESULTS.md`; pick defaults hitting a 15-40 cell brook and < 2 ms. Dump the terrain map with a spring in view: `docs/spring-brook.png`.
- [ ] Cycle; commit `water: generated springs, balanced against evaporation`.

## Task 5: Spray

**Files:** `native/cf_shim.c` (kind-2 particles, emitters), `lib/cube_forge/ffi/window.march`, `lib/cube_forge/water.march` (kind-4 reply on drops), `lib/cube_forge.march` (`apply_reply` branch, burst on spring placement, per-frame emitter flush), `test/flow_test.march`.

- [ ] Test: a source over a two-block pit: the tick's reply contains a `kind_drop()` (= 4 * 2^32) entry whose payload is the landing cell's idx; a source over a one-block step: none. Run: fails.
- [ ] `give`: when `dy == -1` and the receiver is in-chunk and `cell(dx, dy - 1, dz) == 0`, append `kind_drop() + idx(dx, dy, dz)`. `apply_reply`: `kind == 4` -> `Win.spray_at(wx, wy, wz, 3)`. Pass.
- [ ] Shim: second particle kind in the pool: `g_spray` (cap `CF_SPRAY`, default 512; 6 floats: x, y, z, vx, vy, vz, plus life in a parallel u8). `cf_spray_at(x, y, z, n)` queues n spawns at (x+0.5, y+1, z+0.5) with upward random velocity; `cf_spray_frame(dt)` integrates (gravity, life 0.6 s), spawns two per spring above sea level from `g_springs`, and uploads white quads (alpha from life) into slot 247; drawn via `gfx_draw_precip` after the rain. Placement burst: `edit_block` with `id == 4` above sea level -> `spray_at(x, y, z, 40)`.
- [ ] Verify: `CF_WEATHER=0 CF_TIME=0` dumps with and without `CF_SPRAY=0` differ only near a spring/drop; `RESULTS.md` records the spray frame cost.
- [ ] Cycle; commit `water: spray at drops and spring mouths`.

## Self-Review

Spec §1 -> T1; §2 -> T1; §3 -> T2; §4 -> T4; §5 -> T3; §6 -> T3; §7 -> T5; §8 -> T4 balance step; §10 verification: items (T1 test), rate (T1), roof/open (T2), fx unit (T3), kind-4 (T5), CF_TIME identity (T3), hotbar id 4 only from a spring (T1 yield test). Names used across tasks: `spring_rate()`, `evap()`, `Vertex.pack_flow`, `Mesher.flow_of`, `Win.spring_set`, `Win.spray_at`, `kind_drop()`.
