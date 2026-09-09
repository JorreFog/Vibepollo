#!/usr/bin/env bash
# Measure how evenly each available frame limiter delivers frames.
#
# Frame pacing, not average FPS, is what makes a stream look smooth: an
# encoder sampling at a fixed rate turns uneven frame delivery into visible
# judder. This runs a workload under each provider and reports the spread of
# frame times around the target.
#
# tools/frametime_probe.c is the measuring instrument: a GLX workload that
# timestamps its own swaps. Limiters hook glXSwapBuffers, so their effect on
# the cadence shows up exactly as a game would experience it.
#
# It used to use MangoHud's CSV log instead, which made MangoHud both the
# limiter under test and the instrument measuring it, and which silently
# produces no log at all on MangoHud 0.8.4.
#
# Usage: benchmark_frame_limiter.sh [target_fps] [seconds]
set -uo pipefail

TARGET_FPS="${1:-60}"
DURATION="${2:-20}"
shift 2 2>/dev/null || true
OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }

# Build the probe next to this script if it has not been built already.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROBE="${FRAMETIME_PROBE:-$OUT_DIR/frametime_probe}"
if [ ! -x "$PROBE" ]; then
  if ! cc -O2 "$SCRIPT_DIR/frametime_probe.c" -lGL -lX11 -o "$PROBE" 2>"$OUT_DIR/cc.log"; then
    echo "Could not build the frame time probe:" >&2
    cat "$OUT_DIR/cc.log" >&2
    exit 1
  fi
fi

find_lib() {
  for dir in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib; do
    for candidate in "$dir/$2/$1" "$dir/$1"; do
      [ -f "$candidate" ] && { echo "$candidate"; return 0; }
    done
  done
  return 1
}

# MangoHud >= 0.8 only hooks OpenGL through libMangoHud_shim.so; preloading
# libMangoHud.so there attaches nothing and applies no limit at all.
MANGOHUD_LIB="$(find_lib libMangoHud_shim.so mangohud || find_lib libMangoHud.so mangohud || true)"
STRANGLE_LIB="$(find_lib libstrangle.so strangle || true)"

if [ -z "${DISPLAY:-}" ]; then
  echo "The probe needs an X display (XWayland is fine); DISPLAY is unset." >&2
  exit 1
fi

# Summarise the probe's CSV: a "frametime_ms" header followed by one
# millisecond frame time per row.
summarise() {
  local csv="$1" label="$2"
  python3 - "$csv" "$label" "$TARGET_FPS" <<'PY'
import csv, statistics, sys

path, label, target = sys.argv[1], sys.argv[2], float(sys.argv[3])
frametimes = []
with open(path, newline="") as handle:
    for row in csv.reader(handle):
        if not row:
            continue
        try:
            value = float(row[0])
        except ValueError:
            continue          # the header row
        if 0 < value < 1000:
            frametimes.append(value)

if len(frametimes) < 30:
    print(f"{label:<14} no usable samples ({len(frametimes)})")
    sys.exit(0)

# Drop the first second or so; startup is not representative.
frametimes = frametimes[int(target):] or frametimes
mean = statistics.fmean(frametimes)
target_ms = 1000.0 / target
ordered = sorted(frametimes)
p99 = ordered[int(len(ordered) * 0.99) - 1]
print(
    f"{label:<14} fps={1000.0 / mean:7.2f}  frametime={mean:6.2f}ms "
    f"(target {target_ms:.2f})  stdev={statistics.pstdev(frametimes):5.2f}ms  "
    f"p99={p99:6.2f}ms  max={ordered[-1]:6.2f}ms  n={len(frametimes)}"
)
PY
}

run_case() {
  local label="$1" limit="$2"; shift 2
  local csv="$OUT_DIR/$label.csv"

  echo "== $label (limit=${limit:-none}) =="
  # Only the case actually testing MangoHud preloads it; every other case must
  # run with no limiter attached at all, or it would measure the wrong one.
  timeout $((DURATION + 12)) env "$@" "$PROBE" "$DURATION" > "$csv" 2>/dev/null

  if [ -s "$csv" ]; then
    summarise "$csv" "$label"
  else
    echo "$label: the probe recorded nothing"
  fi
  echo
}

echo "probe    : $PROBE"
echo "target   : ${TARGET_FPS} FPS for ${DURATION}s per case"
echo "mangohud : ${MANGOHUD_LIB:-(wrapper only)}"
echo "strangle : ${STRANGLE_LIB:-not installed}"
echo

run_case "unlimited" "0"

if [ -n "$MANGOHUD_LIB" ]; then
  run_case "mangohud" "$TARGET_FPS" \
    MANGOHUD=1 \
    MANGOHUD_CONFIG="fps_limit=${TARGET_FPS},no_display" \
    LD_PRELOAD="$MANGOHUD_LIB"
else
  echo "mangohud: no library found, skipped"; echo
fi

if [ -n "$STRANGLE_LIB" ]; then
  run_case "libstrangle" "$TARGET_FPS" FPS="$TARGET_FPS" LD_PRELOAD="$STRANGLE_LIB"
fi

# "xprop -root <name>" exits 0 and prints "not found" when the property is
# absent, so its exit status says nothing. Match the value line instead.
# Setting the property to probe for it would be worse than useless: it makes
# every later gamescope detection on this machine succeed against a compositor
# that is not there, so restore whatever was already published and never
# create it here.
gamescope_limit="$(xprop -root GAMESCOPE_FPS_LIMIT 2>/dev/null | sed -n 's/^GAMESCOPE_FPS_LIMIT(CARDINAL) = \([0-9]*\)$/\1/p')"
if [ -n "${DISPLAY:-}" ] && have xprop && [ -n "$gamescope_limit" ]; then
  xprop -root -f GAMESCOPE_FPS_LIMIT 32c -set GAMESCOPE_FPS_LIMIT "$TARGET_FPS"
  run_case "gamescope" "$TARGET_FPS"
  xprop -root -f GAMESCOPE_FPS_LIMIT 32c -set GAMESCOPE_FPS_LIMIT "$gamescope_limit"
else
  echo "gamescope: no GAMESCOPE_FPS_LIMIT published on the root window, skipped"
  echo
fi

echo "Lower stdev and a p99 close to the mean mean smoother delivery to the encoder."
