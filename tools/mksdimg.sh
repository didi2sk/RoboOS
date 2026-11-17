#!/bin/sh
# Build a 64 MB test SD image (MBR + FAT32) for QEMU:
#   /RUN/<autostart programs>   other programs in /APPS, plus a text file.
# usage: tools/mksdimg.sh sd.img "blink" "hello button knight"
set -e
IMG=${1:-sd.img}; AUTO=${2-blink}; OTHER=${3-"hello button knight"}
rm -f "$IMG"
truncate -s 64M "$IMG"
echo 'label: dos
start=2048, type=c' | sfdisk -q "$IMG"
rm -f /tmp/part.img
truncate -s $((64*1024*1024 - 2048*512)) /tmp/part.img
mkfs.fat -F 32 -s 1 -n ROBOOS /tmp/part.img >/dev/null
export MTOOLS_SKIP_CHECK=1
mmd -i /tmp/part.img ::/RUN ::/APPS
for a in $AUTO;  do mcopy -i /tmp/part.img sdk/bin/$a.bin ::/RUN/; done
for a in $OTHER; do mcopy -i /tmp/part.img sdk/bin/$a.bin ::/APPS/; done
for b in sd/APPS/*.bas; do mcopy -i /tmp/part.img "$b" ::/APPS/; done
printf 'Hello from a text file on the SD card.\nLine two.\n' > /tmp/readme.txt
mcopy -i /tmp/part.img /tmp/readme.txt "::/Read me please.txt"
dd if=/tmp/part.img of="$IMG" bs=512 seek=2048 conv=notrunc status=none
