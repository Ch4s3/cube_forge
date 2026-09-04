# Fixing G67: reading a heap value in a loop inflates its refcount

**Status:** root cause located and a one-line experiment validated; not landed.
**Repo:** `~/code/march` (`lib/tir/borrow.ml`)
**Symptom it explains:** GAPS.md G67 — cube_forge's skylight sweep cannot
ping-pong two buffers, so it copies ~60 MB per block edit.

## What is actually wrong

March forbids non-tail self-recursion, so **every loop over a heap value is a
TCO loop**. In such a loop the parameter is used at two positions per iteration:

```march
pfn a_go(a : NativeU8Arr, i : Int, n : Int, acc : Int) : Int do
  if i >= n do acc else a_go(a, i + 1, n, acc + NativeArray.get_u8(a, i)) end
end
```

- `NativeArray.get_u8(a, i)` — a read, which should be a *borrowed* position
- `a_go(a, ...)` — forwarded to the self-call, an *owned* position

The dual-position invariant (`specs/perceus-invariants.md`, commit `a5dad194`)
correctly requires a dup when a variable appears at both. But the owned transfer
here is the TCO back-edge, which lowers to `store ptr %ld24, ptr %a.addr` — `a = a`
— and never drops the old reference. So the dup is emitted and never balanced:

```llvm
case_default5:
  %ld17 = load ptr, ptr %a.addr
  call void @march_incrc_local(ptr %ld17)      ; per iteration
  %cr20 = call i64 @native_u8_arr_get(ptr %ld18, i64 %ld19)
  ...
  store ptr %ld24, ptr %a.addr                 ; back-edge: no decrc anywhere
  br label %tco_loop1
```

One `incrc`, zero `decrc` in the whole function. Measured against cube_forge's
relight sweep, the count scales at ~1 per neighbour read: 5 202 for a 9³ box,
44 198 for 17³, 365 754 for 33³.

### Why the parameter is owned at all

`owned_in` classifies an argument as owning when the callee's parameter is not
known to be borrowed. `native_u8_arr_get`'s is not known to be borrowed, because
**`extern_borrow_table` in `lib/tir/borrow.ml:46` has no `NativeArray` entries at
all.** It has `ring_buf_get`, `march_string_byte_at`, and the whole string family —
the array family was never added. So every array read looks like an ownership
transfer, which flips the enclosing parameter to owned, which forces the dup.

Confirmed by a three-case matrix (`--emit-llvm`, count `incrc`/`decrc` per fn):

| loop | classification | incrc | decrc |
|---|---|---|---|
| reads the array each iteration | `a:own` | 1 | 0 |
| forwards it, never reads it | `a:borrow` | 0 | 0 |
| reads a `List` each iteration | `l:own` | 1 | 0 |

The middle row is the control: remove the read and the leak disappears. The read
is what causes it, not the forwarding.

## The fix

### Step 1 — add the NativeArray read builtins to the borrow table

In `extern_borrow_table`, for each of `u8`, `i32`, `f32`, `int`, `float`:

```ocaml
("native_u8_arr_get",    [true; false]);
("native_u8_arr_length", [true]);
("native_u8_arr_sum",    [true]);
("native_u8_arr_to_list",[true]);
```

Deliberately **not** listed: `_set`, `_map`, `_map2`, `_from_list`,
`_alloc_raw`, `_make`, `_filter_mask`. `set` updates in place and must keep
consuming its array, or it loses the uniqueness FBIP depends on. Getting that
wrong is the way this change turns a leak into a double-free, so the split
between reads and writes is the whole safety argument and each added row needs
to be justified against the builtin's C implementation, not guessed from its name.

**Already validated.** With those rows added and the compiler rebuilt:

```
before: a_go(a:own, ...)     a_go incrc: 1
after:  a_go(a:borrow, ...)  a_go incrc: 0
```

Regression status of that experiment so far, on `~/code/march` at `c25bb820`
plus the table rows (uncommitted):

| suite | result |
|---|---|
| `dune build @runtest` (codegen + eval) | **576 tests pass**, 648 s, 1 skip (missing cross sysroot) |
| `run_snapshots.exe -e` (post-perceus goldens) | **33 tests pass**, no golden changed |
| `test_properties.exe -e` (differential oracle) | running when this was written |
| `dune build @oracle` (bench/examples sweep) | not yet run |
| cube_forge's own 79 tests against a patched toolchain | not yet run |

