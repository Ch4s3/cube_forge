# Fix: an unboxed small aggregate built inside a branch leaks

You are working in the March compiler repo (`~/code/march`). There is a memory
leak in the "unboxed small scalar aggregates" feature on `main`
(commit `c0275445`, merged as `7419c689`). It was found by an external project
building against `main` and is reproducible in 20 lines.

## The bug

A single-constructor variant whose fields are all scalars, arity 2..4, is
represented as an inline LLVM struct value. When such a value is **constructed
inside a branch**, one heap object leaks per construction.

Save this as `/tmp/g69.march`:

```march
mod G69 do
  needs IO
  needs IO.Console
  needs IO.Foreign

  extern "c" : Cap(IO.Foreign) do
    fn x_live_allocs(): Int = "march_live_allocs"
  end

  type Pair = Pair(Float, Float)
  pfn fst(p : Pair) : Float do match p do Pair(a, _) -> a end end
  pfn snd(p : Pair) : Float do match p do Pair(_, b) -> b end end

  -- A: no branch — does NOT leak
  pfn straight(i : Int, acc : Float) : Float do
    if i == 0 do acc
    else
      let p = Pair(1.0, 2.0)
      straight(i - 1, acc +. fst(p) +. snd(p))
    end
  end

  -- B: built in a branch — LEAKS one object per iteration
  pfn branched(i : Int, acc : Float) : Float do
    if i == 0 do acc
    else
      let p = if i % 2 == 0 do Pair(1.0, 2.0) else Pair(3.0, 4.0) end
      branched(i - 1, acc +. fst(p) +. snd(p))
    end
  end

  -- C: built in a callee — does NOT leak
  pfn make(a : Float) : Pair do Pair(a, a +. 1.0) end
  pfn viacall(i : Int, acc : Float) : Float do
    if i == 0 do acc
    else
      let p = make(int_to_float(i))
      viacall(i - 1, acc +. fst(p) +. snd(p))
    end
  end

  pfn scale(name : String, f) : Unit do
    let a = x_live_allocs()
    let r = f(5000)
    println("  " ++ name ++ ": live delta " ++ int_to_string(x_live_allocs() - a) ++ " (" ++ float_to_string(r) ++ ")")
  end

  fn main(_cap : Cap(IO)) : Unit do
    scale("no branch      ", fn n -> straight(n, 0.0))
    scale("built in branch", fn n -> branched(n, 0.0))
    scale("across a call  ", fn n -> viacall(n, 0.0))
  end
end
```

```
dune build bin/main.exe
./_build/default/bin/main.exe --compile -o /tmp/g69 /tmp/g69.march && /tmp/g69
```

Observed on `7419c689`, against the same file built at `137737f3` (before the
feature):

| shape, 5 000 iterations | before | after |
|---|---|---|
| no branch | 3 | 3 |
| built in an `if` | 1 | **5 001** |
| built in a callee | 1 | 1 |

It is a leak, not a transient allocation — the delta scales exactly with the
iteration count (100 → 103, 1 000 → 1 001, 10 000 → 10 001).

## Where it comes from

`lib/tir/llvm_ctx.ml`, the coercion arm `| (sty, "ptr") when
Repr.unboxed_of_llvm_ty sty <> None`. The two arms of an `if` merge through a
join slot typed `ptr`, so an unboxed struct is materialised onto the heap to
cross it:

```llvm
%ubmk21 = insertvalue %ub.Pair %ubmk20, double 2.0, 1
%ubbox22 = call ptr @march_alloc(i64 32)
%ubf24  = extractvalue %ub.Pair %ubmk21, 0
store double %ubf24, ptr %ubfp25
store ptr %ubbox22, ptr %res_slot18
```

The `137737f3` build allocates at the same point but emits **seven**
`march_decrc_local` in this function; the `7419c689` build emits **one**. The
`%ubbox` cell is never decremented.

That arm's own comment states the intended contract:

> Ownership: the box is a fresh rc=1 cell that Perceus does not track
> ([Rc_types.needs_rc] is false for the aggregate ...), the same position a
> boxed Float is in.

and the reverse arm says the box "is left alone; whoever owns it releases it,
exactly as for `march_unbox_float`". **In the branch-join case nobody owns it.**
The value is boxed, stored to the join slot, loaded back, unboxed to a struct,
and the cell is dropped on the floor. The Float analogy does not carry, because
`needs_rc` is true for a boxed Float and false for the aggregate — so the RC
pass inserts a drop for one and not the other.

## What to do

Establish the mechanism yourself before changing anything — confirm the missing
decrement rather than taking the above on trust. Then pick a fix; two look
plausible and they are not equivalent:

1. **Make the materialised box owned.** Give the RC pass a case for it so a drop
   is emitted at the end of its live range. Correct, but keeps an allocation per
   construction that the boxed representation did not have to make, so the
   feature would still be a pessimisation for this shape.
2. **Do not materialise at all.** Type the branch join slot as the struct rather
   than `ptr` when both arms produce the same unboxed type. Then no heap cell
   exists and there is nothing to free. Strictly better where it applies, and
   the "no branch" and "across a call" rows above show the machinery already
   handles unboxed values in registers — but it is a larger change and there may
   be join sites (generic payloads, closures, task trampolines) that genuinely
   need the box.

A hybrid is likely right: (2) where both arms agree on the unboxed type, (1) as
the fallback for the joins that must still box. Say which you chose and why.

## Verification

Required, in this order:

- `dune build @runtest` — codegen and eval suites (~11 min, ~576 tests).
- `./_build/default/test/run_snapshots.exe -e` — post-Perceus goldens. Add a
  fixture for this shape to `test/snapshots/src/` and the corpus in
  `test/test_snapshots.ml`, regenerate with `UPDATE_SNAPSHOTS=1`, and **read the
  diff** — the point is that a human can see the drop appear.
- `./_build/default/test/test_properties.exe -e` — the differential oracle
  (~80 min). Slow, but it is the suite that turns an RC mistake into a hard
  failure rather than a silent leak.
- `dune build @oracle` — the bench/examples sweep; zero *new* divergences.
- The repro above must print a flat delta for all three rows.

A leak has no failing assertion, so a test that only checks output will not
catch a regression here. The snapshot fixture and a live-object assertion are
what pin it.

## Notes

- `lib/tir/rc_types.ml` documents `needs_rc`/`borrow_eligible` and a divergence
  table; read it before changing either, since flipping `needs_rc` for unboxed
  aggregates would have effects well beyond this arm.
- `specs/perceus-invariants.md` is the governing ownership spec. If your fix
  changes who drops what, it should be reflected there.
- The feature's own write-up is
  `specs/progress/2026-09-03-unboxed-small-scalar-aggregates.md`. It reports
  allocation unchanged on the engine it was measured against; that measurement
  predates the branch-built aggregate that trips this, so it is not in conflict
  with the above.
- Two dead ends already ruled out, so you do not repeat them: a static count of
  `march_alloc` call sites shows *fewer* allocations after the change (4 against
  6) because the extra sites in the old build sit outside the loop — call sites
  are not executions. And the leaked object is the aggregate itself, not a
  re-boxed `Float` field; the fields stay raw doubles throughout.
