# GAPS.md — friction log for cube_forge

The primary output of this project. Every entry: what was needed, what March
offered, what was done instead, and what would make it clean. Entries are in
the order they were hit. "Verified" means reproduced with a runnable probe in
this repo's toolchain (macOS arm64, 2026-09-03). The toolchain was march
**0.2.0** (`~/.march/current`, the one `forge` silently runs) until G27, and
march **0.3.0** (pinned via `.march-version`) afterwards; entries say which.

Reproduction probes live under `probes/`.

## The five that matter most

1. **Tuples and records are never freed** (G28–G30 root cause,
   `lib/tir/rc_types.ml`: `needs_rc (TTuple|TRecord) = false`, by design).
   `match (a, b)`, multi-value returns and record state all leak. This
   project routes around it with multi-field variants and nested matches.
2. **`NativeArray` values are never freed** (G31): no `dec_rc` is emitted for
   a native-array binding at its last use, so every matrix temporary and every
   buffer growth step leaks.
3. **`main` runs on a scheduler worker, not the OS main thread** (G15):
   GLFW/Cocoa traps. Workaround `MARCH_NUM_SCHEDULERS=1`, which also serialises
   `pmap`.
4. **`&&`/`||` do not short-circuit** (G33), in both backends, undocumented.
5. **A zero-arg extern call bound to an unused name is dropped** (G16) — a
   silent miscompile.

Plus the two that cost the most time without being March-the-language:
`forge build` runs a different compiler than `march` on PATH (G27), and
NativeArray `get` is documented as bounds-checked but is not (G18).

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
- Verified: `probes/externmod3` (0.2.0 and 0.3.0).

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
- Verified: `probes/probe1` (cases A–E, 0.2.0 and 0.3.0). The current
  `probes/probe1` source holds the G21 cases C/F/G/H; the A–E source is in
  this entry's table.

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
- Verified: `probes/probe_dce` (p_effect0 called 2 of 4 times; 0.2.0 and 0.3.0).
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
- **Resolution (2026-09-03):** this is a bug in march **0.2.0** (every
  `~/.march/versions/*` build from Jul 28 – Aug 22 leaks 101–401 per 100
  calls; the Jun/Jul 0.1.0 builds and the Aug 30 **0.3.0** build report 1–3,
  i.e. clean). A `MARCH_TRACE_GC=1` trace of the 0.2.0 binary shows 403
  leaked 24-byte float boxes + 100 leaked 32-byte cells per 100 iterations,
  so the gauge was telling the truth. The 0.3.0 codegen for the same source
  frees the case-merge float box (`llvm_case.ml`, commit `2b363bd7`) — the
  leak is that fix's absence. See G27 for why it took an hour to see this.

### G27. `forge build` silently uses a different compiler than `march` on PATH
- `which march` → `~/.opam/march/bin/march` (0.3.0). `forge build` runs
  `~/.march/current/bin/march` (0.2.0, symlink to `versions/local-main-b26bacf0`)
  and prints nothing about which one. Every "compiler bug" observation in this
  log up to G26 was made against 0.2.0 without my knowing it; G26's leak was
  bisected across 35 installed toolchains before the version skew showed up.
- **Would need:** `forge build` to print the toolchain path/version it
  resolved (or honour PATH), and `forge.toml`'s `march = "~> 0.3"` constraint
  to be enforced against the toolchain forge actually runs.

### G28–G30. Leaks that survive in march 0.3.0 (`probes/probe_leak2`, net live objects per 100 calls, trace-confirmed)

