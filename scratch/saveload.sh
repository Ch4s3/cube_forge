#!/bin/sh
# Save/load round trip: run 1 saves slot 1 at frame 201 (after the frame-200 dump),
# run 2 loads it at frame 30 and dumps frame 0 of the loaded session.
# Passes when the world hash the game prints at save equals the one at load.
set -u
T=${SAVELOAD_DIR:-/tmp/cf_saveload_$$}
mkdir -p "$T"
BIN=./.march/build/release/cube_forge
ENV="CF_SEED=7 CF_NOMOUSE=1 CF_TIME=0 CF_WORKERS=1 CF_AUTOSTART=1 CF_SAVE_DIR=$T"
env $ENV CF_FRAMES=260 CF_AUTOSAVE=201 CF_AUTOSAVE_SLOT=1 CF_DUMP_FRAME=200 CF_DUMP=$T/a.bmp $BIN > "$T/run1.log" 2>&1
env $ENV CF_FRAMES=60 CF_AUTOLOAD=30 CF_AUTOLOAD_SLOT=1 CF_DUMP_FRAME=0 CF_DUMP=$T/b.bmp $BIN > "$T/run2.log" 2>&1
grep -E "saved slot|player ended|state:" "$T/run1.log"
grep -E "loaded slot|load slot|water chunk|player ended|state:" "$T/run2.log"
H1=$(grep -o "saved slot 1 .*world hash [0-9]*" "$T/run1.log" | grep -o "[0-9]*$")
H2=$(grep -o "loaded slot 1 .*world hash [0-9]*" "$T/run2.log" | grep -o "[0-9]*$")
ls -la "$T/slot1" | head -4; du -sh "$T/slot1"
python3 scratch/cmpframe.py "$T/a.bmp" "$T/b.bmp" 2>/dev/null
if [ -n "$H1" ] && [ "$H1" = "$H2" ]; then echo "PASS: world hash $H1 round-tripped"; else echo "FAIL: save hash '$H1' load hash '$H2'"; exit 1; fi
