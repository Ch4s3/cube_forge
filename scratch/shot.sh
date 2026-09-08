#!/bin/zsh
# One screenshot of the running game, as a PNG.
#
#   scratch/shot.sh [options] [-- CF_FOO=bar ...]
#
# Why a script: a usable shot needs eight environment variables to agree, and
# four of them are not optional. Without CF_NOMOUSE the camera yaw depends on
# where the window manager left the pointer, so the same command gives a
# different picture every run. Without CF_AUTOSTART the game sits on the start
# screen and the dump is of a menu. CF_DUMP_FRAME must be a frame the run
# actually reaches. And the dump lands as a BMP, which most things will not
# open. Getting one of those wrong wastes a build-and-run, so they live here.
#
# The defaults are the pinned scenario the rest of scratch/ uses -- seed 7, sun
# at 45 degrees, clock and weather stopped -- so two shots taken a week apart
# are of the same world in the same light, and a difference between them is a
# difference in the code.
#
#   scratch/shot.sh -o /tmp/a.png
#   scratch/shot.sh --seed 18 --spawn 96,96 --yaw 90 --pitch -40 -o /tmp/b.png
#   scratch/shot.sh --night -- CF_AUTOGLOW=100      # any other knob, verbatim
#
# CAMERA. --yaw is DEGREES, 0 looking toward -Z and turning clockwise, so 90 is
# +X (east) and 180 is +Z. --pitch is HUNDREDTHS OF A RADIAN and negative is
# DOWN: -25 is the default gentle downward look, -80 is looking at your feet,
# +50 is up at the sky. These are the game's own units (CF_YAW, CF_PITCH) and
# they are not the same unit, which is worth saying twice.
#
# Some scripted modes pin the camera themselves and ignore --pitch: CF_AUTODIVE
# and CF_AUTOSWIM both spawn looking down at -1.4 radians.
set -u

# Absolute, and taken BEFORE the cd: inside a zsh function $0 is the function's
# own name, so --help read itself out of a file called "usage".
self=${0:A}
cd "${self:h}/.."

out=shot.png
seed=7
frame=200
sun=45
clock=0
weather=0
width=800
height=600
yaw=""; pitch=""; spawn_x=""; spawn_z=""; spawn_y=""
walk=0; keep_bmp=0; build=""
extra=()

die() { print -r -- "shot: $*" >&2; exit 2 }

usage() {
  awk 'NR>1 { if ($0 ~ /^set -u/) exit; sub(/^# ?/, ""); print }' "$self"
  cat <<'USAGE'
Options:
  -o, --out PATH     where to write the PNG (default ./shot.png)
  -s, --seed N       world seed (default 7)
  -f, --frame N      frame to dump (default 200); the run stops ten frames later
      --spawn X,Z    stand on this column instead of the generator's choice
      --spawn-y Y    stand at this height (inspection aid)
      --yaw DEG      facing, degrees, 0 = -Z, clockwise (90 = east)
      --pitch N      look angle, HUNDREDTHS of a radian, negative = down
      --sun DEG      sun angle (default 45); --night is shorthand for 180
      --night        sun below the horizon
      --time N       clock pin in seconds (default 0)
      --weather N    weather pin 0..100 (default 0, clear)
      --size WxH     window size (default 800x600)
      --walk         let the scripted walk run, for a shot in motion
      --keep-bmp     keep the raw BMP beside the PNG
      --debug        use the debug binary even if a release one exists
  -h, --help         this
Anything after `--` is passed to the game as environment, verbatim.
USAGE
  exit 0
}

while (( $# )); do
  case "$1" in
    -o|--out)     out=${2:?--out needs a value}; shift 2 ;;
    -s|--seed)    seed=${2:?--seed needs a value}; shift 2 ;;
    -f|--frame)   frame=${2:?--frame needs a value}; shift 2 ;;
    --spawn)      spawn_x=${${2:?--spawn needs X,Z}%%,*}; spawn_z=${${2:?--spawn needs X,Z}##*,}; shift 2 ;;
    --spawn-y)    spawn_y=${2:?--spawn-y needs a value}; shift 2 ;;
    --yaw)        yaw=${2:?--yaw needs a value}; shift 2 ;;
    --pitch)      pitch=${2:?--pitch needs a value}; shift 2 ;;
    --sun)        sun=${2:?--sun needs a value}; shift 2 ;;
    --night)      sun=180; shift ;;
    --time)       clock=${2:?--time needs a value}; shift 2 ;;
    --weather)    weather=${2:?--weather needs a value}; shift 2 ;;
    --size)       width=${${2:?--size needs WxH}%%x*}; height=${${2:?--size needs WxH}##*x}; shift 2 ;;
    --walk)       walk=1; shift ;;
    --keep-bmp)   keep_bmp=1; shift ;;
    --debug)      build=debug; shift ;;
    -h|--help)    usage ;;
    --)           shift; extra=("$@"); break ;;
    *)            die "unknown option $1 (try --help)" ;;
  esac
done

if [[ -z $build ]]; then
  if [[ -x .march/build/release/cube_forge ]]; then build=release; else build=debug; fi
fi
BIN=.march/build/$build/cube_forge
[[ -x $BIN ]] || die "no $build build at $BIN — run: forge build${${build:#debug}:+ --release}"

# The frame has to be one the run reaches, and the dump frame drains the whole
# mesh queue, so give it a few frames of room rather than stopping on it.
frames=$(( frame + 10 ))

bmp=${out:r}.bmp
mkdir -p "${out:h}" 2>/dev/null

env MARCH_PIN_MAIN=1 CF_NOMOUSE=1 CF_AUTOSTART=1 CF_VSYNC=0 \
    CF_SEED=$seed CF_SUN=$sun CF_TIME=$clock CF_WEATHER=$weather \
    CF_WIDTH=$width CF_HEIGHT=$height \
    CF_FRAMES=$frames CF_DUMP_FRAME=$frame CF_DUMP=$bmp \
    ${yaw:+CF_YAW=$yaw} ${pitch:+CF_PITCH=$pitch} \
    ${spawn_x:+CF_SPAWN_X=$spawn_x} ${spawn_z:+CF_SPAWN_Z=$spawn_z} ${spawn_y:+CF_SPAWN_Y=$spawn_y} \
    ${${walk:#0}:+CF_AUTOWALK=1} \
    "${extra[@]}" "$BIN" > "${out:r}.log" 2>&1

if [[ ! -s $bmp ]]; then
  print -r -- "shot: the game wrote no dump — last lines of ${out:r}.log:" >&2
  tail -5 "${out:r}.log" >&2
  exit 1
fi

# BMP is what the game writes and almost nothing opens. sips ships with macOS;
# if it is ever not there the BMP is still on disk and still correct.
if sips -s format png "$bmp" --out "$out" >/dev/null 2>&1; then
  (( keep_bmp )) || rm -f "$bmp"
else
  print -r -- "shot: could not convert to PNG; the BMP is at $bmp" >&2
  out=$bmp
fi

print -r -- "$out  (seed $seed, frame $frame, $build)"
