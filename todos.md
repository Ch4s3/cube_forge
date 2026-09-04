# todos.md — feature backlog

Planned, not started. One entry per feature; each gets its own design doc under
`docs/superpowers/specs/` when picked up.

## Water

- [x] **Static water** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-static-water-design.md`).
- [x] **Spreading water** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-spreading-water-design.md`); one `WaterChunk` actor per chunk, pull-based levels, edge mirrors.

- [x] **Hotbar + inventory + hold-to-break** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-hotbar-inventory-design.md`).

- [x] **Ground-level spawn + collectable water + top-down map view + swim-underwater targeting fixes** — done 2026-09-03.

## Engine

- [ ] **Main-thread pinning runtime patch** (GAPS.md G15) — chip spawned, in progress in a separate session.
- [x] **Greedy meshing + sections + `blit` stdlib candidate** — done 2026-09-03 (`docs/superpowers/specs/2026-09-03-greedy-sections-blit-design.md`): 6x fewer vertices, section remesh 3–5x faster; blit exists as `cf_f32_blit` + `F32Buf.append`.
- [ ] **Lighting** — flood-fill sky/block light, per-vertex light in the 7-float layout; exercises `NativeU8Arr` at scale.
- [ ] **Texture atlas via stb_image** — real block textures; needs `Bytes` across FFI fixed first (GAPS.md G8).
- [ ] **Chunk streaming with actors** — infinite world, chunk lifecycle as actor state, supervision under load.
- [ ] **Save/load through the CAS** — persistence keyed by BLAKE3 chunk hashes.

## Compiler patches (from GAPS.md "Compiler patches")

- [ ] `march_bytes_borrow` unwrap (G8) — one line + test.
- [ ] Zero-arg extern dead-binding drop (G16).
- [ ] `dec_rc` for NativeArray bindings (G31).
- [ ] Aggregate RC for tuples/records (G28–G30) — the big one.
