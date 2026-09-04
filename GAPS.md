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
   `pmap`. **Patched** (runtime branch `runtime/pin-main-thread`,
   `MARCH_PIN_MAIN=1`); see G15.
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
- **Patched (2026-09-03), march branch `runtime/pin-main-thread`:** a
  `pinned` field on `march_proc` and a scheduler-0-only mutex FIFO ("pin
  queue") that only `sched_loop` on scheduler 0 pops, checked right after the
  global runq. All four enqueue points (spawn, yield re-push, `march_sched_wake`,
  and the deque-overflow path) route a pinned proc there, so no worker can pop
  or steal it. `march_spawn_main` pins `main` when `MARCH_PIN_MAIN=1` is set
  (opt-in; the default stays unpinned). With one scheduler the flag is a
  no-op. Tasks spawned by `main` are not pinned, so `pmap` still fans out.
  Measured on this machine (14 cores, runtime default of 4 scheduler threads):
  - `probes/pin_main/` (pmap_n over 64 CPU-bound elements):
    unpinned default 207 ms, main thread false; **pinned default 210 ms, main
    thread true before and after the pmap**; `MARCH_NUM_SCHEDULERS=1` 762 ms
    (748 ms pinned); stock 0.3.0 with `MARCH_PIN_MAIN=1` still reports false.
  - cube_forge `mesh all`: headless 4 schedulers 327 ms, headless 1 scheduler
    448 ms; **windowed `MARCH_PIN_MAIN=1` 349 ms** (window opens, 240 frames at
    113 fps, frame 200 dump renders) vs windowed `MARCH_NUM_SCHEDULERS=1`
    436 ms. Unpinned windowed still hits the shim's main-thread refusal.
  - Runtime unit test `test/test_scheduler_pin.c` (4 schedulers): pinned proc
    stays on the `march_sched_run` thread across 2000 yields and cross-thread
    wakes while 128 siblings run on workers; a negative control (spawn
    unpinned) fails both assertions.
  Still open upstream: a `forge.toml`/compiler switch to bake the flag in (the
  env var is read in `march_spawn_main`, before any user code runs, so a
  `__attribute__((constructor))` `setenv` in the shim would also work).

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

### G18. `NativeArray.get_*` bounds checking is inconsistent between the C source and the compiled path
- **Correction (spreading-water work):** a compiled `NativeArray.get_int` with index 256 on a
  256-element array *did* abort with `native_int_arr_get: index 256 out of bounds` — so the
  compiled builtin path checks Int arrays even though the C function body I read does not.
  Which of `get_u8` / `get_f32` / `get_float` are checked on which path is not documented;
  the observation below stands for the C source, and the doc string is right for at least
  `get_int` compiled.
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
- **Root-caused 2026-09-04** — see "G37 root-caused" at the end of this file.
  It was three separate causes (G71, G72, G73), and the third — an inc/dec pair
  emitted for every field read of a borrowed variant — is the one that caps the
  mesher. The "8 and 14 threads do not improve further" observation was an
  artifact: the runtime silently ran 4 threads in all three cases.

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
   pinned procs, an env var or `forge.toml` switch to set it. **Landed** on
   march branch `runtime/pin-main-thread` (`MARCH_PIN_MAIN=1`); numbers under
   G15. Probe: `probes/pin_main/`.
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

---

## Greedy meshing / sections / blit notes

### G51. An extern that returns its own (borrowed) argument is a use-after-free, and nothing says so
- `fn blit(dst: NativeF32Arr, …): NativeF32Arr` implemented as `return dst` in C:
  March treats the returned pointer as a fresh owned value and releases `dst`
  after the call (borrow default), so the next use of the result reads freed
  memory; the second call in a chain aborts with `RC underflow (rc was 0)`.
  `probes/probe_ffi5` shows a 3-argument call "working" only because the
  argument stayed live; the 5-argument variant looked like an arity bug for
  an hour.
- The documented fix is `consume dst` (ownership transfers into the binding,
  which then legitimately returns it). That is exactly the `set_*` in-place
  contract, and `cf_f32_blit` now checks `rc == 1` on entry and aborts if the
  buffer is shared, instead of silently copying.
- **Would need:** `forge ffi gen-c` (or the typechecker) to warn when an
  extern's return type equals a borrowed parameter's heap type — or a
  `returns_arg` annotation. The docs' "must not retain" sentence does not
  read as "must not return".

### G52. `&&` in a bounds guard over a NativeArray read is an out-of-bounds read (G33 again)
- `if u + wd < 16 && get_int(mask, u + wd + 16 * v) == key` evaluates the
  read when `u + wd == 16`; on 0.3.0 the compiled `get_int` caught it
  (`index 256 out of bounds`). The idiom every C/Rust/ML programmer reaches
  for is unsafe in March and the language gives no hint.

### G53. `native_int_arr_get` IS bounds-checked compiled (G18 corrected)
- Recorded here so the correction is not lost: the failure mode above was a
  clean runtime abort, not silent memory reading.

### G54. A buffer stored in a persistent `Array` can never be updated in place
- The first design kept per-chunk upload buffers inside `ChunkMesh` values held
  in an `Array.PVec`. `Array.get` hands out a shared reference, so the buffer's
  rc is ≥ 2 whenever anyone can see it, and every in-place operation (`set_*`,
  the new `blit`) either copies or, with the explicit check, refuses. There is
  no way to *move* a value out of a persistent container and put it back
  (`Array` has no `take`/`swap`), so "mutable scratch owned by a long-lived
  structure" is not expressible with FBIP alone. cube_forge sidesteps it by
  letting the shim assemble VBOs from the 16 section buffers
  (`gfx_upload_begin` + `gfx_upload_part`) instead of concatenating in March.
- **Would need:** a linear "borrow out / put back" API on `Array`, or a real
  uniquely-owned mutable buffer type that persistent containers can hold by
  move.

---

## Hotbar / inventory notes

### G55. Nothing new broke — but every "small UI feature" repeats the same three tolls
- The inventory is an 8-field variant with a hand-written accessor per field
  (no records, G28; no field names on variants), the hotbar rebuild has to be
  threaded through a `Ui` wrapper variant so the buffers stay uniquely owned
  (G54), and a scripted test needs an env knob because there is no way to
  drive input or read state from `forge test` (G49). None of it is hard; all
  of it is boilerplate a record type and an inspectable actor would remove.

---

## `@[no_alloc]` on march main (137737f3, 2026-09-03) — G1 revisited

### G56. A function attribute and a `doc` string cannot coexist
- `doc "…"` then `@[no_alloc]` then `fn` → `I got stuck here` at the
  attribute; `@[no_alloc]` then `doc` then `fn` → stuck at the `doc`. Verified
  with the fresh main build. So annotating a documented function means
  deleting its doc string (this project turned them into `--` comments on
  every annotated function). `forge fix --contracts` inserts the attribute
  directly above `fn`, so it never hits this itself only because it skips
  documented functions' doc lines — no, it produces the broken order too if
  the function has a doc; in this codebase none of the 16 it chose had one.

### G1 revisited: `@[no_alloc]` exists now, and here is what it says about the frame loop
Toolchain: origin/main `137737f3` (2026-09-03) + the pin-main cherry-pick, built
locally as `main-nalloc-137737f3`. `forge fix --contracts` inserted 16
attributes on its own (all "consume one, rebuild same shape" functions:
`Vec3.add/sub/scale/cross`, `Quat.mul/conjugate`, `Chunk.set/set_idx`,
`F32Buf.clear`, inventory/scene rebuilders). Then `@[no_alloc(warn)]` on 40
frame-path functions gave these verdicts, each with a precise, transitive
diagnostic naming the constructor or builtin:

| verified allocation-free | rejected, and why |
|---|---|
| `Vec3.dot/length/normalize`, `Mat4.write_view`, `Mat4.mul_into_f32` (+ helpers), `World.block_at/solid_at`, `Chunk.get/get_or_air`, `Player.box_hits/hits_go/in_water/snap` | `Vec3.new`, `Player.eye/forward`, `Quat.from_axis_angle/from_yaw_pitch/rotate`, `Raycast.step/cast` (`Hit`), `Player.sweep_axis` (`Sweep`), `Player.update`: **a fresh small variant is a heap cell**. No stack promotion happened for any of them, even the ones consumed immediately by the caller. |
| | `F32Buf.push` → `native_f32_arr_make` in the growth path (correct: the contract is whole-function, so an amortized-growth buffer can never carry it; G58) |
| | `Hud.build_number`, `Hud.slot`, `tick_fps`: `Nil` is a heap cell (documented caveat) |
| | `read_input`, `draw_chunks`, `frame_loop`: extern calls are opaque; `@[no_alloc(assume)]` is the escape hatch |

Two more findings from the exercise:

### G57. A constructor in each branch of an `if` inside a match arm defeats reuse (contract says so; gauge did not)
- `probes/probe1` `push_h`: `match b do Buf3(d, n, cap) -> if n < cap do Buf3(…) else Buf3(…) end end`
  is reported as allocating `Buf3`; the identical function with the branch
  decision hoisted into `let`s and a single constructor site passes. The
  live-object gauge showed 0 for both because the second branch never ran, so
  the contract is the more honest instrument. `F32Buf.push` was rewritten to
  the single-site shape.

### G58. The contract is whole-function and transitive; "no allocation on this path" is not expressible
- The brief asked for a *path-specific* guarantee. `@[no_alloc]` rejects
  `F32Buf.push` because its growth branch allocates, although the steady-state
  path is in place and that is the property the mesher relies on. There is no
  way to say "the branch guarded by `n < cap` allocates nothing", and no
  `@[no_alloc(amortized)]`. The measurement approach (gauge + probes) stays the
  only way to state that property.

**Net answer to the brief's "every path executed per frame should carry
`@noalloc`":** 15 of the 40 per-frame functions carry it and are verified; the
rest cannot, because every fresh `Vec3`/`Quat`/`Hit`/`Sweep` is a heap cell
that nothing promotes to the stack — the language has no unboxed aggregate.
The frame loop's measured residue is still 1 live object per frame; the
contract's stricter count is roughly a dozen short-lived cells per frame that
are freed immediately.

### G59. `pfn` is not allowed inside a `describe` block
- A helper function written next to the tests that use it, inside
  `describe "…" do … end`, is `I got stuck here` at the `pfn`. Helpers must
  be module-level, so a test file's helpers and tests cannot be grouped.

---

## Top-down map view notes

### G60. `forge test` silently drops a test file that fails to compile — false green
- `test/mapview_test.march` had a trailing comma (`Marker.build(B.new(64), 12.5, -7.0, )`),
  a plain parse error (`march --check` on the file reports it immediately).
  `forge test` printed no error, no warning, nothing: it silently excluded
  the whole file's tests from the run and reported `Finished: 27 tests, 0
  failures` — the same count as before the file existed. The suite looked
  green while an entire file, including tests for a feature just added, ran
  zero assertions. Caught only because I cross-checked the test count against
  `grep -c "  test \""` across `test/*.march`.
- This is already a filed march compiler issue
  (`specs/todos/2026-08-17-forge-test-silent-skip-on-compile-failure.md` in
  the March repo, filed independently before this project hit it) — recorded
  here because it is exactly the failure mode that makes a test suite
  untrustworthy: a typo silently shrinks coverage instead of failing the
  build.
- **Would need:** `forge test` to treat any test file that fails to compile
  as a hard error (or at minimum print a per-file warning naming the file
  and the count of tests it could not run), never a silent partial run.

---

## Terrain generation notes

### G61. A `NativeU8Arr` store wraps mod 256 silently, and near-white textures came out red
- `Texture.speckle_go` adds a per-texel variation of up to +12 to a base colour.
  For snow (242, 246, 250) that overflows: `set_u8` truncates mod 256 rather
  than saturating (documented in `stdlib/native_array.march`: "stores truncate
  mod 2^w two's-complement... never trap"), so 250 + 12 became 6 and the snow
  texture rendered as a red/black grid. Nothing warns: the value is an `Int`
  right up to the store, and every intermediate is in range.
- **Done instead:** an explicit `byte(v)` clamp before every channel store.
- **Would need:** a saturating store (`set_u8_sat`) or, at minimum, a refinement
  on `set_u8`'s value parameter (`{Int | 0 <= _ && _ < 256}`) so an out-of-range
  literal or an obviously-unclamped expression is caught rather than wrapped.
  This is the same class as G18/G52: the narrow-width array API is silent where
  it could be loud.

### G62. The `forge test` silent skip (G60) bit twice more in one session
- `terrain_test.march` was dropped whole, twice: once for a `pfn` inside a
  `test` body (G59's restriction) and once for a stale capability declaration
  in an unrelated test file. Both times `forge test` reported a clean run with
  the *previous* test count. Cross-checking `grep -c '  test "'` against the
  reported count is currently the only way to know the suite ran what you wrote.

## Lighting notes (skylight flood-fill)

### G63. G21 rules out a BFS queue over a NativeArray; level-synchronous sweeps are the workaround
- A light BFS must read `la[n]` to decide whether the neighbour is darker and
  then write `la[n]`. G21 makes that a full 4 MB copy **per write**: a probe of
  1M read-then-write iterations on a 4 MB `NativeU8Arr` reached a 229 GB peak
  footprint and was OOM-killed, against a write-only loop of the same shape
  costing nothing.
- Every variant fails identically: the read in a guard (`if 1 <= get(a, i)`),
  the read hoisted to a `let`, and the read moved into a separate `pfn` helper
  with the write in a write-only helper. G21's "hoisting into a `let` does not
  help" extends to hoisting into a *function*: what matters is that the array is
  still live at the write.
- The same trap bites the obvious double-buffer fix. Swapping `src` and `dst`
  each level fails on the second level, because `src` was read during the first
  and a shared array cannot be a blit destination — `cf_u8_blit` reports
  `rc=2` and aborts. A fresh destination per level is the working shape.
- **Done instead:** `lib/cube_forge/light.march` propagates with a
  level-synchronous downward sweep over levels 15..2 instead of a queue. Every
  pass reads one array and writes a *different* one (the proven
  `F32Buf.copy_into` shape). Skylight is naturally level-ordered, so one
  downward sweep is complete: a voxel written during level L always receives a
  value `< L`, so a later iteration of the same sweep picks it up.
- Two things made it affordable: `cf_u8_blit` (the u8 twin of `cf_f32_blit`),
  which turns the per-level field copy into a `memcpy`; and an early-out in
  `give1` that tests the neighbour's current light *before* the chunk lookup,
  since the best a neighbour can receive is `lv - 1`. The early-out alone took
  the lighting tests from 49 s to 21 s — open sky is almost entirely that case.
- **Would need:** the G21 fix (borrowed-param inference, so a read that is dead
  before the write does not keep the array live). With it, the queue-based BFS
  in the design doc would be directly expressible and strictly faster.

### G64. `World.block_at` in a per-voxel loop is the hidden cost of any voxel sweep
- The bounded relight spent **179 ms of its 217 ms in column seeding alone** —
  961 columns of 256 voxels, each voxel calling `World.block_at`, which
  re-derives the chunk coordinates and walks the `Array.PVec` trie every time.
  727 ns per voxel step.
- A column never leaves its chunk. Hoisting `chunk_at` out of the loop and
  reading through `Chunk.get_or_air` took seeding to **12 ms (15x)**, the whole
  relight to 50 ms, and the full-world flood from 1724 ms to **521 ms**.
- Not a language gap so much as a shape worth writing down: `World.block_at` is
  the right API for a raycast or a physics probe and the wrong one for anything
  that walks voxels in bulk. Every future bulk pass (meshing, save/load, chunk
  streaming) should fetch the chunk once and index it directly.

### G65. Two more identifiers that are silently reserved: `by` and `on`
- `pfn corner_pack(..., by : Int, bz : Int)` and `let on = st % 2` both produce a
  bare `parse error` pointing at the identifier, with no indication that the name
  is the problem. Renaming to `avy` and `lamp` fixed each immediately.
- Related parse traps hit in the same session, all reported as `parse error` at
  the offending token rather than as a named rule:
  - a `doc` string and a function attribute cannot coexist (already G56) — the
    workaround is a `--` comment above `@[no_alloc]`;
  - float comparison is `<`, not `<.`, even though the arithmetic is `+.` / `-.`;
  - a nested `if` inside a one-line `if ... do <expr> else ... end` branch does
    not parse, parenthesised or not.
- **Would need:** a reserved-word list in the error (``on` is reserved`), and the
  parser naming the construct it rejected.

### G66. `forge check` does not catch an arity/type mismatch across modules
- Task 7 changed `Mesher.mesh_section_opaque` from 8 parameters to 9, inserting a
  `NativeU8Arr` where `chunk_mesh.march` was still passing an `Int`. `forge check`
  reported **0 errors** on `lib/`; only `forge build` caught it, at the clang stage.
- Cheap to trip over: the whole point of `check` is the fast inner loop, and a
  cross-module signature change is exactly what it should catch.

### G67. Reading a `NativeArray` in a loop inflates its refcount, permanently
- Perceus emits an `inc_rc` for each read of a borrowed `NativeU8Arr` inside a hot
  loop with no matching `dec_rc`, so the array's refcount grows with the number of
  reads and never comes back down. Measured directly, by using the array as an
  FFI blit destination afterwards (the shim aborts unless `rc == 1`) and reading
  the reported count at three sweep sizes:

  | relight box | voxels visited | reported `rc` | per voxel |
  |---|---|---|---|
  | 9 x 9 x 9 | 729 | 5 202 | 7.1 |
  | 17 x 17 x 17 | 4 913 | 44 198 | 9.0 |
  | 33 x 33 x 33 | 35 937 | 365 754 | 10.2 |

  The count scales with the volume swept, at roughly one increment per neighbour
  read. This is G31 (no `dec_rc` for `NativeArray` bindings) seen from the other
  side: there the consequence was a leak, here it is that **a buffer that has been
  read in a loop can never again be treated as uniquely owned**.
- What it cost: the skylight sweep runs fourteen propagation passes and needs a
  read-only source plus a writable destination each pass. The natural fix is to
  ping-pong two preallocated buffers and re-sync only the slice that was written.
  That is impossible here — after pass one the source's refcount is in the
  hundreds of thousands, so it can never become a destination. The code instead
  allocates and copies a fresh 2 MB prefix on every pass: fourteen 2 MB callocs
  plus fourteen 2 MB copies per block edit, roughly 60 MB of memory traffic to
  propagate light around one broken block.
- No user-level workaround found. `consume` (the G51 fix for an extern returning
  its borrowed argument) does not help, because the inflation happens across
  millions of ordinary reads rather than at one call boundary.
- **Would need:** the `dec_rc` of G31, or better, borrow inference recognising that
  a `NativeArray` passed only to `get_*` is read-only and needs no refcount
  traffic at all. The per-read `inc_rc` is also pure overhead in the hot loop.
- **FIXED** 2026-09-04 in march `bfb16dac` (branch `claude/borrow-native-array-reads`),
  the second of those: `extern_borrow_table` in `lib/tir/borrow.ml` had entries for
  `ring_buf` and the whole string family but none for `NativeArray`, so every array
  read counted as an ownership transfer. Adding `_get`/`_length`/`_sum`/`_to_list`
  for the five array kinds — each checked against `runtime/march_runtime.c` to
  confirm it never `march_decrc(arr)`, with `_set` excluded because its copy path
  does — takes the whole read chain to `borrow` and the loop to zero refcount
  traffic. Verified: 576 codegen tests, 33 TIR snapshot goldens, and the
  differential oracle at 1110/1110 generated programs. Worth **43% of a block
  edit's cost** in cube_forge with no change to cube_forge (see RESULTS.md).
  Still open: the same leak through closure-apply wrappers, where a `List` read
  keeps its parameter owned. Plan:
  `docs/superpowers/plans/2026-09-04-g67-borrowed-array-reads.md`.

### G68. Per-element `NativeArray` writes are O(n) on a shared array, so a per-frame simulation is O(n^2)
- The precipitation pool is 4 floats per particle and its geometry is 54. Written
  in March with `NativeArray.set_float` / `set_f32`, the cost is quadratic in the
  particle count, because each write copies the whole array rather than mutating
  it — the buffers live in the frame variant, so they are never uniquely owned.
- Measured, release build, 4000 particles: the **step alone** (three writes per
  particle over a 16,000-float array, no geometry built at all) ran at **5.9 fps**
  against 270 fps with the pool disabled. The 216,000-float geometry build was
  worse — the process was **SIGKILLed for memory (exit 137)**. 1000 particles was
  free, 2000 cost half the frame rate, 3000 cost 90% of it: the shape of an
  O(n^2) curve, not a fill-rate one.
- Moving both loops into `cf_precip_frame` in the C shim made 4000 particles cost
  **2%** (265 fps against 270) and 16,000 still run at 181 fps.
- `F32Buf.push` has the same problem for the same reason, and `F32Buf.grow`
  compounds it: `copy_into` is an element-by-element recursion, so growing a
  large buffer also recurses once per element.
- This is the first thing in the project that a March-side per-frame loop simply
  could not do. Meshing gets away with it because it runs once at startup.
- **Would need:** either uniqueness the optimiser can see through a variant field,
  or a mutable-array escape hatch (a linear/borrowed `NativeArray` write) for the
  case where the array demonstrably has one owner.

### G68. Reading a field of a *shared* variant allocates, so a read-only accessor can be a hidden per-frame allocation
- `Inventory.hud_key` walked 36 slots through two accessors, each of which is a
  one-line field read:

  ```march
  fn cells(i : Inv) : NativeIntArr do match i do Inv(a, _, _, _, _, _) -> a end end
  fn item_at(i : Inv, s : Int) : Int do NativeArray.get_int(cells(i), 2 * s) end
  ```

  73 destructures a frame. Measured in isolation — an `Inv` built locally and
  hashed 100 times — that costs **5 live objects per 100 iterations**, i.e.
  nothing. Called from the frame loop, where the same `Inv` is reached through
  `Scene -> Ui -> Inv` and is therefore shared, it cost **one extra live object
  every frame**, doubling the frame loop's allocation from 1 to 2.
- The difference is ownership, not the code: destructuring a uniquely-owned
  variant is free and destructuring a shared one is not. Nothing at the call site
  says which case you are in, and the accessor that costs nothing in a test
  costs an allocation in the loop that matters.
- Hoisting the destructure out — matching once and walking the raw array —
  restored 1 per frame:

  ```march
  fn hud_key(i : Inv) : Int do
    match i do Inv(a, sel, p, _, _, _) -> key_go(a, 0, 0) * 1000 + ... end
  end
  ```
- Why this is worth writing down rather than filing as a variant of G28: the
  allocation is invisible at the definition, invisible at the call site, and
  invisible to a microbenchmark of the same function. The only thing that found
  it was a whole-frame live-object count, and the only reason it was noticed at
  all is that the project prints that count every run.
- **Would need:** either field reads on shared variants that do not allocate, or
  a way to see at the call site that an accessor will copy. A lint for "accessor
  called in a loop over a value the function does not own" would catch the shape.

### G69. An unboxed small aggregate built inside a branch leaks, one object per construction
- `c0275445` on March main represents a single-constructor variant whose fields
  are all scalars, arity 2..4, as an inline LLVM struct value. When such a value
  is built inside a branch, it **leaks**: one heap object per construction, never
  freed.
- It is a leak, not a transient allocation. The live-object delta scales exactly
  with the iteration count:

  | iterations | boxed (`137737f3`) | unboxed (`7419c689`) |
  |---|---|---|
  | 100 | 3 | 103 |
  | 1 000 | 1 | 1 001 |
  | 10 000 | 1 | **10 001** |

- The trigger is narrow. Three shapes, same type, same toolchain
  (`probes/unboxed_pair/narrow.march`, 5 000 iterations each):

  | shape | boxed | unboxed |
  |---|---|---|
  | `let p = Pair(1.0, 2.0)` — no branch | 3 | 3 |
  | `let p = if c do Pair(..) else Pair(..) end` | 1 | **5 001** |
  | `let p = make(x)` — built in a callee | 1 | 1 |

  Only the branch case leaks. Without a branch the value stays in registers and
  never reaches the heap at all.

- **Mechanism, from the emitted IR.** The two arms of an `if` merge through a
  join slot typed `ptr`, so an unboxed struct has to be materialised onto the
  heap to pass through it. The new codegen does exactly that:

  ```llvm
  %ubmk20 = insertvalue %ub.Pair poison, double 1.0, 0    ; build the struct value
  %ubmk21 = insertvalue %ub.Pair %ubmk20, double 2.0, 1
  %ubbox22 = call ptr @march_alloc(i64 32)                ; box it for the join slot
  store i32 0, ptr %ubtag23                               ; tag
  %ubf24 = extractvalue %ub.Pair %ubmk21, 0               ; copy the fields back out
  store double %ubf24, ptr %ubfp25
  %ubf26 = extractvalue %ub.Pair %ubmk21, 1
  store double %ubf26, ptr %ubfp27
  store ptr %ubbox22, ptr %res_slot18                     ; the arm's result is a pointer
  ```

  The boxed build allocates in the same place — but it emits **seven**
  `march_decrc_local` in this function against the unboxed build's **one**. The
  box created by the `%ubbox` materialisation is never decremented, so it is
  never freed. Perceus does not have a case for a heap value the unboxing path
  invents after RC insertion has already decided what exists.

- **This is why it is a compiler bug and not a cost of the representation.**
  The same program with the branch hoisted allocates nothing on either
  toolchain, so the heap traffic is not inherent to unboxing — it comes from one
  materialisation site, and the leak comes from that site being invisible to the
  RC pass. Nothing about the language semantics requires it.
- In this engine it cost the frame loop one extra live object per frame (1 -> 2),
  from a two-float weather-cache pair built in exactly that shape.
- The change's own write-up
  (`specs/progress/2026-09-03-unboxed-small-scalar-aggregates.md`) measured this
  engine at `9cd1ffb` and reported allocation unchanged, which was true then —
  the weather pair was written after that commit, so no branch-built aggregate
  existed to trip it.
- **Would need:** a drop for the materialised box — the RC pass has to see it —
  or, better, keep the value unboxed through the join by typing the join slot as
  the struct rather than a pointer, in which case nothing is allocated at all.

#### Two method notes
The first check I ran was a static count of `march_alloc` call sites in the loop
body. It said the new toolchain emitted *fewer* allocations, 4 against 6, and I
nearly reported no regression on the strength of it. Call sites are not
executions, and a net-live-object count is not an allocation count: both builds
allocate here, and only one frees.

My first stated mechanism was also wrong. I guessed that extracting a field from
an unboxed struct yields a raw double that must be re-boxed as a March `Float`.
The IR shows the fields stay raw doubles throughout; the leaked object is the
aggregate itself, boxed to cross a branch join.
### G70. Two more ways a `NativeArray` write silently becomes a whole-array copy
Both found building the biome field, both variants of G68.
- **Wrapping arrays in a variant cell.** A BFS carried its distance array and
  ring queue as `Bfs(d, q, qn)` and rebuilt the cell on every push. The cell
  holds a reference, the matched binding holds another, so each `set_int`
  copied the array: 20 passes over 16,384 columns peaked at **10.8 GB** and the
  test suite was SIGKILLed. The same work as a level sweep with the arrays as
  plain threaded parameters (the `level_go` idiom in `light.march`) peaks at
  120 MB across 840 passes.
- **Discarding the write.** `let _ = NativeArray.set_int(d, i, 0)` compiles and
  does nothing when the array is shared: the write goes into the copy that the
  call returns, and the copy is dropped. The first BFS never seeded a single
  water column and produced no error of any kind. A write's return value is
  the array that was written; it must always be threaded.
- **Would need:** a lint on a discarded `NativeArray.set_*` result, and ideally
  a warning when an array reachable from a variant field is written.

---

## G37 root-caused (2026-09-04)

G37 recorded that chunk meshing gained only ~1.25–1.6x from `List.pmap_n` and
that 8 or 14 scheduler threads did not beat 4. That single symptom turned out
to be three independent causes, below. All numbers were taken on an M3 Max
(10 performance + 4 efficiency cores) that was **under heavy external load
(load average 100–130)** from unrelated compiler builds, so absolute times are
inflated; every claim rests on a ratio between two configurations measured
back to back, never on an absolute.

The probe is `probes/pmap_scaling/pmap_scaling.march`: one `List.pmap_n` over
the same 64-element list with the same worker count, and six interchangeable
task bodies that differ only in what they touch.

### G71. `MARCH_NUM_SCHEDULERS` is a compile-time constant that the environment variable can only *lower*, silently
`runtime/march_scheduler.h:70` defines `MARCH_NUM_SCHEDULERS` as **4** unless
overridden with `-D` at build time, and `march_sched_init` applies the
environment variable as

```c
if (n >= 1 && n <= MARCH_NUM_SCHEDULERS) g_num_scheds = n;
```

so `MARCH_NUM_SCHEDULERS=14` is **ignored without a word** and the process runs
4 scheduler threads. `sample` on a run that asked for 14 shows 5 threads (main
plus four schedulers); the same binary from a runtime built with
`-DMARCH_NUM_SCHEDULERS=16` shows 15.

This invalidates every "8 and 14 don't help" measurement in `RESULTS.md` and in
G37 itself: 4, 8 and 14 were all *the same configuration*. On the pure-arithmetic
body the plateau disappears entirely once the cap is raised:

| scheduler threads | default runtime (cap 4) | runtime built with cap 16 |
|---|---|---|
| 1 | 2034 ms | 1597 ms |
| 4 | 466 ms | 345 ms |
| 10 | (silently 4) 481 ms | **135 ms** |
| 14 | (silently 4) 660 ms | 140 ms |

**11.8x on 14 threads**, against an apparent ceiling of 4.4x. The clamp itself
is not wrong — `g_scheds` is a fixed-size array — but a request the runtime
cannot honour must not be discarded in silence.
- **Would need:** either size the scheduler table from the environment at
  `march_sched_init`, or keep the compile-time bound and warn on stderr when the
  environment asks for more than it. A raised default (`System.cpu_count()`
  clamped to a larger maximum) would also stop 4 being the de-facto limit of
  every parallel March program on a 14-core machine.

### G72. Every allocation and every free bumps one global atomic counter, so allocation-heavy work gets *slower* with more threads
`march_alloc` and the RC free paths unconditionally execute

```c
#define MARCH_ALLOC_BUMP() atomic_fetch_add_explicit(&march_live_alloc_count, 1, memory_order_relaxed)
#define MARCH_FREE_BUMP()  atomic_fetch_sub_explicit(&march_live_alloc_count, 1, memory_order_relaxed)
```

— a read-modify-write on **one cache line** shared by every thread in the
process, on the hottest path there is. It is not behind a debug flag; it backs
the `march_live_allocs()` leak gauge, which this project uses every session.

Allocation-heavy body (`build` + `sum` over cons cells), cap-16 runtime:

| scheduler threads | with the counter | counter compiled out |
|---|---|---|
| 1 | 63 ms | 55 ms |
| 4 | 172 ms | 29 ms |
| 10 | 183 ms | 16 ms |
| 14 | 185 ms | **16 ms** |

With the counter, allocation-heavy parallel work is **3x slower on 14 threads
than on 1**. Without it, the same code speeds up 3.4x — and the 14-thread case
is **11.6x faster**. Deleting two atomic increments is the difference between
anti-scaling and scaling.

In cube_forge this moved `march_alloc` from the single largest March symbol in
the mesh profile (10 552 samples) to outside the top 25, and whole-world meshing
from 1.6x to 2.1x on 14 threads.
- **Would need:** a per-thread counter summed on read. The gauge's only consumer
  reads it between frames, so exactness under concurrency is not required —
  and it is not achieved today either, since `relaxed` gives no ordering.

### G73. Reading a field of a *borrowed* variant still emits an inc/dec pair — which makes shared-data reads anti-parallel
This is the cause of the mesher's own ceiling, and the most serious of the three.

`CubeForge.Chunk` is `Chunk(NativeU8Arr)` and `Chunk.get` is a read:

```march
fn get(c : Chunk, x : Int, y : Int, z : Int) : Int do
  match c do Chunk(a) -> NativeArray.get_u8(a, index(x, y, z)) end
end
```

Borrow inference gets this right — `MARCH_DEBUG_BORROW=1` reports
`CubeForge.Chunk.get(c:borrow, …)`, and likewise `nb_get`, `face_key` and
`fill_mask` all take their five chunks and two fields as `borrow`. But the
emitted IR for that one array read is:

```llvm
%fv1792 = load ptr, ptr %fp1791, align 8      ; project the field
call void @march_incrc_local(ptr %ld1793)     ; ← dup the projected array
%cr1807 = call i64 @native_u8_arr_get(ptr %ld1805, i64 %ld1806)
call void @march_decrc_local(ptr %ld1808)     ; ← drop it again
```

The parent is borrowed and provably alive for the whole call; the projected
value is used only at a borrowed argument position and never escapes. The pair
is pure overhead. And under the scheduler it is not even cheap: `march_incrc_local`
checks `march_sched_in_scheduler()` — a `_Thread_local` read — and then
**always defers to the atomic `march_incrc`**, so inside `pmap_n` there is no
non-atomic RC at all.

An atomic read-modify-write on an object *shared between workers* is a cache
line in exclusive state bouncing between cores. Two probe bodies read the
identical shared `NativeU8Arr` the identical number of times; the only
difference is whether the read goes through a one-field wrapper:

| scheduler threads | raw shared array | same array behind `Cell(NativeU8Arr)` |
|---|---|---|
| 1 | 476 ms | 2 761 ms |
| 4 | 122 ms | 5 297 ms |
| 10 | 82 ms | 9 873 ms |
| 14 | **95 ms (5.8x)** | **11 188 ms (0.25x)** |

Direct reads speed up 5.8x. The same reads through a wrapper get **4x slower**
as threads are added, and at 14 threads are **136x** slower than the direct
form. The wrapper costs 5.8x even single-threaded.

The same effect explains why a shared `Array.PVec` does not parallelize at all
(1.12x on 14 threads) while a private one built per task does (3.4x): a
`PVec`'s trie nodes and 32-element leaves are all `Cons` cells, so a single
`Array.get` dups and drops a dozen shared headers. (Separately, `Array.get`
calls `lst_len(tail)` on every read and `lst_nth` at every level, so it is
~560 ns — a linked-list walk, not the O(log₃₂ n) the docs claim.)

The mesher reads voxels through `Chunk.get` and shares five chunks with every
neighbouring task, so every voxel of every chunk RMWs a header that other
workers are reading at the same moment.
- **Would need:** elide the dup/drop when a field is projected out of a
  *borrowed* scrutinee and the projected binding is only consumed at borrowed
  positions within the arm. This is the same class as the G67 fix (which added
  the `NativeArray` accessors to `extern_borrow_table`), one level up: there the
  borrowed thing was an argument, here it is a projection.
- **Workaround available today:** hoist the field out once and thread the raw
  array through the hot path, which is what `Light` already had to do for a
  different reason (G64).

### What the profile looks like now
`sample` over the mesh loop (`CF_MESH_REPS`, added for this investigation),
14 real scheduler threads, allocation counter removed, by top-of-stack samples:

| | samples |
|---|---|
| refcounting (`march_incrc`/`decrc`/`_local`, `march_sched_in_scheduler`, `_tlv_get_addr`) | ~27 600 |
| the mesher itself (`face_key`, `nb_get`, `fill_mask`, `push`, `section_has`) | ~9 000 |
| libmalloc | ~3 800 |

**The mesher spends three times as long refcounting as meshing.** Raising the
preemption quantum from 1 ms to 50 ms changed nothing (390 → 360 ms at 4
threads), so the `sigprocmask`/`_sigtramp` traffic in the profile is
`swapcontext`'s signal-mask save/restore, not the preemption daemon.

## Procedural audio notes

### G71. `init` is a reserved word, and the parse error points at the whole function

`fn init(mode : Int) : Int do x_aud_init(mode) end` in an ordinary module body
is a bare "parse error" repeated once per downstream reference — 72 of them for
one function. Renaming the function to `start` fixes it; the parameter name
`mode` is fine. `init` joins `by`, `on`, `opaque` and `spawn` (G19, G65, G42,
G34) on the list of identifiers that are silently reserved, and like those the
error names neither the word nor the reason.

`probes/` reproduction: any module with `fn init(x : Int) : Int do x end`.

### G72. A single-letter module alias silently resolves to the wrong module

`alias CubeForge.Biome as B` in one module and `alias CubeForge.Audio as A` in
its test compiled and typechecked cleanly, then failed at link time with

```
"_CubeForge.Biome.append", referenced from: ___march_test_34__
"_CubeForge.Biome.capacity", referenced from: ___march_test_92__
"_CubeForge.Biome.clear", referenced from: _CubeForge.Marker.build
```

`append`, `capacity`, `clear` and `get` are `F32Buf`'s, not `Biome`'s: calls
from *other* modules entirely were rewritten into the `Biome` namespace.
Renaming the aliases to `Biome` and `Audio` fixed it outright. This is G48 (an
alias resolving to the wrong module at link time) with a sharper edge: the
damage is not confined to the module that declares the alias, and the symbols
named in the error belong to a module the failing file never mentions.

Practical rule for this codebase: do not alias a module to a single letter. `N`
for `Noise` survives only because nothing else claims it.

### G41 again: the capability ceiling charges a pure test for the whole graph

`test/audio_test.march` calls nothing but pure functions, and needs
`IO.Spawn`, `IO.Clock`, `IO.Console` and `IO.Process` declared because
`CubeForge.Audio` aliases `CubeForge.Biome`. `test/biome_test.march` aliases
`Biome` directly and needs only `IO.Spawn`, so the ceiling is not simply the
transitive closure of the alias graph either.
