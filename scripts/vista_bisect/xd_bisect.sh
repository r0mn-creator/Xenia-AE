#!/bin/bash
# Bisect harness: set XDtester cvars to AE-like values, run Halo 3 to the menu,
# capture the vista AND the c3.x sign histogram.
#
#   usage: xd_bisect.sh <tag> "cvar=value" "cvar=value" ...
#
# Verdict (verdict.py):
#   POSITIVE dominant -> vista still CORRECT (group is not the cause)
#   NEGATIVE dominant -> vista BROKE          (cause is in this group)
#   NO CAMERA STATES  -> run failed; re-run or split the group
S=3a478943
SCRATCH=/tmp/claude-1000/-home-roman/46acab0c-8222-4ca3-8f0e-184162cd0372/scratchpad
PKG=xendroid.compose.xdtester.debug
C=/storage/emulated/0/Android/data/$PKG/files/compose/xenia-canary.config.toml
L=/storage/emulated/0/Android/data/$PKG/files/compose/xe.log
TAG=$1; shift

adb -s $S shell "am force-stop $PKG"
adb -s $S shell "am force-stop org.xeniaae.aex"   # its ANR dialog steals input
# Snapshot the config once so every group starts from the same baseline.
adb -s $S shell "su -c '[ -f $C.bisectbase ] || cp $C $C.bisectbase; cp $C.bisectbase $C'"

for kv in "$@"; do
  k="${kv%%=*}"; v="${kv#*=}"
  # STRING cvars are quoted in the config; writing them bare breaks the TOML
  # parse, the config fails to load and the game never starts (scores as a
  # bogus "run failed"). Quote anything that is not a bool or a number.
  case "$v" in
    true|false|-[0-9]*|[0-9]*) qv="$v" ;;
    *) qv="\\\"$v\\\"" ;;
  esac
  adb -s $S shell "su -c \"sed -i 's|^$k = .*|$k = $qv|' $C\""
done
echo "--- applied:"
for kv in "$@"; do k="${kv%%=*}"; adb -s $S shell "su -c \"grep -m1 '^$k ' $C\""; done

adb -s $S shell "su -c 'rm -f $L'"
adb -s $S shell "setprop debug.canary.vsconst_states 1"
adb -s $S shell "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
sleep 12

# Two taps launch the tile; RETRY until the game actually starts rendering
# (proved by VSCONSTSTATE appearing), because a missed tap silently leaves you
# on the library screen and scores as "run failed".
launched=0
for attempt in 1 2 3 4; do
  adb -s $S shell input tap 494 360
  sleep 3
  adb -s $S shell input tap 494 360
  sleep 25
  n=$(adb -s $S shell "su -c \"grep -ac VSCONSTSTATE $L 2>/dev/null\"" | tr -d '\r')
  echo "  launch attempt $attempt: VSCONSTSTATE=$n"
  if [ "${n:-0}" -gt 0 ]; then launched=1; break; fi
done
if [ "$launched" != "1" ]; then
  echo "LAUNCH_FAILED $TAG"
fi

sleep 80
adb -s $S exec-out screencap -p > $SCRATCH/bis_${TAG}.png 2>/dev/null
adb -s $S shell "su -c \"grep -a VSCONSTSTATE $L\"" > $SCRATCH/bis_${TAG}.txt 2>/dev/null
echo "BISECT_DONE $TAG  states=$(wc -l < $SCRATCH/bis_${TAG}.txt)"
