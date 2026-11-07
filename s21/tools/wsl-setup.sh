#!/bin/bash
# One-time WSL Ubuntu setup for building the iOS root image (ios-sweet tools/mkiosroot.sh):
#   1. apt packages: apfs-fuse (read Apple's APFS .dmg), hfsprogs (mkfs/fsck.hfsplus),
#      rsync, unzip, and what a kernel module build needs
#   2. the HFS+ kernel module: Microsoft's WSL kernel ships without CONFIG_HFSPLUS_FS, so
#      build fs/hfsplus from the exact source tag of the running kernel and load it.
# Run in WSL:  bash wsl-setup.sh      (asks for your sudo password)
set -euo pipefail

REL=$(uname -r)                               # 6.18.33.2-microsoft-standard-WSL2
VER=${REL%%-*}                                # 6.18.33.2
TAG=linux-msft-wsl-$VER

echo "== 1/3 packages"
sudo apt-get update -qq
# Ubuntu 26.04 dropped the apfs-fuse package: build it from source below.
sudo apt-get install -y -qq hfsprogs rsync unzip build-essential flex bison \
	libssl-dev libelf-dev bc git kmod cmake pkg-config fuse3 libfuse3-dev \
	zlib1g-dev libbz2-dev libattr1-dev

if ! command -v apfs-fuse >/dev/null; then
	echo "== apfs-fuse from source (github.com/sgan81/apfs-fuse)"
	mkdir -p ~/wslk && cd ~/wslk
	[ -d apfs-fuse ] || git clone -q --recursive https://github.com/sgan81/apfs-fuse.git
	# CMake 4 refuses the project's old cmake_minimum_required without the policy flag;
	# GCC 15 no longer pulls in <cstdint> implicitly (PList.h uses uint8_t without it).
	rm -rf apfs-fuse/build
	cmake -S apfs-fuse -B apfs-fuse/build -DCMAKE_BUILD_TYPE=Release -DUSE_FUSE3=ON \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint" >/dev/null
	cmake --build apfs-fuse/build -j"$(nproc)" >/dev/null
	sudo install -m 0755 apfs-fuse/build/apfs-fuse apfs-fuse/build/apfsutil /usr/local/bin/
	command -v apfs-fuse
fi

if grep -qw hfsplus /proc/filesystems; then
	echo "== hfsplus already available"
	exit 0
fi

echo "== 2/3 HFS+ module for $REL (source tag $TAG)"
# The running kernel was built with GCC 13.2 (CONFIG_GCC_PLUGINS=y) and pahole
# (CONFIG_DEBUG_INFO_BTF_MODULES=y, which adds fields to struct module). With GCC 15 and
# no pahole, olddefconfig drops those options and the module's struct module has the wrong
# size ("section size must match the kernel's built struct module size"). Match them.
sudo apt-get install -y -qq gcc-13 g++-13 gcc-13-plugin-dev dwarves
K="CC=gcc-13 HOSTCC=gcc-13 HOSTCXX=g++-13 LOCALVERSION="   # LOCALVERSION= : no "+" in vermagic
mkdir -p ~/wslk && cd ~/wslk
[ -d src ] || git clone -q --depth 1 --branch "$TAG" https://github.com/microsoft/WSL2-Linux-Kernel.git src
cd src
make -s $K clean >/dev/null 2>&1 || true
zcat /proc/config.gz > .config
./scripts/config --module CONFIG_HFSPLUS_FS
make -s $K olddefconfig
echo "config differences vs the running kernel (expect only HFSPLUS and tool versions):"
diff <(zcat /proc/config.gz | grep -E '^(CONFIG_|# CONFIG_)' | sort) \
     <(grep -E '^(CONFIG_|# CONFIG_)' .config | sort) | grep -E '^[<>]' | grep -vE 'HFSPLUS|_VERSION=|VERSION_TEXT' || true
make -s $K modules_prepare -j"$(nproc)"
# No vmlinux build, so no Module.symvers: let modpost warn instead of fail; the symbols
# are resolved against the running kernel at load time.
make -s $K KBUILD_MODPOST_WARN=1 M=fs/hfsplus modules -j"$(nproc)" 2>&1 | grep -vE "undefined!|Skipping BTF" || true
ls -l fs/hfsplus/hfsplus.ko
modinfo fs/hfsplus/hfsplus.ko | grep vermagic

echo "== 3/3 load it"
# MODVERSIONS is on and we did not build vmlinux, so the module may lack symbol CRCs;
# this kernel has CONFIG_MODULE_FORCE_LOAD=y, so fall back to a forced load.
sudo insmod fs/hfsplus/hfsplus.ko 2>/dev/null || sudo insmod -f fs/hfsplus/hfsplus.ko
grep -w hfsplus /proc/filesystems && echo "HFS+ READY" || { echo "HFS+ module did not load"; exit 1; }
