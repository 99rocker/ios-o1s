#!/bin/bash
# Unpack the iPhone X device tree (LZFSE) from the IPSW for mkadt_s21.py's /product node.
#   wsl bash tools/get-d22ap-adt.sh        (run from s21-port; no sudo needed)
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
gcc -O2 -o ~/ios/im4p_lzfse "$T/im4p_lzfse.c" -I ~/wslk/apfs-fuse/3rdparty/lzfse/src ~/wslk/apfs-fuse/build/liblzfse.a
mkdir -p "$T/../research/ipsw-x"
~/ios/im4p_lzfse ~/ios/DeviceTree.d22ap.im4p "$T/../research/ipsw-x/d22ap.adt"
echo "D22AP ADT READY"
