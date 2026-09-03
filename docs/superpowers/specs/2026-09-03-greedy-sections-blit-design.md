# Greedy meshing, section remesh, and `blit` — design

Motivation (RESULTS.md): a whole-chunk remesh costs ~7 ms and every water
tick or block edit pays it for a handful of changed cells; the naive mesher
also emits one quad per face (455k vertices for 64 chunks).

## 1. Sections

- A chunk mesh is 16 vertical sections of 16x16x16 voxels (section `sy` =
  y / 16). Each section has its own opaque and water `F32Buf`.
- A chunk keeps `ChunkMesh(opaque_sections, water_sections, opaque_upload,
  water_upload)`: two `Array.PVec` of 16 buffers, and two persistent upload
  buffers that are rebuilt by concatenating the sections (`F32Buf.append`)
  and uploaded to the chunk's two existing slots. Draw calls are unchanged
  (one per chunk per pass).
- Remesh granularity becomes the section: a block edit rebuilds section
  `y/16` (and the neighbour chunk's same section when on a chunk border, and
  the section above/below when `y % 16` is 15/0 within the same chunk); a
  water tick rebuilds the set of sections named by the cells in the reply.

## 2. Greedy meshing (opaque only)

- Per section, per face direction (6), per slice along that axis: build a
  16x16 mask of face keys (block id when the face is visible, 0 otherwise),
  then merge maximal rectangles of equal key. One quad per rectangle, with
  uv = (w, h) so the repeating texture tiles per block. Shade is per
  direction as before.
- Water keeps the per-cell mesher (levels make faces non-uniform), restricted
  to the section's y range.
- The mask is a `NativeIntArr(256)` reused across slices and directions.

## 3. `blit` — the stdlib candidate

- `NativeArray` has no copy. The shim gains one entry point,
  `cf_f32_blit(dst, di, src, si, n)`, which memcpy's `n` floats and requires
  `dst` to be uniquely owned (rc == 1; it aborts with a message otherwise —
  the same in-place-at-rc==1 contract `set_*` uses, made explicit).
- `F32Buf.append(dst, src)` grows `dst` if needed and blits `src`'s live
  prefix onto it. This is the concatenation step for uploads.
- The C body is the proposed `native_f32_arr_blit`; it belongs in
  `runtime/march_runtime.c` with a March wrapper in `stdlib/native_array.march`.

## 4. Verification

- Tests: a flat 16x16 slab meshes to 1 top quad (6 vertices) greedy vs 256
  naive; a checkerboard slab meshes to 128 quads; greedy and naive agree on
  which blocks are covered for a random-ish section (vertex sets differ, face
  areas equal — checked by summing quad areas); `append` correctness; section
  remesh of one section leaves the other 15 buffers untouched.
- Timing in RESULTS.md: full greedy chunk mesh vs naive; vertex counts for
  the 64-chunk world; section rebuild time; edit and water-tick apply cost
  before/after; fps.
- Frame-loop allocation stays at ≤ 1 per frame outside ticks.

## Out of scope

Greedy water, index buffers, per-cell mesh patching, ambient occlusion.
