#!/bin/bash
# FPS benchmark harness for Canary AE.
#
# WHY THIS EXISTS
# ---------------
# Measured scene variation on NFS Carbon is 8.9-13.9 FPS - a +/-25% spread. A
# single FPS reading therefore cannot tell a real optimisation from noise, and
# has already produced one false "10% win" in this project off a single 12.9 FPS
# sample (the next three were 11.9 / 8.9 / 10.9).
#
# So this records a DISTRIBUTION, not a number, and compares two distributions
# with a non-parametric test. If the test says "not significant", the change did
# nothing detectable - regardless of how much better the median looks.
#
# HOW FRAMES ARE COUNTED
# ----------------------
# Via the emulator's own counter in VulkanCommandProcessor::IssueSwap, enabled
# with debug.canary.fps. Deliberately NOT by counting VdSwap kernel calls in the
# log - that needs log_kernel_calls + log_level=3, which floods the log and
# slows down the exact thing being measured.
#
# USAGE
#   ./fps_bench.sh record baseline 60     # record 60s into 'baseline'
#   ./fps_bench.sh record bindless 60     # record the same route after a change
#   ./fps_bench.sh compare baseline bindless
#
# IMPORTANT: drive the SAME ROUTE for both runs. This controls for measurement
# noise, not for you driving a different track.
#
# SENSITIVITY (measured by simulation against the real 8.9-13.9 FPS spread):
#   60s run (~60 samples):  >=10% change detected every time
#                            5% change detected less than half the time
# So a 60s run can prove a 10% win but cannot disprove a 5% one. If a change is
# expected to be small, record 180s instead.
set -u
DEV=${DEV:-3a478943}
PKG=org.xeniaae.canary
LOG=/sdcard/Android/data/$PKG/files/xeniaae/xe.log
OUTDIR="$(cd "$(dirname "$0")/.." && pwd)/.bench"
mkdir -p "$OUTDIR"

usage() { sed -n '2,30p' "$0" | sed 's/^# \?//'; exit 1; }

record() {
    local name="${1:-}" dur="${2:-60}"
    [ -z "$name" ] && usage
    local pid
    pid=$(adb -s "$DEV" shell pidof $PKG:emu | tr -d '\r')
    [ -z "$pid" ] && { echo "ERROR: emulator not running - launch a game first"; exit 1; }

    # Everything else off: this is the "how would Xenia AE run" state.
    # Saved first, and restored when the run ends - a benchmark must not
    # silently change the state the user left the device in. The first version
    # of this script forced them off and never put them back, which switched
    # the on-screen FPS counter off mid-session and looked like a counter bug.
    local props="testrig.master gpu audio jit mem kernel frame_budget \
                 log_kernel_calls halo3_vista_probe fps"
    SAVED_PROPS=""
    for p in $props; do
        v=$(adb -s "$DEV" shell "getprop debug.canary.$p" | tr -d '\r')
        SAVED_PROPS="$SAVED_PROPS $p=$v"
        [ "$p" = "fps" ] || adb -s "$DEV" shell "setprop debug.canary.$p 0" >/dev/null 2>&1
    done

    local before
    before=$(adb -s "$DEV" shell "grep -c XEFPS $LOG 2>/dev/null" | tr -d '\r ')
    [ -z "$before" ] && before=0

    adb -s "$DEV" shell "setprop debug.canary.fps 1"
    echo "recording '$name' for ${dur}s - drive normally, same route every run"
    for i in $(seq "$dur" -5 5); do printf "\r  %3ds remaining " "$i"; sleep 5; done
    printf "\r                    \r"
    # Restore whatever the user had before, rather than forcing everything off.
    for kv in $SAVED_PROPS; do
        k="${kv%%=*}"; v="${kv#*=}"
        adb -s "$DEV" shell "setprop debug.canary.$k '$v'" >/dev/null 2>&1
    done

    adb -s "$DEV" shell "grep XEFPS $LOG 2>/dev/null" | tr -d '\r' \
        | tail -n +$((before + 1)) > "$OUTDIR/$name.raw"

    python3 - "$OUTDIR/$name.raw" "$OUTDIR/$name.csv" <<'PY'
import sys, re
raw, out = sys.argv[1], sys.argv[2]
vals = []
for line in open(raw, errors='ignore'):
    m = re.search(r'XEFPS (\d+) (\d+)', line)
    if m:
        frames, ms = int(m.group(1)), int(m.group(2))
        if ms > 0:
            vals.append(frames * 1000.0 / ms)
open(out, 'w').write('\n'.join(f'{v:.3f}' for v in vals))
if not vals:
    print("  no samples captured - was a game actually running and rendering?")
    sys.exit(1)
vals.sort()
n = len(vals)
med = vals[n//2] if n % 2 else (vals[n//2-1]+vals[n//2])/2
print(f"  n={n}  median={med:.2f} FPS  min={vals[0]:.2f}  max={vals[-1]:.2f}")
if n < 20:
    print(f"  WARNING: {n} samples is thin. Record 60s+ for a usable comparison.")
PY
    echo "  saved -> $OUTDIR/$name.csv"
}

compare() {
    local a="${1:-}" b="${2:-}"
    [ -z "$a" ] || [ -z "$b" ] && usage
    python3 - "$OUTDIR/$a.csv" "$OUTDIR/$b.csv" "$a" "$b" <<'PY'
import sys, math
pa, pb, na, nb = sys.argv[1:5]
def load(p):
    try:
        return sorted(float(x) for x in open(p) if x.strip())
    except FileNotFoundError:
        print(f"no such run: {p}"); sys.exit(1)
A, B = load(pa), load(pb)
def med(v):
    n=len(v); return v[n//2] if n%2 else (v[n//2-1]+v[n//2])/2
mA, mB = med(A), med(B)

# Mann-Whitney U with a normal approximation. Non-parametric: makes no
# assumption that FPS is normally distributed, which it very much is not.
merged = sorted([(v,0) for v in A] + [(v,1) for v in B])
ranks = {}
i = 0
while i < len(merged):
    j = i
    while j+1 < len(merged) and merged[j+1][0] == merged[i][0]:
        j += 1
    r = (i + j) / 2.0 + 1
    for k in range(i, j+1):
        ranks.setdefault(k, r)
    i = j + 1
Ra = sum(ranks[k] for k,(v,g) in enumerate(merged) if g == 0)
n1, n2 = len(A), len(B)
U = Ra - n1*(n1+1)/2.0
mu = n1*n2/2.0
sigma = math.sqrt(n1*n2*(n1+n2+1)/12.0)
z = (U - mu)/sigma if sigma else 0.0
p = math.erfc(abs(z)/math.sqrt(2))          # two-tailed

delta = (mB - mA)/mA*100 if mA else 0.0
print(f"{na}: n={n1} median={mA:.2f} FPS   (min {A[0]:.2f}, max {A[-1]:.2f})")
print(f"{nb}: n={n2} median={mB:.2f} FPS   (min {B[0]:.2f}, max {B[-1]:.2f})")
print(f"\nmedian change: {delta:+.1f}%     p={p:.4f}")
if p < 0.05:
    print("VERDICT: significant - the change moved FPS." if delta > 0 else
          "VERDICT: significant REGRESSION - this made it slower.")
else:
    print("VERDICT: NOT significant. Indistinguishable from scene noise.")
    print("         Do not claim a win from this. Either the change did")
    print("         nothing, or the effect is smaller than the noise floor.")
PY
}

case "${1:-}" in
    record)  shift; record "$@" ;;
    compare) shift; compare "$@" ;;
    *)       usage ;;
esac
