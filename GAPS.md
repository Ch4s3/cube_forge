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
