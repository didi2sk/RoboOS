#!/bin/sh
# PC test of the WPA2 supplicant (src/wpa.c): builds BearSSL for the host into build/host,
# then tools/test_wpa.py plays the access point (needs python3-cryptography).
set -e
cd "$(dirname "$0")/.."
B=build/host
mkdir -p $B/bearssl
if [ ! -f $B/libbearssl.a ]; then
  for f in third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c; do
    o=$B/bearssl/$(echo "$f" | tr / _).o
    cc -O2 -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c "$f" -o "$o" &
  done
  wait
  ar rcs $B/libbearssl.a $B/bearssl/*.o
fi
cc -O2 -Wall -Isrc -Ithird_party/bearssl/inc tools/test_wpa.c src/wpa.c $B/libbearssl.a -o $B/test_wpa
echo selftest | $B/test_wpa | sed 's/^/  /'
echo "  expected pmk f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e (IEEE 802.11 vector)"
echo "  expected prf bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"
python3 tools/test_wpa.py $B/test_wpa
