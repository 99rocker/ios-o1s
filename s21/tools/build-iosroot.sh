#!/bin/bash
# Build the iOS 16.7.16 (20H392, iPhone X) root image for the S21, in WSL.
#   wsl bash tools/build-iosroot.sh        (run from s21-port; asks for your sudo password)
#
# Inputs (extracted from the verified IPSW): ~/ios/{OS,SystemOS,AppOS}.dmg
# Output: ~/ios/ios16-root.img (16 GiB case-sensitive, journaled HFS+ "iOSRoot",
#         ios-sweet's bring-up tweaks applied) and ~/ios/ios16-root.simg (sparse).
# The copy (apfs-fuse decompressing ~12 GB) is the slow part. If an image from an
# earlier run already holds the copied root, it is reused and only the later steps run.
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
cd ~/ios

if ! grep -qw hfsplus /proc/filesystems; then
	echo "== loading the HFS+ module (WSL restarted since tools/wsl-setup.sh)"
	sudo insmod ~/wslk/src/fs/hfsplus/hfsplus.ko 2>/dev/null || sudo insmod -f ~/wslk/src/fs/hfsplus/hfsplus.ko
fi
grep -qw hfsplus /proc/filesystems || { echo "no HFS+ support: run tools/wsl-setup.sh"; exit 1; }

mkdir -p mnt
mountpoint -q mnt && sudo umount mnt

copied=0
if [ -e ios16-root.img ]; then
	sudo fsck.hfsplus -fy ios16-root.img >/dev/null 2>&1 || true
	sudo mount -t hfsplus -o loop,force ios16-root.img mnt
	if sudo test -f mnt/System/Library/xpc/launchd.plist && sudo test -d mnt/private/preboot/Cryptexes/App/System; then
		copied=1
	fi
	sudo umount mnt
fi

if [ "$copied" = 1 ]; then
	echo "== 1/4 reusing the copied iOS root in ~/ios/ios16-root.img; finishing mkiosroot's last steps"
	sudo mount -t hfsplus -o loop,force ios16-root.img mnt
	# logd's log stores (owner _logd = 272) and the plain-plist system keybag, as in
	# tools/mkiosroot.sh (mkdir/chown/chmod: uutils install rejects unknown numeric ids).
	db=mnt/private/var/db
	sudo mkdir -p "$db/diagnostics/timesync" "$db/uuidtext"
	sudo chown 272:272 "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync"
	sudo chmod 0751 "$db/diagnostics"
	sudo chmod 0755 "$db/uuidtext" "$db/diagnostics/timesync"
	for d in diagnostics uuidtext diagnostics/timesync; do
		sudo touch "$db/$d/.migrated"
		sudo chown 272:272 "$db/$d/.migrated"
		sudo chmod 0644 "$db/$d/.migrated"
	done
	sudo mkdir -p mnt/private/var/keybags
	sudo chown 0:0 mnt/private/var/keybags
	sudo chmod 0700 mnt/private/var/keybags
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
		'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
		'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
		sudo tee mnt/private/var/keybags/systembag.kb >/dev/null
	sudo chown 0:0 mnt/private/var/keybags/systembag.kb
	sudo chmod 0600 mnt/private/var/keybags/systembag.kb
	df -h mnt
	sudo umount mnt
	# Linux hfsplus decomposes some non-ASCII names differently from Apple: let fsck rewrite them.
	sudo fsck.hfsplus -f -y ios16-root.img | grep -v 'Illegal name' || true
else
	if [ -e ios16-root.img ]; then
		echo "~/ios/ios16-root.img has no complete copy: moving it to ios16-root.img.old"
		mv -f ios16-root.img ios16-root.img.old
	fi
	echo "== 1/4 copy iOS out of Apple's images (tools/mkiosroot.sh)"
	bash "$T/mkiosroot.sh" OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
fi

echo "== 2/4 ios-sweet bring-up tweaks (tools/sweet-root-tweaks.py)"
sudo mount -t hfsplus -o loop,force ios16-root.img mnt
sudo python3 "$T/sweet-root-tweaks.py" mnt
sudo umount mnt

echo "== 3/4 check the filesystem"
# Linux fsck_hfs often leaves a "volume header needs minor repair" (free-block count etc.)
# that a further -y pass settles: repair until a read-only check is clean (max 3 passes).
clean=0
for pass in 1 2 3; do
	sudo fsck.hfsplus -f -y ios16-root.img | grep -v 'Illegal name' | tail -1 || true
	if sudo fsck.hfsplus -f -n ios16-root.img >/tmp/fsckn.txt 2>&1; then clean=1; break; fi
done
tail -3 /tmp/fsckn.txt
if [ "$clean" != 1 ]; then
	echo "note: read-only check still asks for a minor volume header repair;"
	echo "      XNU replays/repairs the journaled volume header itself when it mounts it."
	grep -E 'needs|corrupt|Invalid|Missing' /tmp/fsckn.txt | grep -vE 'Volume header needs minor repair|was found corrupt' && exit 1 || true
fi

echo "== 4/4 sparse image + checksum"
rm -f ios16-root.simg
img2simg ios16-root.img ios16-root.simg
sha256sum ios16-root.img | tee ios16-root.img.sha256
ls -l ios16-root.img ios16-root.simg
echo "IOS ROOT IMAGE READY"
