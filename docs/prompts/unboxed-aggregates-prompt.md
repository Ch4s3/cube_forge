# Task: unboxed small aggregates, stack promotion across calls, and a transient allocation contract for March

Repository: the March compiler (OCaml, LLVM backend, Perceus reference counting).
Read `CLAUDE.md` first. Build with `dune build`, test with `scripts/run-tests.sh`
(quick: `-q`). Work on a branch in a detached worktree off `origin/main`; do not
touch the main checkout's dirty tree. Do not push.

## Motivating evidence

The voxel engine at `~/code/cube_forge` (see its `GAPS.md`, entries G1-revisited,
G57, G58, and `RESULTS.md` "@[no_alloc] contract sweep") annotated 40 per-frame
functions with `@[no_alloc(warn)]` on march `137737f3`. 15 verified. The rest
were rejected for one reason: every constructor with fields is a heap cell.

```march
type Vec3 = Vec3(Float, Float, Float)
fn forward(yaw : Float, pitch : Float) : Vec3 do
  let cp = Math.cos(pitch)
  Vec3(0.0 -. Math.sin(yaw) *. cp, Math.sin(pitch), 0.0 -. Math.cos(yaw) *. cp)
end
-- `forward` is marked @[no_alloc] but allocates.
--   In `forward`: constructor `Vec3` is allocated here.
```

`Vec3` compiles to `march_alloc(40)`: a 16-byte header (rc, tag) and three
doubles. There is no dying cell of the same shape to reuse (the inputs are
scalars), so FBIP cannot fire, and stack promotion does not fire either, even
when the caller reads the three fields and drops the value in the same
expression. The same holds for `Quat(Float x4)`, `Hit(Bool, Int x6)`,
`Sweep(Float x3, Bool)`. The engine's live-object gauge shows these cells are
freed the same frame, so the cost is allocator churn (about a dozen
malloc/free pairs per frame), not a leak, but the contract cannot be stated
and math-heavy code pays a heap round trip per vector.

Three pieces of work, in order of value. Each is independently landable;
do them as separate commits with their own tests. If a piece turns out to be
unsound or too large, stop, write up why in `specs/todos/`, and continue with
the next.

## 1. Unboxed representation for small scalar-only single-constructor variants

Goal: a single-constructor variant whose fields are all `Int`/`Float`/`Bool`
(and whose total size is at most, say, 4 words) is represented **inline**:
passed and returned in registers or by value, stored inline inside another
constructor's fields and in `NativeArray` slots where applicable, never
reference-counted, never allocated. `Vec3(x, y, z)` becomes three doubles.

Requirements:
- Semantics unchanged: pattern matching, equality, `Show`, field extraction,
  passing to closures and actors all behave exactly as today. The interpreter
  needs no change beyond agreeing on observable behaviour (the differential
  oracle in `test/test_properties.ml` and `dune build @oracle` must stay green).
- The decision must be a **per-type representation choice** made once
  (`lib/tir/repr.ml` is where representations are decided today; read it and
  `docs/value-representation.md` before designing). Do not special-case names.
- Interaction with Perceus: an unboxed value has no RC; `needs_rc` in
  `lib/tir/rc_types.ml` must say false for it, and `borrow.ml` must treat it as
  a scalar. Read the module doc in `rc_types.ml` about the existing
  divergence table before changing any arm; add a row for this case.
- Interaction with generics: when such a value flows into a generic (type-
  variable) position, box it, the way `Float` is boxed today
  (`march_alloc_float` / `march_unbox_float`); reuse that machinery rather
  than inventing a second one. Document the boundary in
  `docs/value-representation.md`.
- Interaction with the FFI: an unboxed variant passed to an `extern` should
  either be rejected with a clear error or marshalled as its fields; pick
  rejection unless it falls out for free, and say so in `docs/ffi.md`.
- Interaction with `@[no_alloc]`: constructing one must count as no allocation.
- Newtype (single-field) variants are already unboxed as their payload; make
  sure this work composes with `scrutinee_shares_payload_storage` in
  `lib/tir/perceus.ml` rather than conflicting with it.
