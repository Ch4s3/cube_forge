#!/bin/zsh
# End-to-end frame budget: run the real game and assert no frame exceeds a
# budget, and that the mesh the deferred queue produces is the mesh a full
# rebuild produces.
#
#   scratch/frame_budget.sh [budget_ms] [frames] [runs]
#
# Why a script and not a `forge test` case: this has to be the whole program --
# window, GL, actors, the frame loop -- and a unit test cannot open a window.
#
# Why best-of-N: the metric is the worst frame WITHIN a run, and that is the
# thing being asserted. Across runs we take the best, because a machine with
# another build on it measures ~1.5x slower and that is not a regression in
# this program. A real regression fails every run, so best-of-N still catches
# it; a busy machine does not.
#
# The budget is 12 ms against a program whose worst frame is 9-10 in this
# window. That headroom is deliberate: it is roughly what a loaded machine
# costs, and a test that fails when someone else is compiling is a test people
# learn to ignore. It is NOT 8.3 ms -- the 120 Hz frame -- because this scenario
# is windowed and CPU-bound; at fullscreen the phase frames still reach 13-14 ms
# and the game holds ~110 fps, not 120. Tightening this to 8.3 would assert
# something the game does not yet do.
#
# The scenario is pinned so the run is repeatable: fixed seed, fixed sun and
# clock, and CF_NOMOUSE, without which the camera yaw depends on where the
# window manager put the pointer.
set -u

cd "$(dirname "$0")/.."
BIN=.march/build/release/cube_forge
BUDGET_MS=${1:-12}
FRAMES=${2:-400}
RUNS=${3:-3}

if [[ ! -x $BIN ]]; then
  echo "no release build at $BIN — run: forge build --release"
  exit 2
fi

# Pinned, headed, uncapped. Uncapped matters: with vsync on, a frame's elapsed
# time includes the wait for the display and every frame would read as 8 or
# 16 ms whatever the work was.
#
# No CF_DUMP_FRAME in the timed runs. That frame drains the whole queue and
# hashes four million voxels, so it is the longest frame in any run that has
# one -- 56 ms -- and it is a diagnostic, not the game. The hash check below
# gets its own run.
run_game() {
  env MARCH_PIN_MAIN=1 CF_NOMOUSE=1 CF_VSYNC=0 CF_SEED=7 CF_SUN=45 CF_TIME=0 \
      CF_FRAMES=$FRAMES "$@" $BIN 2>&1
}
run_hashed() {
  run_game CF_DUMP_FRAME=$((FRAMES - 10)) "$@" | grep '  state:'
}
# The mesh hash off the "state:" line. Plain `grep mesh` also matches the
# startup line "mesh all (greedy sections)", which has no number after it.
mesh_of() { print -r -- "$1" | grep -o 'mesh [0-9][0-9]*' | tail -1 }

echo "frame budget: ${BUDGET_MS} ms over $FRAMES frames, best of $RUNS (seed 7)"

best=""
for i in $(seq 1 $RUNS); do
  out=$(run_game)
  worst=$(print -r -- "$out" | grep -o 'worst frame after warmup: [0-9.]*' | grep -o '[0-9.]*$')
  if [[ -z $worst ]]; then
    echo "FAIL: the run printed no worst-frame line"
    print -r -- "$out" | tail -20
    exit 1
  fi
  printf "  run %d: worst frame %.2f ms\n" $i $worst
  if [[ -z $best ]] || (( worst < best )); then
    best=$worst
  fi
done

fail=0

printf "\nworst frame (best of %d): %.2f ms against a %s ms budget — " $RUNS $best $BUDGET_MS
if (( best > BUDGET_MS )); then
  echo "FAIL"
  fail=1
else
  echo "ok"
fi

# The queue may not lag behind the world: the dump frame drains it to empty and
# hashes the result, so this compares what the queue finishes against a full
# rebuild of every pass of every section.
incremental=$(mesh_of "$(run_hashed)")
truth=$(mesh_of "$(run_hashed CF_REMESH_ALL_AT=$((FRAMES - 11)))")
echo "drained $incremental against full-rebuild $truth — \c"
if [[ -z $incremental || -z $truth || $incremental != $truth ]]; then
  echo "FAIL"
  fail=1
else
  echo "ok"
fi

exit $fail