| shape | leak / 100 |
|---|---|
| (a) `match (a, b) do (VF(..), VF(..)) -> VF(..)` — tuple scrutinee over two Float variants | **501** |
| (b) same over two Int variants | **501** |
| (c) nested `match a do VF(..) -> match b do VF(..) -> …` — same function, no tuple | 1 |
| (d) `match mkt(1.0) do (x, _) -> …` — returned `(Float, Float)` | **301** |
| (e) same with `(Int, Int)` | **101** |
| (f) `Mat(NativeFloatArr)` built by a recursive `set_float` loop, read via `match m do Mat(a) -> get_float(a, 5)` | **101** |
| (g) `addf(scale(mkf(1.0), 2.0), scale(mkf(3.0), 0.5))` — chain of Float-variant temps | **501** |
| (h) `match mk4(1.0) do (x, _, _, _) -> …` — returned `(Float, Float, Float, Bool)` | **401** |

Trace by size: 1100 × 32 B (tuple cells), 600 × 40 B (3-field variants),
500 × 24 B (float boxes), 100 × 160 B (the 16-float NativeFloatArr), 100 × 48 B.

- **G28.** A tuple built as a match scrutinee is never freed, and neither are
  the values it was built from. This is the idiomatic way to match two
  arguments at once and it is what every binary `Vec3`/`Quat` op does.
- **G29.** A tuple *returned* from a function and destructured by the caller's
  `match` is never freed. Returning `(x, y, z, hit)` from `sweep_axis` is the
  natural multi-value return.
- **G30.** A `NativeFloatArr` inside a single-field variant, read through an
  accessor, is never freed (the 160-byte array leaks each time).
- Consequence for this project: the frame loop leaks 69 objects/frame on
  0.3.0 (was 84 on 0.2.0). `Player.update` 21/call, `Quat` ops 24/call,
  `Mat4.mul` 5/call. The workaround is to avoid tuple scrutinees and tuple
  returns entirely (nested matches, accessor functions, multi-field variants),
  which is exactly the kind of "route around it silently" this log exists to
  make visible.

**Root cause (read in the compiler, `lib/tir/rc_types.ml` module doc):**
`needs_rc (TTuple _ | TRecord _) = false` — "tuples and records are
heap-allocated (via march_alloc) but Perceus never emits inc/dec on the
aggregate itself — the aggregate is never RC-freed and its fields belong to
it." This was introduced deliberately (`0b52510d`, `390dff00` #4) to stop a
class of double-frees on extracted fields, and the doc warns that flipping it
back "starts emitting aggregate-level RC ops on top of the field-level
accounting — double-frees". So in compiled March today:

- every tuple that is not consumed by TCO/FBIP is leaked (G28, G29);
- every record is leaked, which is also why G12's record update showed 2
  allocations per push with nothing ever reclaimed;
- the fields of a leaked aggregate are kept alive with it (the 40-byte
  `VF` cells in the trace).

That is not a bug in a pass, it is a hole in the memory model that the
language's own idioms (`match (a, b)`, multi-value returns, records as
state) walk straight into. A fix means giving aggregates a real owner
(scrutinee free for `$TupleN` patterns, dec at last use for record bindings)
and re-solving the field-level double-free those commits were avoiding. That
is a multi-day compiler task, not something I can land in this exercise
without a differential-oracle run, so cube_forge routes around it: no tuple
scrutinees, no tuple returns, no records anywhere on the frame path.
Multi-field variants (`Sweep(Float, Float, Float, Bool)`) are the
substitute, and nested `match a do … match b do …` replaces `match (a, b)`.

G30 (the 160-byte NativeFloatArr) is a different mechanism: `Mat(NativeFloatArr)`
is a *newtype* (single-field variant, `Mat(a) ≡ a` in storage), and
`add_scrutinee_free_for` skips newtype scrutinees
(`scrutinee_shares_payload_storage`) on the assumption that "the variable's
own RC lifecycle frees the shared object" — but the branch variable is then
treated as borrowed, so when the scrutinee was an owned temporary nobody
frees it. See `probes/probe_leak2` case (i) for the two-field control.

