# GAPS.md — friction log for cube_forge

The primary output of this project. Every entry: what was needed, what March
offered, what was done instead, and what would make it clean. Entries are in
the order they were hit. "Verified" means reproduced with a runnable probe in
this repo's toolchain (march 0.3.0, forge 0.3.0, macOS arm64, 2026-09-03).

Reproduction probes live under `probes/` (copied from the session scratchpad).

---

## Language / spec

### G1. No `@noalloc` annotation exists; `cap no_alloc` is a module-wide directive
- **Needed:** per-function (or per-path) `@noalloc` on the frame loop.
- **Offered:** `cap no_alloc` is a *module-level* declaration
  (`specs/lang/capabilities.md` §"Behavioral module caps") checked by a purely
  syntactic pass (`lib/refinecheck/no_alloc.ml`) that flags `ETuple`, `ERecord`,
  `ECon` with args, and `ELam`. It does not see through calls, does not know
  about FBIP reuse (a constructor that the optimizer will reuse in place is
  still flagged), and does not know that `NativeArray.set_*` may copy.
- **Done instead:** allocation is measured empirically per phase with the
  runtime's `march_live_allocs()` gauge bound through the shim.
- **Would need:** a function-level attribute whose check runs *after* Perceus
  (so reuse counts as no-alloc) and is transitive over calls.

### G2. No `f32x8`; `@[vectorize]` is not a loop vectorizer
- **Needed:** `@[vectorize]` with `f32x8` for 8-wide noise.
- **Offered:** `Simd` is 128-bit only (`F32x4`, `F64x2`, `I32x4`, `I64x2`,
  `U8x16`; `stdlib/simd.march`). `f32x8`/`i32x8` are an open TODO
  (`specs/todos/2026-06-19-p3-language-features.md`). `@[vectorize]` is a
  *contract check* that a `NativeArray.map/map2` call's callback is eligible for
  the compiler's closure-inlining fast path — it does not vectorize arbitrary
  loops and is a hard error on a function with no `NativeArray.map` call.
- **Done instead:** noise written twice — scalar, and 4-wide with `F32x4`
  (two `F32x4` per 8 columns). See the M4 entry for disassembly.

### G3. Field access on a call result does not parse
- `mk(3).x` → `I got stuck here` pointing at `.x`; `(mk(3)).x` parses.
- Verified: `probes/fieldcall.march`.
- **Would need:** `postfix_expr: postfix_expr DOT LOWER_IDENT` to accept a
  call as its left operand.

### G4. Soft keywords cannot be parameter names in extern blocks
- `fn capture_cursor(on: Int)` → parse error at `on`. `on` is a soft keyword
  (documented asymmetry in `surface-syntax.md` §"Reserved soft-keyword
  asymmetry", but the doc says binding positions are *allowed*; extern
  parameter lists are not).

---

## FFI