- Tests: `test/test_codegen.ml` cases asserting (a) the LLVM IR for
  constructing `Vec3` contains no `march_alloc`, (b) a `Vec3`-heavy loop
  reports 0 via `march_live_allocs`, (c) `@[no_alloc]` accepts `forward` above;
  TIR snapshot fixtures under `test/snapshots/src/` for the lowering and the
  Perceus stage; `run_eval` parity for pattern matching and equality; a
  benchmark in `bench/` (a vector-math loop) with before/after numbers in the
  commit message.

## 2. Stack promotion through the immediate consumer

Goal: a heap-represented constructor whose value provably does not escape the
current function, **including when it is passed to a callee that only reads
it** (borrowed parameter, per `borrow.ml`'s inference), is allocated on the
stack. Today the escape analysis stops at the call boundary.

Requirements:
- Find the existing promotion pass (search for "stack-allocated" / `⚡` in the
  LSP performance annotations and the memory-model docs; the pass the docs
  call escape analysis). Extend its "does not escape" verdict to: the value is
  only used as an argument at positions the borrow fixpoint marks `borrow`,
  and is dead after the last such call.
- Never promote a value that is stored into another heap value, captured by a
  closure, sent to an actor, returned, or passed at an `own` position.
- A promoted cell still has a header (tag is read by `case`); only its
  storage moves. Make sure `march_decrc_local` / drop paths are never called
  on a stack cell (audit `emit_case` in `lib/tir/llvm_case.ml` and the drop
  synthesis in `lib/tir/drop.ml`).
- `MARCH_SANITIZE=1` builds of the codegen tests must stay clean.
- Tests: IR assertion that `forward`'s cell becomes an `alloca` when its only
  use is `V.x(v)`/`V.y(v)`/`V.z(v)` in the caller; a `@[no_alloc]` acceptance
  test; snapshot fixtures; the property oracle.

## 3. A transient-allocation contract

Goal: `@[no_alloc(transient)]` (name negotiable; check `specs/` for an
existing plan first) meaning "this function allocates nothing that survives
the call": every allocation it or its callees perform is released before the
function returns. This is the property the engine's frame loop actually has
(1 net live object per frame) and cannot state today.

Requirements:
- Implement it in the same pipeline as `@[no_alloc]` (`lib/tir/` contract
  checker introduced in the `feat(contracts)` commits of 2026-09-03; read the
  design spec under `specs/` first). Reuse its classifier and transitive set;
  the new verdict is "every allocation site's value is provably dead by
  function exit", which you can compute from Perceus's inserted `dec_rc`
  positions and the escape verdict from part 2.
- Be conservative: any allocation that reaches a return value, a field store,
  a closure capture, an actor mailbox, a `Vault`, or an extern is a failure
  with a diagnostic naming the site, in the same voice as the existing
  `@[no_alloc]` messages ("is marked @[no_alloc(transient)] but retains …").
- Whole-function and transitive like the base contract; document that an
  amortized growth path (a buffer that reallocates its storage and keeps it)
  is *retained* and therefore still rejected, so nobody expects it to cover
  that case.
- LSP: the same lens/quick-fix treatment the base contract has.
- `forge fix --contracts` should prefer the strongest attribute that holds.
- Tests mirroring the base contract's `vectorize_check`/contract test groups.

## Verification for all three

- `scripts/run-tests.sh` full suite green; `dune build @oracle` green with no
  new `known_divergence` entries.
- Rebuild `~/code/cube_forge` against the result (`forge toolchain pin`
  a new `~/.march/versions/<name>` built from your branch; see that repo's
  README for the layout) and re-run its `@[no_alloc]` sweep: report how many
  of the 40 annotated functions now verify under the base contract and under
  `transient`, and the change in its `CF_ALLOC_PROBE` numbers. The engine's
  25 tests and `MARCH_PIN_MAIN=1 CF_FRAMES=240 ./.march/build/release/cube_forge`
  must still pass.
- Update `CHANGELOG.md` `[Unreleased]`, `specs/todos/` → `specs/progress/`
  per `CLAUDE.md`, and `docs/memory-model.md` / `docs/value-representation.md`.

Report failures plainly. If part 1 is only possible for a narrower class than
"scalar-only, ≤ 4 words", say what class you landed and why.
