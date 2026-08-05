#!/bin/bash
# Profile the emulator during gameplay and rank the hot functions.
#
# We already know WHICH THREAD is the bottleneck: frame-budget instrumentation
# showed the GPU command thread 99% executing, 0% starved by the guest, 0%
# blocked on the GPU, 0% in vkQueueSubmit. So the cost is inside the PM4 ->
# Vulkan translation itself. This answers the remaining question: WHICH PART.
#
# Candidates, in the order they'd change what we do:
#   * descriptor set alloc/update per draw -> cache and reuse sets
#   * pipeline/shader hash lookups per draw -> memoize on a dirty flag
#   * texture cache revalidation per draw   -> skip when nothing changed
#   * system constant recompute per draw    -> dirty-track the constant buffer
#   * raw packet decode                     -> nothing easy; would need rework
#
# Requires: su -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid'  (already set)
set -u
DEV=3a478943
PKG=org.xeniaae.canary
DUR=${1:-20}
OUT="$(cd "$(dirname "$0")" && pwd)"
SYMS=/home/roman/Android/Xenia-AE/app/build/intermediates/cxx/Debug/2x4c5d6x/obj/arm64-v8a/libe.so

P=$(adb -s $DEV shell pidof $PKG:emu | tr -d '\r')
[ -z "$P" ] && { echo "emulator not running - launch a game first"; exit 1; }

# The path perf.data will record for the library. --symfs is matched by this
# path, so the local tree must mirror it exactly or every frame resolves to
# 'unknown' and the profile is worthless.
DPATH=$(adb -s $DEV shell "cat /proc/$P/maps | grep -m1 'libe.so'" \
        | tr -d '\r' | awk '{print $NF}')
echo "profiling pid=$P for ${DUR}s"
echo "device lib: $DPATH"

mkdir -p "$OUT/symfs$(dirname "$DPATH")"
cp -f "$SYMS" "$OUT/symfs$DPATH"

adb -s $DEV shell "su -c 'simpleperf record -p $P -g --duration $DUR -f 1000 \
    -o /data/local/tmp/perf.data && chmod 666 /data/local/tmp/perf.data'" 2>&1 | tail -2
adb -s $DEV pull /data/local/tmp/perf.data "$OUT/perf.data" >/dev/null 2>&1

# JIT symbol map: written by the emulator when debug.canary.perf_map=1.
# Without it the guest threads - the largest CPU consumer at 57.3% - profile
# as ~97% "unknown", because JIT'd code has no ELF symbols behind it.
MAP=""
for cand in "/data/local/tmp/perf-$P.map" "/tmp/perf-$P.map"; do
    if adb -s $DEV shell "su -c 'test -f $cand'" 2>/dev/null; then
        adb -s $DEV shell "su -c 'cat $cand'" > "$OUT/perf-$P.map" 2>/dev/null
        [ -s "$OUT/perf-$P.map" ] && MAP="$OUT/perf-$P.map" && break
    fi
done
if [ -n "$MAP" ]; then
    echo "JIT symbol map: $(wc -l < "$MAP") entries"
else
    echo "NO JIT symbol map - guest frames will be 'unknown'."
    echo "  enable with: adb shell setprop debug.canary.perf_map 1  (before launching)"
fi

SP=$(ls -d ~/Android/Sdk/ndk/*/simpleperf 2>/dev/null | tail -1)

echo
echo "=== where the time goes (by shared object) ==="
python3 "$SP/report.py" -i "$OUT/perf.data" --sort dso 2>/dev/null | head -12

echo
echo "=== hot functions, GPU command thread only ==="
python3 "$SP/report.py" -i "$OUT/perf.data" --sort symbol \
    --symfs "$OUT/symfs" --comms "GPU Commands" 2>/dev/null | head -35

echo
echo "=== hot functions, whole process ==="
python3 "$SP/report.py" -i "$OUT/perf.data" --sort symbol \
    --symfs "$OUT/symfs" 2>/dev/null | head -25

if [ -n "$MAP" ]; then
    echo
    echo "=== hot JIT'd GUEST functions (resolved via perf map) ==="
    python3 - "$OUT/perf.data" "$MAP" "$SP" <<'PY2'
import sys, re, subprocess, bisect
data, mappath, sp = sys.argv[1:4]
# Load the map: (start, end, name), sorted for binary search.
rows = []
for line in open(mappath, errors='ignore'):
    parts = line.split(None, 2)
    if len(parts) == 3:
        try:
            start = int(parts[0], 16); size = int(parts[1], 16)
            rows.append((start, start + size, parts[2].strip()))
        except ValueError:
            pass
rows.sort()
starts = [r[0] for r in rows]
print(f"  {len(rows)} JIT symbols loaded")
# Dump raw sample addresses for the guest threads and attribute them.
out = subprocess.run(
    ["python3", f"{sp}/report_sample.py", "-i", data],
    capture_output=True, text=True).stdout
hits, total, unresolved = {}, 0, 0
for line in out.splitlines():
    m = re.match(r'\s*([0-9a-fA-F]{6,})\s', line)
    if not m:
        continue
    addr = int(m.group(1), 16); total += 1
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < rows[i][1]:
        hits[rows[i][2]] = hits.get(rows[i][2], 0) + 1
    else:
        unresolved += 1
if total:
    print(f"  {total} samples, {100*(total-unresolved)//total}% resolved to guest functions\n")
    for name, n in sorted(hits.items(), key=lambda x: -x[1])[:25]:
        print(f"    {100.0*n/total:5.2f}%  {name}")
PY2
fi
