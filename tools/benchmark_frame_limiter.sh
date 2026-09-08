#!/usr/bin/env bash
# Measure how evenly each available frame limiter delivers frames.
#
# Frame pacing, not average FPS, is what makes a stream look smooth: an
# encoder sampling at a fixed rate turns uneven frame delivery into visible
# judder. This runs a workload under each provider and reports the spread of
# frame times around the target.
#
# MangoHud is used as the measuring instrument in every run (its CSV log has
# per-frame timings). When MangoHud is also the limiter under test it does
# both jobs; otherwise it logs with its own limiter switched off.
#
# Usage: benchmark_frame_limiter.sh [target_fps] [seconds] [workload...]
set -uo pipefail

TARGET_FPS="${1:-60}"
DURATION="${2:-20}"
shift 2 2>/dev/null || true
WORKLOAD=("${@:-vkcube}")

OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }

find_lib() {
  for dir in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib; do
    for candidate in "$dir/$2/$1" "$dir/$1"; do
      [ -f "$candidate" ] && { echo "$candidate"; return 0; }
    done
  done
  return 1
}

MANGOHUD_LIB="$(find_lib libMangoHud.so mangohud || true)"
STRANGLE_LIB="$(find_lib libstrangle.so strangle || true)"

if [ -z "$MANGOHUD_LIB" ] && ! have mangohud; then
  echo "MangoHud is required to record frame times; install it and re-run." >&2
  exit 1
fi

if ! have "${WORKLOAD[0]}"; then
  echo "Workload '${WORKLOAD[0]}' not found. Pass one explicitly, e.g. 'glxgears'." >&2
  exit 1
fi

# Summarise a MangoHud CSV: it starts with a metadata line, then a header,
# then one row per frame whose second column is the frame time in microseconds.
summarise() {
  local csv="$1" label="$2"
  python3 - "$csv" "$label" "$TARGET_FPS" <<'PY'
import csv, statistics, sys

path, label, target = sys.argv[1], sys.argv[2], float(sys.argv[3])
raw = []
with open(path, newline="") as handle:
    for row in csv.reader(handle):
        if len(row) < 2:
            continue
        try:
            value = float(row[1])
        except ValueError:
            continue          # the system-info and header rows
        if value > 0:
            raw.append(value)

# MangoHud has logged frametimes in microseconds; rather than depend on that,
# infer the unit from the magnitude. Anything a limiter would produce is well
# under 1000 ms per frame, so a median above that must be microseconds.
if raw and statistics.median(raw) > 1000:
    frametimes = [v / 1000.0 for v in raw]
else:
    frametimes = list(raw)
frametimes = [v for v in frametimes if 0 < v < 1000]

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
  local folder="$OUT_DIR/$label"
  mkdir -p "$folder"

  echo "== $label (limit=${limit:-none}) =="
  # log_duration/autostart_log make MangoHud write one CSV and stop on its own.
  env "$@" \
    MANGOHUD=1 \
    MANGOHUD_CONFIG="fps_limit=${limit},no_display,output_folder=${folder},log_duration=${DURATION},autostart_log=1" \
    LD_PRELOAD="${MANGOHUD_LIB}${EXTRA_PRELOAD:+:$EXTRA_PRELOAD}" \
    timeout $((DURATION + 12)) "${WORKLOAD[@]}" >/dev/null 2>&1

  local csv
  csv="$(find "$folder" -name '*.csv' -print -quit 2>/dev/null)"
  if [ -n "$csv" ]; then
    summarise "$csv" "$label"
  else
    echo "$label: MangoHud produced no log"
  fi
  echo
}

echo "workload : ${WORKLOAD[*]}"
echo "target   : ${TARGET_FPS} FPS for ${DURATION}s per case"
echo "mangohud : ${MANGOHUD_LIB:-(wrapper only)}"
echo "strangle : ${STRANGLE_LIB:-not installed}"
echo

EXTRA_PRELOAD=""
run_case "unlimited" "0"
run_case "mangohud" "$TARGET_FPS"

if [ -n "$STRANGLE_LIB" ]; then
  EXTRA_PRELOAD="$STRANGLE_LIB"
  run_case "libstrangle" "0" "FPS=$TARGET_FPS"
  EXTRA_PRELOAD=""
fi

if [ -n "${DISPLAY:-}" ] && have xprop && xprop -root GAMESCOPE_FPS_LIMIT >/dev/null 2>&1; then
  xprop -root -f GAMESCOPE_FPS_LIMIT 32c -set GAMESCOPE_FPS_LIMIT "$TARGET_FPS"
  run_case "gamescope" "0"
  xprop -root -f GAMESCOPE_FPS_LIMIT 32c -set GAMESCOPE_FPS_LIMIT 0
else
  echo "gamescope: not an active gamescope session, skipped"
fi

echo "Lower stdev and a p99 close to the mean mean smoother delivery to the encoder."