### G31. A `NativeArray` value is never freed
- `probes/probe_leak2` cases (j)–(n): a fresh `NativeFloatArr` — passed to a
  reader function, read inline, filled by a `set_float` loop, or `set_float`
  once — leaks 1 per iteration in every shape (100% of arrays). The
  post-Perceus TIR for the inline case is:
  ```
  let a : NativeFloatArr = native_float_arr_make(16, 0.) in
  let $t : Float = native_float_arr_get(a, 5) in +.(acc, $t)   -- no dec_rc a, ever
  ```
  so Perceus emits no `dec_rc` for a native-array binding at its last use.
  `set_*` is "owned/consumed" per the runtime comment (it does free a shared
  input on the copy path), but nothing frees the final array.
- Consequences here: every `F32Buf` growth step leaks the old backing array
  (5 × up to 512 KB per chunk mesh — the "8 allocations" the mesher reports
  are 8 leaks); every per-frame matrix temporary leaked before the in-place
  rewrite; the per-frame residue of 1 object is still under investigation.
- **Would need:** native arrays to get the ordinary owned-value `dec_rc` at
  last use. They are `TCon`s, so `needs_rc` already says yes; whatever
  suppresses the dec (borrow-table classification of `native_*_get`? the
  builtin-call result being treated as non-owning?) is the bug.

### G32. `Simd` has no per-lane floor and no f32x4 <-> i32x4 conversion
- Value noise needs `floor(x)` per lane and the integer lattice coordinate to
  hash. `Simd` offers arithmetic, compare, select and shifts, but no
  `floor`/`trunc`/`convert`. The 4-wide noise in `lib/cube_forge/noise.march`
  therefore extracts each lane, floors and hashes as scalars, and re-packs —
  only the smoothstep and the bilinear blend are vector code. With `f32x8`
  absent (G2) the "eight columns at once" spec item is not expressible at all.

### G33. `&&` and `||` are not short-circuiting (interpreter and compiled)
- `if false && noisy("rhs") …` prints `evaluated rhs`; `if true || noisy("rhs")`
  likewise. Both backends agree, so it is the language, not a bug in one.
  Nothing in `surface-syntax.md` says so. Consequences: `if !headless &&
  Win.open(...) == 0` opened the window in headless mode; every bounds check
  written as `x < 0 || x >= 16 || …` evaluates all six comparisons; and any
  guard of the form `p != Nil && head(p) …` is a panic waiting to happen.
- Verified: `probes/shortcircuit.march`.

---

## M4 notes

### G34. `spawn` is a keyword; `let spawn = …` is a bare "I got stuck here"
- The actor-spawn keyword collides with an ordinary variable name and the
  parse error points at the `let` with no mention of the keyword. Same family
  as G4/G19 (`on`, `by`).

### G35. `Array`'s type is spelled `PVec`, and only as `Array.PVec(a)`
- `type World = World(Array(Chunk), Int)` typechecks against the module's
  functions with `expected PVec(a) but got Array(Chunk)`. Writing `PVec(…)`
  gives `I cannot find PVec`; the working spelling is `Array.PVec(Chunk)`.
  Nothing in `docs/stdlib.md` or the module header says the type has a
  different name from the module.

### G36. A call to a function that no longer exists is reported as an unknown module
- `World.single(chunk)` after `single` was removed: `Unknown module `World``
  (the module exists and is aliased). Same diagnostic class as G6.

### G37. `pmap` scaling is weak for the mesh stage
- See the timing table in `RESULTS.md`. Chunk meshing is embarrassingly
  parallel (each task touches only its own `F32Buf` and reads five chunks),
  yet 4 scheduler threads give ~1.25x over 1 and 8/14 threads do not improve
  further. Candidates: atomic RC traffic on the shared chunk cells (every
  `C.get` on a borrowed chunk is free of RC ops, but the closure captures the
  world), the global allocator lock behind `march_alloc` for the ~5
  F32Buf growth steps per chunk, or `List.pmap_n`'s chunking handing all 64
  elements to a handful of tasks. Not root-caused here.

---

## M5 notes

