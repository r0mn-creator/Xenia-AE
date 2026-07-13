#!/bin/bash
# Automated multi-sample test harness for the Halo 3 menu terrain-buffer fill
# investigation (docs/HALO3_MENU_INVESTIGATION.md). Runs N cold boots of
# Canary AE, launches Halo 3, waits for the terrain shader's VTXDIST log line,
# and reports the fill fraction for each run plus summary stats.
#
# Usage: halo3_menu_sample.sh [N samples] [device serial]
set -uo pipefail

N=${1:-5}
DEVICE=${2:-3a478943}
PKG=org.xeniaae.canary
LOG_PATH=/storage/emulated/0/Android/data/${PKG}/files/xeniaae/xe.log
OUT_DIR=/tmp/claude-1000/-home-roman/abd25da8-dc22-4421-9f0d-aed9d7a723b0/scratchpad/halo3_samples
mkdir -p "$OUT_DIR"

results=()

for i in $(seq 1 "$N"); do
  echo "=== sample $i/$N ===" >&2
  adb -s "$DEVICE" shell am force-stop $PKG
  adb -s "$DEVICE" shell rm -rf /storage/emulated/0/Android/data/${PKG}/files/xeniaae/cache/pipelines_4D5307E6.bin \
      /storage/emulated/0/Android/data/${PKG}/files/xeniaae/cache/shaders
  adb -s "$DEVICE" shell rm -f "$LOG_PATH"
  adb -s "$DEVICE" shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  sleep 4
  adb -s "$DEVICE" shell input tap 803 530
  sleep 2
  adb -s "$DEVICE" shell input tap 803 530

  # Poll for the terrain-sized VTXDIST line specifically (143360 dwords) -
  # smaller unrelated buffers get logged much earlier during loading and
  # would make this return before the menu is actually reached.
  waited=0
  found=0
  while [ "$waited" -lt 120 ]; do
    if adb -s "$DEVICE" shell "grep -q 'nonzero=[0-9]*/143360' $LOG_PATH 2>/dev/null"; then
      found=1
      break
    fi
    sleep 3
    waited=$((waited + 3))
  done

  if [ "$found" -ne 1 ]; then
    echo "sample $i: TIMED OUT waiting for menu" >&2
    results+=("TIMEOUT")
    continue
  fi

  # Give it a couple more seconds to settle to its final steady-state value.
  sleep 5

  adb -s "$DEVICE" pull "$LOG_PATH" "$OUT_DIR/sample_${i}.log" >/dev/null 2>&1

  line=$(grep "VTXDIST" "$OUT_DIR/sample_${i}.log" | grep "nonzero=[0-9]*/143360" | tail -1)
  if [ -z "$line" ]; then
    echo "sample $i: no terrain-sized VTXDIST line found" >&2
    results+=("NONE")
    continue
  fi

  nz=$(echo "$line" | grep -oE "nonzero=[0-9]+" | grep -oE "[0-9]+")
  pct=$(python3 -c "print(f'{$nz/143360*100:.2f}')")
  echo "sample $i: nonzero=$nz/143360 (${pct}%)" >&2
  results+=("$nz")
done

echo ""
echo "=== SUMMARY ==="
valid=()
for r in "${results[@]}"; do
  if [[ "$r" =~ ^[0-9]+$ ]]; then
    valid+=("$r")
  fi
done

if [ "${#valid[@]}" -eq 0 ]; then
  echo "No valid samples collected."
  exit 1
fi

python3 -c "
vals = [$(IFS=,; echo "${valid[*]}")]
pcts = [v/143360*100 for v in vals]
print(f'N={len(vals)} valid samples (of $N requested)')
print(f'raw:  {vals}')
print(f'pct:  {[round(p,2) for p in pcts]}')
print(f'mean: {sum(pcts)/len(pcts):.2f}%')
print(f'min:  {min(pcts):.2f}%')
print(f'max:  {max(pcts):.2f}%')
"