### G5. An extern called qualified from another module links against a mangled name
- `Thing.live_allocs()` where `live_allocs` is an extern in `App.Deep.Thing`
  **typechecks** but the binary fails to link:
  `Undefined symbols: "_App.Deep.Thing.live_allocs"`.
  The typechecker registers externs bare ("an extern is never qualified at a
  call site", `typecheck.ml` ~12811) but codegen qualifies the reference.
- A wrapper `fn wrapped() do live_allocs() end` in the declaring module works.
- **Done instead:** every extern in `lib/cube_forge/ffi/*.march` is private
  (`cf`-prefixed C symbol) and re-exported through an ordinary `fn`. The shim
  surface is therefore declared twice.
- Verified: `probes/externmod3`.

### G6. A qualified extern call *with arguments* reports "Unknown module"
- `Thing.live_allocs()` resolves; `Thing.opn(1, 2, "x")` on the same extern
  block reports `Unknown module `Thing`. Did you mean `String`?`. A plain
  `fn open(x)` in the same module resolves qualified. The diagnostic is wrong
  about what failed.
- Verified: `probes/externmod2`.

### G7. Default extern symbol naming is `<lib>_<fn>`
- Not a bug, but the doc example (`docs/ffi.md`) shows `= "symbol"` everywhere,
  so the default only becomes visible as 14 undefined symbols at link time.

### G8. `Bytes` cannot cross the FFI boundary
- `fn bytes_len(b: Bytes): Int = "probe_bytes_len"` with
  `march_bytes_borrow(b).len` in C returns garbage (54710503744).
  `march_bytes_borrow` is `return march_str_borrow(b)` (`runtime/march_ffi.c:58`)
  but a `Bytes` value is a one-field boxed ADT cell wrapping the String
  (`stdlib/bytes.march` header comment) — the borrow reads the wrapper cell as
  a string. No test under `test/native/ffi_*` passes a `Bytes`.
- **Done instead:** vertex data crosses as `NativeF32Arr`, textures as
  `NativeU8Arr`.
- Verified: `probes/probe1` (first run).

### G9. No `march_ffi.h` accessor for NativeArray payloads
- A `NativeF32Arr`/`NativeU8Arr` extern parameter arrives as the raw heap
  pointer (works, verified), but the shim has to know the runtime layout
  (`len @16`, `data @32`) to read it. `march_ffi.h` promises "runtime internals
  stay private" and offers nothing for native arrays.
- **Would need:** `march_native_arr_borrow(v) -> {ptr, len, elem_kind}`.

### G10. `[ffi]` has no include/cflags field
- Shim sources are compiled on the same clang line as the link, so `-I` flags
  are smuggled through `link = [...]`. Works, but it is not what the key says.

### G11. Extern capability domains are declared, not enforced on callers
- `extern "cf" : Cap(Input)` plus `needs Input` in the declaring module. A
  caller module with *no* `needs Input` and no `needs IO.Foreign` that calls
  the wrapper — or the extern directly — typechecks clean
  (`probes/externmod3`). The declaring module gets
  `warning: declares needs Window but no function requires Cap(Window)`.
  So the "mesher has no business holding the input cap" split is a convention
  visible in `needs` manifests, not something the compiler will refuse.
- **Would need:** an extern block's `Cap(X)` to count as a requirement of each
  extern fn (and of its wrappers) in the demand-driven import check.

---

## Memory model / allocation

### G12. Record update allocates; single-constructor variant gets FBIP reuse
- A push loop over `type Buf = { data: NativeF32Arr, len: Int }` with
  `{ data: d, len: b.len + 1 }` costs **2 allocations per push** (debug and
  `--release`), whether the input is read by projection or destructured with a
  record pattern. The identical loop over `type Buf2 = Buf2(NativeF32Arr, Int)`
  with `match b do Buf2(d, n) -> Buf2(set(d, n, v), n + 1) end` costs **0**.
- The memory-model doc only ever shows reuse on constructors; nothing says
  records are excluded, and 2 allocations (not 1) suggests an intermediate.
- **Done instead:** `F32Buf` is a variant, not a record.
- Verified: `probes/probe1` (second run, cases A–E).

### G13. No growable native buffer in the stdlib
- `NativeArray` is fixed-length; `set_*` is in-place at rc==1 but there is no
  `push`, `resize`, `copy`, or `blit`. `Array.push` exists but `Array` is a
  32-way trie, not contiguous memory, and cannot be handed to a VBO upload.
- **Done instead:** `lib/cube_forge/f32buf.march` — `F32Buf(NativeF32Arr, Int)`
  with doubling growth. Growth is a March-level `get/set` copy loop because
  there is no `blit`.
- **Would need:** `NativeArray.blit(src, si, dst, di, n)` at minimum; ideally
  a `NativeBuf` family with the same in-place-at-rc==1 contract as `set`.

### G14. `NativeArray.fold_*` does not link when compiled
- Documented in `stdlib/native_array.march` itself; noted here because it is
  the obvious way to write "sum of a column" and it fails at link, not check.

---

## Runtime

### G15. `main` does not run on the process main thread — GLFW/Cocoa cannot be called
- `fn main` is spawned as a green thread (`march_spawn_main`) and picked up by
  whichever scheduler worker steals it. `pthread_main_np()` was 0 inside
  `cf_win_open`; `glfwInit` on macOS then traps (SIGTRAP, exit 133) with no
  March-side diagnostic, because Cocoa requires the main thread.
- **Workaround:** `MARCH_NUM_SCHEDULERS=1` — in that mode scheduler 0 runs on
  the calling (main) thread and there is nobody to steal from. Costs all
  parallelism (`pmap` becomes sequential).
- **Would need:** a way to pin the main green thread to scheduler 0 (an
  attribute on `main`, or a forge.toml flag) while workers keep running. The
  scheduler has no affinity concept today (`march_proc` has no such field;
  every runnable proc goes through the Chase-Lev deques or the global runq).
  See the patch note in this file's "Compiler patches" section once done.

### G16. A zero-arg extern call bound to an unused name is dropped (miscompile)
- `let _ = w0()` and `let x = w0()` (x unused) where `w0` wraps a zero-arg
  extern: the C function is **not called**. A bare `w0()` statement, or a use
  of the result, calls it. One-arg externs are called in every shape.
  Debug and release alike. Cost me an hour: `let _ = gfx_init()` silently
  skipped shader compilation and the triangle never appeared.
- Verified: `probes/probe_dce` (p_effect0 called 2 of 4 times).
- **Would need:** the dead-binding elimination pass (or whatever treats a
  zero-arg call as a pure constant) to consult the extern table.

### G17. The first ~2 frames after window creation read back as the clear colour
- Not a March issue (macOS drawable attach), but recorded because a pixel
  read-back at frame 2 was my first "proof" and it was wrong. Verify at frame
  30+.

### G18. `NativeArray.get_*` is documented as "panics if out of bounds" but is unchecked
- `native_u8_arr_get` / `native_int_arr_get` / `native_float_arr_get`
  (`runtime/march_runtime.c`, `DEF_NARROW_INT_ARR` and the i64/f64 versions)
  read `arr + 32 + i * size` with no length comparison. The bounds-checking
  helper `typed_array_check_bounds` belongs to `Array`, not `NativeArray`.
- Consequence for the "prove the bounds checks away" goal: there is no check
  to prove away. The refinement on `Chunk.index` is the *only* thing standing
  between a bad index and a silent out-of-bounds read, and the refinement
  checker is definite-failure only (silence ≠ proof) unless the module opts
  into `cap verified`.
- **Would need:** either a real check in `get` (with a `get_unchecked`
  variant that requires a refined index), or the doc fixed. The former is
  what makes refinements pay for themselves.

### G19. `by` is a reserved word and the parse error points at the next token
- `Vec3(bx, by, bz)` as a pattern → `I got stuck here` with the caret under
  `bz`. `by` is the session-type keyword (`choose by Client`); it is not in
  the soft-keyword list in `surface-syntax.md`, and unlike the soft keywords it
  is not even bindable. A vector library that names components `bx, by, bz`
  is the first thing anyone writes.

### G20. `alias` works in expressions but not in type annotations
- `alias CubeForge.F32Buf as B` then `B.push(b, v)` resolves, but a parameter
  annotated `b : B.F32Buf` fails with `Unknown module `B`. Did you mean `Io`?`.
  The full path `CubeForge.F32Buf.F32Buf` works in the annotation. So every
  signature in a module that uses aliases carries the long form while its body
  uses the short one.

### G21. A borrowed read before a consuming update turns FBIP into a full copy
- `match b do Buf2(d, n) -> if n < NativeArray.length_f32(d) do Buf2(set_f32(d, n, v), n + 1) ...`
  costs **one allocation + a full-array memcpy per push** (1000/1000 in the
  probe; in the real mesher 74258 allocations and 1.8 s for 10608 vertices).
  Hoisting the length into a `let` does not help. Removing the read (case C)
  or caching capacity in the constructor (case H) gives 0 allocations and the
  mesh drops to 4.3 ms.
- The memory-model doc says to "consume the value you transform"; it does not
  say that a *borrowing* read of the same value earlier in the arm counts as a
  second use. Either `length_f32` is not in the borrow table, or liveness
  analysis dups `d` for the call because `d` is still live afterwards.
- **Done instead:** `F32Buf(data, len, cap)` carries its own capacity.
- **Would need:** borrowed-param inference to see that `length_f32` does not
  retain `d`, so the later `set_f32` still sees rc == 1. The LSP's `⧉ copied`
  hint would have shown this; the CLI has no equivalent flag.
- Verified: `probes/probe1` (cases C, F, G, H).

---

## Refinement types (the "prove the bounds away" experiment)

### G22. What Z3 discharges for `x + 16 * (z + 16 * y)`, and what it does not
Probe: `probes/refine_index.march` (`cap verified`, `--refine-report`).
`index(x : {0 <= _ < 16}, y : {0 <= _ < 256}, z : {0 <= _ < 16}) : {0 <= _ < 65536}`.

| call-site shape | result |
|---|---|
| literals `index(3, 200, 7)` | proved |
| guard `if x >= 0 && x < 16 && ... do index(x, y, z)` | proved |
| caller's own contract forwarded `e_forward(x : {..}, ..) -> index(x, y, z)` | proved |
| inline loop decomposition `index(i % 16, i / 256, (i / 16) % 16)` | **unreflectable-predicate** — `%` and `/` are outside the SMT fragment (`+ - *` with literal coefficients only) |
| the same through `let x = i % 16` | **solver-undecided** — "a caller's fact does not travel through a local let" (documented) |
| the postcondition `_ < 65536` on `index` itself | proved |

Runtime dimensions (`index_dyn(sx, sy, sz, x : {_ < sx}, y : {_ < sy}, z : {_ < sz})`):

| shape | result |
|---|---|
| preconditions, contract forwarded | proved (relational predicates over another parameter work) |
| preconditions, guard-established | proved |
| postcondition `_ < sx * sy * sz` | **unreflectable-predicate** — a product of two variables is nonlinear and rejected before Z3 ever sees it, even though Z3 would decide this instance |

So: with literal dimensions the bound proves as long as the index components
arrive as parameters or guards, never as `i % 16`. With runtime dimensions the
*inputs* prove but the *output bound* cannot even be stated. And per G18 there
is no runtime check being removed either way — the mesher's real loop is
`x = i % 16`, which is skipped in silence, and reads unchecked memory if wrong.
This bit me once already: `set(c, x, z, y, id)` (y/z swapped) compiled clean.

- **Would need:** `%` and `/` by a literal in the predicate fragment (Z3
  handles `mod`/`div` by constants fine), let-bound value propagation, and a
  nonlinear escape hatch (`*` between two variables) even if it is only
  attempted with a timeout.

### G23. `expected Unit but got ()` on an aliased call, gone with the full path
- `if In.button_pressed(...) do In.capture_cursor(true) else () end` where
  `In` is `alias CubeForge.Ffi.Input as In` and `capture_cursor(Bool) : Unit`
  wraps a one-arg extern: `error: expected Unit but got ()` at the `()`.
  Writing `CubeForge.Ffi.Input.capture_cursor(true)` in the same position
  typechecks. The line above it, `In.request_close()` (zero-arg) in the same
  shape, is fine. `probes/probe_unit` tries five shapes in isolation and
  cannot reproduce it, so the trigger is something about the real module
  (size? the second extern block? the `Input` capability name?). Recorded as
  a diagnostic-quality finding: `Unit` and `()` are the same type everywhere
  else, and the message gives no clue that the alias is involved.

### G24. A three-segment constructor path does not parse in a pattern
- `match Deep.Inner.mk() do Deep.Inner.T(v) -> v end` →
  `I was expecting -> in the match arm here` (caret at the second dot).
  `Inner.T(v)` (two segments) parses. Expressions accept any depth. With
  modules named `CubeForge.Player`, every constructor pattern outside its own
  module needs an accessor function instead.
- Verified: `probes/dotted_pat.march`.

### G25. A two-segment constructor pattern resolves against the wrong type
- `probes/dotted_pat2.march`: `mod Inner do type T = T(Int) end` and
  `match Inner.mk() do Inner.T(v) -> v end` compiles but warns
  `Non-exhaustive pattern match — missing case: LWWRegister(_, _)`. The
  qualifier was ignored and `T` resolved to the stdlib CRDT type's constructor.
  Silent wrong-type resolution in patterns plus a nonsense diagnostic.

---

## Reference counting

### G26. A Float-field variant (or tuple) passed to a borrowing function is never freed
Probe: `probes/probe_leak` (net live objects over 100 iterations, `-O0` and `-O2` identical):

| shape | net leak / 100 |
|---|---|
| (a) `xf(mkf(1.0))` — `VF(Float,Float,Float)` temp passed to an accessor that matches it | **101** |
| (b) same with `VI(Int,Int,Int)` | 1 (baseline) |
| (c) `let v = mkf(1.0)` then `match v` inline in the caller | 2 |
| (d) `VF` threaded through `bump(v)` and rebuilt (FBIP) | 2 |
| (e) `let v = mkf(1.0)` then `xf(v)` | **101** |
| (f) `match mkt(1.0) do (x, _) -> …` — `(Float, Float)` tuple | **302** |
| (g) `VI` matched inline | 1 |

So the per-call leak is in the *borrowed-parameter* path when the argument's
constructor carries `Float` fields: the callee borrows, nobody drops. `Int`
fields are fine, and the same value matched in the owning function is fine.
The tuple case leaks 3 per call (the cell plus, presumably, two boxed floats).
Every math function in this project (`Vec3.x`, `Vec3.dot`, `Quat.rotate`,
`Mat4.get`…) has exactly this shape, which is why the frame loop leaks 84
objects per frame (`CF_ALLOC_PROBE=1` output: `Player.forward` 1/call,
`Quat.from_yaw_pitch+rotate` 25/call, `Mat4.mul` 5/call, `Player.update`
28/call, `view_proj` 56/call).

- **Consequence:** "no allocation in the frame loop" cannot even be
  measured honestly until this is fixed — every temporary Vec3 is a leak.
- **Status:** see "Compiler patches" below.