### G38. Block edits copy the whole chunk
- `World.set_block` → `Array.set` on the persistent trie plus `Chunk.set` on
  a chunk that is still referenced from the old world value, so `set_u8`
  takes its copy-on-write path: a 64 KB memcpy per edit. Unavoidable with a
  persistent world value and no linear ownership of the chunk being edited
  (the frame loop holds the world in `Scene`, and `Array.get` hands out a
  shared reference). A `linear` chunk borrowed out of the array and put back
  would be the March-native answer; `Array` has no such API.

### G39. Selection outline and HUD are uploaded every frame they change
- `gfx_upload` is a `glBufferData` per call — no `glBufferSubData` in the
  shim on purpose (keep the surface small), so the outline costs one small
  upload per frame while a block is targeted. Zero March-side allocation
  (the `F32Buf` is cleared and refilled in place).


---

## Capability manifests (deliverable check)

`forge cap inspect --allow-foreign .march/build/release/cube_forge`:

```
Capabilities — cube_forge
  IO.Console              [march_println]
  IO.Process              [march_process_env]
Foreign code (IO.Foreign) — extern C declarations present
  Capability analysis stops at the FFI boundary.
Attributed to
  IO.Console            CubeForge
  IO.Process            CubeForge
build: dead-stripped    coverage: partial (foreign code)
```

Declared in source: `CubeForge.Ffi.Window` needs `IO.Foreign, Window`;
`CubeForge.Ffi.Input` needs `IO.Foreign, Input`; `CubeForge` needs
`IO, IO.Foreign, Window, Input, IO.Spawn, IO.Clock, IO.Process`;
`CubeForge.World` needs `IO.Spawn`.

### G40. The declared set and the binary's set do not match, in both directions
- The binary manifest does not mention `Window` or `Input` at all: custom
  capability names attached to extern blocks are not part of what the
  inspector measures ("analysis stops at the FFI boundary"). So the two
  capability domains the spec asked for exist only as `needs` lines.
- The binary does not show `IO.Spawn` or `IO.Clock` either, although
  `List.pmap_n` spawns tasks and `System.monotonic_time` reads the clock — the
  attribution is "to the wrapper" (stdlib), and dead-stripping hides the rest.
- Conversely `forge check` warns that `needs Window` / `needs Input` are
  unused in `CubeForge`, because no *function signature* carries `Cap(Window)`
  (G11). The manifest that is checked and the manifest that is measured are
  different sets, and neither is the one a reviewer of the shim wants.
- `forge cap query` also aborts on the intentional parse-error probes under
  `probes/`, so it cannot be run on this repo as-is.

---

## Compiler patches

None landed. The user offered patches; these are the ones I would send, in
order of value per line, each with a probe already in `probes/`:

1. `runtime/march_ffi.c`: `march_bytes_borrow` must unwrap the `Bytes` cell
   (`*(march_value *)((char *)b + 16)`) before `march_str_borrow` (G8). One
   line plus a `test/native/ffi_bytes` case.
2. The zero-arg-extern dead-binding drop (G16): whichever pass treats a
   nullary call as a pure constant needs to consult the extern table.
3. Main-thread pinning for `main` (G15): a `pinned` flag on `march_proc`, a
   scheduler-0-only queue that `march_sched_wake`/yield-repush use for
   pinned procs, an env var or `forge.toml` switch to set it.
4. `dec_rc` for native-array bindings (G31).
5. Aggregate RC for tuples/records (G28–G30) — the real fix, and a project.

### G41. The capability ceiling charges a test module for a capability it never reaches
- `test/math_test.march` imports only `Vec3`, `Mat4` and `F32Buf`, none of
  which spawn. `forge test` fails with `module CubeForge.Test.Math uses
  IO.Spawn but does not declare needs IO.Spawn` — the test binary links the
  whole `lib/`, and `World.generate`'s `pmap_n` is attributed to the test
  module. The fix is a `needs IO.Spawn` line that is a lie about the test.

---

## Static water notes

### G42. `opaque` is a keyword; `fn opaque(m)` is a bare parse error
- `opaque type` makes `opaque` unusable as a function name, with the same
  no-hint "I got stuck here" as G4/G19/G34 (`on`, `by`, `spawn`).

