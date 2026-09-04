# todos.md — feature backlog

Planned, not started. One entry per feature; each gets its own design doc under
`docs/superpowers/specs/` when picked up.

## Water

- [x] **Static water** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-static-water-design.md`).
- [x] **Spreading water** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-spreading-water-design.md`); one `WaterChunk` actor per chunk, pull-based levels, edge mirrors.

- [x] **Hotbar + inventory + hold-to-break** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-hotbar-inventory-design.md`).

- [x] **Ground-level spawn + collectable water + top-down map view + swim-underwater targeting fixes** — done 2026-09-03.

- [x] **Layered terrain (ruggedness + ridged mountains), sand/snow/granite, 7-slot hotbar, random seed** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-terrain-biomes-design.md`).

- [x] **Vegetation (trees and bushes)** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-vegetation-design.md`): oak and pine as cubes with alpha-cutout leaves, deterministic cell-based placement that agrees across chunk borders, a third (foliage) mesh category with a `discard` shader path, leaf decay when a trunk is chopped, 9-slot hotbar. Costs 5.5x more startup meshing (see RESULTS.md).

- [x] **Bedrock floor** — done 2026-09-03: unbreakable, three layers deep, generated in every column.

## Engine

- [ ] **Main-thread pinning runtime patch** (GAPS.md G15) — chip spawned, in progress in a separate session.
- [x] **Greedy meshing + sections + `blit` stdlib candidate** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-greedy-sections-blit-design.md`): 6x fewer vertices, section remesh 3–5x faster; blit exists as `cf_f32_blit` + `F32Buf.append`.
- [x] **Lighting** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-lighting-design.md`, plan `docs/superpowers/plans/2026-09-03-lighting.md`): flood-fill skylight, smooth per-vertex light + AO with quad flip packed into the greedy key, bounded incremental relight, a moving sun and moon on a 30-minute cycle with warm low-angle light and tinted moonlight (`CF_DAY`, `CF_SUN`), camera flashlight (F, `CF_FLASH`). Vertex layout 7 -> 8 floats. GAPS G63/G64.
- [x] **Dynamic shadows** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-dynamic-shadows-design.md`): voxel DDA ray-march against a 3D occupancy texture; sun, moon and flashlight all cast, reach via `CF_SHADOW_DIST` (default 64). No FBOs, no shadow maps, no depth bias.
- [ ] **Texture atlas via stb_image** — real block textures; needs `Bytes` across FFI fixed first (GAPS.md G8).
- [ ] **Chunk streaming with actors** — infinite world, chunk lifecycle as actor state, supervision under load.
- [ ] **Save/load through the CAS** — persistence keyed by BLAKE3 chunk hashes.

## Compiler patches (from GAPS.md "Compiler patches")

- [ ] `march_bytes_borrow` unwrap (G8) — one line + test.
- [ ] Zero-arg extern dead-binding drop (G16).
- [ ] `dec_rc` for NativeArray bindings (G31).
- [ ] Aggregate RC for tuples/records (G28–G30) — the big one.