That no golden changed is itself a finding: the snapshot corpus has a
`self_tco_loop` fixture, but nothing in it reads a heap parameter inside the
loop, which is why an `inc_rc` with no `dec_rc` never showed up
in a golden. Step 3 is not optional.

The read loop now emits no refcount traffic at all — not a balanced inc/dec pair,
but none, which is also the performance win: the per-iteration `incrc` was pure
overhead on every array loop in the language.

### Step 2 — prove it did not break ownership somewhere else

This is the real work; step 1 is four lines per array kind. The risk is that some
caller relied on a read builtin consuming its argument, and now nobody frees it
(leak) or everybody does (double free). In order of value:

1. `dune build @runtest` — the codegen and eval suites.
2. `./_build/default/test/run_snapshots.exe -e` — the post-perceus goldens. This
   is the artifact to actually read: `git diff test/snapshots/` after a
   `UPDATE_SNAPSHOTS=1` regen shows the exact inc/dec shape that changed, and
   every disappearing `inc_rc` should be one whose `dec_rc` was already missing.
3. `./_build/default/test/test_properties.exe -e` — the differential oracle. A
   compiled binary killed by a signal where the interpreter succeeded is a hard
   failure, so a double-free introduced here surfaces without a targeted test.
4. `dune build @oracle` — the full bench/examples sweep, zero *new* divergences.
5. Rebuild cube_forge against the patched compiler and re-run its 79 tests.

### Step 3 — a regression test that pins the shape

Add `test/snapshots/src/borrowed_array_loop.march` (the `a_go` above, under 20
lines) to the corpus in `test/test_snapshots.ml`, regenerate, and eyeball the
`.expected` files. The golden is the thing that stops this from silently coming
back — the property is "no `inc_rc` on `a` in the loop body", which no
output-comparison test can see.

Also worth a direct assertion in `test/test_codegen.ml`: compile the loop and
assert the emitted IR for `a_go` contains zero `march_incrc`.

### Step 4 — take the win in cube_forge

With `a:borrow`, `Light.box_sweep_go` can ping-pong two preallocated buffers and
re-sync only the written y-slice instead of allocating and copying a fresh 2 MB
prefix per pass. Expected: the sweep's remaining ~10 ms drops substantially and
~60 MB of memory traffic per block edit goes away. The ping-pong code is already
written and was reverted in `b951435`; it can come back as-is.

## What this does not fix

The `List` row of the matrix stays `own`. Its read lowers to `go$apply$4435`, a
closure apply wrapper, and `owned_in`'s `ECallPtr` case is "unknown callee —
conservative: any arg use is owning". Param 0 of an apply function is also
*pinned* owned by the closure ABI, with a comment recording that a previous
attempt to relax it produced three double-frees and eight stdlib crashes.

So indirect and defunctionalized calls keep leaking one refcount per iteration.
That is a genuinely harder problem — it needs either a borrow signature carried
through defunctionalization or a per-callsite escape analysis — and it should be
a separate piece of work, not smuggled into this one.

## Two alternatives I considered and rejected

**Balance the TCO back-edge** — at the back-edge, skip the dup when the argument
is syntactically the same variable as the parameter slot it is stored into
(`a = a`, where dup and the iteration-end drop cancel exactly). This is more
general: it fixes the `List` case too, and any future builtin missing from the
table. I did not pick it as step 1 because it treats the symptom — the dup is
correctly required by the dual-position rule and the real defect is that the read
was never a borrow — and because it needs care that the argument is not also live
after the loop. It is the right *second* fix, and it is what would make the table
in step 1 a performance optimization rather than a correctness dependency.

**Move the extern seeding into `init`.** User-defined externs are seeded as
borrowed only *after* the fixpoint (`borrow.ml`, "Seed user-defined extern (FFI)
functions"), and the `$clo` pin's comment already warns that a post-fixpoint seed
leaves callers-of-callers classified against a stale answer. So FFI-heavy March
code likely has this same bug independently of the builtin table. Worth
confirming with the same three-case matrix over a user extern; I have not
measured it, so I am not claiming it.
