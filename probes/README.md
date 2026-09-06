# Probes

Small standalone forge projects / files that reproduce each finding in
`GAPS.md`. Each directory is a complete forge project: `cd` in and run
`forge build && ./.march/build/debug/<name>` (or `march --check <file>`).

- `fieldcall.march` — G3, field access on a call result does not parse
- `probe1/` — G8 (Bytes over FFI), G9 (NativeArray over FFI), G12 (record vs variant allocation)
- `externmod2/` — G6, qualified extern call with args → "Unknown module"
- `externmod3/` — G5 (qualified extern → link failure), G11 (caller needs no cap)
- `probe_dce/` — G16, zero-arg extern call bound to an unused name is dropped
- `pin_main/` — G15, is `main` on the OS main thread (`pthread_main_np`) and does `pmap_n` stay parallel; run with/without `MARCH_PIN_MAIN=1` and `MARCH_NUM_SCHEDULERS=1`
- `probe_unit/`, `unit_extern.march` — G23, `Unit` vs `()` (not reproduced in isolation)
- `refine_index.march` — G22, refinement checker coverage for the voxel index
- `dotted_pat.march`, `dotted_pat2.march` — G24/G25, dotted constructor patterns
- `oneline_match.march` — (control) one-line match with tuple patterns parses
- `actor_probe/` — control: native arrays in actor state, List reply, Actor.call inside pmap all work
- `probe_ffi5/` — G51, an extern returning its borrowed argument is a use-after-free; `consume` fixes it
- `pvec_get/` — G82, `Array.get`/`Array.set` on a 64-element PVec against a complete binary tree: 112 vs 72 ns a get, 860 vs 200 ns a set
- `array_field_drop.march` — G79 control: the same cell shape in the MAIN module frees correctly (6 MB resident)
- `drop_xmod/` — G79/G80/G81: a library-module variant with array fields churned by bare and qualified name (WHICH=1,2), a persistent vector replacing 64 KB elements (WHICH=4), a closure capturing 1 MB (WHICH=5); run with `/usr/bin/time -l`

