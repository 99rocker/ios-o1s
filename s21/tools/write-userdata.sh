#!/bin/bash
# Write the iOS root image onto the S21's userdata partition from LineageOS recovery
# (ADB enabled in recovery), then read it back and compare SHA-256.
#   bash tools/write-userdata.sh            (Git Bash on Windows; ERASES ANDROID DATA)
#   bash tools/write-userdata.sh --verify   (only the read-back check)
set -euo pipefail
IMG=//wsl.localhost/Ubuntu/home/dadahype/ios/ios16-root.img
WANT=b150099713ec273d5e548d145c51b585dce413c8cabbca4cac9a7bdfa037b98f
DEV=/dev/block/by-name/userdata
SIZE=$(stat -c %s "$IMG")
BS=4194304
COUNT=$((SIZE / BS))
[ $((COUNT * BS)) = "$SIZE" ] || { echo "image size is not a multiple of 4 MiB"; exit 1; }

state=$(adb get-state 2>/dev/null || true)
[ "$state" = recovery ] || { echo "phone is not in recovery with ADB on (state: ${state:-none})"; exit 1; }
id=$(adb shell id | tr -d '\r')
case "$id" in uid=0*) ;; *) echo "recovery shell is not root: $id"; exit 1;; esac
adb shell "ls -l $DEV" | tr -d '\r'
if adb shell "grep -E ' /data | /sdcard ' /proc/mounts" | grep -q .; then
	echo "/data is mounted in recovery: unmount it first"; exit 1
fi
part=$(adb shell "blockdev --getsize64 $DEV" | tr -d '\r')
echo "userdata: $part bytes; image: $SIZE bytes"
[ "$part" -ge "$SIZE" ] || { echo "image does not fit"; exit 1; }

if [ "${1:-}" != --verify ]; then
	echo "== writing $SIZE bytes to $DEV (this takes a while)"
	start=$(date +%s)
	adb exec-in "dd of=$DEV bs=$BS 2>/dev/null; sync; echo done" < "$IMG"
	echo "write took $(( $(date +%s) - start )) s"
fi

echo "== reading back $COUNT x 4 MiB and hashing on the phone"
got=$(adb shell "dd if=$DEV bs=$BS count=$COUNT 2>/dev/null | sha256sum" | tr -d '\r' | cut -d' ' -f1)
echo "phone : $got"
echo "image : $WANT"
[ "$got" = "$WANT" ] && echo "USERDATA VERIFIED" || { echo "CHECKSUM MISMATCH"; exit 1; }
