#!/bin/bash
# Automated NFS Carbon main-menu freeze test.
#
# Reproduces the freeze end to end with no human at the device:
#   boot -> title screen -> press START/A to the main menu -> press A three
#   times 3s apart -> report whether ANY pixel of the game area changed.
#
# The pass/fail signal is pixels, not the log: several "measurements" in this
# investigation turned out to be logging artifacts (VdSwap is kHighFrequency,
# kernel calls need the kLog tag, etc.), whereas the screen cannot lie.
# The bottom 15% is cropped because the debug overlay prints a live counter.
#
# Usage:  nfs_test.sh "<label>" "<extra_args>" [prop=value ...]

set -u
DEV=3a478943
PKG=org.xeniaae.canary
URI="content://media/external/downloads/1000069113"
D="$(dirname "$0")"
LABEL="$1"; shift
EXTRA="$1"; shift

shot() { adb -s $DEV exec-out screencap -p > "$D/$1" 2>/dev/null; }

# Brightness in a region -> cheap, reliable screen-state probe.
bright() { # file x0 y0 x1 y1 threshold
  python3 -c "
from PIL import Image
im=Image.open('$D/$1').convert('L').crop(($2,$3,$4,$5))
print(sum(1 for p in list(im.getdata()) if p>$6))
" 2>/dev/null
}

echo "=== $LABEL ==="
adb -s $DEV shell am force-stop $PKG
adb -s $DEV shell "setprop debug.canary.extra_args '$EXTRA'"
for kv in "$@"; do
  adb -s $DEV shell "setprop debug.canary.${kv%%=*} ${kv#*=}"
done
adb -s $DEV shell "echo -n > /sdcard/Android/data/$PKG/files/xeniaae/xe.log"
adb -s $DEV shell "am start -n $PKG/org.xeniaae.EmulatorActivity \
  -e game_uri '$URI' -e game_title 'Need for Speed - Carbon' \
  -e game_title_id '454107EC'" >/dev/null 2>&1

# 1. wait for "Press START to begin" (the text pulses, so poll for its peak)
for i in $(seq 1 60); do
  sleep 5; shot w.png
  [ "$(bright w.png 790 630 1090 685 120)" -gt 300 ] && break
done
if [ "$(bright w.png 790 630 1090 685 120)" -le 300 ]; then
  echo "RESULT: never reached title screen"; exit 1
fi

# 2. START/A a few times - NFS needs several presses, it will not self-advance
for i in 1 2 3; do
  adb -s $DEV shell input gamepad keyevent --longpress 108; sleep 3
  adb -s $DEV shell input gamepad keyevent --longpress 96;  sleep 3
done
shot menu.png
MENU=$(bright menu.png 900 150 1350 190 150)
if [ "${MENU:-0}" -lt 400 ]; then
  echo "RESULT: never reached main menu (menu-bar px=$MENU)"; exit 1
fi

# 3. the actual test: does A do anything at the main menu?
for i in 1 2 3; do
  adb -s $DEV shell input gamepad keyevent --longpress 96; sleep 3
done
sleep 4; shot afterA.png

python3 -c "
from PIL import Image, ImageChops
a=Image.open('$D/menu.png').convert('RGB'); b=Image.open('$D/afterA.png').convert('RGB')
w,h=a.size; box=(0,0,w,int(h*0.85))
bb=ImageChops.difference(a.crop(box),b.crop(box)).getbbox()
print('RESULT:', 'FROZEN (0 pixels changed)' if bb is None else f'ADVANCED - changed {bb}')
"
