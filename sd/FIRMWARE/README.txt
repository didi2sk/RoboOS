WiFi firmware for RoboOS ('wifi init', /WIFI.TXT) - copy this folder to the SD card as /FIRMWARE.

The files are NOT part of RoboOS and are not covered by its license. They are unmodified copies
from Raspberry Pi's firmware-nonfree package (the same files Raspberry Pi OS uses), branch bookworm,
https://github.com/RPi-Distro/firmware-nonfree/tree/bookworm/debian/config/brcm80211
(tools/getwifi.sh downloads them again). Licenses, as given in that package's debian/copyright:

  brcmfmac43430-sdio.bin / .clm_blob / .txt   Pi 3B, Zero W     Cypress  - LICENCE.cypress
    (bin + clm_blob = cypress/cyfmac43430-sdio.*, txt = brcm/brcmfmac43430-sdio.txt)
  brcmfmac43455-sdio.bin / .clm_blob / .txt   Pi 3B+            Cypress  - LICENCE.cypress
    (bin = cypress/cyfmac43455-sdio-standard.bin, clm_blob = cypress/cyfmac43455-sdio.clm_blob)
  brcmfmac43436-sdio.bin / .clm_blob / .txt   Zero 2 W          Synaptics - LICENCE.synaptics

Both licenses allow redistributing the binaries, unmodified, for use with the vendor's chips.
Copyright: Cypress Semiconductor Corporation (now Infineon), Synaptics Incorporated.