### G43. Threading two growing buffers through one loop needs the loop to own the branch
- The natural shape `Meshes(o1, w1) = block_faces(o, w, …)` per block would
  build one two-field variant per voxel (65 536 per chunk). Instead the loop
  itself decides which buffer a block touches, so each iteration passes both
  buffers straight through and only one is rebuilt. Works, allocation-free
  apart from growth, but it is the loop shape the leak findings dictate, not
  the one you would write first.

---

## Spreading water notes (actor model)

### G44. Message payloads cannot carry native arrays, so a chunk never crosses an actor boundary
- `specs/lang/actors.md`: `NativeU8Arr` and friends are `non_sendable_types`,
  checked at constructor application. `Actor.call` additionally takes a
  zero-arg sentinel, so a call cannot carry arguments at all. The design
  ("send the actor its neighbours' chunks, get a chunk back") was impossible
  as written. What shipped: each `WaterChunk` regenerates its own chunk from
  `(cx, cz, seed)` on `WLoad` (plus its four neighbours, to copy their edge
  columns into a 16 KB mirror), and replies to `WTick` with a packed
  `List(Int)` of cell changes that the frame loop applies to the world it
  renders. Two copies of every chunk exist by construction.
- This is the right shape for the actor model, but the rule bites hardest
  exactly where actors are most useful (bulk data owned by one actor).
  **Would need:** a linear/moved send for uniquely-owned buffers.

### G45. `Pid(...)` cannot be written in a type annotation
- A pid's type is `Pid({ sim : Sim, cx : Int, cz : Int })` (the state record),
  which the parser accepts nowhere; a bare `Pid` annotation is a type error
  (`expected Pid but got Pid({…})`). The `Scene` variant that stores the
  64 pids had to become generic (`type Scene(p) = …`) and every function
  taking a `Scene` lost its annotation.

### G46. Actor message constructors are private to the module and only exist after the actor declaration
- `WLoad(…)` from another module: `I don't know a constructor called WLoad`,
  with or without `import`. Inside the module, the same constructor used in a
  function written *above* the `actor … end` block: same error; moving the
  function below the actor fixes it. So every actor needs a set of
  send/call wrapper functions placed after it (`Water.send_load` etc.).
  Meanwhile the docs say message names share one *global* namespace, which
  is the opposite problem (collisions across modules).

### G47. Discarding a `NativeArray.set_*` result silently drops the write when the array is shared
- `let _f = NativeArray.set_int(flags, ci, 1)` inside a helper that returns
  something else: when `flags` is also held by the caller, `set_int` copies,
  the copy is discarded, and the write is lost with no warning. Cost me one
  debugging round (the water never re-ticked). The in-place contract is only
  honoured through the returned value; there is no lint for a discarded
  `set_*` result even though the function is documented as returning the
  updated array.

### G48. An alias resolves to the wrong module at link time
- `alias CubeForge.Water as W` in a test module typechecked, but the binary
  failed to link: `Undefined symbols: _CubeForge.World.call_tick,
  _CubeForge.World.edited`. Codegen resolved `W.` to `CubeForge.World`
  (both modules start with `W`). Renaming the alias to `Water` fixed it.
  `alias … as V` for `Vec3` never misfired. Verified by the test build.

### G49. `Actor.call` cannot be used from `forge test`
- `Err("actor_call: not in scheduler context")`. Test bodies run outside the
  green-thread scheduler, so any request/reply test of an actor is impossible;
  only `send` + `run_until_idle` smoke tests work. The actor round-trip is
  covered by the `CF_AUTOFLOW` script instead.

### G50. Actor state is a record, and records are never freed
- Every handler returns `{ state with sim: s1 }` — a fresh record per message
  (G28 applies). One record per tick per chunk is small, but it is a leak by
  construction that nothing in the language lets you avoid, since actor state
  must be a record.
