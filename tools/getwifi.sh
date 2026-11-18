#!/bin/sh
# WiFi firmware for RoboOS ('wifi init'): downloads the Broadcom/Cypress/Synaptics files from
# Raspberry Pi's firmware-nonfree repository into a FIRMWARE folder - copy it to the SD card root.
#   tools/getwifi.sh [folder]        (default: sd/FIRMWARE, not part of the git repository)
# The files are not ours to put into the repository: binary-redist licenses from Cypress /
# Synaptics, the .txt NVRAM files are GPL-2. You can also take them from Raspberry Pi OS:
# /lib/firmware/brcm/brcmfmac434*-sdio.* (follow the symlinks).
set -e
D=${1:-sd/FIRMWARE}
B=https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bookworm/debian/config/brcm80211
mkdir -p "$D"
get() { curl -sSfL -o "$D/$2" "$B/$1"; echo "  $2"; }
get cypress/cyfmac43430-sdio.bin        brcmfmac43430-sdio.bin        # Pi 3B, Pi Zero W
get brcm/brcmfmac43430-sdio.txt         brcmfmac43430-sdio.txt
get brcm/brcmfmac43436-sdio.bin         brcmfmac43436-sdio.bin        # Zero 2 W
get brcm/brcmfmac43436-sdio.txt         brcmfmac43436-sdio.txt
get brcm/brcmfmac43436-sdio.clm_blob    brcmfmac43436-sdio.clm_blob
get cypress/cyfmac43455-sdio-standard.bin brcmfmac43455-sdio.bin      # Pi 3B+
get brcm/brcmfmac43455-sdio.txt         brcmfmac43455-sdio.txt
get cypress/cyfmac43455-sdio.clm_blob   brcmfmac43455-sdio.clm_blob
echo "done: copy $D to the SD card as /FIRMWARE"
