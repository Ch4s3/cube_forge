# Dynamic shadows — voxel ray-marched, moving with the sun, moon and flashlight — design

The lighting work that shipped gives the sun a moving *direction* but no
occlusion: `N.L` darkens faces that turn away from the light, and the baked
skylight darkens anything with no sky above it, but nothing casts a shadow. An
overhang shades the same cells at noon and at dusk, and the flashlight shines
through walls.

Scope: shadows that move, cast by the **sun**, the **moon** and the
**flashlight**. Hard-edged. Reach 64 blocks.

## 1. Approach: ray-march, not shadow maps

A voxel DDA in the fragment shader, against the world's opacity uploaded as a
3D texture. Rejected alternatives:

- **Shadow maps** — an orthographic map for the sun/moon plus a perspective one
  for the flashlight: two FBOs, two extra full-geometry passes per frame over
  244k vertices, light-space matrices on the March side, and depth-bias tuning,
  which axis-aligned voxel geometry is especially prone to fighting. More
  machinery, worse fit.
- **Baking a directional flood** — re-flooding the light field from the sun's
  direction. A full flood is ~514 ms, so it cannot track a moving sun, and it
  does nothing for the flashlight, which is the light that most needs to be
  dynamic.

Ray-marching wins on three counts specific to this project: one mechanism
covers all three lights, the shim stays small (no FBOs, no depth pass, no
matrix plumbing — it is meant to be one auditable unsafe surface), and on
axis-aligned voxels a DDA is *exact*, so shadow acne, depth bias, peter-panning
and cascade seams simply do not arise.

The cost is fragment-bound and scales with resolution x reach x lights.

## 2. Occupancy field

The GPU needs to know which voxels block light, and the light field cannot
answer that: an opaque block and an unlit cave floor both read 0 there.

- A `GL_TEXTURE_3D`, 128 x 256 x 128, `R8`, `GL_NEAREST`, `CLAMP_TO_EDGE`,
  bound to texture unit 1 (unit 0 stays the block texture array). 4 MB.
- Occluder = `Light.is_opaque`, so **air and water cast no shadow**, which falls
  out of the existing `opacity` function rather than adding a second notion of
  solidity.
- **No CPU-side copy is retained.** March builds a staging `NativeU8Arr` once at
  startup, hands it to the shim and drops it. A block edit then needs only a
  one-texel `glTexSubImage3D`, because the new block id already says whether it
  occludes. This keeps a second 4 MB array off `World` and makes the edit path
  O(1) instead of O(world).

## 3. Shim additions

Two entry points, both shaped like the existing `cf_gfx_upload_texture`:

- `cf_gfx_upload_occupancy(arr, w, h, d)` — create or replace the 3D texture.
- `cf_gfx_set_voxel(x, y, z, solid)` — one texel, for block edits.

`cf_gfx_upload_occupancy` queries `GL_MAX_3D_TEXTURE_SIZE` and warns loudly if
the world does not fit: GL 3.3 only guarantees 256, and the world's Y dimension
is exactly 256. Every real GPU is far above this, but silently rendering
garbage is not an acceptable failure mode.

## 4. The trace

A standard Amanatides-Woo voxel DDA:

```
float trace(vec3 p, vec3 dir, float maxDist)   // 1.0 = lit, 0.0 = blocked
```

The ray starts at the fragment's world position pushed half a voxel along
`v_normal`, so it begins in the air cell adjacent to the face and cannot
self-intersect the surface it came from. This is the voxel equivalent of depth
bias, except it is exact rather than tuned.

The loop is bounded by both `maxDist` and a hard step cap, so a grazing ray can
never spin.

**At most two traces per fragment.** The sun and the moon are opposite ends of
one arc, so only one is ever above the horizon: one directional trace, plus one
toward the eye for the flashlight.

Four early-outs, all free, remove most of the average cost:

- `ndl <= 0` — the face already points away from the light.
- the light's intensity is 0 — nothing to shadow.
- the flashlight is off, the fragment is outside its cone, or beyond its range.
- the baked skylight is 0 — a sealed cave, already black.

`CF_SHADOW_DIST` sets the reach in blocks (default 64; `0` disables shadows
entirely), so the cost is measurable and shadows can be switched off.

## 5. Composition with the existing lighting

The trace gates only the **direct** term, never ambient:

```
sky   = sunc * inten * (amb + DIRECT * ndls * shadow)
      + MOON_TINT * (MOON_LEVEL * moon * (amb + DIRECT * ndlm * shadow))
flash = ... * traceToEye
```

Sky ambient is not coming from the sun, so shadowing it would be wrong twice
over: it would push shadowed ground to pure black, and it would double-darken
surfaces whose baked skylight is already low. Keeping the two separate is what
lets the existing per-vertex skylight go on doing its job — caves dark,
outdoors bright — while crisp moving shadows ride on top.

## 6. Verification

The DDA lives in GLSL and cannot be unit-tested directly. Two layers instead:

- **March-side properties.** The occupancy staging buffer holds 1 at index `i`
  exactly when `Light.is_opaque(block at i)`, checked over a generated world;
  and a block edit maps to the right voxel index.
- **An end-to-end pixel test.** Build a pillar on flat ground, pin the sun low
  with `CF_SUN`, dump the frame, and assert that ground on the shadowed side of
  the pillar is measurably darker than ground on the lit side, and that the
  difference disappears with `CF_SHADOW_DIST=0`. This is a regression test, not
  a screenshot to eyeball.

## 7. Risks

- **Fragment cost** is the main one. Mitigated by the bounded reach, the four
  early-outs and the `CF_SHADOW_DIST` knob; measured before and after, recorded
  in `RESULTS.md`.
- **Hard edges only.** Soft shadows would need multiple rays per fragment and
  are deliberately out of scope.
- **Shadows do not affect the baked skylight**, so a block edit still does not
  need a relight or remesh for shadows to update — they are entirely dynamic.
